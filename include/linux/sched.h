/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_H
#define _LINUX_SCHED_H



#include <uapi/linux/sched.h>

#include <asm/current.h>

#include <linux/pid.h>
#include <linux/sem.h>
#include <linux/shm.h>
#include <linux/kcov.h>
#include <linux/mutex.h>
#include <linux/plist.h>
#include <linux/hrtimer.h>
#include <linux/seccomp.h>
#include <linux/nodemask.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/resource.h>
#include <linux/latencytop.h>
#include <linux/sched/prio.h>
#include <linux/sched/types.h>
#include <linux/signal_types.h>
#include <linux/mm_types_task.h>
#include <linux/task_io_accounting.h>
#include <linux/posix-timers.h>
#include <linux/rseq.h>
#include <linux/android_kabi.h>
#include <linux/android_vendor.h>
#ifdef CONFIG_PACKAGE_RUNTIME_INFO
#include <linux/pkg_stat.h>
#endif


struct audit_context;
struct backing_dev_info;
struct bio_list;
struct blk_plug;
struct capture_control;
struct cfs_rq;
struct fs_struct;
struct futex_pi_state;
struct io_context;
struct mempolicy;
struct nameidata;
struct nsproxy;
struct perf_event_context;
struct pid_namespace;
struct pipe_inode_info;
struct rcu_node;
struct reclaim_state;
struct robust_list_head;
struct root_domain;
struct rq;
struct sched_attr;
struct sched_param;
struct seq_file;
struct sighand_struct;
struct signal_struct;
struct task_delay_info;
struct task_group;




#define TASK_RUNNING			0x0000
#define TASK_INTERRUPTIBLE		0x0001
#define TASK_UNINTERRUPTIBLE		0x0002
#define __TASK_STOPPED			0x0004
#define __TASK_TRACED			0x0008

#define EXIT_DEAD			0x0010
#define EXIT_ZOMBIE			0x0020
#define EXIT_TRACE			(EXIT_ZOMBIE | EXIT_DEAD)

#define TASK_PARKED			0x0040
#define TASK_DEAD			0x0080
#define TASK_WAKEKILL			0x0100
#define TASK_WAKING			0x0200
#define TASK_NOLOAD			0x0400
#define TASK_NEW			0x0800
#define TASK_STATE_MAX			0x1000


#define TASK_KILLABLE			(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_STOPPED			(TASK_WAKEKILL | __TASK_STOPPED)
#define TASK_TRACED			(TASK_WAKEKILL | __TASK_TRACED)

#define TASK_IDLE			(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)


#define TASK_NORMAL			(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)


#define TASK_REPORT			(TASK_RUNNING | TASK_INTERRUPTIBLE | \
					 TASK_UNINTERRUPTIBLE | __TASK_STOPPED | \
					 __TASK_TRACED | EXIT_DEAD | EXIT_ZOMBIE | \
					 TASK_PARKED)

#define task_is_traced(task)		((task->state & __TASK_TRACED) != 0)

#define task_is_stopped(task)		((task->state & __TASK_STOPPED) != 0)

#define task_is_stopped_or_traced(task)	((task->state & (__TASK_STOPPED | __TASK_TRACED)) != 0)

#define task_contributes_to_load(task)	((task->state & TASK_UNINTERRUPTIBLE) != 0 && \
					 (task->flags & PF_FROZEN) == 0 && \
					 (task->state & TASK_NOLOAD) == 0)

enum task_boost_type {
	TASK_BOOST_NONE = 0,
	TASK_BOOST_ON_MID,
	TASK_BOOST_ON_MAX,
	TASK_BOOST_STRICT_MAX,
	TASK_BOOST_END,
};

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP


#define is_special_task_state(state)				\
	((state) & (__TASK_STOPPED | __TASK_TRACED | TASK_PARKED | TASK_DEAD))

#define __set_current_state(state_value)			\
	do {							\
		WARN_ON_ONCE(is_special_task_state(state_value));\
		current->task_state_change = _THIS_IP_;		\
		current->state = (state_value);			\
	} while (0)

#define set_current_state(state_value)				\
	do {							\
		WARN_ON_ONCE(is_special_task_state(state_value));\
		current->task_state_change = _THIS_IP_;		\
		smp_store_mb(current->state, (state_value));	\
	} while (0)

#define set_special_state(state_value)					\
	do {								\
		unsigned long flags; 			\
		WARN_ON_ONCE(!is_special_task_state(state_value));	\
		raw_spin_lock_irqsave(&current->pi_lock, flags);	\
		current->task_state_change = _THIS_IP_;			\
		current->state = (state_value);				\
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);	\
	} while (0)
#else

#define __set_current_state(state_value)				\
	current->state = (state_value)

#define set_current_state(state_value)					\
	smp_store_mb(current->state, (state_value))


#define set_special_state(state_value)					\
	do {								\
		unsigned long flags; 			\
		raw_spin_lock_irqsave(&current->pi_lock, flags);	\
		current->state = (state_value);				\
		raw_spin_unlock_irqrestore(&current->pi_lock, flags);	\
	} while (0)

#endif


#define TASK_COMM_LEN			16

enum task_event {
	PUT_PREV_TASK   = 0,
	PICK_NEXT_TASK  = 1,
	TASK_WAKE       = 2,
	TASK_MIGRATE    = 3,
	TASK_UPDATE     = 4,
	IRQ_UPDATE      = 5,
};


enum migrate_types {
	GROUP_TO_RQ,
	RQ_TO_GROUP,
};

#if defined(CONFIG_HOTPLUG_CPU) && defined(CONFIG_SCHED_WALT)
extern int sched_isolate_cpu(int cpu);
extern int sched_unisolate_cpu(int cpu);
extern int sched_unisolate_cpu_unlocked(int cpu);
#else
static inline int sched_isolate_cpu(int cpu)
{
	return 0;
}

static inline int sched_unisolate_cpu(int cpu)
{
	return 0;
}

static inline int sched_unisolate_cpu_unlocked(int cpu)
{
	return 0;
}
#endif

extern void scheduler_tick(void);

#define	MAX_SCHEDULE_TIMEOUT		LONG_MAX

extern long schedule_timeout(long timeout);
extern long schedule_timeout_interruptible(long timeout);
extern long schedule_timeout_killable(long timeout);
extern long schedule_timeout_uninterruptible(long timeout);
extern long schedule_timeout_idle(long timeout);
asmlinkage void schedule(void);
extern void schedule_preempt_disabled(void);
asmlinkage void preempt_schedule_irq(void);

extern int __must_check io_schedule_prepare(void);
extern void io_schedule_finish(int token);
extern long io_schedule_timeout(long timeout);
extern void io_schedule(void);


struct prev_cputime {
#ifndef CONFIG_VIRT_CPU_ACCOUNTING_NATIVE
	u64				utime;
	u64				stime;
	raw_spinlock_t			lock;
#endif
};

enum vtime_state {
	
	VTIME_INACTIVE = 0,
	
	VTIME_USER,
	
	VTIME_SYS,
};

struct vtime {
	seqcount_t		seqcount;
	unsigned long long	starttime;
	enum vtime_state	state;
	u64			utime;
	u64			stime;
	u64			gtime;
};


enum uclamp_id {
	UCLAMP_MIN = 0,
	UCLAMP_MAX,
	UCLAMP_CNT
};

#ifdef CONFIG_SMP
extern struct root_domain def_root_domain;
extern struct mutex sched_domains_mutex;
#endif

struct sched_info {
#ifdef CONFIG_SCHED_INFO
	

	
	unsigned long			pcount;

	
	unsigned long long		run_delay;

	

	
	unsigned long long		last_arrival;

	
	unsigned long long		last_queued;

#endif 
};


# define SCHED_FIXEDPOINT_SHIFT		10
# define SCHED_FIXEDPOINT_SCALE		(1L << SCHED_FIXEDPOINT_SHIFT)


# define SCHED_CAPACITY_SHIFT		SCHED_FIXEDPOINT_SHIFT
# define SCHED_CAPACITY_SCALE		(1L << SCHED_CAPACITY_SHIFT)

struct load_weight {
	unsigned long			weight;
	u32				inv_weight;
};


struct util_est {
	unsigned int			enqueued;
	unsigned int			ewma;
#define UTIL_EST_WEIGHT_SHIFT		2
} __attribute__((__aligned__(sizeof(u64))));


struct sched_avg {
	u64				last_update_time;
	u64				load_sum;
	u64				runnable_load_sum;
	u32				util_sum;
	u32				period_contrib;
	unsigned long			load_avg;
	unsigned long			runnable_load_avg;
	unsigned long			util_avg;
	struct util_est			util_est;
} ____cacheline_aligned;

struct sched_statistics {
#ifdef CONFIG_SCHEDSTATS
	u64				wait_start;
	u64				wait_max;
	u64				wait_count;
	u64				wait_sum;
	u64				iowait_count;
	u64				iowait_sum;

	u64				sleep_start;
	u64				sleep_max;
	s64				sum_sleep_runtime;

	u64				block_start;
	u64				block_max;
	u64				exec_max;
	u64				slice_max;

	u64				nr_migrations_cold;
	u64				nr_failed_migrations_affine;
	u64				nr_failed_migrations_running;
	u64				nr_failed_migrations_hot;
	u64				nr_forced_migrations;

	u64				nr_wakeups;
	u64				nr_wakeups_sync;
	u64				nr_wakeups_migrate;
	u64				nr_wakeups_local;
	u64				nr_wakeups_remote;
	u64				nr_wakeups_affine;
	u64				nr_wakeups_affine_attempts;
	u64				nr_wakeups_passive;
	u64				nr_wakeups_idle;
#endif
};

struct sched_entity {
	
	struct load_weight		load;
	unsigned long			runnable_weight;
	struct rb_node			run_node;
	struct list_head		group_node;
	unsigned int			on_rq;

	u64				exec_start;
	u64				sum_exec_runtime;
	u64				vruntime;
	u64				prev_sum_exec_runtime;

	u64				nr_migrations;

	struct sched_statistics		statistics;

#ifdef CONFIG_FAIR_GROUP_SCHED
	int				depth;
	struct sched_entity		*parent;
	
	struct cfs_rq			*cfs_rq;
	
	struct cfs_rq			*my_q;
#endif

#ifdef CONFIG_SMP
	
	struct sched_avg		avg;
#endif

	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
};

struct cpu_cycle_counter_cb {
	u64 (*get_cpu_cycle_counter)(int cpu);
};

DECLARE_PER_CPU_READ_MOSTLY(int, sched_load_boost);

#ifdef CONFIG_QCOM_HYP_CORE_CTL
extern int hh_vcpu_populate_affinity_info(u32 cpu_index, u64 cap_id);
extern int hh_vpm_grp_populate_info(u64 cap_id, int virq_num);
#else
static inline int hh_vcpu_populate_affinity_info(u32 cpu_index, u64 cap_id)
{
	return 0;
}
static inline int  hh_vpm_grp_populate_info(u64 cap_id, int virq_num)
{
	return 0;
}
#endif 

#ifdef CONFIG_SCHED_WALT
extern void walt_task_dead(struct task_struct *p);
extern int
register_cpu_cycle_counter_cb(struct cpu_cycle_counter_cb *cb);
extern void
sched_update_cpu_freq_min_max(const cpumask_t *cpus, u32 fmin, u32 fmax);
extern void free_task_load_ptrs(struct task_struct *p);
extern int set_task_boost(int boost, u64 period);
extern void walt_update_cluster_topology(void);

#define RAVG_HIST_SIZE_MAX  5
#define NUM_BUSY_BUCKETS 10

#define WALT_LOW_LATENCY_PROCFS	BIT(0)
#define WALT_LOW_LATENCY_BINDER	BIT(1)

struct walt_task_struct {
	
	u64				mark_start;
	u32				sum, demand;
	u32				coloc_demand;
	u32				sum_history[RAVG_HIST_SIZE_MAX];
	u32				*curr_window_cpu, *prev_window_cpu;
	u32				curr_window, prev_window;
	u32				pred_demand;
	u8				busy_buckets[NUM_BUSY_BUCKETS];
	u16				demand_scaled;
	u16				pred_demand_scaled;
	u64				active_time;
	int				boost;
	bool				wake_up_idle;
	bool				misfit;
	bool				rtg_high_prio;
	u8				low_latency;
	u64				boost_period;
	u64				boost_expires;
	u64				last_sleep_ts;
	u32				init_load_pct;
	u32				unfilter;
	u64				last_wake_ts;
	u64				last_enqueued_ts;
	struct walt_related_thread_group __rcu	*grp;
	struct list_head		grp_list;
	u64				cpu_cycles;
	cpumask_t			cpus_requested;
	bool				iowaited;
	#ifdef CONFIG_RTMM
	u64				proc_load;
	#endif
};

#else
static inline void walt_task_dead(struct task_struct *p) { }

static inline int
register_cpu_cycle_counter_cb(struct cpu_cycle_counter_cb *cb)
{
	return 0;
}

static inline void free_task_load_ptrs(struct task_struct *p) { }

static inline void sched_update_cpu_freq_min_max(const cpumask_t *cpus,
					u32 fmin, u32 fmax) { }

static inline void set_task_boost(int boost, u64 period) { }
static inline void walt_update_cluster_topology(void) { }
#endif 

struct sched_rt_entity {
	struct list_head		run_list;
	unsigned long			timeout;
	unsigned long			watchdog_stamp;
	unsigned int			time_slice;
	unsigned short			on_rq;
	unsigned short			on_list;

	struct sched_rt_entity		*back;
#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity		*parent;
	
	struct rt_rq			*rt_rq;
	
	struct rt_rq			*my_q;
#endif

	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
} __randomize_layout;

struct sched_dl_entity {
	struct rb_node			rb_node;

	
	u64				dl_runtime;	
	u64				dl_deadline;	
	u64				dl_period;	
	u64				dl_bw;		
	u64				dl_density;	

	
	s64				runtime;	
	u64				deadline;	
	unsigned int			flags;		

	
	unsigned int			dl_throttled      : 1;
	unsigned int			dl_boosted        : 1;
	unsigned int			dl_yielded        : 1;
	unsigned int			dl_non_contending : 1;
	unsigned int			dl_overrun	  : 1;

	
	struct hrtimer			dl_timer;

	
	struct hrtimer inactive_timer;
};

#ifdef CONFIG_UCLAMP_TASK

#define UCLAMP_BUCKETS CONFIG_UCLAMP_BUCKETS_COUNT


struct uclamp_se {
	unsigned int value		: bits_per(SCHED_CAPACITY_SCALE);
	unsigned int bucket_id		: bits_per(UCLAMP_BUCKETS);
	unsigned int active		: 1;
	unsigned int user_defined	: 1;
};
#endif 

union rcu_special {
	struct {
		u8			blocked;
		u8			need_qs;
		u8			exp_hint; 
		u8			deferred_qs;
	} b; 
	u32 s; 
};

enum perf_event_task_context {
	perf_invalid_context = -1,
	perf_hw_context = 0,
	perf_sw_context,
	perf_nr_task_contexts,
};

struct wake_q_node {
	struct wake_q_node *next;
};

struct task_struct {
#ifdef CONFIG_THREAD_INFO_IN_TASK
	
	struct thread_info		thread_info;
#endif
	
	volatile long			state;

	
	randomized_struct_fields_start

	void				*stack;
	refcount_t			usage;
	
	unsigned int			flags;
	unsigned int			ptrace;

#ifdef CONFIG_SMP
	struct llist_node		wake_entry;
	int				on_cpu;
#ifdef CONFIG_THREAD_INFO_IN_TASK
	
	unsigned int			cpu;
#endif
	unsigned int			wakee_flips;
	unsigned long			wakee_flip_decay_ts;
	struct task_struct		*last_wakee;

	
	int				recent_used_cpu;
	int				wake_cpu;
#endif
	int				on_rq;

	int				prio;
	int				static_prio;
	int				normal_prio;
	unsigned int			rt_priority;

	const struct sched_class	*sched_class;
	struct sched_entity		se;
	struct sched_rt_entity		rt;

#ifdef CONFIG_SCHED_WALT
	struct walt_task_struct		wts;
#endif

#ifdef CONFIG_CGROUP_SCHED
	struct task_group		*sched_task_group;
#endif
	struct sched_dl_entity		dl;

#ifdef CONFIG_UCLAMP_TASK
	
	struct uclamp_se		uclamp_req[UCLAMP_CNT];
	
	struct uclamp_se		uclamp[UCLAMP_CNT];
#endif

#ifdef CONFIG_PREEMPT_NOTIFIERS
	
	struct hlist_head		preempt_notifiers;
#endif

#ifdef CONFIG_BLK_DEV_IO_TRACE
	unsigned int			btrace_seq;
#endif

	unsigned int			policy;
	int				nr_cpus_allowed;
	const cpumask_t			*cpus_ptr;
	cpumask_t			cpus_mask;

#ifdef CONFIG_PREEMPT_RCU
	int				rcu_read_lock_nesting;
	union rcu_special		rcu_read_unlock_special;
	struct list_head		rcu_node_entry;
	struct rcu_node			*rcu_blocked_node;
#endif 

#ifdef CONFIG_TASKS_RCU
	unsigned long			rcu_tasks_nvcsw;
	u8				rcu_tasks_holdout;
	u8				rcu_tasks_idx;
	int				rcu_tasks_idle_cpu;
	struct list_head		rcu_tasks_holdout_list;
#endif 

	struct sched_info		sched_info;

	struct list_head		tasks;
#ifdef CONFIG_SMP
	struct plist_node		pushable_tasks;
	struct rb_node			pushable_dl_tasks;
#endif

	struct mm_struct		*mm;
	struct mm_struct		*active_mm;

	
	struct vmacache			vmacache;

#ifdef SPLIT_RSS_COUNTING
	struct task_rss_stat		rss_stat;
#endif
	int				exit_state;
	int				exit_code;
	int				exit_signal;
	
	int				pdeath_signal;
	
	unsigned long			jobctl;

	
	unsigned int			personality;

	
	unsigned			sched_reset_on_fork:1;
	unsigned			sched_contributes_to_load:1;
	unsigned			sched_migrated:1;
	unsigned			sched_remote_wakeup:1;
#ifdef CONFIG_PSI
	unsigned			sched_psi_wake_requeue:1;
#endif

	
	unsigned			:0;

	

	
	unsigned			in_execve:1;
	unsigned			in_iowait:1;
#ifndef TIF_RESTORE_SIGMASK
	unsigned			restore_sigmask:1;
#endif
#ifdef CONFIG_MEMCG
	unsigned			in_user_fault:1;
#endif
#ifdef CONFIG_COMPAT_BRK
	unsigned			brk_randomized:1;
#endif
#ifdef CONFIG_CGROUPS
	
	unsigned			no_cgroup_migration:1;
	
	unsigned			frozen:1;
#endif
#ifdef CONFIG_BLK_CGROUP
	
	unsigned			use_memdelay:1;
#endif

	unsigned long			atomic_flags; 

	struct restart_block		restart_block;

	pid_t				pid;
	pid_t				tgid;

#ifdef CONFIG_STACKPROTECTOR
	
	unsigned long			stack_canary;
#endif
	

	
	struct task_struct __rcu	*real_parent;

	
	struct task_struct __rcu	*parent;

	
	struct list_head		children;
	struct list_head		sibling;
	struct task_struct		*group_leader;

	
	struct list_head		ptraced;
	struct list_head		ptrace_entry;

	
	struct pid			*thread_pid;
	struct hlist_node		pid_links[PIDTYPE_MAX];
	struct list_head		thread_group;
	struct list_head		thread_node;

	struct completion		*vfork_done;

	
	int __user			*set_child_tid;

	
	int __user			*clear_child_tid;

	u64				utime;
	u64				stime;
#ifdef CONFIG_ARCH_HAS_SCALED_CPUTIME
	u64				utimescaled;
	u64				stimescaled;
#endif
	u64				gtime;
#ifdef CONFIG_CPU_FREQ_TIMES
	u64				*time_in_state;
	unsigned int			max_state;
#endif
	struct prev_cputime		prev_cputime;
#ifdef CONFIG_VIRT_CPU_ACCOUNTING_GEN
	struct vtime			vtime;
#endif

#ifdef CONFIG_NO_HZ_FULL
	atomic_t			tick_dep_mask;
#endif
	
	unsigned long			nvcsw;
	unsigned long			nivcsw;

	
	u64				start_time;

	
	u64				real_start_time;

	
	unsigned long			min_flt;
	unsigned long			maj_flt;

	
	struct posix_cputimers		posix_cputimers;

	

	
	const struct cred __rcu		*ptracer_cred;

	
	const struct cred __rcu		*real_cred;

	
	const struct cred __rcu		*cred;

#ifdef CONFIG_KEYS
	
	struct key			*cached_requested_key;
#endif

	
	char				comm[TASK_COMM_LEN];

	struct nameidata		*nameidata;

#ifdef CONFIG_SYSVIPC
	struct sysv_sem			sysvsem;
	struct sysv_shm			sysvshm;
#endif
#ifdef CONFIG_DETECT_HUNG_TASK
	
	unsigned long			last_switch_count;
	unsigned long			last_switch_time;
	bool hang_detection_enabled;
#endif
	
	struct fs_struct		*fs;

	
	struct files_struct		*files;

	
	struct nsproxy			*nsproxy;

	
	struct signal_struct		*signal;
	struct sighand_struct		*sighand;
	sigset_t			blocked;
	sigset_t			real_blocked;
	
	sigset_t			saved_sigmask;
	struct sigpending		pending;
	unsigned long			sas_ss_sp;
	size_t				sas_ss_size;
	unsigned int			sas_ss_flags;

	struct callback_head		*task_works;

#ifdef CONFIG_AUDIT
#ifdef CONFIG_AUDITSYSCALL
	struct audit_context		*audit_context;
#endif
	kuid_t				loginuid;
	unsigned int			sessionid;
#endif
	struct seccomp			seccomp;

	
	u64				parent_exec_id;
	u64				self_exec_id;

	
	spinlock_t			alloc_lock;

	
	raw_spinlock_t			pi_lock;

	struct wake_q_node		wake_q;

#ifdef CONFIG_RT_MUTEXES
	
	struct rb_root_cached		pi_waiters;
	
	struct task_struct		*pi_top_task;
	
	struct rt_mutex_waiter		*pi_blocked_on;
#endif

#ifdef CONFIG_DEBUG_MUTEXES
	
	struct mutex_waiter		*blocked_on;
#endif

#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	int				non_block_count;
#endif

#ifdef CONFIG_TRACE_IRQFLAGS
	unsigned int			irq_events;
	unsigned long			hardirq_enable_ip;
	unsigned long			hardirq_disable_ip;
	unsigned int			hardirq_enable_event;
	unsigned int			hardirq_disable_event;
	int				hardirqs_enabled;
	int				hardirq_context;
	unsigned long			softirq_disable_ip;
	unsigned long			softirq_enable_ip;
	unsigned int			softirq_disable_event;
	unsigned int			softirq_enable_event;
	int				softirqs_enabled;
	int				softirq_context;
#endif

#ifdef CONFIG_LOCKDEP
# define MAX_LOCK_DEPTH			48UL
	u64				curr_chain_key;
	int				lockdep_depth;
	unsigned int			lockdep_recursion;
	struct held_lock		held_locks[MAX_LOCK_DEPTH];
#endif

#ifdef CONFIG_UBSAN
	unsigned int			in_ubsan;
#endif

	
	void				*journal_info;

	
	struct bio_list			*bio_list;

#ifdef CONFIG_BLOCK
	
	struct blk_plug			*plug;
#endif

	
	struct reclaim_state		*reclaim_state;

	struct backing_dev_info		*backing_dev_info;

	struct io_context		*io_context;

#ifdef CONFIG_COMPACTION
	struct capture_control		*capture_control;
#endif
	
	unsigned long			ptrace_message;
	kernel_siginfo_t		*last_siginfo;

	struct task_io_accounting	ioac;
#ifdef CONFIG_PSI
	
	unsigned int			psi_flags;
#endif
#ifdef CONFIG_TASK_XACCT
	
	u64				acct_rss_mem1;
	
	u64				acct_vm_mem1;
	
	u64				acct_timexpd;
#endif
#ifdef CONFIG_CPUSETS
	
	nodemask_t			mems_allowed;
	
	seqcount_t			mems_allowed_seq;
	int				cpuset_mem_spread_rotor;
	int				cpuset_slab_spread_rotor;
#endif
#ifdef CONFIG_CGROUPS
	
	struct css_set __rcu		*cgroups;
	
	struct list_head		cg_list;
#endif
#ifdef CONFIG_X86_CPU_RESCTRL
	u32				closid;
	u32				rmid;
#endif
#ifdef CONFIG_FUTEX
	struct robust_list_head __user	*robust_list;
#ifdef CONFIG_COMPAT
	struct compat_robust_list_head __user *compat_robust_list;
#endif
	struct list_head		pi_state_list;
	struct futex_pi_state		*pi_state_cache;
	struct mutex			futex_exit_mutex;
	unsigned int			futex_state;
#endif
#ifdef CONFIG_PERF_EVENTS
	struct perf_event_context	*perf_event_ctxp[perf_nr_task_contexts];
	struct mutex			perf_event_mutex;
	struct list_head		perf_event_list;
#endif
#ifdef CONFIG_DEBUG_PREEMPT
	unsigned long			preempt_disable_ip;
#endif
#ifdef CONFIG_NUMA
	
	struct mempolicy		*mempolicy;
	short				il_prev;
	short				pref_node_fork;
#endif
#ifdef CONFIG_NUMA_BALANCING
	int				numa_scan_seq;
	unsigned int			numa_scan_period;
	unsigned int			numa_scan_period_max;
	int				numa_preferred_nid;
	unsigned long			numa_migrate_retry;
	
	u64				node_stamp;
	u64				last_task_numa_placement;
	u64				last_sum_exec_runtime;
	struct callback_head		numa_work;

	
	struct numa_group __rcu		*numa_group;

	
	unsigned long			*numa_faults;
	unsigned long			total_numa_faults;

	
	unsigned long			numa_faults_locality[3];

	unsigned long			numa_pages_migrated;
#endif 

#ifdef CONFIG_RSEQ
	struct rseq __user *rseq;
	u32 rseq_sig;
	
	unsigned long rseq_event_mask;
#endif

	struct tlbflush_unmap_batch	tlb_ubc;

	union {
		refcount_t		rcu_users;
		struct rcu_head		rcu;
	};

	
	struct pipe_inode_info		*splice_pipe;

	struct page_frag		task_frag;

#ifdef CONFIG_TASK_DELAY_ACCT
	struct task_delay_info		*delays;
#endif

#ifdef CONFIG_FAULT_INJECTION
	int				make_it_fail;
	unsigned int			fail_nth;
#endif
	
	int				nr_dirtied;
	int				nr_dirtied_pause;
	
	unsigned long			dirty_paused_when;

#ifdef CONFIG_LATENCYTOP
	int				latency_record_count;
	struct latency_record		latency_record[LT_SAVECOUNT];
#endif
	
	u64				timer_slack_ns;
	u64				default_timer_slack_ns;
#ifdef CONFIG_PERF_CRITICAL_RT_TASK
	unsigned int    		critical_rt_task;
#endif
#ifdef CONFIG_SF_BINDER
	unsigned int			sf_binder_task;
#endif
#ifdef CONFIG_PERF_HUMANTASK
	unsigned int			human_task;
	unsigned int                    inherit_task;
#endif
#ifdef CONFIG_KASAN
	unsigned int			kasan_depth;
#endif

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	
	int				curr_ret_stack;
	int				curr_ret_depth;

	
	struct ftrace_ret_stack		*ret_stack;

	
	unsigned long long		ftrace_timestamp;

	
	atomic_t			trace_overrun;

	
	atomic_t			tracing_graph_pause;
#endif

#ifdef CONFIG_TRACING
	
	unsigned long			trace;

	
	unsigned long			trace_recursion;
#endif 

#ifdef CONFIG_KCOV
	

	
	unsigned int			kcov_mode;

	
	unsigned int			kcov_size;

	
	void				*kcov_area;

	
	struct kcov			*kcov;

	
	u64				kcov_handle;

	
	int				kcov_sequence;
#endif

#ifdef CONFIG_MEMCG
	struct mem_cgroup		*memcg_in_oom;
	gfp_t				memcg_oom_gfp_mask;
	int				memcg_oom_order;

	
	unsigned int			memcg_nr_pages_over_high;

	
	struct mem_cgroup		*active_memcg;
#endif

#ifdef CONFIG_BLK_CGROUP
	struct request_queue		*throttle_queue;
#endif

#ifdef CONFIG_UPROBES
	struct uprobe_task		*utask;
#endif
#if defined(CONFIG_BCACHE) || defined(CONFIG_BCACHE_MODULE)
	unsigned int			sequential_io;
	unsigned int			sequential_io_avg;
#endif
#ifdef CONFIG_DEBUG_ATOMIC_SLEEP
	unsigned long			task_state_change;
#endif
	int				pagefault_disabled;
#ifdef CONFIG_MMU
	struct task_struct		*oom_reaper_list;
#endif
#ifdef CONFIG_VMAP_STACK
	struct vm_struct		*stack_vm_area;
#endif
#ifdef CONFIG_THREAD_INFO_IN_TASK
	
	refcount_t			stack_refcount;
#endif
#ifdef CONFIG_LIVEPATCH
	int patch_state;
#endif
#ifdef CONFIG_SECURITY
	
	void				*security;
#endif

#ifdef CONFIG_GCC_PLUGIN_STACKLEAK
	unsigned long			lowest_stack;
	unsigned long			prev_lowest_stack;
#endif

#ifdef CONFIG_PACKAGE_RUNTIME_INFO
	struct package_runtime_info pkg;
#endif

	ANDROID_VENDOR_DATA_ARRAY(1, 3);
	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
	ANDROID_KABI_RESERVE(3);
	ANDROID_KABI_RESERVE(4);
	ANDROID_KABI_RESERVE(5);
	ANDROID_KABI_RESERVE(6);
	ANDROID_KABI_RESERVE(7);
	ANDROID_KABI_RESERVE(8);

	
	randomized_struct_fields_end

	
	struct thread_struct		thread;

	
};

static inline struct pid *task_pid(struct task_struct *task)
{
	return task->thread_pid;
}


pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type, struct pid_namespace *ns);

static inline pid_t task_pid_nr(struct task_struct *tsk)
{
	return tsk->pid;
}

static inline pid_t task_pid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PID, ns);
}

static inline pid_t task_pid_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PID, NULL);
}


static inline pid_t task_tgid_nr(struct task_struct *tsk)
{
	return tsk->tgid;
}


static inline int pid_alive(const struct task_struct *p)
{
	return p->thread_pid != NULL;
}

static inline pid_t task_pgrp_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PGID, ns);
}

static inline pid_t task_pgrp_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_PGID, NULL);
}


static inline pid_t task_session_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_SID, ns);
}

static inline pid_t task_session_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_SID, NULL);
}

static inline pid_t task_tgid_nr_ns(struct task_struct *tsk, struct pid_namespace *ns)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_TGID, ns);
}

static inline pid_t task_tgid_vnr(struct task_struct *tsk)
{
	return __task_pid_nr_ns(tsk, PIDTYPE_TGID, NULL);
}

static inline pid_t task_ppid_nr_ns(const struct task_struct *tsk, struct pid_namespace *ns)
{
	pid_t pid = 0;

	rcu_read_lock();
	if (pid_alive(tsk))
		pid = task_tgid_nr_ns(rcu_dereference(tsk->real_parent), ns);
	rcu_read_unlock();

	return pid;
}

static inline pid_t task_ppid_nr(const struct task_struct *tsk)
{
	return task_ppid_nr_ns(tsk, &init_pid_ns);
}


static inline pid_t task_pgrp_nr(struct task_struct *tsk)
{
	return task_pgrp_nr_ns(tsk, &init_pid_ns);
}

#define TASK_REPORT_IDLE	(TASK_REPORT + 1)
#define TASK_REPORT_MAX		(TASK_REPORT_IDLE << 1)

static inline unsigned int task_state_index(struct task_struct *tsk)
{
	unsigned int tsk_state = READ_ONCE(tsk->state);
	unsigned int state = (tsk_state | tsk->exit_state) & TASK_REPORT;

	BUILD_BUG_ON_NOT_POWER_OF_2(TASK_REPORT_MAX);

	if (tsk_state == TASK_IDLE)
		state = TASK_REPORT_IDLE;

	return fls(state);
}

static inline char task_index_to_char(unsigned int state)
{
	static const char state_char[] = "RSDTtXZPI";

	BUILD_BUG_ON(1 + ilog2(TASK_REPORT_MAX) != sizeof(state_char) - 1);

	return state_char[state];
}

static inline char task_state_to_char(struct task_struct *tsk)
{
	return task_index_to_char(task_state_index(tsk));
}


static inline int is_global_init(struct task_struct *tsk)
{
	return task_tgid_nr(tsk) == 1;
}

extern struct pid *cad_pid;


#define PF_IDLE			0x00000002	
#define PF_EXITING		0x00000004	
#define PF_VCPU			0x00000010	
#define PF_WQ_WORKER		0x00000020	
#define PF_FORKNOEXEC		0x00000040	
#define PF_MCE_PROCESS		0x00000080      
#define PF_SUPERPRIV		0x00000100	
#define PF_DUMPCORE		0x00000200	
#define PF_SIGNALED		0x00000400	
#define PF_MEMALLOC		0x00000800	
#define PF_NPROC_EXCEEDED	0x00001000	
#define PF_USED_MATH		0x00002000	
#define PF_NOFREEZE		0x00008000	
#define PF_FROZEN		0x00010000	
#define PF_KSWAPD		0x00020000	
#define PF_MEMALLOC_NOFS	0x00040000	
#define PF_MEMALLOC_NOIO	0x00080000	
#define PF_LESS_THROTTLE	0x00100000	
#define PF_KTHREAD		0x00200000	
#define PF_RANDOMIZE		0x00400000	
#define PF_SWAPWRITE		0x00800000	
#define PF_MEMSTALL		0x01000000	
#define PF_UMH			0x02000000	
#define PF_NO_SETAFFINITY	0x04000000	
#define PF_MCE_EARLY		0x08000000      
#define PF_MEMALLOC_NOCMA	0x10000000	
#define PF_FREEZER_SKIP		0x40000000	
#define PF_SUSPEND_TASK		0x80000000      


#define clear_stopped_child_used_math(child)	do { (child)->flags &= ~PF_USED_MATH; } while (0)
#define set_stopped_child_used_math(child)	do { (child)->flags |= PF_USED_MATH; } while (0)
#define clear_used_math()			clear_stopped_child_used_math(current)
#define set_used_math()				set_stopped_child_used_math(current)

#define conditional_stopped_child_used_math(condition, child) \
	do { (child)->flags &= ~PF_USED_MATH, (child)->flags |= (condition) ? PF_USED_MATH : 0; } while (0)

#define conditional_used_math(condition)	conditional_stopped_child_used_math(condition, current)

#define copy_to_stopped_child_used_math(child) \
	do { (child)->flags &= ~PF_USED_MATH, (child)->flags |= current->flags & PF_USED_MATH; } while (0)


#define tsk_used_math(p)			((p)->flags & PF_USED_MATH)
#define used_math()				tsk_used_math(current)

static __always_inline bool is_percpu_thread(void)
{
#ifdef CONFIG_SMP
	return (current->flags & PF_NO_SETAFFINITY) &&
		(current->nr_cpus_allowed  == 1);
#else
	return true;
#endif
}


#define PFA_NO_NEW_PRIVS		0	
#define PFA_SPREAD_PAGE			1	
#define PFA_SPREAD_SLAB			2	
#define PFA_SPEC_SSB_DISABLE		3	
#define PFA_SPEC_SSB_FORCE_DISABLE	4	
#define PFA_SPEC_IB_DISABLE		5	
#define PFA_SPEC_IB_FORCE_DISABLE	6	
#define PFA_SPEC_SSB_NOEXEC		7	

#define TASK_PFA_TEST(name, func)					\
	static inline bool task_##func(struct task_struct *p)		\
	{ return test_bit(PFA_##name, &p->atomic_flags); }

#define TASK_PFA_SET(name, func)					\
	static inline void task_set_##func(struct task_struct *p)	\
	{ set_bit(PFA_##name, &p->atomic_flags); }

#define TASK_PFA_CLEAR(name, func)					\
	static inline void task_clear_##func(struct task_struct *p)	\
	{ clear_bit(PFA_##name, &p->atomic_flags); }

TASK_PFA_TEST(NO_NEW_PRIVS, no_new_privs)
TASK_PFA_SET(NO_NEW_PRIVS, no_new_privs)

TASK_PFA_TEST(SPREAD_PAGE, spread_page)
TASK_PFA_SET(SPREAD_PAGE, spread_page)
TASK_PFA_CLEAR(SPREAD_PAGE, spread_page)

TASK_PFA_TEST(SPREAD_SLAB, spread_slab)
TASK_PFA_SET(SPREAD_SLAB, spread_slab)
TASK_PFA_CLEAR(SPREAD_SLAB, spread_slab)

TASK_PFA_TEST(SPEC_SSB_DISABLE, spec_ssb_disable)
TASK_PFA_SET(SPEC_SSB_DISABLE, spec_ssb_disable)
TASK_PFA_CLEAR(SPEC_SSB_DISABLE, spec_ssb_disable)

TASK_PFA_TEST(SPEC_SSB_NOEXEC, spec_ssb_noexec)
TASK_PFA_SET(SPEC_SSB_NOEXEC, spec_ssb_noexec)
TASK_PFA_CLEAR(SPEC_SSB_NOEXEC, spec_ssb_noexec)

TASK_PFA_TEST(SPEC_SSB_FORCE_DISABLE, spec_ssb_force_disable)
TASK_PFA_SET(SPEC_SSB_FORCE_DISABLE, spec_ssb_force_disable)

TASK_PFA_TEST(SPEC_IB_DISABLE, spec_ib_disable)
TASK_PFA_SET(SPEC_IB_DISABLE, spec_ib_disable)
TASK_PFA_CLEAR(SPEC_IB_DISABLE, spec_ib_disable)

TASK_PFA_TEST(SPEC_IB_FORCE_DISABLE, spec_ib_force_disable)
TASK_PFA_SET(SPEC_IB_FORCE_DISABLE, spec_ib_force_disable)

static inline void
current_restore_flags(unsigned long orig_flags, unsigned long flags)
{
	current->flags &= ~flags;
	current->flags |= orig_flags & flags;
}

extern int cpuset_cpumask_can_shrink(const struct cpumask *cur, const struct cpumask *trial);
extern int task_can_attach(struct task_struct *p, const struct cpumask *cs_cpus_allowed);
#ifdef CONFIG_SMP
extern void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask);
extern int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask);
extern bool cpupri_check_rt(void);
#else
static inline void do_set_cpus_allowed(struct task_struct *p, const struct cpumask *new_mask)
{
}
static inline int set_cpus_allowed_ptr(struct task_struct *p, const struct cpumask *new_mask)
{
	if (!cpumask_test_cpu(0, new_mask))
		return -EINVAL;
	return 0;
}
static inline bool cpupri_check_rt(void)
{
	return false;
}
#endif

extern int yield_to(struct task_struct *p, bool preempt);
extern void set_user_nice(struct task_struct *p, long nice);
extern int task_prio(const struct task_struct *p);


static inline int task_nice(const struct task_struct *p)
{
	return PRIO_TO_NICE((p)->static_prio);
}

extern int can_nice(const struct task_struct *p, const int nice);
extern int task_curr(const struct task_struct *p);
extern int idle_cpu(int cpu);
extern int available_idle_cpu(int cpu);
extern int sched_setscheduler(struct task_struct *, int, const struct sched_param *);
extern int sched_setscheduler_nocheck(struct task_struct *, int, const struct sched_param *);
extern int sched_setattr(struct task_struct *, const struct sched_attr *);
extern int sched_setattr_nocheck(struct task_struct *, const struct sched_attr *);
extern struct task_struct *idle_task(int cpu);


static inline bool is_idle_task(const struct task_struct *p)
{
	return !!(p->flags & PF_IDLE);
}

extern struct task_struct *curr_task(int cpu);
extern void ia64_set_curr_task(int cpu, struct task_struct *p);

void yield(void);

union thread_union {
#ifndef CONFIG_ARCH_TASK_STRUCT_ON_STACK
	struct task_struct task;
#endif
#ifndef CONFIG_THREAD_INFO_IN_TASK
	struct thread_info thread_info;
#endif
	unsigned long stack[THREAD_SIZE/sizeof(long)];
};

#ifndef CONFIG_THREAD_INFO_IN_TASK
extern struct thread_info init_thread_info;
#endif

extern unsigned long init_stack[THREAD_SIZE / sizeof(unsigned long)];

#ifdef CONFIG_THREAD_INFO_IN_TASK
static inline struct thread_info *task_thread_info(struct task_struct *task)
{
	return &task->thread_info;
}
#elif !defined(__HAVE_THREAD_FUNCTIONS)
# define task_thread_info(task)	((struct thread_info *)(task)->stack)
#endif



extern struct task_struct *find_task_by_vpid(pid_t nr);
extern struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns);


extern struct task_struct *find_get_task_by_vpid(pid_t nr);

extern int wake_up_state(struct task_struct *tsk, unsigned int state);
extern int wake_up_process(struct task_struct *tsk);
extern void wake_up_new_task(struct task_struct *tsk);

#ifdef CONFIG_SMP
extern void kick_process(struct task_struct *tsk);
#else
static inline void kick_process(struct task_struct *tsk) { }
#endif

extern void __set_task_comm(struct task_struct *tsk, const char *from, bool exec);

static inline void set_task_comm(struct task_struct *tsk, const char *from)
{
	__set_task_comm(tsk, from, false);
}

extern char *__get_task_comm(char *to, size_t len, struct task_struct *tsk);
#define get_task_comm(buf, tsk) ({			\
	BUILD_BUG_ON(sizeof(buf) != TASK_COMM_LEN);	\
	__get_task_comm(buf, sizeof(buf), tsk);		\
})

#ifdef CONFIG_SMP
void scheduler_ipi(void);
extern unsigned long wait_task_inactive(struct task_struct *, long match_state);
#else
static inline void scheduler_ipi(void) { }
static inline unsigned long wait_task_inactive(struct task_struct *p, long match_state)
{
	return 1;
}
#endif


static inline void set_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	set_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline void clear_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	clear_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline void update_tsk_thread_flag(struct task_struct *tsk, int flag,
					  bool value)
{
	update_ti_thread_flag(task_thread_info(tsk), flag, value);
}

static inline int test_and_set_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_and_set_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline int test_and_clear_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_and_clear_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline int test_tsk_thread_flag(struct task_struct *tsk, int flag)
{
	return test_ti_thread_flag(task_thread_info(tsk), flag);
}

static inline void set_tsk_need_resched(struct task_struct *tsk)
{
	set_tsk_thread_flag(tsk,TIF_NEED_RESCHED);
}

static inline void clear_tsk_need_resched(struct task_struct *tsk)
{
	clear_tsk_thread_flag(tsk,TIF_NEED_RESCHED);
}

static inline int test_tsk_need_resched(struct task_struct *tsk)
{
	return unlikely(test_tsk_thread_flag(tsk,TIF_NEED_RESCHED));
}


#ifndef CONFIG_PREEMPTION
extern int _cond_resched(void);
#else
static inline int _cond_resched(void) { return 0; }
#endif

#define cond_resched() ({			\
	___might_sleep(__FILE__, __LINE__, 0);	\
	_cond_resched();			\
})

extern int __cond_resched_lock(spinlock_t *lock);

#define cond_resched_lock(lock) ({				\
	___might_sleep(__FILE__, __LINE__, PREEMPT_LOCK_OFFSET);\
	__cond_resched_lock(lock);				\
})

static inline void cond_resched_rcu(void)
{
#if defined(CONFIG_DEBUG_ATOMIC_SLEEP) || !defined(CONFIG_PREEMPT_RCU)
	rcu_read_unlock();
	cond_resched();
	rcu_read_lock();
#endif
}


static inline int spin_needbreak(spinlock_t *lock)
{
#ifdef CONFIG_PREEMPTION
	return spin_is_contended(lock);
#else
	return 0;
#endif
}

static __always_inline bool need_resched(void)
{
	return unlikely(tif_need_resched());
}


#ifdef CONFIG_SMP

static inline unsigned int task_cpu(const struct task_struct *p)
{
#ifdef CONFIG_THREAD_INFO_IN_TASK
	return READ_ONCE(p->cpu);
#else
	return READ_ONCE(task_thread_info(p)->cpu);
#endif
}

extern void set_task_cpu(struct task_struct *p, unsigned int cpu);

#else

static inline unsigned int task_cpu(const struct task_struct *p)
{
	return 0;
}

static inline void set_task_cpu(struct task_struct *p, unsigned int cpu)
{
}

#endif 


#ifndef vcpu_is_preempted
static inline bool vcpu_is_preempted(int cpu)
{
	return false;
}
#endif

extern long sched_setaffinity(pid_t pid, const struct cpumask *new_mask);
extern long sched_getaffinity(pid_t pid, struct cpumask *mask);

#ifndef TASK_SIZE_OF
#define TASK_SIZE_OF(tsk)	TASK_SIZE
#endif

#ifdef CONFIG_RSEQ


enum rseq_event_mask_bits {
	RSEQ_EVENT_PREEMPT_BIT	= RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT,
	RSEQ_EVENT_SIGNAL_BIT	= RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT,
	RSEQ_EVENT_MIGRATE_BIT	= RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT,
};

enum rseq_event_mask {
	RSEQ_EVENT_PREEMPT	= (1U << RSEQ_EVENT_PREEMPT_BIT),
	RSEQ_EVENT_SIGNAL	= (1U << RSEQ_EVENT_SIGNAL_BIT),
	RSEQ_EVENT_MIGRATE	= (1U << RSEQ_EVENT_MIGRATE_BIT),
};

static inline void rseq_set_notify_resume(struct task_struct *t)
{
	if (t->rseq)
		set_tsk_thread_flag(t, TIF_NOTIFY_RESUME);
}

void __rseq_handle_notify_resume(struct ksignal *sig, struct pt_regs *regs);

static inline void rseq_handle_notify_resume(struct ksignal *ksig,
					     struct pt_regs *regs)
{
	if (current->rseq)
		__rseq_handle_notify_resume(ksig, regs);
}

static inline void rseq_signal_deliver(struct ksignal *ksig,
				       struct pt_regs *regs)
{
	preempt_disable();
	__set_bit(RSEQ_EVENT_SIGNAL_BIT, &current->rseq_event_mask);
	preempt_enable();
	rseq_handle_notify_resume(ksig, regs);
}


static inline void rseq_preempt(struct task_struct *t)
{
	__set_bit(RSEQ_EVENT_PREEMPT_BIT, &t->rseq_event_mask);
	rseq_set_notify_resume(t);
}


static inline void rseq_migrate(struct task_struct *t)
{
	__set_bit(RSEQ_EVENT_MIGRATE_BIT, &t->rseq_event_mask);
	rseq_set_notify_resume(t);
}


static inline void rseq_fork(struct task_struct *t, unsigned long clone_flags)
{
	if (clone_flags & CLONE_VM) {
		t->rseq = NULL;
		t->rseq_sig = 0;
		t->rseq_event_mask = 0;
	} else {
		t->rseq = current->rseq;
		t->rseq_sig = current->rseq_sig;
		t->rseq_event_mask = current->rseq_event_mask;
	}
}

static inline void rseq_execve(struct task_struct *t)
{
	t->rseq = NULL;
	t->rseq_sig = 0;
	t->rseq_event_mask = 0;
}

#else

static inline void rseq_set_notify_resume(struct task_struct *t)
{
}
static inline void rseq_handle_notify_resume(struct ksignal *ksig,
					     struct pt_regs *regs)
{
}
static inline void rseq_signal_deliver(struct ksignal *ksig,
				       struct pt_regs *regs)
{
}
static inline void rseq_preempt(struct task_struct *t)
{
}
static inline void rseq_migrate(struct task_struct *t)
{
}
static inline void rseq_fork(struct task_struct *t, unsigned long clone_flags)
{
}
static inline void rseq_execve(struct task_struct *t)
{
}

#endif

void __exit_umh(struct task_struct *tsk);

static inline void exit_umh(struct task_struct *tsk)
{
	if (unlikely(tsk->flags & PF_UMH))
		__exit_umh(tsk);
}

#ifdef CONFIG_DEBUG_RSEQ

void rseq_syscall(struct pt_regs *regs);

#else

static inline void rseq_syscall(struct pt_regs *regs)
{
}

#endif

const struct sched_avg *sched_trace_cfs_rq_avg(struct cfs_rq *cfs_rq);
extern char *sched_trace_cfs_rq_path(struct cfs_rq *cfs_rq, char *str, int len);
extern int sched_trace_cfs_rq_cpu(struct cfs_rq *cfs_rq);

extern const struct sched_avg *sched_trace_rq_avg_rt(struct rq *rq);
const struct sched_avg *sched_trace_rq_avg_dl(struct rq *rq);
const struct sched_avg *sched_trace_rq_avg_irq(struct rq *rq);

int sched_trace_rq_cpu(struct rq *rq);

const struct cpumask *sched_trace_rd_span(struct root_domain *rd);

#ifdef CONFIG_SCHED_WALT
#define PF_WAKE_UP_IDLE	1
static inline u32 sched_get_wake_up_idle(struct task_struct *p)
{
	return p->wts.wake_up_idle;
}

static inline int sched_set_wake_up_idle(struct task_struct *p,
						int wake_up_idle)
{
	p->wts.wake_up_idle = !!wake_up_idle;
	return 0;
}

static inline void set_wake_up_idle(bool enabled)
{
	current->wts.wake_up_idle = enabled;
}
#else
static inline u32 sched_get_wake_up_idle(struct task_struct *p)
{
	return 0;
}

static inline int sched_set_wake_up_idle(struct task_struct *p,
						int wake_up_idle)
{
	return 0;
}

static inline void set_wake_up_idle(bool enabled) {}
#endif

#endif
