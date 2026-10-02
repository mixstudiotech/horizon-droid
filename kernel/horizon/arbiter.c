// SPDX-License-Identifier: GPL-2.0
/*
 * Horizon synchronization SVCs, with the semantics of Horizon's
 * KConditionVariable and KAddressArbiter:
 *
 *  - ArbitrateLock/ArbitrateUnlock: user space owns the lock word, which holds
 *    the owner's thread handle, with HZN_HANDLE_WAIT_MASK set once someone
 *    waits. On unlock the kernel hands the lock straight to the highest
 *    priority waiter by writing that waiter's tag into the word.
 *  - WaitProcessWideKeyAtomic/SignalProcessWideKey: condition variables on
 *    top of those locks; a signalled waiter takes its lock back (or queues
 *    for it) before it is woken.
 *  - WaitForAddress/SignalToAddress: the address arbiter.
 *
 * Waiters live on per-process lists in priority order (FIFO within a
 * priority). arb_lock is a mutex, so the lock words can be read and written
 * with ordinary user accessors while it is held; read-modify-write cycles
 * that race with user space atomics use the futex cmpxchg helper.
 */

#include <linux/hrtimer.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/wake_q.h>
#include <linux/uaccess.h>
#include <asm/futex.h>

#include "internal.h"

enum {
	HZN_ARB_WAIT_IF_LESS_THAN		= 0,
	HZN_ARB_DECREMENT_AND_WAIT_IF_LESS_THAN	= 1,
	HZN_ARB_WAIT_IF_EQUAL			= 2,
};

enum {
	HZN_SIGNAL				= 0,
	HZN_SIGNAL_AND_INCREMENT_IF_EQUAL	= 1,
	HZN_SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL = 2,
};

struct hzn_waiter {
	struct list_head node;
	struct task_struct *task;
	struct task_struct *owner;	/* reference while donating priority */
	struct list_head donor_node;
	struct list_head *queue;
	u32 __user *addr;	/* lock word, or arbiter address */
	u32 __user *key;	/* condition variable */
	u32 tag;		/* written into the lock word on hand-over */
	int priority;
	bool done;
	long result;
};

static void hzn_waiter_init(struct hzn_waiter *w)
{
	INIT_LIST_HEAD(&w->node);
	INIT_LIST_HEAD(&w->donor_node);
	w->owner = NULL;
	w->queue = NULL;
	w->task = current;
	w->priority = hzn_current()->priority;
	w->done = false;
	w->result = HZN_RESULT_SUCCESS;
}

/* Queues @w behind all waiters of the same or a higher priority. */
static void hzn_enqueue(struct list_head *list, struct hzn_waiter *w)
{
	struct hzn_waiter *pos;

	w->queue = list;
	w->task->hzn_thread->waiter = w;
	list_for_each_entry(pos, list, node) {
		if (pos->priority > w->priority) {
			list_add_tail(&w->node, &pos->node);
			return;
		}
	}
	list_add_tail(&w->node, list);
}

/* Only the affected owner's donor list and its upstream chain are visited. */
static void hzn_enqueue_donor(struct hzn_waiter *w)
{
	struct list_head *head = &w->owner->hzn_thread->donors;
	struct hzn_waiter *pos;

	list_for_each_entry(pos, head, donor_node) {
		if (pos->priority > w->priority) {
			list_add_tail(&w->donor_node, &pos->donor_node);
			return;
		}
	}
	list_add_tail(&w->donor_node, head);
}

static void hzn_restore_priority(struct task_struct *task)
{
	while (task) {
		struct hzn_thread *thread = task->hzn_thread;
		struct hzn_waiter *w = thread->waiter;
		int priority = thread->base_priority;

		if (!list_empty(&thread->donors)) {
			struct hzn_waiter *top = list_first_entry(&thread->donors,
						struct hzn_waiter, donor_node);

			priority = min(priority, top->priority);
		}
		if (priority == thread->priority)
			break;
		WRITE_ONCE(thread->priority, priority);
		/* Do not deboost the unlocking task before its wakeups are sent. */
		if (task != current)
			hzn_apply_priority(task, priority);
		if (!w)
			break;
		w->priority = priority;
		list_del_init(&w->node);
		hzn_enqueue(w->queue, w);
		if (w->owner) {
			list_del_init(&w->donor_node);
			hzn_enqueue_donor(w);
		}
		task = w->owner;
	}
}

static void hzn_set_owner(struct hzn_waiter *w, struct task_struct *owner)
{
	struct task_struct *old = w->owner;

	if (old == owner)
		return;
	list_del_init(&w->donor_node);
	w->owner = owner ? get_task_struct(owner) : NULL;
	if (owner) {
		hzn_enqueue_donor(w);
		hzn_restore_priority(owner);
	}
	if (old) {
		hzn_restore_priority(old);
		put_task_struct(old);
	}
}

/* Do not introduce a cycle into the priority inheritance chain. */
static bool hzn_owner_cycle(struct task_struct *owner, struct task_struct *waiter)
{
	while (owner) {
		struct hzn_waiter *w;

		if (owner == waiter)
			return true;
		w = owner->hzn_thread->waiter;
		owner = w ? w->owner : NULL;
	}
	return false;
}

static void hzn_unlink_waiter(struct hzn_waiter *w)
{
	list_del_init(&w->node);
	w->queue = NULL;
	w->task->hzn_thread->waiter = NULL;
	hzn_set_owner(w, NULL);
}

static void hzn_arb_unlock(struct hzn_process *proc, struct wake_q_head *wake_q)
{
	struct hzn_thread *self = hzn_current();

	/* Wake before deboost; the PI mutex protects this final handoff too. */
	if (wake_q)
		wake_up_q(wake_q);
	if (self->applied_priority != self->priority)
		hzn_apply_priority(current, self->priority);
	rt_mutex_unlock(&proc->arb_lock);
}

void hzn_set_priority(struct task_struct *task, int priority)
{
	struct hzn_process *proc = task->hzn_thread->proc;

	rt_mutex_lock(&proc->arb_lock);
	task->hzn_thread->base_priority = priority;
	hzn_restore_priority(task);
	hzn_arb_unlock(proc, NULL);
}

/*
 * Ends @w's wait. Called with arb_lock held; the wake-up happens later. Once
 * done is set the waiter may return (its stack, and @w, are gone then), and
 * done has to be visible before the wake-up is queued: if the task is on
 * someone else's wake queue already, that wake-up is the one it gets.
 */
static void hzn_end_wait(struct hzn_waiter *w, long result,
			 struct wake_q_head *wake_q)
{
	struct task_struct *task = get_task_struct(w->task);

	hzn_unlink_waiter(w);
	w->result = result;
	/* Pairs with the acquires in hzn_wait(): result is visible with done. */
	smp_store_release(&w->done, true);
	wake_q_add_safe(wake_q, task);
}

static long hzn_wait(struct hzn_process *proc, struct hzn_waiter *w,
		     s64 timeout)
{
	struct hrtimer_sleeper t;
	bool timed = timeout > 0;
	long ret;

	if (timed) {
		hrtimer_setup_sleeper_on_stack(&t, CLOCK_MONOTONIC,
					       HRTIMER_MODE_REL);
		hrtimer_set_expires_range_ns(&t.timer, ns_to_ktime(timeout),
					     current->timer_slack_ns);
		hrtimer_sleeper_start_expires(&t, HRTIMER_MODE_REL);
	}

	for (;;) {
		set_current_state(HZN_WAIT_STATE);
		/* Pairs with hzn_end_wait(). */
		if (smp_load_acquire(&w->done))
			break;
		if (timed && !t.task)
			break;
		if (hzn_check_signals())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);

	if (timed) {
		hrtimer_cancel(&t.timer);
		destroy_hrtimer_on_stack(&t.timer);
	}

	/* Ended (by hzn_end_wait()) after all? */
	if (smp_load_acquire(&w->done))
		return w->result;

	rt_mutex_lock(&proc->arb_lock);
	if (w->done) {
		ret = w->result;
	} else {
		hzn_unlink_waiter(w);
		ret = fatal_signal_pending(current) ?
		      HZN_RESULT_TERMINATION_REQUESTED : HZN_RESULT_TIMED_OUT;
	}
	hzn_arb_unlock(proc, NULL);
	return ret;
}

static int hzn_fault_in_writeable(u32 __user *addr)
{
	struct mm_struct *mm = current->mm;
	int ret;

	mmap_read_lock(mm);
	ret = fixup_user_fault(mm, (unsigned long)addr, FAULT_FLAG_WRITE, NULL);
	mmap_read_unlock(mm);
	return ret < 0 ? ret : 0;
}

/* *addr = new if it is still old; *cur gets the value that was there. */
static int hzn_cmpxchg_word(u32 __user *addr, u32 old, u32 new, u32 *cur)
{
	int ret;

	for (;;) {
		pagefault_disable();
		ret = futex_atomic_cmpxchg_inatomic(cur, addr, old, new);
		pagefault_enable();
		if (ret == -EAGAIN) {
			cond_resched();
			continue;
		}
		if (ret != -EFAULT)
			return ret;
		if (hzn_fault_in_writeable(addr))
			return -EFAULT;
	}
}

/*
 * Takes the lock at @addr for @tag if it is free, otherwise sets the wait
 * bit. *prev is the previous lock word (0 if the lock was taken).
 */
static int hzn_update_lock(u32 __user *addr, u32 tag, u32 *prev)
{
	u32 cur, seen;
	int ret;

	if (get_user(cur, addr))
		return -EFAULT;
	for (;;) {
		ret = hzn_cmpxchg_word(addr, cur,
				       cur ? cur | HZN_HANDLE_WAIT_MASK : tag,
				       &seen);
		if (ret)
			return ret;
		if (seen == cur) {
			*prev = cur;
			return 0;
		}
		cur = seen;
	}
}

static bool hzn_bad_address(const void __user *addr)
{
	return !addr || ((unsigned long)addr & 3);
}

/* Hands the lock at @addr to its best waiter, or releases it. */
static long hzn_release_lock(struct hzn_process *proc, u32 __user *addr,
			     struct wake_q_head *wake_q)
{
	struct hzn_waiter *w, *next = NULL;
	bool more = false;
	u32 value = 0;

	list_for_each_entry(w, &proc->lock_waiters, node) {
		if (w->addr != addr)
			continue;
		if (next) {
			more = true;
			break;
		}
		next = w;
	}
	if (next)
		value = next->tag | (more ? HZN_HANDLE_WAIT_MASK : 0);

	if (put_user(value, addr)) {
		if (next)
			hzn_end_wait(next, HZN_RESULT_INVALID_CURRENT_MEMORY,
				     wake_q);
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	}
	if (next) {
		/* Reparenting can reorder queues, so detach next first. */
		list_del_init(&next->node);
		next->queue = NULL;
		next->task->hzn_thread->waiter = NULL;
		list_for_each_entry(w, &proc->lock_waiters, node)
			if (w->addr == addr)
				hzn_set_owner(w, next->task);
		hzn_end_wait(next, HZN_RESULT_SUCCESS, wake_q);
	}
	return HZN_RESULT_SUCCESS;
}

long hzn_arbitrate_lock(u32 owner, u32 __user *addr, u32 tag)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_waiter w;
	struct task_struct *owner_task;
	u32 value;

	if (hzn_bad_address(addr))
		return HZN_RESULT_INVALID_ADDRESS;

	rt_mutex_lock(&proc->arb_lock);
	if (get_user(value, addr)) {
		hzn_arb_unlock(proc, NULL);
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	}
	/* The lock changed hands already: let user space try again. */
	if (value != (owner | HZN_HANDLE_WAIT_MASK)) {
		hzn_arb_unlock(proc, NULL);
		return HZN_RESULT_SUCCESS;
	}
	owner_task = hzn_is_pseudo_handle(owner) ? NULL : hzn_handle_to_thread(owner);
	if (!owner_task) {
		hzn_arb_unlock(proc, NULL);
		return HZN_RESULT_INVALID_HANDLE;
	}

	if (hzn_owner_cycle(owner_task, current)) {
		put_task_struct(owner_task);
		hzn_arb_unlock(proc, NULL);
		return HZN_RESULT_INVALID_STATE;
	}
	hzn_waiter_init(&w);
	w.addr = addr;
	w.tag = tag;
	hzn_enqueue(&proc->lock_waiters, &w);
	hzn_set_owner(&w, owner_task);
	put_task_struct(owner_task);
	hzn_arb_unlock(proc, NULL);

	return hzn_wait(proc, &w, -1);
}

long hzn_arbitrate_unlock(u32 __user *addr)
{
	struct hzn_process *proc = hzn_current()->proc;
	DEFINE_WAKE_Q(wake_q);
	long ret;

	if (hzn_bad_address(addr))
		return HZN_RESULT_INVALID_ADDRESS;

	rt_mutex_lock(&proc->arb_lock);
	ret = hzn_release_lock(proc, addr, &wake_q);
	hzn_arb_unlock(proc, &wake_q);
	return ret;
}

long hzn_wait_process_wide_key(u32 __user *addr, u32 __user *key, u32 tag,
			       s64 timeout)
{
	struct hzn_process *proc = hzn_current()->proc;
	DEFINE_WAKE_Q(wake_q);
	struct hzn_waiter w;
	long ret;

	if (hzn_bad_address(addr) || hzn_bad_address(key))
		return HZN_RESULT_INVALID_ADDRESS;

	rt_mutex_lock(&proc->arb_lock);
	/* Mark the key as having waiters, then release the lock. */
	if (put_user(1, key)) {
		hzn_arb_unlock(proc, NULL);
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	}
	ret = hzn_release_lock(proc, addr, &wake_q);
	if (ret != HZN_RESULT_SUCCESS || timeout == 0) {
		hzn_arb_unlock(proc, &wake_q);
		return ret != HZN_RESULT_SUCCESS ? ret : HZN_RESULT_TIMED_OUT;
	}

	hzn_waiter_init(&w);
	w.addr = addr;
	w.key = key;
	w.tag = tag;
	hzn_enqueue(&proc->cv_waiters, &w);
	hzn_arb_unlock(proc, &wake_q);

	/*
	 * On a time-out the lock is not taken back: user space re-acquires it,
	 * as on Horizon.
	 */
	return hzn_wait(proc, &w, timeout);
}

static bool hzn_key_has_waiters(struct hzn_process *proc, u32 __user *key)
{
	struct hzn_waiter *w;

	list_for_each_entry(w, &proc->cv_waiters, node)
		if (w->key == key)
			return true;
	return false;
}

long hzn_signal_process_wide_key(u32 __user *key, s32 count)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_waiter *w, *tmp;
	DEFINE_WAKE_Q(wake_q);
	LIST_HEAD(selected);
	s32 n = 0;
	u32 prev;

	if (hzn_bad_address(key))
		return HZN_RESULT_INVALID_ADDRESS;

	rt_mutex_lock(&proc->arb_lock);
	list_for_each_entry_safe(w, tmp, &proc->cv_waiters, node) {
		if (w->key != key)
			continue;
		if (count > 0 && n >= count)
			break;
		n++;
		list_move_tail(&w->node, &selected);
		w->queue = &selected;
	}
	/*
	 * Donation may reorder another CV waiter that owns this waiter's lock.
	 * Select the wake batch first, then always take its current first entry;
	 * no iterator survives a priority change, and selection stays linear.
	 */
	while (!list_empty(&selected)) {
		w = list_first_entry(&selected, struct hzn_waiter, node);
		list_del_init(&w->node);
		/* Take the waiter's lock for it, or queue it for the lock. */
		if (hzn_update_lock(w->addr, w->tag, &prev))
			hzn_end_wait(w, HZN_RESULT_INVALID_CURRENT_MEMORY,
				     &wake_q);
		else if (!prev)
			hzn_end_wait(w, HZN_RESULT_SUCCESS, &wake_q);
		else {
			u32 handle = prev & ~HZN_HANDLE_WAIT_MASK;
			struct task_struct *owner = hzn_is_pseudo_handle(handle) ?
				NULL : hzn_handle_to_thread(handle);

			if (!owner || hzn_owner_cycle(owner, w->task)) {
				hzn_end_wait(w, HZN_RESULT_INVALID_STATE, &wake_q);
			} else {
				hzn_enqueue(&proc->lock_waiters, w);
				hzn_set_owner(w, owner);
			}
			if (owner)
				put_task_struct(owner);
		}
	}
	if (!hzn_key_has_waiters(proc, key))
		put_user(0, key);
	hzn_arb_unlock(proc, &wake_q);
	return HZN_RESULT_SUCCESS;
}

long hzn_wait_for_address(u32 __user *addr, u32 type, s32 value,
			  s64 timeout)
{
	struct hzn_process *proc = hzn_current()->proc;
	struct hzn_waiter w;
	u32 cur, seen;

	if (hzn_bad_address(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (type > HZN_ARB_WAIT_IF_EQUAL)
		return HZN_RESULT_INVALID_ENUM_VALUE;

	rt_mutex_lock(&proc->arb_lock);
	if (get_user(cur, addr))
		goto fault;
	switch (type) {
	case HZN_ARB_WAIT_IF_LESS_THAN:
		if ((s32)cur >= value)
			goto invalid_state;
		break;
	case HZN_ARB_DECREMENT_AND_WAIT_IF_LESS_THAN:
		for (;;) {
			if ((s32)cur >= value)
				goto invalid_state;
			if (hzn_cmpxchg_word(addr, cur, cur - 1, &seen))
				goto fault;
			if (seen == cur)
				break;
			cur = seen;
		}
		break;
	case HZN_ARB_WAIT_IF_EQUAL:
		if ((s32)cur != value)
			goto invalid_state;
		break;
	}
	if (timeout == 0) {
		hzn_arb_unlock(proc, NULL);
		return HZN_RESULT_TIMED_OUT;
	}

	hzn_waiter_init(&w);
	w.addr = addr;
	hzn_enqueue(&proc->addr_waiters, &w);
	hzn_arb_unlock(proc, NULL);
	return hzn_wait(proc, &w, timeout);

invalid_state:
	hzn_arb_unlock(proc, NULL);
	return HZN_RESULT_INVALID_STATE;
fault:
	hzn_arb_unlock(proc, NULL);
	return HZN_RESULT_INVALID_CURRENT_MEMORY;
}

/* Wakes up to @count (all if <= 0) waiters on @addr, best priority first. */
static void hzn_wake_address(struct hzn_process *proc, u32 __user *addr,
			     s32 count, struct wake_q_head *wake_q)
{
	struct hzn_waiter *w, *tmp;
	s32 n = 0;

	list_for_each_entry_safe(w, tmp, &proc->addr_waiters, node) {
		if (w->addr != addr)
			continue;
		if (count > 0 && n >= count)
			break;
		n++;
		hzn_end_wait(w, HZN_RESULT_SUCCESS, wake_q);
	}
}

/* KAddressArbiter::SignalAndModifyByWaitingCountIfEqual's new value. */
static s32 hzn_modified_value(struct hzn_process *proc, u32 __user *addr,
			      s32 value, s32 count)
{
	struct hzn_waiter *w;
	s32 waiters = 0;

	list_for_each_entry(w, &proc->addr_waiters, node) {
		if (w->addr != addr)
			continue;
		/* Only whether the count exceeds the wake limit matters. */
		if (count > 0 && waiters == count)
			return value;
		waiters++;
		if (count <= 0)
			break;
	}
	return waiters ? (u32)value - 1 : (u32)value + 1;
}

long hzn_signal_to_address(u32 __user *addr, u32 type, s32 value, s32 count)
{
	struct hzn_process *proc = hzn_current()->proc;
	DEFINE_WAKE_Q(wake_q);
	long ret = HZN_RESULT_SUCCESS;
	u32 cur;
	s32 new;

	if (hzn_bad_address(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (type > HZN_SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL)
		return HZN_RESULT_INVALID_ENUM_VALUE;

	rt_mutex_lock(&proc->arb_lock);
	switch (type) {
	case HZN_SIGNAL:
		break;
	case HZN_SIGNAL_AND_INCREMENT_IF_EQUAL:
		if (hzn_cmpxchg_word(addr, value, value + 1, &cur))
			ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
		else if ((s32)cur != value)
			ret = HZN_RESULT_INVALID_STATE;
		break;
	case HZN_SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL:
		new = hzn_modified_value(proc, addr, value, count);
		if (new != value) {
			if (hzn_cmpxchg_word(addr, value, new, &cur))
				ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
		} else if (get_user(cur, addr)) {
			ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
		}
		if (ret == HZN_RESULT_SUCCESS && (s32)cur != value)
			ret = HZN_RESULT_INVALID_STATE;
		break;
	}
	if (ret == HZN_RESULT_SUCCESS)
		hzn_wake_address(proc, addr, count, &wake_q);
	hzn_arb_unlock(proc, &wake_q);
	return ret;
}
