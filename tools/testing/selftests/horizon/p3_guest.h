/* SPDX-License-Identifier: GPL-2.0 */
/* Included after the common guest helpers. */
static u32 p3_pair(bool port, s32 maximum, bool light, u32 *a, u32 *b)
{
	u64 r[8] = { 0, 0, port ? (u64)maximum : (u64)light, port ? light : 0, 0 };
	u32 result;

	if (port)
		result = SVCR(0x70, r);
	else
		result = SVCR(0x40, r);
	*a = r[1];
	*b = r[2];
	return result;
}
static u32 p3_event(u32 *a, u32 *b)
{
	u64 r[8] = { 0 };
	u32 result = SVCR(0x45, r);

	*a = r[1];
	*b = r[2];
	return result;
}
static u32 p3_signal(u32 h)
{
	u64 o;
	return SVC(0x11, h, 0, 0, 0, 0, 0, &o);
}
static u32 p3_reset(u32 h)
{
	u64 o;
	return SVC(0x17, h, 0, 0, 0, 0, 0, &o);
}
static u32 p3_receive(u32 *hs, s32 n, u32 target, s64 timeout, s32 *index,
		       u64 buffer)
{
	u64 r[8] = { 0 };
	u32 result;

	if (buffer) {
		r[1] = buffer; r[2] = PAGE; r[3] = (u64)hs;
		r[4] = n; r[5] = target; r[6] = timeout;
		result = SVCR(0x44, r);
	} else {
		r[1] = (u64)hs; r[2] = n; r[3] = target; r[4] = timeout;
		result = SVCR(0x43, r);
	}
	*index = r[1];
	return result;
}
static u32 p3_control(u32 h, u32 op, u64 a, u64 s, u32 p)
{
	u64 o;
	return SVC(0x4c, h, op, a, s, p, 0, &o);
}

static void test_p3_memory_events(void)
{
	u64 o, a, b, v, region = info(12, CUR_PROCESS, 0), region_size = info(13, CUR_PROCESS, 0);
	u32 w, r, h, result;
	s32 index;
	struct mem_info mi;

	out("# P3 memory/event begin\n");
	check_eq("CreateEvent", p3_event(&w, &r), R_SUCCESS);
	check("event endpoint handles differ", w && r && w != r);
	check_eq("fresh event times out", svc_wait_synchronization(&index, &r, 1, 0), R_TIMED_OUT);
	check_eq("writable endpoint cannot be waited", svc_wait_synchronization(&index, &w, 1, 0), R_INVALID_HANDLE);
	check_eq("SignalEvent rejects readable endpoint", p3_signal(r), R_INVALID_HANDLE);
	check_eq("ResetSignal rejects unsignalled event", p3_reset(r), R_INVALID_STATE);
	check_eq("SignalEvent", p3_signal(w), R_SUCCESS);
	check_eq("repeated SignalEvent", p3_signal(w), R_SUCCESS);
	check_eq("event stays signalled after wait", svc_wait_synchronization(&index, &r, 1, 0), R_SUCCESS);
	check_eq("ResetSignal consumes signalled state", p3_reset(r), R_SUCCESS);
	check_eq("repeated signal is not a counter", svc_wait_synchronization(&index, &r, 1, 0), R_TIMED_OUT);
	check_eq("ClearEvent is idempotent", svc_clear_event(w), R_SUCCESS);
	p3_signal(w);
	check_eq("ReplyAndReceive supports ordinary events", p3_receive(&r, 1, 0, 0, &index, 0), R_SUCCESS);
	check_eq("event receive index", index, 0);
	svc_close_handle(w); svc_close_handle(r);

	check_eq("CreateSharedMemory rejects unaligned size", SVC(0x50, 0, PAGE - 1, 3, 3, 0, 0, &o), R_INVALID_SIZE);
	check_eq("CreateSharedMemory rejects execute permission", SVC(0x50, 0, PAGE, 5, 3, 0, 0, &o), R_INVALID_NEW_MEMORY_PERMISSION);
	check_eq("CreateSharedMemory", SVC(0x50, 0, PAGE, 3, 1, 0, 0, &o), R_SUCCESS);
	h = o;
	a = find_free(region, region_size, 2 * PAGE);
	b = a + PAGE;
	check_eq("MapSharedMemory exact size", SVC(0x13, h, a, 2 * PAGE, 3, 0, 0, &o), R_INVALID_SIZE);
	check_eq("MapSharedMemory owner's permission", SVC(0x13, h, a, PAGE, 1, 0, 0, &o), R_INVALID_NEW_MEMORY_PERMISSION);
	check_eq("MapSharedMemory", SVC(0x13, h, a, PAGE, 3, 0, 0, &o), R_SUCCESS);
	check_eq("fresh shared memory is zero", *(volatile u64 *)a, 0);
	*(volatile u64 *)a = 0x3456;
	check_eq("second shared mapping", SVC(0x13, h, b, PAGE, 3, 0, 0, &o), R_SUCCESS);
	check_eq("shared mappings alias", *(volatile u64 *)b, 0x3456);
	svc_query_memory(&mi, a);
	check_eq("shared mapping state", mi.state, 6);
	check_eq("GetInfo34 rejects shared memory", svc_get_info(&v, 34, h, 0), R_INVALID_HANDLE);
	check_eq("unmap second shared mapping", SVC(0x14, h, b, PAGE, 0, 0, 0, &o), R_SUCCESS);
	check_eq("unmap first shared mapping", SVC(0x14, h, a, PAGE, 0, 0, 0, &o), R_SUCCESS);
	svc_close_handle(h);

	*(volatile u64 *)(heap + 0x400000) = 0x9876;
	check_eq("CreateTransferMemory with owner R", svc_create_transfer_memory(&h, heap + 0x400000, PAGE, 1), R_SUCCESS);
	check_eq("GetInfo34 original address", svc_get_info(&v, 34, h, 0), R_SUCCESS);
	check_eq("transfer memory hint value", v, heap + 0x400000);
	check_eq("transfer hint subtype", svc_get_info(&v, 34, h, 1), R_INVALID_COMBINATION);
	svc_query_memory(&mi, heap + 0x400000);
	check("transfer source is locked and read-only", mi.perm == 1 && (mi.attr & MA_LOCKED));
	check_eq("cannot reprotect loaned source", svc_set_memory_permission(heap + 0x400000, PAGE, 3), R_INVALID_CURRENT_MEMORY);
	a = find_free(region, region_size, PAGE);
	check_eq("MapTransferMemory requires owner permission", SVC(0x51, h, a, PAGE, 3, 0, 0, &o), R_INVALID_STATE);
	check_eq("MapTransferMemory", SVC(0x51, h, a, PAGE, 1, 0, 0, &o), R_SUCCESS);
	check_eq("transfer shares original pages", *(volatile u64 *)a, 0x9876);
	*(volatile u64 *)a = 0x1122;
	check_eq("transfer receiver always has RW", *(volatile u64 *)(heap + 0x400000), 0x1122);
	check_eq("transfer can only map once", SVC(0x51, h, a + PAGE, PAGE, 1, 0, 0, &o), R_INVALID_STATE);
	svc_query_memory(&mi, a);
	check_eq("transfer state", mi.state, 14);
	check_eq("SDK rollback via UnmapSharedMemory", SVC(0x14, h, a, PAGE, 0, 0, 0, &o), R_SUCCESS);
	check_eq("transfer remaps after rollback", SVC(0x51, h, a, PAGE, 1, 0, 0, &o), R_SUCCESS);
	check_eq("UnmapTransferMemory", SVC(0x52, h, a, PAGE, 0, 0, 0, &o), R_SUCCESS);
	svc_close_handle(h);
	svc_query_memory(&mi, heap + 0x400000);
	check("closing transfer restores source", mi.perm == 3 && !(mi.attr & MA_LOCKED));
	*(volatile u64 *)(heap + 0x400000) = 0;

	check_eq("CreateCodeMemory", SVC(0x4b, 0, heap + 0x500000, PAGE, 0, 0, 0, &o), R_SUCCESS);
	h = o;
	a = find_free(region, region_size, 2 * PAGE); b = a + PAGE;
	check_eq("ControlCodeMemory map writable", p3_control(h, 0, a, PAGE, 3), R_SUCCESS);
	check_eq("code memory is initialized to FF", *(volatile u32 *)a, 0xffffffffu);
	((volatile u32 *)a)[0] = 0x52800540; /* mov w0, #42 */
	((volatile u32 *)a)[1] = 0xd65f03c0; /* ret */
	check_eq("ControlCodeMemory map executable", p3_control(h, 2, b, PAGE, 5), R_SUCCESS);
	check_eq("generated code executes", ((u32 (*)(void))b)(), 42);
	svc_query_memory(&mi, b);
	check_eq("generated code state", mi.state, 20);
	check_eq("ControlCodeMemory unmap executable", p3_control(h, 3, b, PAGE, 0), R_SUCCESS);
	check_eq("ControlCodeMemory unmap writable", p3_control(h, 1, a, PAGE, 0), R_SUCCESS);
	svc_close_handle(h);
	svc_query_memory(&mi, heap + 0x500000);
	check_eq("code source is restored", mi.perm, 3);
	result = SVC(0x2a, 0, 0, 0, 0, 0, 0, &o);
	check_eq("FlushEntireDataCache", result, R_SUCCESS);
	check_eq("transfer owner None", svc_create_transfer_memory(&h, heap + 0x400000, PAGE, 0), 0);
	svc_query_memory(&mi, heap + 0x400000);
	check_eq("transfer None removes owner access", mi.perm, 0);
	svc_close_handle(h);
	svc_query_memory(&mi, heap + 0x400000);
	check_eq("transfer None restores owner access", mi.perm, 3);
	out("# P3 memory/event end\n");
}

static u32 p3_server, p3_client, p3_event_read, p3_event_write;
static volatile u32 p3_server_status;
static u8 p3_user_buffer[PAGE] __attribute__((aligned(PAGE)));

static void p3_server_thread(u64 use_user_buffer)
{
	u32 *cmd = use_user_buffer ? (u32 *)p3_user_buffer : get_tls();
	u64 buf = use_user_buffer ? (u64)cmd : 0;
	s32 index;
	u32 r;
	int i;

	for (i = 0; i < 20; i++) {
		memset(cmd, 0, 0x100);
		r = p3_receive(&p3_server, 1, 0, 2000 * MS, &index, buf);
		if (r || index != 0) {
			p3_server_status = 1;
			break;
		}
		if (cmd[0] != 4 || cmd[1] != 0x80000004 || cmd[2] != 3 ||
		    cmd[6] != i || cmd[5] != p3_event_read) {
			/* copied handles must name the same event, with a new fd */
			if (cmd[0] != 4 || cmd[1] != 0x80000004 || cmd[2] != 3 || cmd[6] != i)
				p3_server_status = 2;
		}
		if (cmd[3] != (u32)lsys(NR_getpid, 0, 0, 0))
			p3_server_status = 3;
		{
			u32 transferred = cmd[5];
			if (p3_signal(p3_event_write) || p3_reset(transferred))
				p3_server_status = 4;
			svc_close_handle(transferred);
		}
		memset(cmd, 0, 0x100);
		cmd[0] = 0; cmd[1] = 1; cmd[2] = i + 100;
		r = p3_receive(NULL, 0, p3_server, 0, &index, buf);
		if (r != R_TIMED_OUT || index != -1) {
			p3_server_status = 5;
			break;
		}
	}
	svc_exit_thread();
}

static void test_p3_ipc(void)
{
	u64 o, r[8];
	u32 port_server, port_client, thread, session2;
	u32 *cmd = get_tls();
	s32 index;
	int i, mode;

	out("# P3 native IPC begin\n");
	check_eq("CreatePort rejects zero session limit", p3_pair(true, 0, false, &port_server, &port_client), KRES(119));
	check_eq("CreatePort", p3_pair(true, 1, false, &port_server, &port_client), R_SUCCESS);
	check_eq("AcceptSession empty port", SVC(0x41, 0, port_server, 0, 0, 0, 0, &o), R_NOT_FOUND);
	check_eq("ConnectToPort rejects server port", SVC(0x72, 0, port_server, 0, 0, 0, 0, &o), R_INVALID_HANDLE);
	check_eq("ConnectToPort", SVC(0x72, 0, port_client, 0, 0, 0, 0, &o), R_SUCCESS);
	session2 = o;
	check_eq("port session limit", SVC(0x72, 0, port_client, 0, 0, 0, 0, &o), KRES(7));
	check_eq("server port signals pending accept", svc_wait_synchronization(&index, &port_server, 1, 0), R_SUCCESS);
	check_eq("AcceptSession", SVC(0x41, 0, port_server, 0, 0, 0, 0, &o), R_SUCCESS);
	p3_server = o;
	svc_close_handle(session2);
	check_eq("server observes client close", p3_receive(&p3_server, 1, 0, 0, &index, 0), KRES(123));
	svc_close_handle(p3_server);
	check_eq("session limit restored after close", SVC(0x72, 0, port_client, 0, 0, 0, 0, &o), R_SUCCESS);
	session2 = o;
	svc_close_handle(port_server);
	check_eq("port close fails unaccepted client", svc_send_sync_request(session2), KRES(123));
	check_eq("closed server port rejects new connections", SVC(0x72, 0, port_client, 0, 0, 0, 0, &o), KRES(131));
	svc_close_handle(session2); svc_close_handle(port_client);
	check_eq("ReplyAndReceive timeout", p3_receive(NULL, 0, 0, 0, &index, 0), R_TIMED_OUT);
	check_eq("ReplyAndReceive timeout output", index, -1);
	memset(r, 0, sizeof(r)); r[1] = (u64)p3_user_buffer + 1; r[2] = PAGE;
	check_eq("user receive rejects unaligned address", SVCR(0x44, r), R_INVALID_ADDRESS);

	memset(r, 0, sizeof(r)); r[1] = find_free(info(12, CUR_PROCESS, 0), info(13, CUR_PROCESS, 0), PAGE); r[2] = PAGE;
	check_eq("user receive validates an unmapped buffer before waiting", SVCR(0x44, r), R_INVALID_CURRENT_MEMORY);
	for (mode = 0; mode < 2; mode++) {
		check_eq("CreateSession", p3_pair(false, 0, false, &p3_server, &p3_client), R_SUCCESS);
		check_eq("native IPC event", p3_event(&p3_event_write, &p3_event_read), R_SUCCESS);
		memset(cmd, 0, 0x100);
		cmd[0] = (1u << 16) | (1u << 20); cmd[6] = 2;
		check_eq("invalid map attribute after a pointer descriptor", svc_send_sync_request(p3_client), R_INVALID_COMBINATION);
		memset(cmd, 0, 0x100);
		cmd[1] = 0x80000000; cmd[2] = 64; cmd[3] = cmd[4] = p3_event_write;
		check_eq("duplicate moved handles are rejected", svc_send_sync_request(p3_client), R_INVALID_HANDLE);
		check_eq("failed move leaves original handle", p3_signal(p3_event_write), 0);
		p3_reset(p3_event_read);
		p3_server_status = 0;
		check_eq("native IPC server thread", svc_create_thread(&thread, p3_server_thread,
			mode, STACK_TOP(0), HZN_TEST_PRIORITY, -2), R_SUCCESS);
		svc_start_thread(thread);
		for (i = 0; i < 20; i++) {
			memset(cmd, 0, 0x100);
			cmd[0] = 4; cmd[1] = 0x80000004; cmd[2] = 3;
			cmd[3] = 0xdeaddead; cmd[4] = 0; cmd[5] = p3_event_read; cmd[6] = i;
			check_eq("native IPC SendSyncRequest", svc_send_sync_request(p3_client), R_SUCCESS);
			check_eq("native IPC reply payload", cmd[2], i + 100);
		}
		check("native server exits", join(thread, 3000 * MS));
		check_eq("native server checks", p3_server_status, 0);
		svc_close_handle(thread); svc_close_handle(p3_server); svc_close_handle(p3_client);
		svc_close_handle(p3_event_write); svc_close_handle(p3_event_read);
	}
	out("# P3 native IPC end\n");
}

static void p3_map_descriptor(u32 *w, u64 addr, u64 size)
{
	w[0] = size; w[1] = addr;
	w[2] = (((addr >> 36) & 7) << 2) | (((addr >> 32) & 15) << 28) |
		(((size >> 32) & 15) << 24);
}
static u64 p3_map_address(u32 *w)
{
	return w[1] | ((u64)(w[2] >> 28) << 32) | ((u64)((w[2] >> 2) & 7) << 36);
}
static void p3_pointer_descriptor(u32 *w, u64 addr, u32 size)
{
	w[0] = (size << 16) | (((addr >> 36) & 7) << 6) | (((addr >> 32) & 15) << 12);
	w[1] = addr;
}
static u64 p3_pointer_address(u32 *w)
{
	return w[1] | ((u64)((w[0] >> 12) & 15) << 32) | ((u64)((w[0] >> 6) & 7) << 36);
}
static void p3_receive_list(u32 *cmd, u32 offset, u64 addr, u32 size)
{
	cmd[1] |= (3u << 10) | (offset << 20);
	cmd[offset] = addr;
	cmd[offset + 1] = (addr >> 32) | (size << 16);
}

static int p3_cross_server(void)
{
	u32 server, client, shared, moved, *cmd = get_tls();
	u64 o, identity, mapped, region = info(12, CUR_PROCESS, 0), region_size = info(13, CUR_PROCESS, 0);
	s32 index;
	struct mem_info mi;

	out("# P3 cross-process server begin\n");
	check_eq("cross server heap", svc_set_heap_size(&heap, 4 * 1024 * 1024), 0);
	check_eq("cross server bridge", svc_connect_to_named_port(&session, HZN_TEST_PORT), 0);
	check_eq("cross CreateSession", p3_pair(false, 0, false, &server, &client), 0);
	check_eq("GNU C launcher starts a second Horizon process", ipc1(CMD_P3_NATIVE_START, client, 0, 0), 1);
	svc_close_handle(client);
	memset(cmd, 0, 0x100);
	p3_receive_list(cmd, 8, heap + PAGE, 64);
	check_eq("cross receive", p3_receive(&server, 1, 0, 3000 * MS, &index, 0), 0);
	check_eq("cross PID is actual sender", cmd[3], cmd[20]);
	check_eq("copied process handle GetProcessId", SVC(0x24, 0, cmd[6], 0, 0, 0, 0, &identity), 0);
	check_eq("copied process ID", identity, cmd[20]);
	check_eq("copied thread handle GetProcessId", SVC(0x24, 0, cmd[7], 0, 0, 0, 0, &identity), 0);
	check_eq("copied thread process ID", identity, cmd[20]);
	check_eq("copied thread handle GetThreadId", SVC(0x25, 0, cmd[7], 0, 0, 0, 0, &identity), 0);
	check_eq("copied thread ID", identity, cmd[21]);
	svc_close_handle(cmd[6]); svc_close_handle(cmd[7]);
	check("cross PID differs from receiver", cmd[3] != (u32)lsys(NR_getpid, 0, 0, 0));
	shared = cmd[5]; moved = cmd[8];
	check_eq("X pointer uses receiver C list", p3_pointer_address(cmd + 9), heap + PAGE);
	check("X pointer payload", has((u8 *)(heap + PAGE), 32, 61));
	check("A mapped payload", has((u8 *)p3_map_address(cmd + 11), PAGE, 21));
	svc_query_memory(&mi, p3_map_address(cmd + 11));
	check_eq("A mapping is read-only", mi.perm, 1);
	fill((u8 *)p3_map_address(cmd + 14), 100, 77);
	*(volatile u64 *)p3_map_address(cmd + 17) = 0x7654;
	mapped = find_free(region, region_size, PAGE);
	check_eq("copied shared handle keeps remote permission", SVC(0x13, shared, mapped, PAGE, 3, 0, 0, &o), R_INVALID_NEW_MEMORY_PERMISSION);
	check_eq("copied shared handle maps remotely", SVC(0x13, shared, mapped, PAGE, 1, 0, 0, &o), 0);
	check_eq("copied shared object contents", *(volatile u64 *)mapped, 0x4444);
	check_eq("moved event handle signals across processes", p3_signal(moved), 0);
	svc_close_handle(moved);
	fill((u8 *)(heap + 5 * PAGE), 16, 88);
	memset(cmd, 0, 0x100);
	cmd[0] = 1u << 16; cmd[1] = 0x80000001; cmd[2] = 2; cmd[3] = shared;
	p3_pointer_descriptor(cmd + 4, heap + 5 * PAGE, 16); cmd[6] = 0xfed;
	check_eq("cross reply", p3_receive(NULL, 0, server, 0, &index, 0), R_TIMED_OUT);
	check_eq("cross reply timeout index", index, -1);
	check_eq("remote shared unmap", SVC(0x14, shared, mapped, PAGE, 0, 0, 0, &o), 0);
	svc_close_handle(shared);
	check_eq("cross peer closure", p3_receive(&server, 1, 0, 3000 * MS, &index, 0), KRES(123));
	svc_close_handle(server); svc_close_handle(session);
	out("# P3 cross-process server end\n");
	return failures;
}

static int p3_cross_client(const char *arg)
{
	u32 client = 0, shared, w, r, *cmd;
	u64 o, region = info(12, CUR_PROCESS, 0), region_size = info(13, CUR_PROCESS, 0), mapped;
	s32 index;

	while (*arg >= '0' && *arg <= '9')
		client = client * 10 + *arg++ - '0';
	out("# P3 cross-process client begin\n");
	check_eq("cross client heap", svc_set_heap_size(&heap, 4 * 1024 * 1024), 0);
	check_eq("cross shared memory", SVC(0x50, 0, PAGE, 3, 1, 0, 0, &o), 0);
	shared = o;
	mapped = find_free(region, region_size, PAGE);
	check_eq("cross owner shared map", SVC(0x13, shared, mapped, PAGE, 3, 0, 0, &o), 0);
	*(volatile u64 *)mapped = 0x4444;
	check_eq("cross event", p3_event(&w, &r), 0);
	fill((u8 *)(heap + PAGE), PAGE, 21);
	fill((u8 *)(heap + 2 * PAGE), PAGE, 55);
	fill((u8 *)(heap + 4 * PAGE), 32, 61);
	cmd = (u32 *)heap;
	memset(cmd, 0, PAGE);
	cmd[0] = 4 | (1u << 16) | (1u << 20) | (1u << 24) | (1u << 28);
	cmd[1] = 0x80000004; cmd[2] = 39; cmd[5] = shared; cmd[6] = CUR_PROCESS; cmd[7] = CUR_THREAD; cmd[8] = w;
	p3_pointer_descriptor(cmd + 9, heap + 4 * PAGE, 32);
	p3_map_descriptor(cmd + 11, heap + PAGE, PAGE);
	p3_map_descriptor(cmd + 14, heap + 2 * PAGE + 7, 100);
	p3_map_descriptor(cmd + 17, heap + 3 * PAGE, PAGE);
	cmd[20] = lsys(NR_getpid, 0, 0, 0);
	SVC(0x25, 0, CUR_THREAD, 0, 0, 0, 0, &o); cmd[21] = o;
	p3_receive_list(cmd, 32, heap + 6 * PAGE, 64);
	check_eq("cross user-buffer SendSyncRequest", SVC(0x22, heap, PAGE, client, 0, 0, 0, &o), 0);
	check_eq("cross reply payload", cmd[6], 0xfed);
	check("cross reply X payload", has((u8 *)(heap + 6 * PAGE), 16, 88));
	check("B output copied back", has((u8 *)(heap + 2 * PAGE + 7), 100, 77));
	check_eq("B prefix guard unchanged", *(u8 *)(heap + 2 * PAGE + 6), 55 + 6 * 7);
	check_eq("B suffix guard unchanged", *(u8 *)(heap + 2 * PAGE + 107), (u8)(55 + 107 * 7));
	check_eq("W output uses original pages", *(volatile u64 *)(heap + 3 * PAGE), 0x7654);
	check_eq("moved handle was removed", p3_signal(w), R_INVALID_HANDLE);
	check_eq("moved event signalled", svc_wait_synchronization(&index, &r, 1, 0), 0);
	check("reply copy handle translated", cmd[3] && cmd[3] != shared);
	svc_close_handle(cmd[3]); svc_close_handle(r);
	SVC(0x14, shared, mapped, PAGE, 0, 0, 0, &o);
	svc_close_handle(shared); svc_close_handle(client);
	out("# P3 cross-process client end\n");
	return failures;
}

static void p3_perf_server(u64 rounds)
{
	u32 *cmd = get_tls();
	s32 index;
	u64 i;

	for (i = 0; i < rounds; i++) {
		cmd[0] = cmd[1] = 0;
		if (p3_receive(&p3_server, 1, 0, 3000 * MS, &index, 0))
			break;
		cmd[0] = 0; cmd[1] = 1; cmd[2]++;
		if (p3_receive(NULL, 0, p3_server, 0, &index, 0) != R_TIMED_OUT)
			break;
	}
	if (i != rounds)
		p3_server_status = 1;
	svc_close_handle(p3_server);
	svc_exit_thread();
}
static void p3_perf_sample(const char *name, u64 ticks)
{
	out("# P3_PERF "); out(name); out(" ");
	out_dec(ticks * 1000000000ull / cntfrq()); out("\n");
}
static int p3_perf(bool native)
{
	u64 begin, end;
	u32 thread = 0, h, *cmd = get_tls();
	int sample, i;

	check_eq("perf heap", svc_set_heap_size(&heap, 4 * 1024 * 1024), 0);
	if (native) {
		check_eq("perf session", p3_pair(false, 0, false, &p3_server, &p3_client), 0);
		p3_server_status = 0;
		check_eq("perf server thread", svc_create_thread(&thread, p3_perf_server, 10000,
			STACK_TOP(0), HZN_TEST_PRIORITY, -2), 0);
		svc_start_thread(thread);
	} else {
		check_eq("perf bridge", svc_connect_to_named_port(&session, HZN_TEST_PORT), 0);
	}
	for (sample = 0; sample < 10; sample++) {
		begin = cntpct();
		for (i = 0; i < 1000; i++) {
			if (native) {
				cmd[0] = 4; cmd[1] = 1; cmd[2] = i;
				if (svc_send_sync_request(p3_client) || cmd[2] != i + 1)
					return 1;
			} else if (ipc1(CMD_ECHO, i, 0, 0) != i + 1) {
				return 1;
			}
		}
		end = cntpct();
		p3_perf_sample(native ? "native_1000" : "bridge_1000", end - begin);
	}
	if (native) {
		check("perf server finishes", join(thread, 3000 * MS));
		check_eq("perf server result", p3_server_status, 0);
		svc_close_handle(thread); svc_close_handle(p3_client);
	} else {
		/* Existing and P3 kernels run this identical transfer-memory experiment. */
		for (sample = 0; sample < 10; sample++) {
			begin = cntpct();
			for (i = 0; i < 100; i++) {
				if (svc_create_transfer_memory(&h, heap + PAGE * 256, PAGE * 256, 3))
					return 1;
				svc_close_handle(h);
			}
			p3_perf_sample("tmem_1MiB_100", cntpct() - begin);
		}
		ipc1(CMD_BYE, 0, 0, 0);
	}
	return failures;
}
