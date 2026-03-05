// SPDX-License-Identifier: GPL-2.0
/*
 * ftm5_kprobe.c — Suppress force-cal BTN_TOUCH UP on ftm5 (Pixel 6)
 *
 * IC event order in one SPI batch:
 *   1. EVT_TYPE_STATUS_FORCE_CAL (event[1]==0x05)
 *   2. EVT_ID_LEAVE_POINT → clears touch_id
 *   3. (loop ends, touch_id==0 → BTN_TOUCH UP)
 *
 * Strategy (v6 — event patching, no setScanMode):
 *   Hook fts_status_event_handler to detect force-cal events.
 *   Hook fts_leave_pointer_event_handler pre: if force-cal pending,
 *   corrupt event[1] lower nibble (touchType) to value > 7.
 *   The handler checks touchType and skips clearing touch_id when invalid.
 *   Result: touch_id stays non-zero → no BTN_TOUCH UP.
 *
 *   Re-arm (setScanMode) is NOT done here — fts_filter handles it
 *   via sysfs. This avoids CFI violations from calling module functions
 *   through resolved pointers.
 *
 *   post_handler restores original event byte after single-step of
 *   first instruction (stack frame setup on ARM64), before handler
 *   actually reads event[0].
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/spinlock.h>
#include <linux/jiffies.h>

/* Struct offsets from ftm5.ko disassembly */
#define OFF_TOUCH_ID		0x11B0

/* Max age for force-cal flag */
#define FORCECAL_MAX_AGE_MS	50

/* Touch type is in lower nibble of event[1] (from disassembly).
 * Valid types: 0,1,2,4,7 (bitmask 0x97). Values > 7 cause skip.
 * We OR in 0x08 to make touchType=8+ while preserving touchId (upper nibble). */
#define TOUCH_TYPE_POISON	0x08

/* Module parameters */
static int max_rearms = 12;
module_param(max_rearms, int, 0644);
MODULE_PARM_DESC(max_rearms, "Max force-cal suppress cycles per touch (default: 12)");

/* Protected state */
static DEFINE_SPINLOCK(state_lock);

struct suppress_state {
	unsigned long forcecal_jiffies;
	bool forcecal_pending;
	int rearm_count;
};

static struct suppress_state st;

/* Statistics */
static unsigned long total_suppressed;
module_param(total_suppressed, ulong, 0444);

static unsigned long total_rearms_exhausted;
module_param(total_rearms_exhausted, ulong, 0444);

static unsigned long total_stale;
module_param(total_stale, ulong, 0444);

static unsigned long hits_forcecal;
module_param(hits_forcecal, ulong, 0444);

static unsigned long hits_status;
module_param(hits_status, ulong, 0444);

static unsigned long hits_leave;
module_param(hits_leave, ulong, 0444);

static unsigned long hits_enter;
module_param(hits_enter, ulong, 0444);

static inline bool forcecal_is_fresh(void)
{
	if (!st.forcecal_pending)
		return false;
	if (jiffies_to_msecs(jiffies - st.forcecal_jiffies) > FORCECAL_MAX_AGE_MS) {
		st.forcecal_pending = false;
		total_stale++;
		return false;
	}
	return true;
}

/*
 * Status pre-handler: detect force-cal, set flag.
 */
static int status_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	unsigned char *event = (unsigned char *)regs->regs[1];
	unsigned long flags;

	hits_status++;

	if (event[1] != 0x05)
		return 0;

	hits_forcecal++;

	spin_lock_irqsave(&state_lock, flags);
	st.forcecal_jiffies = jiffies;
	st.forcecal_pending = true;
	spin_unlock_irqrestore(&state_lock, flags);
	return 0;
}

/*
 * Leave pre-handler: if force-cal pending, corrupt event[0] touchType
 * so the handler skips clearing touch_id.
 */
static int leave_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	unsigned char *event = (unsigned char *)regs->regs[1];
	unsigned long flags;

	hits_leave++;

	spin_lock_irqsave(&state_lock, flags);

	if (!forcecal_is_fresh()) {
		spin_unlock_irqrestore(&state_lock, flags);
		return 0;
	}

	if (st.rearm_count < max_rearms) {
		/* Corrupt touchType in event[1] — handler will skip clear_bit.
		 * Lower nibble = touchType, upper nibble = touchId.
		 * OR in 0x08 → touchType > 7 → handler skips. */
		event[1] |= TOUCH_TYPE_POISON;

		st.rearm_count++;
		total_suppressed++;

		pr_info("ftm5_kprobe: SUPPRESSED (cycle %d/%d)\n",
			st.rearm_count, max_rearms);
	} else {
		total_rearms_exhausted++;
		pr_info("ftm5_kprobe: limit %d, allowing UP\n", max_rearms);
	}

	st.forcecal_pending = false;
	spin_unlock_irqrestore(&state_lock, flags);
	return 0;
}

/*
 * Enter pre-handler: reset rearm counter on new touch-down.
 */
static int enter_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	void *info = (void *)regs->regs[0];
	unsigned long cur_touch_id;
	unsigned long flags;

	cur_touch_id = *(unsigned long *)((char *)info + OFF_TOUCH_ID);
	hits_enter++;

	spin_lock_irqsave(&state_lock, flags);
	if (cur_touch_id == 0) {
		if (st.rearm_count > 0)
			pr_info("ftm5_kprobe: new touch, reset (was %d)\n",
				st.rearm_count);
		st.rearm_count = 0;
		st.forcecal_pending = false;
	}
	spin_unlock_irqrestore(&state_lock, flags);
	return 0;
}

/* Probe definitions */
static struct kprobe kp_status = {
	.symbol_name = "fts_status_event_handler",
	.pre_handler = status_pre_handler,
};

static struct kprobe kp_leave = {
	.symbol_name = "fts_leave_pointer_event_handler",
	.pre_handler = leave_pre_handler,
};

static struct kprobe kp_enter = {
	.symbol_name = "fts_enter_pointer_event_handler",
	.pre_handler = enter_pre_handler,
};

static int __init ftm5_kprobe_init(void)
{
	int ret;

	pr_info("ftm5_kprobe: loading v6.1 (max_rearms=%d)\n", max_rearms);

	ret = register_kprobe(&kp_status);
	if (ret < 0) {
		pr_err("ftm5_kprobe: kp_status failed: %d\n", ret);
		return ret;
	}

	ret = register_kprobe(&kp_leave);
	if (ret < 0) {
		pr_err("ftm5_kprobe: kp_leave failed: %d\n", ret);
		goto err_status;
	}

	ret = register_kprobe(&kp_enter);
	if (ret < 0) {
		pr_err("ftm5_kprobe: kp_enter failed: %d\n", ret);
		goto err_leave;
	}

	pr_info("ftm5_kprobe: v6 loaded — event patching, no setScanMode\n");
	return 0;

err_leave:
	unregister_kprobe(&kp_leave);
err_status:
	unregister_kprobe(&kp_status);
	return ret;
}

static void __exit ftm5_kprobe_exit(void)
{
	unregister_kprobe(&kp_enter);
	unregister_kprobe(&kp_leave);
	unregister_kprobe(&kp_status);

	pr_info("ftm5_kprobe: unloaded (suppressed=%lu exhausted=%lu stale=%lu)\n",
		total_suppressed, total_rearms_exhausted, total_stale);
}

module_init(ftm5_kprobe_init);
module_exit(ftm5_kprobe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Denis");
MODULE_DESCRIPTION("Suppress force-cal BTN_TOUCH UP on ftm5 (Pixel 6 non-OEM screen)");
MODULE_VERSION("6.0");
