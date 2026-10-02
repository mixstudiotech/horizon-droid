// SPDX-License-Identifier: GPL-2.0
/*
 * Horizon IPC: sessions, named ports and synchronous requests, served by
 * ordinary Linux processes through horizon_servctl(2).
 *
 * svcSendSyncRequest queues a request on the service task and sleeps. The
 * service takes it with HZN_SCTL_GET_CMD (a copy of the requester's command
 * page: its TLS page, or the first page of its buffer for the WithUserBuffer
 * SVCs) and answers with HZN_SCTL_PUT_CMD. Whatever has to happen in
 * the requester's handle table or address space meanwhile (new handles,
 * sharing memory) is done by the requester itself: the service posts an
 * operation, the sleeping requester runs it in its own context and the
 * service waits for the result. No other task's file table or mm is ever
 * modified from here.
 *
 * Commands that name a process by pid need ptrace access to it, as
 * process_vm_readv(2) does. Named ports are per user: a process only
 * connects to services of its own real user ID.
 */

#include <linux/anon_inodes.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/oom.h>
#include <linux/pid.h>
#include <linux/ptrace.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

#include "internal.h"

#define SERVCTL_RET(result)	(-(long)(result))

#define HZN_PORT_NAME_MAX_LENGTH 11

struct hzn_named_service {
	char name[HZN_PORT_NAME_MAX_LENGTH + 1];
	kuid_t uid;			/* of the registering service */
	struct task_struct *service;
	struct list_head entry;
};

static LIST_HEAD(hzn_named_services);
static DEFINE_SPINLOCK(hzn_named_services_lock);

static const struct file_operations hzn_session_fops;

/* Requests */

static struct hzn_request *hzn_request_alloc(void)
{
	struct hzn_request *req = kzalloc_obj(*req);

	if (!req)
		return NULL;
	kref_init(&req->ref);
	INIT_LIST_HEAD(&req->node);
	atomic_set(&req->state, HZN_REQ_PENDING);
	spin_lock_init(&req->op_lock);
	init_completion(&req->op_done);
	return req;
}

static void hzn_request_release(struct kref *ref)
{
	struct hzn_request *req = container_of(ref, struct hzn_request, ref);

	if (req->cmd)
		unpin_user_pages_dirty_lock(&req->cmd, 1, true);
	if (req->session_file)
		fput(req->session_file);
	if (req->requester)
		put_task_struct(req->requester);
	kfree(req);
}

static void hzn_request_put(struct hzn_request *req)
{
	kref_put(&req->ref, hzn_request_release);
}

static void hzn_request_finish(struct hzn_request *req, int state)
{
	if (atomic_cmpxchg(&req->state, HZN_REQ_PENDING, state) == HZN_REQ_PENDING &&
	    req->requester)
		wake_up_process(req->requester);
}

/* Service side: has the requester run @op and returns its result. */
static long hzn_request_post_op(struct hzn_request *req, int op,
				struct file *file, unsigned long addr,
				unsigned long len)
{
	spin_lock(&req->op_lock);
	if (atomic_read(&req->state) != HZN_REQ_PENDING) {
		spin_unlock(&req->op_lock);
		return -ESRCH;
	}
	req->op = op;
	req->op_file = file;
	req->op_addr = addr;
	req->op_len = len;
	req->op_result = -ESRCH;
	reinit_completion(&req->op_done);
	spin_unlock(&req->op_lock);

	wake_up_process(req->requester);
	wait_for_completion(&req->op_done);
	return req->op_result;
}

/* Requester side. */
static void hzn_request_run_op(struct hzn_request *req)
{
	struct file *file = NULL;
	long ret;
	int op;

	spin_lock(&req->op_lock);
	op = req->op;
	req->op = HZN_OP_NONE;
	spin_unlock(&req->op_lock);

	switch (op) {
	case HZN_OP_INSTALL_FILE:
		/* Consumes the file reference, also on failure. */
		ret = hzn_handle_add(req->op_file);
		req->op_file = NULL;
		if (ret == HZN_INVALID_HANDLE)
			ret = -EMFILE;
		break;
	case HZN_OP_MAKE_SHARED:
		ret = hzn_make_shared(req->op_addr, req->op_len, &file);
		req->op_file = file;
		break;
	default:
		return;
	}
	req->op_result = ret;
	complete(&req->op_done);
}

/* The requester is going away: fail a pending operation. */
static void hzn_request_abandon(struct hzn_request *req)
{
	int op;

	spin_lock(&req->op_lock);
	atomic_cmpxchg(&req->state, HZN_REQ_PENDING, HZN_REQ_ABANDONED);
	op = req->op;
	req->op = HZN_OP_NONE;
	spin_unlock(&req->op_lock);
	if (op != HZN_OP_NONE)
		complete(&req->op_done);
}

static long hzn_request_wait(struct hzn_request *req)
{
	int state;

	for (;;) {
		set_current_state(HZN_WAIT_STATE);
		state = atomic_read(&req->state);
		if (state != HZN_REQ_PENDING)
			break;
		if (READ_ONCE(req->op) != HZN_OP_NONE) {
			__set_current_state(TASK_RUNNING);
			hzn_request_run_op(req);
			continue;
		}
		if (hzn_check_signals())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);

	if (state == HZN_REQ_PENDING) {
		hzn_request_abandon(req);
		return HZN_RESULT_TERMINATION_REQUESTED;
	}
	return state == HZN_REQ_HANDLED ? HZN_RESULT_SUCCESS :
					  HZN_RESULT_SESSION_CLOSED;
}

/* Services */

static struct hzn_service *hzn_service_of(struct task_struct *task)
{
	/* Pairs with the cmpxchg() that publishes a new one. */
	struct hzn_service *svc = smp_load_acquire(&task->hzn_service), *old;

	if (svc)
		return svc;
	svc = kzalloc_obj(*svc);
	if (!svc)
		return NULL;
	spin_lock_init(&svc->lock);
	INIT_LIST_HEAD(&svc->requests);
	old = cmpxchg(&task->hzn_service, NULL, svc);
	if (old) {
		kfree(svc);
		return old;
	}
	return svc;
}

static bool hzn_service_queue(struct task_struct *task, struct hzn_service *svc,
			      struct hzn_request *req)
{
	bool queued;

	spin_lock(&svc->lock);
	queued = !svc->stopped && !(READ_ONCE(task->flags) & PF_EXITING);
	if (queued)
		list_add_tail(&req->node, &svc->requests);
	spin_unlock(&svc->lock);
	if (queued)
		wake_up_process(task);
	return queued;
}

/* From do_exit() of a service task. */
void hzn_service_exit(struct task_struct *tsk)
{
	struct hzn_service *svc = tsk->hzn_service;
	struct hzn_named_service *ns, *tmp_ns;
	struct hzn_request *req, *tmp;
	LIST_HEAD(names);
	LIST_HEAD(reqs);
	struct hzn_request *cur;

	spin_lock(&hzn_named_services_lock);
	list_for_each_entry_safe(ns, tmp_ns, &hzn_named_services, entry)
		if (ns->service == tsk)
			list_move(&ns->entry, &names);
	spin_unlock(&hzn_named_services_lock);
	list_for_each_entry_safe(ns, tmp_ns, &names, entry) {
		put_task_struct(ns->service);
		kfree(ns);
	}

	spin_lock(&svc->lock);
	svc->stopped = true;
	list_splice_init(&svc->requests, &reqs);
	cur = svc->cur;
	svc->cur = NULL;
	spin_unlock(&svc->lock);

	list_for_each_entry_safe(req, tmp, &reqs, node) {
		list_del_init(&req->node);
		hzn_request_finish(req, HZN_REQ_FAILED);
		hzn_request_put(req);
	}
	if (cur) {
		hzn_request_finish(cur, HZN_REQ_FAILED);
		hzn_request_put(cur);
	}
}

/* From __put_task_struct(); the service was stopped in hzn_service_exit(). */
void hzn_service_free(struct hzn_service *svc)
{
	WARN_ON(!list_empty(&svc->requests) || svc->cur);
	kfree(svc);
}

/* Sessions */

static int hzn_session_release(struct inode *inode, struct file *file)
{
	struct hzn_session *session = file->private_data;
	struct task_struct *task = NULL;
	struct hzn_service *svc;
	struct hzn_request *req;

	if (session->service && session->id)
		task = get_pid_task(session->service, PIDTYPE_PID);
	if (task) {
		/* A request without a command tells the service to clean up. */
		svc = hzn_service_of(task);
		req = svc ? hzn_request_alloc() : NULL;
		if (req) {
			req->close_session_id = session->id;
			if (!hzn_service_queue(task, svc, req))
				hzn_request_put(req);
		}
		put_task_struct(task);
	}
	put_pid(session->service);
	kfree(session);
	return 0;
}

static const struct file_operations hzn_session_fops = {
	.release	= hzn_session_release,
};

bool hzn_is_session(struct file *file)
{
	return file->f_op == &hzn_session_fops;
}

/* Consumes the reference to @service. */
static struct file *hzn_session_file(struct pid *service, unsigned long id)
{
	struct hzn_session *session = kzalloc_obj(*session);
	struct file *file;

	if (!session) {
		put_pid(service);
		return ERR_PTR(-ENOMEM);
	}
	session->service = service;
	session->id = id;
	file = anon_inode_getfile("[horizon session]", &hzn_session_fops,
				  session, O_RDWR);
	if (IS_ERR(file)) {
		put_pid(service);
		kfree(session);
	}
	return file;
}

long hzn_connect_to_named_port(const char __user *uname, u32 *handle)
{
	char name[HZN_PORT_NAME_MAX_LENGTH + 1];
	struct hzn_named_service *ns;
	struct pid *service = NULL;
	struct file *file;
	u32 h;

	if (strncpy_from_user(name, uname, sizeof(name)) < 0)
		return HZN_RESULT_INVALID_ADDRESS;
	name[sizeof(name) - 1] = 0;

	spin_lock(&hzn_named_services_lock);
	list_for_each_entry(ns, &hzn_named_services, entry) {
		if (!strcmp(ns->name, name) && uid_eq(ns->uid, current_uid())) {
			service = get_task_pid(ns->service, PIDTYPE_PID);
			break;
		}
	}
	spin_unlock(&hzn_named_services_lock);
	if (!service)
		return HZN_RESULT_NOT_FOUND;

	file = hzn_session_file(service, 0);
	if (IS_ERR(file))
		return HZN_RESULT_OUT_OF_MEMORY;
	h = hzn_handle_add(file);
	if (h == HZN_INVALID_HANDLE)
		return HZN_RESULT_OUT_OF_HANDLES;
	*handle = h;
	return HZN_RESULT_SUCCESS;
}

/*
 * The command buffer, as much of it as the kernel needs (from yuzu): word 0
 * has the command type in its low 16 bits, word 1 the size of the raw data
 * (in words) in its low 10 bits.
 */
#define COMMAND_BUFFER_WORDS	(0x100 / sizeof(u32))
#define COMMAND_TYPE(cmd)	((cmd)[0] & 0xffff)

enum command_type {
	COMMAND_TYPE_CLOSE = 2,
	COMMAND_TYPE_REQUEST = 4,
	COMMAND_TYPE_REQUEST_WITH_CONTEXT = 6,
	COMMAND_TYPE_TIPC_CLOSE = 15,
	COMMAND_TYPE_TIPC_COMMAND_REGION = 16,
};

#define DATA_PAYLOAD_MAGIC_SFCO	0x4f434653	/* "SFCO" */

#define IPC_ERR_REMOTE_PROCESS_DEAD \
	(((union hzn_result_code){ .bf = { HZN_ERROR_MODULE_HIPC, 301 } }).raw)

/* A response that carries only @result (yuzu's ResponseBuilder). */
static void hzn_build_result_response(u32 *cmd, bool is_domain, u32 result)
{
	u32 type = COMMAND_TYPE(cmd);
	bool is_tipc = type >= COMMAND_TYPE_TIPC_COMMAND_REGION;
	bool is_request = type == COMMAND_TYPE_REQUEST ||
			  type == COMMAND_TYPE_REQUEST_WITH_CONTEXT;
	unsigned int i = 2, raw_size;

	memset(cmd, 0, COMMAND_BUFFER_WORDS * sizeof(u32));

	/* result, domain header, payload header, padding, unused */
	raw_size = is_tipc ? 1 : 2;
	if (is_domain)
		raw_size += 4;
	if (!is_tipc)
		raw_size += 2 + 4 + 2;
	cmd[1] = raw_size;

	if (!is_tipc) {
		i = 4;			/* the payload is 16-byte aligned */
		if (is_domain && is_request)
			i += 4;		/* after the domain header */
		cmd[i] = DATA_PAYLOAD_MAGIC_SFCO;
		i += 2;
	}
	cmd[i] = result;
}

/* Sends the request in the pinned page @page, and unpins it. */
static long hzn_send_request(u32 handle, struct page *page, unsigned long addr, u64 size)
{
	struct hzn_session *session;
	struct task_struct *task;
	struct hzn_service *svc;
	struct hzn_request *req;
	struct file *file;
	u32 *cmd;
	long ret;

	file = hzn_handle_get(handle);
	if (!file) {
		unpin_user_page(page);
		return HZN_RESULT_INVALID_HANDLE;
	}
	if (hzn_is_native_client(file)) {
		unpin_user_page(page);
		ret = hzn_native_send(file, addr, size);
		fput(file);
		return ret;
	}
	if (!hzn_is_session(file)) {
		unpin_user_page(page);
		fput(file);
		return HZN_RESULT_INVALID_HANDLE;
	}
	session = file->private_data;
	cmd = page_address(page);

	if (COMMAND_TYPE(cmd) == COMMAND_TYPE_CLOSE ||
	    COMMAND_TYPE(cmd) == COMMAND_TYPE_TIPC_CLOSE) {
		/* The service learns about it when the handle is closed. */
		hzn_build_result_response(cmd, session->is_domain,
					  HZN_RESULT_SUCCESS);
		ret = IPC_ERR_REMOTE_PROCESS_DEAD;
		goto out_unpin;
	}
	if (!session->service) {
		pr_warn_ratelimited("horizon: session without a service, stubbing the response\n");
		hzn_build_result_response(cmd, session->is_domain,
					  HZN_RESULT_SUCCESS);
		ret = HZN_RESULT_SUCCESS;
		goto out_unpin;
	}

	task = get_pid_task(session->service, PIDTYPE_PID);
	if (!task) {
		ret = HZN_RESULT_SESSION_CLOSED;
		goto out_unpin;
	}
	svc = hzn_service_of(task);
	req = svc ? hzn_request_alloc() : NULL;
	if (!req) {
		put_task_struct(task);
		ret = HZN_RESULT_OUT_OF_MEMORY;
		goto out_unpin;
	}
	req->requester = get_task_struct(current);
	req->cmd = page;
	req->session_file = file;

	kref_get(&req->ref);	/* for the service's queue */
	if (!hzn_service_queue(task, svc, req)) {
		put_task_struct(task);
		hzn_request_put(req);
		hzn_request_put(req);
		return HZN_RESULT_SESSION_CLOSED;
	}
	put_task_struct(task);

	ret = hzn_request_wait(req);
	hzn_request_put(req);
	return ret;

out_unpin:
	unpin_user_pages_dirty_lock(&page, 1, true);
	fput(file);
	return ret;
}

/* svcSendSyncRequest: the command is at the start of the thread's TLS page. */
long hzn_send_sync_request(u32 handle)
{
	struct page *page;

	if (pin_user_pages_fast(current->thread.uw.tp_value, 1, FOLL_WRITE,
				&page) != 1)
		return HZN_RESULT_INVALID_ADDRESS;
	return hzn_send_request(handle, page, current->thread.uw.tp_value, 0x100);
}

/*
 * svcSendSyncRequestWithUserBuffer: the command is at the start of a buffer
 * of the program. The service sees the first page of it.
 */
long hzn_send_sync_request_with_user_buffer(unsigned long buf, u64 size,
                                              u32 handle)
{
	struct page **pages;
	unsigned long count;
	long ret = hzn_pin_ipc_user_buffer(buf, size, &pages, &count);

	if (ret)
		return ret;
	/* The existing requester owns and unpins the first page. */
	ret = hzn_send_request(handle, pages[0], buf, size);
	if (count > 1)
		unpin_user_pages(pages + 1, count - 1);
	kvfree(pages);
	return ret;
}

/*
 * svcSendAsyncRequestWithUserBuffer. Services answer synchronously here, so
 * the request is done before this returns, and the event that tells so is
 * signalled from the start.
 */
long hzn_send_async_request_with_user_buffer(unsigned long buf, u64 size,
					      u32 handle, u32 *event)
{
	struct file *file;
	long ret;

	ret = hzn_send_sync_request_with_user_buffer(buf, size, handle);
	if (ret != HZN_RESULT_SUCCESS)
		return ret;
	file = eventfd_file_create(1, 0);
	if (IS_ERR(file))
		return HZN_RESULT_OUT_OF_RESOURCE;
	*event = hzn_handle_add(file);
	return *event == HZN_INVALID_HANDLE ? HZN_RESULT_OUT_OF_HANDLES :
					      HZN_RESULT_SUCCESS;
}

/* horizon_servctl */

static long hzn_sctl_register(const char __user *uname)
{
	char name[HZN_PORT_NAME_MAX_LENGTH + 1];
	struct hzn_named_service *ns, *new;
	struct task_struct *old = NULL;

	if (strncpy_from_user(name, uname, sizeof(name)) < 0)
		return SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
	name[sizeof(name) - 1] = 0;
	if (!hzn_service_of(current))
		return SERVCTL_RET(HZN_RESULT_OUT_OF_MEMORY);

	new = kzalloc_obj(*new);
	if (!new)
		return SERVCTL_RET(HZN_RESULT_OUT_OF_MEMORY);
	strscpy(new->name, name);
	new->uid = current_uid();
	new->service = get_task_struct(current);

	spin_lock(&hzn_named_services_lock);
	list_for_each_entry(ns, &hzn_named_services, entry) {
		if (!strcmp(ns->name, name) && uid_eq(ns->uid, new->uid)) {
			old = ns->service;
			ns->service = new->service;
			break;
		}
	}
	if (!old)
		list_add_tail(&new->entry, &hzn_named_services);
	spin_unlock(&hzn_named_services_lock);

	if (old) {
		put_task_struct(old);
		kfree(new);
	}
	return 0;
}

/*
 * Copies the command to the service's command page, which stays mapped from
 * one request to the next (mapping a page for each request would cost a TLB
 * shoot-down for each when it is unmapped).
 */
static int hzn_service_put_command(struct hzn_service *svc, struct page *cmd)
{
	unsigned long addr = svc->cmd_addr;
	int tries;

	for (tries = 0; tries < 2; tries++) {
		if (addr && !copy_to_user((void __user *)addr, page_address(cmd),
					  PAGE_SIZE)) {
			svc->cmd_addr = addr;
			return 0;
		}
		/* None yet, or the service unmapped it. */
		addr = vm_mmap(NULL, 0, PAGE_SIZE, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, 0);
		if (IS_ERR_VALUE(addr))
			break;
	}
	return -EFAULT;
}

static long hzn_sctl_get_cmd(struct hzn_service *svc,
			     unsigned long __user *session_id)
{
	struct hzn_session *session;
	struct hzn_request *req;

	if (svc->cur)
		return SERVCTL_RET(HZN_RESULT_INVALID_STATE);

again:
	spin_lock(&svc->lock);
	while (list_empty(&svc->requests)) {
		set_current_state(TASK_INTERRUPTIBLE | TASK_FREEZABLE);
		spin_unlock(&svc->lock);
		if (signal_pending(current)) {
			__set_current_state(TASK_RUNNING);
			return SERVCTL_RET(HZN_RESULT_CANCELLED);
		}
		schedule();
		spin_lock(&svc->lock);
	}
	__set_current_state(TASK_RUNNING);
	req = list_first_entry(&svc->requests, struct hzn_request, node);
	list_del_init(&req->node);
	spin_unlock(&svc->lock);

	/* A close request: report the session and return 0. */
	if (!req->cmd) {
		unsigned long id = req->close_session_id;

		hzn_request_put(req);
		if (put_user(id, session_id))
			return SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
		return 0;
	}
	if (atomic_read(&req->state) != HZN_REQ_PENDING) {
		hzn_request_put(req);	/* the requester was killed */
		goto again;
	}

	session = req->session_file->private_data;
	if (put_user(session->id, session_id) ||
	    hzn_service_put_command(svc, req->cmd))
		goto fail;
	svc->cur = req;
	return svc->cmd_addr;

fail:
	hzn_request_finish(req, HZN_REQ_FAILED);
	hzn_request_put(req);
	return SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
}

static long hzn_sctl_put_cmd(struct hzn_service *svc, unsigned long id,
			     bool is_domain)
{
	struct hzn_request *req = svc->cur;
	struct hzn_session *session;

	if (!req)
		return SERVCTL_RET(HZN_RESULT_INVALID_STATE);
	if (copy_from_user(page_address(req->cmd),
			   (const void __user *)svc->cmd_addr, PAGE_SIZE))
		return SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
	svc->cur = NULL;

	session = req->session_file->private_data;
	session->id = id;
	session->is_domain = is_domain;

	hzn_request_finish(req, HZN_REQ_HANDLED);
	hzn_request_put(req);
	return 0;
}

static long hzn_sctl_install(struct hzn_request *req, struct file *file)
{
	long ret = hzn_request_post_op(req, HZN_OP_INSTALL_FILE, file, 0, 0);

	if (ret == -ESRCH) {		/* not consumed */
		if (hzn_is_session(file))
			((struct hzn_session *)file->private_data)->id = 0;
		fput(file);
		return SERVCTL_RET(HZN_RESULT_SESSION_CLOSED);
	}
	if (ret < 0)
		return SERVCTL_RET(HZN_RESULT_OUT_OF_HANDLES);
	return ret;
}

static long hzn_sctl_create_session(struct hzn_service *svc, long pid,
				    unsigned long id)
{
	struct pid *service;
	struct file *file;

	if (!svc->cur)
		return SERVCTL_RET(HZN_RESULT_INVALID_STATE);

	switch (pid) {
	case -1:
		service = NULL;
		break;
	case 0:
		service = get_task_pid(current, PIDTYPE_PID);
		break;
	default:
		service = find_get_pid(pid);
		if (!service)
			return SERVCTL_RET(HZN_RESULT_NOT_FOUND);
	}
	file = hzn_session_file(service, id);
	if (IS_ERR(file))
		return SERVCTL_RET(HZN_RESULT_OUT_OF_MEMORY);
	return hzn_sctl_install(svc->cur, file);
}

static long hzn_sctl_copy_handle(struct hzn_service *svc, unsigned int fd)
{
	struct file *file;

	if (!svc->cur)
		return SERVCTL_RET(HZN_RESULT_INVALID_STATE);
	file = fget_raw(fd);
	if (!file)
		return SERVCTL_RET(HZN_RESULT_INVALID_HANDLE);
	return hzn_sctl_install(svc->cur, file);
}

/* Copies between the caller and @mm through a bounce page. */
static long hzn_copy_remote(struct mm_struct *mm, unsigned long remote,
			    unsigned long local, size_t len, bool write)
{
	void *buf;
	size_t chunk;
	long ret = 0;

	if (!len)
		return 0;
	buf = (void *)__get_free_page(GFP_KERNEL);
	if (!buf)
		return SERVCTL_RET(HZN_RESULT_OUT_OF_MEMORY);

	while (len) {
		chunk = min_t(size_t, len, PAGE_SIZE);
		if (write) {
			if (copy_from_user(buf, (const void __user *)local, chunk) ||
			    access_remote_vm(mm, remote, buf, chunk, FOLL_WRITE) != chunk)
				break;
		} else {
			if (access_remote_vm(mm, remote, buf, chunk, 0) != chunk ||
			    copy_to_user((void __user *)local, buf, chunk))
				break;
		}
		remote += chunk;
		local += chunk;
		len -= chunk;
	}
	if (len)
		ret = SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
	free_page((unsigned long)buf);
	return ret;
}

static long hzn_sctl_buffer(struct hzn_service *svc, unsigned long there,
			    unsigned long here, size_t len, bool write)
{
	struct mm_struct *mm;
	long ret;

	if (!svc->cur)
		return SERVCTL_RET(HZN_RESULT_INVALID_STATE);
	mm = get_task_mm(svc->cur->requester);
	if (!mm)
		return SERVCTL_RET(HZN_RESULT_SESSION_CLOSED);
	ret = hzn_copy_remote(mm, there, here, len, write);
	mmput(mm);
	return ret;
}

/*
 * The mm of the process with @pid (any of its threads), if the caller may
 * ptrace it.
 */
static struct mm_struct *hzn_pid_mm(pid_t pid)
{
	struct task_struct *task, *t;
	struct mm_struct *mm;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (!task)
		return NULL;
	/* The main thread may be gone while others still run. */
	t = find_lock_task_mm(task);
	put_task_struct(task);
	if (!t)
		return NULL;
	get_task_struct(t);
	task_unlock(t);
	mm = mm_access(t, PTRACE_MODE_ATTACH_REALCREDS);
	put_task_struct(t);
	return IS_ERR(mm) ? NULL : mm;
}

static long hzn_sctl_buffer_pid(unsigned long there, unsigned long here,
				size_t len, pid_t pid, bool write)
{
	struct mm_struct *mm = hzn_pid_mm(pid);
	long ret;

	if (!mm)
		return SERVCTL_RET(HZN_RESULT_INVALID_ID);
	ret = hzn_copy_remote(mm, there, here, len, write);
	mmput(mm);
	return ret;
}

/*
 * Maps [there, there + len) of the requester at @here in the caller, shared:
 * directly when the requester's memory already is a shmem mapping (heap,
 * data, transfer and physical memory are), otherwise after the requester
 * has moved that range into shmem.
 */
static long hzn_sctl_map_memory(struct hzn_service *svc, unsigned long there,
				unsigned long here, size_t len)
{
	struct hzn_request *req = svc->cur;
	unsigned long start, size, dst, m, end, here_end;
	struct mm_struct *mm;
	struct file *file;
	pgoff_t pgoff = 0;
	long ret;

	if (!req)
		return SERVCTL_RET(HZN_RESULT_INVALID_STATE);
	if (!len)
		return SERVCTL_RET(HZN_RESULT_INVALID_SIZE);
	if ((there ^ here) & ~PAGE_MASK ||
	    check_add_overflow(there, len, &end) || end > TASK_SIZE_MAX ||
	    check_add_overflow(here, len, &here_end) || here_end > TASK_SIZE_MAX)
		return SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
	start = there & PAGE_MASK;
	size = PAGE_ALIGN(end) - start;
	dst = here & PAGE_MASK;

	mm = get_task_mm(req->requester);
	if (!mm)
		return SERVCTL_RET(HZN_RESULT_SESSION_CLOSED);
	file = hzn_find_shared(mm, start, size, &pgoff);
	mmput(mm);

	if (!file) {
		ret = hzn_request_post_op(req, HZN_OP_MAKE_SHARED, NULL, start, size);
		if (ret)
			return SERVCTL_RET(ret == -ESRCH ? HZN_RESULT_SESSION_CLOSED :
							   HZN_RESULT_INVALID_ADDRESS);
		file = req->op_file;
		req->op_file = NULL;
		pgoff = 0;
	}

	m = vm_mmap(file, dst, size, PROT_READ | PROT_WRITE,
		    MAP_SHARED | MAP_FIXED, (unsigned long)pgoff << PAGE_SHIFT);
	fput(file);
	return m == dst ? 0 : SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS);
}

static long hzn_sctl_memwatch(pid_t pid, unsigned long addr, size_t len,
			      loff_t __user *vec, size_t vec_len, bool clear)
{
	struct mm_struct *mm = hzn_pid_mm(pid);
	long ret;

	if (!mm)
		return SERVCTL_RET(HZN_RESULT_INVALID_ID);
	ret = hzn_memwatch(mm, addr, len, clear, vec, vec_len);
	mmput(mm);
	return ret < 0 ? SERVCTL_RET(HZN_RESULT_INVALID_ADDRESS) : ret;
}

/*
 * horizon_servctl is called by Linux services, so it returns negative values
 * on error, but they are Horizon result codes rather than errno values (they
 * often get relayed to the Horizon process).
 */
SYSCALL_DEFINE6(horizon_servctl, unsigned int, cmd,
		unsigned long, arg1, unsigned long, arg2,
		unsigned long, arg3, unsigned long, arg4,
		unsigned long, arg5)
{
	struct hzn_service *svc;

	switch (cmd) {
	case HZN_SCTL_REGISTER_NAMED_SERVICE:
		return hzn_sctl_register((const char __user *)arg1);
	case HZN_SCTL_READ_BUFFER_FROM:
		return hzn_sctl_buffer_pid(arg1, arg2, arg3, arg4, false);
	case HZN_SCTL_WRITE_BUFFER_TO:
		return hzn_sctl_buffer_pid(arg1, arg2, arg3, arg4, true);
	case HZN_SCTL_MEMWATCH_GET_CLEAR:
		return hzn_sctl_memwatch(arg1, arg2, arg3, (loff_t __user *)arg4,
					 arg5, true);
	case HZN_SCTL_MEMWATCH_GET:
		return hzn_sctl_memwatch(arg1, arg2, arg3, (loff_t __user *)arg4,
					 arg5, false);
	}

	svc = hzn_service_of(current);
	if (!svc)
		return SERVCTL_RET(HZN_RESULT_OUT_OF_MEMORY);

	switch (cmd) {
	case HZN_SCTL_GET_CMD:
		return hzn_sctl_get_cmd(svc, (unsigned long __user *)arg1);
	case HZN_SCTL_PUT_CMD:
		return hzn_sctl_put_cmd(svc, arg1, arg2);
	case HZN_SCTL_CREATE_SESSION_HANDLE:
		return hzn_sctl_create_session(svc, arg1, arg2);
	case HZN_SCTL_CREATE_COPY_HANDLE:
		return hzn_sctl_copy_handle(svc, arg1);
	case HZN_SCTL_GET_PROCESS_ID:
		/* The process, not the thread: it outlives the request. */
		if (!svc->cur)
			return SERVCTL_RET(HZN_RESULT_INVALID_STATE);
		return task_tgid_vnr(svc->cur->requester);
	case HZN_SCTL_GET_TITLE_ID:
		if (!svc->cur || !svc->cur->requester->hzn_thread)
			return SERVCTL_RET(HZN_RESULT_INVALID_STATE);
		return svc->cur->requester->hzn_thread->proc->title_id;
	case HZN_SCTL_WRITE_BUFFER:
		return hzn_sctl_buffer(svc, arg1, arg2, arg3, true);
	case HZN_SCTL_READ_BUFFER:
		return hzn_sctl_buffer(svc, arg1, arg2, arg3, false);
	case HZN_SCTL_MAP_MEMORY:
		return hzn_sctl_map_memory(svc, arg1, arg2, arg3);
	}
	return SERVCTL_RET(HZN_RESULT_INVALID_ARGUMENT);
}
