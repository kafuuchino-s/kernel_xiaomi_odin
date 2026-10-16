// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Xiaomi Technologies Co., Ltd. 2019. All rights reserved. */
/* Xiaomi MIGT 2.0, derived from projects-nexus/nexus_kernel_xiaomi_sm8250
 * f12f206b5339ad1294642e89195fe08918fb2459 drivers/mihw/migt.c.
 * Dream port: stock task ABI, capacity-scaled WALT input, ordered work.
 */
#define pr_fmt(fmt) "migt: " fmt
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pkg_stat.h>
#include <linux/sched/core_ctl.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define MAX_CEILING_USERS 10
#define VIP_REQ_LIMIT 3
#define MAX_DURATION_MS 60000U
#define MAX_CLUSTER CLUSTER_TYPES

enum { QUEUE_BUFFER = 1, DEQUEUE_BUFFER, SET_CEILING };
enum { NO_MIGT, MIGT_1_0, MIGT_2_0, MIGT_3_0 };
struct migt_cpu {
	unsigned int boost_freq, ceiling_freq, min, max;
};
static DEFINE_PER_CPU(struct migt_cpu, migt_info);
static DEFINE_SPINLOCK(migt_lock);
static DEFINE_MUTEX(ioctl_lock);
/* Private scheduler protocol: serialize the entire policy with UID changes. */
void migt_policy_lock(void)
{
	mutex_lock(&ioctl_lock);
}
void migt_policy_unlock(void)
{
	mutex_unlock(&ioctl_lock);
}
static bool migt_ready, migt_enabled;
static cpumask_var_t active_cluster_cpus;
static struct workqueue_struct *migt_wq;
static struct delayed_work migt_work, migt_rem, ceiling_work, ceiling_rem;
static struct work_struct irq_boost_work;
static struct hrtimer frame_timer, vtask_timer;
static u32 sys_migt_count;
static u64 last_update_time;
static uid_t last_traced_uid;
/* Stock reads the last QUEUE_BUFFER UID; scheduler/GLK share this getter. */
int get_cur_render_uid(void)
{
	return READ_ONCE(last_traced_uid);
}
static int render_pid = -1;
static bool migt_boost_flag, core_boost_active;
static unsigned int boost_cycle;
static int ceiling_users[MAX_CEILING_USERS], ceiling_count;
static bool ceiling_active;
static unsigned long ceiling_next_update;

extern unsigned int minor_window_app;
extern int enable_pkg_monitor, set_render_as_stask, force_stask_to_big;
extern int stask_candidate_num, vip_task_max_num, ip_task_max_num;
extern int need_reset_runtime_everytime, force_reset_runtime, vip_task_schedboost;
extern void add_work_to_migt(int uid);
extern void mi_vip_task_req(int *pid, unsigned int nr, unsigned int times);
extern int get_mi_dynamic_vip_num(void);

int glk_disable = 1, glk_fbreak_enable, glk_freq_limit_start, glk_freq_limit_walt;
unsigned int glk_minfreq[CLUSTER_TYPES], glk_maxfreq[CLUSTER_TYPES];
static unsigned int boost_policy, migt_ms = 50, migt_thresh = 18;
static unsigned int ceiling_slack_ms = 1000, ceiling_from_user;
static unsigned int first_lunch_fps = 100, itask_detect_interval = 5000;
static unsigned int cpu_boost_cycle = 10, high_resolution_enable = 1;
static unsigned int render_update = 1, over_thresh_count, migt_debug;
static unsigned int migt_viptask_thresh = 50, fps_variance_ratio = 10;
static unsigned int fps_mexec_update_policy, perf_mod = 1, tsched_debug;
static int mi_viptask[VIP_REQ_LIMIT];

/* Sysfs writes are bounded before publication; no zero divisor or ms overflow. */
static int set_bounded_uint(const char *buf, const struct kernel_param *kp)
{
	unsigned int val, lo = 0, hi = MAX_DURATION_MS;
	int ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (kp->arg == &migt_thresh || kp->arg == &migt_ms ||
	    kp->arg == &ceiling_slack_ms || kp->arg == &itask_detect_interval ||
	    kp->arg == &cpu_boost_cycle)
		lo = 1;
	if (kp->arg == &boost_policy || kp->arg == &fps_mexec_update_policy)
		hi = kp->arg == &boost_policy ? MIGT_3_0 : 2;
	if (kp->arg == &migt_viptask_thresh)
		hi = 100;
	if (val < lo || val > hi)
		return -ERANGE;
	WRITE_ONCE(*(unsigned int *)kp->arg, val);
	return 0;
}
static const struct kernel_param_ops bounded_uint_ops = {
	.set = set_bounded_uint, .get = param_get_uint,
};
#define bounded_param(name) module_param_cb(name, &bounded_uint_ops, &name, 0644)
bounded_param(boost_policy);
bounded_param(migt_ms);
bounded_param(migt_thresh);
bounded_param(ceiling_slack_ms);
bounded_param(itask_detect_interval);
bounded_param(cpu_boost_cycle);
bounded_param(first_lunch_fps);
bounded_param(migt_viptask_thresh);
bounded_param(fps_variance_ratio);
bounded_param(fps_mexec_update_policy);
module_param(minor_window_app, uint, 0644);
module_param(enable_pkg_monitor, int, 0644);
module_param(set_render_as_stask, int, 0644);
module_param(force_stask_to_big, int, 0644);
module_param(stask_candidate_num, int, 0644);
module_param(vip_task_max_num, int, 0644);
module_param(ip_task_max_num, int, 0644);
module_param(need_reset_runtime_everytime, int, 0644);
module_param(force_reset_runtime, int, 0644);
module_param(glk_disable, int, 0644);
module_param(glk_fbreak_enable, int, 0644);
module_param(glk_freq_limit_start, int, 0644);
module_param(glk_freq_limit_walt, int, 0644);
module_param_array(glk_minfreq, uint, NULL, 0644);
module_param_array(glk_maxfreq, uint, NULL, 0644);
module_param(ceiling_from_user, uint, 0644);
module_param(high_resolution_enable, uint, 0644);
module_param(render_update, uint, 0644);
module_param(over_thresh_count, uint, 0444);
module_param(migt_debug, uint, 0644);
module_param(perf_mod, uint, 0644);
module_param(tsched_debug, uint, 0644);

/* Parse complete request before touching live per-CPU configuration. */
static int parse_freq(char *text, unsigned int *values)
{
	char *p = strim(text), *token, *colon;
	unsigned int cpu, val;
	int ret;
	if (!*p)
		return -EINVAL;
	if (!strchr(p, ':')) {
		ret = kstrtouint(p, 0, &val);
		if (ret)
			return ret;
		if (val > INT_MAX)
			return -ERANGE;
		for_each_possible_cpu(cpu)
			values[cpu] = val;
		return 0;
	}
	while ((token = strsep(&p, " \t\n"))) {
		if (!*token)
			continue;
		colon = strchr(token, ':');
		if (!colon || strchr(colon + 1, ':'))
			return -EINVAL;
		*colon++ = '\0';
		if (kstrtouint(token, 0, &cpu) || kstrtouint(colon, 0, &val) ||
		    cpu >= nr_cpu_ids || !cpu_possible(cpu) || val > INT_MAX)
			return -EINVAL;
		values[cpu] = val;
	}
	return 0;
}
static int set_migt_freq(const char *buf, const struct kernel_param *kp)
{
	unsigned int *values, cpu;
	bool enabled = false, ceiling = strstr(kp->name, "migt_ceiling_freq");
	char *text;
	int ret = -ENOMEM;
	values = kcalloc(nr_cpu_ids, sizeof(*values), GFP_KERNEL);
	text = kstrdup(buf, GFP_KERNEL);
	if (!values || !text)
		goto out;
	for_each_possible_cpu(cpu)
		values[cpu] = ceiling ? READ_ONCE(per_cpu(migt_info, cpu).ceiling_freq) :
			READ_ONCE(per_cpu(migt_info, cpu).boost_freq);
	ret = parse_freq(text, values);
	if (ret)
		goto out;
	for_each_possible_cpu(cpu) {
		if (ceiling)
			WRITE_ONCE(per_cpu(migt_info, cpu).ceiling_freq, values[cpu]);
		else
			WRITE_ONCE(per_cpu(migt_info, cpu).boost_freq, values[cpu]);
		if (READ_ONCE(per_cpu(migt_info, cpu).boost_freq))
			enabled = true;
	}
	WRITE_ONCE(migt_enabled, enabled);
out:
	kfree(text);
	kfree(values);
	return ret;
}
static int get_migt_freq(char *buf, const struct kernel_param *kp)
{
	unsigned int cpu;
	int n = 0;
	bool ceiling = strstr(kp->name, "migt_ceiling_freq");
	for_each_possible_cpu(cpu)
		n += scnprintf(buf + n, PAGE_SIZE - n, "%u:%u ", cpu,
			ceiling ? READ_ONCE(per_cpu(migt_info, cpu).ceiling_freq) :
			READ_ONCE(per_cpu(migt_info, cpu).boost_freq));
	return n + scnprintf(buf + n, PAGE_SIZE - n, "\n");
}
static const struct kernel_param_ops freq_ops = {
	.set = set_migt_freq, .get = get_migt_freq,
};
module_param_cb(migt_freq, &freq_ops, NULL, 0644);
module_param_cb(migt_ceiling_freq, &freq_ops, NULL, 0644);

static int parse_vip(char *text, int *pids, unsigned int *nr, unsigned int *times)
{
	char *p = strim(text), *duration = strchr(p, '-'), *token;
	unsigned int val;
	*nr = 0;
	if (!duration || strchr(duration + 1, '-'))
		return -EINVAL;
	*duration++ = '\0';
	if (kstrtouint(duration, 0, times) || *times > MAX_DURATION_MS)
		return -EINVAL;
	while ((token = strsep(&p, ":"))) {
		if (*nr == VIP_REQ_LIMIT || kstrtouint(token, 0, &val) ||
		    !val || val > INT_MAX)
			return -EINVAL;
		pids[(*nr)++] = val;
	}
	return *nr ? 0 : -EINVAL;
}
static int set_mi_vip_task_req(const char *buf, const struct kernel_param *kp)
{
	int pids[VIP_REQ_LIMIT] = {}, ret;
	unsigned int nr, times;
	char *text;
	if (!smp_load_acquire(&migt_ready))
		return -EAGAIN;
	text = kstrdup(buf, GFP_KERNEL);
	if (!text)
		return -ENOMEM;
	ret = parse_vip(text, pids, &nr, &times);
	if (!ret) {
		memcpy(mi_viptask, pids, sizeof(pids));
		mi_vip_task_req(pids, nr, times);
	}
	kfree(text);
	return ret;
}
static int get_mi_viptask(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%d:%d:%d %d\n", mi_viptask[0],
		mi_viptask[1], mi_viptask[2], get_mi_dynamic_vip_num());
}
static const struct kernel_param_ops vip_ops = {
	.set = set_mi_vip_task_req, .get = get_mi_viptask,
};
module_param_cb(mi_viptask, &vip_ops, NULL, 0644);

/* Base carries the newer freq QoS backport, no CPUFREQ_ADJUST. Requests are
 * private to this driver, removed while the policy is still alive. Never take
 * policy->rwsem under policy_lock: QoS only queues policy->update. */
struct migt_policy {
	struct list_head node;
	struct cpufreq_policy *policy;
	struct freq_qos_request min, max;
};
static LIST_HEAD(migt_policies);
static DEFINE_MUTEX(policy_lock);
static int attach_policy(struct cpufreq_policy *policy)
{
	struct migt_policy *p;
	int ret = 0;
	mutex_lock(&policy_lock);
	list_for_each_entry(p, &migt_policies, node)
		if (p->policy == policy)
			goto unlock;
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p) {
		ret = -ENOMEM;
		goto unlock;
	}
	p->policy = policy;
	ret = freq_qos_add_request(&policy->constraints, &p->min,
		FREQ_QOS_MIN, READ_ONCE(per_cpu(migt_info, policy->cpu).min));
	if (ret < 0)
		goto free;
	ret = freq_qos_add_request(&policy->constraints, &p->max,
		FREQ_QOS_MAX, max(READ_ONCE(per_cpu(migt_info, policy->cpu).min),
			min_t(unsigned int, INT_MAX, READ_ONCE(per_cpu(migt_info, policy->cpu).max))));
	if (ret < 0) {
		freq_qos_remove_request(&p->min);
		goto free;
	}
	list_add_tail(&p->node, &migt_policies);
	ret = 0;
	goto unlock;
free:
	kfree(p);
unlock:
	mutex_unlock(&policy_lock);
	return ret;
}
static void detach_policies(struct cpufreq_policy *policy)
{
	struct migt_policy *p, *next;
	mutex_lock(&policy_lock);
	list_for_each_entry_safe(p, next, &migt_policies, node) {
		if (policy && p->policy != policy)
			continue;
		freq_qos_remove_request(&p->max);
		freq_qos_remove_request(&p->min);
		list_del(&p->node);
		kfree(p);
	}
	mutex_unlock(&policy_lock);
}
/* Called with CPU hotplug read lock, including initial enumeration. */
static int attach_online_policies(void)
{
	struct cpufreq_policy *policy;
	int cpu, ret = 0;
	for_each_online_cpu(cpu) {
		cpumask_and(active_cluster_cpus, cpu_coregroup_mask(cpu), cpu_online_mask);
		if (cpu != cpumask_first(active_cluster_cpus))
			continue;
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		ret = attach_policy(policy);
		cpufreq_cpu_put(policy);
		if (ret)
			break;
	}
	return ret;
}
static void update_policy_online(void)
{
	struct migt_policy *p;
	get_online_cpus();
	if (attach_online_policies())
		pr_err_ratelimited("cannot attach policy QoS requests\n");
	mutex_lock(&policy_lock);
	list_for_each_entry(p, &migt_policies, node) {
		unsigned int cpu = p->policy->cpu;
		unsigned int min = READ_ONCE(per_cpu(migt_info, cpu).min);
		unsigned int max = min_t(unsigned int, INT_MAX,
			READ_ONCE(per_cpu(migt_info, cpu).max));
		int ret;
		max = max(min, max);
		ret = freq_qos_update_request(&p->max, max);
		if (ret < 0)
			pr_err_ratelimited("max QoS update failed: %d\n", ret);
		ret = freq_qos_update_request(&p->min, min);
		if (ret < 0)
			pr_err_ratelimited("min QoS update failed: %d\n", ret);
	}
	mutex_unlock(&policy_lock);
	put_online_cpus();
}
static int boost_adjust_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	int ret;
	if (event == CPUFREQ_REMOVE_POLICY)
		detach_policies(policy);
	else if (event == CPUFREQ_CREATE_POLICY) {
		ret = attach_policy(policy);
		if (ret) {
			pr_err("policy QoS allocation failed: %d\n", ret);
			return notifier_from_errno(ret);
		}
	}
	return NOTIFY_OK;
}
static struct notifier_block boost_adjust_nb = { .notifier_call = boost_adjust_notify };

static void do_migt_rem(struct work_struct *work)
{
	int cpu;
	if (boost_cycle > 1) {
		boost_cycle--;
		glk_force_maxfreq_break(true);
		mod_delayed_work(migt_wq, &migt_rem, msecs_to_jiffies(READ_ONCE(migt_ms)));
		return;
	}
	boost_cycle = 0;
	glk_force_maxfreq_break(false);
	glk_maxfreq_break(false);
	if (core_boost_active) {
		core_ctl_set_boost(false);
		core_boost_active = false;
	}
	for_each_possible_cpu(cpu)
		WRITE_ONCE(per_cpu(migt_info, cpu).min, 0);
	WRITE_ONCE(vip_task_schedboost, 0);
	update_policy_online();
}
static void do_migt(struct work_struct *work)
{
	int cpu, cluster = -1;
	cancel_delayed_work(&migt_rem);
	if (!READ_ONCE(migt_enabled) || !READ_ONCE(boost_policy)) {
		boost_cycle = 0;
		do_migt_rem(work);
		return;
	}
	if (!core_boost_active) {
		core_ctl_set_boost(true);
		core_boost_active = true;
	}
	WRITE_ONCE(vip_task_schedboost, 1);
	glk_maxfreq_break(true);
	for_each_possible_cpu(cpu) {
		if (cpu == cpumask_first(cpu_coregroup_mask(cpu)))
			cluster++;
		WRITE_ONCE(per_cpu(migt_info, cpu).min,
			cluster >= MID_CLUSTER &&
			(READ_ONCE(boost_policy) != MIGT_1_0 || cluster == MID_CLUSTER) ?
			READ_ONCE(per_cpu(migt_info, cpu).boost_freq) : 0);
	}
	boost_cycle = READ_ONCE(cpu_boost_cycle);
	update_policy_online();
	mod_delayed_work(migt_wq, &migt_rem, msecs_to_jiffies(READ_ONCE(migt_ms)));
}
static enum hrtimer_restart do_boost_work(struct hrtimer *timer)
{
	if (smp_load_acquire(&migt_ready))
		queue_work(migt_wq, &irq_boost_work);
	return HRTIMER_NORESTART;
}
static void do_ceiling_rem(struct work_struct *work)
{
	int cpu;
	for_each_possible_cpu(cpu)
		WRITE_ONCE(per_cpu(migt_info, cpu).max, UINT_MAX);
	WRITE_ONCE(ceiling_active, false);
	update_policy_online();
}
static void do_ceiling_limit(struct work_struct *work)
{
	int cpu;
	if (!READ_ONCE(ceiling_active))
		return;
	if (!READ_ONCE(ceiling_from_user)) {
		update_freq_limit((u64)READ_ONCE(migt_thresh) * NSEC_PER_MSEC);
		return;
	}
	for_each_possible_cpu(cpu) {
		unsigned int freq = READ_ONCE(per_cpu(migt_info, cpu).ceiling_freq);
		WRITE_ONCE(per_cpu(migt_info, cpu).max, freq ?: UINT_MAX);
	}
	update_policy_online();
}
void trigger_ceiling_limit(void)
{
	if (smp_load_acquire(&migt_ready) && READ_ONCE(ceiling_active))
		mod_delayed_work(migt_wq, &ceiling_work, 0);
}
static bool ceiling_user(uid_t uid)
{
	int i;
	for (i = 0; i < ceiling_count; i++)
		if (ceiling_users[i] == uid)
			return true;
	return false;
}
static void add_ceiling_user(uid_t uid)
{
	if (ceiling_user(uid))
		return;
	if (ceiling_count == MAX_CEILING_USERS) {
		memmove(ceiling_users, ceiling_users + 1,
			(MAX_CEILING_USERS - 1) * sizeof(*ceiling_users));
		ceiling_count--;
	}
	ceiling_users[ceiling_count++] = uid;
}

int package_runtime_should_stop(void) { return !READ_ONCE(boost_policy); }
int migt_enable(void) { return READ_ONCE(boost_policy); }
u64 get_migt_thresh(void) { return READ_ONCE(migt_thresh); }

static void trigger_migt(uid_t uid)
{
	unsigned int policy = READ_ONCE(boost_policy), count, thresh = READ_ONCE(migt_thresh);
	unsigned long flags;
	u64 now = ktime_get_ns();
	bool detect;
	if (!READ_ONCE(migt_enabled) || !policy)
		return;
	spin_lock_irqsave(&migt_lock, flags);
	count = ++sys_migt_count;
	detect = count > READ_ONCE(first_lunch_fps) && policy >= MIGT_2_0 &&
		(count % READ_ONCE(itask_detect_interval) == 0 || current->pid != render_pid);
	if (now >= last_update_time && now - last_update_time >= (u64)thresh * NSEC_PER_MSEC)
		over_thresh_count++;
	migt_boost_flag = false;
	last_update_time = now;
	render_pid = current->pid;
	spin_unlock_irqrestore(&migt_lock, flags);
	WRITE_ONCE(glk_freq_limit_start, count > READ_ONCE(first_lunch_fps));
	if (detect && READ_ONCE(render_update))
		add_work_to_migt(uid);
	if (ceiling_user(uid)) {
		WRITE_ONCE(ceiling_active, true);
		if (time_after_eq(jiffies, ceiling_next_update)) {
			ceiling_next_update = jiffies + msecs_to_jiffies(READ_ONCE(ceiling_slack_ms) / 2);
			mod_delayed_work(migt_wq, &ceiling_rem, msecs_to_jiffies(READ_ONCE(ceiling_slack_ms)));
		}
	}
	if (policy == MIGT_2_0)
		return;
	if (policy == MIGT_3_0) {
		game_load_history_update(count);
		trigger_ceiling_limit();
		thresh += thresh / 2;
	}
	if (READ_ONCE(high_resolution_enable))
		hrtimer_start(&frame_timer, ms_to_ktime(thresh), HRTIMER_MODE_REL);
	else
		mod_delayed_work(migt_wq, &migt_work, msecs_to_jiffies(thresh));
}
static long migt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	uid_t uid = from_kuid(&init_user_ns, current_uid());
	unsigned long flags;
	int type;
	if (!smp_load_acquire(&migt_ready))
		return -EAGAIN;
	/* Stock/HAL uses NR only, no demonstrated fixed magic or payload. */
	if (_IOC_NR(cmd) < QUEUE_BUFFER || _IOC_NR(cmd) > SET_CEILING)
		return -ENOTTY;
	mutex_lock(&ioctl_lock);
	switch (_IOC_NR(cmd)) {
	case QUEUE_BUFFER:
		if (uid != READ_ONCE(last_traced_uid)) {
			/* Block old-UID hooks before draining their timers/work. */
			spin_lock_irqsave(&migt_lock, flags);
			last_traced_uid = 0;
			sys_migt_count++;
			spin_unlock_irqrestore(&migt_lock, flags);
			for (type = 0; type < RENDER_TYPES; type++)
				reset_render_info(type);
			game_load_reset();
			hrtimer_cancel(&frame_timer);
			hrtimer_cancel(&vtask_timer);
			cancel_delayed_work_sync(&migt_work);
			cancel_work_sync(&irq_boost_work);
			cancel_delayed_work_sync(&ceiling_work);
			cancel_delayed_work_sync(&migt_rem);
			boost_cycle = 0;
			mod_delayed_work(migt_wq, &ceiling_rem, 0);
			mod_delayed_work(migt_wq, &migt_rem, 0);
			flush_delayed_work(&ceiling_rem);
			flush_delayed_work(&migt_rem);
			spin_lock_irqsave(&migt_lock, flags);
			sys_migt_count = 0;
			last_traced_uid = uid;
			last_update_time = ktime_get_ns();
			migt_boost_flag = false;
			spin_unlock_irqrestore(&migt_lock, flags);
		}
		update_render_info(current, RENDER_QUEUE_THREAD);
		trigger_migt(uid);
		break;
	case DEQUEUE_BUFFER:
		update_render_info(current, RENDER_DEQUEUE_THREAD);
		break;
	case SET_CEILING:
		/* HAL can issue this from another UID; stock targets queued renderer. */
		add_ceiling_user(READ_ONCE(last_traced_uid));
		break;
	}
	mutex_unlock(&ioctl_lock);
	return 0;
}
static int migt_mmap(struct file *file, struct vm_area_struct *vma)
{
	return -ENODEV;
}
static const struct file_operations migt_fops = {
	.owner = THIS_MODULE, .unlocked_ioctl = migt_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = migt_ioctl,
#endif
	.mmap = migt_mmap, .llseek = no_llseek,
};
static struct miscdevice migt_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "migt", .fops = &migt_fops,
};

static u64 sat_add(u64 a, u64 b) { return a > U64_MAX - b ? U64_MAX : a + b; }
static unsigned int get_bucket_index(u64 exec, unsigned int thresh)
{
	u64 fast_div = div64_u64(NUM_MIGT_BUCKETS * MSEC_PER_SEC, max(thresh, 1U));
	/* Preserve stock modulo buckets; reduce input before multiplying. */
	u64 bounded = exec % (NUM_MIGT_BUCKETS * (u64)NSEC_PER_SEC);
	return div64_u64(bounded * fast_div, NSEC_PER_SEC) % NUM_MIGT_BUCKETS;
}
void migt_hook(struct task_struct *tsk, u64 delta, int cpu)
{
	unsigned long flags;
	unsigned int policy, thresh, index, i, frame;
	uid_t uid;
	u64 start, diff, previous, now = ktime_get_ns(), monitor;
	bool request = false;
	if (!smp_load_acquire(&migt_ready) || !tsk || cpu < 0 || cpu >= nr_cpu_ids ||
	    !cpu_possible(cpu))
		return;
	policy = READ_ONCE(boost_policy);
	if (policy < MIGT_2_0)
		return;
	/* update_pkg_load already receives capacity-scaled WALT delta. */
	if (policy == MIGT_3_0)
		game_load_update(tsk, delta, cpu);
	spin_lock_irqsave(&migt_lock, flags);
	uid = last_traced_uid;
	frame = sys_migt_count;
	start = last_update_time;
	spin_unlock_irqrestore(&migt_lock, flags);
	thresh = READ_ONCE(migt_thresh);
	monitor = div64_u64((u64)thresh * NSEC_PER_MSEC * READ_ONCE(migt_viptask_thresh), 100);
	diff = now >= start ? now - start : 0;
	write_lock_irqsave(&tsk->pkg.lock, flags);
	/* UID/exit can change despite a task reference; recheck under shared lock. */
	if (uid < USER_PKG_MIN_UID || from_kuid(&init_user_ns, task_uid(tsk)) != uid ||
	    (tsk->flags & PF_EXITING))
		goto unlock;
	if (frame != tsk->pkg.migt.migt_count) {
		previous = delta > diff ? delta - diff : delta;
		tsk->pkg.migt.fps_exec = sat_add(tsk->pkg.migt.fps_exec, previous);
		delta -= previous;
		if (tsk->pkg.migt.fps_exec > monitor) {
			switch (READ_ONCE(fps_mexec_update_policy)) {
			case 0:
				tsk->pkg.migt.fps_mexec = sat_add(tsk->pkg.migt.fps_mexec, 1);
				break;
			case 1:
				tsk->pkg.migt.fps_mexec = (tsk->pkg.migt.fps_mexec >> 1) +
					(tsk->pkg.migt.fps_exec >> 1) +
					((tsk->pkg.migt.fps_mexec & 1) && (tsk->pkg.migt.fps_exec & 1));
				break;
			default:
				tsk->pkg.migt.fps_mexec = max(tsk->pkg.migt.fps_mexec, tsk->pkg.migt.fps_exec);
			}
		}
		index = get_bucket_index(tsk->pkg.migt.fps_exec, thresh);
		for (i = 0; i <= index; i++)
			if (tsk->pkg.migt.bucket[i] != U32_MAX)
				tsk->pkg.migt.bucket[i]++;
		tsk->pkg.migt.fps_exec = 0;
		tsk->pkg.migt.migt_count = frame;
	}
	tsk->pkg.migt.fps_exec = sat_add(tsk->pkg.migt.fps_exec, delta);
	index = get_bucket_index(tsk->pkg.migt.fps_exec, thresh);
	request = (u64)tsk->pkg.migt.bucket[index] * READ_ONCE(fps_variance_ratio) <
		tsk->pkg.migt.bucket[0] && (tsk->pkg.migt.flag & MASK_ITASK) &&
		tsk->pkg.migt.fps_exec > monitor;
unlock:
	write_unlock_irqrestore(&tsk->pkg.lock, flags);
	if (request) {
		spin_lock_irqsave(&migt_lock, flags);
		if (uid == last_traced_uid && frame == sys_migt_count && !migt_boost_flag) {
			migt_boost_flag = true;
			/* No cancelling timer or sleep under RQ/pkg locks. */
			hrtimer_start(&vtask_timer, ns_to_ktime(0), HRTIMER_MODE_REL);
		}
		spin_unlock_irqrestore(&migt_lock, flags);
	}
}
static int __init migt_init(void)
{
	int cpu, ret;
	if (!alloc_cpumask_var(&active_cluster_cpus, GFP_KERNEL))
		return -ENOMEM;
	migt_wq = alloc_ordered_workqueue("migt_wq", WQ_HIGHPRI);
	if (!migt_wq) {
		ret = -ENOMEM;
		goto free_mask;
	}
	INIT_DELAYED_WORK(&migt_work, do_migt);
	INIT_WORK(&irq_boost_work, do_migt);
	INIT_DELAYED_WORK(&migt_rem, do_migt_rem);
	INIT_DELAYED_WORK(&ceiling_work, do_ceiling_limit);
	INIT_DELAYED_WORK(&ceiling_rem, do_ceiling_rem);
	hrtimer_init(&frame_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	frame_timer.function = do_boost_work;
	hrtimer_init(&vtask_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vtask_timer.function = do_boost_work;
	for_each_possible_cpu(cpu)
		per_cpu(migt_info, cpu).max = UINT_MAX;
	get_online_cpus();
	ret = cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		goto unlock_cpus;
	ret = attach_online_policies();
	if (ret)
		goto unregister_notifier;
	ret = misc_register(&migt_misc);
	if (ret)
		goto unregister_notifier;
	smp_store_release(&migt_ready, true);
	put_online_cpus();
	pr_info("initialized\n");
	return 0;
unregister_notifier:
	cpufreq_unregister_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
	detach_policies(NULL);
unlock_cpus:
	put_online_cpus();
	destroy_workqueue(migt_wq);
	migt_wq = NULL;
free_mask:
	free_cpumask_var(active_cluster_cpus);
	return ret;
}
late_initcall(migt_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Dream MIGT frame boost and game ceiling");
