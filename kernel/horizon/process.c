// SPDX-License-Identifier: GPL-2.0
/*
 * Horizon processes and threads: lifetime, handles and scheduling.
 *
 * Horizon threads are scheduled as SCHED_FIFO tasks: 64 Horizon priorities
 * map monotonically onto RT priorities 1..49, below threaded interrupt
 * handlers (50), so strict priority order and FIFO order within a level
 * come from the RT scheduler. Without CAP_SYS_NICE the RT priority is capped
 * at RLIMIT_RTPRIO, and with an RLIMIT_RTPRIO of 0 the threads stay
 * SCHED_NORMAL.
 *
 * A thread created by svcCreateThread runs at once, but only into a task
 * work that holds it in the kernel, killable and freezable, until
 * svcStartThread. Like any other thread it takes part in group exit, exec
 * and suspend meanwhile. svcSetThreadActivity pauses a thread the same way,
 * with a task work that runs before it returns to user space.
 */

#include <linux/anon_inodes.h>
#include <linux/cpumask.h>
#include <linux/poll.h>
#include <linux/ptrace.h>
#include <linux/sched/debug.h>
#include <linux/elf.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/horizon.h>
#include <linux/irq-entry-common.h>
#include <linux/mman.h>
#include <linux/pid.h>
#include <linux/regset.h>
#include <linux/resume_user_mode.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <uapi/linux/pidfd.h>
#include <uapi/linux/sched/types.h>
#include <asm/horizon/syscall.h>
#include <asm/insn.h>
#include <asm/processor.h>
#include <asm/spectre.h>

#include "internal.h"

#define IDEAL_CORE_DONT_CARE		((s32)-1)
#define IDEAL_CORE_USE_PROCESS_VALUE	((s32)-2)
#define IDEAL_CORE_NO_UPDATE		((s32)-3)

#define HZN_YIELD_WITHOUT_CORE_MIGRATION	0
#define HZN_YIELD_WITH_CORE_MIGRATION		(-1)
#define HZN_YIELD_TO_ANY_THREAD			(-2)

struct hzn_process *hzn_process_alloc(void)
{
	struct hzn_process *proc = kzalloc_obj(*proc);

	if (!proc)
		return NULL;
	kref_init(&proc->ref);
	mutex_init(&proc->mem_lock);
	INIT_LIST_HEAD(&proc->aliases);
	INIT_LIST_HEAD(&proc->locked);
	INIT_LIST_HEAD(&proc->object_maps);
	init_waitqueue_head(&proc->exception_wq);
	spin_lock_init(&proc->lock);
	INIT_LIST_HEAD(&proc->unstarted);
	rt_mutex_init(&proc->arb_lock);
	INIT_LIST_HEAD(&proc->lock_waiters);
	INIT_LIST_HEAD(&proc->cv_waiters);
	INIT_LIST_HEAD(&proc->addr_waiters);
	return proc;
}

static void hzn_process_release(struct kref *ref)
{
	struct hzn_process *proc = container_of(ref, struct hzn_process, ref);

	WARN_ON(!list_empty(&proc->unstarted));
	hzn_process_free_aliases(proc);
	hzn_process_free_object_maps(proc);
	hzn_process_free_locked(proc);
	if (proc->heap_file)
		fput(proc->heap_file);
	kfree(proc);
}

void hzn_process_put(struct hzn_process *proc)
{
	kref_put(&proc->ref, hzn_process_release);
}

struct hzn_thread *hzn_thread_alloc(struct hzn_process *proc)
{
	struct hzn_thread *thread = kzalloc_obj(*thread);

	if (!thread)
		return NULL;
	kref_get(&proc->ref);
	thread->proc = proc;
	thread->priority = HZN_LOWEST_THREAD_PRIORITY;
	thread->base_priority = HZN_LOWEST_THREAD_PRIORITY;
	thread->applied_priority = HZN_LOWEST_THREAD_PRIORITY;
	INIT_LIST_HEAD(&thread->donors);
	INIT_LIST_HEAD(&thread->unstarted_node);
	mutex_init(&thread->lock);
	return thread;
}

static void hzn_thread_free(struct hzn_thread *thread)
{
	hzn_process_put(thread->proc);
	kfree(thread);
}

/*
 * Handles are file descriptors of the Horizon process, offset by one so that
 * 0 stays the invalid handle.
 */

/* Installs @file in the current process and consumes the reference. */
u32 hzn_handle_add(struct file *file)
{
	int fd = get_unused_fd_flags(0);

	if (fd < 0) {
		fput(file);
		return HZN_INVALID_HANDLE;
	}
	fd_install(fd, file);
	return (u32)fd + 1;
}

/* Reserve both entries before publishing either endpoint; consumes both refs. */
long hzn_handle_add_pair(struct file *first, struct file *second,
			 u32 *first_handle, u32 *second_handle)
{
	int a = get_unused_fd_flags(0), b;

	if (a < 0)
		goto fail;
	b = get_unused_fd_flags(0);
	if (b < 0) {
		put_unused_fd(a);
		goto fail;
	}
	fd_install(a, first);
	fd_install(b, second);
	*first_handle = (u32)a + 1;
	*second_handle = (u32)b + 1;
	return HZN_RESULT_SUCCESS;
fail:
	fput(first);
	fput(second);
	return HZN_RESULT_OUT_OF_HANDLES;
}

struct file *hzn_handle_get(u32 handle)
{
	if (handle == HZN_INVALID_HANDLE || hzn_is_pseudo_handle(handle))
		return NULL;
	return fget_raw(handle - 1);
}

int hzn_handle_close(u32 handle)
{
	if (handle == HZN_INVALID_HANDLE || hzn_is_pseudo_handle(handle))
		return -EBADF;
	return close_fd(handle - 1);
}

/* Keep the identity alive after exit, while pidfd supplies exit polling. */
struct hzn_thread_handle {
	struct task_struct *task;
	struct pid *process;
	struct file *pidfd;
};

static __poll_t hzn_thread_handle_poll(struct file *file, poll_table *wait)
{
	struct hzn_thread_handle *handle = file->private_data;

	return vfs_poll(handle->pidfd, wait);
}

static int hzn_thread_handle_release(struct inode *inode, struct file *file)
{
	struct hzn_thread_handle *handle = file->private_data;

	fput(handle->pidfd);
	put_pid(handle->process);
	put_task_struct(handle->task);
	kfree(handle);
	return 0;
}

static const struct file_operations hzn_thread_handle_fops = {
	.poll = hzn_thread_handle_poll,
	.release = hzn_thread_handle_release,
};

static struct file *hzn_thread_file_prepare(struct task_struct *task, int *reserved_fd)
{
	struct hzn_thread_handle *handle;
	struct file *file;
	int fd;

	handle = kmalloc_obj(*handle);
	if (!handle)
		return ERR_PTR(-ENOMEM);
	fd = pidfd_prepare(task_pid(task), PIDFD_THREAD, &handle->pidfd);
	if (fd < 0) {
		kfree(handle);
		return ERR_PTR(fd);
	}
	file = anon_inode_getfile("[horizon thread]", &hzn_thread_handle_fops,
				 handle, O_RDWR);
	if (IS_ERR(file)) {
		put_unused_fd(fd);
		fput(handle->pidfd);
		kfree(handle);
		return file;
	}
	task->hzn_thread->id = task_pid_vnr(task);
	handle->task = get_task_struct(task);
	handle->process = get_pid(task_tgid(task));
	*reserved_fd = fd;
	return file;
}

/* Initialize TLS before making a newly created thread handle visible. */
static struct file *hzn_thread_handle_prepare(struct task_struct *task, int *fd)
{
	struct file *file = hzn_thread_file_prepare(task, fd);
	u32 handle;

	if (IS_ERR(file))
		return file;
	handle = (u32)*fd + 1;
	if (put_user(handle, (u32 __user *)(task->hzn_thread->tls +
					  HZN_TLS_THREAD_HANDLE))) {
		put_unused_fd(*fd);
		fput(file);
		return ERR_PTR(-EFAULT);
	}
	return file;
}

u32 hzn_thread_handle_create(struct task_struct *task)
{
	int fd;
	struct file *file = hzn_thread_handle_prepare(task, &fd);

	if (IS_ERR(file))
		return HZN_INVALID_HANDLE;
	task->hzn_thread->handle = (u32)fd + 1;
	fd_install(fd, file);
	return (u32)fd + 1;
}

struct file *hzn_copy_handle_file(u32 handle)
{
	struct file *file;
	int fd;

	if (!hzn_is_pseudo_handle(handle))
		return hzn_handle_get(handle);
	if (handle == HZN_PSEUDO_HANDLE_CURRENT_THREAD) {
		file = hzn_thread_file_prepare(current, &fd);
		if (IS_ERR(file))
			return NULL;
	} else {
		fd = pidfd_prepare(task_tgid(current), 0, &file);
		if (fd < 0)
			return NULL;
	}
	put_unused_fd(fd);
	return file;
}

long hzn_get_process_id(u32 handle, u64 *id)
{
	struct file *file;
	struct pid *pid;
	long ret = HZN_RESULT_INVALID_HANDLE;

	if (hzn_is_pseudo_handle(handle)) {
		*id = task_tgid_vnr(current);
		return HZN_RESULT_SUCCESS;
	}
	file = hzn_handle_get(handle);
	if (!file)
		return ret;
	if (file->f_op == &hzn_thread_handle_fops) {
		struct hzn_thread_handle *h = file->private_data;
		*id = pid_vnr(h->process);
		ret = HZN_RESULT_SUCCESS;
	} else {
		pid = pidfd_pid(file);
		if (!IS_ERR(pid)) {
			*id = pid_vnr(pid);
			ret = HZN_RESULT_SUCCESS;
		}
	}
	fput(file);
	return ret;
}

long hzn_get_thread_id(u32 handle, u64 *id)
{
	struct file *file;
	long ret = HZN_RESULT_INVALID_HANDLE;

	if (handle == HZN_PSEUDO_HANDLE_CURRENT_THREAD) {
		*id = hzn_current()->id;
		return HZN_RESULT_SUCCESS;
	}
	file = hzn_handle_get(handle);
	if (!file)
		return ret;
	if (file->f_op == &hzn_thread_handle_fops) {
		struct hzn_thread_handle *h = file->private_data;
		*id = pid_vnr(pidfd_pid(h->pidfd));
		ret = HZN_RESULT_SUCCESS;
	}
	fput(file);
	return ret;
}

/* Returns a reference to a thread of the current Horizon process, or NULL. */
struct task_struct *hzn_handle_to_thread(u32 handle)
{
	struct hzn_thread *self = hzn_current();
	struct task_struct *task;
	struct hzn_thread_handle *thread_handle;
	struct file *file;

	if (!self)
		return NULL;
	if (handle == HZN_PSEUDO_HANDLE_CURRENT_THREAD)
		return get_task_struct(current);

	file = hzn_handle_get(handle);
	if (!file)
		return NULL;
	thread_handle = file->private_data;
	task = file->f_op == &hzn_thread_handle_fops ?
		get_task_struct(thread_handle->task) : NULL;
	fput(file);
	if (task && (!task->hzn_thread || task->hzn_thread->proc != self->proc)) {
		put_task_struct(task);
		task = NULL;
	}
	return task;
}

/* Is @handle a real (not pseudo) handle of a thread of the current process? */
bool hzn_thread_exists(u32 handle)
{
	struct task_struct *task;

	if (hzn_is_pseudo_handle(handle))
		return false;
	task = hzn_handle_to_thread(handle);
	if (!task)
		return false;
	put_task_struct(task);
	return true;
}

bool hzn_valid_priority(s64 priority)
{
	return priority >= HZN_HIGHEST_THREAD_PRIORITY &&
	       priority <= HZN_LOWEST_THREAD_PRIORITY;
}

/* Is @core one of the cores of the process, and online? */
static bool hzn_valid_core(struct hzn_process *proc, s32 core)
{
	return core >= 0 && core < 64 && (proc->core_mask & BIT_ULL(core)) &&
	       cpu_online(core);
}

static int hzn_rt_priority(int priority)
{
	return 1 + (HZN_LOWEST_THREAD_PRIORITY - priority) * 48 /
		   HZN_LOWEST_THREAD_PRIORITY;
}

/* Permission checks are made against the current task, as for sched_setscheduler(2). */
void hzn_apply_priority(struct task_struct *p, int priority)
{
	struct sched_param param = { .sched_priority = hzn_rt_priority(priority) };
	unsigned long limit;

	if (p->hzn_thread) {
		WRITE_ONCE(p->hzn_thread->priority, priority);
		p->hzn_thread->applied_priority = priority;
	}
	if (!sched_setscheduler(p, SCHED_FIFO, &param))
		return;
	limit = task_rlimit(p, RLIMIT_RTPRIO);
	if (limit && limit < param.sched_priority) {
		param.sched_priority = limit;
		if (!sched_setscheduler(p, SCHED_FIFO, &param))
			return;
	}
	param.sched_priority = 0;
	if (p->policy != SCHED_NORMAL)
		sched_setscheduler_nocheck(p, SCHED_NORMAL, &param);
}

/*
 * svcSleepThread with a 0, -1 or -2 timeout: the yield user space asked for,
 * as sched_yield(2) does it.
 */
void hzn_yield(s64 type)
{
	struct sched_param param = { .sched_priority = 0 };
	int rt_priority = current->rt_priority;

	if (type != HZN_YIELD_TO_ANY_THREAD || current->policy != SCHED_FIFO) {
		yield();
		return;
	}
	/* Let lower priority threads run as well, then take the priority back. */
	sched_setscheduler_nocheck(current, SCHED_NORMAL, &param);
	yield();
	param.sched_priority = rt_priority;
	sched_setscheduler_nocheck(current, SCHED_FIFO, &param);
}

/* Maps a TLS page for a new thread just above the existing ones. */
static unsigned long hzn_alloc_tls(struct hzn_process *proc)
{
	unsigned long addr, off;

	for (off = PAGE_SIZE; off < HZN_TLS_AREA_SIZE; off += PAGE_SIZE) {
		addr = vm_mmap(NULL, proc->tls_base + off, PAGE_SIZE,
			       PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
		if (addr != (unsigned long)-EEXIST)
			return addr;
	}
	return (unsigned long)-ENOMEM;
}

bool horizon_defer_task_work(void)
{
	return current->hzn_thread &&
		test_bit(HZN_THREAD_CHECKING_SIGNALS, &current->hzn_thread->flags);
}

/* Called with no spinlock held: stop for ptrace without unwinding an SVC. */
bool hzn_check_signals(void)
{
	struct pt_regs *regs;
	int syscall;

	/* Leave queued work for the outer task_work_run(), without recursion. */
	if (test_thread_flag(TIF_NOTIFY_SIGNAL)) {
		set_notify_resume(current);
		clear_notify_signal();
	}
	if (!task_sigpending(current))
		return false;
	__set_current_state(TASK_RUNNING);
	if (fatal_signal_pending(current))
		return true;
	regs = task_pt_regs(current);
	/* Horizon waits keep their kernel state; never restart them as Linux SVCs. */
	syscall = regs->syscallno;
	forget_syscall(regs);
	set_bit(HZN_THREAD_CHECKING_SIGNALS, &current->hzn_thread->flags);
	arch_do_signal_or_restart(regs);
	clear_bit(HZN_THREAD_CHECKING_SIGNALS, &current->hzn_thread->flags);
	regs->syscallno = syscall;
	return fatal_signal_pending(current);
}

/* Wait for StartThread after allowing the initial ptrace stop to complete. */
static void hzn_thread_start_work(struct callback_head *work)
{
	struct hzn_thread *thread = container_of(work, struct hzn_thread,
						 start_work);
	int state = HZN_THREAD_CREATED;

	/* Killed before it ever ran: this is exit_task_work(). */
	if (current->flags & PF_EXITING)
		return;
	for (;;) {
		if (hzn_check_signals())
			break;
		set_current_state(HZN_WAIT_STATE);
		state = READ_ONCE(thread->state);
		if (state != HZN_THREAD_CREATED)
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	if (state == HZN_THREAD_DISCARDED)
		do_exit(0);
}

long hzn_create_thread(unsigned long entry, unsigned long arg,
		       unsigned long stack_top, s32 priority, s32 core,
		       u32 *handle)
{
	struct hzn_thread *self = hzn_current();
	struct hzn_process *proc = self->proc;
	struct kernel_clone_args args = {
		.flags		= CLONE_VM | CLONE_FS | CLONE_FILES |
				  CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM,
		.exit_signal	= 0,
		.stack		= stack_top,
	};
	struct hzn_thread *thread;
	struct pt_regs *regs;
	struct task_struct *p;
	struct file *file;
	struct pid *pid = NULL;
	int fd, trace;
	unsigned long tls;
	u32 h;

	if (core == IDEAL_CORE_USE_PROCESS_VALUE)
		core = proc->ideal_core;
	if (!hzn_valid_core(proc, core))
		return HZN_RESULT_INVALID_CORE_ID;
	if (!hzn_valid_priority(priority))
		return HZN_RESULT_INVALID_PRIORITY;

	tls = hzn_alloc_tls(proc);
	if (IS_ERR_VALUE(tls))
		return HZN_RESULT_OUT_OF_MEMORY;

	thread = hzn_thread_alloc(proc);
	if (!thread) {
		vm_munmap(tls, PAGE_SIZE);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	thread->priority = priority;
	thread->base_priority = priority;
	thread->tls = tls;
	thread->ideal_core = core;
	thread->affinity = BIT_ULL(core);

	trace = ptrace_event_enabled(current, PTRACE_EVENT_CLONE) ?
		PTRACE_EVENT_CLONE : 0;
	p = copy_process(NULL, trace, cpu_to_node(core), &args);
	if (IS_ERR(p)) {
		hzn_thread_free(thread);
		vm_munmap(tls, PAGE_SIZE);
		return HZN_RESULT_OUT_OF_RESOURCE;
	}

	/* p is TASK_NEW: nothing runs it before wake_up_new_task(). */
	thread->task = p;
	p->hzn_thread = thread;
	set_tsk_thread_flag(p, TIF_HORIZON);
	regs = task_pt_regs(p);
	start_thread_common(regs, entry, PSR_MODE_EL0t);
	forget_syscall(regs);
	regs->sp = stack_top;
	regs->regs[0] = arg;
	spectre_v4_enable_task_mitigation(p);
	p->thread.uw.tp_value = tls;
	p->thread.uw.tp2_value = 0;
	set_cpus_allowed_ptr(p, cpumask_of(core));
	hzn_apply_priority(p, priority);

	file = hzn_thread_handle_prepare(p, &fd);
	h = IS_ERR(file) ? HZN_INVALID_HANDLE : (u32)fd + 1;
	spin_lock(&proc->lock);
	if (h == HZN_INVALID_HANDLE) {
		thread->state = HZN_THREAD_DISCARDED;
	} else {
		thread->handle = h;
		list_add_tail(&thread->unstarted_node, &proc->unstarted);
	}
	spin_unlock(&proc->lock);

	init_task_work(&thread->start_work, hzn_thread_start_work);
	WARN_ON(task_work_add(p, &thread->start_work, TWA_RESUME));
	/* Match kernel_clone(): hold the PID through wakeup and the clone event. */
	if (trace)
		pid = get_pid(task_pid(p));
	wake_up_new_task(p);
	/* Other threads may use the handle as soon as fd_install() publishes it. */
	if (!IS_ERR(file))
		fd_install(fd, file);
	if (trace) {
		ptrace_event_pid(trace, pid);
		put_pid(pid);
	}

	if (IS_ERR(file)) {
		if (PTR_ERR(file) == -EFAULT)
			return HZN_RESULT_INVALID_CURRENT_MEMORY;
		if (PTR_ERR(file) == -ENOMEM)
			return HZN_RESULT_OUT_OF_MEMORY;
		return HZN_RESULT_OUT_OF_HANDLES;
	}
	*handle = h;
	return HZN_RESULT_SUCCESS;
}

long hzn_start_thread(u32 handle)
{
	struct task_struct *p = hzn_handle_to_thread(handle);
	long ret = HZN_RESULT_INVALID_STATE;
	struct hzn_thread *thread;
	struct hzn_process *proc;

	if (!p)
		return HZN_RESULT_INVALID_HANDLE;
	thread = p->hzn_thread;
	proc = thread->proc;

	spin_lock(&proc->lock);
	if (thread->state == HZN_THREAD_CREATED) {
		WRITE_ONCE(thread->state, HZN_THREAD_STARTED);
		list_del_init(&thread->unstarted_node);
		proc->nr_running++;
		wake_up_process(p);
		ret = HZN_RESULT_SUCCESS;
	}
	spin_unlock(&proc->lock);
	put_task_struct(p);
	return ret;
}

long hzn_get_core_mask(u32 handle, s32 *ideal_core, u64 *affinity)
{
	struct task_struct *p = hzn_handle_to_thread(handle);
	struct hzn_thread *thread;

	if (!p)
		return HZN_RESULT_INVALID_HANDLE;
	thread = p->hzn_thread;
	mutex_lock(&thread->lock);
	*ideal_core = thread->ideal_core;
	*affinity = thread->affinity;
	mutex_unlock(&thread->lock);
	put_task_struct(p);
	return HZN_RESULT_SUCCESS;
}

/* The affinity mask has to be within the core mask of the process. */
static long hzn_check_core_mask(struct hzn_process *proc, s32 ideal_core,
				u64 affinity)
{
	if ((affinity | proc->core_mask) != proc->core_mask)
		return HZN_RESULT_INVALID_CORE_ID;
	if (!affinity)
		return HZN_RESULT_INVALID_COMBINATION;
	if (ideal_core >= 0 && ideal_core < 64)
		return affinity & BIT_ULL(ideal_core) ?
		       HZN_RESULT_SUCCESS : HZN_RESULT_INVALID_COMBINATION;
	if (ideal_core != IDEAL_CORE_NO_UPDATE &&
	    ideal_core != IDEAL_CORE_DONT_CARE)
		return HZN_RESULT_INVALID_CORE_ID;
	return HZN_RESULT_SUCCESS;
}

long hzn_set_core_mask(u32 handle, s32 ideal_core, u64 affinity)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct task_struct *p = hzn_handle_to_thread(handle);
	struct hzn_thread *thread;
	cpumask_var_t mask;
	long ret;
	int cpu;

	if (!p)
		return HZN_RESULT_INVALID_HANDLE;
	thread = p->hzn_thread;
	if (ideal_core == IDEAL_CORE_USE_PROCESS_VALUE) {
		ideal_core = proc->ideal_core;
		affinity = BIT_ULL(ideal_core);
	}
	ret = hzn_check_core_mask(proc, ideal_core, affinity);
	if (ret != HZN_RESULT_SUCCESS)
		goto out;
	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		ret = HZN_RESULT_OUT_OF_MEMORY;
		goto out;
	}
	for (cpu = 0; cpu < min_t(int, nr_cpu_ids, 64); cpu++)
		if (affinity & BIT_ULL(cpu))
			cpumask_set_cpu(cpu, mask);

	mutex_lock(&thread->lock);
	if (ideal_core == IDEAL_CORE_NO_UPDATE) {
		ideal_core = thread->ideal_core;
		if (ideal_core >= 0 && !(affinity & BIT_ULL(ideal_core)))
			ret = HZN_RESULT_INVALID_COMBINATION;
	}
	if (ret == HZN_RESULT_SUCCESS && set_cpus_allowed_ptr(p, mask))
		ret = HZN_RESULT_INVALID_CORE_ID;
	if (ret == HZN_RESULT_SUCCESS) {
		thread->ideal_core = ideal_core;
		thread->affinity = affinity;
	}
	mutex_unlock(&thread->lock);
	free_cpumask_var(mask);
out:
	put_task_struct(p);
	return ret;
}

/*
 * svcCancelSynchronization: ends the thread's svcWaitSynchronization or
 * svcReplyAndReceive, or the next one it starts, with Cancelled.
 */
long hzn_cancel_synchronization(u32 handle)
{
	struct task_struct *p = hzn_handle_to_thread(handle);

	if (!p)
		return HZN_RESULT_INVALID_HANDLE;
	set_bit(HZN_THREAD_WAIT_CANCELLED, &p->hzn_thread->flags);
	/* Those waits are interruptible; the flag is seen before it sleeps. */
	wake_up_state(p, TASK_INTERRUPTIBLE);
	put_task_struct(p);
	return HZN_RESULT_SUCCESS;
}

bool hzn_wait_cancelled(bool clear)
{
	unsigned long *flags = &hzn_current()->flags;

	if (clear)
		return test_and_clear_bit(HZN_THREAD_WAIT_CANCELLED, flags);
	return test_bit(HZN_THREAD_WAIT_CANCELLED, flags);
}

enum {
	HZN_THREAD_ACTIVITY_RUNNABLE,
	HZN_THREAD_ACTIVITY_PAUSED,
};

/*
 * A paused thread runs this on its way to user space, and waits here until
 * it is resumed: whatever SVC it was in still completes, but no user code
 * runs. Like any other Horizon wait it is killable and freezable.
 */
static void hzn_thread_pause_work(struct callback_head *work)
{
	struct hzn_thread *thread = container_of(work, struct hzn_thread,
						 pause_work);

	clear_bit(HZN_THREAD_PAUSE_QUEUED, &thread->flags);
	smp_mb__after_atomic();		/* see hzn_pause() */
	if (current->flags & PF_EXITING)
		return;
	WRITE_ONCE(thread->parked, true);
	for (;;) {
		if (hzn_check_signals())
			break;
		set_current_state(HZN_WAIT_STATE);
		if (!READ_ONCE(thread->paused))
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	WRITE_ONCE(thread->parked, false);
}

static long hzn_pause(struct task_struct *p, struct hzn_thread *thread)
{
	WRITE_ONCE(thread->paused, true);
	/* Ordered with the clearing in hzn_thread_pause_work() by the RMW. */
	if (!test_and_set_bit(HZN_THREAD_PAUSE_QUEUED, &thread->flags)) {
		init_task_work(&thread->pause_work, hzn_thread_pause_work);
		/* With a signal kick, so that it stops running user code. */
		if (task_work_add(p, &thread->pause_work, TWA_SIGNAL)) {
			clear_bit(HZN_THREAD_PAUSE_QUEUED, &thread->flags);
			WRITE_ONCE(thread->paused, false);
			return HZN_RESULT_INVALID_STATE;	/* exiting */
		}
	}
	/*
	 * As on Horizon, return once it no longer runs. A thread that is not
	 * on a CPU has its user registers saved, and cannot run user code
	 * any more before it parks. Stop waiting if we are to be paused (or
	 * signalled) ourselves, or two threads pausing each other would spin.
	 */
	while (task_curr(p) && !READ_ONCE(thread->parked) &&
	       !signal_pending(current)) {
		cond_resched();
		cpu_relax();
	}
	return HZN_RESULT_SUCCESS;
}

static void hzn_resume(struct task_struct *p, struct hzn_thread *thread)
{
	WRITE_ONCE(thread->paused, false);
	smp_mb();	/* pairs with set_current_state() in the pause work */
	if (READ_ONCE(thread->parked))
		wake_up_process(p);
}

long hzn_set_thread_activity(u32 handle, u32 activity)
{
	struct hzn_thread *thread;
	struct task_struct *p;
	long ret;
	int state;

	if (activity > HZN_THREAD_ACTIVITY_PAUSED)
		return HZN_RESULT_INVALID_ENUM_VALUE;
	p = hzn_handle_to_thread(handle);
	if (!p)
		return HZN_RESULT_INVALID_HANDLE;
	if (p == current) {
		put_task_struct(p);
		return HZN_RESULT_BUSY;
	}
	thread = p->hzn_thread;

	mutex_lock(&thread->lock);
	spin_lock(&thread->proc->lock);
	state = thread->state;
	spin_unlock(&thread->proc->lock);
	/* Only started threads can be paused, and only paused ones resumed. */
	if (state != HZN_THREAD_STARTED || (p->flags & PF_EXITING) ||
	    thread->paused == (activity == HZN_THREAD_ACTIVITY_PAUSED)) {
		ret = HZN_RESULT_INVALID_STATE;
	} else if (activity == HZN_THREAD_ACTIVITY_PAUSED) {
		ret = hzn_pause(p, thread);
	} else {
		hzn_resume(p, thread);
		ret = HZN_RESULT_SUCCESS;
	}
	mutex_unlock(&thread->lock);
	put_task_struct(p);
	return ret;
}

/* svc::ThreadContext (64-bit) */
struct hzn_thread_context {
	u64 r[29];
	u64 fp, lr, sp, pc;
	u32 pstate, padding;
	__uint128_t v[32];
	u32 fpcr, fpsr;
	u64 tpidr;
};
static_assert(sizeof(struct hzn_thread_context) == 0x320);

/*
 * The user context of a paused thread @p, as Horizon reports it: a thread
 * that is in an SVC has the address of the svc instruction as its PC, and
 * only its callee-saved registers are meaningful.
 */
static void hzn_fill_context(struct task_struct *p,
			     struct hzn_thread_context *ctx,
			     struct user_fpsimd_state *fp)
{
	const struct user_regset_view *view = task_user_regset_view(p);
	struct pt_regs *regs = task_pt_regs(p);
	bool in_svc = in_syscall(regs);
	int i, first = in_svc ? 19 : 0;

	for (i = first; i < ARRAY_SIZE(ctx->r); i++)
		ctx->r[i] = regs->regs[i];
	ctx->fp = regs->regs[29];
	ctx->lr = regs->regs[30];
	ctx->sp = regs->sp;
	ctx->pc = regs->pc - (in_svc ? AARCH64_INSN_SIZE : 0);
	ctx->pstate = regs->pstate & (PSR_N_BIT | PSR_Z_BIT | PSR_C_BIT |
				      PSR_V_BIT);
	ctx->tpidr = p->thread.uw.tp2_value;	/* TPIDR_EL0, see task_user_tls() */

	for (i = 0; i < view->n; i++) {
		if (view->regsets[i].core_note_type != NT_PRFPREG)
			continue;
		if (regset_get(p, &view->regsets[i], sizeof(*fp), fp) != sizeof(*fp))
			return;
		/* v8..v15 are the callee-saved ones. */
		first = in_svc ? 8 : 0;
		memcpy(&ctx->v[first], &fp->vregs[first],
		       (in_svc ? 8 : 32) * sizeof(ctx->v[0]));
		ctx->fpcr = fp->fpcr;
		ctx->fpsr = fp->fpsr;
		return;
	}
}

struct hzn_context_snapshot {
	struct hzn_thread_context ctx;
	struct user_fpsimd_state fp;
};

/* task_call_func holds off wakeups and context switches. No allocations here. */
static int hzn_snapshot_context(struct task_struct *p, void *data)
{
	struct hzn_context_snapshot *buf = data;

	if (task_curr(p) || READ_ONCE(p->on_cpu))
		return -EAGAIN;
	if (!(p->flags & PF_EXITING))
		hzn_fill_context(p, &buf->ctx, &buf->fp);
	return 0;
}

long hzn_get_thread_context(void __user *uctx, u32 handle)
{
	struct hzn_context_snapshot *buf;
	struct hzn_thread *thread;
	struct task_struct *p;
	long ret = HZN_RESULT_SUCCESS;

	p = hzn_handle_to_thread(handle);
	if (!p)
		return HZN_RESULT_INVALID_HANDLE;
	if (!access_ok(uctx, sizeof(buf->ctx))) {
		ret = HZN_RESULT_INVALID_POINTER;
		goto out;
	}
	if (p == current) {
		ret = HZN_RESULT_BUSY;
		goto out;
	}
	buf = kzalloc_obj(*buf);
	if (!buf) {
		ret = HZN_RESULT_OUT_OF_MEMORY;
		goto out;
	}
	thread = p->hzn_thread;
	mutex_lock(&thread->lock);
	if (!thread->paused)
		ret = HZN_RESULT_INVALID_STATE;
	else if (try_get_task_stack(p)) {
		while (task_call_func(p, hzn_snapshot_context, buf) == -EAGAIN) {
			ktime_t delay = ns_to_ktime(50000);

			if (fatal_signal_pending(current)) {
				ret = HZN_RESULT_TERMINATION_REQUESTED;
				break;
			}
			set_current_state(TASK_KILLABLE);
			schedule_hrtimeout(&delay, HRTIMER_MODE_REL);
		}
		put_task_stack(p);
	} /* An exited thread whose stack is gone has an all-zero context. */
	mutex_unlock(&thread->lock);
	if (ret == HZN_RESULT_SUCCESS &&
	    copy_to_user(uctx, &buf->ctx, sizeof(buf->ctx)))
		ret = HZN_RESULT_INVALID_POINTER;
	kfree(buf);
out:
	put_task_struct(p);
	return ret;
}

static void hzn_thread_exit(struct task_struct *tsk)
{
	struct hzn_thread *thread = tsk->hzn_thread, *t, *tmp;
	struct hzn_process *proc = thread->proc;
	bool last;

	spin_lock(&proc->lock);
	if (thread->state == HZN_THREAD_STARTED)
		proc->nr_running--;
	list_del_init(&thread->unstarted_node);
	thread->state = HZN_THREAD_EXITED;
	/* The process ends with its last thread: never start the others. */
	last = !proc->nr_running;
	if (last) {
		list_for_each_entry_safe(t, tmp, &proc->unstarted, unstarted_node) {
			list_del_init(&t->unstarted_node);
			WRITE_ONCE(t->state, HZN_THREAD_DISCARDED);
			wake_up_process(t->task);
		}
	}
	spin_unlock(&proc->lock);
	if (last) {
		mutex_lock(&proc->mem_lock);
		hzn_process_free_object_maps(proc);
		mutex_unlock(&proc->mem_lock);
	}
	/* Let a thread waiting to handle its own exception have its turn. */
	hzn_release_exception(thread);
}

/* Called from copy_process(): children start out as ordinary Linux tasks. */
void horizon_fork(struct task_struct *p)
{
	p->hzn_thread = NULL;
	p->hzn_service = NULL;
	p->hzn_in_execve = 0;
	clear_tsk_thread_flag(p, TIF_HORIZON);
}

/* Called early in do_exit(), after PF_EXITING is set. */
void horizon_exit(struct task_struct *tsk)
{
	struct hzn_thread *thread = tsk->hzn_thread;

	if (tsk->hzn_service)
		hzn_service_exit(tsk);
	if (!thread)
		return;
	hzn_thread_exit(tsk);
	/* A thread that exits on its own gives its thread-local page back. */
	if (tsk->mm && !(tsk->signal->flags & SIGNAL_GROUP_EXIT) &&
	    !tsk->signal->group_exec_task)
		vm_munmap(thread->tls, PAGE_SIZE);
}

/* Called from __put_task_struct(). */
void horizon_free_task(struct task_struct *tsk)
{
	if (tsk->hzn_thread) {
		hzn_thread_free(tsk->hzn_thread);
		tsk->hzn_thread = NULL;
	}
	if (tsk->hzn_service) {
		hzn_service_free(tsk->hzn_service);
		tsk->hzn_service = NULL;
	}
}

/* From arch_setup_new_exec(): the new image is not a Horizon one (yet). */
void horizon_exec_reset(struct task_struct *tsk)
{
	struct hzn_thread *thread = tsk->hzn_thread;

	clear_tsk_thread_flag(tsk, TIF_HORIZON);
	if (!thread)
		return;
	hzn_thread_exit(tsk);
	tsk->hzn_thread = NULL;
	hzn_thread_free(thread);
}
