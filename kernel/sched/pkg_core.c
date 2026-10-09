// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dream's three-cluster package accounting, adapted from
 * projects-nexus/nexus_kernel_xiaomi_sm8250@f12f206b5339ad1294642e89195fe08918fb2459.
 * Stock callsites and constructors: analysis/stock-pkg-callsites.json and
 * stock-pkg-core.asm. Keep the recovered pkg_stat.h; no front/back ABI fields.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/cred.h>
#include <linux/sched/signal.h>
#include <linux/pkg_stat.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include "pkg.h"

DEFINE_MUTEX(pkg_runtime_mutex);

static int curr_runtime_items;
static int uid_max_value;
static int runtime_window_size = 300;
static bool runtime_enable;
static u64 sys_update_time;
static struct work_struct package_runtime_roll;
static enum cluster_type cpu_cluster_map[NR_CPUS];
static struct user_struct user_zero = { .__count = REFCOUNT_INIT(1) };
static struct runtime_info package_runtime_info[HISTORY_WINDOWS];
DECLARE_BITMAP(package_uid_bit, TRACE_UID_MAX);
int pause_mode;

void *get_curr_runtime_items(void) { return &curr_runtime_items; }
void *get_uid_max_value(void) { return &uid_max_value; }
void *get_runtime_window_size(void) { return &runtime_window_size; }
void *get_user_zero(void) { return &user_zero; }

static void map_cpu_cluster(void)
{
	unsigned int cpu, member;
	const struct cpumask *cluster;
	int type = LITTLE_CLUSTER;

	/* Topology is fixed for this SoC; do not remap concurrently on CPU0 ticks. */
	for_each_possible_cpu(cpu) {
		cluster = cpu_coregroup_mask(cpu);
		if (cpu != cpumask_first(cluster))
			continue;
		for_each_cpu(member, cluster)
			cpu_cluster_map[member] = min(type, BIG_CLUSTER);
		type++;
	}
}

enum cluster_type get_cluster_type(int cpu)
{
	return cpu_cluster_map[cpu];
}

void pkg_uid_register_locked(struct user_struct *user)
{
	unsigned int uid = __kuid_val(user->uid);

	/* The last bit represents overflow in the legacy proc interface. */
	if (uid >= TRACE_UID_MAX - 1)
		return;
	if (!test_and_set_bit(uid, package_uid_bit))
		get_uid(user);
	if (uid > uid_max_value)
		WRITE_ONCE(uid_max_value, uid);
}

bool pkg_uid_retire_locked(struct user_struct *user)
{
	unsigned int uid = __kuid_val(user->uid);

	/* Registry + this worker's find_user() reference, no live credentials. */
	if (uid >= TRACE_UID_MAX - 1 || user == &root_user ||
	    refcount_read(&user->__count) != 2)
		return false;
	return test_and_clear_bit(uid, package_uid_bit);
}

void pkg_task_attach(struct task_struct *task, struct user_struct *user)
{
	unsigned long flags;

	/* Credentials pin the user; release_task() unlinks before the last put. */
	write_lock_irqsave(&user->pkg.lock, flags);
	if (!WARN_ON_ONCE(!list_empty(&task->pkg.list)))
		list_add_tail(&task->pkg.list, &user->pkg.list);
	write_unlock_irqrestore(&user->pkg.lock, flags);
}

void pkg_task_clear_policy_locked(struct task_struct *task)
{
#ifdef CONFIG_MIGT
	int bucket;

	lockdep_assert_held(&task->pkg.lock);
	/* Retain explicit PID boost; invalidate policy derived from old UID. */
	task->pkg.migt.flag &= MASK_CLE_GTASK & ~MINOR_TASK;
	task->pkg.migt.migt_count = 0;
	task->pkg.migt.wake_render = 0;
	task->pkg.migt.fps_exec = 0;
	task->pkg.migt.fps_mexec = 0;
	for (bucket = 0; bucket < NUM_MIGT_BUCKETS; bucket++)
		task->pkg.migt.bucket[bucket] = 0;
#endif
}

bool pkg_task_detach(struct task_struct *task, struct user_struct *user)
{
	unsigned long flags, task_flags;
	bool attached;

	/* UID policy lock order: user -> task; never acquire rq while held. */
	write_lock_irqsave(&user->pkg.lock, flags);
	attached = !list_empty(&task->pkg.list);
	if (attached) {
#ifdef CONFIG_MIGT
		write_lock_irqsave(&task->pkg.lock, task_flags);
		pkg_task_clear_policy_locked(task);
		write_unlock_irqrestore(&task->pkg.lock, task_flags);
#endif
		list_del_init(&task->pkg.list);
	}
	write_unlock_irqrestore(&user->pkg.lock, flags);
	return attached;
}

static void snapshot_user(struct user_struct *user, int index)
{
	unsigned long flags;

	write_lock_irqsave(&user->pkg.lock, flags);
	user->pkg.sup_cluster_runtime[index] =
		user->pkg.sup_cluster_runtime[HISTORY_ITMES];
	user->pkg.big_cluster_runtime[index] =
		user->pkg.big_cluster_runtime[HISTORY_ITMES];
	user->pkg.little_cluster_runtime[index] =
		user->pkg.little_cluster_runtime[HISTORY_ITMES];
	write_unlock_irqrestore(&user->pkg.lock, flags);
}

static void package_runtime_roll_wk(struct work_struct *work)
{
	struct user_struct *user;
	int uid, index;
	u64 now;

	/* Proc selection/reset and rotation publish one coherent ring state.
	 * WALT only takes individual package IRQ locks, never this mutex.
	 */
	mutex_lock(&pkg_runtime_mutex);
	index = READ_ONCE(curr_runtime_items);
	now = get_jiffies_64();
	/* A timer may requeue this work while the previous rotation is running. */
	if (!smp_load_acquire(&runtime_enable) || READ_ONCE(pause_mode) ||
	    now - READ_ONCE(sys_update_time) <
	    (u64)READ_ONCE(runtime_window_size) * HZ)
		goto unlock;
	for_each_set_bit(uid, package_uid_bit, TRACE_UID_MAX - 1) {
		user = find_user(KUIDT_INIT(uid));
		if (!user)
			continue;
		snapshot_user(user, index);
		/* Drop the registry pin before the lookup pin; both remain accounted. */
		pkg_release_registered_user(user);
		free_uid(user);
	}
	snapshot_user(&user_zero, index);
	WRITE_ONCE(package_runtime_info[index].update_time,
		   READ_ONCE(sys_update_time));
	WRITE_ONCE(sys_update_time, get_jiffies_64());
	WRITE_ONCE(curr_runtime_items, (index + 1) % HISTORY_ITMES);
unlock:
	mutex_unlock(&pkg_runtime_mutex);
}

static struct user_struct *task_user(struct task_struct *task)
{
	struct user_struct *user;

	rcu_read_lock();
	user = task_cred_xxx(task, user);
	if (user)
		get_uid(user);
	rcu_read_unlock();
	return user;
}

static void add_runtime(struct package_runtime_info *package,
			enum cluster_type cluster, u64 delta, int index)
{
	switch (cluster) {
	case BIG_CLUSTER:
		package->sup_cluster_runtime[HISTORY_ITMES] += delta;
		break;
	case MID_CLUSTER:
		package->big_cluster_runtime[HISTORY_ITMES] += delta;
		break;
	case LITTLE_CLUSTER:
		package->little_cluster_runtime[HISTORY_ITMES] += delta;
		break;
	default:
		return;
	}
	if (index >= 0) {
		package->sup_cluster_runtime[index] =
			package->sup_cluster_runtime[HISTORY_ITMES];
		package->big_cluster_runtime[index] =
			package->big_cluster_runtime[HISTORY_ITMES];
		package->little_cluster_runtime[index] =
			package->little_cluster_runtime[HISTORY_ITMES];
	}
}

void update_pkg_load(struct task_struct *task, int cpu, int flag,
		     u64 wallclock, u64 delta)
{
	struct user_struct *user, *account;
	enum cluster_type cluster;
	unsigned long flags;
	int index;

	if (smp_load_acquire(&runtime_enable) && !READ_ONCE(pause_mode) &&
	    !is_idle_task(task)) {
		cluster = get_cluster_type(cpu);
		index = READ_ONCE(curr_runtime_items);
		write_lock_irqsave(&task->pkg.lock, flags);
		add_runtime(&task->pkg, cluster, delta, index);
		write_unlock_irqrestore(&task->pkg.lock, flags);
		user = task_user(task);
		account = user && __kuid_val(user->uid) < TRACE_UID_MAX - 1 ?
			user : &user_zero;
		write_lock_irqsave(&account->pkg.lock, flags);
		add_runtime(&account->pkg, cluster, delta, -1);
		write_unlock_irqrestore(&account->pkg.lock, flags);
		if (user)
			free_uid(user);
	}
	/* Stock invokes MIGT even when history collection is paused. */
	migt_hook(task, delta, cpu);
}
EXPORT_SYMBOL(update_pkg_load);

bool pkg_enable(void)
{
	return !READ_ONCE(pause_mode);
}
EXPORT_SYMBOL(pkg_enable);

void package_runtime_monitor(u64 now)
{
	if (!smp_load_acquire(&runtime_enable) || READ_ONCE(pause_mode) ||
	    now - READ_ONCE(sys_update_time) <
	    (u64)READ_ONCE(runtime_window_size) * HZ)
		return;
	queue_work_on(0, system_long_wq, &package_runtime_roll);
}
EXPORT_SYMBOL(package_runtime_monitor);

void init_task_runtime_info(struct task_struct *task)
{
	if (!task)
		return;
	memset(task->pkg.sup_cluster_runtime, 0,
	       sizeof(task->pkg.sup_cluster_runtime));
	memset(task->pkg.big_cluster_runtime, 0,
	       sizeof(task->pkg.big_cluster_runtime));
	memset(task->pkg.little_cluster_runtime, 0,
	       sizeof(task->pkg.little_cluster_runtime));
	migt_monitor_init(task);
}
EXPORT_SYMBOL(init_task_runtime_info);

void init_package_runtime_info(struct user_struct *user)
{
	if (!user)
		return;
	memset(user->pkg.sup_cluster_runtime, 0,
	       sizeof(user->pkg.sup_cluster_runtime));
	memset(user->pkg.big_cluster_runtime, 0,
	       sizeof(user->pkg.big_cluster_runtime));
	memset(user->pkg.little_cluster_runtime, 0,
	       sizeof(user->pkg.little_cluster_runtime));
	rwlock_init(&user->pkg.lock);
	INIT_LIST_HEAD(&user->pkg.list);
	/* alloc_uid() registers only the allocation winner, under its hash lock. */
}
EXPORT_SYMBOL(init_package_runtime_info);

struct runtime_info *get_package_runtime_info(void)
{
	return package_runtime_info;
}

static int __init pkg_init(void)
{
	u64 now = get_jiffies_64();
	int ret;

	init_package_runtime_info(&user_zero);
	map_cpu_cluster();
	INIT_WORK(&package_runtime_roll, package_runtime_roll_wk);
	WRITE_ONCE(sys_update_time, now);
	package_runtime_info[HISTORY_ITMES].update_time = now;
	package_runtime_info[HISTORY_ITMES + 1].update_time = now;
	ret = create_runtime_proc();
	if (ret)
		return ret;
	smp_store_release(&runtime_enable, true);
	return 0;
}
late_initcall(pkg_init);
