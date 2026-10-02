# SPDX-License-Identifier: GPL-2.0
uname_M := $(shell uname -m 2>/dev/null || echo not)
ARCH ?= $(shell echo $(uname_M) | sed -e 's/aarch64.*/arm64/')

CFLAGS += -Wall -O2 $(KHDR_INCLUDES)

ifeq ($(ARCH),arm64)
TEST_GEN_PROGS := hzn_test
TEST_GEN_PROGS_EXTENDED := hzn_guest
TEST_FILES := gdb-smoke.gdb
endif

include ../lib.mk

$(OUTPUT)/hzn_test: hzn_test.c hzn_test.h ../kselftest.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ hzn_test.c $(LDLIBS)

# A freestanding Horizon program, linked where the Horizon loader puts it.
$(OUTPUT)/hzn_guest: guest.c guest.lds hzn_test.h p3_guest.h
	$(CC) -g -O2 -Wall -ffreestanding -fno-builtin -fno-stack-protector \
		-fno-pic -fno-pie -no-pie -mgeneral-regs-only -mno-outline-atomics \
		-fno-asynchronous-unwind-tables -fno-unwind-tables -nostdlib -static \
		-Wl,-T,guest.lds -Wl,--build-id=none -Wl,-z,noexecstack \
		-o $@ guest.c -lgcc
