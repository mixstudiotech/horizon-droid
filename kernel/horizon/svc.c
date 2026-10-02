// SPDX-License-Identifier: GPL-2.0
/*
 * Horizon SVC entry points. "svc #imm" from a Horizon thread is dispatched
 * here through horizon_sys_call_table: the result code goes in x0 and an
 * output value, if any, in x1.
 */

#include <linux/cred.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/kernel_stat.h>
#include <linux/pid.h>
#include <linux/poll.h>
#include <linux/ptrace.h>
#include <linux/random.h>
#include <linux/sched/cputime.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/user_namespace.h>
#include <clocksource/arm_arch_timer.h>
#include <asm/horizon/syscall.h>
#include <asm/sysreg.h>

#include "internal.h"

#define ARGUMENT_HANDLE_COUNT_MAX	0x40

/* svcBreak reasons with this bit only inform a debugger. */
#define HZN_BREAK_NOTIFICATION_ONLY	0x80000000u

asmlinkage long __arm64_hsys_ni_syscall(const struct pt_regs *regs)
{
	pr_warn_ratelimited("horizon: %s[%d]: svc 0x%x is not implemented\n",
			    current->comm, task_pid_nr(current), regs->syscallno);
	return HZN_RESULT_UNKNOWN;
}

HSYSCALL_DEFINE2(set_heap_size, long, __unused, u64, size)
{
	unsigned long addr;
	long ret = hzn_set_heap_size(size, &addr);

	if (ret == HZN_RESULT_SUCCESS)
		HSYSCALL_OUT(addr);
	return ret;
}

HSYSCALL_DEFINE3(set_memory_permission, unsigned long, addr, u64, size,
		 u32, perm)
{
	return hzn_set_memory_permission(addr, size, perm);
}

HSYSCALL_DEFINE4(set_memory_attribute, unsigned long, addr, u64, size,
		 u32, mask, u32, value)
{
	return hzn_set_memory_attribute(addr, size, mask, value);
}

HSYSCALL_DEFINE3(map_memory, unsigned long, dst, unsigned long, src, u64, size)
{
	return hzn_map_memory(dst, src, size);
}

HSYSCALL_DEFINE3(unmap_memory, unsigned long, dst, unsigned long, src, u64, size)
{
	return hzn_unmap_memory(dst, src, size);
}

HSYSCALL_DEFINE3(query_memory, void __user *, info, long, __unused,
		 unsigned long, addr)
{
	struct memory_info mi;

	hzn_query_memory(&mi, addr);
	if (put_user(mi.addr, (u64 __user *)(info + 0x00)) ||
	    put_user(mi.size, (u64 __user *)(info + 0x08)) ||
	    put_user((u32)mi.state & 0xff, (u32 __user *)(info + 0x10)) ||
	    put_user((u32)mi.attr, (u32 __user *)(info + 0x14)) ||
	    put_user((u32)mi.perm, (u32 __user *)(info + 0x18)) ||
	    put_user(mi.ipc_refcount, (u32 __user *)(info + 0x1c)) ||
	    put_user(mi.device_refcount, (u32 __user *)(info + 0x20)) ||
	    put_user(0, (u32 __user *)(info + 0x24)))
		return HZN_RESULT_INVALID_ADDRESS;
	HSYSCALL_OUT(0);	/* page info */
	return HZN_RESULT_SUCCESS;
}

HSYSCALL_DEFINE0(exit_process)
{
	do_group_exit(0);
}

HSYSCALL_DEFINE6(create_thread, long, __unused, unsigned long, entry,
		 unsigned long, arg, unsigned long, stack_top,
		 s32, priority, s32, core)
{
	u32 handle;
	long ret = hzn_create_thread(entry, arg, stack_top, priority, core,
				     &handle);

	if (ret == HZN_RESULT_SUCCESS)
		HSYSCALL_OUT(handle);
	return ret;
}

HSYSCALL_DEFINE1(start_thread, u32, handle)
{
	return hzn_start_thread(handle);
}

HSYSCALL_DEFINE0(exit_thread)
{
	do_exit(0);
}

/* Debugger stops preserve the absolute sleep deadline and the pending SVC. */
static void hzn_sleep(s64 ns)
{
	ktime_t expires = ktime_add_safe(ktime_get(), ns_to_ktime(ns));

	while (!hzn_check_signals()) {
		set_current_state(HZN_WAIT_STATE);
		if (!schedule_hrtimeout_range(&expires, current->timer_slack_ns,
					      HRTIMER_MODE_ABS))
			break;
	}
	__set_current_state(TASK_RUNNING);
}

HSYSCALL_DEFINE1(sleep_thread, s64, ns)
{
	if (ns > 0)
		hzn_sleep(ns);
	else if (ns >= -2)	/* 0, -1 and -2 are the three kinds of yield */
		hzn_yield(ns);
	return HZN_RESULT_SUCCESS;
}

HSYSCALL_DEFINE2(get_thread_priority, long, __unused, u32, handle)
{
	struct task_struct *task = hzn_handle_to_thread(handle);

	if (!task)
		return HZN_RESULT_INVALID_HANDLE;
	HSYSCALL_OUT(task->hzn_thread->priority);
	put_task_struct(task);
	return HZN_RESULT_SUCCESS;
}

HSYSCALL_DEFINE2(set_thread_priority, u32, handle, u32, priority)
{
	struct task_struct *task;

	if (!hzn_valid_priority(priority))
		return HZN_RESULT_INVALID_PRIORITY;
	task = hzn_handle_to_thread(handle);
	if (!task)
		return HZN_RESULT_INVALID_HANDLE;
	hzn_set_priority(task, priority);
	put_task_struct(task);
	return HZN_RESULT_SUCCESS;
}

HSYSCALL_DEFINE3(get_thread_core_mask, long, __unused1, long, __unused2,
		 u32, handle)
{
	u64 affinity;
	s32 core;
	long ret = hzn_get_core_mask(handle, &core, &affinity);

	if (ret == HZN_RESULT_SUCCESS) {
		HSYSCALL_OUT((u32)core);
		HSYSCALL_OUTN(2, affinity);
	}
	return ret;
}

HSYSCALL_DEFINE3(set_thread_core_mask, u32, handle, s32, core, u64, affinity)
{
	return hzn_set_core_mask(handle, core, affinity);
}

HSYSCALL_DEFINE0(get_current_processor_number)
{
	return raw_smp_processor_id();
}

/* Resets an event (an eventfd) by reading its counter down to zero. */
static long hzn_clear_event(u32 handle, bool reset)
{
	struct file *file = hzn_handle_get(handle);
	struct eventfd_ctx *ctx;
	struct iov_iter iter;
	struct kiocb kiocb;
	struct kvec kv;
	u64 value;
	long ret;
	int i;

	if (!file)
		return HZN_RESULT_INVALID_HANDLE;
	if (hzn_clear_native_event(file, reset, &ret)) {
		fput(file);
		return ret;
	}
	ctx = eventfd_ctx_fileget(file);
	if (IS_ERR(ctx)) {
		fput(file);
		return HZN_RESULT_INVALID_HANDLE;
	}
	eventfd_ctx_put(ctx);

	/* One read clears it, or decrements a semaphore-mode eventfd. */
	for (i = 0; i < 64; i++) {
		kv.iov_base = &value;
		kv.iov_len = sizeof(value);
		iov_iter_kvec(&iter, ITER_DEST, &kv, 1, sizeof(value));
		init_sync_kiocb(&kiocb, file);
		kiocb.ki_flags |= IOCB_NOWAIT;
		if (vfs_iocb_iter_read(file, &kiocb, &iter) <= 0)
			break;
	}
	fput(file);
	return HZN_RESULT_SUCCESS;
}

/* Signals an event (an eventfd): adds one to its counter. */
HSYSCALL_DEFINE1(clear_event, u32, handle)
{
	return hzn_clear_event(handle, false);
}

HSYSCALL_DEFINE1(reset_signal, u32, handle)
{
	return hzn_clear_event(handle, true);
}

HSYSCALL_DEFINE1(signal_event, u32, handle)
{
	return hzn_signal_event(handle);
}

HSYSCALL_DEFINE0(create_event)
{
	u32 writable, readable;
	long ret = hzn_create_event(&writable, &readable);

	if (ret == HZN_RESULT_SUCCESS) {
		HSYSCALL_OUT(writable);
		HSYSCALL_OUTN(2, readable);
	}
	return ret;
}

HSYSCALL_DEFINE4(map_shared_memory, u32, handle, unsigned long, addr,
		 u64, size, u32, perm)
{
	return hzn_map_shared_memory(handle, addr, size, perm);
}

HSYSCALL_DEFINE3(unmap_shared_memory, u32, handle, unsigned long, addr,
		 u64, size)
{
	return hzn_unmap_shared_memory(handle, addr, size);
}

HSYSCALL_DEFINE4(create_transfer_memory, long, __unused, unsigned long, addr,
		 u64, size, u32, perm)
{
	u32 handle;
	long ret = hzn_create_transfer_memory(addr, size, perm, &handle);

	if (ret == HZN_RESULT_SUCCESS)
		HSYSCALL_OUT(handle);
	return ret;
}

HSYSCALL_DEFINE1(close_handle, u32, handle)
{
	if (!hzn_is_pseudo_handle(handle) && hzn_handle_close(handle))
		return HZN_RESULT_INVALID_HANDLE;
	return HZN_RESULT_SUCCESS;
}

/*
 * Waits until one of the handles is signalled: an event (eventfd) is
 * readable, a thread (pidfd) has exited. Debugger stops preserve the poll
 * registrations and absolute deadline. svcCancelSynchronization
 * ends it, unless a handle is signalled or the time-out is 0.
 */
static long hzn_wait_synchronization(u32 __user *uhandles, s32 num,
				     s64 timeout, s32 *index)
{
	u32 handles[ARGUMENT_HANDLE_COUNT_MAX];
	struct file *files[ARGUMENT_HANDLE_COUNT_MAX];
	struct poll_wqueues table;
	ktime_t expires = 0;
	long ret = HZN_RESULT_SUCCESS;
	s32 i, signalled = -1;

	if (num < 0 || num > ARGUMENT_HANDLE_COUNT_MAX)
		return HZN_RESULT_OUT_OF_RANGE;
	if (num && copy_from_user(handles, uhandles, num * sizeof(u32)))
		return HZN_RESULT_INVALID_POINTER;
	for (i = 0; i < num; i++) {
		files[i] = hzn_handle_get(handles[i]);
		if (!files[i] || !file_can_poll(files[i])) {
			pr_warn_ratelimited("horizon: wait on unsupported handle 0x%x\n",
					    handles[i]);
			if (files[i])
				fput(files[i]);
			while (i)
				fput(files[--i]);
			return HZN_RESULT_INVALID_HANDLE;
		}
	}

	if (timeout > 0)
		expires = ktime_add_safe(ktime_get(), ns_to_ktime(timeout));
	poll_initwait(&table);
	for (;;) {
		for (i = 0; i < num; i++) {
			if (vfs_poll(files[i], &table.pt) & EPOLLIN) {
				signalled = i;
				break;
			}
		}
		table.pt._qproc = NULL;	/* registered on every queue now */
		if (signalled >= 0)
			break;
		if (table.error) {
			ret = HZN_RESULT_OUT_OF_MEMORY;
			break;
		}
		if (timeout == 0) {
			ret = HZN_RESULT_TIMED_OUT;
			break;
		}
		if (hzn_wait_cancelled(true)) {
			ret = HZN_RESULT_CANCELLED;
			break;
		}
		if (hzn_check_signals()) {
			ret = HZN_RESULT_TERMINATION_REQUESTED;
			break;
		}
		set_current_state(TASK_INTERRUPTIBLE | TASK_FREEZABLE);
		if (!READ_ONCE(table.triggered) && !hzn_wait_cancelled(false)) {
			if (timeout > 0) {
				if (!schedule_hrtimeout_range(&expires,
							      current->timer_slack_ns,
							      HRTIMER_MODE_ABS))
					timeout = 0;	/* poll once more */
			} else {
				schedule();
			}
		}
		__set_current_state(TASK_RUNNING);
		/* As do_poll(): a wake-up from now on is seen in the next pass. */
		smp_store_mb(table.triggered, 0);
	}
	poll_freewait(&table);

	for (i = 0; i < num; i++)
		fput(files[i]);
	*index = signalled;
	return ret;
}

HSYSCALL_DEFINE4(wait_synchronization, long, __unused, u32 __user *, handles,
		 s32, num, s64, timeout)
{
	s32 index;
	long ret = hzn_wait_synchronization(handles, num, timeout, &index);

	if (ret == HZN_RESULT_SUCCESS)
		HSYSCALL_OUT(index);
	return ret;
}

HSYSCALL_DEFINE1(cancel_synchronization, u32, handle)
{
	return hzn_cancel_synchronization(handle);
}

HSYSCALL_DEFINE3(arbitrate_lock, u32, owner, u32 __user *, addr, u32, tag)
{
	return hzn_arbitrate_lock(owner, addr, tag);
}

HSYSCALL_DEFINE1(arbitrate_unlock, u32 __user *, addr)
{
	return hzn_arbitrate_unlock(addr);
}

HSYSCALL_DEFINE4(wait_process_wide_key_atomic, u32 __user *, addr,
		 u32 __user *, key, u32, tag, s64, timeout)
{
	return hzn_wait_process_wide_key(addr, key, tag, timeout);
}

HSYSCALL_DEFINE2(signal_process_wide_key, u32 __user *, key, s32, count)
{
	return hzn_signal_process_wide_key(key, count);
}

/* Horizon programs read CNTPCT_EL0 directly as well (see process.c). */
HSYSCALL_DEFINE0(get_system_tick)
{
	isb();
	return read_sysreg(cntpct_el0);
}

HSYSCALL_DEFINE2(connect_to_named_port, long, __unused,
		 const char __user *, name)
{
	u32 handle;
	long ret = hzn_connect_to_named_port(name, &handle);

	if (ret == HZN_RESULT_SUCCESS)
		HSYSCALL_OUT(handle);
	return ret;
}

HSYSCALL_DEFINE1(send_sync_request, u32, handle)
{
	return hzn_send_sync_request(handle);
}

HSYSCALL_DEFINE3(send_sync_request_with_user_buffer, unsigned long, buf,
		 u64, size, u32, handle)
{
	return hzn_send_sync_request_with_user_buffer(buf, size, handle);
}

HSYSCALL_DEFINE4(send_async_request_with_user_buffer, long, __unused,
		 unsigned long, buf, u64, size, u32, handle)
{
	u32 event;
	long ret = hzn_send_async_request_with_user_buffer(buf, size, handle,
							    &event);

	if (ret == HZN_RESULT_SUCCESS)
		HSYSCALL_OUT(event);
	return ret;
}

/* IDs also accept copied handles from another Horizon process. */
HSYSCALL_DEFINE2(get_process_id, long, __unused, u32, handle)
{
	u64 id;
	long ret = hzn_get_process_id(handle, &id);
	if (!ret)
		HSYSCALL_OUT(id);
	return ret;
}

HSYSCALL_DEFINE2(get_thread_id, long, __unused, u32, handle)
{
	u64 id;
	long ret = hzn_get_thread_id(handle, &id);
	if (!ret)
		HSYSCALL_OUT(id);
	return ret;
}

HSYSCALL_DEFINE3(break, u32, reason, u64, info1, u64, info2)
{
	/* A debugger receives notification-only breaks as well as fatal ones. */
	if (READ_ONCE(current->ptrace) & PT_PTRACED) {
		force_sig(SIGTRAP);
		return HZN_RESULT_SUCCESS;
	}
	if (reason & HZN_BREAK_NOTIFICATION_ONLY)
		return HZN_RESULT_SUCCESS;
	pr_info("horizon: %s[%d]: break, reason 0x%x info 0x%llx 0x%llx\n",
		current->comm, task_tgid_nr(current), reason, info1, info2);
	do_group_exit(1 << 8);
}

HSYSCALL_DEFINE1(return_from_exception, u32, result)
{
	return hzn_return_from_exception(result);
}

HSYSCALL_DEFINE2(output_debug_string, const char __user *, str, u64, size)
{
	size_t len = min_t(u64, size, 1024);
	char *msg = kmalloc(len + 1, GFP_KERNEL);

	if (!msg)
		return HZN_RESULT_OUT_OF_MEMORY;
	if (copy_from_user(msg, str, len)) {
		kfree(msg);
		return HZN_RESULT_INVALID_ADDRESS;
	}
	msg[len] = 0;
	pr_info_ratelimited("horizon: %s[%d]: %s\n", current->comm,
			    task_tgid_nr(current), msg);
	kfree(msg);
	return HZN_RESULT_SUCCESS;
}

enum {
	/* 1.0.0+ */
	INFO_ALLOWED_CPU_CORE_MASK = 0,
	INFO_ALLOWED_THREAD_PRIORITY_MASK = 1,
	INFO_MAP_REGION_BASE_ADDR = 2,
	INFO_MAP_REGION_SIZE = 3,
	INFO_HEAP_REGION_BASE_ADDR = 4,
	INFO_HEAP_REGION_SIZE = 5,
	INFO_TOTAL_PHYSICAL_MEMORY_AVAILABLE = 6,
	INFO_TOTAL_PHYSICAL_MEMORY_USED = 7,
	INFO_IS_CURRENT_PROCESS_BEING_DEBUGGED = 8,
	INFO_RESOURCE_LIMIT = 9,
	INFO_IDLE_TICK_COUNT = 10,
	INFO_RANDOM_ENTROPY = 11,
	INFO_THREAD_TICK_COUNT_DEPRECATED = 0xF0000002,
	/* 2.0.0+ */
	INFO_ASLR_REGION_BASE_ADDR = 12,
	INFO_ASLR_REGION_SIZE = 13,
	INFO_STACK_REGION_BASE_ADDR = 14,
	INFO_STACK_REGION_SIZE = 15,
	/* 3.0.0+ */
	INFO_SYSTEM_RESOURCE_SIZE = 16,
	INFO_SYSTEM_RESOURCE_USAGE = 17,
	INFO_TITLE_ID = 18,
	/* 4.0.0 only */
	INFO_INITIAL_PROCESS_ID_RANGE = 19,
	/* 5.0.0+ */
	INFO_USER_EXCEPTION_CONTEXT_ADDR = 20,
	/* 6.0.0+ */
	INFO_TOTAL_PHYSICAL_MEMORY_AVAILABLE_WITHOUT_SYSTEM_RESOURCE = 21,
	INFO_TOTAL_PHYSICAL_MEMORY_USED_WITHOUT_SYSTEM_RESOURCE = 22,
	/* 9.0.0+ */
	INFO_IS_APPLICATION = 23,
	/* 11.0.0+ */
	INFO_FREE_THREAD_COUNT = 24,
	/* 13.0.0+ */
	INFO_THREAD_TICK_COUNT = 25,
	/* 14.0.0+ */
	INFO_IS_SVC_PERMITTED = 26,
	/* 16.0.0+ */
	INFO_IO_REGION_HINT = 27,
	/* 18.0.0+ */
	INFO_ALIAS_REGION_EXTRA_SIZE = 28,
};

/* Program IDs of applications; lower ones are system programs and applets. */
#define HZN_APPLICATION_ID_MIN	0x0100000000010000ULL

static u64 hzn_ns_to_ticks(u64 ns)
{
	return mul_u64_u32_div(ns, arch_timer_get_rate(), NSEC_PER_SEC);
}

/* Counter ticks of CPU time used by @task. */
static u64 hzn_thread_ticks(struct task_struct *task)
{
	return hzn_ns_to_ticks(task_sched_runtime(task));
}

/* Counter ticks @cpu has been idle. */
static u64 hzn_idle_ticks(int cpu)
{
	u64 us = get_cpu_idle_time_us(cpu, NULL);

	if (us == (u64)-1)
		return hzn_ns_to_ticks(kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE]);
	return hzn_ns_to_ticks(us * NSEC_PER_USEC);
}

/* The process resource limit on threads is the user's RLIMIT_NPROC. */
static u64 hzn_free_thread_count(void)
{
	unsigned long limit = rlimit(RLIMIT_NPROC);
	long used = get_rlimit_value(task_ucounts(current), UCOUNT_RLIMIT_NPROC);

	if (used < 0 || used >= limit)
		return 0;
	return min_t(unsigned long, limit - used, S32_MAX);
}

/* The process information svcGetInfo has for the current process. */
static u64 hzn_process_info(struct hzn_process *proc, u32 type)
{
	switch (type) {
	case INFO_ALLOWED_CPU_CORE_MASK:
		return proc->core_mask;
	case INFO_ALLOWED_THREAD_PRIORITY_MASK:
		return U64_MAX;
	case INFO_MAP_REGION_BASE_ADDR:
		return proc->alias_start;
	case INFO_MAP_REGION_SIZE:
		return proc->alias_size;
	case INFO_HEAP_REGION_BASE_ADDR:
		return proc->heap_start;
	case INFO_HEAP_REGION_SIZE:
		return proc->heap_region_size;
	case INFO_ASLR_REGION_BASE_ADDR:
		return proc->alias_code_start;
	case INFO_ASLR_REGION_SIZE:
		return proc->alias_code_size;
	case INFO_STACK_REGION_BASE_ADDR:
		return proc->stack_region_start;
	case INFO_STACK_REGION_SIZE:
		return proc->stack_region_size;
	case INFO_SYSTEM_RESOURCE_SIZE:
		return proc->system_resource_size;
	case INFO_TITLE_ID:
		return proc->title_id;
	case INFO_USER_EXCEPTION_CONTEXT_ADDR:
		return proc->plr;
	/*
	 * Programs size their heaps from these; the values are the ones that
	 * worked for the original Horizon Linux.
	 */
	case INFO_TOTAL_PHYSICAL_MEMORY_AVAILABLE:
	case INFO_TOTAL_PHYSICAL_MEMORY_AVAILABLE_WITHOUT_SYSTEM_RESOURCE:
		return 0x60000000;
	case INFO_IS_APPLICATION:
		return proc->title_id >= HZN_APPLICATION_ID_MIN;
	case INFO_FREE_THREAD_COUNT:
		return hzn_free_thread_count();
	}
	/* Memory used, and the alias region extra size of ASan builds. */
	return 0;
}

/* GetInfo describes the process even when only one of its threads is traced. */
static bool hzn_process_being_debugged(void)
{
	struct task_struct *task;
	bool attached = false;

	if (READ_ONCE(current->ptrace) & PT_PTRACED)
		return true;
	rcu_read_lock();
	for_each_thread(current, task) {
		if (READ_ONCE(task->ptrace) & PT_PTRACED) {
			attached = true;
			break;
		}
	}
	rcu_read_unlock();
	return attached;
}

HSYSCALL_DEFINE4(get_info, long, __unused, u32, type, u32, handle,
		 u64, subtype)
{
	struct task_struct *task;
	u64 out;
	int cpu;

	switch (type) {
	case INFO_ALLOWED_CPU_CORE_MASK ... INFO_TOTAL_PHYSICAL_MEMORY_USED:
	case INFO_ASLR_REGION_BASE_ADDR ... INFO_TITLE_ID:
	case INFO_USER_EXCEPTION_CONTEXT_ADDR ... INFO_FREE_THREAD_COUNT:
	case INFO_ALIAS_REGION_EXTRA_SIZE:
		/* There are no process handles but the pseudo handle. */
		if (subtype)
			return HZN_RESULT_INVALID_COMBINATION;
		if (handle != HZN_PSEUDO_HANDLE_CURRENT_PROCESS)
			return HZN_RESULT_INVALID_HANDLE;
		out = hzn_process_info(hzn_current()->proc, type);
		break;
	case 34: /* TransferMemoryHint: the owner's original virtual address. */
		if (subtype)
			return HZN_RESULT_INVALID_COMBINATION;
		{
			long ret = hzn_transfer_memory_hint(handle, &out);
			if (ret)
				return ret;
		}
		break;
	case INFO_IS_CURRENT_PROCESS_BEING_DEBUGGED:
		if (handle != HZN_INVALID_HANDLE)
			return HZN_RESULT_INVALID_HANDLE;
		if (subtype)
			return HZN_RESULT_INVALID_COMBINATION;
		out = hzn_process_being_debugged();
		break;
	case INFO_RESOURCE_LIMIT:
		if (handle != HZN_INVALID_HANDLE)
			return HZN_RESULT_INVALID_HANDLE;
		if (subtype)
			return HZN_RESULT_INVALID_COMBINATION;
		out = HZN_INVALID_HANDLE;	/* no resource limit object */
		break;
	case INFO_IDLE_TICK_COUNT:
		/* Of the current core, which is the only one it may ask for. */
		if (handle != HZN_INVALID_HANDLE)
			return HZN_RESULT_INVALID_HANDLE;
		cpu = get_cpu();
		out = hzn_idle_ticks(cpu);
		put_cpu();
		if (subtype != U64_MAX && subtype != cpu)
			return HZN_RESULT_INVALID_COMBINATION;
		break;
	case INFO_RANDOM_ENTROPY:
		out = get_random_u64();
		break;
	case INFO_INITIAL_PROCESS_ID_RANGE:
		/* Horizon 5.0.0 and later have no such information. */
		return HZN_RESULT_INVALID_ENUM_VALUE;
	case INFO_THREAD_TICK_COUNT:
		/* Linux keeps no CPU time per core: it is all on the last one. */
		if (subtype != U64_MAX &&
		    subtype >= min_t(unsigned int, nr_cpu_ids, 64))
			return HZN_RESULT_INVALID_COMBINATION;
		fallthrough;
	case INFO_THREAD_TICK_COUNT_DEPRECATED:
		task = hzn_handle_to_thread(handle);
		if (!task)
			return HZN_RESULT_INVALID_HANDLE;
		out = hzn_thread_ticks(task);
		if (type == INFO_THREAD_TICK_COUNT && subtype != U64_MAX &&
		    subtype != task_cpu(task))
			out = 0;
		put_task_struct(task);
		break;
	case INFO_IS_SVC_PERMITTED:
		/* Only asked about svcSynchronizePreemptionState, which it may use. */
		if (handle != HZN_INVALID_HANDLE)
			return HZN_RESULT_INVALID_HANDLE;
		if (subtype != __HNR_synchronize_preemption_state)
			return HZN_RESULT_INVALID_COMBINATION;
		out = 1;
		break;
	case INFO_IO_REGION_HINT:
		/* There are no I/O regions, so no handle can name one. */
		if (subtype)
			return HZN_RESULT_INVALID_COMBINATION;
		return HZN_RESULT_INVALID_HANDLE;
	default:
		pr_warn_ratelimited("horizon: get_info %u/%llu is not implemented\n",
				    type, subtype);
		return HZN_RESULT_INVALID_ENUM_VALUE;
	}

	HSYSCALL_OUT(out);
	return HZN_RESULT_SUCCESS;
}

HSYSCALL_DEFINE2(map_physical_memory, unsigned long, addr, u64, size)
{
	return hzn_map_physical_memory(addr, size);
}

HSYSCALL_DEFINE2(unmap_physical_memory, unsigned long, addr, u64, size)
{
	return hzn_unmap_physical_memory(addr, size);
}

/*
 * The thread that ran on this core before the current one, for sampling
 * profilers: Horizon only tells that to debug (development) systems, so,
 * as on retail ones, there is no such thread.
 */
HSYSCALL_DEFINE0(get_last_thread_info)
{
	return HZN_RESULT_NO_THREAD;
}

HSYSCALL_DEFINE2(set_thread_activity, u32, handle, u32, activity)
{
	return hzn_set_thread_activity(handle, activity);
}

HSYSCALL_DEFINE2(get_thread_context3, void __user *, ctx, u32, handle)
{
	return hzn_get_thread_context(ctx, handle);
}

HSYSCALL_DEFINE4(wait_for_address, u32 __user *, addr, u32, type, s32, value,
		 s64, timeout)
{
	return hzn_wait_for_address(addr, type, value, timeout);
}

HSYSCALL_DEFINE4(signal_to_address, u32 __user *, addr, u32, type, s32, value,
		 s32, count)
{
	return hzn_signal_to_address(addr, type, value, count);
}

/*
 * Horizon pins a thread that is preempted while its thread-local "disable
 * count" is non-zero, and the thread unpins itself with this SVC. Threads
 * are never pinned here, so there is nothing to do.
 */
HSYSCALL_DEFINE0(synchronize_preemption_state)
{
	return HZN_RESULT_SUCCESS;
}

HSYSCALL_DEFINE4(create_session, long, unused0, long, unused1, bool, light, u64, name)
{
	u32 server, client;
	long ret = hzn_create_session(light, name, &server, &client);
	if (!ret) {
		HSYSCALL_OUT(server);
		HSYSCALL_OUTN(2, client);
	}
	return ret;
}
HSYSCALL_DEFINE2(accept_session, long, unused, u32, port)
{
	u32 session;
	long ret = hzn_accept_session(port, &session);
	if (!ret)
		HSYSCALL_OUT(session);
	return ret;
}
HSYSCALL_DEFINE5(create_port, long, unused0, long, unused1, s32, maximum, bool, light, u64, name)
{
	u32 server, client;
	long ret = hzn_create_port(maximum, light, name, &server, &client);
	if (!ret) {
		HSYSCALL_OUT(server);
		HSYSCALL_OUTN(2, client);
	}
	return ret;
}
HSYSCALL_DEFINE2(connect_to_port, long, unused, u32, port)
{
	u32 session;
	long ret = hzn_connect_to_port(port, &session);
	if (!ret)
		HSYSCALL_OUT(session);
	return ret;
}
HSYSCALL_DEFINE5(reply_and_receive, long, unused, u32 __user *, handles,
                 s32, num, u32, target, s64, timeout)
{
	s32 index;
	long ret = hzn_reply_and_receive(current->thread.uw.tp_value, 0x100,
					handles, num, target, timeout, &index);
	HSYSCALL_OUT((u32)index);
	return ret;
}
/* This ABI has seven inputs (including unused x0), outside Linux's six-arg macros. */
asmlinkage long __arm64_hsys_reply_and_receive_with_user_buffer(const struct pt_regs *regs)
{
	unsigned long addr = regs->regs[1];
	u64 size = regs->regs[2];
	struct page **pages;
	unsigned long count;
	s32 index = -1;
	long ret = hzn_pin_ipc_user_buffer(addr, size, &pages, &count);

	if (ret)
		return ret;
	ret = hzn_reply_and_receive(addr, size, (u32 __user *)regs->regs[3],
				   (s32)regs->regs[4], (u32)regs->regs[5],
				   (s64)regs->regs[6], &index);
	unpin_user_pages(pages, count);
	kvfree(pages);
	((struct pt_regs *)regs)->regs[1] = (u32)index;
	return ret;
}

HSYSCALL_DEFINE0(flush_entire_data_cache)
{
	return hzn_flush_entire_data_cache();
}

HSYSCALL_DEFINE4(create_shared_memory, long, unused, u64, size, u32, owner, u32, remote)
{
	u32 handle;
	long ret = hzn_create_shared_memory(size, owner, remote, &handle);
	if (!ret)
		HSYSCALL_OUT(handle);
	return ret;
}
HSYSCALL_DEFINE4(map_transfer_memory, u32, handle, unsigned long, addr, u64, size, u32, perm)
{
	return hzn_map_transfer_memory(handle, addr, size, perm);
}
HSYSCALL_DEFINE3(unmap_transfer_memory, u32, handle, unsigned long, addr, u64, size)
{
	return hzn_unmap_transfer_memory(handle, addr, size);
}
HSYSCALL_DEFINE3(create_code_memory, long, unused, unsigned long, addr, u64, size)
{
	u32 handle;
	long ret = hzn_create_code_memory(addr, size, &handle);
	if (!ret)
		HSYSCALL_OUT(handle);
	return ret;
}
HSYSCALL_DEFINE5(control_code_memory, u32, handle, u32, op, unsigned long, addr, u64, size, u32, perm)
{
	return hzn_control_code_memory(handle, op, addr, size, perm);
}

#undef __SYSCALL
#define __SYSCALL(nr, sym)	asmlinkage long __arm64_##sym(const struct pt_regs *);
#include <asm/horizon/unistd.h>

#undef __SYSCALL
#define __SYSCALL(nr, sym)	[nr] = __arm64_##sym,

const syscall_fn_t horizon_sys_call_table[__HNR_syscalls] = {
	[0 ... __HNR_syscalls - 1] = __arm64_hsys_ni_syscall,
#include <asm/horizon/unistd.h>
};
