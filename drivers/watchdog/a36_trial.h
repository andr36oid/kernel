/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DW_A36_TRIAL_H
#define _DW_A36_TRIAL_H

#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/a36_kernel_trial.h>
#include <linux/timekeeping.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/workqueue.h>

/* Presence enables a one-shot trial; ordinary boots never enter this path. */
static unsigned int a36_trial_limit;
static int __init a36_trial_limit_setup(char *value)
{
	unsigned long long seconds = 600;

	if (value && *value && kstrtoull(value, 0, &seconds)) {
		pr_warn("trial: invalid limit; using 600 s\n");
		seconds = 600;
	}
	a36_trial_limit = clamp_t(unsigned long long, seconds, 60, 1800);
	return 1;
}
__setup("a36_trial_limit=", a36_trial_limit_setup);

#if IS_BUILTIN(CONFIG_DW_WATCHDOG)
bool a36_kernel_trial_requested(void)
{
	return a36_trial_limit != 0;
}
#endif


struct a36_trial_watchdog {
	struct delayed_work work;
	struct device *dev;
	void __iomem *regs;
	u64 deadline_ns;
	u64 next_notice_ns;
	u64 cutoff_ns;
	u64 hardware_ns;
	u32 top;
	bool countdown_committed;
	unsigned int limit;
	unsigned long period_jiffies;
	bool active;
};

/* A pure decision helper, also exercised by the host/guest fault model. */
static bool a36_trial_expired(u64 now, u64 deadline)
{
	return now >= deadline;
}

static void a36_trial_work(struct work_struct *work)
{
	struct a36_trial_watchdog *trial = container_of(to_delayed_work(work),
					struct a36_trial_watchdog, work);
	unsigned long flags, delay;
	u64 now;
	bool committed_now = false;

	/* No scheduling or interrupt handler may delay a stale-budget reload.
	 * One millisecond of programming margin is reserved at the cutoff.
	 */
	local_irq_save(flags);
	now = ktime_to_ns(ktime_get_boottime());
	if (!trial->countdown_committed) {
		if (now >= trial->cutoff_ns) {
			trial->countdown_committed = true;
			committed_now = true;
		} else {
			writel(WDOG_COUNTER_RESTART_KICK_VALUE,
			       trial->regs + WDOG_COUNTER_RESTART_REG_OFFSET);
		}
	}
	local_irq_restore(flags);

	if (committed_now)
		dev_notice(trial->dev,
			   "trial: final hardware countdown; no further feeds\n");
	if (a36_trial_expired(now, trial->deadline_ns)) {
		/* A real working counter has already reset us by this point. */
		dev_emerg(trial->dev,
			  "trial: deadline passed without hardware reset; not feeding\n");
		return;
	}
	if (now >= trial->next_notice_ns) {
		dev_notice(trial->dev, "trial: %llu s remain\n",
			   (unsigned long long)DIV_ROUND_UP_ULL(
				trial->deadline_ns - now, NSEC_PER_SEC));
		trial->next_notice_ns = now + 60ULL * NSEC_PER_SEC;
	}
	delay = min_t(unsigned long, trial->period_jiffies,
		      nsecs_to_jiffies(trial->deadline_ns - now) + 1);
	/* Logging may continue after cutoff; no code on that path reloads. */
	queue_delayed_work(system_highpri_wq, &trial->work, delay);
}

static int a36_trial_begin(struct a36_trial_watchdog *trial,
			   struct device *dev, void __iomem *regs,
			   unsigned long rate)
{
	u64 now;
	u32 control;
	unsigned long flags;
	int top;

	if (!a36_trial_limit)
		return 0;
	if (!of_machine_is_compatible("rockchip,px30") &&
	    !of_machine_is_compatible("rockchip,rk3326"))
		return -EINVAL;
	if (!rate || rate > (1ULL << 31))
		return -EINVAL;

	trial->dev = dev;
	trial->regs = regs;
	trial->limit = a36_trial_limit;
	/* Pick the longest fixed TOP that fits the whole trial. */
	for (top = 15; top >= 0; top--) {
		u64 ns = DIV_ROUND_UP_ULL((1ULL << (16 + top)) * NSEC_PER_SEC,
					 rate);
		if (ns + NSEC_PER_MSEC < (u64)trial->limit * NSEC_PER_SEC) {
			trial->top = top;
			trial->hardware_ns = ns;
			break;
		}
	}
	if (top < 0)
		return -EINVAL;
	trial->period_jiffies = max_t(unsigned long, 1,
		min_t(unsigned long, HZ,
		      nsecs_to_jiffies(trial->hardware_ns / 4)));
	control = readl(regs + WDOG_CONTROL_REG_OFFSET);
	if (!(control & WDOG_CONTROL_REG_WDT_EN_MASK))
		dev_warn(dev, "trial: loader watchdog idle; arming late, no pre-probe protection\n");

	/* Keep the live loader timer while handing over, then use a fixed TOP
	 * that fits the deadline. Stop feeds one hardware period before it. Never reset/disarm
	 * the peripheral.
	 */
	local_irq_save(flags);
	now = ktime_to_ns(ktime_get_boottime());
	trial->deadline_ns = now + (u64)trial->limit * NSEC_PER_SEC;
	trial->cutoff_ns = trial->deadline_ns - trial->hardware_ns - NSEC_PER_MSEC;
	trial->next_notice_ns = now + 60ULL * NSEC_PER_SEC;
	writel(trial->top | (trial->top << 4),
	       regs + WDOG_TIMEOUT_RANGE_REG_OFFSET);
	control &= ~WDOG_CONTROL_REG_RESP_MODE_MASK;
	control |= WDOG_CONTROL_REG_WDT_EN_MASK;
	writel(control, regs + WDOG_CONTROL_REG_OFFSET);
	writel(WDOG_COUNTER_RESTART_KICK_VALUE,
	       regs + WDOG_COUNTER_RESTART_REG_OFFSET);
	local_irq_restore(flags);
	trial->active = true;
	INIT_DELAYED_WORK(&trial->work, a36_trial_work);
	dev_notice(dev, "trial: reset in %u s; pclk=%lu Hz; no userspace takeover\n",
		   trial->limit, rate);
	queue_delayed_work(system_highpri_wq, &trial->work,
			   trial->period_jiffies);
	return 1;
}

/* Administrative unbind must not disarm or free a still-running worker.
 * Leave clocks and hardware running: with no further feeds it resets early.
 */
static void a36_trial_remove(struct a36_trial_watchdog *trial)
{
	cancel_delayed_work_sync(&trial->work);
}
#endif
