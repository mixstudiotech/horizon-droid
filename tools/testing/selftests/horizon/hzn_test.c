// SPDX-License-Identifier: GPL-2.0
/*
 * Tests of the Horizon (Nintendo Switch) personality, CONFIG_HORIZON.
 *
 * hzn_test runs hzn_guest, a freestanding Horizon program, the way the
 * Horizon Linux loader (mizu) does: as a horizon_hdr image executed with
 * horizon_execveat(2). Meanwhile it serves the guest's IPC on the "hzntest"
 * named port, as a mizu service does, and checks how each run ends: with the
 * right exit status, and without hanging.
 */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/horizon.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"
#include "hzn_test.h"

#ifndef __NR_horizon_execve
#define __NR_horizon_execve	500
#define __NR_horizon_execveat	501
#define __NR_horizon_servctl	502
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open		434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd	438
#endif

#define IMAGE_BASE	0x8000000ul
#define PAGE		0x1000ul

/* Horizon result codes horizon_servctl() fails with */
#define R_CANCELLED	(1u | (118u << 9))
#define R_SESSION_CLOSED	(1u | (123u << 9))

/* Negative Horizon result codes on failure. */
static long servctl(long cmd, long a1, long a2, long a3, long a4, long a5)
{
	long r;

	errno = 0;
	r = syscall(__NR_horizon_servctl, cmd, a1, a2, a3, a4, a5);
	if (r == -1 && errno)
		r = -errno;	/* libc took a small result code for an errno */
	return r;
}

/* ---- the image ------------------------------------------------------------ */

static void *guest_elf;
static size_t guest_elf_size;

static int load_guest(const char *path)
{
	struct stat st;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0 || fstat(fd, &st))
		return -1;
	guest_elf_size = st.st_size;
	guest_elf = malloc(guest_elf_size);
	if (!guest_elf || read(fd, guest_elf, guest_elf_size) != (ssize_t)guest_elf_size)
		return -1;
	close(fd);
	return 0;
}

enum image_kind { IMAGE_OK, IMAGE_BAD_SIZE };

/* A memfd with the guest as one code set, laid out as mizu's loader does. */
static int build_image(int address_space_type, enum image_kind kind)
{
	const Elf64_Ehdr *eh = guest_elf;
	const Elf64_Phdr *ph;
	struct horizon_codeset_hdr *cs;
	struct horizon_hdr *hdr;
	size_t hdr_size, file_pos, size;
	uint64_t end = 0;
	uint8_t *img;
	int i, seg, fd;

	if (guest_elf_size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG) ||
	    eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_machine != EM_AARCH64)
		return -1;
	ph = (const Elf64_Phdr *)((const uint8_t *)guest_elf + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++)
		if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz - IMAGE_BASE > end)
			end = ph[i].p_vaddr + ph[i].p_memsz - IMAGE_BASE;
	end = (end + PAGE - 1) & ~(PAGE - 1);

	hdr_size = sizeof(*hdr) + sizeof(*cs);
	file_pos = (hdr_size + PAGE - 1) & ~(PAGE - 1);
	size = file_pos + end;
	img = calloc(1, size);
	if (!img)
		return -1;

	hdr = (struct horizon_hdr *)img;
	hdr->magic = HORIZON_MAGIC;
	hdr->title_id = HZN_TEST_TITLE_ID;
	hdr->ideal_core = 0;
	hdr->is_64bit = 1;
	hdr->address_space_type = address_space_type;
	hdr->system_resource_size = HZN_TEST_SYSTEM_RESOURCE_SIZE;
	hdr->main_thread_priority = HZN_TEST_PRIORITY;
	hdr->num_codesets = 1;
	cs = &hdr->codesets[0];
	cs->memory_size = end;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_vaddr < IMAGE_BASE || ph[i].p_vaddr & (PAGE - 1) ||
		    ph[i].p_offset + ph[i].p_filesz > guest_elf_size) {
			free(img);
			return -1;
		}
		seg = ph[i].p_flags & PF_X ? 0 : ph[i].p_flags & PF_W ? 2 : 1;
		cs->segments[seg].offset = ph[i].p_offset;
		cs->segments[seg].addr = ph[i].p_vaddr - IMAGE_BASE;
		cs->segments[seg].size = (ph[i].p_memsz + PAGE - 1) & ~(PAGE - 1);
		memcpy(img + file_pos + ph[i].p_vaddr - IMAGE_BASE,
		       (const uint8_t *)guest_elf + ph[i].p_offset, ph[i].p_filesz);
	}
	if (kind == IMAGE_BAD_SIZE)
		cs->memory_size = 0x1234;

	fd = memfd_create("hzn-guest", MFD_CLOEXEC);
	if (fd >= 0 && write(fd, img, size) != (ssize_t)size) {
		close(fd);
		fd = -1;
	}
	free(img);
	return fd;
}

/* ---- the freezer ------------------------------------------------------------ */

#define FREEZER		"/tmp/hzn-freezer"
#define FREEZER_GROUP	FREEZER "/hzn"
static bool have_freezer, freezer_v2;

static int write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC), r;

	if (fd < 0)
		return -1;
	r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
	close(fd);
	return r;
}

static int read_file(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = 0;
	return 0;
}

/*
 * The cgroup v1 freezer freezes like suspend does (freeze_task()). With
 * HZN_FREEZER=v2 the cgroup v2 freezer is tried instead, which only freezes
 * a task once it gets to the signal delivery path.
 */
static void setup_freezer(void)
{
	const char *v = getenv("HZN_FREEZER");

	freezer_v2 = v && !strcmp(v, "v2");
	mkdir(FREEZER, 0755);
	if (mount("hzn-freezer", FREEZER, freezer_v2 ? "cgroup2" : "cgroup", 0,
		  freezer_v2 ? NULL : "freezer") && errno != EBUSY)
		return;
	if (mkdir(FREEZER_GROUP, 0755) && errno != EEXIST)
		return;
	have_freezer = true;
}

static void cleanup_freezer(void)
{
	if (!have_freezer)
		return;
	rmdir(FREEZER_GROUP);
	umount(FREEZER);
	rmdir(FREEZER);
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, ms % 1000 * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

/* Freezes @pid, which has to get frozen within 5 s, and thaws it again. */
static bool freeze(pid_t pid)
{
	char buf[32];
	bool frozen = false;
	int i;

	snprintf(buf, sizeof(buf), "%d", pid);
	if (write_file(FREEZER_GROUP "/cgroup.procs", buf) ||
	    write_file(freezer_v2 ? FREEZER_GROUP "/cgroup.freeze" :
				    FREEZER_GROUP "/freezer.state",
		       freezer_v2 ? "1" : "FROZEN"))
		return false;
	for (i = 0; i < 100 && !frozen; i++) {
		sleep_ms(50);
		if (freezer_v2)
			frozen = !read_file(FREEZER_GROUP "/cgroup.events", buf, sizeof(buf)) &&
				 strstr(buf, "frozen 1");
		else
			frozen = !read_file(FREEZER_GROUP "/freezer.state", buf, sizeof(buf)) &&
				 !strncmp(buf, "FROZEN", 6);
	}
	if (!frozen)
		ksft_print_msg("not frozen: %s", buf);
	if (freezer_v2)
		write_file(FREEZER_GROUP "/cgroup.freeze", "0");
	else
		write_file(FREEZER_GROUP "/freezer.state", "THAWED");
	return frozen;
}

/* ---- the service -------------------------------------------------------------- */

struct run {
	const char *name;
	const char *mode;		/* argv[1] of the guest */
	int address_space_type;
	enum { HORIZON_EXEC, PLAIN_EXEC, ELF_EXEC } exec;
	enum image_kind image;
	int want;			/* exit status, or -signal */
	bool freeze;			/* freeze it when it is ready */
	bool needs_true;		/* needs an ELF that exits with 0 */
	int timeout;			/* seconds */
	unsigned long cpus;		/* CPU affinity to exec it with, or 0 */
	bool traced;			/* run it under ptrace, as a debugger does */
	bool trace_clones;
	int want_traps;
};

static const struct run *cur_run;
/* SIGTRAP stops of a traced guest, its exec included. */
static int traps, clones;
static bool trace_ok;
static pid_t clone_parent, clone_child;
static pid_t guest, native_peer;
static int event_fd = -1, shmem_fd = -1;
static unsigned long closed_ids[64];
static int nr_closed;
static unsigned long next_session_id = 1000;
static bool freeze_ok;
static long stop_install_result, stop_kill_result;
static unsigned long stop_kill_session;
static const char *true_path;

static uint64_t get_u64(const uint32_t *b, int i)
{
	return b[4 + 2 * i] | (uint64_t)b[5 + 2 * i] << 32;
}

static void put_u64(uint32_t *b, int i, uint64_t v)
{
	b[4 + 2 * i] = v;
	b[5 + 2 * i] = v >> 32;
}

static uint8_t pattern(uint64_t i, uint8_t seed)
{
	return seed + i * 7;
}

static uint64_t sum_of(const uint8_t *p, uint64_t len)
{
	uint64_t s = 0;

	while (len--)
		s += *p++;
	return s;
}

/* MAP_MEMORY [there, there + len) of the guest, check and rewrite it. */
static uint64_t do_map(uint64_t there, uint64_t len, uint8_t seed)
{
	size_t area = len + 2 * PAGE;
	uint8_t *res, *here;
	uint64_t i, ok = 1;

	res = mmap(NULL, area, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (res == MAP_FAILED)
		return 0;
	here = res + PAGE + (there & (PAGE - 1));
	if (servctl(HZN_SCTL_MAP_MEMORY, there, (long)here, len, 0, 0)) {
		munmap(res, area);
		return 0;
	}
	for (i = 0; i < len; i++)
		if (here[i] != pattern(i, seed))
			ok = 0;
	for (i = 0; i < len; i++)
		here[i] = pattern(i, seed + 1);
	munmap(res, area);
	return ok;
}

/* Map the guest's transfer memory @handle, check and rewrite it. */
static uint64_t do_tmem(uint32_t handle, uint64_t len, uint8_t seed)
{
	int pidfd = syscall(__NR_pidfd_open, guest, 0), fd = -1;
	uint64_t i, ok = 1;
	uint8_t *p;

	if (pidfd >= 0) {
		fd = syscall(__NR_pidfd_getfd, pidfd, handle - 1, 0);
		close(pidfd);
	}
	if (fd < 0)
		return 0;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return 0;
	for (i = 0; i < len; i++)
		if (p[i] != pattern(i, seed))
			ok = 0;
	for (i = 0; i < len; i++)
		p[i] = pattern(i, seed + 1);
	munmap(p, len);
	return ok;
}

/* A GNU C Linux launcher relays a native client fd into another Horizon image. */
static bool start_native_peer(uint32_t handle)
{
	int pidfd, clientfd, imagefd;
	char handle_arg[32];

	pidfd = syscall(__NR_pidfd_open, guest, 0);
	if (pidfd < 0)
		return false;
	clientfd = syscall(__NR_pidfd_getfd, pidfd, handle - 1, 0);
	close(pidfd);
	if (clientfd < 0)
		return false;
	imagefd = build_image(cur_run->address_space_type, IMAGE_OK);
	if (imagefd < 0) {
		close(clientfd);
		return false;
	}
	fcntl(clientfd, F_SETFD, 0);
	snprintf(handle_arg, sizeof(handle_arg), "%u", clientfd + 1);
	fflush(stdout);
	native_peer = fork();
	if (!native_peer) {
		char *args[] = { "hzn-guest", "p3-cross-client", handle_arg, NULL };
		char *env[] = { NULL };
		syscall(__NR_horizon_execveat, imagefd, "", args, env, AT_EMPTY_PATH);
		_exit(43);
	}
	close(imagefd);
	close(clientfd);
	return native_peer > 0;
}

/*
 * The requester is stopped while it waits for our answer, as a debugger or a
 * group stop leaves it, and killed while we install a handle for it. It
 * takes the SIGKILL in the middle of its wait, which has to unwind and
 * abandon the request before the thread exits: the install fails
 * (SessionClosed) instead of waiting for it forever. Returns the result of
 * the install.
 */
static long stop_install(void)
{
	char path[64], buf[512], *s;
	pid_t killer;
	int i, fd;
	long r;

	if (kill(guest, SIGSTOP))
		return 1;
	snprintf(path, sizeof(path), "/proc/%d/stat", guest);
	for (i = 0; i < 500; i++) {
		if (!read_file(path, buf, sizeof(buf)) && (s = strrchr(buf, ')')) && s[1] && s[2] == 'T')
			break;
		sleep_ms(10);
	}
	if (i == 500)
		return 2;
	fflush(stdout);
	killer = fork();
	if (killer < 0)
		return 3;	/* nothing would kill it: do not install */
	if (!killer) {
		sleep_ms(300);
		kill(guest, SIGKILL);
		_exit(0);
	}
	fd = eventfd(0, EFD_CLOEXEC);
	r = fd < 0 ? 4 : servctl(HZN_SCTL_CREATE_COPY_HANDLE, fd, 0, 0, 0, 0);
	if (fd >= 0)
		close(fd);
	while (waitpid(killer, NULL, 0) < 0 && errno == EINTR)
		;
	return r;
}

/* Stops the guest; 0 once all of its threads are stopped (a group stop). */
static long stop_guest(void)
{
	siginfo_t si;
	int i;

	if (kill(guest, SIGSTOP))
		return 1;
	for (i = 0; i < 500; i++) {
		si.si_pid = 0;
		if (!waitid(P_PID, guest, &si, WSTOPPED | WNOHANG) && si.si_pid == guest)
			return 0;
		sleep_ms(10);
	}
	return 2;
}

static bool was_closed(unsigned long id)
{
	int i;

	for (i = 0; i < nr_closed; i++)
		if (closed_ids[i] == id)
			return true;
	return false;
}

/*
 * After "stop-kill": every thread of the guest took the SIGKILL in the
 * middle of an SVC wait. Once they are gone, wake what they waited on: a
 * wait left behind would touch a dead thread (KASAN reports that). And the
 * guest's session has to close: a request left behind would keep it open.
 */
static bool after_stop_kill(void)
{
	unsigned long id;
	long ptr;
	int i;

	if (stop_kill_result) {
		ksft_print_msg("%s: stopping the guest gave %ld\n", cur_run->name,
			       stop_kill_result);
		return false;
	}
	sleep_ms(100);		/* an RCU grace period: the threads are freed */
	if (event_fd >= 0)
		eventfd_write(event_fd, 1);
	sleep_ms(1500);		/* past the timeout of the timed wait */
	for (i = 0; i < 100 && !was_closed(stop_kill_session); i++) {
		id = 0;
		ptr = servctl(HZN_SCTL_GET_CMD, (long)&id, 0, 0, 0, 0);
		if (!ptr && nr_closed < 64)
			closed_ids[nr_closed++] = id;
		else if (ptr > 0)
			servctl(HZN_SCTL_PUT_CMD, id, 0, 0, 0, 0);
	}
	if (was_closed(stop_kill_session))
		return true;
	ksft_print_msg("%s: its session %lu did not close\n", cur_run->name, stop_kill_session);
	return false;
}

/* Handles one request; returns false if it is not to be answered. */
static bool handle_request(uint32_t *b, unsigned long session_id)
{
	uint64_t a0 = get_u64(b, 0), a1 = get_u64(b, 1), a2 = get_u64(b, 2), i;
	static uint8_t buf[0x10000];
	uint32_t cmd = b[2];
	loff_t vec[16];
	long r, pid;

	b[2] = 0;
	switch (cmd) {
	case CMD_ECHO:
		put_u64(b, 0, a0 + 1);
		break;
	case CMD_IDS:
		put_u64(b, 0, servctl(HZN_SCTL_GET_PROCESS_ID, 0, 0, 0, 0, 0));
		put_u64(b, 1, servctl(HZN_SCTL_GET_TITLE_ID, 0, 0, 0, 0, 0));
		put_u64(b, 2, session_id);
		break;
	case CMD_READ:
		if (a1 > sizeof(buf) || servctl(HZN_SCTL_READ_BUFFER, a0, (long)buf, a1, 0, 0))
			b[2] = 1;
		else
			put_u64(b, 0, sum_of(buf, a1));
		break;
	case CMD_READ_FROM:
		pid = servctl(HZN_SCTL_GET_PROCESS_ID, 0, 0, 0, 0, 0);
		if (a1 > sizeof(buf) ||
		    servctl(HZN_SCTL_READ_BUFFER_FROM, a0, (long)buf, a1, pid, 0))
			b[2] = 1;
		else
			put_u64(b, 0, sum_of(buf, a1));
		break;
	case CMD_WRITE:
		if (a1 > sizeof(buf)) {
			b[2] = 1;
			break;
		}
		for (i = 0; i < a1; i++)
			buf[i] = pattern(i, a2);
		if (servctl(HZN_SCTL_WRITE_BUFFER, a0, (long)buf, a1, 0, 0))
			b[2] = 1;
		else
			put_u64(b, 0, 0);
		break;
	case CMD_EVENT:
		if (event_fd >= 0)
			close(event_fd);
		event_fd = eventfd(0, EFD_CLOEXEC);
		r = event_fd < 0 ? -1 : servctl(HZN_SCTL_CREATE_COPY_HANDLE, event_fd, 0, 0, 0, 0);
		if (r <= 0)
			b[2] = 1;
		else
			put_u64(b, 0, r);
		break;
	case CMD_SIGNAL:
		if (eventfd_write(event_fd, 1))
			b[2] = 1;
		break;
	case CMD_SHMEM:
		if (shmem_fd >= 0)
			close(shmem_fd);
		shmem_fd = memfd_create("hzn-shared", MFD_CLOEXEC);
		r = shmem_fd < 0 || ftruncate(shmem_fd, PAGE) || pwrite(shmem_fd, "\x5a", 1, 5) != 1 ? -1 :
		    servctl(HZN_SCTL_CREATE_COPY_HANDLE, shmem_fd, 0, 0, 0, 0);
		if (r <= 0)
			b[2] = 1;
		else
			put_u64(b, 0, r);
		break;
	case CMD_MAP_INVALID:
		r = 0;
		if (servctl(HZN_SCTL_MAP_MEMORY, 0, 0, ULONG_MAX, 0, 0) < 0)
			r |= 1;
		if (servctl(HZN_SCTL_MAP_MEMORY, a0, 0, ULONG_MAX, 0, 0) < 0)
			r |= 2;
		if (servctl(HZN_SCTL_MAP_MEMORY, a0, -PAGE, PAGE, 0, 0) < 0)
			r |= 4;
		if (servctl(HZN_SCTL_MAP_MEMORY, 0, 0, 1ul << 63, 0, 0) < 0)
			r |= 8;
		put_u64(b, 0, r);
		break;
	case CMD_MAP:
		put_u64(b, 0, do_map(a0, a1, a2));
		break;
	case CMD_MEMWATCH:
		pid = servctl(HZN_SCTL_GET_PROCESS_ID, 0, 0, 0, 0, 0);
		r = servctl(a2 ? HZN_SCTL_MEMWATCH_GET_CLEAR : HZN_SCTL_MEMWATCH_GET,
			    pid, a0, a1, (long)vec, 16);
		if (r < 0) {
			b[2] = 1;
		} else {
			put_u64(b, 0, r);
			put_u64(b, 1, r ? (uint64_t)vec[0] : ~0ull);
		}
		break;
	case CMD_SESSION:
	case CMD_STUB_SESSION:
		r = servctl(HZN_SCTL_CREATE_SESSION_HANDLE, cmd == CMD_SESSION ? 0 : -1,
			    cmd == CMD_SESSION ? a0 : 0, 0, 0, 0);
		if (r <= 0)
			b[2] = 1;
		else
			put_u64(b, 0, r);
		break;
	case CMD_CLOSED:
		put_u64(b, 0, was_closed(a0));
		break;
	case CMD_TMEM:
		put_u64(b, 0, do_tmem(a0, a1, a2));
		break;
	case CMD_P3_NATIVE_START:
		put_u64(b, 0, start_native_peer(a0));
		break;
	case CMD_HANG:
		return false;
	case CMD_STOP_INSTALL:
		stop_install_result = stop_install();
		return false;
	case CMD_STOP_KILL:
		/* serve() kills it, stopped with its threads waiting. */
		stop_kill_session = session_id;
		stop_kill_result = stop_guest();
		return false;
	case CMD_READY:
		/* Freeze it while its threads wait, this one for our answer. */
		if (cur_run->freeze)
			freeze_ok = freeze(guest);
		break;
	case CMD_BYE:
		break;
	default:
		b[2] = 2;
	}
	return true;
}

static void on_alarm(int sig)
{
}

/* Resume both the leader and new threads; clone events are not break traps. */
static void resume_traced(pid_t tid, int status)
{
	int sig = WSTOPSIG(status);
	unsigned int event = (unsigned int)status >> 16;
	unsigned long child = 0;

	if (sig == SIGTRAP && !event) {
		if (tid == guest && !traps && cur_run->trace_clones &&
		    ptrace(PTRACE_SETOPTIONS, tid, NULL,
			   (void *)(long)(PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL)))
			trace_ok = false;
		traps++;
	}
	if (event == PTRACE_EVENT_CLONE) {
		if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) || !child)
			trace_ok = false;
		clones++;
		clone_parent = tid;
		clone_child = child;
		/* Like GDB all-stop, wait for the child stop before resuming either. */
		return;
	}
	if (tid == clone_child) {
		if (ptrace(PTRACE_CONT, clone_parent, NULL, NULL))
			trace_ok = false;
		clone_parent = clone_child = 0;
	}
	if (ptrace(PTRACE_CONT, tid, NULL,
		   (void *)(long)(sig == SIGTRAP || sig == SIGSTOP ? 0 : sig)))
		trace_ok = false;
}

static bool poll_guest(int *status)
{
	pid_t tid;

	while ((tid = waitpid(cur_run->traced ? -1 : guest, status,
			     WNOHANG | (cur_run->traced ? __WALL : 0))) > 0) {
		if (WIFSTOPPED(*status))
			resume_traced(tid, *status);
		else if (tid == guest)
			return true;
	}
	return false;
}

/* Waits up to @ms for the guest to exit; its wait status, or -1. */
static int reap(int ms)
{
	int status, i;

	for (i = 0; i < ms / 10; i++) {
		if (poll_guest(&status))
			return status;
		sleep_ms(10);
	}
	return -1;
}

/*
 * Serves the guest until it exits; returns its wait status, or -1 if it had
 * to be killed after @timeout seconds. *put_ok tells whether answering a
 * request of a killed guest went right.
 */
static int serve(int timeout, bool *put_ok)
{
	time_t deadline = time(NULL) + timeout;
	unsigned long session_id;
	int status;
	uint32_t *b;
	long ptr;

	*put_ok = true;
	for (;;) {
		if (poll_guest(&status))
			return status;
		if (time(NULL) > deadline) {
			ksft_print_msg("%s: timed out\n", cur_run->name);
			kill(guest, SIGKILL);
			reap(10000);
			return -1;
		}
		session_id = 0;
		ptr = servctl(HZN_SCTL_GET_CMD, (long)&session_id, 0, 0, 0, 0);
		if (ptr < 0) {
			if (-ptr != R_CANCELLED)
				ksft_print_msg("GET_CMD failed: 0x%lx\n", -ptr);
			continue;
		}
		if (!ptr) {
			if (nr_closed < 64)
				closed_ids[nr_closed++] = session_id;
			continue;
		}
		b = (uint32_t *)ptr;
		if (!session_id)
			session_id = next_session_id++;
		if (!handle_request(b, session_id)) {
			/* Kill it while it waits for the answer, then answer. */
			kill(guest, SIGKILL);
			status = reap(10000);
			*put_ok = !servctl(HZN_SCTL_PUT_CMD, session_id, 0, 0, 0, 0);
			return status;
		}
		if (servctl(HZN_SCTL_PUT_CMD, session_id, 0, 0, 0, 0))
			ksft_print_msg("PUT_CMD failed\n");
	}
}

static void run_one(const struct run *r)
{
	char *argv[] = { "hzn-guest", (char *)r->mode,
			 (char *)(true_path ? true_path : "-"), NULL };
	char *envp[] = { NULL };
	bool ok, put_ok;
	int fd, status;

	if (r->needs_true && !true_path) {
		ksft_test_result_skip("%s: no true(1)\n", r->name);
		return;
	}
	if (r->freeze && !have_freezer) {
		ksft_test_result_skip("%s: no cgroup v1 freezer\n", r->name);
		return;
	}
	if (r->cpus && sysconf(_SC_NPROCESSORS_ONLN) < 64 &&
	    r->cpus >> sysconf(_SC_NPROCESSORS_ONLN)) {
		ksft_test_result_skip("%s: not enough CPUs\n", r->name);
		return;
	}
	fd = build_image(r->address_space_type, r->image);
	if (fd < 0) {
		ksft_test_result_fail("%s: cannot build the image\n", r->name);
		return;
	}
	native_peer = 0;
	cur_run = r;
	freeze_ok = false;
	nr_closed = 0;
	fflush(stdout);
	guest = fork();
	if (!guest) {
		if (r->cpus) {
			cpu_set_t cpus;
			int i;

			CPU_ZERO(&cpus);
			for (i = 0; i < 64; i++)
				if (r->cpus & 1ul << i)
					CPU_SET(i, &cpus);
			sched_setaffinity(0, sizeof(cpus), &cpus);
		}
		if (r->traced && ptrace(PTRACE_TRACEME, 0, NULL, NULL))
			_exit(44);
		switch (r->exec) {
		case PLAIN_EXEC:
			syscall(__NR_execveat, fd, "", argv, envp, AT_EMPTY_PATH);
			break;
		case ELF_EXEC:
			argv[0] = (char *)true_path;
			argv[1] = NULL;
			syscall(__NR_horizon_execve, true_path, argv, envp);
			break;
		default:
			syscall(__NR_horizon_execveat, fd, "", argv, envp, AT_EMPTY_PATH);
		}
		_exit(errno == ENOEXEC ? 42 : 43);
	}
	close(fd);
	if (guest < 0) {
		ksft_test_result_fail("%s: fork: %s\n", r->name, strerror(errno));
		return;
	}

	traps = clones = 0;
	trace_ok = true;
	clone_parent = clone_child = 0;
	status = serve(r->timeout, &put_ok);
	if (status == -1)
		ok = false;
	else if (r->want >= 0)
		ok = WIFEXITED(status) && WEXITSTATUS(status) == r->want;
	else
		ok = WIFSIGNALED(status) && WTERMSIG(status) == -r->want;
	if (native_peer > 0) {
		int peer_status = 0, j;
		for (j = 0; j < 500; j++) {
			if (waitpid(native_peer, &peer_status, WNOHANG) == native_peer)
				break;
			sleep_ms(10);
		}
		if (j == 500) {
			kill(native_peer, SIGKILL);
			waitpid(native_peer, &peer_status, 0);
			ok = false;
		}
		if (!WIFEXITED(peer_status) || WEXITSTATUS(peer_status))
			ok = false;
	}
	if (r->traced && (!trace_ok || traps != (r->want_traps ?: 2) ||
			  clones != (r->trace_clones ? 1 : 0))) {
		ksft_print_msg("%s: %d SIGTRAP stops, %d clone events, ptrace %s\n",
			       r->name, traps, clones, trace_ok ? "ok" : "failed");
		ok = false;
	}
	if (!ok && status != -1)
		ksft_print_msg("%s: wait status 0x%x\n", r->name, status);
	if (!put_ok)
		ksft_print_msg("%s: answering the killed guest failed\n", r->name);
	if (r->freeze && !freeze_ok)
		ksft_print_msg("%s: the guest did not freeze\n", r->name);
	if (!strcmp(r->mode, "stop-install") && stop_install_result != -(long)R_SESSION_CLOSED) {
		ksft_print_msg("%s: installing the handle gave %ld\n", r->name, stop_install_result);
		ok = false;
	}
	if (!strcmp(r->mode, "stop-kill") && status != -1 && !after_stop_kill())
		ok = false;
	ksft_test_result(ok && put_ok && (!r->freeze || freeze_ok), "%s\n", r->name);
}

static const struct run runs[] = {
	{ "native HIPC across processes, 39-bit", "p3-cross-server", 3, HORIZON_EXEC, IMAGE_OK, 0, false, false, 20 },
	{ "native HIPC across processes, 36-bit", "p3-cross-server", 1, HORIZON_EXEC, IMAGE_OK, 0, false, false, 20 },
	{ "SVCs, 39-bit address space", "basic", 3, HORIZON_EXEC, IMAGE_OK, 0, false, false, 120 },
	{ "SVCs, 36-bit address space", "basic", 1, HORIZON_EXEC, IMAGE_OK, 0, false, false, 120 },
	{ "the core mask is the CPU affinity at exec", "coremask", 3,
	  HORIZON_EXEC, IMAGE_OK, 0, false, false, 20, 0x3 },
	{ "svcExitProcess with an unstarted thread", "exit-unstarted", 3,
	  HORIZON_EXEC, IMAGE_OK, 0, false, false, 20 },
	{ "last svcExitThread with an unstarted thread", "exitthread-unstarted", 3,
	  HORIZON_EXEC, IMAGE_OK, 0, false, false, 20 },
	{ "svcBreak", "break", 3, HORIZON_EXEC, IMAGE_OK, 1, false, false, 20 },
	{ "svcBreak under a debugger stops, then goes on", "traced-break", 3,
	  HORIZON_EXEC, IMAGE_OK, 5, false, false, 20, 0, true },
	{ "DebuggerPresence is process-wide", "traced-worker-info", 3,
	  HORIZON_EXEC, IMAGE_OK, 5, false, false, 20, 0, true, false, 1 },
	{ "new threads report clone events and both break kinds", "traced-worker-break", 3,
	  HORIZON_EXEC, IMAGE_OK, 5, false, false, 20, 0, true, true, 3 },
	{ "an exception the program does not handle", "exception-unhandled", 3,
	  HORIZON_EXEC, IMAGE_OK, -SIGSEGV, false, false, 20 },
	{ "SIGKILL with threads waiting in every way", "hang", 3,
	  HORIZON_EXEC, IMAGE_OK, -SIGKILL, false, false, 30 },
	{ "a stopped requester killed while its service installs a handle", "stop-install", 3,
	  HORIZON_EXEC, IMAGE_OK, -SIGKILL, false, false, 20 },
	{ "a stopped program killed with threads waiting in every way", "stop-kill", 3,
	  HORIZON_EXEC, IMAGE_OK, -SIGKILL, false, false, 30 },
	{ "freezing with threads waiting in every way", "freeze", 3,
	  HORIZON_EXEC, IMAGE_OK, 0, true, false, 30 },
	{ "execve() with threads waiting in every way", "exec", 3,
	  HORIZON_EXEC, IMAGE_OK, 0, false, true, 30 },
	{ "image with a bad code set is not executed", "-", 3,
	  HORIZON_EXEC, IMAGE_BAD_SIZE, 42, false, false, 20 },
	{ "plain execveat() does not run Horizon images", "-", 3,
	  PLAIN_EXEC, IMAGE_OK, 42, false, false, 20 },
	{ "horizon_execve() still runs ELF", "-", 3, ELF_EXEC, IMAGE_OK, 0, false, true, 20 },
};

int main(int argc, char **argv)
{
	const char *guest_path = argc > 1 ? argv[1] : "hzn_guest";
	struct sigaction sa = { .sa_handler = on_alarm };	/* no SA_RESTART */
	struct itimerval it = { { 0, 50000 }, { 0, 50000 } };
	char path[PATH_MAX];
	cpu_set_t cpus;
	unsigned int i;
	long r;

	ksft_print_header();
	r = servctl(HZN_SCTL_REGISTER_NAMED_SERVICE, (long)HZN_TEST_PORT, 0, 0, 0, 0);
	if (r == -ENOSYS)
		ksft_exit_skip("no Horizon support (CONFIG_HORIZON)\n");
	if (r)
		ksft_exit_fail_msg("REGISTER_NAMED_SERVICE: 0x%lx\n", -r);

	/* The guest is next to us. */
	if (!strchr(guest_path, '/') && readlink("/proc/self/exe", path, sizeof(path) - 64) > 0) {
		path[sizeof(path) - 64] = 0;
		strcpy(strrchr(path, '/') + 1, guest_path);
		guest_path = path;
	}
	if (load_guest(guest_path))
		ksft_exit_fail_msg("cannot read %s\n", guest_path);
	if (!access("/bin/true", X_OK))
		true_path = "/bin/true";
	else if (!access("/usr/bin/true", X_OK))
		true_path = "/usr/bin/true";
	if (!geteuid())
		setup_freezer();

	/* Stay off CPU 0, where the guest's (SCHED_FIFO) threads run. */
	if (!sched_getaffinity(0, sizeof(cpus), &cpus) && CPU_COUNT(&cpus) > 1) {
		CPU_CLR(0, &cpus);
		sched_setaffinity(0, sizeof(cpus), &cpus);
	}

	/* Interrupt GET_CMD now and then, to notice that the guest exited. */
	sigaction(SIGALRM, &sa, NULL);
	setitimer(ITIMER_REAL, &it, NULL);

	if (argc > 2 && !strcmp(argv[2], "--gdb-inferior")) {
		struct run debug = { "external GDB with a new Horizon thread",
			"gdb-worker-break", 3, HORIZON_EXEC, IMAGE_OK, 5, false, false, 120 };
		ksft_set_plan(1);
		run_one(&debug);
	} else if (argc > 2 && (!strcmp(argv[2], "--p3-benchmark") || !strcmp(argv[2], "--bridge-benchmark"))) {
		struct run bench = { "Horizon benchmark", !strcmp(argv[2], "--p3-benchmark") ?
			"p3-perf" : "bridge-perf", 3, HORIZON_EXEC, IMAGE_OK, 0, false, false, 120 };
		ksft_set_plan(1);
		run_one(&bench);
	} else {
		ksft_set_plan(ARRAY_SIZE(runs));
		for (i = 0; i < ARRAY_SIZE(runs); i++)
			run_one(&runs[i]);
	}

	cleanup_freezer();
	ksft_finished();
}
