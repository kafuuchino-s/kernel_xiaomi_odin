/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_USER_H
#define _LINUX_SCHED_USER_H

#include <linux/uidgid.h>
#include <linux/atomic.h>
#include <linux/refcount.h>
#include <linux/ratelimit.h>
#include <linux/android_kabi.h>
#ifdef CONFIG_PACKAGE_RUNTIME_INFO
#include <linux/pkg_stat.h>
#endif

struct user_struct {
	refcount_t __count;	
	atomic_t processes;	
	atomic_t sigpending;	
#ifdef CONFIG_FANOTIFY
	atomic_t fanotify_listeners;
#endif
#ifdef CONFIG_EPOLL
	atomic_long_t epoll_watches; 
#endif
#ifdef CONFIG_POSIX_MQUEUE
	
	unsigned long mq_bytes;	
#endif
	unsigned long locked_shm; 
	unsigned long unix_inflight;	
	atomic_long_t pipe_bufs;  

	
	struct hlist_node uidhash_node;
	kuid_t uid;

#ifdef CONFIG_PACKAGE_RUNTIME_INFO
	struct package_runtime_info pkg;
#endif

#if defined(CONFIG_PERF_EVENTS) || defined(CONFIG_BPF_SYSCALL) || \
    defined(CONFIG_NET) || defined(CONFIG_IO_URING)
	atomic_long_t locked_vm;
#endif

	
	struct ratelimit_state ratelimit;

	ANDROID_KABI_RESERVE(1);
	ANDROID_KABI_RESERVE(2);
};

extern int uids_sysfs_init(void);

extern struct user_struct *find_user(kuid_t);

extern struct user_struct root_user;
#define INIT_USER (&root_user)



extern struct user_struct * alloc_uid(kuid_t);
static inline struct user_struct *get_uid(struct user_struct *u)
{
	refcount_inc(&u->__count);
	return u;
}
extern void free_uid(struct user_struct *);

#endif 
