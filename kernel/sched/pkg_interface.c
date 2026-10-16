// SPDX-License-Identifier: GPL-2.0-only
/* Dream 5.4.259 proc protocol; retain recovered task/user ABI.
 * Names/modes: analysis/stock-pkg-proc-interfaces.json.
 * Runtime text: projects-nexus/nexus_kernel_xiaomi_sm8250
 * f12f206b5339ad1294642e89195fe08918fb2459, without front/back extensions.
 */
#ifndef PKG_PROC_HOST_CHECK
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cred.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/pkg_stat.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/mutex.h>
#include "sched.h"
#include "pkg.h"
#include "odin_glk.h"
#endif

/* Core owns this sleepable lock: roll worker -> proc snapshot/reset.
 * WALT uses only pkg.lock. No new public ABI field or export. */
extern struct mutex pkg_runtime_mutex;
#ifdef CONFIG_MIGT
extern int vip_task_schedboost, force_stask_to_big;
#endif
static DEFINE_MUTEX(proc_lifecycle_lock);
static struct proc_dir_entry *package_root;
static bool proc_ready;
static int runtime_traced_uid = 1000;
static int traced_window = 3;
static int develop_mode = 1;

struct runtime_sample {
	unsigned int id;
	u64 value[3];
};
struct task_sample {
	struct runtime_sample runtime;
	char comm[TASK_COMM_LEN];
#ifdef CONFIG_MIGT
	u32 bucket[NUM_MIGT_BUCKETS];
	unsigned int flag;
	unsigned long boost_end;
	bool saved_little, current_little;
#endif
};
struct window_sample {
	int index;
	u64 start, end, elapsed;
};

static int parse_value(const char __user *buf, size_t count,
		       int low, int high, int *value)
{
	char text[32];
	int ret;

	if (!count)
		return -EINVAL;
	if (count >= sizeof(text))
		return -E2BIG;
	if (copy_from_user(text, buf, count))
		return -EFAULT;
	/* Embedded NUL must not turn a malformed suffix into accepted input. */
	if (memchr(text, '\0', count))
		return -EINVAL;
	text[count] = '\0';
	ret = kstrtoint(strim(text), 10, value);
	if (ret)
		return ret;
	if (*value < low || *value > high)
		return -ERANGE;
	return 0;
}

static u64 sum_runtime(const u64 *v)
{
	u64 total = v[0];
	int i;

	for (i = 1; i < 3; i++)
		total = U64_MAX - total < v[i] ? U64_MAX : total + v[i];
	return total;
}

/* Percentage in hundredths, bounded without overflowing value * 10000.
 * Rejecting a wrapped counter happens before this function. */
static u64 runtime_percent(u64 value, u64 elapsed)
{
	u64 whole, rem, fraction = 0, residue = 0;
	int i;

	if (!elapsed)
		return 0;
	if (value <= U64_MAX / 10000)
		return div64_u64(value * 10000, elapsed);
	whole = div64_u64(value, elapsed);
	rem = value - whole * elapsed;
	if (whole > U64_MAX / 10000)
		return U64_MAX;
	/* Four decimal digits, each formed by ten bounded additions. */
	for (i = 0; i < 4; i++) {
		int j, digit = 0;
		residue = 0;
		for (j = 0; j < 10; j++) {
			if (residue >= elapsed - rem) {
				residue -= elapsed - rem;
				digit++;
			} else {
				residue += rem;
			}
		}
		fraction = fraction * 10 + digit;
		rem = residue;
	}
	return U64_MAX - whole * 10000 < fraction ? U64_MAX :
		whole * 10000 + fraction;
}

static int window_index(int ring, int window, bool live)
{
	if (ring < 0 || ring >= HISTORY_ITMES ||
	    window < 0 || window >= HISTORY_ITMES)
		return -ERANGE;
	return live ? HISTORY_ITMES :
		(HISTORY_ITMES + ring - window) % HISTORY_ITMES;
}

/* Caller holds pkg_runtime_mutex, also held by the core roll worker. */
static int snapshot_window(struct window_sample *window)
{
	struct runtime_info *info = get_package_runtime_info();
	u64 now = get_jiffies_64(), ticks;

	window->index = HISTORY_ITMES;
	window->start = READ_ONCE(info[HISTORY_ITMES + 1].update_time);
	window->end = now;
	window->elapsed = 0;
	if (now <= window->start)
		return 0;
	ticks = now - window->start;
	/* Dream HZ=250: the native 64-bit conversion multiplies by 4000000.
	 * jiffies_to_nsecs() instead truncates via u32 microseconds after 71min. */
	if (ticks > div64_u64(U64_MAX, NSEC_PER_SEC / HZ))
		return -ERANGE;
	window->elapsed = jiffies64_to_nsecs(ticks);
	return 0;
}

static void sample_package(struct package_runtime_info *pkg,
			   struct runtime_sample *sample, int index)
{
	/* A reset/wrapped component invalidates the whole interval. */
	if (pkg->sup_cluster_runtime[index] < pkg->sup_cluster_runtime[HISTORY_ITMES + 1] ||
	    pkg->big_cluster_runtime[index] < pkg->big_cluster_runtime[HISTORY_ITMES + 1] ||
	    pkg->little_cluster_runtime[index] < pkg->little_cluster_runtime[HISTORY_ITMES + 1]) {
		memset(sample->value, 0, sizeof(sample->value));
		return;
	}
	sample->value[0] = pkg->sup_cluster_runtime[index] -
		pkg->sup_cluster_runtime[HISTORY_ITMES + 1];
	sample->value[1] = pkg->big_cluster_runtime[index] -
		pkg->big_cluster_runtime[HISTORY_ITMES + 1];
	sample->value[2] = pkg->little_cluster_runtime[index] -
		pkg->little_cluster_runtime[HISTORY_ITMES + 1];
}

static void sample_user(struct user_struct *user,
			struct runtime_sample *sample, int index)
{
	unsigned long flags;

	read_lock_irqsave(&user->pkg.lock, flags);
	sample_package(&user->pkg, sample, index);
	read_unlock_irqrestore(&user->pkg.lock, flags);
}

/* Storage is allocated outside IRQ/RCU locks. On list growth return an error,
 * never silently omit tasks. Each acquired reference has exactly one put. */
static int get_user_tasks(struct user_struct *user,
			  struct task_struct ***result, size_t *count)
{
	struct task_struct *task, **tasks;
	unsigned long flags;
	size_t capacity = 0, nr = 0, i;
	bool grew = false;

	*result = NULL;
	*count = 0;
	read_lock_irqsave(&user->pkg.lock, flags);
	list_for_each_entry(task, &user->pkg.list, pkg.list)
		capacity++;
	read_unlock_irqrestore(&user->pkg.lock, flags);
	if (!capacity)
		return 0;
	tasks = kvmalloc_array(capacity, sizeof(*tasks), GFP_KERNEL);
	if (!tasks)
		return -ENOMEM;
	read_lock_irqsave(&user->pkg.lock, flags);
	list_for_each_entry(task, &user->pkg.list, pkg.list) {
		if (nr == capacity) {
			grew = true;
			break;
		}
		get_task_struct(task);
		tasks[nr++] = task;
	}
	read_unlock_irqrestore(&user->pkg.lock, flags);
	if (grew) {
		for (i = 0; i < nr; i++)
			put_task_struct(tasks[i]);
		kvfree(tasks);
		return -EAGAIN;
	}
	*result = tasks;
	*count = nr;
	return 0;
}

static void put_user_tasks(struct task_struct **tasks, size_t nr)
{
	size_t i;

	for (i = 0; i < nr; i++)
		put_task_struct(tasks[i]);
	kvfree(tasks);
}

/* user lock excludes UID detach/transfer; RCU only protects cred lookup. */
static bool task_matches_user(struct task_struct *task, struct user_struct *user)
{
	bool match;

	rcu_read_lock();
	match = !list_empty(&task->pkg.list) && uid_eq(task_uid(task), user->uid);
	rcu_read_unlock();
	return match;
}

static int snapshot_tasks(struct user_struct *user, int index,
			  struct task_sample **result, size_t *count)
{
	struct task_struct **tasks;
	struct task_sample *samples;
	unsigned long flags;
	size_t nr, i, used = 0;
	int ret = get_user_tasks(user, &tasks, &nr);

	*result = NULL;
	*count = 0;
	if (ret || !nr)
		return ret;
	samples = kvcalloc(nr, sizeof(*samples), GFP_KERNEL);
	if (!samples) {
		put_user_tasks(tasks, nr);
		return -ENOMEM;
	}
	for (i = 0; i < nr; i++) {
		struct task_struct *task = tasks[i];
		struct task_sample *s = &samples[used];
		bool match;

		read_lock_irqsave(&user->pkg.lock, flags);
		match = task_matches_user(task, user);
		if (match) {
			read_lock(&task->pkg.lock);
			s->runtime.id = task_pid_nr(task);
			sample_package(&task->pkg, &s->runtime, index);
#ifdef CONFIG_MIGT
			memcpy(s->bucket, task->pkg.migt.bucket, sizeof(s->bucket));
			s->flag = task->pkg.migt.flag;
			s->boost_end = task->pkg.migt.boost_end;
			s->saved_little = !cpumask_test_cpu(4, &task->pkg.migt.cpus_allowed);
#endif
			read_unlock(&task->pkg.lock);
		}
		read_unlock_irqrestore(&user->pkg.lock, flags);
		if (!match)
			continue;
		get_task_comm(s->comm, task);
#ifdef CONFIG_MIGT
		/* Actual scheduler affinity uses task/rq locking, outside pkg locks. */
		{
			struct rq_flags rf;
			struct rq *rq = task_rq_lock(task, &rf);

			s->current_little = !cpumask_test_cpu(4, &task->cpus_mask);
			task_rq_unlock(rq, task, &rf);
		}
#endif
		used++;
	}
	put_user_tasks(tasks, nr);
	*result = samples;
	*count = used;
	return 0;
}

static void baseline_package(struct package_runtime_info *pkg, int index)
{
	pkg->sup_cluster_runtime[HISTORY_ITMES + 1] = pkg->sup_cluster_runtime[index];
	pkg->big_cluster_runtime[HISTORY_ITMES + 1] = pkg->big_cluster_runtime[index];
	pkg->little_cluster_runtime[HISTORY_ITMES + 1] = pkg->little_cluster_runtime[index];
}

/* All allocations and task references are acquired before changing baseline.
 * user -> task lock order excludes UID transfer during each commit. */
struct reset_sample {
	struct user_struct *user;
	struct task_struct **tasks;
	size_t nr;
};
static void commit_reset(struct reset_sample *s, int index)
{
	unsigned long flags;
	size_t i;

	write_lock_irqsave(&s->user->pkg.lock, flags);
	baseline_package(&s->user->pkg, index);
	write_unlock_irqrestore(&s->user->pkg.lock, flags);
	for (i = 0; i < s->nr; i++) {
		read_lock_irqsave(&s->user->pkg.lock, flags);
		if (task_matches_user(s->tasks[i], s->user)) {
			write_lock(&s->tasks[i]->pkg.lock);
			baseline_package(&s->tasks[i]->pkg, index);
			write_unlock(&s->tasks[i]->pkg.lock);
		}
		read_unlock_irqrestore(&s->user->pkg.lock, flags);
	}
}

/* Core mutex prevents ring overwrite throughout reset. Registry retirement
 * belongs exclusively to core/user.c, never proc. WALT is not globally frozen. */
static int set_traced_window(int window, int live)
{
	struct reset_sample *samples = NULL;
	struct user_struct *user;
	struct runtime_info *info = get_package_runtime_info();
	unsigned long flags;
	int bit, index, ret = 0;
	size_t nr = 0, i;
	u64 stamp;

	mutex_lock(&pkg_runtime_mutex);
	if (window == -1)
		window = READ_ONCE(traced_window);
	if (live == -1)
		live = READ_ONCE(develop_mode);
	index = window_index(READ_ONCE(*(int *)get_curr_runtime_items()), window, live);
	if (index < 0) {
		ret = index;
		goto out;
	}
	stamp = live ? get_jiffies_64() : READ_ONCE(info[index].update_time);
	samples = kvcalloc(TRACE_UID_MAX - 1, sizeof(*samples), GFP_KERNEL);
	if (!samples) {
		ret = -ENOMEM;
		goto out;
	}
	for_each_set_bit(bit, package_uid_bit, TRACE_UID_MAX - 1) {
		user = find_user(KUIDT_INIT(bit));
		if (!user)
			continue;
		samples[nr].user = user;
		/* Include this lookup in cleanup even if task allocation fails. */
		nr++;
		ret = get_user_tasks(user, &samples[nr - 1].tasks, &samples[nr - 1].nr);
		if (ret)
			goto out;
	}
	for (i = 0; i < nr; i++)
		commit_reset(&samples[i], index);
	user = get_user_zero();
	write_lock_irqsave(&user->pkg.lock, flags);
	baseline_package(&user->pkg, index);
	write_unlock_irqrestore(&user->pkg.lock, flags);
	WRITE_ONCE(info[HISTORY_ITMES + 1].update_time, stamp);
	WRITE_ONCE(traced_window, window);
	WRITE_ONCE(develop_mode, live);
 out:
	for (i = 0; i < nr; i++) {
		put_user_tasks(samples[i].tasks, samples[i].nr);
		free_uid(samples[i].user);
	}
	kvfree(samples);
	mutex_unlock(&pkg_runtime_mutex);
	return ret;
}

static int collect_users(struct runtime_sample **result, size_t *count)
{
	struct runtime_sample *samples;
	struct user_struct *user;
	int bit, maximum = READ_ONCE(*(int *)get_uid_max_value());
	size_t nr = 0;

	*result = NULL;
	*count = 0;
	if (maximum < 0 || maximum >= TRACE_UID_MAX - 1)
		return -ERANGE;
	/* Fixed UID ceiling; allocation outside IRQ/RCU. Concurrent publication
	 * above this snapshot's maximum is included by the next read. */
	samples = kvcalloc((size_t)maximum + 2, sizeof(*samples), GFP_KERNEL);
	if (!samples)
		return -ENOMEM;
	for_each_set_bit(bit, package_uid_bit, maximum + 1) {
		user = find_user(KUIDT_INIT(bit));
		if (!user)
			continue;
		samples[nr].id = bit;
		sample_user(user, &samples[nr++], HISTORY_ITMES);
		free_uid(user);
	}
	samples[nr].id = TRACE_UID_MAX - 1;
	sample_user(get_user_zero(), &samples[nr++], HISTORY_ITMES);
	*result = samples;
	*count = nr;
	return 0;
}

static void print_window(struct seq_file *m, const struct window_sample *w)
{
	/* Avoid subtracting a future baseline from now. */
	seq_printf(m, "time info: from [0]s to [%llu]s ago\n",
		w->end >= w->start ? div64_u64(w->end - w->start, HZ) : 0);
}

static void print_runtime(struct seq_file *m, const struct runtime_sample *s,
			  const char *comm, u64 elapsed)
{
	u64 pct[3], total = sum_runtime(s->value), bias[2];
	int i;

	for (i = 0; i < 3; i++)
		pct[i] = runtime_percent(s->value[i], elapsed);
	bias[0] = runtime_percent(s->value[0], total);
	bias[1] = runtime_percent(s->value[1], total);
	if (comm) {
		seq_printf(m, "%5u,%16s,s:%16llu [%3llu.%02llu],b:%16llu [%3llu.%02llu]l:%16llu [%3llu.%02llu],| s:[%3llu.%02llu], b:[%3llu.%02llu]\n",
			s->id, comm, s->value[0], pct[0] / 100, pct[0] % 100,
			s->value[1], pct[1] / 100, pct[1] % 100,
			s->value[2], pct[2] / 100, pct[2] % 100,
			bias[0] / 100, bias[0] % 100, bias[1] / 100, bias[1] % 100);
		return;
	}
	seq_printf(m, "%5u: ", s->id);
	seq_printf(m, "<s>%16llu [%3llu.%02llu] <b>%16llu [%3llu.%02llu] <l>:%16llu [%3llu.%02llu]--bias: s-[%3llu.%02llu] b-[%3llu.%02llu]\n",
		s->value[0], pct[0] / 100, pct[0] % 100,
		s->value[1], pct[1] / 100, pct[1] % 100,
		s->value[2], pct[2] / 100, pct[2] % 100,
		bias[0] / 100, bias[0] % 100, bias[1] / 100, bias[1] % 100);
}

static void print_totals(struct seq_file *m, const u64 *total, u64 elapsed)
{
	u64 s = runtime_percent(total[0], elapsed);
	u64 b = runtime_percent(total[1], elapsed);
	u64 l = runtime_percent(total[2], elapsed);

	seq_printf(m, "usage---sup cluster:[%llu.%02llu] big cluster:[%llu.%02llu] little cluster:[%llu.%02llu]\n",
		s / 100, s % 100, b / 100, b % 100, l / 100, l % 100);
	seq_printf(m, "total_usage---sup cluster:[%llu] big cluster:[%llu] little cluster:[%llu]\n",
		total[0], total[1], total[2]);
}

/* Top reads compute their own winner, avoiding stale donor globals. */
static int select_top(const struct runtime_sample *samples, size_t nr, int kind)
{
	u64 best = 0, value;
	int uid = 0;
	size_t i;

	for (i = 0; i < nr; i++) {
		value = kind == 2 ? sum_runtime(samples[i].value) :
			samples[i].value[kind == 0 ? 1 : 2];
		if (value > best) {
			best = value;
			uid = samples[i].id;
		}
	}
	return uid;
}

static int runtime_show(struct seq_file *m, void *v)
{
	long kind = (long)m->private;
	struct runtime_sample *users = NULL;
	struct task_sample *tasks = NULL;
	struct window_sample window;
	struct user_struct *user;
	u64 total[3] = { 0 }, top[4] = { 0 };
	unsigned int top_uid[4] = { 0 };
	size_t nr = 0, i;
	int uid = READ_ONCE(runtime_traced_uid), ret;
#ifdef CONFIG_MIGT
	bool enabled = migt_enable();
	bool super_enabled = READ_ONCE(vip_task_schedboost) || READ_ONCE(force_stask_to_big);
	unsigned long tick = jiffies;
#endif

	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	mutex_lock(&pkg_runtime_mutex);
	ret = snapshot_window(&window);
	if (ret)
		goto unlock;
	if (kind <= 3) {
		ret = collect_users(&users, &nr);
		if (ret)
			goto unlock;
		if (kind)
			uid = window.elapsed ? select_top(users, nr, kind - 1) : 0;
	}
	if (kind) {
#ifdef CONFIG_MIGT
		if (kind == 5) {
			int render_uid = get_cur_render_uid();
			if (render_uid >= 0 && render_uid < TRACE_UID_MAX - 1)
				uid = render_uid;
		}
#endif
		user = find_user(KUIDT_INIT(uid));
		if (user) {
			ret = snapshot_tasks(user, window.index, &tasks, &nr);
			free_uid(user);
		} else {
			nr = 0;
		}
	}
 unlock:
	mutex_unlock(&pkg_runtime_mutex);
	if (ret)
		goto out;
	if (kind)
		seq_printf(m, "traced uid %d\n", uid);
	print_window(m, &window);
	for (i = 0; i < nr; i++) {
#ifdef CONFIG_MIGT
		if (kind == 5) {
			const struct task_sample *s = &tasks[i];
			int b;
			/* Match scheduler classifiers from owned flags without invoking
			 * game_vip_task(), whose expired boost path takes a write lock. */
			bool vip = enabled ? !!(s->flag & MASK_VTASK) :
				time_before(tick, s->boost_end) && (s->flag & MASK_MI_VTASK);
			const char *type = enabled && super_enabled && (s->flag & MASK_STASK) ? "sup" :
				(s->flag & MASK_RTASK) ? "ren" : vip ? "vip" :
				enabled && (s->flag & MASK_ITASK) ? "imp" : "nor";

			seq_printf(m, "%5u,%16s buckests:{", s->runtime.id, s->comm);
			for (b = 0; b < NUM_MIGT_BUCKETS; b++)
				seq_printf(m, " %8u", s->bucket[b]);
			seq_printf(m, "  } %s %d %d %d\n", type,
				s->current_little, s->saved_little, !!(s->flag & MINOR_TASK));
			continue;
		}
#endif
		if (window.elapsed) {
			const struct runtime_sample *s = kind ? &tasks[i].runtime : &users[i];
			u64 sum = sum_runtime(s->value);
			int c;

			if (!sum)
				continue;
			print_runtime(m, s, kind ? tasks[i].comm : NULL, window.elapsed);
			for (c = 0; c < 3; c++) {
				total[c] = U64_MAX - total[c] < s->value[c] ? U64_MAX :
					total[c] + s->value[c];
				if (s->value[c] > top[c + 1]) {
					top[c + 1] = s->value[c];
					top_uid[c + 1] = s->id;
				}
			}
			if (sum > top[0]) {
				top[0] = sum;
				top_uid[0] = s->id;
			}
		}
	}
	if (kind != 5) {
		print_totals(m, total, window.elapsed);
		if (!kind) {
			static const char * const labels[] = {
				"g:     top_package", "score: top_package",
				"bcore: top_package", "lcore: top_package",
			};
			int c;

			seq_puts(m, "top load package info--- 0 for s/b/l cluster\n");
			for (c = 0; c < 4; c++) {
				u64 pct = runtime_percent(top[c], window.elapsed);

				seq_printf(m, "%s: %u %3llu.%02llu\n", labels[c],
					top_uid[c], pct / 100, pct % 100);
			}
		}
	}
 out:
	kvfree(tasks);
	kvfree(users);
	return ret;
}

static int trace_stat_show(struct seq_file *m, void *v)
{
	seq_printf(m, "traced uid  is %d\ntraced max uid is %d\n",
		READ_ONCE(runtime_traced_uid), READ_ONCE(*(int *)get_uid_max_value()));
	return 0;
}
static int traced_window_show(struct seq_file *m, void *v)
{
	seq_printf(m, "traced window is last %d\n", READ_ONCE(traced_window));
	return 0;
}
static int window_size_show(struct seq_file *m, void *v)
{
	seq_printf(m, "window size is %d\n", READ_ONCE(*(int *)get_runtime_window_size()));
	return 0;
}
static int develop_mode_show(struct seq_file *m, void *v)
{
	seq_printf(m, "develop mode %s\n", READ_ONCE(develop_mode) ? "open" : "close");
	return 0;
}
static int pause_mode_show(struct seq_file *m, void *v)
{
	seq_printf(m, "pause mode %s\n", READ_ONCE(pause_mode) ? "yes" : "no");
	return 0;
}

static ssize_t traced_uid_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *pos)
{
	int value, ret;

	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	ret = parse_value(buf, count, 0, TRACE_UID_MAX - 2, &value);
	if (ret)
		return ret;
	WRITE_ONCE(runtime_traced_uid, value);
	return count;
}
static ssize_t traced_window_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *pos)
{
	int value, ret;

	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	ret = parse_value(buf, count, 0, HISTORY_ITMES - 1, &value);
	if (!ret)
		ret = set_traced_window(value, -1);
	return ret ? ret : count;
}
static ssize_t develop_mode_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *pos)
{
	int value, ret;

	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	ret = parse_value(buf, count, 0, 1, &value);
	if (!ret)
		ret = set_traced_window(-1, value);
	return ret ? ret : count;
}
static ssize_t pause_mode_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *pos)
{
	int value, ret;

	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	ret = parse_value(buf, count, 0, 1, &value);
	if (ret)
		return ret;
	WRITE_ONCE(pause_mode, value);
	return count;
}

static int proc_single_open(struct file *file,
			    int (*show)(struct seq_file *, void *), void *data)
{
	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	return single_open(file, show, data);
}
#define RUNTIME_OPEN(name, kind) \
static int name(struct inode *inode, struct file *file) \
{ return proc_single_open(file, runtime_show, (void *)(long)(kind)); }
RUNTIME_OPEN(show_all_open, 0)
RUNTIME_OPEN(top_bcore_open, 1)
RUNTIME_OPEN(top_lcore_open, 2)
RUNTIME_OPEN(top_open, 3)
RUNTIME_OPEN(traced_open, 4)
RUNTIME_OPEN(exec_buckets_open, 5)
#define SIMPLE_OPEN(name, show) \
static int name(struct inode *inode, struct file *file) \
{ return proc_single_open(file, show, NULL); }
SIMPLE_OPEN(trace_stat_open, trace_stat_show)
SIMPLE_OPEN(traced_window_open, traced_window_show)
SIMPLE_OPEN(window_size_open, window_size_show)
SIMPLE_OPEN(develop_mode_open, develop_mode_show)
SIMPLE_OPEN(pause_mode_open, pause_mode_show)

/* Reset once at open, rather than repeatedly when seq_file retries show. */
static int reset_show(struct seq_file *m, void *v)
{
	seq_puts(m, "reset traced window show success!\n");
	return 0;
}
static int reset_open(struct inode *inode, struct file *file)
{
	int ret;

	if (!smp_load_acquire(&proc_ready))
		return -EAGAIN;
	ret = single_open(file, reset_show, NULL);
	if (ret)
		return ret;
	ret = set_traced_window(-1, -1);
	if (ret)
		single_release(inode, file);
	return ret;
}
#define FOPS(name, opener, writer) \
static const struct file_operations name = { \
	.open = opener, .read = seq_read, .write = writer, \
	.llseek = seq_lseek, .release = single_release, \
}
FOPS(show_all_fops, show_all_open, NULL);
FOPS(top_bcore_fops, top_bcore_open, NULL);
FOPS(top_lcore_fops, top_lcore_open, NULL);
FOPS(top_fops, top_open, NULL);
FOPS(traced_fops, traced_open, traced_uid_write);
FOPS(trace_stat_fops, trace_stat_open, NULL);
FOPS(traced_window_fops, traced_window_open, traced_window_write);
FOPS(window_size_fops, window_size_open, NULL);
FOPS(develop_mode_fops, develop_mode_open, develop_mode_write);
FOPS(reset_fops, reset_open, NULL);
FOPS(pause_mode_fops, pause_mode_open, pause_mode_write);
FOPS(exec_buckets_fops, exec_buckets_open, NULL);

struct proc_spec {
	const char *parent, *name;
	umode_t mode;
	const struct file_operations *fops;
};
static const struct proc_spec proc_nodes[] = {
	{ "pkg", "show_all", 0664, &show_all_fops },
	{ "pkg", "top_package_on_bcore", 0664, &top_bcore_fops },
	{ "pkg", "top_package_on_lcore", 0664, &top_lcore_fops },
	{ "pkg", "top_package", 0664, &top_fops },
	{ "pkg", "show_traced_package", 0666, &traced_fops },
	{ "stat", "show_tracestat", 0664, &trace_stat_fops },
	{ "stat", "show_traced_window", 0664, &traced_window_fops },
	{ "stat", "show_windowsize", 0664, &window_size_fops },
	{ "stat", "develop_mode", 0666, &develop_mode_fops },
	{ "stat", "reset_traced_window", 0664, &reset_fops },
	{ "stat", "pause_mode", 0666, &pause_mode_fops },
	{ "migt", "show_exec_buckets", 0664, &exec_buckets_fops },
};

static void delete_runtime_proc_locked(void)
{
	smp_store_release(&proc_ready, false);
	delete_game_load_proc();
	if (package_root)
		proc_remove(package_root);
	package_root = NULL;
}
void delete_runtime_proc(void)
{
	mutex_lock(&proc_lifecycle_lock);
	delete_runtime_proc_locked();
	mutex_unlock(&proc_lifecycle_lock);
}
int create_runtime_proc(void)
{
	struct proc_dir_entry *pkg, *stat, *migt;
	size_t i;
	int ret = -ENOMEM;

	mutex_lock(&proc_lifecycle_lock);
	if (package_root) {
		ret = -EALREADY;
		goto out;
	}
	package_root = proc_mkdir("package", NULL);
	if (!package_root)
		goto fail;
	pkg = proc_mkdir("pkg", package_root);
	if (!pkg)
		goto fail;
	stat = proc_mkdir("stat", package_root);
	if (!stat)
		goto fail;
	migt = proc_mkdir("migt", package_root);
	if (!migt)
		goto fail;
	for (i = 0; i < ARRAY_SIZE(proc_nodes); i++) {
		const struct proc_spec *s = &proc_nodes[i];
		struct proc_dir_entry *parent = !strcmp(s->parent, "pkg") ? pkg :
			!strcmp(s->parent, "stat") ? stat : migt;

		if (!proc_create(s->name, s->mode, parent, s->fops))
			goto fail;
	}
	/* Stock initializer passes migt, not a new /proc/uid root. */
	ret = create_game_load_proc(migt);
	if (ret)
		goto fail;
	smp_store_release(&proc_ready, true);
	ret = 0;
	goto out;
 fail:
	delete_runtime_proc_locked();
 out:
	mutex_unlock(&proc_lifecycle_lock);
	return ret;
}
