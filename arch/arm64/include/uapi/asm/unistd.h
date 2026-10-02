/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#include <asm/unistd_64.h>

/* Horizon syscalls: wired up with CONFIG_HORIZON, otherwise ENOSYS. */
#define __NR_horizon_execve	500
#define __NR_horizon_execveat	501
#define __NR_horizon_servctl	502
