/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Hooks the core kernel calls for Horizon (Nintendo Switch) tasks.
 * Everything else lives in kernel/horizon/.
 */
#ifndef _LINUX_HORIZON_H
#define _LINUX_HORIZON_H

#include <linux/types.h>

struct task_struct;

#ifdef CONFIG_HORIZON
void horizon_fork(struct task_struct *p);
void horizon_exit(struct task_struct *tsk);
void horizon_free_task(struct task_struct *tsk);
void horizon_exec_reset(struct task_struct *tsk);
void horizon_update_cntkctl(void);
bool horizon_defer_task_work(void);
bool horizon_defer_exit(int exit_code);
bool horizon_user_exception(int signo, int code, unsigned long far);
#else
static inline void horizon_fork(struct task_struct *p) { }
static inline void horizon_exit(struct task_struct *tsk) { }
static inline void horizon_free_task(struct task_struct *tsk) { }
static inline void horizon_exec_reset(struct task_struct *tsk) { }
static inline bool horizon_defer_task_work(void) { return false; }
static inline bool horizon_defer_exit(int exit_code) { return false; }
static inline bool horizon_user_exception(int signo, int code,
					  unsigned long far)
{
	return false;
}
#endif

#endif /* _LINUX_HORIZON_H */
