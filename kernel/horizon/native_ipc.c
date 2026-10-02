// SPDX-License-Identifier: GPL-2.0
/* Horizon-to-Horizon HIPC; Linux horizon_servctl sessions stay in ipc.c. */
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/poll.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "internal.h"

#define HZN_TLS_COMMAND_SIZE 0x100
#define HZN_IPC_MAX_WORDS 2073 /* maximum 11-bit C-list offset plus 13 entries */
#define HZN_IPC_HANDLES 30
#define HZN_IPC_BUFFERS 60

struct hzn_ipc_buffer {
	struct file *file;
	pgoff_t offset;
	unsigned long source, size, map_size, mapped;
	struct mm_struct *map_mm;
	u16 word, head;
	u8 index;
	bool pointer, output, bounce;
};
struct hzn_ipc_message {
	u32 *words;
	size_t length;
	u32 handles[HZN_IPC_HANDLES];
	struct file *files[HZN_IPC_HANDLES];
	u16 handle_word;
	u8 copies, moves, nr_buffers;
	struct hzn_ipc_buffer buffers[];
};
struct hzn_native_request {
	struct kref ref;
	struct list_head node;
	struct completion done;
	struct hzn_ipc_message *message, *reply;
	long result;
};
struct hzn_native_port;
struct hzn_native_session {
	struct kref ref;
	struct mutex lock;
	wait_queue_head_t wait;
	struct list_head requests;
	struct hzn_native_request *active;
	struct hzn_native_port *port;
	bool client_closed, server_closed, light;
};
struct hzn_pending_session {
	struct list_head node;
	struct file *server;
};
struct hzn_native_port {
	struct kref ref;
	struct mutex lock;
	wait_queue_head_t wait;
	struct list_head pending;
	u32 maximum, sessions;
	bool client_closed, server_closed, light;
};
static const struct file_operations hzn_native_client_fops, hzn_native_server_fops;
static const struct file_operations hzn_client_port_fops, hzn_server_port_fops;

long hzn_pin_ipc_user_buffer(unsigned long addr, u64 size, struct page ***pages,
			     unsigned long *count)
{
	struct page **p;
	unsigned long nr;
	long pinned;

	if (!PAGE_ALIGNED(addr))
		return HZN_RESULT_INVALID_ADDRESS;
	if (!size || !PAGE_ALIGNED(size))
		return HZN_RESULT_INVALID_SIZE;
	if (addr + size <= addr || addr + size > hzn_current()->proc->space_end)
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	nr = size >> PAGE_SHIFT;
	if (nr > INT_MAX)
		return HZN_RESULT_INVALID_SIZE;
	p = kvmalloc_array(nr, sizeof(*p), GFP_KERNEL);
	if (!p)
		return HZN_RESULT_OUT_OF_MEMORY;
	pinned = pin_user_pages_fast(addr, nr, FOLL_WRITE, p);
	if (pinned != nr) {
		if (pinned > 0)
			unpin_user_pages(p, pinned);
		kvfree(p);
		return HZN_RESULT_INVALID_CURRENT_MEMORY;
	}
	*pages = p;
	*count = nr;
	return HZN_RESULT_SUCCESS;
}

static void hzn_ipc_unmap_buffer(struct hzn_ipc_buffer *buf)
{
	if (!buf->map_mm)
		return;
	if (mmget_not_zero(buf->map_mm)) {
		mmap_write_lock(buf->map_mm);
		/* VMAs may have merged or split; verify every page offset. */
		{
			unsigned long a = buf->mapped, end = a + buf->map_size;
			struct vm_area_struct *vma;
			while (a < end) {
				vma = vma_lookup(buf->map_mm, a);
				if (!vma || vma->vm_file != buf->file ||
				    vma->vm_pgoff + ((a - vma->vm_start) >> PAGE_SHIFT) !=
				    buf->offset + ((a - buf->mapped) >> PAGE_SHIFT))
					break;
				a = min(vma->vm_end, end);
			}
			if (a == end)
				do_munmap(buf->map_mm, buf->mapped, buf->map_size, NULL);
		}
		mmap_write_unlock(buf->map_mm);
		mmput(buf->map_mm);
	}
	mmdrop(buf->map_mm);
	buf->map_mm = NULL;
}

static void hzn_ipc_message_free(struct hzn_ipc_message *msg)
{
	int i;

	if (!msg)
		return;
	for (i = 0; i < msg->copies + msg->moves; i++)
		if (msg->files[i])
			fput(msg->files[i]);
	for (i = 0; i < msg->nr_buffers; i++) {
		hzn_ipc_unmap_buffer(&msg->buffers[i]);
		if (msg->buffers[i].file)
			fput(msg->buffers[i].file);
	}
	kfree(msg->words);
	kfree(msg);
}

static long hzn_ipc_capture_buffer(struct hzn_ipc_buffer *buf, unsigned long addr,
				   unsigned long size, bool input)
{
	void *page;
	loff_t pos = 0;
	unsigned long off, n;
	long ret = HZN_RESULT_INVALID_CURRENT_MEMORY;

	buf->source = addr;
	buf->size = size;
	buf->head = addr & ~PAGE_MASK;
	buf->map_size = PAGE_ALIGN(buf->head + size);
	pos = buf->head;
	if (!size)
		return HZN_RESULT_SUCCESS;
	if (addr + size <= addr || addr + size > hzn_current()->proc->space_end)
		return ret;
	{
		struct vm_area_struct *vma;
		unsigned long a = addr, end = addr + size;
		unsigned long need = (input ? VM_READ : 0) | (buf->output ? VM_WRITE : 0);
		mmap_read_lock(current->mm);
		while (a < end) {
			vma = vma_lookup(current->mm, a);
			if (!vma || (vma->vm_flags & need) != need)
				break;
			a = min(vma->vm_end, end);
		}
		mmap_read_unlock(current->mm);
		if (a != end)
			return ret;
	}
	/* Aligned shmem buffers use the same pages, without a payload copy. */
	if (!buf->pointer && PAGE_ALIGNED(addr) && PAGE_ALIGNED(size)) {
		buf->file = hzn_find_shared(current->mm, addr, size, &buf->offset);
		if (buf->file)
			return HZN_RESULT_SUCCESS;
	}
	buf->bounce = true;
	buf->file = shmem_file_setup("horizon_ipc", buf->map_size, EMPTY_VMA_FLAGS);
	if (IS_ERR(buf->file)) {
		buf->file = NULL;
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	if (!input) {
		/* Validate output ranges now, before accepting a request. */
		struct page *p;
		for (off = addr & PAGE_MASK; off < PAGE_ALIGN(addr + size); off += PAGE_SIZE) {
			if (pin_user_pages_fast(off, 1, FOLL_WRITE, &p) != 1)
				return ret;
			unpin_user_page(p);
		}
		return HZN_RESULT_SUCCESS;
	}
	page = (void *)__get_free_page(GFP_KERNEL);
	if (!page)
		return HZN_RESULT_OUT_OF_MEMORY;
	ret = HZN_RESULT_SUCCESS;
	for (off = 0; off < size; off += n) {
		n = min_t(unsigned long, PAGE_SIZE, size - off);
		if (copy_from_user(page, (void __user *)(addr + off), n) ||
		    kernel_write(buf->file, page, n, &pos) != n) {
			ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
			break;
		}
	}
	free_page((unsigned long)page);
	return ret;
}

static long hzn_ipc_capture(unsigned long addr, u64 size, bool reply,
			    struct hzn_ipc_message **out)
{
	struct hzn_ipc_message *msg;
	u32 header[3], special = 0, *w;
	unsigned int i, pos = 2, ptr, send, recv, exch, total, wire, raw, ccount, coff;
	long ret = HZN_RESULT_INVALID_COMBINATION;

	if (size < 8 || copy_from_user(header, (void __user *)addr, 8))
		return HZN_RESULT_INVALID_POINTER;
	ptr = (header[0] >> 16) & 15;
	send = (header[0] >> 20) & 15;
	recv = (header[0] >> 24) & 15;
	exch = header[0] >> 28;
	raw = header[1] & 1023;
	ccount = (header[1] >> 10) & 15;
	coff = (header[1] >> 20) & 2047;
	if (reply && (send || recv || exch))
		return ret;
	if (header[1] & BIT(31)) {
		if (copy_from_user(&special, (void __user *)(addr + 8), 4))
			return HZN_RESULT_INVALID_POINTER;
		pos++;
		if (special & 1)
			pos += 2;
		pos += ((special >> 1) & 15) + ((special >> 5) & 15);
	}
	total = pos + 2 * ptr + 3 * (send + recv + exch) + raw;
	wire = total;
	if (ccount >= 2)
		total = max(total, (coff ? coff : total) + 2 * (ccount == 2 ? 1 : ccount - 2));
	if (total > HZN_IPC_MAX_WORDS || total * 4 > size)
		return ret;
	msg = kzalloc(struct_size(msg, buffers, ptr + send + recv + exch), GFP_KERNEL);
	if (!msg)
		return HZN_RESULT_OUT_OF_MEMORY;
	msg->length = wire * 4;
	msg->words = memdup_user((void __user *)addr, total * 4);
	if (IS_ERR(msg->words)) {
		msg->words = NULL;
		ret = HZN_RESULT_INVALID_POINTER;
		goto fail;
	}
	w = msg->words;
	pos = 2;
	if (w[1] & BIT(31)) {
		pos++;
		if (special & 1) {
			u64 pid = task_tgid_vnr(current);
			memcpy(w + pos, &pid, 8);
			pos += 2;
		}
		msg->copies = (special >> 1) & 15;
		msg->moves = (special >> 5) & 15;
		msg->handle_word = pos;
		for (i = 0; i < msg->copies + msg->moves; i++) {
			msg->handles[i] = w[pos++];
			if (!msg->handles[i])
				continue;
			if (i >= msg->copies && hzn_is_pseudo_handle(msg->handles[i])) {
				ret = HZN_RESULT_INVALID_HANDLE;
				goto fail;
			}
			if (i >= msg->copies) {
				unsigned int j;
				for (j = msg->copies; j < i; j++)
					if (msg->handles[j] == msg->handles[i]) {
						ret = HZN_RESULT_INVALID_HANDLE;
						goto fail;
					}
			}
			msg->files[i] = hzn_copy_handle_file(msg->handles[i]);
			if (!msg->files[i]) {
				ret = HZN_RESULT_INVALID_HANDLE;
				goto fail;
			}
		}
	}
	for (i = 0; i < ptr + send + recv + exch; i++) {
		struct hzn_ipc_buffer *buf = &msg->buffers[msg->nr_buffers++];
		unsigned long source, length;
		bool input;

		buf->word = pos;
		buf->pointer = i < ptr;
		if (buf->pointer) {
			source = w[pos + 1] | ((u64)((w[pos] >> 12) & 15) << 32) |
				 ((u64)((w[pos] >> 6) & 7) << 36);
			length = w[pos] >> 16;
			buf->index = w[pos] & 15;
			input = true;
			pos += 2;
		} else {
			source = w[pos + 1] | ((u64)(w[pos + 2] >> 28) << 32) |
				 ((u64)((w[pos + 2] >> 2) & 7) << 36);
			length = w[pos] | ((u64)((w[pos + 2] >> 24) & 15) << 32);
			if ((w[pos + 2] & 3) == 2) {
				ret = HZN_RESULT_INVALID_COMBINATION;
				goto fail;
			}
			input = i < ptr + send || i >= ptr + send + recv;
			buf->output = i >= ptr + send;
			pos += 3;
		}
		ret = hzn_ipc_capture_buffer(buf, source, length, input);
		if (ret)
			goto fail;
	}
	*out = msg;
	return HZN_RESULT_SUCCESS;
fail:
	hzn_ipc_message_free(msg);
	return ret;
}

/* Copy a bounce buffer in the current process, never through a foreign mm. */
static long hzn_ipc_copy_buffer(struct hzn_ipc_buffer *buf, unsigned long dst)
{
	void *page = (void *)__get_free_page(GFP_KERNEL);
	loff_t pos = ((loff_t)buf->offset << PAGE_SHIFT) + buf->head;
	unsigned long off, n;
	long ret = HZN_RESULT_SUCCESS;

	if (!page)
		return HZN_RESULT_OUT_OF_MEMORY;
	for (off = 0; off < buf->size; off += n) {
		n = min_t(unsigned long, PAGE_SIZE, buf->size - off);
		if (kernel_read(buf->file, page, n, &pos) != n ||
		    copy_to_user((void __user *)(dst + off), page, n)) {
			ret = HZN_RESULT_INVALID_CURRENT_MEMORY;
			break;
		}
	}
	free_page((unsigned long)page);
	return ret;
}

static unsigned long hzn_ipc_map_buffer(struct hzn_ipc_buffer *buf)
{
	struct hzn_process *proc = hzn_current()->proc;
	unsigned long a = proc->alias_code_start, end = a + proc->alias_code_size, m;
	struct vm_area_struct *vma;
	int retry;

	for (retry = 0; retry < 32; retry++) {
		mmap_read_lock(current->mm);
		for (;;) {
			vma = find_vma(current->mm, a);
			if (!vma || vma->vm_start >= a + buf->map_size)
				break;
			a = vma->vm_end;
		}
		mmap_read_unlock(current->mm);
		if (a >= end || buf->map_size > end - a)
			return -ENOMEM;
		m = vm_mmap(buf->file, a, buf->map_size, buf->output ? 3 : 1,
			    MAP_SHARED | MAP_FIXED_NOREPLACE, (unsigned long)buf->offset << PAGE_SHIFT);
		if (m != (unsigned long)-EEXIST)
			return m;
	}
	return -ENOMEM;
}

static void hzn_ipc_commit_moves(struct hzn_ipc_message *msg)
{
	int i;

	for (i = msg->copies; i < msg->copies + msg->moves; i++)
		if (msg->handles[i])
			hzn_handle_close(msg->handles[i]);
}

/* Reserve all handles before publishing anything, so EMFILE cannot leak a prefix. */
static long hzn_ipc_deliver(struct hzn_ipc_message *msg, unsigned long addr, u64 size)
{
	u32 *receiver;
	size_t receiver_size = min_t(u64, size, HZN_IPC_MAX_WORDS * 4);
	int fds[HZN_IPC_HANDLES], i, reserved = 0;
	unsigned int count, coff, pos, raw_end, consumed = 0;
	unsigned long target, capacity;
	long ret = HZN_RESULT_INVALID_COMBINATION;

	if (msg->length > size)
		return ret;
	receiver = NULL;
	count = coff = raw_end = 0;
	/* Plain messages need no snapshot of the receiver's unused C descriptors. */
	if ((msg->words[0] >> 16) & 15) {
		/* C descriptors belong to the receiver, and must be read before overwrite. */
		receiver = memdup_user((void __user *)addr, receiver_size);
		if (IS_ERR(receiver))
			return HZN_RESULT_INVALID_POINTER;
		count = (receiver[1] >> 10) & 15;
		coff = (receiver[1] >> 20) & 2047;
		pos = 2;
		if (receiver[1] & BIT(31))
			pos += 1 + (receiver[2] & 1 ? 2 : 0) + ((receiver[2] >> 1) & 15) +
			       ((receiver[2] >> 5) & 15);
		pos += 2 * ((receiver[0] >> 16) & 15) +
		       3 * (((receiver[0] >> 20) & 15) + ((receiver[0] >> 24) & 15) + (receiver[0] >> 28));
		raw_end = pos + (receiver[1] & 1023);
		if (!coff)
			coff = raw_end;
	}

	for (i = 0; i < msg->copies + msg->moves; i++) {
		fds[i] = msg->files[i] ? get_unused_fd_flags(0) : -1;
		if (msg->files[i] && fds[i] < 0) {
			ret = HZN_RESULT_OUT_OF_HANDLES;
			goto rollback;
		}
		reserved++;
		msg->words[msg->handle_word + i] = fds[i] + 1;
	}
	for (i = 0; i < msg->nr_buffers; i++) {
		struct hzn_ipc_buffer *buf = &msg->buffers[i];
		u32 *w = msg->words + buf->word;

		if (!buf->size) {
			target = 0;
		} else if (buf->pointer) {
			unsigned int entry;
			ret = HZN_RESULT_INVALID_COMBINATION;
			if (!count) {
				ret = HZN_RESULT_INVALID_COMBINATION;
				goto rollback;
			}
			if (count == 1) {
				target = addr + ALIGN(msg->length, 16) + consumed;
				capacity = size - min_t(u64, size, ALIGN(msg->length, 16) + consumed);
			} else {
				entry = coff + (count == 2 ? 0 : 2 * buf->index);
				if ((count != 2 && buf->index >= count - 2) ||
				    (entry + 2) * 4 > min_t(u64, size, receiver_size))
					goto rollback;
				target = receiver[entry] | ((u64)(receiver[entry + 1] & 127) << 32);
				capacity = receiver[entry + 1] >> 16;
				if (count == 2) {
					target += consumed;
					capacity -= min_t(unsigned long, capacity, consumed);
				}
			}
			if (buf->size > capacity)
				goto rollback;
			ret = hzn_ipc_copy_buffer(buf, target);
			if (ret)
				goto rollback;
			consumed += ALIGN(buf->size, 16);
		} else {
			target = hzn_ipc_map_buffer(buf);
			if (IS_ERR_VALUE(target)) {
				ret = HZN_RESULT_OUT_OF_MEMORY;
				goto rollback;
			}
			buf->mapped = target;
			buf->map_mm = current->mm;
			mmgrab(buf->map_mm);
			target += buf->head;
		}
		if (buf->pointer) {
			w[0] = (w[0] & ~0xf1c0u) | (((target >> 36) & 7) << 6) |
			       (((target >> 32) & 15) << 12);
			w[1] = target;
		} else {
			w[1] = target;
			w[2] = (w[2] & 0x0fffffe3u) | (((target >> 36) & 7) << 2) |
			       (((target >> 32) & 15) << 28);
		}
	}
	/* Sender C-list addresses are private; the receiver keeps its own C list. */
	msg->words[1] &= ~((15u << 10) | (2047u << 20));
	if (copy_to_user((void __user *)addr, msg->words, msg->length)) {
		ret = HZN_RESULT_INVALID_POINTER;
		goto rollback;
	}
	for (i = 0; i < reserved; i++)
		if (fds[i] >= 0)
			fd_install(fds[i], get_file(msg->files[i]));
	kfree(receiver);
	return HZN_RESULT_SUCCESS;
rollback:
	for (i = 0; i < reserved; i++)
		if (fds[i] >= 0)
			put_unused_fd(fds[i]);
	for (i = 0; i < msg->nr_buffers; i++)
		hzn_ipc_unmap_buffer(&msg->buffers[i]);
	kfree(receiver);
	return ret;
}

static void hzn_request_release(struct kref *ref)
{
	struct hzn_native_request *req = container_of(ref, struct hzn_native_request, ref);

	hzn_ipc_message_free(req->message);
	hzn_ipc_message_free(req->reply);
	kfree(req);
}
static void hzn_native_port_free(struct kref *ref)
{
	struct hzn_native_port *port = container_of(ref, struct hzn_native_port, ref);

	WARN_ON(!list_empty(&port->pending));
	kfree(port);
}
static void hzn_native_session_free(struct kref *ref)
{
	struct hzn_native_session *s = container_of(ref, struct hzn_native_session, ref);

	WARN_ON(!list_empty(&s->requests) || s->active);
	if (s->port)
		kref_put(&s->port->ref, hzn_native_port_free);
	kfree(s);
}
static int hzn_native_session_close(struct inode *inode, struct file *file)
{
	struct hzn_native_session *s = file->private_data;
	struct hzn_native_request *req, *tmp;
	LIST_HEAD(failed);
	bool client = file->f_op == &hzn_native_client_fops;

	mutex_lock(&s->lock);
	if (client)
		s->client_closed = true;
	else
		s->server_closed = true;
	list_splice_init(&s->requests, &failed);
	if (s->active) {
		list_add(&s->active->node, &failed);
		s->active = NULL;
	}
	list_for_each_entry(req, &failed, node) {
		int i;
		for (i = 0; i < req->message->nr_buffers; i++)
			hzn_ipc_unmap_buffer(&req->message->buffers[i]);
		req->result = HZN_RESULT_SESSION_CLOSED;
		complete(&req->done);
	}
	mutex_unlock(&s->lock);
	wake_up_all(&s->wait);
	list_for_each_entry_safe(req, tmp, &failed, node) {
		list_del_init(&req->node);
		kref_put(&req->ref, hzn_request_release);
	}
	if (client && s->port) {
		mutex_lock(&s->port->lock);
		s->port->sessions--;
		mutex_unlock(&s->port->lock);
		wake_up_all(&s->port->wait);
	}
	kref_put(&s->ref, hzn_native_session_free);
	return 0;
}
static __poll_t hzn_native_session_poll(struct file *file, poll_table *pt)
{
	struct hzn_native_session *s = file->private_data;
	bool ready;

	poll_wait(file, &s->wait, pt);
	mutex_lock(&s->lock);
	ready = file->f_op == &hzn_native_server_fops ?
		s->client_closed || (!s->active && !list_empty(&s->requests)) : s->server_closed;
	mutex_unlock(&s->lock);
	return ready ? EPOLLIN : 0;
}
static const struct file_operations hzn_native_client_fops = {
	.release = hzn_native_session_close, .poll = hzn_native_session_poll,
};
static const struct file_operations hzn_native_server_fops = {
	.release = hzn_native_session_close, .poll = hzn_native_session_poll,
};

static long hzn_new_session(bool light, struct hzn_native_port *port,
			    struct file **server, struct file **client)
{
	struct hzn_native_session *s = kzalloc_obj(*s);

	if (!s)
		return HZN_RESULT_OUT_OF_MEMORY;
	kref_init(&s->ref);
	mutex_init(&s->lock);
	init_waitqueue_head(&s->wait);
	INIT_LIST_HEAD(&s->requests);
	s->light = light;
	s->port = port;
	if (port)
		kref_get(&port->ref);
	*server = anon_inode_getfile("horizon_server", &hzn_native_server_fops, s, O_RDWR);
	if (IS_ERR(*server)) {
		kref_put(&s->ref, hzn_native_session_free);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	kref_get(&s->ref);
	*client = anon_inode_getfile("horizon_client", &hzn_native_client_fops, s, O_RDWR);
	if (IS_ERR(*client)) {
		kref_put(&s->ref, hzn_native_session_free);
		fput(*server);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	return HZN_RESULT_SUCCESS;
}
long hzn_create_session(bool light, u64 name, u32 *server, u32 *client)
{
	struct file *s, *c;
	long ret = hzn_new_session(light, NULL, &s, &c);

	return ret ? ret : hzn_handle_add_pair(s, c, server, client);
}

static int hzn_port_close(struct inode *inode, struct file *file)
{
	struct hzn_native_port *port = file->private_data;
	struct hzn_pending_session *p, *tmp;
	LIST_HEAD(abandoned);

	mutex_lock(&port->lock);
	if (file->f_op == &hzn_server_port_fops) {
		port->server_closed = true;
		list_splice_init(&port->pending, &abandoned);
	} else {
		port->client_closed = true;
	}
	mutex_unlock(&port->lock);
	list_for_each_entry_safe(p, tmp, &abandoned, node) {
		list_del(&p->node);
		fput(p->server);
		kfree(p);
	}
	wake_up_all(&port->wait);
	kref_put(&port->ref, hzn_native_port_free);
	return 0;
}
static __poll_t hzn_port_poll(struct file *file, poll_table *pt)
{
	struct hzn_native_port *p = file->private_data;
	bool ready;

	poll_wait(file, &p->wait, pt);
	mutex_lock(&p->lock);
	ready = file->f_op == &hzn_server_port_fops ?
		!list_empty(&p->pending) || p->client_closed : p->server_closed || p->sessions < p->maximum;
	mutex_unlock(&p->lock);
	return ready ? EPOLLIN : 0;
}
static const struct file_operations hzn_server_port_fops = {
	.release = hzn_port_close, .poll = hzn_port_poll,
};
static const struct file_operations hzn_client_port_fops = {
	.release = hzn_port_close, .poll = hzn_port_poll,
};
long hzn_create_port(s32 maximum, bool light, u64 name, u32 *server, u32 *client)
{
	struct hzn_native_port *p;
	struct file *s, *c;

	if (maximum <= 0)
		return HZN_RESULT_OUT_OF_RANGE;
	p = kzalloc_obj(*p);
	if (!p)
		return HZN_RESULT_OUT_OF_MEMORY;
	kref_init(&p->ref);
	mutex_init(&p->lock);
	init_waitqueue_head(&p->wait);
	INIT_LIST_HEAD(&p->pending);
	p->maximum = maximum;
	p->light = light;
	s = anon_inode_getfile("horizon_server_port", &hzn_server_port_fops, p, O_RDWR);
	if (IS_ERR(s)) {
		kref_put(&p->ref, hzn_native_port_free);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	kref_get(&p->ref);
	c = anon_inode_getfile("horizon_client_port", &hzn_client_port_fops, p, O_RDWR);
	if (IS_ERR(c)) {
		kref_put(&p->ref, hzn_native_port_free);
		fput(s);
		return HZN_RESULT_OUT_OF_MEMORY;
	}
	return hzn_handle_add_pair(s, c, server, client);
}

long hzn_connect_to_port(u32 handle, u32 *out)
{
	struct file *file = hzn_handle_get(handle), *server, *client;
	struct hzn_native_port *p;
	struct hzn_pending_session *pending;
	long ret = HZN_RESULT_INVALID_HANDLE;
	int fd;

	if (!file || file->f_op != &hzn_client_port_fops)
		goto put;
	fd = get_unused_fd_flags(0);
	ret = HZN_RESULT_OUT_OF_HANDLES;
	if (fd < 0)
		goto put;
	pending = kmalloc_obj(*pending);
	ret = HZN_RESULT_OUT_OF_MEMORY;
	if (!pending)
		goto unreserve;
	p = file->private_data;
	mutex_lock(&p->lock);
	ret = HZN_RESULT_PORT_CLOSED;
	if (p->server_closed)
		goto unlock;
	ret = HZN_RESULT_OUT_OF_SESSIONS;
	if (p->sessions == p->maximum)
		goto unlock;
	ret = hzn_new_session(p->light, p, &server, &client);
	if (ret)
		goto unlock;
	p->sessions++;
	pending->server = server;
	list_add_tail(&pending->node, &p->pending);
	mutex_unlock(&p->lock);
	fd_install(fd, client);
	*out = fd + 1;
	wake_up_all(&p->wait);
	fput(file);
	return HZN_RESULT_SUCCESS;
unlock:
	mutex_unlock(&p->lock);
	kfree(pending);
unreserve:
	put_unused_fd(fd);
put:
	if (file)
		fput(file);
	return ret;
}
long hzn_accept_session(u32 handle, u32 *out)
{
	struct file *file = hzn_handle_get(handle);
	struct hzn_native_port *p;
	struct hzn_pending_session *pending;
	long ret = HZN_RESULT_INVALID_HANDLE;
	int fd;

	if (!file || file->f_op != &hzn_server_port_fops)
		goto put;
	fd = get_unused_fd_flags(0);
	ret = HZN_RESULT_OUT_OF_HANDLES;
	if (fd < 0)
		goto put;
	p = file->private_data;
	mutex_lock(&p->lock);
	if (list_empty(&p->pending)) {
		ret = HZN_RESULT_NOT_FOUND;
		mutex_unlock(&p->lock);
		put_unused_fd(fd);
		goto put;
	}
	pending = list_first_entry(&p->pending, struct hzn_pending_session, node);
	list_del(&pending->node);
	mutex_unlock(&p->lock);
	fd_install(fd, pending->server);
	kfree(pending);
	*out = fd + 1;
	ret = HZN_RESULT_SUCCESS;
put:
	if (file)
		fput(file);
	return ret;
}

bool hzn_is_native_client(struct file *file)
{
	return file->f_op == &hzn_native_client_fops;
}

long hzn_native_send(struct file *file, unsigned long addr, u64 size)
{
	struct hzn_native_session *s = file->private_data;
	struct hzn_native_request *req;
	long ret;
	int i;

	if (s->light)
		return HZN_RESULT_INVALID_HANDLE;
	mutex_lock(&s->lock);
	ret = s->server_closed ? HZN_RESULT_SESSION_CLOSED : HZN_RESULT_SUCCESS;
	mutex_unlock(&s->lock);
	if (ret)
		return ret;
	req = kzalloc_obj(*req);
	if (!req)
		return HZN_RESULT_OUT_OF_MEMORY;
	kref_init(&req->ref);
	INIT_LIST_HEAD(&req->node);
	init_completion(&req->done);
	ret = hzn_ipc_capture(addr, size, false, &req->message);
	if (ret)
		goto put;
	mutex_lock(&s->lock);
	if (s->server_closed) {
		mutex_unlock(&s->lock);
		ret = HZN_RESULT_SESSION_CLOSED;
		goto put;
	}
	kref_get(&req->ref);
	list_add_tail(&req->node, &s->requests);
	hzn_ipc_commit_moves(req->message);
	mutex_unlock(&s->lock);
	wake_up_all(&s->wait);
	do {
		ret = wait_for_completion_state(&req->done, HZN_WAIT_STATE);
	} while (ret && !hzn_check_signals());
	if (ret) {
		mutex_lock(&s->lock);
		if (!completion_done(&req->done)) {
			if (s->active == req)
				s->active = NULL;
			if (!list_empty(&req->node))
				list_del_init(&req->node);
			req->result = HZN_RESULT_TERMINATION_REQUESTED;
			complete(&req->done);
			kref_put(&req->ref, hzn_request_release);
		}
		mutex_unlock(&s->lock);
		wake_up_all(&s->wait);
	}
	ret = req->result;
	if (!ret) {
		for (i = 0; i < req->message->nr_buffers; i++) {
			struct hzn_ipc_buffer *buf = &req->message->buffers[i];
			if (buf->output && buf->bounce) {
				ret = hzn_ipc_copy_buffer(buf, buf->source);
				if (ret)
					break;
			}
		}
		if (!ret)
			ret = hzn_ipc_deliver(req->reply, addr, size);
	}
put:
	kref_put(&req->ref, hzn_request_release);
	return ret;
}

static long hzn_native_reply(struct file *file, unsigned long addr, u64 size)
{
	struct hzn_native_session *s = file->private_data;
	struct hzn_native_request *req;
	struct hzn_ipc_message *reply;
	long ret;
	int i;

	if (s->light)
		return HZN_RESULT_INVALID_HANDLE;
	ret = hzn_ipc_capture(addr, size, true, &reply);
	if (ret)
		return ret;
	mutex_lock(&s->lock);
	req = s->active;
	ret = HZN_RESULT_INVALID_STATE;
	if (!req)
		goto unlock;
	s->active = NULL;
	/* Remove the server's aliases before allowing the client to reuse its pages. */
	for (i = 0; i < req->message->nr_buffers; i++)
		hzn_ipc_unmap_buffer(&req->message->buffers[i]);
	hzn_ipc_commit_moves(reply);
	req->reply = reply;
	reply = NULL;
	req->result = HZN_RESULT_SUCCESS;
	complete(&req->done);
	kref_put(&req->ref, hzn_request_release);
	ret = HZN_RESULT_SUCCESS;
unlock:
	mutex_unlock(&s->lock);
	wake_up_all(&s->wait);
	hzn_ipc_message_free(reply);
	return ret;
}

static long hzn_native_receive(struct file *file, unsigned long addr, u64 size)
{
	struct hzn_native_session *s = file->private_data;
	struct hzn_native_request *req;
	long ret = HZN_RESULT_NOT_FOUND;

	if (s->light)
		return HZN_RESULT_INVALID_HANDLE;
	mutex_lock(&s->lock);
	if (s->client_closed) {
		ret = HZN_RESULT_SESSION_CLOSED;
		goto unlock;
	}
	if (s->active || list_empty(&s->requests))
		goto unlock;
	req = list_first_entry(&s->requests, struct hzn_native_request, node);
	list_del_init(&req->node);
	s->active = req;
	ret = hzn_ipc_deliver(req->message, addr, size);
	if (ret) {
		s->active = NULL;
		req->result = ret;
		complete(&req->done);
		kref_put(&req->ref, hzn_request_release);
	}
unlock:
	mutex_unlock(&s->lock);
	return ret;
}

long hzn_reply_and_receive(unsigned long addr, u64 size, u32 __user *handles,
			   s32 num, u32 target, s64 timeout, s32 *index)
{
	u32 ids[64];
	struct file *files[64], *reply = NULL;
	struct poll_wqueues table;
	ktime_t expires = 0;
	long ret = HZN_RESULT_SUCCESS;
	int i, got = 0;

	*index = -1;
	if (num < 0 || num > 64)
		return HZN_RESULT_OUT_OF_RANGE;
	if (num && copy_from_user(ids, handles, num * sizeof(u32)))
		return HZN_RESULT_INVALID_POINTER;
	for (i = 0; i < num; i++) {
		files[i] = hzn_handle_get(ids[i]);
		if (files[i])
			got++;
		if (!files[i] || !file_can_poll(files[i])) {
			ret = HZN_RESULT_INVALID_HANDLE;
			goto put;
		}
	}
	if (target) {
		reply = hzn_handle_get(target);
		if (!reply || reply->f_op != &hzn_native_server_fops) {
			ret = HZN_RESULT_INVALID_HANDLE;
			goto put;
		}
		ret = hzn_native_reply(reply, addr, size);
		if (ret)
			goto put;
	}
	if (timeout > 0)
		expires = ktime_add_safe(ktime_get(), ns_to_ktime(timeout));
	poll_initwait(&table);
	for (;;) {
		for (i = 0; i < num; i++) {
			if (!(vfs_poll(files[i], &table.pt) & EPOLLIN))
				continue;
			ret = files[i]->f_op == &hzn_native_server_fops ?
				hzn_native_receive(files[i], addr, size) : HZN_RESULT_SUCCESS;
			if (ret == HZN_RESULT_NOT_FOUND)
				continue;
			*index = i;
			goto done;
		}
		table.pt._qproc = NULL;
		if (table.error) {
			ret = HZN_RESULT_OUT_OF_MEMORY;
			break;
		}
		if (!timeout) {
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
		set_current_state(HZN_WAIT_STATE);
		if (!READ_ONCE(table.triggered) && !hzn_wait_cancelled(false)) {
			if (timeout > 0) {
				if (!schedule_hrtimeout_range(&expires, current->timer_slack_ns,
							 HRTIMER_MODE_ABS))
					timeout = 0;
			} else {
				schedule();
			}
		}
		__set_current_state(TASK_RUNNING);
		smp_store_mb(table.triggered, 0);
	}
done:
	poll_freewait(&table);
put:
	if (reply)
		fput(reply);
	while (got)
		fput(files[--got]);
	return ret;
}
