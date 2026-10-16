// SPDX-License-Identifier: GPL-2.0-only
/* Dream ABI port of projects-nexus/nexus_kernel_xiaomi_sm8250@f12f206b5339ad1294642e89195fe08918fb2459; retain recovered pkg_stat.h. */
#define pr_fmt(fmt) "migt-sched: " fmt
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/sched/signal.h>
#include <linux/kthread.h>
#include <linux/sort.h>
#include <linux/cred.h>
#include <linux/pkg_stat.h>
#include <linux/jiffies.h>
#include "pkg.h"

#define RENDER_AGING_TIME (3 * HZ)
#define USER_MAXTHREAD (1 << 10)
#define MIGT_THREAD_NAME "migt_thread"
#define BOOST_MAXTIME_LIMIT (60 * HZ)
#define DEFAULT_BOOST_MINTIME 30
#define for_each_user_process(p, user) \
	list_for_each_entry(p, &user->pkg.list, pkg.list)
#define min_value(x, y)  (x < y ? x : y)

#define mi_time_after(a,b)	\
	(typecheck(unsigned long, a) && \
	 typecheck(unsigned long, b) && \
	 ((long)((b) - (a)) < 0))
#define mi_time_before(a,b) mi_time_after(b,a)

int enable_pkg_monitor = 1;
int set_render_as_stask;
int force_stask_to_big;
int stask_candidate_num = 1;
int vip_task_max_num = 15;
int ip_task_max_num = 5;
int vip_task_schedboost = 0;
int need_reset_runtime_everytime;
int force_reset_runtime;
unsigned int minor_window_app;
extern void trigger_ceiling_limit(void);
int vip_task_max_cpu_nr = 7;

static int boost_task_pid;
static int migt_debug;
static struct ctl_table_header *migt_sched_header;
static DECLARE_WAIT_QUEUE_HEAD(migt_wait_queue);
static atomic_t migt_has_work_todo = ATOMIC_INIT(0);
static atomic_t migt_init_sucess = ATOMIC_INIT(0);
static atomic_t mi_dynamic_vip_num = ATOMIC_INIT(0);
static DEFINE_SPINLOCK(migt_work_lock);
static DEFINE_SPINLOCK(migt_mask_lock);
struct task_struct *migt_thread;
//static cpumask_t minor_app_use_cpumask;
static cpumask_t viptask_use_cpumask;

enum STASK_POLICY
{
	CHOOSE_WAKERS,
	CHOOSE_HERAY,
} pick_stask_policy = CHOOSE_HERAY;

static struct rend_info_s {
	seqcount_t seq ____cacheline_aligned_in_smp;
	raw_spinlock_t lock;
	bool active;
	int pid;
	unsigned long last_update_jiffies;
	char comm[TASK_COMM_LEN];
} render_info[RENDER_TYPES];

static struct task_runtime {
	struct task_struct *tsk;
	u64 run_times;
	u64 fps_max;
} user_array[USER_MAXTHREAD];
static struct rend_info_s  rinfo_back[RENDER_TYPES];

static struct migt_work {
	int uid;
} g_work;

void add_work_to_migt(int data)
{
	unsigned long flags;

	if (data <= 0 || !atomic_read(&migt_init_sucess))
		return;
	/* Coalesce pending requests to the latest UID; never overwrite active work. */
	spin_lock_irqsave(&migt_work_lock, flags);
	g_work.uid = data;
	atomic_set(&migt_has_work_todo, 1);
	spin_unlock_irqrestore(&migt_work_lock, flags);
	wake_up_interruptible(&migt_wait_queue);
}

void migt_monitor_hook(int enqueue, int cpu,
		struct task_struct *p, u64 walltime)
{
	u64 exec_delta;
	unsigned long flags;

	if (!READ_ONCE(enable_pkg_monitor))
		return;
	write_lock_irqsave(&p->pkg.lock, flags);
	if (enqueue) {
		p->pkg.migt.prev_sum = p->se.sum_exec_runtime;
	} else {
		exec_delta = p->se.sum_exec_runtime >= p->pkg.migt.prev_sum ?
			p->se.sum_exec_runtime - p->pkg.migt.prev_sum : 0;
		if (exec_delta > p->pkg.migt.max_exec)
			p->pkg.migt.max_exec = min_t(u64, exec_delta, U32_MAX);
	}
	write_unlock_irqrestore(&p->pkg.lock, flags);
}

static inline void set_render_flag(struct task_struct *tsk,
		enum RENDER_TYPE type)
{
	unsigned long flags;

	write_lock_irqsave(&tsk->pkg.lock, flags);
	tsk->pkg.migt.flag |= 1 << (GAME_QRENDER_TASK + type);
	write_unlock_irqrestore(&tsk->pkg.lock, flags);
}

static inline void clean_render_flag(struct task_struct *tsk,
		enum RENDER_TYPE type)
{
	unsigned long flags;

	write_lock_irqsave(&tsk->pkg.lock, flags);
	tsk->pkg.migt.flag &= ~(1 << (GAME_QRENDER_TASK + type));
	write_unlock_irqrestore(&tsk->pkg.lock, flags);
}

static void get_render_info(struct rend_info_s *rinfo)
{
	int type;
	unsigned int seq;

	for (type = 0; type < RENDER_TYPES; type++) {
		rinfo[type].active = 0;
		do {
			seq = read_seqcount_begin(&render_info[type].seq);

			memcpy(rinfo[type].comm, render_info[type].comm,
				TASK_COMM_LEN);
			rinfo[type].active = render_info[type].active;

		} while (read_seqcount_retry(&render_info[type].seq, seq));
	}
}

static
bool is_render_sibling(struct task_struct *tsk, struct rend_info_s *rinfo)
{
	int type, len;

	for (type = 0; type < RENDER_TYPES; type++) {
		if (!rinfo[type].active)
			continue;

		len = strlen(rinfo[type].comm);
		if (!strncmp(rinfo[type].comm, tsk->comm, len))
			return true;
	}

	return false;
}

static int pick_stask(struct task_runtime *array, int num)
{
	unsigned long long max_exec = 0;
	int i, max = 0;

	for (i = 0; i < num; i++) {
		if (array[i].fps_max > max_exec) {
			max_exec = array[i].fps_max;
			max = i;
		}
	}

	return max;
}

static void set_viptask_flag(struct task_struct *p, int pri, int super_id)
{
	int stask = clamp_t(int, READ_ONCE(stask_candidate_num), 0, USER_MAXTHREAD);
	int vip = clamp_t(int, READ_ONCE(vip_task_max_num), 0, USER_MAXTHREAD);
	int ip = clamp_t(int, READ_ONCE(ip_task_max_num), 0, USER_MAXTHREAD);

	/* Caller holds task->pkg.lock; bounded totals avoid signed overflow. */
	if (pri == -1) {
		if (!set_render_as_stask)
			p->pkg.migt.flag |= 1 << GAME_VIP_TASK;
		else
			p->pkg.migt.flag |= 1 << GAME_SUPER_TASK;
		return;
	} else if (pri == super_id) {
		p->pkg.migt.flag |= 1 << GAME_SUPER_TASK;
		if (migt_debug)
			printk("set %s pri %d as super\n", p->comm, pri);
		return;
	} else if (pri < stask + vip) {
		if (migt_debug)
			printk("set %s pri %d as vip\n", p->comm, pri);
		p->pkg.migt.flag |= 1 << GAME_VIP_TASK;
		return;
	} else if (pri < stask + vip + ip)
		p->pkg.migt.flag |= 1 << GAME_IP_TASK;
}

static void clear_viptask_flag(struct task_struct *p)
{
	if (is_render_thread(p))
		return;

	p->pkg.migt.flag &= MASK_CLE_GTASK;
}

static int cmp(const void *a, const void *b)
{
	struct task_runtime *p1 = (struct task_runtime *)a;
	struct task_runtime *p2 = (struct task_runtime *)b;

	if (p1->run_times > p2->run_times)
		return -1;

	if (p1->run_times < p2->run_times)
		return 1;

	return 0;
}

static bool migt_matches_user(struct task_struct *task, struct user_struct *user)
{
	/* Caller holds user->pkg.lock: detach must precede UID publication. */
	return !is_idle_task(task) && !list_empty(&task->pkg.list) &&
		uid_eq(task_uid(task), user->uid);
}

static void mark_viptask(int uid)
{
	struct user_struct *user;
	struct task_struct *task;
	unsigned long flags;
	bool candidate, reset;
	int nr = 0, i = 0, j, bucket, super_id = -2, stask_count;

	user = find_user(KUIDT_INIT(uid));
	if (!user)
		return;
	get_render_info(rinfo_back);
	reset = READ_ONCE(need_reset_runtime_everytime) ||
		xchg(&force_reset_runtime, 0);
	/* List lock protects lifetime acquisition only; no rq/affinity/log work. */
	read_lock_irqsave(&user->pkg.lock, flags);
	for_each_user_process(task, user) {
		if (nr == USER_MAXTHREAD)
			break;
		if (is_idle_task(task))
			continue;
		get_task_struct(task);
		user_array[nr++].tsk = task;
	}
	read_unlock_irqrestore(&user->pkg.lock, flags);

	for (j = 0; j < nr; j++) {
		task = user_array[j].tsk;
		candidate = false;
		/* Recheck and mutate with UID migration excluded, then lock fields. */
		read_lock_irqsave(&user->pkg.lock, flags);
		if (migt_matches_user(task, user)) {
			write_lock(&task->pkg.lock);
			if (reset) {
				for (bucket = 0; bucket < NUM_MIGT_BUCKETS; bucket++)
					task->pkg.migt.bucket[bucket] = 0;
				task->pkg.migt.fps_mexec = 0;
			}
			if (is_render_thread(task) || is_render_sibling(task, rinfo_back)) {
				set_viptask_flag(task, -1, 0);
			} else if (!task->pkg.migt.wake_render) {
				clear_viptask_flag(task);
			} else {
				user_array[i].tsk = task;
				user_array[i].run_times = task->pkg.migt.wake_render;
				user_array[i].fps_max = task->pkg.migt.fps_mexec;
				i++;
				candidate = true;
			}
			write_unlock(&task->pkg.lock);
		}
		read_unlock_irqrestore(&user->pkg.lock, flags);
		if (!candidate)
			put_task_struct(task);
	}
	/* Candidate refs remain owned by this array; sorting uses owned snapshots. */
	sort(user_array, i, sizeof(struct task_runtime), cmp, NULL);
	stask_count = clamp_t(int, READ_ONCE(stask_candidate_num), 0, i);
	if (pick_stask_policy == CHOOSE_HERAY && stask_count)
		super_id = pick_stask(user_array, stask_count);
	for (j = 0; j < i; j++) {
		task = user_array[j].tsk;
		read_lock_irqsave(&user->pkg.lock, flags);
		if (migt_matches_user(task, user)) {
			write_lock(&task->pkg.lock);
			clear_viptask_flag(task);
			set_viptask_flag(task, j, super_id);
			write_unlock(&task->pkg.lock);
		}
		read_unlock_irqrestore(&user->pkg.lock, flags);
		put_task_struct(task);
		user_array[j].tsk = NULL;
	}
	free_uid(user);
}

void migt_monitor_init(struct task_struct *p)
{
	int i;

	p->pkg.migt.migt_count  = 0;
	p->pkg.migt.prev_sum    = 0;
	p->pkg.migt.max_exec    = 0;
	p->pkg.migt.fps_exec    = 0;
	p->pkg.migt.fps_mexec   = 0;
	p->pkg.migt.flag        = MIGT_NORMAL_TASK;
	p->pkg.migt.run_times   = 0;
	p->pkg.migt.wake_render = 0;
	p->pkg.migt.boost_end = 0;
	cpumask_copy(&p->pkg.migt.cpus_allowed, cpu_possible_mask);
	for (i = 0; i < NUM_MIGT_BUCKETS; i++) {
		p->pkg.migt.bucket[i] = 0;
#ifdef VTASK_BOOST_DEBUG
		p->pkg.migt.boostat[i] = 0;
#endif
	}
}

static int migt_hardwork(void *data)
{
	unsigned long flags;
	int uid, ret;

	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(migt_wait_queue,
			atomic_read(&migt_has_work_todo) || kthread_should_stop());
		if (kthread_should_stop())
			break;
		if (ret)
			continue;
		spin_lock_irqsave(&migt_work_lock, flags);
		uid = g_work.uid;
		g_work.uid = 0;
		atomic_set(&migt_has_work_todo, 0);
		spin_unlock_irqrestore(&migt_work_lock, flags);
		/* Drop pending lock before sleeping; UID switch holds this mutex too. */
		migt_policy_lock();
		if (!kthread_should_stop() && uid > 0 && uid == get_cur_render_uid()) {
			trigger_ceiling_limit();
			mark_viptask(uid);
		}
		migt_policy_unlock();
	}
	return 0;
}

static void inline set_mi_vip_task(struct task_struct *p, unsigned int jiff)
{
	unsigned long flags;

	if (!p)
		return;
	write_lock_irqsave(&p->pkg.lock, flags);
	p->pkg.migt.boost_end = jiffies + jiff;
	if (!(p->pkg.migt.flag & MASK_MI_VTASK))
		atomic_inc(&mi_dynamic_vip_num);
	p->pkg.migt.flag |= MASK_MI_VTASK;
	write_unlock_irqrestore(&p->pkg.lock, flags);
}

static void inline clean_mi_vip_task(struct task_struct *p)
{
	unsigned long flags;

	write_lock_irqsave(&p->pkg.lock, flags);
	if ((p->pkg.migt.flag & MASK_MI_VTASK) &&
	    time_after_eq(jiffies, p->pkg.migt.boost_end)) {
		atomic_dec(&mi_dynamic_vip_num);
		p->pkg.migt.flag &= ~MASK_MI_VTASK;
	}
	write_unlock_irqrestore(&p->pkg.lock, flags);
}

static int proc_boost_mi_task(struct ctl_table *table, int write,
                void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec(table, write,
			buffer, lenp, ppos);
	struct task_struct *target;

	if (ret || !write)
		return ret;
	rcu_read_lock();
	target = find_task_by_vpid(boost_task_pid);
	if (unlikely(!target)) {
		rcu_read_unlock();
		pr_err("Invalid input %d, no such process\n", boost_task_pid);
		return -ESRCH;
	}

	get_task_struct(target);
	pr_info("%d, %s set as mi vip task from %u", boost_task_pid,
			target->comm, jiffies);
	rcu_read_unlock();
	set_mi_vip_task(target, HZ);
	put_task_struct(target);
	boost_task_pid = -1;
	return ret;
}

void mi_vip_task_req(int *pid, unsigned int nr, unsigned int jiff)
{
	int i, task_pid;
	struct task_struct *target;

#define MAX_MI_VIP_REQ 5
	if (nr > MAX_MI_VIP_REQ) {
		pr_err("req too many vip tasks\n");
		return;
	}

	if (!pid)
		return;

	jiff = min_value(BOOST_MAXTIME_LIMIT, jiff);
	if (unlikely(!jiff))
		jiff = DEFAULT_BOOST_MINTIME;

	for (i = 0; i < nr; i++) {
		rcu_read_lock();
		task_pid = pid[i];
		target = find_task_by_vpid(task_pid);

		if (unlikely(!target)) {
			rcu_read_unlock();
			pr_err("Invalid input %d, no such process\n", task_pid);
			continue;
		}

		get_task_struct(target);
		pr_info("%d, %s set as mi vip task from %u to %u", task_pid,
				target->comm, jiffies, jiffies + jiff);
		rcu_read_unlock();
		set_mi_vip_task(target, jiff);
		put_task_struct(target);
	}
}

int get_mi_dynamic_vip_num(void)
{
	return atomic_read(&mi_dynamic_vip_num);
}

static void update_viptask_mask(int count)
{
	cpumask_t mask;
	unsigned long flags;
	int cpu;

	cpumask_clear(&mask);
	for_each_possible_cpu(cpu)
		if (cpu < count)
			cpumask_set_cpu(cpu, &mask);
	spin_lock_irqsave(&migt_mask_lock, flags);
	cpumask_copy(&viptask_use_cpumask, &mask);
	spin_unlock_irqrestore(&migt_mask_lock, flags);
}

int proc_viptask_cpus_set(struct ctl_table *table, int write,
		void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int count = READ_ONCE(vip_task_max_cpu_nr), ret;
	struct ctl_table proposed = *table;

	proposed.data = &count;
	ret = proc_dointvec(&proposed, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;
	if (count < 1 || count > nr_cpu_ids)
		return -EINVAL;
	WRITE_ONCE(vip_task_max_cpu_nr, count);
	update_viptask_mask(count);
	return 0;
}

static struct ctl_table migt_table[] = {
	{
		.procname       = "boost_pid",
		.data           = &boost_task_pid,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_boost_mi_task,
	},
	{
		.procname       = "viptask_maxcpu_nr",
		.data           = &vip_task_max_cpu_nr,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_viptask_cpus_set,
	},
	{
		.procname	= "enable_pkg_monitor",
		.data		= &enable_pkg_monitor,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname       = "migt_sched_debug",
		.data           = &migt_debug,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec,
	},
	{
		.procname	= "render_stask",
		.data		= &set_render_as_stask,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "force_stask_tob",
		.data		= &force_stask_to_big,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "stask_candidate_num",
		.data		= &stask_candidate_num,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "vip_task_max_num",
		.data		= &vip_task_max_num,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{
		.procname	= "ip_task_max_num",
		.data		= &ip_task_max_num,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{}
};

static struct ctl_table migt_ctl_root[] = {
	{
		.procname	= "migt",
		.mode		= 0555,
		.child	= migt_table,
	},
	{}
};

static int __init migt_sched_init(void)
{
	int type, ret;

	/* Initialize everything before publishing sysctl or accepting work. */
	for (type = 0; type < RENDER_TYPES; type++) {
		seqcount_init(&render_info[type].seq);
		raw_spin_lock_init(&render_info[type].lock);
		reset_render_info(type);
	}
	update_viptask_mask(clamp_t(int, vip_task_max_cpu_nr, 1, nr_cpu_ids));
	migt_sched_header = register_sysctl_table(migt_ctl_root);
	if (!migt_sched_header)
		return -ENOMEM;
	migt_thread = kthread_run(migt_hardwork, NULL, MIGT_THREAD_NAME);
	if (IS_ERR(migt_thread)) {
		ret = PTR_ERR(migt_thread);
		unregister_sysctl_table(migt_sched_header);
		migt_sched_header = NULL;
		return ret;
	}
	atomic_set(&migt_init_sucess, 1);
	return 0;
}

static void __exit migt_sched_exit(void)
{
	atomic_set(&migt_init_sucess, 0);
	if (!IS_ERR_OR_NULL(migt_thread))
		kthread_stop(migt_thread);
	if (migt_sched_header)
		unregister_sysctl_table(migt_sched_header);
	migt_sched_header = NULL;
}

int game_task(struct task_struct *p)
{
	if (!migt_enable())
		return 0;

	return from_kuid(&init_user_ns, task_uid(p)) == get_cur_render_uid();
}

int game_ip_task(struct task_struct *p)
{
	if (!migt_enable())
		return 0;

	return (p->pkg.migt.flag & MASK_ITASK);
}

int __game_vip_task(struct task_struct *p)
{
	if (!migt_enable())
		return 0;

	return (p->pkg.migt.flag & MASK_VTASK);
}

int game_super_task(struct task_struct *p)
{
        if (!migt_enable())
                return 0;

	if (vip_task_schedboost ||
		force_stask_to_big)
		return (p->pkg.migt.flag & MASK_STASK);

	return 0;
}

int game_vip_task(struct task_struct *p)
{
	unsigned long boost_end = p->pkg.migt.boost_end;

	if (!migt_enable()) {
		if (mi_time_before(jiffies, boost_end)) {
			if (migt_debug)
				pr_info("%d %s is mi vip task %d\n",
					p->pid, p->comm,
					p->pkg.migt.flag & MASK_MI_VTASK);

			return p->pkg.migt.flag & MASK_MI_VTASK;
		}

		else if (p->pkg.migt.flag & MASK_MI_VTASK)
			clean_mi_vip_task(p);
	}

	return __game_vip_task(p);
}

static int is_minor_window_app(int uid)
{
	 if (!minor_window_app)
		 return 0;

	 return uid == minor_window_app;
}

int minor_window_task(struct task_struct *p)
{
	int uid = from_kuid(&init_user_ns, task_uid(p));
	return is_minor_window_app(uid);
}

bool get_minor_window_cpumask(struct task_struct *p, cpumask_t *mask)
{
	unsigned long flags;

	if (!mask)
		return false;
	/* Stock helper selects possible CPUs for SUPER, configured mask otherwise. */
	if (READ_ONCE(p->pkg.migt.flag) & MASK_STASK) {
		cpumask_copy(mask, cpu_possible_mask);
	} else {
		spin_lock_irqsave(&migt_mask_lock, flags);
		cpumask_copy(mask, &viptask_use_cpumask);
		spin_unlock_irqrestore(&migt_mask_lock, flags);
	}
	return true;
}

void reset_render_info(enum RENDER_TYPE type)
{
	int pid;
	unsigned long flags;
	struct task_struct *tsk;

	if ((unsigned int)type >= RENDER_TYPES)
		return;
	raw_spin_lock_irqsave(&render_info[type].lock, flags);
	write_seqcount_begin(&render_info[type].seq);
	render_info[type].active = false;
	render_info[type].last_update_jiffies = 0;
	pid = render_info[type].pid;
	render_info[type].pid = -1;
	memset(render_info[type].comm, 0, TASK_COMM_LEN);
	write_seqcount_end(&render_info[type].seq);
	raw_spin_unlock_irqrestore(&render_info[type].lock, flags);

	if (pid > 0) {
		rcu_read_lock();
		tsk = find_task_by_vpid(pid);
		if (tsk)
			clean_render_flag(tsk, type);
		rcu_read_unlock();
	}
}

void update_render_info(struct task_struct *tsk, enum RENDER_TYPE type)
{
	int pid;
	unsigned long flags;
	struct task_struct *old_render;

	if ((unsigned int)type >= RENDER_TYPES)
		return;
	raw_spin_lock_irqsave(&render_info[type].lock, flags);
	write_seqcount_begin(&render_info[type].seq);
	if (likely(render_info[type].pid == tsk->pid)) {
		write_seqcount_end(&render_info[type].seq);
		raw_spin_unlock_irqrestore(&render_info[type].lock, flags);
		return;
	}

	render_info[type].active = true;
	set_render_flag(tsk, type);
	render_info[type].active  = 1;
	render_info[type].last_update_jiffies = jiffies;
	pid = render_info[type].pid;
	render_info[type].pid = tsk->pid;
	memset(render_info[type].comm, 0, TASK_COMM_LEN);
	strscpy(render_info[type].comm, tsk->comm, TASK_COMM_LEN);
	write_seqcount_end(&render_info[type].seq);
	raw_spin_unlock_irqrestore(&render_info[type].lock, flags);

	if (pid > 0) {
		rcu_read_lock();
		old_render = find_task_by_vpid(pid);
		if (old_render)
			clean_render_flag(old_render, type);
		rcu_read_unlock();
	}
}

int is_render_thread(struct task_struct *tsk)
{
	return READ_ONCE(tsk->pkg.migt.flag) & MASK_RTASK;
}

module_init(migt_sched_init);
module_exit(migt_sched_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vip-task detected by David");
