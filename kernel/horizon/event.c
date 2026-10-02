// SPDX-License-Identifier: GPL-2.0
/* Horizon manual-reset events: distinct readable and writable handles. */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/eventfd.h>
#include <linux/poll.h>
#include <linux/slab.h>

#include "internal.h"

struct hzn_event {
	struct kref ref;
	spinlock_t lock;
	wait_queue_head_t wait;
	bool signalled;
};

static void hzn_event_free(struct kref *ref)
{
	kfree(container_of(ref, struct hzn_event, ref));
}

static int hzn_event_release(struct inode *inode, struct file *file)
{
	struct hzn_event *event = file->private_data;

	kref_put(&event->ref, hzn_event_free);
	return 0;
}

static __poll_t hzn_event_poll(struct file *file, poll_table *pt)
{
	struct hzn_event *event = file->private_data;

	poll_wait(file, &event->wait, pt);
	return READ_ONCE(event->signalled) ? EPOLLIN : 0;
}

static const struct file_operations hzn_event_write_fops = {
	.release = hzn_event_release,
};

static const struct file_operations hzn_event_read_fops = {
	.poll = hzn_event_poll,
	.release = hzn_event_release,
};

long hzn_create_event(u32 *write_handle, u32 *read_handle)
{
	struct hzn_event *event;
	struct file *write_file, *read_file;

	event = kzalloc_obj(*event);
	if (!event)
		return HZN_RESULT_OUT_OF_MEMORY;
	kref_init(&event->ref);
	spin_lock_init(&event->lock);
	init_waitqueue_head(&event->wait);
	write_file = anon_inode_getfile("[horizon writable event]",
				       &hzn_event_write_fops, event, O_WRONLY);
	if (IS_ERR(write_file)) {
		kref_put(&event->ref, hzn_event_free);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	kref_get(&event->ref);
	read_file = anon_inode_getfile("[horizon readable event]",
				      &hzn_event_read_fops, event, O_RDONLY);
	if (IS_ERR(read_file)) {
		kref_put(&event->ref, hzn_event_free);
		fput(write_file);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	return hzn_handle_add_pair(write_file, read_file, write_handle, read_handle);
}

long hzn_signal_event(u32 handle)
{
	struct file *file = hzn_handle_get(handle);
	struct hzn_event *event;
	bool changed;

	if (!file)
		return HZN_RESULT_INVALID_HANDLE;
	if (file->f_op != &hzn_event_write_fops) {
		struct eventfd_ctx *ctx = eventfd_ctx_fileget(file);
		fput(file);
		if (IS_ERR(ctx))
			return HZN_RESULT_INVALID_HANDLE;
		eventfd_signal(ctx);
		eventfd_ctx_put(ctx);
		return HZN_RESULT_SUCCESS;
	}
	event = file->private_data;
	spin_lock(&event->lock);
	changed = !event->signalled;
	WRITE_ONCE(event->signalled, true);
	spin_unlock(&event->lock);
	if (changed)
		wake_up_all(&event->wait);
	fput(file);
	return HZN_RESULT_SUCCESS;
}

/* Legacy service-created eventfds are handled by the caller. */
bool hzn_clear_native_event(struct file *file, bool reset, long *result)
{
	struct hzn_event *event;

	if (file->f_op != &hzn_event_read_fops &&
	    file->f_op != &hzn_event_write_fops)
		return false;
	if (reset && file->f_op != &hzn_event_read_fops) {
		*result = HZN_RESULT_INVALID_HANDLE;
		return true;
	}
	event = file->private_data;
	spin_lock(&event->lock);
	*result = reset && !event->signalled ? HZN_RESULT_INVALID_STATE :
					    HZN_RESULT_SUCCESS;
	WRITE_ONCE(event->signalled, false);
	spin_unlock(&event->lock);
	return true;
}
