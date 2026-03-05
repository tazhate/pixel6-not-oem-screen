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
 *   touch_id:        0x11B0  (unsigned long, touch slot bitmask)
 *   palm_touch_mask: 0x11B8  (unsigned long)
 *   grip_touch_mask: 0x11C0  (unsigned long)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>

/* Struct offsets from ftm5.ko disassembly */
#define OFF_TOUCH_ID		0x11B0
#define OFF_PALM_TOUCH_MASK	0x11B8
#define OFF_GRIP_TOUCH_MASK	0x11C0

/* Scan mode constants (from ftsSoftware.h) */
#define SCAN_MODE_LOCKED	0x03
#define LOCKED_ACTIVE		0x00

/*
 * Max age for leave_in_progress flag (in jiffies).
 * Events in the same SPI batch are processed in <1ms.
 * 5ms (≈1-2 jiffies at HZ=250) is generous but prevents stale state
 * from previous touch events leaking into new ones.
 */
#define LEAVE_MAX_AGE_MS	5

/* Module parameters — tunable at load time or via sysfs */
static int max_rearms = 12;
module_param(max_rearms, int, 0644);
MODULE_PARM_DESC(max_rearms, "Max force-cal suppress cycles per touch (default: 12)");

/*
 * Protected state between kprobe 1 (leave) and kprobe 2 (status).
 * All fields guarded by state_lock.
 *
 * The ftm5 driver processes events under its own mutex, so concurrent
 * kprobe invocations shouldn't happen. The spinlock protects against
 * theoretical SMP races and ensures memory ordering.
 */
static DEFINE_SPINLOCK(state_lock);

struct suppress_state {
	unsigned long touch_id;
	unsigned long palm_mask;
	unsigned long grip_mask;
	void *info;		/* fts_ts_info pointer */
	unsigned long leave_jiffies;	/* when leave_pre fired */
	bool leave_pending;	/* leave handler cleared touch_id */
	int rearm_count;	/* per-touch rearm counter */
};

static struct suppress_state st;

/* Resolved function pointer for setScanMode */
static int (*fn_setScanMode)(void *info, u8 mode, u8 settings);

/* Info pointer for workqueue (set under state_lock, read in work fn) */
static void *work_info;

/* Work struct for deferred setScanMode call */
static struct work_struct rearm_work;

/* Statistics — readable via /sys/module/ftm5_kprobe/parameters/ */
static unsigned long total_suppressed;
module_param(total_suppressed, ulong, 0444);
MODULE_PARM_DESC(total_suppressed, "Total force-cal events suppressed");

static unsigned long total_rearms_exhausted;
module_param(total_rearms_exhausted, ulong, 0444);
MODULE_PARM_DESC(total_rearms_exhausted, "Times rearm limit was reached");

static unsigned long total_stale_leaves;
module_param(total_stale_leaves, ulong, 0444);
MODULE_PARM_DESC(total_stale_leaves, "Stale leave_pending states discarded");

/*
 * Workqueue handler: re-arm IC to LOCKED_ACTIVE mode.
 * Runs in process context so SPI I/O is safe.
 */
static void rearm_work_fn(struct work_struct *work)
{
	void *info;
	int ret;

	/* Read the info pointer that was set before scheduling */
	info = READ_ONCE(work_info);
	if (!fn_setScanMode || !info)
		return;

	ret = fn_setScanMode(info, SCAN_MODE_LOCKED, LOCKED_ACTIVE);
	if (ret < 0)
		pr_warn("ftm5_kprobe: setScanMode failed: %d\n", ret);
}

/*
 * Check if leave_pending is fresh (same SPI batch).
 * Must be called with state_lock held.
 */
static inline bool leave_is_fresh(void)
{
	if (!st.leave_pending)
		return false;

	if (jiffies_to_msecs(jiffies - st.leave_jiffies) > LEAVE_MAX_AGE_MS) {
		st.leave_pending = false;
		total_stale_leaves++;
		return false;
	}
	return true;
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
	unsigned long cur_touch_id;
	unsigned long flags;

	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);

	if (cur_touch_id != 0) {
		spin_lock_irqsave(&state_lock, flags);
		st.touch_id = cur_touch_id;
		st.palm_mask = *(unsigned long *)((char *)info + OFF_PALM_TOUCH_MASK);
		st.grip_mask = *(unsigned long *)((char *)info + OFF_GRIP_TOUCH_MASK);
		st.info = info;
		st.leave_jiffies = jiffies;
		st.leave_pending = true;
		spin_unlock_irqrestore(&state_lock, flags);
	}

	return 0;
}

/*
 * Kprobe 1 post-handler: check if touch_id was actually cleared.
 * If not (e.g. multitouch with other fingers still down), cancel restore.
 */
static void leave_post_handler(struct kprobe *p, struct pt_regs *regs,
			       unsigned long flags_unused)
{
	unsigned long cur_touch_id;
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);
	if (!st.leave_pending || !st.info) {
		spin_unlock_irqrestore(&state_lock, flags);
		return;
	}

	cur_touch_id = *(unsigned long *)((char *)st.info + OFF_TOUCH_ID);

	/*
	 * If touch_id wasn't fully cleared → other fingers still active.
	 * Don't suppress — this isn't a force-cal-induced total lift.
	 */
	if (cur_touch_id != 0) {
		st.leave_pending = false;
	}
	spin_unlock_irqrestore(&state_lock, flags);
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
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);

	/* Check leave is fresh (same SPI batch) */
	if (!leave_is_fresh()) {
		spin_unlock_irqrestore(&state_lock, flags);
		return 0;
	}

	/* Must be a force-cal status event (event[1] == 0x05) */
	if (event[1] != 0x05) {
		st.leave_pending = false;
		spin_unlock_irqrestore(&state_lock, flags);
		return 0;
	}

	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);

	if (cur_touch_id == 0 && st.touch_id != 0) {
		if (st.rearm_count < max_rearms) {
			/* Restore touch_id — prevent BTN_TOUCH UP */
			*(unsigned long *)((char *)info + OFF_TOUCH_ID) = st.touch_id;
			*(unsigned long *)((char *)info + OFF_PALM_TOUCH_MASK) = st.palm_mask;
			*(unsigned long *)((char *)info + OFF_GRIP_TOUCH_MASK) = st.grip_mask;

			st.rearm_count++;
			total_suppressed++;

			pr_info("ftm5_kprobe: force-cal suppressed (cycle %d/%d, "
				"sub=0x%02x, touch_id=0x%lx)\n",
				st.rearm_count, max_rearms,
				event[2], st.touch_id);

			/* Save info ptr for workqueue and schedule re-arm */
			WRITE_ONCE(work_info, st.info);
			schedule_work(&rearm_work);
		} else {
			total_rearms_exhausted++;
			pr_info("ftm5_kprobe: rearm limit reached (%d), "
				"allowing BTN_UP\n", max_rearms);
		}
	}

	st.leave_pending = false;
	spin_unlock_irqrestore(&state_lock, flags);
	return 0;
}

/*
 * Kprobe 3: pre-handler for fts_enter_pointer_event_handler
 *
 * Reset rearm counter on new touch-down (real ENTER_POINT).
 * Also clear any stale leave_pending state.
 */
static int enter_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	void *info = (void *)regs->regs[0];
	unsigned long cur_touch_id;
	unsigned long flags;

	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);

	spin_lock_irqsave(&state_lock, flags);

	/* New touch-down (touch_id was 0 before this enter) → reset state */
	if (cur_touch_id == 0) {
		if (st.rearm_count > 0)
			pr_info("ftm5_kprobe: new touch-down, reset rearm "
				"(was %d)\n", st.rearm_count);
		st.rearm_count = 0;
		st.leave_pending = false;
	}

	spin_unlock_irqrestore(&state_lock, flags);
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

	pr_info("ftm5_kprobe: loading v2 (max_rearms=%d)\n", max_rearms);

	INIT_WORK(&rearm_work, rearm_work_fn);

	ret = resolve_setScanMode();
	if (ret < 0)
		return ret;

	ret = register_kprobe(&kp_leave);
	if (ret < 0) {
		pr_err("ftm5_kprobe: register kp_leave failed: %d\n", ret);
		return ret;
	}
	pr_info("ftm5_kprobe: kp_leave at %px\n", kp_leave.addr);

	ret = register_kprobe(&kp_status);
	if (ret < 0) {
		pr_err("ftm5_kprobe: register kp_status failed: %d\n", ret);
		goto err_unreg_leave;
	}
	pr_info("ftm5_kprobe: kp_status at %px\n", kp_status.addr);

	ret = register_kprobe(&kp_enter);
	if (ret < 0) {
		pr_err("ftm5_kprobe: register kp_enter failed: %d\n", ret);
		goto err_unreg_status;
	}
	pr_info("ftm5_kprobe: kp_enter at %px\n", kp_enter.addr);

	pr_info("ftm5_kprobe: loaded — 3 kprobes active\n");
	return 0;

err_unreg_status:
	unregister_kprobe(&kp_status);
err_unreg_leave:
	unregister_kprobe(&kp_leave);
	return ret;
}

static void __exit ftm5_kprobe_exit(void)
{
	cancel_work_sync(&rearm_work);

	unregister_kprobe(&kp_enter);
	unregister_kprobe(&kp_status);
	unregister_kprobe(&kp_leave);

	pr_info("ftm5_kprobe: unloaded (suppressed=%lu exhausted=%lu stale=%lu)\n",
		total_suppressed, total_rearms_exhausted, total_stale_leaves);
}

module_init(ftm5_kprobe_init);
module_exit(ftm5_kprobe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis");
MODULE_DESCRIPTION("Kprobe module to suppress force-cal BTN_TOUCH UP on ftm5 (Pixel 6 non-OEM screen)");
MODULE_VERSION("2.0");
