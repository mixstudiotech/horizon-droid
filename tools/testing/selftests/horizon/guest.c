// SPDX-License-Identifier: GPL-2.0
/*
 * A Horizon program for the horizon selftests. It is freestanding: "svc #N"
 * (N > 0) are Horizon SVCs, "svc #0" stays a Linux system call, used here
 * for output, exit and exec. argv[1] says what to do (see guest_main()); the
 * exit status of "basic" is the number of failed checks. Output lines are
 * TAP diagnostics ("# ...") of hzn_test's TAP output.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hzn_test.h"

typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;

/* Linux system calls */
#define NR_write		64
#define NR_exit_group		94
#define NR_sched_getscheduler	120
#define NR_getpid		172
#define NR_geteuid		175
#define NR_gettid		178
#define NR_execve		221

/* Horizon result codes (module 1, the kernel) */
#define KRES(desc)		(1u | ((desc) << 9))
#define R_SUCCESS		0u
#define R_INVALID_SIZE		KRES(101)
#define R_INVALID_ADDRESS	KRES(102)
#define R_INVALID_CURRENT_MEMORY	KRES(106)
#define R_INVALID_NEW_MEMORY_PERMISSION	KRES(108)
#define R_INVALID_MEMORY_REGION	KRES(110)
#define R_INVALID_PRIORITY	KRES(112)
#define R_INVALID_CORE_ID	KRES(113)
#define R_INVALID_HANDLE	KRES(114)
#define R_INVALID_COMBINATION	KRES(116)
#define R_TIMED_OUT		KRES(117)
#define R_CANCELLED		KRES(118)
#define R_INVALID_ENUM_VALUE	KRES(120)
#define R_NOT_FOUND		KRES(121)
#define R_BUSY			KRES(122)
#define R_NOT_HANDLED		KRES(124)
#define R_INVALID_STATE		KRES(125)
#define R_NO_THREAD		KRES(129)
#define R_UNKNOWN		0xffffffffu
#define R_HIPC_REMOTE_DEAD	(11u | (301u << 9))

#define CUR_THREAD		0xffff8000u
#define CUR_PROCESS		0xffff8001u
#define HANDLE_WAIT_MASK	0x40000000u

#define IDEAL_CORE_DONT_CARE	(-1)
#define IDEAL_CORE_USE_PROCESS	(-2)
#define IDEAL_CORE_NO_UPDATE	(-3)

enum {
	MS_FREE = 0x00, MS_CODE = 0x03, MS_CODE_DATA = 0x04, MS_NORMAL = 0x05,
	MS_STACK = 0x0b, MS_THREAD_LOCAL = 0x0c,
};
#define MA_LOCKED	1u
#define MA_PERMISSION_LOCKED	0x10u
#define PERM_NONE	0u
#define PERM_R		1u
#define PERM_RW		3u
#define PERM_RX		5u

enum {
	INFO_CORE_MASK = 0,
	INFO_ALIAS_REGION_ADDR = 2, INFO_ALIAS_REGION_SIZE = 3,
	INFO_HEAP_REGION_ADDR = 4, INFO_HEAP_REGION_SIZE = 5,
	INFO_DEBUGGER_PRESENCE = 8,
	INFO_RESOURCE_LIMIT = 9, INFO_IDLE_TICK_COUNT = 10,
	INFO_RANDOM_ENTROPY = 11,
	INFO_STACK_REGION_ADDR = 14, INFO_STACK_REGION_SIZE = 15,
	INFO_TITLE_ID = 18, INFO_INITIAL_PROCESS_ID_RANGE = 19,
	INFO_USER_EXCEPTION_CONTEXT_ADDR = 20,
	INFO_IS_APPLICATION = 23, INFO_FREE_THREAD_COUNT = 24,
	INFO_THREAD_TICK_COUNT = 25, INFO_IS_SVC_PERMITTED = 26,
	INFO_IO_REGION_HINT = 27, INFO_ALIAS_REGION_EXTRA_SIZE = 28,
	INFO_THREAD_TICK_COUNT_OLD = 0xf0000002,
};

enum { WAIT_IF_LESS_THAN, DECREMENT_AND_WAIT_IF_LESS_THAN, WAIT_IF_EQUAL };
enum { SIGNAL, SIGNAL_AND_INCREMENT_IF_EQUAL, SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL };

#define PAGE		0x1000ul
#define MS		1000000ll

struct mem_info {
	u64 addr, size;
	u32 state, attr, perm, ipc_refcount, device_refcount, padding;
};

struct thread_context {
	u64 r[29];
	u64 fp, lr, sp, pc;
	u32 pstate, padding;
	u64 v[32][2];
	u32 fpcr, fpsr;
	u64 tpidr;
};

/* ---- libc bits the compiler may call ---------------------------------- */

void *memset(void *d, int c, size_t n)
{
	u8 *p = d;

	while (n--)
		*p++ = c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	u8 *p = d;
	const u8 *q = s;

	while (n--)
		*p++ = *q++;
	return d;
}

static size_t slen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

static bool seq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* ---- system calls ------------------------------------------------------ */

static long lsys(long nr, long a, long b, long c)
{
	register long x8 asm("x8") = nr;
	register long x0 asm("x0") = a;
	register long x1 asm("x1") = b;
	register long x2 asm("x2") = c;

	asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
	return x0;
}

#define SVC(n, a0, a1, a2, a3, a4, a5, out)				\
({									\
	register u64 _x0 asm("x0") = (u64)(a0);				\
	register u64 _x1 asm("x1") = (u64)(a1);				\
	register u64 _x2 asm("x2") = (u64)(a2);				\
	register u64 _x3 asm("x3") = (u64)(a3);				\
	register u64 _x4 asm("x4") = (u64)(a4);				\
	register u64 _x5 asm("x5") = (u64)(a5);				\
	asm volatile("svc %6"						\
		     : "+r"(_x0), "+r"(_x1), "+r"(_x2), "+r"(_x3),	\
		       "+r"(_x4), "+r"(_x5)				\
		     : "i"(n) : "memory", "cc");			\
	*(out) = _x1;							\
	(u32)_x0;							\
})

/* "svc #n" with x0..x7 from r[] and back into it. */
#define SVCR(n, r)							\
({									\
	register u64 _x0 asm("x0") = (r)[0];				\
	register u64 _x1 asm("x1") = (r)[1];				\
	register u64 _x2 asm("x2") = (r)[2];				\
	register u64 _x3 asm("x3") = (r)[3];				\
	register u64 _x4 asm("x4") = (r)[4];				\
	register u64 _x5 asm("x5") = (r)[5];				\
	register u64 _x6 asm("x6") = (r)[6];				\
	register u64 _x7 asm("x7") = (r)[7];				\
	asm volatile("svc %8"						\
		     : "+r"(_x0), "+r"(_x1), "+r"(_x2), "+r"(_x3),	\
		       "+r"(_x4), "+r"(_x5), "+r"(_x6), "+r"(_x7)	\
		     : "i"(n) : "memory", "cc");			\
	(r)[0] = _x0, (r)[1] = _x1, (r)[2] = _x2, (r)[3] = _x3;		\
	(r)[4] = _x4, (r)[5] = _x5, (r)[6] = _x6, (r)[7] = _x7;		\
	(u32)_x0;							\
})

static u32 svc_set_heap_size(u64 *addr, u64 size)
{
	return SVC(0x01, 0, size, 0, 0, 0, 0, addr);
}

static u32 svc_set_memory_permission(u64 addr, u64 size, u32 perm)
{
	u64 o;

	return SVC(0x02, addr, size, perm, 0, 0, 0, &o);
}

static u32 svc_set_memory_attribute(u64 addr, u64 size, u32 mask, u32 value)
{
	u64 o;

	return SVC(0x03, addr, size, mask, value, 0, 0, &o);
}

static u32 svc_map_memory(u64 dst, u64 src, u64 size)
{
	u64 o;

	return SVC(0x04, dst, src, size, 0, 0, 0, &o);
}

static u32 svc_unmap_memory(u64 dst, u64 src, u64 size)
{
	u64 o;

	return SVC(0x05, dst, src, size, 0, 0, 0, &o);
}

static u32 svc_query_memory(struct mem_info *mi, u64 addr)
{
	u64 o;

	return SVC(0x06, mi, 0, addr, 0, 0, 0, &o);
}

static void __attribute__((noreturn)) svc_exit_process(void)
{
	u64 o;

	SVC(0x07, 0, 0, 0, 0, 0, 0, &o);
	__builtin_unreachable();
}

static u32 svc_create_thread(u32 *handle, void (*entry)(u64), u64 arg,
			     u64 stack_top, s32 priority, s32 core)
{
	u64 o;
	u32 r = SVC(0x08, 0, entry, arg, stack_top, (s64)priority, (s64)core, &o);

	*handle = o;
	return r;
}

static u32 svc_start_thread(u32 handle)
{
	u64 o;

	return SVC(0x09, handle, 0, 0, 0, 0, 0, &o);
}

static void __attribute__((noreturn)) svc_exit_thread(void)
{
	u64 o;

	SVC(0x0a, 0, 0, 0, 0, 0, 0, &o);
	__builtin_unreachable();
}

static u32 svc_sleep_thread(s64 ns)
{
	u64 o;

	return SVC(0x0b, ns, 0, 0, 0, 0, 0, &o);
}

static u32 svc_get_thread_priority(u32 *prio, u32 handle)
{
	u64 o;
	u32 r = SVC(0x0c, 0, handle, 0, 0, 0, 0, &o);

	*prio = o;
	return r;
}

static u32 svc_set_thread_priority(u32 handle, u32 prio)
{
	u64 o;

	return SVC(0x0d, handle, prio, 0, 0, 0, 0, &o);
}

static u32 svc_get_thread_core_mask(s32 *core, u64 *mask, u32 handle)
{
	u64 r[8] = { 0, 0, handle };
	u32 res = SVCR(0x0e, r);

	*core = r[1];
	*mask = r[2];
	return res;
}

static u32 svc_set_thread_core_mask(u32 handle, s32 core, u64 mask)
{
	u64 o;

	return SVC(0x0f, handle, (s64)core, mask, 0, 0, 0, &o);
}

static u32 svc_get_current_processor_number(void)
{
	u64 o;

	return SVC(0x10, 0, 0, 0, 0, 0, 0, &o);
}

static u32 svc_clear_event(u32 handle)
{
	u64 o;

	return SVC(0x12, handle, 0, 0, 0, 0, 0, &o);
}

static u32 svc_create_transfer_memory(u32 *handle, u64 addr, u64 size, u32 perm)
{
	u64 o;
	u32 r = SVC(0x15, 0, addr, size, perm, 0, 0, &o);

	*handle = o;
	return r;
}

static u32 svc_close_handle(u32 handle)
{
	u64 o;

	return SVC(0x16, handle, 0, 0, 0, 0, 0, &o);
}

static u32 svc_wait_synchronization(s32 *index, const u32 *handles, s32 num,
				    s64 timeout)
{
	u64 o;
	u32 r = SVC(0x18, 0, handles, (s64)num, timeout, 0, 0, &o);

	*index = o;
	return r;
}

static u32 svc_cancel_synchronization(u32 handle)
{
	u64 o;

	return SVC(0x19, handle, 0, 0, 0, 0, 0, &o);
}

static u32 svc_arbitrate_lock(u32 owner, u32 *addr, u32 tag)
{
	u64 o;

	return SVC(0x1a, owner, addr, tag, 0, 0, 0, &o);
}

static u32 svc_arbitrate_unlock(u32 *addr)
{
	u64 o;

	return SVC(0x1b, addr, 0, 0, 0, 0, 0, &o);
}

static u32 svc_wait_process_wide_key(u32 *addr, u32 *key, u32 tag, s64 timeout)
{
	u64 o;

	return SVC(0x1c, addr, key, tag, timeout, 0, 0, &o);
}

static u32 svc_signal_process_wide_key(u32 *key, s32 count)
{
	u64 o;

	return SVC(0x1d, key, (s64)count, 0, 0, 0, 0, &o);
}

/* The tick is the whole of x0. */
static u64 svc_get_system_tick(void)
{
	register u64 x0 asm("x0");

	asm volatile("svc 0x1e" : "=r"(x0) : : "x1", "memory", "cc");
	return x0;
}

static u32 svc_connect_to_named_port(u32 *handle, const char *name)
{
	u64 o;
	u32 r = SVC(0x1f, 0, name, 0, 0, 0, 0, &o);

	*handle = o;
	return r;
}

static u32 svc_signal_event(u32 handle)
{
	u64 o;

	return SVC(0x11, handle, 0, 0, 0, 0, 0, &o);
}

static u32 svc_send_sync_request_with_user_buffer(void *buf, u64 size, u32 handle)
{
	u64 o;

	return SVC(0x22, buf, size, handle, 0, 0, 0, &o);
}

static u32 svc_send_async_request_with_user_buffer(u32 *event, void *buf, u64 size,
						   u32 handle)
{
	u64 o;
	u32 r = SVC(0x23, 0, buf, size, handle, 0, 0, &o);

	*event = o;
	return r;
}

static u32 svc_send_sync_request(u32 handle)
{
	u64 o;

	return SVC(0x21, handle, 0, 0, 0, 0, 0, &o);
}

static u32 svc_get_process_id(u64 *id, u32 handle)
{
	return SVC(0x24, 0, handle, 0, 0, 0, 0, id);
}

static u32 svc_get_thread_id(u64 *id, u32 handle)
{
	return SVC(0x25, 0, handle, 0, 0, 0, 0, id);
}

static u32 svc_break(u32 reason, u64 info1, u64 info2)
{
	u64 o;

	return SVC(0x26, reason, info1, info2, 0, 0, 0, &o);
}

static u32 svc_output_debug_string(const char *s, u64 len)
{
	u64 o;

	return SVC(0x27, s, len, 0, 0, 0, 0, &o);
}

static u32 svc_return_from_exception(u32 result)
{
	u64 o;

	return SVC(0x28, result, 0, 0, 0, 0, 0, &o);
}

static u32 svc_get_info(u64 *out, u32 type, u32 handle, u64 sub)
{
	return SVC(0x29, 0, type, handle, sub, 0, 0, out);
}

static u32 svc_map_physical_memory(u64 addr, u64 size)
{
	u64 o;

	return SVC(0x2c, addr, size, 0, 0, 0, 0, &o);
}

static u32 svc_unmap_physical_memory(u64 addr, u64 size)
{
	u64 o;

	return SVC(0x2d, addr, size, 0, 0, 0, 0, &o);
}

static u32 svc_get_last_thread_info(void)
{
	u64 r[8] = { 0 };

	return SVCR(0x2f, r);
}

static u32 svc_set_thread_activity(u32 handle, u32 activity)
{
	u64 o;

	return SVC(0x32, handle, activity, 0, 0, 0, 0, &o);
}

static u32 svc_get_thread_context3(struct thread_context *ctx, u32 handle)
{
	u64 o;

	return SVC(0x33, ctx, handle, 0, 0, 0, 0, &o);
}

static u32 svc_wait_for_address(volatile u32 *addr, u32 type, s32 value, s64 timeout)
{
	u64 o;

	return SVC(0x34, addr, type, (s64)value, timeout, 0, 0, &o);
}

static u32 svc_signal_to_address(volatile u32 *addr, u32 type, s32 value, s32 count)
{
	u64 o;

	return SVC(0x35, addr, type, (s64)value, (s64)count, 0, 0, &o);
}

static u32 svc_synchronize_preemption_state(void)
{
	u64 o;

	return SVC(0x36, 0, 0, 0, 0, 0, 0, &o);
}

static u32 svc_unimplemented(void)
{
	u64 o;

	return SVC(0x7f, 0, 0, 0, 0, 0, 0, &o);
}

static void *get_tls(void)
{
	void *p;

	asm volatile("mrs %0, tpidrro_el0" : "=r"(p));
	return p;
}

static u64 cntpct(void)
{
	u64 t;

	asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t) : : "memory");
	return t;
}

static u64 cntfrq(void)
{
	u64 f;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}

/* ---- output and checks --------------------------------------------------- */

static void out(const char *s)
{
	lsys(NR_write, 1, (long)s, slen(s));
}

static void out_hex(u64 v)
{
	char b[19];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 16; i++)
		b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	b[18] = 0;
	out(b);
}

static void out_dec(u64 v)
{
	char b[21];
	int i = 20;

	b[i] = 0;
	do {
		b[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	out(b + i);
}

static int tests, failures;

static bool check(const char *name, bool ok)
{
	tests++;
	if (!ok)
		failures++;
	out(ok ? "# ok " : "# not ok ");
	out_dec(tests);
	out(" ");
	out(name);
	out("\n");
	return ok;
}

static bool check_eq(const char *name, u64 got, u64 want)
{
	tests++;
	if (got != want)
		failures++;
	out(got == want ? "# ok " : "# not ok ");
	out_dec(tests);
	out(" ");
	out(name);
	if (got != want) {
		out(": got ");
		out_hex(got);
		out(", want ");
		out_hex(want);
	}
	out("\n");
	return got == want;
}

/* ---- synchronization, as libnx does it --------------------------------- */

static void mutex_lock(u32 *m, u32 self)
{
	u32 cur = __atomic_load_n(m, __ATOMIC_ACQUIRE);

	for (;;) {
		if (!cur) {
			if (__atomic_compare_exchange_n(m, &cur, self, false,
							__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
				return;
			continue;
		}
		if (!(cur & HANDLE_WAIT_MASK) &&
		    !__atomic_compare_exchange_n(m, &cur, cur | HANDLE_WAIT_MASK, false,
						 __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
			continue;
		if (svc_arbitrate_lock(cur & ~HANDLE_WAIT_MASK, m, self) != R_SUCCESS)
			svc_break(0, 1, 0);
		cur = __atomic_load_n(m, __ATOMIC_ACQUIRE);
		if ((cur & ~HANDLE_WAIT_MASK) == self)
			return;
	}
}

static void mutex_unlock(u32 *m, u32 self)
{
	u32 cur = self;

	if (__atomic_compare_exchange_n(m, &cur, 0, false, __ATOMIC_RELEASE,
					__ATOMIC_RELAXED))
		return;
	if (cur & HANDLE_WAIT_MASK)
		svc_arbitrate_unlock(m);
}

static u32 condvar_wait(u32 *cv, u32 *m, u32 self, s64 timeout)
{
	u32 r = svc_wait_process_wide_key(m, cv, self, timeout);

	if (r == R_TIMED_OUT)
		mutex_lock(m, self);
	return r;
}

/* ---- threads ------------------------------------------------------------- */

#define NR_STACKS	8
#define STACK_SIZE	0x8000
static u8 stacks[NR_STACKS][STACK_SIZE] __attribute__((aligned(16)));
#define STACK_TOP(i)	((u64)&stacks[i][STACK_SIZE])

static u32 main_handle;
static u32 handles[NR_STACKS];
static int other_core;		/* a second core, or 0 */

/* Waits for the thread with @handle to exit. */
static bool join(u32 handle, s64 timeout)
{
	s32 index;

	return svc_wait_synchronization(&index, &handle, 1, timeout) == R_SUCCESS &&
	       index == 0;
}

/* Waits until *addr != value (at most ~5 s). */
static bool wait_change(volatile u32 *addr, u32 value)
{
	int i;

	for (i = 0; i < 50 && *addr == value; i++)
		svc_wait_for_address(addr, WAIT_IF_EQUAL, value, 100 * MS);
	return *addr != value;
}

static void set_and_wake(volatile u32 *addr, u32 value)
{
	__atomic_store_n(addr, value, __ATOMIC_RELEASE);
	svc_signal_to_address(addr, SIGNAL, 0, -1);
}

/* ---- tests ----------------------------------------------------------------- */

static const u32 rodata_word = 0x5a5aa5a5;
static u32 data_word = 1;
static u32 bss_word;

static u64 info(u32 type, u32 handle, u64 sub)
{
	u64 v = 0;

	if (svc_get_info(&v, type, handle, sub) != R_SUCCESS)
		return ~0ull;
	return v;
}

static void test_basics(void)
{
	struct mem_info mi;
	u64 tls = (u64)get_tls(), t0, t1, v;
	u32 r;

	check("TLS page is set", tls && !(tls & (PAGE - 1)));
	svc_query_memory(&mi, tls);
	check_eq("TLS page is thread-local memory", mi.state, MS_THREAD_LOCAL);
	check_eq("TLS page is read-write", mi.perm, PERM_RW);
	check_eq("the main thread's handle is in its TLS (thread_handle)",
		 *(volatile u32 *)(tls + 0x110), main_handle);

	t0 = cntpct();
	svc_sleep_thread(1 * MS);
	v = svc_get_system_tick();
	t1 = cntpct();
	check("CNTPCT_EL0 can be read and advances", t1 > t0 && cntfrq());
	check("GetSystemTick is CNTPCT_EL0", v >= t0 && v <= t1);

	check_eq("title ID", info(INFO_TITLE_ID, CUR_PROCESS, 0), HZN_TEST_TITLE_ID);
	check("random entropy", info(INFO_RANDOM_ENTROPY, 0, 0) != info(INFO_RANDOM_ENTROPY, 0, 0));
	check("thread tick count", info(INFO_THREAD_TICK_COUNT_OLD, CUR_THREAD, ~0ull) + 1 > 1);
	check_eq("unknown info type", svc_get_info(&v, 0x1234, CUR_PROCESS, 0),
		 R_INVALID_ENUM_VALUE);

	svc_query_memory(&mi, (u64)&test_basics);
	check_eq("text is code", mi.state, MS_CODE);
	check_eq("text is R-X", mi.perm, PERM_RX);
	svc_query_memory(&mi, (u64)&rodata_word);
	check_eq("rodata is code", mi.state, MS_CODE);
	check_eq("rodata is R--", mi.perm, PERM_R);
	svc_query_memory(&mi, (u64)&data_word);
	check_eq("data is code data", mi.state, MS_CODE_DATA);
	check_eq("data is RW-", mi.perm, PERM_RW);
	svc_query_memory(&mi, (u64)&bss_word);
	check_eq("bss is code data", mi.state, MS_CODE_DATA);

	check_eq("main thread runs on its ideal core", svc_get_current_processor_number(), 0);
	r = svc_set_memory_attribute(tls, PAGE, 8, 8);
	check_eq("SetMemoryAttribute(uncached)", r, R_SUCCESS);
	check_eq("OutputDebugString", svc_output_debug_string("hzn_guest says hi", 17), R_SUCCESS);
	check_eq("unimplemented SVC", svc_unimplemented(), R_UNKNOWN);
	check_eq("svcBreak(notification only) returns", svc_break(0x80000003u, 0, 0), R_SUCCESS);
	check_eq("svc #0 is still Linux", lsys(NR_getpid, 0, 0, 0) > 0, 1);
	if (lsys(NR_geteuid, 0, 0, 0) == 0)
		check_eq("main thread is SCHED_FIFO", lsys(NR_sched_getscheduler, 0, 0, 0), 1);
}

static u64 heap;

static void test_heap(void)
{
	u64 base = info(INFO_HEAP_REGION_ADDR, CUR_PROCESS, 0), addr = 0;
	volatile u64 *p;
	struct mem_info mi;
	u64 i;
	bool zero = true;

	check_eq("SetHeapSize(size not a multiple of 2 MiB)", svc_set_heap_size(&addr, 0x1000),
		 R_INVALID_SIZE);
	check_eq("SetHeapSize(4 MiB)", svc_set_heap_size(&addr, 0x400000), R_SUCCESS);
	check_eq("heap is at the heap region", addr, base);
	heap = addr;
	p = (volatile u64 *)heap;
	p[0] = 0x1234;
	p[0x3ffff8 / 8] = 0x5678;
	svc_query_memory(&mi, heap);
	check_eq("heap is normal memory", mi.state, MS_NORMAL);
	check_eq("heap is RW-", mi.perm, PERM_RW);

	check_eq("SetHeapSize(8 MiB)", svc_set_heap_size(&addr, 0x800000), R_SUCCESS);
	check("growing keeps the contents", addr == heap && p[0] == 0x1234 &&
	      p[0x3ffff8 / 8] == 0x5678);
	p[0x7ffff8 / 8] = 0x9abc;
	check_eq("SetHeapSize(6 MiB)", svc_set_heap_size(&addr, 0x600000), R_SUCCESS);
	check_eq("SetHeapSize(8 MiB) again", svc_set_heap_size(&addr, 0x800000), R_SUCCESS);
	for (i = 0x600000; i < 0x800000; i += 8)
		zero &= p[i / 8] == 0;
	check("memory given back reads as zero", zero);
	check_eq("heap below survives", p[0x3ffff8 / 8], 0x5678);
}

/* A free range of @len in [start, start + size), 64 KiB aligned, or 0. */
static u64 find_free(u64 start, u64 size, u64 len)
{
	u64 addr = start, s, e;
	struct mem_info mi;

	while (addr < start + size) {
		if (svc_query_memory(&mi, addr) != R_SUCCESS)
			return 0;
		if (mi.state == MS_FREE) {
			s = mi.addr < start ? start : mi.addr;
			e = mi.addr + mi.size;
			if (e > start + size)
				e = start + size;
			s = (s + 0xffff) & ~0xffffull;
			if (e > s && e - s >= len)
				return s;
		}
		if (mi.addr + mi.size <= addr)
			return 0;
		addr = mi.addr + mi.size;
	}
	return 0;
}

static void test_map_memory(void)
{
	u64 stack = info(INFO_STACK_REGION_ADDR, CUR_PROCESS, 0);
	u64 stack_size = info(INFO_STACK_REGION_SIZE, CUR_PROCESS, 0);
	u64 alias = info(INFO_ALIAS_REGION_ADDR, CUR_PROCESS, 0);
	u64 alias_size = info(INFO_ALIAS_REGION_SIZE, CUR_PROCESS, 0);
	u64 src = heap + 0x100000, size = 0x10000, dst, addr, i;
	volatile u32 *s = (u32 *)src, *d;
	struct mem_info mi;
	bool ok = true;

	dst = stack_size ? find_free(stack, stack_size, size) : find_free(alias, alias_size, size);
	if (!check("found a free range for svcMapMemory", dst))
		return;
	d = (u32 *)dst;
	for (i = 0; i < size / 4; i++)
		s[i] = i ^ 0xabcd;
	check_eq("MapMemory outside the alias and stack regions",
		 svc_map_memory(heap + 0x200000, src, size), R_INVALID_MEMORY_REGION);
	check_eq("MapMemory", svc_map_memory(dst, src, size), R_SUCCESS);
	for (i = 0; i < size / 4; i++)
		ok &= d[i] == (i ^ 0xabcd);
	check("the alias shows the source", ok);
	d[5] = 0xfeed;
	svc_query_memory(&mi, src);
	check_eq("the source is locked", mi.attr & MA_LOCKED, MA_LOCKED);
	svc_query_memory(&mi, dst);
	check_eq("the alias is stack memory", mi.state, MS_STACK);
	check_eq("heap cannot shrink below an aliased source",
		 svc_set_heap_size(&addr, 0), R_INVALID_STATE);
	check_eq("UnmapMemory with the wrong size", svc_unmap_memory(dst, src, size / 2),
		 R_INVALID_MEMORY_REGION);
	check_eq("UnmapMemory", svc_unmap_memory(dst, src, size), R_SUCCESS);
	check("the source has what was written to the alias", s[5] == 0xfeed && s[6] == (6 ^ 0xabcd));
	svc_query_memory(&mi, dst);
	check_eq("the alias is gone", mi.state, MS_FREE);

	addr = find_free(alias, alias_size, 0x10000);
	check_eq("MapPhysicalMemory", svc_map_physical_memory(addr, 0x10000), R_SUCCESS);
	((volatile u32 *)addr)[100] = 7;
	svc_query_memory(&mi, addr);
	check("physical memory is RW-", mi.perm == PERM_RW && ((volatile u32 *)addr)[100] == 7);
	check_eq("UnmapPhysicalMemory", svc_unmap_physical_memory(addr, 0x10000), R_SUCCESS);
	svc_query_memory(&mi, addr);
	check_eq("physical memory is gone", mi.state, MS_FREE);
}

/* A page of module data, protected like the RELRO of an SDK module. */
static u8 relro_page[PAGE] __attribute__((aligned(PAGE))) = { 0x42 };

static void test_memory_permission(void)
{
	u64 page = heap + 0x300000, relro = (u64)relro_page, tls = (u64)get_tls();
	u64 text = (u64)&test_basics & ~(PAGE - 1);
	volatile u32 *p = (u32 *)page;
	struct mem_info mi;
	u32 h;

	p[1] = 0x600d;
	check_eq("SetMemoryPermission(unaligned)",
		 svc_set_memory_permission(page + 8, PAGE, PERM_R), R_INVALID_ADDRESS);
	check_eq("SetMemoryPermission(size 0)", svc_set_memory_permission(page, 0, PERM_R),
		 R_INVALID_SIZE);
	check_eq("SetMemoryPermission(R-X)", svc_set_memory_permission(page, PAGE, PERM_RX),
		 R_INVALID_NEW_MEMORY_PERMISSION);
	check_eq("SetMemoryPermission(-W-)", svc_set_memory_permission(page, PAGE, 2),
		 R_INVALID_NEW_MEMORY_PERMISSION);
	check_eq("SetMemoryPermission(text)", svc_set_memory_permission(text, PAGE, PERM_R),
		 R_INVALID_CURRENT_MEMORY);
	check_eq("SetMemoryPermission(TLS)", svc_set_memory_permission(tls, PAGE, PERM_R),
		 R_INVALID_CURRENT_MEMORY);

	check_eq("SetMemoryPermission(heap, R--)", svc_set_memory_permission(page, PAGE, PERM_R),
		 R_SUCCESS);
	svc_query_memory(&mi, page);
	check("... the page is R-- normal memory", mi.addr == page && mi.size == PAGE &&
	      mi.state == MS_NORMAL && mi.perm == PERM_R);
	check_eq("... and keeps its contents", p[1], 0x600d);
	check_eq("SetMemoryPermission(heap, ---)",
		 svc_set_memory_permission(page, PAGE, PERM_NONE), R_SUCCESS);
	svc_query_memory(&mi, page);
	check_eq("... the page is ---", mi.perm, PERM_NONE);
	check_eq("SetMemoryPermission(heap, RW-)", svc_set_memory_permission(page, PAGE, PERM_RW),
		 R_SUCCESS);
	p[2] = 0xbeef;
	check("... the page is writable again", p[1] == 0x600d && p[2] == 0xbeef);
	check_eq("PermissionLocked on the heap",
		 svc_set_memory_attribute(page, PAGE, MA_PERMISSION_LOCKED, MA_PERMISSION_LOCKED),
		 R_INVALID_CURRENT_MEMORY);

	/* rtld and nn::ro::ProtectRelro: make it read-only, then lock that. */
	check_eq("SetMemoryPermission(module data, R--)",
		 svc_set_memory_permission(relro, PAGE, PERM_R), R_SUCCESS);
	svc_query_memory(&mi, relro);
	check("... it stays code data", mi.state == MS_CODE_DATA && mi.perm == PERM_R &&
	      mi.attr == 0);
	check_eq("PermissionLocked without its value",
		 svc_set_memory_attribute(relro, PAGE, MA_PERMISSION_LOCKED, 0),
		 R_INVALID_COMBINATION);
	check_eq("SetMemoryAttribute(PermissionLocked)",
		 svc_set_memory_attribute(relro, PAGE, MA_PERMISSION_LOCKED, MA_PERMISSION_LOCKED),
		 R_SUCCESS);
	svc_query_memory(&mi, relro);
	check("... QueryMemory reports it", mi.addr == relro && mi.size == PAGE &&
	      mi.state == MS_CODE_DATA && mi.perm == PERM_R && mi.attr == MA_PERMISSION_LOCKED);
	check_eq("... the contents stay", relro_page[0], 0x42);
	check_eq("... locking it again",
		 svc_set_memory_attribute(relro, PAGE, MA_PERMISSION_LOCKED, MA_PERMISSION_LOCKED),
		 R_SUCCESS);
	check_eq("SetMemoryPermission(locked)", svc_set_memory_permission(relro, PAGE, PERM_RW),
		 R_INVALID_CURRENT_MEMORY);
	check_eq("CreateTransferMemory(locked)",
		 svc_create_transfer_memory(&h, relro, PAGE, PERM_R), R_INVALID_CURRENT_MEMORY);
	svc_query_memory(&mi, relro + PAGE);
	check("the next page is neither locked nor read-only", mi.addr == relro + PAGE &&
	      mi.state == MS_CODE_DATA && mi.perm == PERM_RW && mi.attr == 0);
}

/*
 * User exceptions. The entry point (see the end) gets them with x0 the type
 * and x1 the exception info, saves x9-x29, which the kernel does not
 * restore, and calls guest_exception() on a stack of its own, like rtld and
 * nnSdk's UserExceptionHandler.
 */
struct exc_info {
	u64 r[9];
	u64 lr, sp, pc;
	u32 pstate, afsr0, afsr1, esr;
	u64 far;
};

#define EXC_INSTRUCTION_ABORT	0x100u
#define EXC_DATA_ABORT		0x101u
#define ESR_EC(esr)		((esr) >> 26)

u8 exc_stack[0x2000] __attribute__((aligned(16)));
static volatile u32 exc_count, exc_type, exc_esr, exc_result;
static volatile u64 exc_pc, exc_far, exc_info_addr, exc_x3, exc_tick;
static volatile u32 exc_hold, exc_holding;

u32 guest_exception(u32 type, struct exc_info *info);

/* Returns the result for svcReturnFromException. */
u32 guest_exception(u32 type, struct exc_info *info)
{
	exc_count++;
	exc_type = type;
	exc_info_addr = (u64)info;
	exc_pc = info->pc;
	exc_far = info->far;
	exc_esr = info->esr;
	exc_tick = cntpct();
	while (exc_hold) {
		exc_holding = 1;
		svc_sleep_thread(1 * MS);
	}
	if (exc_result != R_SUCCESS)
		return exc_result;
	info->pc += 4;			/* go on after the faulting instruction */
	if (exc_x3)
		info->r[3] = exc_x3;	/* as if it had loaded this */
	return R_SUCCESS;
}

static volatile u64 release_tick;

static void fault_fn(u64 addr)
{
	(void)*(volatile u32 *)addr;
	svc_exit_thread();
}

static void release_fn(u64 arg)
{
	svc_sleep_thread(30 * MS);
	release_tick = cntpct();
	exc_hold = 0;
	svc_exit_thread();
}

static void test_exceptions(void)
{
	u64 plr = info(INFO_USER_EXCEPTION_CONTEXT_ADDR, CUR_PROCESS, 0), pc;
	u64 got_x0, got_x3, got_x20;
	u32 w, h_release, r, n;

	check_eq("ReturnFromException outside of an exception",
		 svc_return_from_exception(R_SUCCESS), R_INVALID_STATE);

	/* A load from an unmapped page; the handler supplies the value. */
	{
		register u64 x0 asm("x0") = 0x1111;
		register u64 x2 asm("x2") = 0x10;
		register u64 x3 asm("x3") = 0;
		register u64 x20 asm("x20") = 0x2222;

		exc_x3 = 0x7777;
		asm volatile("adr %[pc], 1f\n"
			     "1: ldr x3, [x2]\n"
			     : [pc] "=&r"(pc), "+r"(x0), "+r"(x3), "+r"(x20)
			     : "r"(x2) : "memory");
		/* Register variables do not survive calls: copy them first. */
		got_x0 = x0;
		got_x3 = x3;
		got_x20 = x20;
		exc_x3 = 0;
		check_eq("a data abort enters the handler", exc_count, 1);
		check_eq("... as DataAbort", exc_type, EXC_DATA_ABORT);
		check_eq("... with the info in the process local region", exc_info_addr,
			 plr + 0x148);
		check_eq("... the faulting PC", exc_pc, pc);
		check_eq("... the fault address", exc_far, 0x10);
		check_eq("... and the ESR of a data abort", ESR_EC(exc_esr), 0x24);
		check_eq("it resumes with the x3 the handler left", got_x3, 0x7777);
		check_eq("... x0 as it was", got_x0, 0x1111);
		check_eq("... and x20, which the handler saved", got_x20, 0x2222);
	}

	/* A store to the permission-locked page of test_memory_permission(). */
	{
		register u64 x2 asm("x2") = (u64)relro_page;

		asm volatile("str xzr, [x2]" : : "r"(x2) : "memory");
		check("a store to locked RELRO is a data abort",
		      exc_count == 2 && exc_type == EXC_DATA_ABORT &&
		      exc_far == (u64)relro_page && (exc_esr & (1u << 6)));
		check_eq("... and stores nothing", relro_page[0], 0x42);
	}

	asm volatile("udf #0x5a" : : : "memory");
	check("an undefined instruction is an instruction abort",
	      exc_count == 3 && exc_type == EXC_INSTRUCTION_ABORT && ESR_EC(exc_esr) == 0);
	asm volatile("brk #0x123" : : : "memory");
	check("so is brk", exc_count == 4 && exc_type == EXC_INSTRUCTION_ABORT &&
	      ESR_EC(exc_esr) == 0x3c);

	/* One thread at a time: another one that faults waits for the first. */
	exc_hold = 1;
	exc_holding = 0;
	release_tick = 0;
	n = exc_count;
	if (svc_create_thread(&w, fault_fn, 0x20, STACK_TOP(1), HZN_TEST_PRIORITY, -2) ||
	    svc_start_thread(w)) {
		check("thread for the exception test", false);
		exc_hold = 0;
		return;
	}
	wait_change(&exc_holding, 0);
	r = svc_create_thread(&h_release, release_fn, 0, STACK_TOP(2), HZN_TEST_PRIORITY, -2);
	if (r == R_SUCCESS)
		svc_start_thread(h_release);
	(void)*(volatile u32 *)0x28;
	check("a second thread's exception waits for the first",
	      exc_count == n + 2 && exc_far == 0x28 && release_tick &&
	      exc_tick >= release_tick);
	check("both threads are done", join(w, 5000 * MS) &&
	      (r != R_SUCCESS || join(h_release, 5000 * MS)));
	svc_close_handle(w);
	if (r == R_SUCCESS)
		svc_close_handle(h_release);
}

static volatile u32 gate, ready, flag;
static u64 worker_tls, worker_tid, worker_cpu;
static u32 worker_tls_state, worker_tls_handle;

static void worker_fn(u64 arg)
{
	struct mem_info mi;

	worker_tls = (u64)get_tls();
	worker_tls_handle = *(volatile u32 *)(worker_tls + 0x110);
	svc_query_memory(&mi, worker_tls);
	worker_tls_state = mi.state;
	svc_get_thread_id(&worker_tid, CUR_THREAD);
	set_and_wake(&ready, 1);
	while (gate == 0)
		svc_wait_for_address(&gate, WAIT_IF_EQUAL, 0, -1);
	worker_cpu = svc_get_current_processor_number();
	flag = arg;
	svc_exit_thread();
}

static void nop_fn(u64 arg)
{
	svc_exit_thread();
}

static void test_threads(void)
{
	u32 h, h2, prio = 0, r;
	struct mem_info mi;
	u64 id, tls_total = 0, addr;
	int i;

	check_eq("CreateThread(priority 64)",
		 svc_create_thread(&h, worker_fn, 1, STACK_TOP(0), 64, -2), R_INVALID_PRIORITY);
	r = svc_create_thread(&h, worker_fn, 0x77, STACK_TOP(0), HZN_TEST_PRIORITY, -2);
	if (!check_eq("CreateThread", r, R_SUCCESS))
		return;
	check_eq("GetThreadPriority", svc_get_thread_priority(&prio, h) == R_SUCCESS ? prio : 99,
		 HZN_TEST_PRIORITY);
	svc_sleep_thread(20 * MS);
	check_eq("a thread does not run before StartThread", ready, 0);
	check_eq("StartThread", svc_start_thread(h), R_SUCCESS);
	check_eq("StartThread again", svc_start_thread(h), R_INVALID_STATE);
	check("the thread runs", wait_change(&ready, 0));
	check("it has a TLS page of its own", worker_tls && worker_tls != (u64)get_tls() &&
	      worker_tls_state == MS_THREAD_LOCAL);
	check_eq("its handle is in its TLS (thread_handle)", worker_tls_handle, h);
	check_eq("GetThreadId", svc_get_thread_id(&id, h) == R_SUCCESS ? id : 0, worker_tid);
	check_eq("GetThreadId(main)", svc_get_thread_id(&id, main_handle) == R_SUCCESS ? id : 0,
		 lsys(NR_getpid, 0, 0, 0));
	check_eq("SetThreadPriority", svc_set_thread_priority(h, 40), R_SUCCESS);
	svc_get_thread_priority(&prio, h);
	check_eq("GetThreadPriority after SetThreadPriority", prio, 40);
	if (other_core)
		check_eq("SetThreadCoreMask", svc_set_thread_core_mask(h, other_core,
								       1ull << other_core), R_SUCCESS);
	check_eq("WaitSynchronization(running thread, 0)", join(h, 0), 0);
	set_and_wake(&gate, 1);
	check("WaitSynchronization(thread) ends when it exits", join(h, 5000 * MS));
	check_eq("the thread ran to the end", flag, 0x77);
	if (other_core)
		check_eq("it moved to the other core", worker_cpu, other_core);
	svc_query_memory(&mi, worker_tls);
	check_eq("its TLS page is gone", mi.state, MS_FREE);
	check_eq("CloseHandle(thread)", svc_close_handle(h), R_SUCCESS);
	check_eq("CloseHandle again", svc_close_handle(h), R_INVALID_HANDLE);

	/* Threads come and go without leaving thread-local pages behind. */
	for (i = 0; i < 64; i++) {
		if (svc_create_thread(&h2, nop_fn, 0, STACK_TOP(1), HZN_TEST_PRIORITY, -2) ||
		    svc_start_thread(h2) || !join(h2, 5000 * MS))
			break;
		svc_close_handle(h2);
	}
	check_eq("64 threads created, started and joined", i, 64);
	addr = (u64)get_tls() & ~0xffffffull;
	for (i = 0; i < 4096; i++) {
		if (svc_query_memory(&mi, addr) || mi.addr + mi.size <= addr)
			break;
		if (mi.state == MS_THREAD_LOCAL)
			tls_total += mi.size;
		addr = mi.addr + mi.size;
		if (addr > (u64)get_tls() + 0x10000 * PAGE)
			break;
	}
	check_eq("only the main thread's TLS page and the process local region are left",
		 tls_total, 2 * PAGE);
}

static void test_core_mask(void)
{
	u64 mask = info(INFO_CORE_MASK, CUR_PROCESS, 0), aff = 0;
	s32 core = 99;
	u32 h;

	check("the core mask has the ideal core", mask != ~0ull && (mask & 1));
	if (other_core)
		check("... and the other core", mask & 2);
	check_eq("GetThreadCoreMask(main)", svc_get_thread_core_mask(&core, &aff, CUR_THREAD),
		 R_SUCCESS);
	check("... is its ideal core only", core == 0 && aff == 1);
	check_eq("GetThreadCoreMask(bad handle)", svc_get_thread_core_mask(&core, &aff, 0x7777),
		 R_INVALID_HANDLE);
	if (!check_eq("CreateThread", svc_create_thread(&h, nop_fn, 0, STACK_TOP(0),
							HZN_TEST_PRIORITY, IDEAL_CORE_USE_PROCESS),
		      R_SUCCESS))
		return;
	svc_get_thread_core_mask(&core, &aff, h);
	check("a new thread has the process ideal core", core == 0 && aff == 1);
	check_eq("SetThreadCoreMask(not in the process mask)",
		 svc_set_thread_core_mask(h, 0, 1ull << 63), R_INVALID_CORE_ID);
	check_eq("SetThreadCoreMask(no cores)",
		 svc_set_thread_core_mask(h, IDEAL_CORE_DONT_CARE, 0), R_INVALID_COMBINATION);
	check_eq("SetThreadCoreMask(bad ideal core)", svc_set_thread_core_mask(h, -4, 1),
		 R_INVALID_CORE_ID);
	check_eq("SetThreadCoreMask(no ideal core)",
		 svc_set_thread_core_mask(h, IDEAL_CORE_DONT_CARE, mask), R_SUCCESS);
	svc_get_thread_core_mask(&core, &aff, h);
	check("... reads back", core == -1 && aff == mask);
	check_eq("SetThreadCoreMask(keep the ideal core)",
		 svc_set_thread_core_mask(h, IDEAL_CORE_NO_UPDATE, 1), R_SUCCESS);
	svc_get_thread_core_mask(&core, &aff, h);
	check("... reads back", core == -1 && aff == 1);
	check_eq("SetThreadCoreMask(the process ideal core)",
		 svc_set_thread_core_mask(h, IDEAL_CORE_USE_PROCESS, 0), R_SUCCESS);
	svc_get_thread_core_mask(&core, &aff, h);
	check("... reads back", core == 0 && aff == 1);
	if (other_core) {
		check_eq("SetThreadCoreMask(ideal core not in the affinity)",
			 svc_set_thread_core_mask(h, 1, 1), R_INVALID_COMBINATION);
		check_eq("SetThreadCoreMask(kept ideal core not in the affinity)",
			 svc_set_thread_core_mask(h, IDEAL_CORE_NO_UPDATE, 2), R_INVALID_COMBINATION);
	}
	svc_start_thread(h);
	join(h, 5000 * MS);
	svc_close_handle(h);
}

static void test_info(void)
{
	struct mem_info mi;
	u64 v = 0, v2, plr;

	check_eq("GetInfo(process information, subtype 1)",
		 svc_get_info(&v, INFO_TITLE_ID, CUR_PROCESS, 1), R_INVALID_COMBINATION);
	check_eq("GetInfo(process information, thread handle)",
		 svc_get_info(&v, INFO_TITLE_ID, CUR_THREAD, 0), R_INVALID_HANDLE);
	check_eq("no debugger: DebuggerPresence is 0", info(INFO_DEBUGGER_PRESENCE, 0, 0), 0);
	check_eq("DebuggerPresence rejects a handle",
		 svc_get_info(&v, INFO_DEBUGGER_PRESENCE, CUR_PROCESS, 0), R_INVALID_HANDLE);
	check_eq("DebuggerPresence rejects a subtype",
		 svc_get_info(&v, INFO_DEBUGGER_PRESENCE, 0, 1), R_INVALID_COMBINATION);
	check_eq("ResourceLimit: none", info(INFO_RESOURCE_LIMIT, 0, 0), 0);
	check_eq("ResourceLimit(process handle)",
		 svc_get_info(&v, INFO_RESOURCE_LIMIT, CUR_PROCESS, 0), R_INVALID_HANDLE);
	v = info(INFO_IDLE_TICK_COUNT, 0, ~0ull);
	svc_sleep_thread(20 * MS);
	v2 = info(INFO_IDLE_TICK_COUNT, 0, ~0ull);
	check("IdleTickCount grows while the core sleeps", v != ~0ull && v2 != ~0ull && v2 > v);
	check_eq("IdleTickCount(this core)", svc_get_info(&v, INFO_IDLE_TICK_COUNT, 0,
							  svc_get_current_processor_number()),
		 R_SUCCESS);
	check_eq("IdleTickCount(another core)", svc_get_info(&v, INFO_IDLE_TICK_COUNT, 0, 63),
		 R_INVALID_COMBINATION);
	check_eq("IdleTickCount(a handle)",
		 svc_get_info(&v, INFO_IDLE_TICK_COUNT, CUR_PROCESS, ~0ull), R_INVALID_HANDLE);
	check_eq("InitialProcessIdRange is gone since 5.0.0",
		 svc_get_info(&v, INFO_INITIAL_PROCESS_ID_RANGE, 0, 0), R_INVALID_ENUM_VALUE);
	plr = info(INFO_USER_EXCEPTION_CONTEXT_ADDR, CUR_PROCESS, 0);
	svc_query_memory(&mi, plr);
	check("UserExceptionContextAddress is a thread-local page",
	      plr && !(plr & (PAGE - 1)) && mi.state == MS_THREAD_LOCAL && mi.perm == PERM_RW);
	((volatile u64 *)plr)[0x1c0 / 8] = 0x1234;	/* the dying message region */
	check_eq("IsApplication", info(INFO_IS_APPLICATION, CUR_PROCESS, 0), 1);
	v = info(INFO_FREE_THREAD_COUNT, CUR_PROCESS, 0);
	check("FreeThreadCount", v > 0 && v <= 0x7fffffff);
	v = info(INFO_THREAD_TICK_COUNT, CUR_THREAD, ~0ull);
	check("ThreadTickCount", v && v != ~0ull);
	check("ThreadTickCount(this core)", info(INFO_THREAD_TICK_COUNT, CUR_THREAD, 0) >= v);
	if (other_core)
		check_eq("ThreadTickCount(another core)",
			 info(INFO_THREAD_TICK_COUNT, CUR_THREAD, other_core), 0);
	check_eq("ThreadTickCount(core 64)", svc_get_info(&v, INFO_THREAD_TICK_COUNT, CUR_THREAD, 64),
		 R_INVALID_COMBINATION);
	check_eq("ThreadTickCount(bad handle)", svc_get_info(&v, INFO_THREAD_TICK_COUNT, 0x7777, ~0ull),
		 R_INVALID_HANDLE);
	check_eq("IsSvcPermitted(SynchronizePreemptionState)",
		 info(INFO_IS_SVC_PERMITTED, 0, 0x36), 1);
	check_eq("IsSvcPermitted(another SVC)", svc_get_info(&v, INFO_IS_SVC_PERMITTED, 0, 0x35),
		 R_INVALID_COMBINATION);
	check_eq("IoRegionHint: no I/O regions",
		 svc_get_info(&v, INFO_IO_REGION_HINT, main_handle, 0), R_INVALID_HANDLE);
	check_eq("AliasRegionExtraSize", info(INFO_ALIAS_REGION_EXTRA_SIZE, CUR_PROCESS, 0), 0);
	check_eq("SynchronizePreemptionState", svc_synchronize_preemption_state(), R_SUCCESS);
}

static volatile u32 cancel_result;

static void cancel_fn(u64 arg)
{
	s32 index;

	/* The main thread does not exit: this waits until it is cancelled. */
	cancel_result = svc_wait_synchronization(&index, &main_handle, 1, -1);
	svc_exit_thread();
}

static volatile u32 spin_go, spin_count;

/* Counts in user space, with known values in some registers. */
static void spin_fn(u64 arg)
{
	asm volatile("mov	x19, #0x1919\n"
		     "mov	x20, #0x2020\n"
		     "mov	x28, #0x2828\n"
		     ".inst	0x9e670268\n"	/* fmov d8, x19 */
		     "msr	tpidr_el0, x20\n"
		     "1:	ldr	w9, [%0]\n"
		     "add	w9, w9, #1\n"
		     "str	w9, [%0]\n"
		     "ldr	w9, [%1]\n"
		     "cbnz	w9, 1b\n"
		     : : "r"(&spin_count), "r"(&spin_go)
		     : "x9", "x19", "x20", "x28", "memory");
	svc_exit_thread();
}

static u32 pause_word;
static volatile u32 woke;

/* Waits in svcWaitForAddress with a known x19. */
static void svc_wait_fn(u64 arg)
{
	register u64 x0 asm("x0") = (u64)&pause_word;
	register u64 x1 asm("x1") = WAIT_IF_EQUAL;
	register u64 x2 asm("x2") = 0;
	register u64 x3 asm("x3") = -1;

	asm volatile("mov	x19, #0x1919\n"
		     "svc	0x34"
		     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
		     : : "x4", "x5", "x19", "memory", "cc");
	set_and_wake(&woke, 1);
	svc_exit_thread();
}

static void test_thread_svcs(void)
{
	static struct thread_context ctx;
	u32 h, c1, c2, r;
	u64 id = 0;
	s32 index;
	int i;

	check_eq("GetProcessId(current process)",
		 svc_get_process_id(&id, CUR_PROCESS) ? 0 : id, lsys(NR_getpid, 0, 0, 0));
	check_eq("GetProcessId(current thread)",
		 svc_get_process_id(&id, CUR_THREAD) ? 0 : id, lsys(NR_getpid, 0, 0, 0));
	check_eq("GetProcessId(a thread handle)",
		 svc_get_process_id(&id, main_handle) ? 0 : id, lsys(NR_getpid, 0, 0, 0));
	check_eq("GetProcessId(bad handle)", svc_get_process_id(&id, 0x7777), R_INVALID_HANDLE);
	check_eq("GetLastThreadInfo: none, as on retail Horizon", svc_get_last_thread_info(),
		 R_NO_THREAD);

	/* CancelSynchronization */
	check_eq("CancelSynchronization(bad handle)", svc_cancel_synchronization(0x7777),
		 R_INVALID_HANDLE);
	check_eq("CancelSynchronization(current thread)", svc_cancel_synchronization(CUR_THREAD),
		 R_SUCCESS);
	check_eq("... a wait with time-out 0 still times out",
		 svc_wait_synchronization(&index, &main_handle, 1, 0), R_TIMED_OUT);
	check_eq("... the next wait is cancelled",
		 svc_wait_synchronization(&index, &main_handle, 1, 1000 * MS), R_CANCELLED);
	check_eq("... and only that one", svc_wait_synchronization(&index, &main_handle, 1, 5 * MS),
		 R_TIMED_OUT);
	cancel_result = 0;
	r = svc_create_thread(&h, cancel_fn, 0, STACK_TOP(1), HZN_TEST_PRIORITY, -2);
	if (check_eq("create the waiter", r, R_SUCCESS)) {
		svc_start_thread(h);
		svc_sleep_thread(20 * MS);
		check_eq("CancelSynchronization(waiting thread)", svc_cancel_synchronization(h),
			 R_SUCCESS);
		check("... ends its wait with Cancelled", join(h, 5000 * MS) &&
		      cancel_result == R_CANCELLED);
		svc_cancel_synchronization(CUR_THREAD);
		check_eq("a signalled handle wins over a cancel request",
			 svc_wait_synchronization(&index, &h, 1, 5 * MS), R_SUCCESS);
		check_eq("... which stays",
			 svc_wait_synchronization(&index, &main_handle, 1, 5 * MS), R_CANCELLED);
		svc_close_handle(h);
	}

	/* SetThreadActivity, GetThreadContext3: a thread in user space */
	spin_go = 1;
	spin_count = 0;
	/* On one core it would never let the main thread run again. */
	if (other_core && check_eq("create the spinner",
				   svc_create_thread(&h, spin_fn, 0, STACK_TOP(1),
						     HZN_TEST_PRIORITY, other_core),
				   R_SUCCESS)) {
		check_eq("SetThreadActivity(unstarted thread)", svc_set_thread_activity(h, 1),
			 R_INVALID_STATE);
		svc_start_thread(h);
		for (i = 0; i < 1000 && !spin_count; i++)
			svc_sleep_thread(MS);
		check_eq("GetThreadContext3(running thread)", svc_get_thread_context3(&ctx, h),
			 R_INVALID_STATE);
		check_eq("SetThreadActivity(pause)", svc_set_thread_activity(h, 1), R_SUCCESS);
		c1 = spin_count;
		svc_sleep_thread(20 * MS);
		c2 = spin_count;
		check("a paused thread does not run", c1 && c1 == c2);
		check_eq("SetThreadActivity(pause) again", svc_set_thread_activity(h, 1),
			 R_INVALID_STATE);
		check_eq("GetThreadContext3", svc_get_thread_context3(&ctx, h), R_SUCCESS);
		check("... registers", ctx.r[19] == 0x1919 && ctx.r[20] == 0x2020 &&
		      ctx.r[28] == 0x2828);
		check("... PC in the loop", ctx.pc > (u64)spin_fn && ctx.pc < (u64)spin_fn + 0x80);
		check("... SP on its stack", ctx.sp > (u64)stacks[1] && ctx.sp <= STACK_TOP(1));
		check_eq("... TPIDR_EL0", ctx.tpidr, 0x2020);
		check_eq("... v8", ctx.v[8][0], 0x1919);
		check_eq("SetThreadActivity(resume)", svc_set_thread_activity(h, 0), R_SUCCESS);
		for (i = 0; i < 1000 && spin_count == c2; i++)
			svc_sleep_thread(MS);
		check("it runs again", spin_count != c2);
		check_eq("SetThreadActivity(resume) again", svc_set_thread_activity(h, 0),
			 R_INVALID_STATE);
		check_eq("SetThreadActivity(current thread)",
			 svc_set_thread_activity(CUR_THREAD, 1), R_BUSY);
		check_eq("SetThreadActivity(bad activity)", svc_set_thread_activity(h, 2),
			 R_INVALID_ENUM_VALUE);
		check_eq("GetThreadContext3(current thread)",
			 svc_get_thread_context3(&ctx, CUR_THREAD), R_BUSY);
		check_eq("GetThreadContext3(bad handle)", svc_get_thread_context3(&ctx, 0x7777),
			 R_INVALID_HANDLE);
		spin_go = 0;
		check("the spinner ends", join(h, 5000 * MS));
		/* Its handle retains the exited thread identity. */
		check_eq("SetThreadActivity(exited thread)", svc_set_thread_activity(h, 1),
			 R_INVALID_STATE);
		svc_close_handle(h);
	}

	/* A thread paused in an SVC */
	woke = 0;
	r = svc_create_thread(&h, svc_wait_fn, 0, STACK_TOP(1), HZN_TEST_PRIORITY, -2);
	if (!check_eq("create the waiter", r, R_SUCCESS))
		return;
	svc_start_thread(h);
	svc_sleep_thread(20 * MS);
	check_eq("SetThreadActivity(pause a waiting thread)", svc_set_thread_activity(h, 1),
		 R_SUCCESS);
	check_eq("GetThreadContext3", svc_get_thread_context3(&ctx, h), R_SUCCESS);
	check_eq("... the PC is the svc instruction", *(u32 *)ctx.pc, 0xd4000681);
	check("... only callee-saved registers", !ctx.r[0] && !ctx.r[18] && ctx.r[19] == 0x1919);
	svc_signal_to_address(&pause_word, SIGNAL, 0, -1);
	svc_sleep_thread(20 * MS);
	check_eq("its wait ends, but it stays paused", woke, 0);
	check_eq("SetThreadActivity(resume)", svc_set_thread_activity(h, 0), R_SUCCESS);
	check("... and it goes on", wait_change(&woke, 0) && join(h, 5000 * MS));
	svc_close_handle(h);
}

static u32 mtx;
static u64 counter;
#define MUTEX_ITERS 3000

static void mutex_fn(u64 arg)
{
	u32 self = handles[arg];
	u64 c;
	int i;

	for (i = 0; i < MUTEX_ITERS; i++) {
		mutex_lock(&mtx, self);
		c = counter;
		if (!(i & 31))
			svc_sleep_thread(0);
		counter = c + 1;
		mutex_unlock(&mtx, self);
	}
	if (arg)
		svc_exit_thread();
}

static u32 cv_mtx, cv, cv2;
static u32 queue_len, produced, consumed;
#define CV_ITEMS 500

static void consumer_fn(u64 arg)
{
	u32 self = handles[arg];

	for (;;) {
		mutex_lock(&cv_mtx, self);
		while (!queue_len && produced < CV_ITEMS)
			condvar_wait(&cv, &cv_mtx, self, -1);
		if (!queue_len) {
			mutex_unlock(&cv_mtx, self);
			break;
		}
		queue_len--;
		consumed++;
		mutex_unlock(&cv_mtx, self);
	}
	svc_exit_thread();
}

static volatile u32 arb_word, arb_woken;

static void arb_waiter_fn(u64 arg)
{
	u32 r = svc_wait_for_address(&arb_word, WAIT_IF_EQUAL, 0, -1);

	arb_woken = r == R_SUCCESS ? 1 : 2;
	svc_exit_thread();
}

static void test_sync(void)
{
	u32 r;
	int i, ok;
	u64 t0, t1;

	/* ArbitrateLock/ArbitrateUnlock under contention */
	r = svc_create_thread(&handles[1], mutex_fn, 1, STACK_TOP(1), HZN_TEST_PRIORITY, -2);
	r |= svc_create_thread(&handles[2], mutex_fn, 2, STACK_TOP(2), HZN_TEST_PRIORITY,
			       other_core);
	if (!check_eq("create the mutex threads", r, R_SUCCESS))
		return;
	handles[0] = main_handle;
	svc_start_thread(handles[1]);
	svc_start_thread(handles[2]);
	mutex_fn(0);
	ok = join(handles[1], 20000 * MS) && join(handles[2], 20000 * MS);
	check("mutex threads finish", ok);
	check_eq("mutual exclusion", counter, 3 * MUTEX_ITERS);
	check_eq("the mutex is free", mtx, 0);
	check_eq("ArbitrateLock on a lock that changed hands",
		 svc_arbitrate_lock(handles[1], &mtx, main_handle), R_SUCCESS);
	mtx = CUR_THREAD | HANDLE_WAIT_MASK;
	check_eq("ArbitrateLock with a pseudo handle as the owner",
		 svc_arbitrate_lock(CUR_THREAD, &mtx, main_handle), R_INVALID_HANDLE);
	mtx = 0;
	svc_close_handle(handles[1]);
	svc_close_handle(handles[2]);

	/* WaitProcessWideKeyAtomic/SignalProcessWideKey */
	r = svc_create_thread(&handles[3], consumer_fn, 3, STACK_TOP(3), HZN_TEST_PRIORITY, -2);
	r |= svc_create_thread(&handles[4], consumer_fn, 4, STACK_TOP(4), HZN_TEST_PRIORITY,
			       other_core);
	if (!check_eq("create the consumers", r, R_SUCCESS))
		return;
	svc_start_thread(handles[3]);
	svc_start_thread(handles[4]);
	for (i = 0; i < CV_ITEMS; i++) {
		mutex_lock(&cv_mtx, main_handle);
		queue_len++;
		produced++;
		svc_signal_process_wide_key(&cv, 1);
		mutex_unlock(&cv_mtx, main_handle);
		if (!(i & 15))
			svc_sleep_thread(0);
	}
	mutex_lock(&cv_mtx, main_handle);
	svc_signal_process_wide_key(&cv, -1);
	mutex_unlock(&cv_mtx, main_handle);
	ok = join(handles[3], 20000 * MS) && join(handles[4], 20000 * MS);
	check("consumers finish", ok);
	check_eq("everything produced was consumed", consumed, CV_ITEMS);
	check_eq("the condition variable mutex is free", cv_mtx, 0);
	svc_close_handle(handles[3]);
	svc_close_handle(handles[4]);

	mutex_lock(&cv_mtx, main_handle);
	t0 = cntpct();
	r = condvar_wait(&cv2, &cv_mtx, main_handle, 10 * MS);
	t1 = cntpct();
	check_eq("WaitProcessWideKeyAtomic times out", r, R_TIMED_OUT);
	check("... after the time-out", (t1 - t0) * 1000 >= cntfrq() * 9);
	check_eq("... and the mutex is ours again", cv_mtx, main_handle);
	mutex_unlock(&cv_mtx, main_handle);
	/* As on Horizon, only a signal that finds no waiter clears the key. */
	check_eq("the key still says there are waiters", cv2, 1);
	check_eq("SignalProcessWideKey without waiters",
		 svc_signal_process_wide_key(&cv2, 1), R_SUCCESS);
	check_eq("... clears the key", cv2, 0);

	/* WaitForAddress/SignalToAddress */
	arb_word = 0;
	check_eq("WaitIfEqual, not equal", svc_wait_for_address(&arb_word, WAIT_IF_EQUAL, 1, 0),
		 R_INVALID_STATE);
	check_eq("WaitIfEqual, time-out 0", svc_wait_for_address(&arb_word, WAIT_IF_EQUAL, 0, 0),
		 R_TIMED_OUT);
	arb_word = 3;
	t0 = cntpct();
	r = svc_wait_for_address(&arb_word, WAIT_IF_LESS_THAN, 5, 10 * MS);
	t1 = cntpct();
	check("WaitIfLessThan times out", r == R_TIMED_OUT && (t1 - t0) * 1000 >= cntfrq() * 9);
	check_eq("DecrementAndWaitIfLessThan",
		 svc_wait_for_address(&arb_word, DECREMENT_AND_WAIT_IF_LESS_THAN, 5, 0), R_TIMED_OUT);
	check_eq("... decrements", arb_word, 2);
	check_eq("bad arbitration type", svc_wait_for_address(&arb_word, 3, 0, 0),
		 R_INVALID_ENUM_VALUE);
	check_eq("SignalAndIncrementIfEqual",
		 svc_signal_to_address(&arb_word, SIGNAL_AND_INCREMENT_IF_EQUAL, 2, 1), R_SUCCESS);
	check_eq("... increments", arb_word, 3);
	check_eq("SignalAndIncrementIfEqual, not equal",
		 svc_signal_to_address(&arb_word, SIGNAL_AND_INCREMENT_IF_EQUAL, 2, 1),
		 R_INVALID_STATE);
	check_eq("SignalAndModifyByWaitingCountIfEqual, no waiters",
		 svc_signal_to_address(&arb_word, SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL, 3, 1),
		 R_SUCCESS);
	check_eq("... increments", arb_word, 4);

	arb_word = 0;
	r = svc_create_thread(&handles[5], arb_waiter_fn, 0, STACK_TOP(5), HZN_TEST_PRIORITY, -2);
	if (!check_eq("create the address waiter", r, R_SUCCESS))
		return;
	svc_start_thread(handles[5]);
	svc_sleep_thread(20 * MS);
	check_eq("the waiter waits", arb_woken, 0);
	check_eq("SignalAndModifyByWaitingCountIfEqual, a waiter",
		 svc_signal_to_address(&arb_word, SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL, 0, 1),
		 R_SUCCESS);
	check_eq("... one waiter: -1", arb_word, (u32)-1);
	check("the waiter was woken", join(handles[5], 5000 * MS) && arb_woken == 1);
	svc_close_handle(handles[5]);

	t0 = cntpct();
	check_eq("SleepThread(20 ms)", svc_sleep_thread(20 * MS), R_SUCCESS);
	t1 = cntpct();
	check("... sleeps 20 ms", (t1 - t0) * 1000 >= cntfrq() * 19);
	check_eq("SleepThread(-1), a yield", svc_sleep_thread(-1), R_SUCCESS);
	check_eq("SleepThread(-2), a yield", svc_sleep_thread(-2), R_SUCCESS);
}

/* ---- IPC with hzn_test ------------------------------------------------------ */

static u32 session;

/* Sends @cmd with @nargs arguments; results go to @res. Returns the SVC result. */
static u32 ipc(u32 sess, u32 cmd, int nargs, const u64 *args, u64 *res, int nres)
{
	u32 *b = get_tls(), r;
	int i;

	b[0] = 4;
	b[1] = 2 + 2 * nargs;
	b[2] = cmd;
	b[3] = 0;
	for (i = 0; i < nargs; i++) {
		b[4 + 2 * i] = args[i];
		b[5 + 2 * i] = args[i] >> 32;
	}
	r = svc_send_sync_request(sess);
	if (r)
		return r;
	if (b[2])
		return 0xdead;
	for (i = 0; i < nres; i++)
		res[i] = b[4 + 2 * i] | (u64)b[5 + 2 * i] << 32;
	return 0;
}

static u64 ipc1(u32 cmd, u64 a0, u64 a1, u64 a2)
{
	u64 args[3] = { a0, a1, a2 }, res = ~0ull;

	if (ipc(session, cmd, 3, args, &res, 1))
		return ~0ull;
	return res;
}

static void fill(volatile u8 *p, u64 len, u8 seed)
{
	u64 i;

	for (i = 0; i < len; i++)
		p[i] = seed + i * 7;
}

static bool has(volatile u8 *p, u64 len, u8 seed)
{
	u64 i;

	for (i = 0; i < len; i++)
		if (p[i] != (u8)(seed + i * 7))
			return false;
	return true;
}

static u64 sum(volatile u8 *p, u64 len)
{
	u64 s = 0, i;

	for (i = 0; i < len; i++)
		s += p[i];
	return s;
}

static volatile u32 ipc_thread_ok;

static void ipc_thread_fn(u64 arg)
{
	u64 res[3] = { 0 };

	if (!ipc(session, CMD_IDS, 0, NULL, res, 3) && res[0] == lsys(NR_getpid, 0, 0, 0))
		ipc_thread_ok = 1;
	else
		ipc_thread_ok = 2;
	svc_exit_thread();
}

static void test_ipc(void)
{
	u64 res[3], args[3], heap_buf = heap + 0x300000, mw = heap + 0x380000;
	u8 local[0x6000];
	volatile u8 *stack_buf = (u8 *)(((u64)local + PAGE - 1) & ~(PAGE - 1));
	u32 h, ev, s2, *b = get_tls();
	s32 index;
	u32 r;

	check_eq("ConnectToNamedPort(no such port)", svc_connect_to_named_port(&h, "nosuchport"),
		 R_NOT_FOUND);
	r = svc_connect_to_named_port(&session, HZN_TEST_PORT);
	if (!check_eq("ConnectToNamedPort", r, R_SUCCESS))
		return;

	check_eq("MAP_MEMORY rejects overflowing ranges",
		 ipc1(CMD_MAP_INVALID, heap_buf, 0, 0), 15);
	check_eq("echo", ipc1(CMD_ECHO, 41, 0, 0), 42);
	check_eq("echo again (same session)", ipc1(CMD_ECHO, 99, 0, 0), 100);
	r = ipc(session, CMD_IDS, 0, NULL, res, 3);
	check_eq("GET_PROCESS_ID", r ? 0 : res[0], lsys(NR_getpid, 0, 0, 0));
	check_eq("GET_TITLE_ID", r ? 0 : res[1], HZN_TEST_TITLE_ID);
	check("the service gave the session an ID", !r && res[2]);

	fill((u8 *)heap_buf, 0x3000, 3);
	check_eq("READ_BUFFER (heap)", ipc1(CMD_READ, heap_buf + 5, 0x2ff0, 0),
		 sum((u8 *)heap_buf + 5, 0x2ff0));
	fill(stack_buf, 100, 9);
	check_eq("READ_BUFFER (stack)", ipc1(CMD_READ, (u64)stack_buf, 100, 0), sum(stack_buf, 100));
	check_eq("WRITE_BUFFER", ipc1(CMD_WRITE, heap_buf + 0x10, 0x2100, 77), 0);
	check("... wrote", has((u8 *)heap_buf + 0x10, 0x2100, 77));
	check_eq("READ_BUFFER_FROM (pid)", ipc1(CMD_READ_FROM, heap_buf, 0x1000, 0),
		 sum((u8 *)heap_buf, 0x1000));

	ipc_thread_ok = 0;
	if (!svc_create_thread(&h, ipc_thread_fn, 0, STACK_TOP(6), HZN_TEST_PRIORITY, -2)) {
		svc_start_thread(h);
		join(h, 5000 * MS);
		svc_close_handle(h);
	}
	check_eq("GET_PROCESS_ID from another thread is the process ID", ipc_thread_ok, 1);

	/* Events */
	ev = ipc1(CMD_EVENT, 0, 0, 0);
	check("CREATE_COPY_HANDLE (an eventfd)", ev && ev != ~0u);
	check_eq("the event is not signalled", svc_wait_synchronization(&index, &ev, 1, 0),
		 R_TIMED_OUT);
	check_eq("ClearEvent of a clear event", svc_clear_event(ev), R_SUCCESS);
	ipc1(CMD_SIGNAL, 0, 0, 0);
	r = svc_wait_synchronization(&index, &ev, 1, 1000 * MS);
	check("the event is signalled", r == R_SUCCESS && index == 0);
	check_eq("ClearEvent", svc_clear_event(ev), R_SUCCESS);
	check_eq("the event is clear again", svc_wait_synchronization(&index, &ev, 1, 0),
		 R_TIMED_OUT);
	check_eq("WaitSynchronization(no handles) times out",
		 svc_wait_synchronization(&index, NULL, 0, 5 * MS), R_TIMED_OUT);
	check_eq("WaitSynchronization(bad handle)",
		 svc_wait_synchronization(&index, &(u32){ 0x7777 }, 1, 0), R_INVALID_HANDLE);
	check_eq("ClearEvent(not an event)", svc_clear_event(main_handle), R_INVALID_HANDLE);
	svc_close_handle(ev);

	/* MAP_MEMORY: heap (already shmem) and stack (made shared first) */
	fill((u8 *)heap_buf, 0x4000, 21);
	check_eq("MAP_MEMORY (heap)", ipc1(CMD_MAP, heap_buf, 0x4000, 21), 1);
	check("... the service wrote through its mapping", has((u8 *)heap_buf, 0x4000, 22));
	fill(stack_buf, 0x2000, 31);
	check_eq("MAP_MEMORY (stack)", ipc1(CMD_MAP, (u64)stack_buf, 0x2000, 31), 1);
	check("... the service wrote through its mapping", has(stack_buf, 0x2000, 32));
	fill(stack_buf, 0x2000, 41);
	check_eq("... and the stack still works", ipc1(CMD_READ, (u64)stack_buf, 0x2000, 0),
		 sum(stack_buf, 0x2000));

	/* MEMWATCH */
	fill((u8 *)mw, 4 * PAGE, 1);
	args[0] = mw;
	args[1] = 4 * PAGE;
	args[2] = 1;
	r = ipc(session, CMD_MEMWATCH, 3, args, res, 2);
	check_eq("MEMWATCH_GET_CLEAR reports the written pages", r ? ~0ull : res[0], 4);
	r = ipc(session, CMD_MEMWATCH, 3, args, res, 2);
	check_eq("... and nothing once cleared", r ? ~0ull : res[0], 0);
	((volatile u8 *)mw)[2 * PAGE + 9] = 1;
	args[2] = 0;
	r = ipc(session, CMD_MEMWATCH, 3, args, res, 2);
	check("MEMWATCH_GET reports a write", !r && res[0] == 1 && res[1] == 2 * PAGE);
	args[2] = 1;
	r = ipc(session, CMD_MEMWATCH, 3, args, res, 2);
	check("... and still does", !r && res[0] == 1 && res[1] == 2 * PAGE);
	(void)((volatile u8 *)mw)[PAGE + 9];
	args[2] = 0;
	r = ipc(session, CMD_MEMWATCH, 3, args, res, 2);
	check_eq("reads are not writes", r ? ~0ull : res[0], 0);

	/* Sessions */
	s2 = ipc1(CMD_SESSION, 0x77, 0, 0);
	check("CREATE_SESSION_HANDLE", s2 && s2 != ~0u);
	r = ipc(s2, CMD_IDS, 0, NULL, res, 3);
	check_eq("a request on the new session", r ? 0 : res[2], 0x77);
	check_eq("CloseHandle(session)", svc_close_handle(s2), R_SUCCESS);
	svc_sleep_thread(20 * MS);
	check_eq("the service learns that the session was closed",
		 ipc1(CMD_CLOSED, 0x77, 0, 0), 1);
	s2 = ipc1(CMD_STUB_SESSION, 0, 0, 0);
	b[0] = 4;
	b[2] = 1234;
	r = svc_send_sync_request(s2);
	check("a session without a service gets a stub answer",
	      r == R_SUCCESS && b[4] == 0x4f434653 && b[6] == 0);
	svc_close_handle(s2);
	b[0] = 2;
	check_eq("a Close command", svc_send_sync_request(session), R_HIPC_REMOTE_DEAD);
	check_eq("SendSyncRequest(not a session)", svc_send_sync_request(main_handle),
		 R_INVALID_HANDLE);

	/* Transfer memory: the owner keeps the memory, the service shares it */
	fill((u8 *)heap + 0x80000, 0x10000, 7);
	r = svc_create_transfer_memory(&h, heap + 0x80000, 0x10000, 3);
	check_eq("CreateTransferMemory", r, R_SUCCESS);
	check("... the memory keeps its contents", has((u8 *)heap + 0x80000, 0x10000, 7));
	check_eq("the service maps the transfer memory", ipc1(CMD_TMEM, h, 0x10000, 7), 1);
	check("... and shares it with the owner", has((u8 *)heap + 0x80000, 0x10000, 8));
	svc_close_handle(h);
	((volatile u8 *)heap)[0x80000] = 5;
	check_eq("the memory stays usable after the handle is closed",
		 ((volatile u8 *)heap)[0x80000], 5);
}

/* ---- modes for the exit tests ---------------------------------------------- */

/* Requests in a buffer of the program instead of the TLS. */
static u32 ipc_buf[PAGE / 4] __attribute__((aligned(PAGE)));

static void put_echo(u32 *b, u64 v)
{
	b[0] = 4;
	b[1] = 4;
	b[2] = CMD_ECHO;
	b[3] = 0;
	b[4] = v;
	b[5] = v >> 32;
}

static void test_user_buffer_ipc(void)
{
	s32 index;
	u32 ev, r;

	if (!session)
		return;
	check_eq("SendSyncRequestWithUserBuffer(unaligned)",
		 svc_send_sync_request_with_user_buffer(&ipc_buf[1], PAGE, session),
		 R_INVALID_ADDRESS);
	check_eq("SendSyncRequestWithUserBuffer(size 0)",
		 svc_send_sync_request_with_user_buffer(ipc_buf, 0, session), R_INVALID_SIZE);
	put_echo(ipc_buf, 41);
	check_eq("SendSyncRequestWithUserBuffer",
		 svc_send_sync_request_with_user_buffer(ipc_buf, PAGE, session), R_SUCCESS);
	check("... the answer is in the buffer", ipc_buf[2] == 0 && ipc_buf[4] == 42);

	put_echo(ipc_buf, 99);
	r = svc_send_async_request_with_user_buffer(&ev, ipc_buf, PAGE, session);
	check_eq("SendAsyncRequestWithUserBuffer", r, R_SUCCESS);
	if (r != R_SUCCESS)
		return;
	check("... its event is signalled",
	      svc_wait_synchronization(&index, &ev, 1, 0) == R_SUCCESS && index == 0);
	check("... and the answer is in the buffer", ipc_buf[2] == 0 && ipc_buf[4] == 100);
	check_eq("ClearEvent on it", svc_clear_event(ev), R_SUCCESS);
	check_eq("... clears it", svc_wait_synchronization(&index, &ev, 1, 0), R_TIMED_OUT);
	check_eq("SignalEvent", svc_signal_event(ev), R_SUCCESS);
	check("... signals it",
	      svc_wait_synchronization(&index, &ev, 1, 0) == R_SUCCESS && index == 0);
	check_eq("SignalEvent on a session", svc_signal_event(session), R_INVALID_HANDLE);
	check_eq("SignalEvent(invalid handle)", svc_signal_event(0), R_INVALID_HANDLE);
	svc_close_handle(ev);
}

static u32 never;
static u32 held_mtx;

static void wait_forever_fn(u64 arg)
{
	svc_wait_for_address(&never, WAIT_IF_EQUAL, 0, -1);
	svc_exit_thread();
}

static void lock_fn(u64 arg)
{
	mutex_lock(&held_mtx, handles[arg]);
	svc_exit_thread();
}

static void sleep_fn(u64 arg)
{
	svc_sleep_thread(60000 * MS);
	svc_exit_thread();
}

static u32 blocked_event;

static void event_fn(u64 arg)
{
	s32 index;

	svc_wait_synchronization(&index, &blocked_event, 1, -1);
	svc_exit_thread();
}

static void cv_fn(u64 arg)
{
	u32 self = handles[arg];

	mutex_lock(&cv_mtx, self);
	condvar_wait(&cv, &cv_mtx, self, -1);
	mutex_unlock(&cv_mtx, self);
	svc_exit_thread();
}

/* Threads that wait in every way there is, and one that never starts. */
static void make_waiters(bool with_ipc)
{
	u32 h;

	svc_create_thread(&h, nop_fn, 0, STACK_TOP(0), HZN_TEST_PRIORITY, -2);	/* unstarted */
	svc_create_thread(&handles[1], wait_forever_fn, 1, STACK_TOP(1), HZN_TEST_PRIORITY, -2);
	mutex_lock(&held_mtx, main_handle);
	svc_create_thread(&handles[2], lock_fn, 2, STACK_TOP(2), HZN_TEST_PRIORITY, -2);
	svc_create_thread(&handles[3], sleep_fn, 3, STACK_TOP(3), HZN_TEST_PRIORITY, -2);
	svc_create_thread(&handles[4], cv_fn, 4, STACK_TOP(4), HZN_TEST_PRIORITY, -2);
	if (with_ipc && !svc_connect_to_named_port(&session, HZN_TEST_PORT)) {
		blocked_event = ipc1(CMD_EVENT, 0, 0, 0);
		svc_create_thread(&handles[5], event_fn, 5, STACK_TOP(5), HZN_TEST_PRIORITY, -2);
	}
	for (h = 1; h <= 5; h++)
		if (handles[h])
			svc_start_thread(handles[h]);
	svc_sleep_thread(50 * MS);
}

/* argv strings start at the first non-zero byte above the initial SP. */
static const char *next_arg(const char *s)
{
	while (*s)
		s++;
	return s + 1;
}

/* Synchronization and thread-lifetime regressions. */
static volatile u32 rev_word, rev_ready, rev_stop, rev_low_ready, rev_high_done;
static u32 rev_mtx, rev_h[3];
static void rev_arb_fn(u64 arg)
{
	__atomic_add_fetch(&rev_ready, 1, __ATOMIC_RELEASE);
	svc_wait_for_address(&rev_word, WAIT_IF_EQUAL, 7, 5000 * MS);
	svc_exit_thread();
}
static void rev_arb_case(int n, s32 count, u32 expected)
{
	u32 hs[3];
	int i;

	rev_word = 7;
	rev_ready = 0;
	for (i = 0; i < n; i++) {
		if (!check_eq("arb create",
			      svc_create_thread(&hs[i], rev_arb_fn, 0,
						STACK_TOP(i + 1), 44, 0),
			      R_SUCCESS))
			return;
		svc_start_thread(hs[i]);
	}
	for (i = 0; i < 100 && rev_ready != (u32)n; i++)
		svc_sleep_thread(MS);
	svc_sleep_thread(20 * MS);
	check_eq("arb waiters entered", rev_ready, n);
	check_eq("arb signal",
		 svc_signal_to_address(
			 &rev_word, SIGNAL_AND_MODIFY_BY_WAITING_COUNT_IF_EQUAL,
			 7, count),
		 R_SUCCESS);
	out("# arb n=");
	out_dec(n);
	out(" count=");
	if (count < 0)
		out("all");
	else
		out_dec(count);
	out("\n");
	check_eq("arb reference value", rev_word, expected);
	svc_signal_to_address(&rev_word, SIGNAL, 0, -1);
	for (i = 0; i < n; i++) {
		check("arb cleanup", join(hs[i], 5000 * MS));
		svc_close_handle(hs[i]);
	}
}
static void rev_low_fn(u64 arg)
{
	mutex_lock(&rev_mtx, rev_h[0]);
	rev_low_ready = 1;
	svc_sleep_thread(100 * MS);
	mutex_unlock(&rev_mtx, rev_h[0]);
	svc_exit_thread();
}
static void rev_medium_fn(u64 arg)
{
	while (!__atomic_load_n(&rev_stop, __ATOMIC_ACQUIRE))
		asm volatile("" ::: "memory");
	svc_exit_thread();
}
static void rev_high_fn(u64 arg)
{
	mutex_lock(&rev_mtx, rev_h[2]);
	rev_high_done = 1;
	mutex_unlock(&rev_mtx, rev_h[2]);
	svc_exit_thread();
}
static void test_sync_regressions(void)
{
	int i;

	out("# synchronization regressions start\n");
	check_eq("main affinity", svc_set_thread_core_mask(CUR_THREAD, 0, 1),
		 R_SUCCESS);
	rev_arb_case(0, 1, 8);
	rev_arb_case(1, 1, 6);
	rev_arb_case(1, 2, 6);
	rev_arb_case(2, 2, 6);
	rev_arb_case(3, 1, 7);
	rev_arb_case(1, 0, 6);
	rev_arb_case(2, 1, 7);
	rev_arb_case(1, -1, 6);
	/* FIFO inheritance is only observable with permission to use RT policy. */
	if (lsys(NR_sched_getscheduler, 0, 0, 0) != 1) {
		out("# priority inheritance: SCHED_FIFO unavailable, skipped\n");
		return;
	}
	rev_stop = 0;
	rev_low_ready = 0;
	rev_high_done = 0;
	rev_mtx = 0;
	check_eq("main priority", svc_set_thread_priority(CUR_THREAD, 10),
		 R_SUCCESS);
	check_eq("low create",
		 svc_create_thread(&rev_h[0], rev_low_fn, 0, STACK_TOP(1), 50,
				   0),
		 R_SUCCESS);
	check_eq("medium create",
		 svc_create_thread(&rev_h[1], rev_medium_fn, 0, STACK_TOP(2),
				   30, 0),
		 R_SUCCESS);
	check_eq("high create",
		 svc_create_thread(&rev_h[2], rev_high_fn, 0, STACK_TOP(3), 20,
				   0),
		 R_SUCCESS);
	svc_start_thread(rev_h[0]);
	for (i = 0; i < 100 && !rev_low_ready; i++)
		svc_sleep_thread(MS);
	check_eq("low owns mutex", rev_low_ready, 1);
	svc_start_thread(rev_h[1]);
	svc_start_thread(rev_h[2]);
	svc_sleep_thread(300 * MS);
	check_eq("priority inheritance lets owner run", rev_high_done, 1);
	__atomic_store_n(&rev_stop, 1, __ATOMIC_RELEASE);
	for (i = 0; i < 3; i++) {
		check("PI cleanup", join(rev_h[i], 5000 * MS));
		svc_close_handle(rev_h[i]);
	}
	check_eq("restore main priority",
		 svc_set_thread_priority(CUR_THREAD, HZN_TEST_PRIORITY),
		 R_SUCCESS);
	out("# synchronization regressions end\n");
}

static u32 chain_mtx[2], chain_h[4], chain_gate;
static volatile u32 chain_low_ready, chain_mid_ready, chain_done, chain_stop;
static u32 chain_low_final, chain_mid_final;

static void chain_low_fn(u64 arg)
{
	mutex_lock(&chain_mtx[1], chain_h[0]);
	chain_low_ready = 1;
	svc_wait_for_address(&chain_gate, WAIT_IF_EQUAL, 0, 5000 * MS);
	mutex_unlock(&chain_mtx[1], chain_h[0]);
	svc_get_thread_priority(&chain_low_final, CUR_THREAD);
	svc_exit_thread();
}

static void chain_mid_fn(u64 arg)
{
	mutex_lock(&chain_mtx[0], chain_h[1]);
	chain_mid_ready = 1;
	mutex_lock(&chain_mtx[1], chain_h[1]);
	mutex_unlock(&chain_mtx[1], chain_h[1]);
	mutex_unlock(&chain_mtx[0], chain_h[1]);
	svc_get_thread_priority(&chain_mid_final, CUR_THREAD);
	svc_exit_thread();
}

static void chain_high_fn(u64 arg)
{
	mutex_lock(&chain_mtx[0], chain_h[2]);
	chain_done = 1;
	mutex_unlock(&chain_mtx[0], chain_h[2]);
	svc_exit_thread();
}

static void chain_hog_fn(u64 arg)
{
	while (!__atomic_load_n(&chain_stop, __ATOMIC_ACQUIRE))
		asm volatile("" ::: "memory");
	svc_exit_thread();
}

static void test_priority_chain(void)
{
	u32 priority, r;
	int i;

	if (lsys(NR_sched_getscheduler, 0, 0, 0) != 1)
		return;
	svc_set_thread_priority(CUR_THREAD, 10);
	r = svc_create_thread(&chain_h[0], chain_low_fn, 0, STACK_TOP(1), 50,
			      0);
	r |= svc_create_thread(&chain_h[1], chain_mid_fn, 0, STACK_TOP(2), 40,
			       0);
	r |= svc_create_thread(&chain_h[2], chain_high_fn, 0, STACK_TOP(3), 20,
			       0);
	r |= svc_create_thread(&chain_h[3], chain_hog_fn, 0, STACK_TOP(4), 30,
			       0);
	if (!check_eq("create PI chain", r, R_SUCCESS))
		return;
	svc_start_thread(chain_h[0]);
	for (i = 0; i < 100 && !chain_low_ready; i++)
		svc_sleep_thread(MS);
	svc_start_thread(chain_h[1]);
	for (i = 0; i < 100 && !chain_mid_ready; i++)
		svc_sleep_thread(MS);
	svc_start_thread(chain_h[2]);
	svc_sleep_thread(20 * MS);
	svc_get_thread_priority(&priority, chain_h[0]);
	check_eq("priority inherited through two owners", priority, 20);
	svc_set_thread_priority(chain_h[2], 25);
	svc_get_thread_priority(&priority, chain_h[0]);
	check_eq("waiter priority change propagates", priority, 25);
	svc_set_thread_priority(chain_h[0], 55);
	svc_get_thread_priority(&priority, chain_h[0]);
	check_eq("owner base change preserves donation", priority, 25);
	svc_start_thread(chain_h[3]);
	chain_gate = 1;
	svc_signal_to_address(&chain_gate, SIGNAL, 0, -1);
	svc_sleep_thread(200 * MS);
	check_eq("PI chain progresses ahead of medium spinner", chain_done, 1);
	__atomic_store_n(&chain_stop, 1, __ATOMIC_RELEASE);
	for (i = 0; i < 4; i++) {
		check("PI chain cleanup", join(chain_h[i], 5000 * MS));
		svc_close_handle(chain_h[i]);
	}
	check_eq("low owner restored to changed base", chain_low_final, 55);
	check_eq("middle owner restored to base", chain_mid_final, 40);
	svc_set_thread_priority(CUR_THREAD, HZN_TEST_PRIORITY);
}

/* A CV waiter can still own a different mutex needed by another waiter. */
static u32 cv_pi_mtx[3], cv_pi_h[3], cv_pi_key, cv_pi_ready, cv_pi_done;
static u32 cv_pi_result[3];

static void cv_pi_fn(u64 arg)
{
	u32 self = cv_pi_h[arg];

	if (arg == 2)
		mutex_lock(&cv_pi_mtx[0], self);
	mutex_lock(&cv_pi_mtx[arg], self);
	__atomic_fetch_or(&cv_pi_ready, 1u << arg, __ATOMIC_RELEASE);
	cv_pi_result[arg] = condvar_wait(&cv_pi_key, &cv_pi_mtx[arg], self, -1);
	mutex_unlock(&cv_pi_mtx[arg], self);
	if (arg == 2)
		mutex_unlock(&cv_pi_mtx[0], self);
	__atomic_fetch_or(&cv_pi_done, 1u << arg, __ATOMIC_RELEASE);
	svc_exit_thread();
}

static void test_cv_priority_reorder(void)
{
	u32 r = 0;
	int i;

	svc_set_thread_priority(CUR_THREAD, 10);
	for (i = 0; i < 3; i++)
		r |= svc_create_thread(&cv_pi_h[i], cv_pi_fn, i, STACK_TOP(i + 1),
				       20 + 10 * i, 0);
	if (!check_eq("create CV priority reorder threads", r, R_SUCCESS))
		return;
	for (i = 0; i < 3; i++) {
		svc_start_thread(cv_pi_h[i]);
		svc_sleep_thread(20 * MS);
	}
	check_eq("all CV priority reorder waiters ready",
		 __atomic_load_n(&cv_pi_ready, __ATOMIC_ACQUIRE), 7);
	svc_signal_process_wide_key(&cv_pi_key, 3);
	svc_sleep_thread(50 * MS);
	check_eq("CV signal retains waiters reordered by inheritance",
		 __atomic_load_n(&cv_pi_done, __ATOMIC_ACQUIRE), 7);
	/* Rescue any missed waiter so failures do not hang the test suite. */
	svc_signal_process_wide_key(&cv_pi_key, -1);
	for (i = 0; i < 3; i++) {
		check("CV priority reorder cleanup", join(cv_pi_h[i], 5000 * MS));
		check_eq("CV priority reorder wait succeeds", cv_pi_result[i], R_SUCCESS);
		svc_close_handle(cv_pi_h[i]);
	}
	svc_set_thread_priority(CUR_THREAD, HZN_TEST_PRIORITY);
}

static void test_context_races(void)
{
	static struct thread_context ctx;
	u32 h, r;
	u64 tid_before, tid_after;
	int i, bad = 0;

	if (!other_core)
		return;
	spin_go = 1;
	spin_count = 0;
	if (svc_create_thread(&h, spin_fn, 0, STACK_TOP(1), HZN_TEST_PRIORITY,
			      other_core))
		return;
	svc_start_thread(h);
	for (i = 0; i < 1000 && !spin_count; i++)
		svc_sleep_thread(MS);
	for (i = 0; i < 128; i++) {
		if (svc_set_thread_activity(h, 1) ||
		    svc_get_thread_context3(&ctx, h) || ctx.r[19] != 0x1919 ||
		    ctx.v[8][0] != 0x1919 || ctx.tpidr != 0x2020)
			bad++;
		if (svc_set_thread_activity(h, 0))
			bad++;
	}
	check_eq("immediate pause/context/resume stress", bad, 0);
	spin_go = 0;
	check("context spinner exits", join(h, 5000 * MS));
	svc_close_handle(h);
	bad = 0;
	for (i = 0; i < 128; i++) {
		if (svc_create_thread(&h, nop_fn, 0, STACK_TOP(1),
				      HZN_TEST_PRIORITY, other_core)) {
			bad++;
			break;
		}
		svc_get_thread_id(&tid_before, h);
		svc_start_thread(h);
		r = svc_set_thread_activity(h, 1);
		if (r == R_SUCCESS) {
			if (svc_get_thread_context3(&ctx, h))
				bad++;
			r = svc_set_thread_activity(h, 0);
			if (r != R_SUCCESS && r != R_INVALID_STATE)
				bad++;
		} else if (r != R_INVALID_STATE)
			bad++;
		if (!join(h, 5000 * MS))
			bad++;
		if (svc_get_thread_id(&tid_after, h) || tid_before != tid_after)
			bad++;
		if (svc_set_thread_activity(h, 1) != R_INVALID_STATE)
			bad++;
		svc_close_handle(h);
	}
	check_eq("pause/context racing with thread exit", bad, 0);
}

#include "p3_guest.h"

static volatile u32 debug_worker_result;

static void debug_worker_fn(u64 breaks)
{
	bool attached = info(INFO_DEBUGGER_PRESENCE, 0, 0) == 1;

	if (breaks) {
		if (svc_break(0x80000004u, 0x1234, 0x5678) || svc_break(0, 0, 0))
			attached = false;
	}
	debug_worker_result = attached;
	svc_exit_thread();
}

int guest_main(u32 handle, const char *above_sp);

int guest_main(u32 handle, const char *above_sp)
{
	const char *argv0 = above_sp, *mode, *arg2;
	u32 h;

	while (!*argv0)
		argv0++;
	mode = next_arg(argv0);
	arg2 = next_arg(mode);
	main_handle = handle;

	if (!seq(mode, "traced-worker-break") && !seq(mode, "gdb-worker-break") &&
	    svc_create_thread(&h, nop_fn, 0, STACK_TOP(0), HZN_TEST_PRIORITY, 1) == R_SUCCESS) {
		other_core = 1;
		svc_start_thread(h);
		join(h, 5000 * MS);
		svc_close_handle(h);
	}

	if (seq(mode, "p3-perf"))
		return p3_perf(true);
	if (seq(mode, "bridge-perf"))
		return p3_perf(false);
	if (seq(mode, "p3-cross-server"))
		return p3_cross_server();
	if (seq(mode, "p3-cross-client"))
		return p3_cross_client(arg2);
	if (seq(mode, "basic")) {
		out("# hzn_guest: basic\n");
		test_basics();
		test_heap();
		test_map_memory();
		test_memory_permission();
		test_exceptions();
		test_threads();
		test_core_mask();
		test_info();
		test_thread_svcs();
		test_sync();
		test_sync_regressions();
		test_priority_chain();
		test_cv_priority_reorder();
		test_context_races();
		test_ipc();
		test_p3_memory_events();
		test_p3_ipc();
		test_user_buffer_ipc();
		if (session)
			ipc1(CMD_BYE, 0, 0, 0);
		out("# hzn_guest: ");
		out_dec(tests);
		out(" checks, ");
		out_dec(failures);
		out(" failed\n");
		return failures;
	}
	if (seq(mode, "coremask")) {
		/* hzn_test let it run on cores 0 and 1 only. */
		out("# hzn_guest: coremask\n");
		check_eq("the core mask is the CPU affinity", info(INFO_CORE_MASK, CUR_PROCESS, 0), 3);
		check_eq("CreateThread(core 2)", svc_create_thread(&h, nop_fn, 0, STACK_TOP(0),
								   HZN_TEST_PRIORITY, 2),
			 R_INVALID_CORE_ID);
		check_eq("SetThreadCoreMask(core 2)", svc_set_thread_core_mask(CUR_THREAD, 0, 7),
			 R_INVALID_CORE_ID);
		check_eq("SetThreadCoreMask(core 1)", svc_set_thread_core_mask(CUR_THREAD, 1, 2),
			 R_SUCCESS);
		svc_sleep_thread(-1);
		check_eq("... moves the thread", svc_get_current_processor_number(), 1);
		return failures;
	}
	if (seq(mode, "exit-unstarted")) {
		svc_create_thread(&h, nop_fn, 0, STACK_TOP(0), HZN_TEST_PRIORITY, -2);
		svc_exit_process();
	}
	if (seq(mode, "exitthread-unstarted")) {
		svc_create_thread(&h, nop_fn, 0, STACK_TOP(0), HZN_TEST_PRIORITY, -2);
		svc_exit_thread();
	}
	if (seq(mode, "exception-unhandled")) {
		/* The handler gives it back: the fault kills the program. */
		exc_result = R_NOT_HANDLED;
		(void)*(volatile u32 *)0x10;
		return 4;
	}
	if (seq(mode, "traced-worker-info") || seq(mode, "traced-worker-break") ||
	    seq(mode, "gdb-worker-break")) {
		u64 breaks = !seq(mode, "traced-worker-info");

		if (svc_create_thread(&h, debug_worker_fn, breaks, STACK_TOP(0),
				      HZN_TEST_PRIORITY, -2) || svc_start_thread(h) ||
		    !join(h, 10000 * MS))
			return 4;
		svc_close_handle(h);
		return debug_worker_result ? 5 : 3;
	}
	if (seq(mode, "traced-break")) {
		/* hzn_test traces it, as a debugger does. */
		if (info(INFO_DEBUGGER_PRESENCE, 0, 0) != 1)
			return 3;
		if (svc_break(0, 0x1234, 0x5678) != R_SUCCESS)
			return 4;
		return 5;	/* the debugger let it go on */
	}
	if (seq(mode, "break")) {
		svc_break(0x80000004u, 0, 0);
		svc_break(0, 0, 0);
		return 2;
	}
	if (seq(mode, "hang")) {
		make_waiters(true);
		ipc1(CMD_READY, 0, 0, 0);
		ipc1(CMD_HANG, 0, 0, 0);
		return 3;
	}
	if (seq(mode, "freeze")) {
		make_waiters(true);
		ipc1(CMD_READY, 0, 0, 0);
		svc_sleep_thread(5000 * MS);
		svc_exit_process();
	}
	if (seq(mode, "exec")) {
		const char *av[2] = { arg2, NULL }, *ev[1] = { NULL };

		make_waiters(false);
		lsys(NR_execve, (long)arg2, (long)av, (long)ev);
		return 77;
	}
	out("# hzn_guest: unknown mode\n");
	return 100;
}

/*
 * Entry: x1 is the main thread handle, SP is 16-byte aligned (the first
 * store through it checks that) and the argv strings are above it.
 */
asm(".pushsection .text.start, \"ax\"\n"
    ".global _start\n"
    "_start:\n"
    "	cbnz	x0, 1f\n"
    "	stp	x29, x30, [sp, #-16]!\n"
    "	mov	x29, sp\n"
    "	mov	w0, w1\n"
    "	add	x1, sp, #16\n"
    "	bl	guest_main\n"
    "	mov	x8, #94\n"
    "	svc	#0\n"
    "	b	.\n"
    /* A user exception: x0 is its type, x1 its info, SP in the PLR. */
    "1:	sub	sp, sp, #0xb0\n"
    "	stp	x9, x10, [sp, #0x00]\n"
    "	stp	x11, x12, [sp, #0x10]\n"
    "	stp	x13, x14, [sp, #0x20]\n"
    "	stp	x15, x16, [sp, #0x30]\n"
    "	stp	x17, x18, [sp, #0x40]\n"
    "	stp	x19, x20, [sp, #0x50]\n"
    "	stp	x21, x22, [sp, #0x60]\n"
    "	stp	x23, x24, [sp, #0x70]\n"
    "	stp	x25, x26, [sp, #0x80]\n"
    "	stp	x27, x28, [sp, #0x90]\n"
    "	str	x29, [sp, #0xa0]\n"
    "	mov	x19, sp\n"
    "	adrp	x9, exc_stack\n"
    "	add	x9, x9, :lo12:exc_stack\n"
    "	add	sp, x9, #0x2000\n"
    "	bl	guest_exception\n"
    "	mov	sp, x19\n"
    "	ldp	x9, x10, [sp, #0x00]\n"
    "	ldp	x11, x12, [sp, #0x10]\n"
    "	ldp	x13, x14, [sp, #0x20]\n"
    "	ldp	x15, x16, [sp, #0x30]\n"
    "	ldp	x17, x18, [sp, #0x40]\n"
    "	ldp	x19, x20, [sp, #0x50]\n"
    "	ldp	x21, x22, [sp, #0x60]\n"
    "	ldp	x23, x24, [sp, #0x70]\n"
    "	ldp	x25, x26, [sp, #0x80]\n"
    "	ldp	x27, x28, [sp, #0x90]\n"
    "	ldr	x29, [sp, #0xa0]\n"
    "	svc	#0x28\n"
    "	b	.\n"
    ".popsection\n");
