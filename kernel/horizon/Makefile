# SPDX-License-Identifier: GPL-2.0

obj-y += process.o exec.o svc.o arbiter.o memory.o ipc.o memwatch.o event.o mem_objects.o cache.o native_ipc.o exception.o

# The SVC table fills every slot with the "not implemented" handler first.
CFLAGS_svc.o += $(call cc-disable-warning, override-init)
