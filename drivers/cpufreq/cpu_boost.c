// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>
#include <linux/threads.h>
#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/minmax.h>
#include <linux/cpu_boost.h>
#include <linux/spinlock.h>

static struct freq_qos_request boost_max_req[NR_CPUS];
static DECLARE_BITMAP(boost_max_registered, NR_CPUS);
static DECLARE_BITMAP(boost_max_active, NR_CPUS);

static struct freq_qos_request boost_kick_req[NR_CPUS];
static DECLARE_BITMAP(boost_kick_registered, NR_CPUS);
static DECLARE_BITMAP(boost_kick_active, NR_CPUS);

static DEFINE_SPINLOCK(pending_lock);
static DECLARE_BITMAP(pending_max_enable, NR_CPUS);
static DECLARE_BITMAP(pending_kick_enable, NR_CPUS);
static bool max_engaged;
static bool kick_engaged;

static atomic_long_t boost_expires = ATOMIC_LONG_INIT(0);

static struct delayed_work boost_disable_work;

static struct notifier_block boost_policy_nb;

static inline bool boost_window_expired(unsigned long now, unsigned long exp)
{
	return time_after(now, exp);
}

static inline s32 kick_khz_for_cpu(int cpu)
{
	if (cpu <= 1)
		return (s32)CONFIG_CPU_BOOST_KICK_KHZ_LITTLE;
	else if (cpu <= 4)
		return (s32)CONFIG_CPU_BOOST_KICK_KHZ_MID;
	else if (cpu <= 6)
		return (s32)CONFIG_CPU_BOOST_KICK_KHZ_BIG;
	else
		return (s32)CONFIG_CPU_BOOST_KICK_KHZ_PRIME;
}

static void apply_freq_min(struct cpufreq_policy *policy, int leader,
			   struct freq_qos_request *reqs,
			   unsigned long *registered, unsigned long *active,
			   s32 req_khz)
{
	if (!test_bit(leader, registered)) {
		if (freq_qos_add_request(&policy->constraints, &reqs[leader],
					 FREQ_QOS_MIN, req_khz) == 0) {
			__set_bit(leader, registered);
			__set_bit(leader, active);
		}
	} else if (!test_bit(leader, active)) {
		if (freq_qos_update_request(&reqs[leader], req_khz) >= 0)
			__set_bit(leader, active);
	}
}

static void cpu_boost_worker(struct work_struct *work)
{
	unsigned long now, exp, delay, flags;
	DECLARE_BITMAP(en_max, NR_CPUS);
	DECLARE_BITMAP(en_kick, NR_CPUS);
	bool has_max, has_kick;
	int cpu, leader;
	struct cpufreq_policy *policy;
	s32 max_khz, req_khz;

	bitmap_zero(en_max, NR_CPUS);
	bitmap_zero(en_kick, NR_CPUS);

	spin_lock_irqsave(&pending_lock, flags);
	has_max = !bitmap_empty(pending_max_enable, NR_CPUS);
	if (has_max) {
		bitmap_copy(en_max, pending_max_enable, NR_CPUS);
		bitmap_zero(pending_max_enable, NR_CPUS);
	}
	has_kick = !bitmap_empty(pending_kick_enable, NR_CPUS);
	if (has_kick) {
		bitmap_copy(en_kick, pending_kick_enable, NR_CPUS);
		bitmap_zero(pending_kick_enable, NR_CPUS);
	}
	spin_unlock_irqrestore(&pending_lock, flags);

	cpus_read_lock();
	if (has_max || has_kick) {
		for_each_online_cpu(cpu) {
			policy = cpufreq_cpu_get(cpu);
			if (!policy)
				continue;

			leader = policy->cpu;
			if (cpu == leader) {
				max_khz = (s32)policy->cpuinfo.max_freq;

				if (has_max && test_bit(leader, en_max))
					apply_freq_min(policy, leader,
						       boost_max_req,
						       boost_max_registered,
						       boost_max_active,
						       max_khz);

				if (has_kick && test_bit(leader, en_kick)) {
					req_khz = kick_khz_for_cpu(leader);
					if (req_khz > 0) {
						if (req_khz > max_khz)
							req_khz = max_khz;
						apply_freq_min(policy, leader,
							       boost_kick_req,
							       boost_kick_registered,
							       boost_kick_active,
							       req_khz);
					}
				}
			}
			cpufreq_cpu_put(policy);
		}

		spin_lock_irqsave(&pending_lock, flags);
		if (has_max)
			max_engaged = true;
		if (has_kick)
			kick_engaged = true;
		spin_unlock_irqrestore(&pending_lock, flags);
	}

	/*
	 * Re-check expiry and pending work under pending_lock before dropping
	 * engaged flags so a concurrent caller extending boost_expires cannot
	 * miss a wakeup.
	 */
	spin_lock_irqsave(&pending_lock, flags);
	now = jiffies;
	exp = atomic_long_read(&boost_expires);
	if (!boost_window_expired(now, exp) ||
	    !bitmap_empty(pending_max_enable, NR_CPUS) ||
	    !bitmap_empty(pending_kick_enable, NR_CPUS)) {
		if (!bitmap_empty(pending_max_enable, NR_CPUS) ||
		    !bitmap_empty(pending_kick_enable, NR_CPUS))
			delay = 0;
		else
			delay = time_after(exp, now) ? exp - now : 1;
		spin_unlock_irqrestore(&pending_lock, flags);
		cpus_read_unlock();
		mod_delayed_work(system_unbound_wq, &boost_disable_work, delay);
		return;
	}
	max_engaged = false;
	kick_engaged = false;
	spin_unlock_irqrestore(&pending_lock, flags);

	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		leader = policy->cpu;
		if (cpu == leader) {
			if (test_and_clear_bit(leader, boost_max_active))
				(void)freq_qos_update_request(&boost_max_req[leader],
							      FREQ_QOS_MIN_DEFAULT_VALUE);

			if (test_and_clear_bit(leader, boost_kick_active))
				(void)freq_qos_update_request(&boost_kick_req[leader],
							      FREQ_QOS_MIN_DEFAULT_VALUE);
		}
		cpufreq_cpu_put(policy);
	}
	cpus_read_unlock();
}

static inline void extend_boost_expiry(unsigned int duration_ms)
{
	unsigned long now = jiffies;
	unsigned long new_exp = now + msecs_to_jiffies(duration_ms);
	unsigned long old = atomic_long_read(&boost_expires);

	for (;;) {
		if (time_after(old, new_exp))
			break;

		if (atomic_long_try_cmpxchg(&boost_expires, &old, new_exp))
			break;
	}
}

void cpu_boost_max(unsigned int duration_ms)
{
	unsigned long flags;
	bool queue = false;

	extend_boost_expiry(duration_ms);

	spin_lock_irqsave(&pending_lock, flags);
	if (!max_engaged) {
		bitmap_fill(pending_max_enable, NR_CPUS);
		queue = true;
	}
	spin_unlock_irqrestore(&pending_lock, flags);

	if (queue)
		mod_delayed_work(system_unbound_wq, &boost_disable_work, 0);
}
EXPORT_SYMBOL_GPL(cpu_boost_max);

void cpu_boost_kick(unsigned int duration_ms)
{
	unsigned long flags;
	bool queue = false;

	extend_boost_expiry(duration_ms);

	spin_lock_irqsave(&pending_lock, flags);
	if (!kick_engaged) {
		bitmap_fill(pending_kick_enable, NR_CPUS);
		queue = true;
	}
	spin_unlock_irqrestore(&pending_lock, flags);

	if (queue)
		mod_delayed_work(system_unbound_wq, &boost_disable_work, 0);
}
EXPORT_SYMBOL_GPL(cpu_boost_kick);

static int boost_policy_notifier(struct notifier_block *nb,
				 unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	int leader = policy ? policy->cpu : -1;

	if (val == CPUFREQ_REMOVE_POLICY && leader >= 0) {
		clear_bit(leader, boost_max_active);
		if (test_and_clear_bit(leader, boost_max_registered))
			freq_qos_remove_request(&boost_max_req[leader]);

		clear_bit(leader, boost_kick_active);
		if (test_and_clear_bit(leader, boost_kick_registered))
			freq_qos_remove_request(&boost_kick_req[leader]);

		{
			unsigned long flags;
			spin_lock_irqsave(&pending_lock, flags);
			__clear_bit(leader, pending_max_enable);
			__clear_bit(leader, pending_kick_enable);
			spin_unlock_irqrestore(&pending_lock, flags);
		}
	}
	return 0;
}

static int __init cpu_boost_init(void)
{
	INIT_DELAYED_WORK(&boost_disable_work, cpu_boost_worker);
	boost_policy_nb.notifier_call = boost_policy_notifier;
	cpufreq_register_notifier(&boost_policy_nb, CPUFREQ_POLICY_NOTIFIER);
	pr_info("cpu_boost driver initialized\n");
	return 0;
}

late_initcall(cpu_boost_init);
