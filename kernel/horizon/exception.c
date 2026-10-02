// SPDX-License-Identifier: GPL-2.0
/*
 * Horizon user exceptions.
 *
 * When a thread of a Horizon program faults, Horizon enters the program
 * again at its entry point, with x0 the exception type and x1 the exception
 * info in the process local region, on a small stack in that region. rtld's
 * _start passes that to nnSdk's nn::os::detail::UserExceptionHandler, which
 * runs the handlers the program registered and ends with
 * svcReturnFromException: on success the thread resumes with the context in
 * the exception info, which the handler may have changed; otherwise the
 * fault takes its course. One thread of a process is in the handler at a
 * time, others that fault meanwhile wait for it.
 *
 * Faults come here from arm64_force_sig_fault(), before they become
 * signals. Breakpoints a debugger set, hardware breakpoints, watchpoints and
 * single steps stay signals for the debugger.
 */

#include <linux/horizon.h>
#include <linux/ptrace.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <asm/esr.h>

#include "internal.h"

/* svc::aarch64::ExceptionInfo, in the process local region. */
struct hzn_exception_info {
	u64 r[9];
	u64 lr, sp, pc;
	u32 pstate, afsr0, afsr1, esr;
	u64 far;
};
static_assert(sizeof(struct hzn_exception_info) == 0x78);

/* Where it is in the process local region; the stack is below it. */
#define HZN_PLR_EXCEPTION_INFO	0x148
#define HZN_PLR_EXCEPTION_STACK	ALIGN_DOWN(HZN_PLR_EXCEPTION_INFO, 16)

/* The PSTATE bits a program sees and may change: NZCV. */
#define HZN_PSR_USER_MASK	0xf0000000UL

/* svc::ExceptionType */
#define HZN_EXCEPTION_INSTRUCTION_ABORT		0x100
#define HZN_EXCEPTION_DATA_ABORT		0x101
#define HZN_EXCEPTION_UNALIGNED_INSTRUCTION	0x102
#define HZN_EXCEPTION_UNALIGNED_DATA		0x103

/* Undefined and breakpoint instructions are instruction aborts, as on Horizon. */
static u32 hzn_exception_type(int signo, int code, unsigned long esr)
{
	switch (signo) {
	case SIGSEGV:
		return ESR_ELx_EC(esr) == ESR_ELx_EC_IABT_LOW ?
		       HZN_EXCEPTION_INSTRUCTION_ABORT : HZN_EXCEPTION_DATA_ABORT;
	case SIGBUS:
		if (code != BUS_ADRALN)
			return HZN_EXCEPTION_DATA_ABORT;
		return ESR_ELx_EC(esr) == ESR_ELx_EC_PC_ALIGN ?
		       HZN_EXCEPTION_UNALIGNED_INSTRUCTION :
		       HZN_EXCEPTION_UNALIGNED_DATA;
	default:	/* SIGILL, SIGTRAP */
		return HZN_EXCEPTION_INSTRUCTION_ABORT;
	}
}

/* The ESR of the fault: the paths to arm64_force_sig_fault() leave it here. */
static unsigned long hzn_exception_esr(int signo)
{
	unsigned long esr = current->thread.fault_code;

	/* A brk instruction does not set it. */
	if (signo == SIGTRAP)
		return ESR_ELx_EC_BRK64 << ESR_ELx_EC_SHIFT | ESR_ELx_IL;
	/* Undefined instructions leave it 0: EC 0 is "unknown reason". */
	if (signo == SIGILL && !esr)
		return ESR_ELx_IL;
	return esr;
}

static bool hzn_claim_exception(struct hzn_process *proc,
				struct hzn_thread *thread)
{
	return !cmpxchg(&proc->exception_thread, NULL, thread);
}

/* Ends @thread's turn in the handler, if it has it. */
void hzn_release_exception(struct hzn_thread *thread)
{
	struct hzn_process *proc = thread->proc;

	if (cmpxchg(&proc->exception_thread, thread, NULL) == thread)
		wake_up(&proc->exception_wq);
}

/*
 * Called by arm64_force_sig_fault() for a fault of the current task, before
 * it becomes the signal @signo. Returns true if the program takes it.
 */
bool horizon_user_exception(int signo, int code, unsigned long far)
{
	struct hzn_thread *thread = hzn_current();
	struct pt_regs *regs = current_pt_regs();
	struct hzn_exception_info info;
	struct hzn_process *proc;
	unsigned long esr, addr;

	if (!thread || !test_thread_flag(TIF_HORIZON) || !user_mode(regs) ||
	    compat_user_mode(regs) || irqs_disabled())
		return false;
	switch (signo) {
	case SIGSEGV:
	case SIGBUS:
	case SIGILL:
		break;
	case SIGTRAP:
		/* Only brk instructions, and not while a debugger is attached. */
		if (code != TRAP_BRKPT || current->ptrace)
			return false;
		break;
	default:
		return false;
	}
	proc = thread->proc;
	/* A fault in the handler itself is not given to it again. */
	if (!proc->plr || READ_ONCE(proc->exception_thread) == thread)
		return false;
	while (wait_event_state(proc->exception_wq,
			       hzn_claim_exception(proc, thread), HZN_WAIT_STATE)) {
		if (hzn_check_signals())
			return false;
	}

	esr = hzn_exception_esr(signo);
	memcpy(info.r, regs->regs, sizeof(info.r));
	info.lr = regs->regs[30];
	info.sp = regs->sp;
	info.pc = regs->pc;
	info.pstate = regs->pstate & HZN_PSR_USER_MASK;
	info.afsr0 = 0;
	info.afsr1 = 0;
	info.esr = esr;
	info.far = far;
	addr = proc->plr + HZN_PLR_EXCEPTION_INFO;
	if (copy_to_user((void __user *)addr, &info, sizeof(info))) {
		hzn_release_exception(thread);
		return false;
	}
	thread->exception_signo = signo;
	thread->exception_code = code;
	thread->exception_far = far;

	regs->regs[0] = hzn_exception_type(signo, code, esr);
	regs->regs[1] = addr;
	regs->sp = proc->plr + HZN_PLR_EXCEPTION_STACK;
	regs->pc = HZN_IMAGE_BASE;	/* the entry point */
	regs->pstate &= ~(HZN_PSR_USER_MASK | PSR_BTYPE_MASK);
	return true;
}

/*
 * svcReturnFromException. The returned value goes to x0, so it is the x0 of
 * the restored context.
 */
long hzn_return_from_exception(u32 result)
{
	struct hzn_thread *thread = hzn_current();
	struct pt_regs *regs = current_pt_regs();
	struct hzn_process *proc = thread->proc;
	struct hzn_exception_info info;
	bool restored;

	if (READ_ONCE(proc->exception_thread) != thread)
		return HZN_RESULT_INVALID_STATE;

	restored = !copy_from_user(&info,
				   (const void __user *)(proc->plr + HZN_PLR_EXCEPTION_INFO),
				   sizeof(info));
	if (restored) {
		memcpy(regs->regs, info.r, sizeof(info.r));
		regs->regs[30] = info.lr;
		regs->sp = info.sp;
		regs->pc = info.pc;
		regs->pstate = (info.pstate & HZN_PSR_USER_MASK) |
			       (regs->pstate & ~HZN_PSR_USER_MASK);
		/* x0 is no system call result to restart with. */
		forget_syscall(regs);
	}
	hzn_release_exception(thread);

	/* Not handled: the fault becomes its signal after all. */
	if (result != HZN_RESULT_SUCCESS || !restored)
		force_sig_fault(thread->exception_signo, thread->exception_code,
				(void __user *)thread->exception_far);
	return regs->regs[0];
}
