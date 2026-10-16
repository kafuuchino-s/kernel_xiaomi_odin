#ifndef _LINUX_PKG_STAT_H
#define _LINUX_PKG_STAT_H

#define HISTORY_ITMES		 4
#define HISTORY_WINDOWS          (HISTORY_ITMES + 2)
#define USER_PKG_MIN_UID         10000
#ifdef CONFIG_MIGT
#define NUM_MIGT_BUCKETS         10
enum RENDER_TYPE {
	RENDER_QUEUE_THREAD,
	RENDER_DEQUEUE_THREAD,
	RENDER_TYPES
};

enum MIGT_TASK_TYPE {
	MIGT_NORMAL_TASK,
	GAME_NORMAL_TASK,
	GAME_IP_TASK,
	GAME_VIP_TASK,
	GAME_QRENDER_TASK,
	GAME_DQRENDER_TASK,
	GAME_SUPER_TASK,
	MIGT_MINOR_TASK,
	MI_VIP_TASK,
	GAME_TASK_LEVELS
};
#endif
enum cluster_type {
	LITTLE_CLUSTER = 0,
	MID_CLUSTER,
	BIG_CLUSTER,
	CLUSTER_TYPES,
};
#define MAX_CLUSTER		CLUSTER_TYPES
#define PKG_TASK_BUSY		1
#define MASK_STASK	(1 << GAME_SUPER_TASK)
#define MASK_RTASK	((1 << GAME_QRENDER_TASK) | (1 << GAME_DQRENDER_TASK))
#define MASK_VTASK	(MASK_STASK | MASK_RTASK | (1 << GAME_VIP_TASK))
#define MASK_ITASK	(MASK_VTASK | (1 << GAME_IP_TASK))
#define MASK_GTASK	(MASK_ITASK | (1 << GAME_NORMAL_TASK))
#define MASK_CLE_GTASK	(~MASK_GTASK)
#define MINOR_TASK	(1 << MIGT_MINOR_TASK)
#define MASK_MI_VTASK	(1 << MI_VIP_TASK)

#ifdef CONFIG_PACKAGE_RUNTIME_INFO
struct package_runtime_info {
	rwlock_t lock;
	struct list_head list;
	u64 sup_cluster_runtime[HISTORY_WINDOWS];
	u64 big_cluster_runtime[HISTORY_WINDOWS];
	u64 little_cluster_runtime[HISTORY_WINDOWS];
#ifdef CONFIG_MIGT
	struct {
		cpumask_t cpus_allowed;
		u32 migt_count;
		enum MIGT_TASK_TYPE flag;
		u32 wake_render;
		unsigned long boost_end;
		u64 run_times;
		u64 prev_sum;
		u32 max_exec;
		u64 fps_exec;
		u64 fps_mexec;
#ifdef VTASK_BOOST_DEBUG
		u32 boostat[NUM_MIGT_BUCKETS];
#endif
		u32 bucket[NUM_MIGT_BUCKETS];
	} migt;
#endif
};

static inline bool user_pkg(int uid)
{
	return uid > USER_PKG_MIN_UID;
}

bool pkg_enable(void);
void update_pkg_load(struct task_struct *tsk, int cpu, int flag,
		u64 wallclock, u64 delta);
void package_runtime_monitor(u64 now);
void init_task_runtime_info(struct task_struct *tsk);
void init_package_runtime_info(struct user_struct *user);
#ifdef CONFIG_MIGT
void migt_hook(struct task_struct *tsk, u64 delta, int cpu);
void migt_monitor_init(struct task_struct *p);
void migt_hook(struct task_struct *tsk, u64 delta, int cpu);
int minor_window_task(struct task_struct *tsk);
int get_cur_render_uid(void);
int game_task(struct task_struct *tsk);
int game_ip_task(struct task_struct *tsk);
int game_vip_task(struct task_struct *tsk);
int game_super_task(struct task_struct *tsk);
int is_render_thread(struct task_struct *tsk);
void reset_render_info(enum RENDER_TYPE type);
void update_render_info(struct task_struct *tsk, enum RENDER_TYPE type);
void game_load_update(struct task_struct *tsk, u64 delta, int cpu);
void game_load_history_update(u64 tick);
void game_load_reset(void);
u64 get_scale_exec_time(u64 delta, int cpu);
void update_freq_limit(u64 target_ns);
int migt_enable(void);
int glk_enable(void);
void glk_maxfreq_break(bool val);
void glk_minfreq_break(bool val);
void glk_force_maxfreq_break(bool val);
bool get_minor_window_cpumask(struct task_struct *p, cpumask_t *mask);
#endif
#else
static inline bool pkg_enable(void) { return false; }
static inline void update_pkg_load(struct task_struct *tsk, int cpu, int flag,
		u64 wallclock, u64 delta) {}
static inline void package_runtime_monitor(u64 now) {}
static inline void init_task_runtime_info(struct task_struct *tsk) {}
static inline void init_package_runtime_info(struct user_struct *user) {}
#ifdef CONFIG_MIGT
static inline void migt_hook(struct task_struct *tsk, u64 delta, int cpu) {};
static inline void migt_monitor_init(struct task_struct *p) {};
static inline int get_cur_render_uid(void) {return -1; };
static inline int game_task(struct task_struct *tsk) {return 0; }
static inline int game_ip_task(struct task_struct *tsk) {return 0; }
static inline int game_vip_task(struct task_struct *tsk) {return 0; }
static inline int game_super_task(struct task_struct *tsk) {return 0; }
static inline void
game_load_update(struct task_struct *tsk, u64 delta, int cpu) {}
static inline void game_load_history_update(u64 tick) {}
static inline void game_load_reset(void) {}
static inline int minor_window_task(struct task_struct *tsk) {return 0; }
static inline u64 update_freq_limit(u64 ts) {return 0; }
static inline int is_render_thread(struct task_struct *tsk) {return false; }
static inline void reset_render_info(enum RENDER_TYPE type) {}
static inline void
update_render_info(struct task_struct *tsk, enum RENDER_TYPE type) {}
static inline int migt_enable(void) {return 0; }
static inline int glk_enable(void) {return 0; }
static inline void glk_maxfreq_break(bool val) {}
static inline void glk_minfreq_break(bool val) {}
static inline void glk_force_maxfreq_break(bool val) {}
static inline bool
get_minor_window_cpumask(struct task_struct *p, cpumask_t *mask) {return 0; }
static inline u64 get_scale_exec_time(u64 delta, int cpu) {return 0; }
#endif
#endif
#ifdef CONFIG_MIGT
u64 sched_ktime_clock(void);
#endif
#endif 

