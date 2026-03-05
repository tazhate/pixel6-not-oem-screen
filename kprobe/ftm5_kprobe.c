// SPDX-License-Identifier: GPL-2.0
/*
 * ftm5_kprobe.c — Kprobe module to suppress force-cal BTN_TOUCH UP on ftm5
 *
 * Problem: In LOCKED_ACTIVE mode, the FTS IC firmware triggers force
 * recalibration at ~308ms of sustained touch. This sends EVT_ID_LEAVE_POINT
 * (clearing touch_id) followed by EVT_TYPE_STATUS_FORCE_CAL (logging only).
 * After the event loop, touch_id==0 causes a spurious BTN_TOUCH UP.
 *
 * Solution: Two kprobes intercept the event handlers:
 *   Kprobe 1 (fts_leave_pointer_event_handler pre): save touch_id before clear
 *   Kprobe 2 (fts_status_event_handler pre): if force-cal cleared touch_id,
 *            restore it so the event loop sees touch_id!=0 → no BTN_UP.
 *            Schedule workqueue to re-arm setScanMode(LOCKED_ACTIVE).
 *
 * A third kprobe on fts_enter_pointer_event_handler resets the rearm counter
 * on new touch-down events.
 *
 * Struct offsets verified from ftm5.ko disassembly (android14-6.1, Pixel 6):
 *   touch_id:       0x11B0  (unsigned long, touch slot bitmask)
 *   palm_touch_mask: 0x11B8 (unsigned long)
 *   grip_touch_mask: 0x11C0 (unsigned long)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/workqueue.h>
#include <linux/version.h>

#define MAX_REARMS	12	/* Max force-cal suppress cycles per touch */

/* Struct offsets from ftm5.ko disassembly */
#define OFF_TOUCH_ID		0x11B0
#define OFF_PALM_TOUCH_MASK	0x11B8
#define OFF_GRIP_TOUCH_MASK	0x11C0

/* Force-cal event sub-type for mutual frame flatness */
#define FORCE_CAL_MUTUAL_FLATNESS	0x35

/* Scan mode constants (from ftsSoftware.h) */
#define SCAN_MODE_LOCKED	0x03
#define LOCKED_ACTIVE		0x00

/* Saved state between kprobe 1 and kprobe 2 */
static unsigned long saved_touch_id;
static unsigned long saved_palm_mask;
static unsigned long saved_grip_mask;
static void *saved_info;		/* fts_ts_info pointer */
static int rearm_count;			/* per-touch rearm counter */
static bool leave_in_progress;		/* flag: leave handler just ran */

/* Resolved function pointer for setScanMode */
static int (*fn_setScanMode)(void *info, u8 mode, u8 settings);

/* Work struct for deferred setScanMode call */
static struct work_struct rearm_work;

/* Statistics */
static unsigned long total_suppressed;
static unsigned long total_rearms_exhausted;

/*
 * Workqueue handler: re-arm IC to LOCKED_ACTIVE mode.
 * Runs in process context so SPI I/O is safe.
 */
static void rearm_work_fn(struct work_struct *work)
{
	int ret;

	if (!fn_setScanMode || !saved_info)
		return;

	ret = fn_setScanMode(saved_info, SCAN_MODE_LOCKED, LOCKED_ACTIVE);
	if (ret < 0)
		pr_warn("ftm5_kprobe: setScanMode failed: %d\n", ret);
	else
		pr_debug("ftm5_kprobe: re-armed LOCKED_ACTIVE (cycle %d)\n",
			 rearm_count);
}

/*
 * Kprobe 1: pre-handler for fts_leave_pointer_event_handler
 *
 * Save touch_id BEFORE the handler clears it.
 * ARM64 calling convention: x0 = info, x1 = event data
 */
static int leave_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	void *info = (void *)regs->regs[0];
	unsigned char *event = (unsigned char *)regs->regs[1];
	unsigned long cur_touch_id;

	/* Only care about finger-type events on valid touch IDs */
	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);

	if (cur_touch_id != 0) {
		saved_touch_id = cur_touch_id;
		saved_palm_mask = *(unsigned long *)((char *)info + OFF_PALM_TOUCH_MASK);
		saved_grip_mask = *(unsigned long *)((char *)info + OFF_GRIP_TOUCH_MASK);
		saved_info = info;
		leave_in_progress = true;

		pr_debug("ftm5_kprobe: leave_pre: saved touch_id=0x%lx event=%02x %02x\n",
			 saved_touch_id, event[0], event[1]);
	}

	return 0;
}

/*
 * Kprobe 1 post-handler: check if touch_id was actually cleared.
 * If not (e.g. invalid touchType), cancel the pending restore.
 */
static void leave_post_handler(struct kprobe *p, struct pt_regs *regs,
			       unsigned long flags)
{
	unsigned long cur_touch_id;

	if (!leave_in_progress || !saved_info)
		return;

	cur_touch_id = *(unsigned long *)((char *)saved_info + OFF_TOUCH_ID);

	/* If touch_id wasn't actually cleared, this wasn't a force-cal leave */
	if (cur_touch_id != 0 || saved_touch_id == cur_touch_id) {
		leave_in_progress = false;
		pr_debug("ftm5_kprobe: leave_post: touch_id not cleared (0x%lx), "
			 "canceling restore\n", cur_touch_id);
	}
}

/*
 * Kprobe 2: pre-handler for fts_status_event_handler
 *
 * If the leave handler just cleared touch_id AND this is a force-cal event,
 * restore touch_id to prevent the spurious BTN_TOUCH UP.
 */
static int status_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	void *info = (void *)regs->regs[0];
	unsigned char *event = (unsigned char *)regs->regs[1];
	unsigned long cur_touch_id;

	/* Only act if leave handler flagged a potential force-cal scenario */
	if (!leave_in_progress)
		return 0;

	/* Check this is actually a force-cal status event (event[1] == 0x05) */
	if (event[1] != 0x05) {
		leave_in_progress = false;
		return 0;
	}

	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);

	if (cur_touch_id == 0 && saved_touch_id != 0) {
		if (rearm_count < MAX_REARMS) {
			/* Restore touch_id — prevent BTN_TOUCH UP */
			*(unsigned long *)((char *)info + OFF_TOUCH_ID) = saved_touch_id;
			*(unsigned long *)((char *)info + OFF_PALM_TOUCH_MASK) = saved_palm_mask;
			*(unsigned long *)((char *)info + OFF_GRIP_TOUCH_MASK) = saved_grip_mask;

			rearm_count++;
			total_suppressed++;

			pr_info("ftm5_kprobe: force-cal suppressed (cycle %d/%d, "
				"sub=0x%02x, touch_id=0x%lx)\n",
				rearm_count, MAX_REARMS,
				event[2], saved_touch_id);

			/* Re-arm LOCKED_ACTIVE via workqueue (SPI needs process ctx) */
			schedule_work(&rearm_work);
		} else {
			/* Exhausted rearms — let BTN_UP through */
			total_rearms_exhausted++;
			pr_info("ftm5_kprobe: rearm limit reached (%d), "
				"allowing BTN_UP\n", MAX_REARMS);
		}
	}

	leave_in_progress = false;
	return 0;
}

/*
 * Kprobe 3: pre-handler for fts_enter_pointer_event_handler
 *
 * Reset rearm counter on new touch-down (real ENTER_POINT).
 * This detects a fresh finger placement after a genuine lift.
 */
static int enter_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	void *info = (void *)regs->regs[0];
	unsigned long cur_touch_id;

	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);

	/* If touch_id was 0 before this enter event → new touch-down */
	if (cur_touch_id == 0 && rearm_count > 0) {
		pr_info("ftm5_kprobe: new touch-down, reset rearm count "
			"(was %d)\n", rearm_count);
		rearm_count = 0;
	}

	return 0;
}

/* Kprobe definitions */
static struct kprobe kp_leave = {
	.symbol_name = "fts_leave_pointer_event_handler",
	.pre_handler = leave_pre_handler,
	.post_handler = leave_post_handler,
};

static struct kprobe kp_status = {
	.symbol_name = "fts_status_event_handler",
	.pre_handler = status_pre_handler,
};

static struct kprobe kp_enter = {
	.symbol_name = "fts_enter_pointer_event_handler",
	.pre_handler = enter_pre_handler,
};

/*
 * Resolve setScanMode address using the kprobe registration trick.
 * Register a temporary kprobe on the symbol name, grab its address,
 * then unregister.
 */
static int resolve_setScanMode(void)
{
	struct kprobe kp_tmp = { .symbol_name = "setScanMode" };
	int ret;

	ret = register_kprobe(&kp_tmp);
	if (ret < 0) {
		pr_err("ftm5_kprobe: failed to resolve setScanMode: %d\n", ret);
		return ret;
	}

	fn_setScanMode = (void *)kp_tmp.addr;
	unregister_kprobe(&kp_tmp);

	pr_info("ftm5_kprobe: resolved setScanMode at %px\n", fn_setScanMode);
	return 0;
}

static int __init ftm5_kprobe_init(void)
{
	int ret;

	pr_info("ftm5_kprobe: loading (MAX_REARMS=%d)\n", MAX_REARMS);

	/* Initialize work struct */
	INIT_WORK(&rearm_work, rearm_work_fn);

	/* Resolve setScanMode function pointer */
	ret = resolve_setScanMode();
	if (ret < 0)
		return ret;

	/* Register kprobe on fts_leave_pointer_event_handler */
	ret = register_kprobe(&kp_leave);
	if (ret < 0) {
		pr_err("ftm5_kprobe: register kp_leave failed: %d\n", ret);
		return ret;
	}
	pr_info("ftm5_kprobe: kp_leave registered at %px\n", kp_leave.addr);

	/* Register kprobe on fts_status_event_handler */
	ret = register_kprobe(&kp_status);
	if (ret < 0) {
		pr_err("ftm5_kprobe: register kp_status failed: %d\n", ret);
		goto err_unreg_leave;
	}
	pr_info("ftm5_kprobe: kp_status registered at %px\n", kp_status.addr);

	/* Register kprobe on fts_enter_pointer_event_handler */
	ret = register_kprobe(&kp_enter);
	if (ret < 0) {
		pr_err("ftm5_kprobe: register kp_enter failed: %d\n", ret);
		goto err_unreg_status;
	}
	pr_info("ftm5_kprobe: kp_enter registered at %px\n", kp_enter.addr);

	pr_info("ftm5_kprobe: loaded successfully — 3 kprobes active\n");
	return 0;

err_unreg_status:
	unregister_kprobe(&kp_status);
err_unreg_leave:
	unregister_kprobe(&kp_leave);
	return ret;
}

static void __exit ftm5_kprobe_exit(void)
{
	/* Cancel any pending work */
	cancel_work_sync(&rearm_work);

	unregister_kprobe(&kp_enter);
	unregister_kprobe(&kp_status);
	unregister_kprobe(&kp_leave);

	pr_info("ftm5_kprobe: unloaded (total suppressed: %lu, "
		"rearm exhaustions: %lu)\n",
		total_suppressed, total_rearms_exhausted);
}

module_init(ftm5_kprobe_init);
module_exit(ftm5_kprobe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis");
MODULE_DESCRIPTION("Kprobe module to suppress force-cal BTN_TOUCH UP on ftm5 (Pixel 6 non-OEM screen)");
MODULE_VERSION("1.0");
