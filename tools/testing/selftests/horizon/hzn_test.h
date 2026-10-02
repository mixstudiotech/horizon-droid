/* SPDX-License-Identifier: GPL-2.0 */
/*
 * What hzn_test (the launcher and service) and hzn_guest (the Horizon
 * program) agree on. Plain definitions only: the guest has no libc.
 */
#ifndef HZN_TEST_H
#define HZN_TEST_H

#define HZN_TEST_PORT		"hzntest"
#define HZN_TEST_TITLE_ID	0x0100000000c0ffeeULL
#define HZN_TEST_PRIORITY	44
#define HZN_TEST_SYSTEM_RESOURCE_SIZE	0x100000

/*
 * Requests on the "hzntest" port: word 0 is the command type (4, a
 * request), word 2 the command, words 4.. the arguments (u64 each). The
 * service answers with the status (0: done) in word 2 and the results
 * (u64 each) from word 4.
 */
enum {
	CMD_ECHO = 1,		/* v -> v + 1 */
	CMD_IDS,		/* -> process ID, title ID, session ID */
	CMD_READ,		/* addr, len -> sum of the bytes (READ_BUFFER) */
	CMD_WRITE,		/* addr, len, seed: pattern (WRITE_BUFFER) */
	CMD_EVENT,		/* -> handle of a new event (an eventfd) */
	CMD_SIGNAL,		/* signal that event */
	CMD_MAP,		/* addr, len, seed -> 1 if MAP_MEMORY saw the
				 * pattern; then writes pattern seed + 1
				 */
	CMD_MEMWATCH,		/* addr, len, clear -> count, first offset */
	CMD_SESSION,		/* id -> handle of a new session to us */
	CMD_STUB_SESSION,	/* -> handle of a session without service */
	CMD_CLOSED,		/* id -> 1 if that session was closed */
	CMD_TMEM,		/* handle, len, seed -> 1 if the transfer
				 * memory has the pattern; then writes
				 * pattern seed + 1
				 */
	CMD_HANG,		/* no answer: the guest gets killed */
	CMD_READY,		/* the guest's threads are all waiting */
	CMD_READ_FROM,		/* addr, len -> sum (READ_BUFFER_FROM pid) */
	CMD_MAP_INVALID,	/* invalid source/destination range regression */
	CMD_P3_NATIVE_START,
	CMD_SHMEM,		/* -> handle of a memfd page with 0x5a at
				 * offset 5: shared memory as a plain file,
				 * like switch-loader's
				 */
	CMD_STOP_INSTALL,	/* no answer: the guest is stopped, then
				 * killed while the service installs a handle
				 * for it
				 */
	CMD_STOP_KILL,		/* no answer: the guest is stopped with all
				 * its threads waiting, then killed
				 */
	CMD_BYE = 99,
};

#endif /* HZN_TEST_H */
