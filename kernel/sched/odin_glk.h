/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ODIN_GLK_H
#define _ODIN_GLK_H
struct cpufreq_policy;
/* Matches Dream stock include/linux/sched/cpufreq.h. */
#define SCHED_CPUFREQ_GLK (1U << 9)
unsigned long glk_cal_freq(struct cpufreq_policy *policy,
		unsigned long util, unsigned long max);
unsigned int glk_freq_limit(struct cpufreq_policy *policy, unsigned int *freq);
int create_game_load_proc(void *rootdir);
void delete_game_load_proc(void);
#endif
