/* Dream 5.4 manual stage: SUSFS installs its own mutually exclusive hooks. */
#ifndef _LINUX_ODIN_KSU_HOOKS_H
#define _LINUX_ODIN_KSU_HOOKS_H
#include <linux/types.h>
#include <linux/compiler_types.h>
struct filename;
#if defined(CONFIG_KSU_MANUAL_HOOK) && defined(CONFIG_KSU_SUSFS)
#error "KSU manual and SUSFS inline hooks must be mutually exclusive"
#endif
#ifdef CONFIG_KSU
int ksu_handle_execveat(int *fd, struct filename **filename, void *argv, void *envp, int *flags);
int ksu_handle_post_execve(int *fd, const char *filename, void *argv, void *envp, int *flags, int *retval);
void ksu_handle_sys_read(unsigned int fd);
void ksu_handle_vfs_fstat(int fd, loff_t *size);
#ifdef CONFIG_KSU_SUSFS
#define TIF_PROC_IN_KSU_EXECVE 61
int ksu_handle_faccessat(int *dfd, struct filename **filename, int *mode, int *flags);
int ksu_handle_stat(int *dfd, struct filename **filename, int *flags);
#else
int ksu_handle_faccessat(int *dfd, const char __user **filename, int *mode, int *flags);
int ksu_handle_stat(int *dfd, const char __user **filename, int *flags);
#endif
int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid);
int ksu_handle_input_handle_event(unsigned int *type, unsigned int *code, int *value);
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd, void __user **arg);
#endif
#endif
