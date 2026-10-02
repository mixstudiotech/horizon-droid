/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Horizon SVC plumbing for arm64.
 *
 * HSYSCALL_DEFINEn() is SYSCALL_DEFINEn() for Horizon SVCs: arguments come
 * from x0..x5, the return value (a Horizon result code) goes to x0,
 * HSYSCALL_OUT() writes the output register x1 and HSYSCALL_OUTN() the
 * output register xn.
 */
#ifndef __ASM_HORIZON_SYSCALL_H
#define __ASM_HORIZON_SYSCALL_H

#include <linux/linkage.h>
#include <linux/syscalls.h>
#include <asm/syscall.h>

/* Only the numbers: leave __SYSCALL as it was. */
#ifndef __SYSCALL
#define __HZN_SYSCALL_UNDEF
#endif
#include <asm/horizon/unistd.h>
#ifdef __HZN_SYSCALL_UNDEF
#undef __SYSCALL
#undef __HZN_SYSCALL_UNDEF
#endif

extern const syscall_fn_t horizon_sys_call_table[];

asmlinkage long __arm64_hsys_ni_syscall(const struct pt_regs *regs);

#define __HSYSCALL_DEFINEx(x, name, ...)						\
	asmlinkage long __arm64_hsys##name(const struct pt_regs *regs);			\
	static long __se_hsys##name(u64 *__out, __MAP(x, __SC_LONG, __VA_ARGS__));	\
	static inline long __do_hsys##name(u64 *__out, __MAP(x, __SC_DECL, __VA_ARGS__));\
	asmlinkage long __arm64_hsys##name(const struct pt_regs *regs)			\
	{										\
		return __se_hsys##name((u64 *)&regs->regs[1],				\
				       SC_ARM64_REGS_TO_ARGS(x, __VA_ARGS__));		\
	}										\
	static long __se_hsys##name(u64 *__out, __MAP(x, __SC_LONG, __VA_ARGS__))	\
	{										\
		long ret = __do_hsys##name(__out, __MAP(x, __SC_CAST, __VA_ARGS__));	\
		__MAP(x, __SC_TEST, __VA_ARGS__);					\
		__PROTECT(x, ret, __MAP(x, __SC_ARGS, __VA_ARGS__));			\
		return ret;								\
	}										\
	static inline long __do_hsys##name(u64 *__out, __MAP(x, __SC_DECL, __VA_ARGS__))

#define HSYSCALL_DEFINE0(sname)							\
	asmlinkage long __arm64_hsys_##sname(const struct pt_regs *regs);	\
	static inline long __do_hsys_##sname(u64 *__out);			\
	asmlinkage long __arm64_hsys_##sname(const struct pt_regs *regs)	\
	{									\
		return __do_hsys_##sname((u64 *)&regs->regs[1]);		\
	}									\
	static inline long __do_hsys_##sname(u64 *__out)

#define HSYSCALL_DEFINE1(name, ...) __HSYSCALL_DEFINEx(1, _##name, __VA_ARGS__)
#define HSYSCALL_DEFINE2(name, ...) __HSYSCALL_DEFINEx(2, _##name, __VA_ARGS__)
#define HSYSCALL_DEFINE3(name, ...) __HSYSCALL_DEFINEx(3, _##name, __VA_ARGS__)
#define HSYSCALL_DEFINE4(name, ...) __HSYSCALL_DEFINEx(4, _##name, __VA_ARGS__)
#define HSYSCALL_DEFINE5(name, ...) __HSYSCALL_DEFINEx(5, _##name, __VA_ARGS__)
#define HSYSCALL_DEFINE6(name, ...) __HSYSCALL_DEFINEx(6, _##name, __VA_ARGS__)

#define HSYSCALL_OUT(out)	(*__out = (u64)(out))
#define HSYSCALL_OUTN(n, out)	(__out[(n) - 1] = (u64)(out))

#endif /* __ASM_HORIZON_SYSCALL_H */
