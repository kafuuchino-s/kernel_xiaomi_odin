/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ODIN_SCHED_PKG_H
#define ODIN_SCHED_PKG_H

#include <linux/mutex.h>

/* Private bookkeeping; do not change Dream's task/user package ABI. */
#define TRACE_UID_MAX 81920

struct runtime_info {
	u64 update_time;
};

extern DECLARE_BITMAP(package_uid_bit, TRACE_UID_MAX);
extern int pause_mode;
extern struct mutex pkg_runtime_mutex;
/* Shared sleepable renderer policy gate; driver uses its ioctl mutex. */
void migt_policy_lock(void);
void migt_policy_unlock(void);
void *get_curr_runtime_items(void);
void *get_uid_max_value(void);
void *get_runtime_window_size(void);
void *get_user_zero(void);
struct runtime_info *get_package_runtime_info(void);
int create_runtime_proc(void);
void delete_runtime_proc(void);

/* Called with the UID hash lock held, after the allocation winner is known. */
void pkg_uid_register_locked(struct user_struct *user);
bool pkg_uid_retire_locked(struct user_struct *user);
/* Consumes only the registry reference, never the caller's lookup reference. */
void pkg_release_registered_user(struct user_struct *user);
void pkg_task_attach(struct task_struct *task, struct user_struct *user);
bool pkg_task_detach(struct task_struct *task, struct user_struct *user);
/* Caller holds task->pkg.lock across final cleanup and cred publication. */
void pkg_task_clear_policy_locked(struct task_struct *task);

#endif
