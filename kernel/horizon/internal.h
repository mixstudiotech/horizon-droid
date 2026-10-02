/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Horizon (Nintendo Switch) system call personality: shared definitions.
 *
 * A Horizon process is an ordinary Linux thread group whose threads have
 * TIF_HORIZON set: "svc #imm" with a non-zero immediate is dispatched to the
 * Horizon SVC table, "svc #0" stays a Linux system call. Handles are file
 * descriptors (handle = fd + 1); thread handles retain task identity and poll a thread pidfd.
 */
#ifndef _KERNEL_HORIZON_INTERNAL_H
#define _KERNEL_HORIZON_INTERNAL_H

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rtmutex.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <uapi/linux/horizon.h>

#include "result.h"
#include "types.h"

/* horizon_hdr::address_space_type */
enum hzn_address_space_type {
	HZN_IS_32_BIT = 0,
	HZN_IS_36_BIT = 1,
	HZN_IS_32_BIT_NO_MAP = 2,
	HZN_IS_39_BIT = 3,
};

#define HZN_HIGHEST_THREAD_PRIORITY	0
#define HZN_LOWEST_THREAD_PRIORITY	63

#define HZN_INVALID_HANDLE			((u32)0)
#define HZN_PSEUDO_HANDLE_CURRENT_THREAD	0xFFFF8000u
#define HZN_PSEUDO_HANDLE_CURRENT_PROCESS	0xFFFF8001u
#define HZN_HANDLE_WAIT_MASK			0x40000000u

static inline bool hzn_is_pseudo_handle(u32 handle)
{
	return handle == HZN_PSEUDO_HANDLE_CURRENT_THREAD ||
	       handle == HZN_PSEUDO_HANDLE_CURRENT_PROCESS;
}

/* Debugger stops wake a wait, but only fatal signals terminate the operation. */
#define HZN_WAIT_STATE	(TASK_INTERRUPTIBLE | TASK_NOLOAD | TASK_FREEZABLE)
bool hzn_check_signals(void);

/* Where the main image is loaded, and the TLS of the main thread. */
#define HZN_IMAGE_BASE		SZ_128M
#define HZN_TLS_OFFSET		0x18c000
/* Thread-local pages are allocated in [tls_base, tls_base + this). */
#define HZN_TLS_AREA_SIZE	(0x10000 * PAGE_SIZE)
/*
 * svc::ThreadLocalRegion::thread_handle: the kernel puts each thread's own
 * handle there, and nn::os reads it as the current thread's handle (mutex
 * owners, for one).
 */
#define HZN_TLS_THREAD_HANDLE	0x110

/* svcMapMemory bookkeeping: [dst, dst + size) aliases [src, src + size). */
struct hzn_alias {
	struct list_head node;
	unsigned long dst, src, size;
	struct file *file;		/* shmem object backing both */
	pgoff_t pgoff;			/* of src within file */
};

struct hzn_process {
	struct kref ref;

	u64 title_id;
	u64 core_mask;			/* the cores its threads may use */
	u32 system_resource_size;
	u8 ideal_core;
	u8 address_space_type;

	/* Layout, fixed at exec. */
	unsigned long space_end;
	unsigned long image_end;	/* code sets: [HZN_IMAGE_BASE, image_end) */
	unsigned long alias_code_start, alias_code_size;
	unsigned long alias_start, alias_size;
	unsigned long heap_start, heap_region_size;
	unsigned long stack_region_start, stack_region_size;
	unsigned long tls_base;
	unsigned long plr;		/* process local region, a page */

	/* Heap, memory aliases and permission-locked ranges. */
	struct mutex mem_lock;
	struct file *heap_file;		/* shmem, heap_region_size bytes */
	unsigned long heap_size;	/* currently mapped */
	struct list_head aliases;
	struct list_head object_maps;
	struct list_head locked;	/* see memory.c */

	/* Threads: their states, the unstarted list and nr_running. */
	spinlock_t lock;
	struct list_head unstarted;	/* HZN_THREAD_CREATED threads */
	int nr_running;			/* HZN_THREAD_STARTED threads */

	/* The thread in the user exception handler; see exception.c. */
	struct hzn_thread *exception_thread;
	wait_queue_head_t exception_wq;

	/* Synchronization: see arbiter.c. */
	struct rt_mutex arb_lock;
	struct list_head lock_waiters;	/* ArbitrateLock */
	struct list_head cv_waiters;	/* WaitProcessWideKeyAtomic */
	struct list_head addr_waiters;	/* WaitForAddress */
};

/* hzn_thread::state, protected by hzn_process::lock */
enum {
	HZN_THREAD_CREATED,		/* waiting for svcStartThread */
	HZN_THREAD_STARTED,
	HZN_THREAD_DISCARDED,		/* to exit without running */
	HZN_THREAD_EXITED,
};

struct hzn_waiter;

struct hzn_thread {
	struct hzn_process *proc;	/* reference */
	struct task_struct *task;
	u32 handle;			/* this thread's own handle */
	u64 id;				/* retained after Linux reaps the task */
	int priority;			/* effective Horizon priority */
	int base_priority, applied_priority;
	struct list_head donors;		/* waiters on locks this thread owns */
	struct hzn_waiter *waiter;	/* under proc->arb_lock */
	int state;
	unsigned long tls;		/* thread-local page, TPIDRRO_EL0 */
	struct list_head unstarted_node;
	struct callback_head start_work;

	struct mutex lock;		/* ideal_core, affinity, paused */
	int ideal_core;			/* -1: none */
	u64 affinity;			/* the cores it may run on */

	unsigned long flags;		/* HZN_THREAD_* */
	/* The fault it is in the user exception handler for. */
	int exception_signo, exception_code;
	unsigned long exception_far;

	bool paused;			/* by svcSetThreadActivity */
	bool parked;			/* paused, in pause_work */
	struct callback_head pause_work;
};

/* hzn_thread::flags */
enum {
	HZN_THREAD_WAIT_CANCELLED,	/* by svcCancelSynchronization */
	HZN_THREAD_PAUSE_QUEUED,	/* pause_work is queued */
	HZN_THREAD_CHECKING_SIGNALS, /* signal check must not recurse into task work */
};

/* A Linux task that serves Horizon sessions (horizon_servctl). */
struct hzn_service {
	spinlock_t lock;		/* requests, stopped */
	struct list_head requests;	/* queued, not yet taken */
	bool stopped;
	struct hzn_request *cur;	/* between GET_CMD and PUT_CMD */
	unsigned long cmd_addr;		/* where the service sees the command */
};

/* Private data of a session handle. */
struct hzn_session {
	struct pid *service;		/* NULL: requests are stubbed */
	unsigned long id;
	bool is_domain;
};

enum {
	HZN_REQ_PENDING,
	HZN_REQ_HANDLED,
	HZN_REQ_FAILED,
	HZN_REQ_ABANDONED,
};

/* Work the service has the blocked requester do in its own context. */
enum {
	HZN_OP_NONE,
	HZN_OP_INSTALL_FILE,		/* op_file -> new handle */
	HZN_OP_MAKE_SHARED,		/* [op_addr, +op_len) -> shmem, op_file */
};

struct hzn_request {
	struct kref ref;
	struct list_head node;
	struct task_struct *requester;	/* NULL for close requests */
	struct page *cmd;		/* pinned TLS page, NULL for close */
	struct file *session_file;
	unsigned long close_session_id;
	atomic_t state;

	spinlock_t op_lock;		/* the op_* fields */
	int op;
	struct file *op_file;
	unsigned long op_addr, op_len;
	long op_result;
	struct completion op_done;
};

static inline struct hzn_thread *hzn_current(void)
{
	return current->hzn_thread;
}

/* process.c */
struct hzn_process *hzn_process_alloc(void);
void hzn_process_put(struct hzn_process *proc);
struct hzn_thread *hzn_thread_alloc(struct hzn_process *proc);
u32 hzn_handle_add(struct file *file);
long hzn_handle_add_pair(struct file *first, struct file *second,
			 u32 *first_handle, u32 *second_handle);
struct file *hzn_handle_get(u32 handle);
int hzn_handle_close(u32 handle);
struct file *hzn_copy_handle_file(u32 handle);
long hzn_get_process_id(u32 handle, u64 *id);
long hzn_get_thread_id(u32 handle, u64 *id);
u32 hzn_thread_handle_create(struct task_struct *task);
struct task_struct *hzn_handle_to_thread(u32 handle);
bool hzn_thread_exists(u32 handle);
bool hzn_valid_priority(s64 priority);
void hzn_apply_priority(struct task_struct *p, int priority);
void hzn_set_priority(struct task_struct *p, int priority);
void hzn_yield(s64 type);
long hzn_create_thread(unsigned long entry, unsigned long arg,
		       unsigned long stack_top, s32 priority, s32 core,
		       u32 *handle);
long hzn_start_thread(u32 handle);
long hzn_get_core_mask(u32 handle, s32 *ideal_core, u64 *affinity);
long hzn_set_core_mask(u32 handle, s32 ideal_core, u64 affinity);
long hzn_cancel_synchronization(u32 handle);
bool hzn_wait_cancelled(bool clear);
long hzn_set_thread_activity(u32 handle, u32 activity);
long hzn_get_thread_context(void __user *uctx, u32 handle);

/* memory.c */
long hzn_set_heap_size(u64 size, unsigned long *addr);
long hzn_map_memory(unsigned long dst, unsigned long src, u64 size);
long hzn_unmap_memory(unsigned long dst, unsigned long src, u64 size);
long hzn_set_memory_permission(unsigned long addr, u64 size, u32 perm);
long hzn_set_memory_attribute(unsigned long addr, u64 size, u32 mask, u32 value);
long hzn_query_memory(struct memory_info *mi, unsigned long addr);
long hzn_create_transfer_memory(unsigned long addr, u64 size, u32 perm,
				u32 *handle);
long hzn_map_shared_memory(u32 handle, unsigned long addr, u64 size,
			   u32 perm);
long hzn_unmap_shared_memory(u32 handle, unsigned long addr, u64 size);
long hzn_map_physical_memory(unsigned long addr, u64 size);
long hzn_unmap_physical_memory(unsigned long addr, u64 size);
struct file *hzn_find_shared(struct mm_struct *mm, unsigned long start,
			     unsigned long size, pgoff_t *pgoff);
long hzn_make_shared(unsigned long start, unsigned long size,
		     struct file **filep);
void hzn_process_free_aliases(struct hzn_process *proc);
void hzn_process_free_locked(struct hzn_process *proc);

/* mem_objects.c */
long hzn_create_shared_memory(u64 size, u32 owner, u32 remote, u32 *handle);
long hzn_map_transfer_memory(u32 handle, unsigned long addr, u64 size, u32 perm);
long hzn_unmap_transfer_memory(u32 handle, unsigned long addr, u64 size);
long hzn_create_code_memory(unsigned long addr, u64 size, u32 *handle);
long hzn_control_code_memory(u32 handle, u32 op, unsigned long addr, u64 size, u32 perm);
long hzn_transfer_memory_hint(u32 handle, u64 *addr);
bool hzn_is_memory_object(struct file *file);
long hzn_map_typed_shared_memory(u32 handle, unsigned long addr, u64 size, u32 perm);
long hzn_unmap_typed_shared_memory(u32 handle, unsigned long addr, u64 size);
bool hzn_memory_source_overlaps(struct mm_struct *mm, unsigned long addr, u64 size);
void hzn_query_memory_object(struct hzn_process *proc, struct memory_info *mi, unsigned long addr);
void hzn_process_free_object_maps(struct hzn_process *proc);

/* arbiter.c */
long hzn_arbitrate_lock(u32 owner, u32 __user *addr, u32 tag);
long hzn_arbitrate_unlock(u32 __user *addr);
long hzn_wait_process_wide_key(u32 __user *addr, u32 __user *key, u32 tag,
			       s64 timeout);
long hzn_signal_process_wide_key(u32 __user *key, s32 count);
long hzn_wait_for_address(u32 __user *addr, u32 type, s32 value,
			  s64 timeout);
long hzn_signal_to_address(u32 __user *addr, u32 type, s32 value,
			   s32 count);

/* ipc.c */
long hzn_connect_to_named_port(const char __user *name, u32 *handle);
long hzn_send_sync_request(u32 handle);
long hzn_send_sync_request_with_user_buffer(unsigned long buf, u64 size,
					     u32 handle);
long hzn_send_async_request_with_user_buffer(unsigned long buf, u64 size,
					      u32 handle, u32 *event);
bool hzn_is_session(struct file *file);
void hzn_service_exit(struct task_struct *tsk);
void hzn_service_free(struct hzn_service *svc);

/* native_ipc.c */
long hzn_pin_ipc_user_buffer(unsigned long addr, u64 size, struct page ***pages, unsigned long *count);
long hzn_create_session(bool light, u64 name, u32 *server, u32 *client);
long hzn_create_port(s32 maximum, bool light, u64 name, u32 *server, u32 *client);
long hzn_connect_to_port(u32 handle, u32 *session);
long hzn_accept_session(u32 handle, u32 *session);
bool hzn_is_native_client(struct file *file);
long hzn_native_send(struct file *file, unsigned long addr, u64 size);
long hzn_reply_and_receive(unsigned long addr, u64 size, u32 __user *handles,
                          s32 num, u32 target, s64 timeout, s32 *index);

/* event.c */
long hzn_create_event(u32 *write_handle, u32 *read_handle);
long hzn_signal_event(u32 handle);
bool hzn_clear_native_event(struct file *file, bool reset, long *result);

long hzn_flush_entire_data_cache(void);
/* exception.c */
void hzn_release_exception(struct hzn_thread *thread);
long hzn_return_from_exception(u32 result);

/* memwatch.c */
long hzn_memwatch(struct mm_struct *mm, unsigned long start, size_t len,
		  bool clear, loff_t __user *vec, size_t vec_len);

#endif /* _KERNEL_HORIZON_INTERNAL_H */
