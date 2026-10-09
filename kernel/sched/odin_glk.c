// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) Xiaomi Technologies Co., Ltd. 2019. All rights reserved. */
/* Xiaomi Game load tracking, derived from projects-nexus/nexus_kernel_xiaomi_sm8250
 * f12f206b5339ad1294642e89195fe08918fb2459 kernel/sched/glk.c.
 * Dream ABI port: independent per-CPU runtime locks, serialized history snapshots.
 */
#define pr_fmt(fmt) "glk: " fmt
#include <linux/cred.h>
#include <linux/cpufreq.h>
#include <linux/irq_work.h>
#include <linux/module.h>
#include <linux/pkg_stat.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include "sched.h"
#include "walt/walt.h"
#include "odin_glk.h"

#define FRAME_LOAD_HISITMES 21
enum { FREQ_CPU_LOAD, FREQ_GAME_LOAD, FREQ_STASK_LOAD, FREQ_VIP_LOAD,
	FREQ_RENDER_LOAD, FREQ_LOAD_ITEMS };
struct game_load {
	u64 runtime[FREQ_LOAD_ITEMS], previous[FREQ_LOAD_ITEMS];
	raw_spinlock_t lock;
	int cluster;
};
struct game_load_items {
	u64 cpu_util[CLUSTER_TYPES], frame_start, frame_period;
};
static DEFINE_PER_CPU(struct game_load, load_info);
static DEFINE_RAW_SPINLOCK(history_lock);
static struct game_load_items history[FRAME_LOAD_HISITMES];
static struct cpumask cluster_cpumask[CLUSTER_TYPES];
static unsigned int cluster_number, cur_history_items;
static u64 last_update_time, cpu_util_freq_divisor;
static bool game_load_has_init, glk_maxfreq_limit = true, glk_minfreq_limit = true;
static bool force_maxfreq_break;
static int sysctl_game_load_choose_policy, perf_mod, game_load_debug;
static unsigned int glk_highspeed_load[CLUSTER_TYPES] = {90, 90, 90};
static unsigned int glk_lowspeed_load[CLUSTER_TYPES] = {30, 30, 30};
static struct proc_dir_entry *game_load_dir, *game_load_parent;
static DEFINE_MUTEX(proc_lock);
static struct ctl_table_header *game_load_header;
static struct irq_work glk_cpufreq_irq_work;
extern int glk_disable, glk_freq_limit_walt, glk_fbreak_enable, glk_freq_limit_start;
extern unsigned int glk_maxfreq[CLUSTER_TYPES], glk_minfreq[CLUSTER_TYPES];
extern u64 get_migt_thresh(void);
extern int vip_task_schedboost, force_stask_to_big;

static u64 sat_add(u64 a, u64 b) { return a > U64_MAX - b ? U64_MAX : a + b; }
int glk_enable(void)
{
	u64 now = ktime_get_ns(), last = READ_ONCE(last_update_time);
	return smp_load_acquire(&game_load_has_init) && !READ_ONCE(glk_disable) &&
		migt_enable() > 2 && now >= last && now - last < NSEC_PER_SEC;
}
void game_load_update(struct task_struct *task, u64 delta, int cpu)
{
	unsigned long flags;
	unsigned int mask, i, game_flags;
	int render_uid;
	struct game_load *load;
	if (!smp_load_acquire(&game_load_has_init) || cpu < 0 || cpu >= nr_cpu_ids ||
	    !cpu_possible(cpu))
		return;
	/* render seqcount writer also takes pkg.lock: read UID before pkg.lock. */
	render_uid = get_cur_render_uid();
	read_lock_irqsave(&task->pkg.lock, flags);
	game_flags = task->pkg.migt.flag;
	mask = from_kuid(&init_user_ns, task_uid(task)) == render_uid ?
		(1 << FREQ_GAME_LOAD) : 0;
	read_unlock_irqrestore(&task->pkg.lock, flags);
	mask |= 1 << FREQ_CPU_LOAD;
	if (game_flags & MASK_VTASK)
		mask |= 1 << FREQ_VIP_LOAD;
	if ((READ_ONCE(vip_task_schedboost) || READ_ONCE(force_stask_to_big)) &&
	    (game_flags & MASK_STASK))
		mask |= 1 << FREQ_STASK_LOAD;
	if (game_flags & MASK_RTASK)
		mask |= 1 << FREQ_RENDER_LOAD;
	load = &per_cpu(load_info, cpu);
	raw_spin_lock_irqsave(&load->lock, flags);
	for (i = 0; i < FREQ_LOAD_ITEMS; i++)
		if (mask & (1 << i))
			load->runtime[i] = sat_add(load->runtime[i], delta);
	raw_spin_unlock_irqrestore(&load->lock, flags);
}
static unsigned int target_period(unsigned int thresh)
{
	if (thresh >= 11 && thresh <= 16)
		return MSEC_PER_SEC / 90;
	if (thresh >= 17 && thresh <= 24)
		return MSEC_PER_SEC / 60;
	if (thresh >= 25 && thresh <= 33)
		return MSEC_PER_SEC / 40;
	return MSEC_PER_SEC / 30;
}
static u64 roll_cluster(int cluster, unsigned int policy)
{
	u64 total[FREQ_LOAD_ITEMS] = {};
	unsigned long flags;
	int cpu, i;
	for_each_cpu(cpu, &cluster_cpumask[cluster]) {
		struct game_load *load = &per_cpu(load_info, cpu);
		raw_spin_lock_irqsave(&load->lock, flags);
		for (i = 0; i < FREQ_LOAD_ITEMS; i++) {
			load->previous[i] = load->runtime[i];
			load->runtime[i] = 0;
			total[i] = i == FREQ_CPU_LOAD ? max(total[i], load->previous[i]) :
				sat_add(total[i], load->previous[i]);
		}
		raw_spin_unlock_irqrestore(&load->lock, flags);
	}
	return total[policy < FREQ_LOAD_ITEMS ? policy : FREQ_STASK_LOAD];
}
static void glk_irq_work(struct irq_work *work)
{
	int cpu;
	unsigned long flags;
	/* history/load locks released before any RQ operation. Existing WALT owns
	 * cluster aggregation; update one RQ at a time through its typed API. */
	for_each_online_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);
		raw_spin_lock_irqsave(&rq->lock, flags);
		if (cpu_online(cpu)) {
			walt_update_task_ravg(rq->curr, rq, TASK_UPDATE, sched_ktime_clock(), 0);
			cpufreq_update_util(rq, SCHED_CPUFREQ_GLK);
		}
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}
}
void game_load_history_update(u64 tick)
{
	unsigned long flags;
	unsigned int cid, policy = READ_ONCE(sysctl_game_load_choose_policy);
	struct game_load_items *frame;
	u64 now;
	if (!smp_load_acquire(&game_load_has_init))
		return;
	raw_spin_lock_irqsave(&history_lock, flags);
	now = ktime_get_ns();
	last_update_time = now;
	frame = &history[cur_history_items];
	if (!frame->frame_start) {
		frame->frame_start = now;
		goto unlock;
	}
	frame->frame_period = now >= frame->frame_start ? now - frame->frame_start : 0;
	/* Stock GLK normalizes ns runtime by target period(ms) * 1024. */
	cpu_util_freq_divisor = (u64)target_period(get_migt_thresh()) << 10;
	for (cid = 0; cid < cluster_number; cid++)
		frame->cpu_util[cid] = div64_u64(roll_cluster(cid, policy), cpu_util_freq_divisor);
	cur_history_items = (cur_history_items + 1) % FRAME_LOAD_HISITMES;
	memset(&history[cur_history_items], 0, sizeof(*frame));
	history[cur_history_items].frame_start = now;
unlock:
	raw_spin_unlock_irqrestore(&history_lock, flags);
	irq_work_queue(&glk_cpufreq_irq_work);
}
void game_load_reset(void)
{
	unsigned long flags, cpu_flags;
	int cpu;
	if (!smp_load_acquire(&game_load_has_init))
		return;
	raw_spin_lock_irqsave(&history_lock, flags);
	memset(history, 0, sizeof(history));
	cur_history_items = 0;
	last_update_time = 0;
	cpu_util_freq_divisor = 0;
	for_each_possible_cpu(cpu) {
		struct game_load *load = &per_cpu(load_info, cpu);
		raw_spin_lock_irqsave(&load->lock, cpu_flags);
		memset(load->runtime, 0, sizeof(load->runtime));
		memset(load->previous, 0, sizeof(load->previous));
		raw_spin_unlock_irqrestore(&load->lock, cpu_flags);
	}
	raw_spin_unlock_irqrestore(&history_lock, flags);
}
void glk_force_maxfreq_break(bool val) { WRITE_ONCE(force_maxfreq_break, val); }
void glk_maxfreq_break(bool val)
{
	if (READ_ONCE(glk_fbreak_enable))
		WRITE_ONCE(glk_maxfreq_limit, !val);
}
void glk_minfreq_break(bool val)
{
	if (READ_ONCE(glk_fbreak_enable))
		WRITE_ONCE(glk_minfreq_limit, !val);
}
static void soft_limits(struct cpufreq_policy *policy, int cid,
		unsigned int *minfreq, unsigned int *maxfreq)
{
	unsigned int min = READ_ONCE(glk_minfreq[cid]), max = READ_ONCE(glk_maxfreq[cid]);
	*minfreq = READ_ONCE(glk_minfreq_limit) ? clamp(min, policy->min, policy->max) : policy->min;
	*maxfreq = READ_ONCE(glk_maxfreq_limit) && !READ_ONCE(force_maxfreq_break) && max ?
		clamp(max, policy->min, policy->max) : policy->max;
	if (*maxfreq < *minfreq)
		*maxfreq = *minfreq;
}
unsigned int glk_freq_limit(struct cpufreq_policy *policy, unsigned int *freq)
{
	unsigned int minfreq, maxfreq;
	int cid;
	if (!glk_enable() || !READ_ONCE(glk_freq_limit_walt) ||
	    !READ_ONCE(glk_freq_limit_start) || policy->cpu >= nr_cpu_ids)
		return *freq;
	cid = per_cpu(load_info, policy->cpu).cluster;
	if (cid < 0 || cid >= cluster_number)
		return *freq;
	soft_limits(policy, cid, &minfreq, &maxfreq);
	*freq = clamp(*freq, minfreq, maxfreq);
	return *freq;
}
unsigned long glk_cal_freq(struct cpufreq_policy *policy,
		unsigned long util, unsigned long max)
{
	unsigned long flags;
	unsigned int prev, minfreq, maxfreq;
	u64 game_load, period, glk_load, target;
	int cid;
	if (!glk_enable() || policy->cpu >= nr_cpu_ids)
		return 0;
	cid = per_cpu(load_info, policy->cpu).cluster;
	if (cid < 0 || cid >= cluster_number || !capacity_orig_of(policy->cpu))
		return 0;
	raw_spin_lock_irqsave(&history_lock, flags);
	prev = (cur_history_items + FRAME_LOAD_HISITMES - 1) % FRAME_LOAD_HISITMES;
	period = history[prev].frame_period;
	game_load = history[prev].cpu_util[cid];
	raw_spin_unlock_irqrestore(&history_lock, flags);
	if (!period)
		return 0;
	soft_limits(policy, cid, &minfreq, &maxfreq);
	glk_load = div64_u64(min_t(u64, game_load, U32_MAX) * 100, capacity_orig_of(policy->cpu));
	if (period > get_migt_thresh() * 2 * NSEC_PER_MSEC)
		return READ_ONCE(perf_mod) && policy->cur >= maxfreq ? policy->max : maxfreq;
	if (glk_load > READ_ONCE(glk_highspeed_load[cid]))
		return maxfreq;
	if (glk_load < READ_ONCE(glk_lowspeed_load[cid]))
		return minfreq;
	target = div64_u64(glk_load * ((u64)maxfreq + (maxfreq >> 3)), 100);
	return clamp_t(u64, target, minfreq, maxfreq);
}
/* Stock's weak no-op is replaced by a real recalculation/kick using target_ns.
 * No new symbol export: only the built-in MIGT worker calls this. */
void update_freq_limit(u64 target_ns)
{
	unsigned long flags;
	unsigned int prev, cid, policy = READ_ONCE(sysctl_game_load_choose_policy);
	u64 divisor = max_t(u64, 1, div64_u64(target_ns, NSEC_PER_MSEC)) << 10;
	int cpu;
	if (!glk_enable() || !target_ns || target_ns > 60000ULL * NSEC_PER_MSEC)
		return;
	raw_spin_lock_irqsave(&history_lock, flags);
	prev = (cur_history_items + FRAME_LOAD_HISITMES - 1) % FRAME_LOAD_HISITMES;
	for (cid = 0; cid < cluster_number; cid++) {
		u64 total = 0;
		for_each_cpu(cpu, &cluster_cpumask[cid]) {
			struct game_load *load = &per_cpu(load_info, cpu);
			unsigned long cpu_flags;
			u64 value;
			raw_spin_lock_irqsave(&load->lock, cpu_flags);
			value = load->previous[policy < FREQ_LOAD_ITEMS ? policy : FREQ_STASK_LOAD];
			raw_spin_unlock_irqrestore(&load->lock, cpu_flags);
			total = policy == FREQ_CPU_LOAD ? max(total, value) : sat_add(total, value);
		}
		history[prev].cpu_util[cid] = div64_u64(total, divisor);
	}
	raw_spin_unlock_irqrestore(&history_lock, flags);
	irq_work_queue(&glk_cpufreq_irq_work);
}

static int history_load_show(struct seq_file *m, void *v)
{
	struct game_load_items frame;
	unsigned long flags;
	unsigned int i, j;
	for (i = 0; i < FRAME_LOAD_HISITMES; i++) {
		raw_spin_lock_irqsave(&history_lock, flags);
		frame = history[i];
		raw_spin_unlock_irqrestore(&history_lock, flags);
		if (!frame.frame_period)
			continue;
		seq_printf(m, "period %llu:\n", frame.frame_period);
		for (j = 0; j < cluster_number; j++)
			seq_printf(m, "%c: %llu\n", "lmb"[j], frame.cpu_util[j]);
	}
	return 0;
}
static int cpu_load_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	u64 values[FREQ_LOAD_ITEMS], divisor = READ_ONCE(cpu_util_freq_divisor);
	int cpu, i;
	for_each_possible_cpu(cpu) {
		struct game_load *load = &per_cpu(load_info, cpu);
		raw_spin_lock_irqsave(&load->lock, flags);
		memcpy(values, load->previous, sizeof(values));
		raw_spin_unlock_irqrestore(&load->lock, flags);
		seq_printf(m, "cpu%d:", cpu);
		for (i = 0; i < FREQ_LOAD_ITEMS; i++)
			seq_printf(m, " %llu", divisor ? div64_u64(values[i], divisor) : 0);
		seq_putc(m, '\n');
	}
	return 0;
}
static int history_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, history_load_show, NULL);
}
static int cpu_load_open(struct inode *inode, struct file *file)
{
	return single_open(file, cpu_load_show, NULL);
}
static const struct file_operations history_load_fops = {
	.open = history_load_open, .read = seq_read, .llseek = seq_lseek, .release = single_release,
};
static const struct file_operations cpu_load_fops = {
	.open = cpu_load_open, .read = seq_read, .llseek = seq_lseek, .release = single_release,
};
/* Called synchronously by package proc setup; never retain a pending parent. */
static int create_game_load_proc_locked(void *rootdir)
{
	if (!rootdir)
		return -EINVAL;
	if (!smp_load_acquire(&game_load_has_init))
		return -EAGAIN;
	if (game_load_dir)
		return game_load_parent == rootdir ? 0 : -EBUSY;
	game_load_dir = proc_mkdir("glk", rootdir);
	if (!game_load_dir)
		return -ENOMEM;
	if (!proc_create("game_history_load", 0444, game_load_dir, &history_load_fops) ||
	    !proc_create("game_cpu_load", 0444, game_load_dir, &cpu_load_fops)) {
		proc_remove(game_load_dir);
		game_load_dir = NULL;
		game_load_parent = NULL;
		return -ENOMEM;
	}
	game_load_parent = rootdir;
	return 0;
}
int create_game_load_proc(void *rootdir)
{
	int ret;
	mutex_lock(&proc_lock);
	ret = create_game_load_proc_locked(rootdir);
	mutex_unlock(&proc_lock);
	return ret;
}
void delete_game_load_proc(void)
{
	mutex_lock(&proc_lock);
	if (game_load_dir)
		proc_remove(game_load_dir);
	game_load_dir = NULL;
	game_load_parent = NULL;
	mutex_unlock(&proc_lock);
}
static int zero, one = 1, hundred = 100, last_policy = FREQ_LOAD_ITEMS - 1;
#define ctl(name, value, low, high) { .procname = name, .data = &value, \
	.maxlen = sizeof(value), .mode = 0644, .proc_handler = proc_dointvec_minmax, \
	.extra1 = &low, .extra2 = &high }
static int signed_max = INT_MAX;
static struct ctl_table game_load_table[] = {
	ctl("game_load_choose_policy", sysctl_game_load_choose_policy, zero, last_policy),
	ctl("glk_disable", glk_disable, zero, one),
	ctl("glk_freq_limit_walt", glk_freq_limit_walt, zero, one),
	ctl("game_hispeed_load", glk_highspeed_load, zero, hundred),
	ctl("game_lowspeed_load", glk_lowspeed_load, zero, hundred),
	ctl("game_minfreq_limit", glk_minfreq, zero, signed_max),
	ctl("game_maxfreq_limit", glk_maxfreq, zero, signed_max),
	ctl("freq_break_enable", glk_fbreak_enable, zero, one),
	ctl("perf_mod", perf_mod, zero, one),
	ctl("load_debug", game_load_debug, zero, one),
	{}
};
static struct ctl_table game_ctl_root[] = {
	{ .procname = "glk", .mode = 0555, .child = game_load_table }, {}
};
static int __init game_load_init(void)
{
	struct cpumask remaining;
	int cpu, member;
	cpumask_copy(&remaining, cpu_possible_mask);
	for_each_possible_cpu(cpu) {
		raw_spin_lock_init(&per_cpu(load_info, cpu).lock);
		per_cpu(load_info, cpu).cluster = -1;
	}
	while (!cpumask_empty(&remaining)) {
		if (cluster_number == CLUSTER_TYPES)
			return -E2BIG;
		cpu = cpumask_first(&remaining);
		cpumask_and(&cluster_cpumask[cluster_number], cpu_coregroup_mask(cpu), &remaining);
		if (cpumask_empty(&cluster_cpumask[cluster_number]))
			return -EINVAL;
		for_each_cpu(member, &cluster_cpumask[cluster_number])
			per_cpu(load_info, member).cluster = cluster_number;
		cpumask_andnot(&remaining, &remaining, &cluster_cpumask[cluster_number]);
		cluster_number++;
	}
	init_irq_work(&glk_cpufreq_irq_work, glk_irq_work);
	game_load_header = register_sysctl_table(game_ctl_root);
	if (!game_load_header)
		return -ENOMEM;
	smp_store_release(&game_load_has_init, true);
	return 0;
}
device_initcall(game_load_init);
