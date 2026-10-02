# odin3-horizon: Linux 7.2 for Ubuntu 26.04 on the AYN Odin 3

This branch holds the source of the `7.2.0-odin3` kernel of the Ubuntu 26.04
image for the AYN Odin 3 (Qualcomm SM8750 / CQ8725S), shipped as
`linux-image-7.2.0-odin3_7.2.0-2_arm64.deb` and `linux-headers-7.2.0-odin3_7.2.0-2_arm64.deb`,
with the Horizon kernel interface of this repository on top.
It has no history in common with the Rockchip branches of this repository.

Up to commit `36e21d3` ("arm64: add odin3_defconfig and README.odin3.md"), every file is
byte-identical to the tree those packages were compiled from, except this README and
`arch/arm64/configs/odin3_defconfig`. Check out that commit to rebuild the shipped kernel.

## History

1. **Linux 7.2**: the kernel.org tarball `linux-7.2.tar.xz`
   (sha256 `f9fef3d14c0df53819026f4be74459835c2a0b0dcbf5b5bbd9ea19f0829402b3`),
   unmodified. Its tree equals upstream commit `8d3ae59288f1` (tag `v7.2`).
2. **ROCKNIX SM8750 patches**, one commit per patch, in the order ROCKNIX applies them, from
   [ROCKNIX/distribution@a55d58a1209b35e287dd55a3aad67a5543b467ce](https://github.com/ROCKNIX/distribution/tree/a55d58a1209b35e287dd55a3aad67a5543b467ce):
   `projects/ROCKNIX/packages/linux/patches/mainline/`, then `.../patches/7.2/`,
   then `projects/ROCKNIX/devices/SM8750/patches/linux/`.
3. **Device trees** for the AYN Odin 3 and the KONKR Pocket FIT Elite, from ROCKNIX
   `projects/ROCKNIX/devices/SM8750/linux/dts/qcom/` (copied in after the patches, as ROCKNIX does).
4. **Four ubuntu-odin3 patches**, applied after the device trees:
   - `arm64: dts: qcom: build the AYN Odin 3 and KONKR DTBs` (ours: lets `bindeb-pkg` ship the DTBs);
   - `clk: qcom: dispcc-sm8750: knock down display block resets on probe` and
     `arm64: dts: qcom: cq8725s-ayn: point the codec at the USBSS switch`, from
     [armbian/build@2d20a726](https://github.com/armbian/build/tree/2d20a726)
     `patch/kernel/archive/sm8750-7.1/` (0700, 0065);
   - `drm/msm: adreno a8xx: force GX GDSC collapse before CX in recovery`, from
     pocknix-odin3-support `kernel/patches/20-sm8750/0051` (issue #54).
5. `odin3_defconfig` and this README.
6. **Horizon**: horizon-droid `d46ed0a7` ("merge horizon-linux from 5.10 branch initially"),
   cherry-picked from the Rockchip 6.1 branch `dev-rk-6.1-rkr5.1` and ported to 7.2. Its
   commit message lists what the port changed.
7. **Horizon on kernel facilities**: the same user-space interface, reimplemented in
   `kernel/horizon/` on what Linux 7.2 already has, with selftests. See [Horizon](#horizon)
   below.

Each patch commit ends with a `Source:` line naming the file it came from. Patches that `git am`
cannot import (no mail header, no author address, or hunks that need fuzz) were applied with
`patch -p1 --forward`, as the build does, and carry an `Applied-with:` line saying why.

## Configuration

`odin3_defconfig` is `make savedefconfig` of the configuration the packages were built with:
ROCKNIX `devices/SM8750/linux/linux.aarch64.conf`, merged with the Ubuntu fragment of the
ubuntu-odin3 build (`LOCALVERSION="-odin3"`, AppArmor/Yama/Landlock, compressed firmware loading,
dm-crypt, USB/IP, no debug info, ...), then `olddefconfig`. The Horizon configuration
clears `CONFIG_LOCALVERSION`, so the kernel release ends in `-horizon` without a
device or service suffix. `CONFIG_EXTRA_FIRMWARE_DIR` points at `firmware/` in the
source tree instead of the build host's `/build/kernel/external-firmware`.

## Building

Cross-compiled on Ubuntu 26.04 (x86_64) with its GCC 15.2 (`gcc-aarch64-linux-gnu`
15.2.0-16ubuntu1). Put the built-in firmware in place first (next section), then:

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- odin3_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" \
     LOCALVERSION= DTC_FLAGS=-@ KDEB_PKGVERSION=7.2.0-2 bindeb-pkg
```

- `LOCALVERSION=` keeps the release at `7.2.0-horizon`; in a git checkout the kernel
  otherwise appends `+`.
- `DTC_FLAGS=-@` keeps the symbols DT overlays need, like the shipped DTBs.
  The Odin 3 boots `qcom/cq8725s-ayn-odin3.dtb`.
- The shipped packages were built with `KBUILD_BUILD_USER=odin3 KBUILD_BUILD_HOST=odin3-build`.
- On Ubuntu 26.04 put GNU coreutils (`/usr/bin/gnu*`, e.g. `gnuinstall` linked as `install`)
  first in `PATH`: the default uutils `install -D` races in the parallel `dtbs_install` step
  of `bindeb-pkg`.

## Horizon

`CONFIG_HORIZON` adds the Nintendo Switch (Horizon OS) system call interface that the Horizon
Linux loader [mizu](https://github.com/kentjhall/mizu) runs programs on. A Horizon program is
a Linux process. Its `svc #N` calls are handled by the kernel, and Linux processes serve its IPC.
The user-space interface is that of the original:
- `horizon_execve`, `horizon_execveat` and `horizon_servctl`, syscalls 500 to 502;
- `include/uapi/linux/horizon.h`.

Build it with:

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- odin3_defconfig horizon.config
```

### How it works

The code is in `kernel/horizon/`. The rest of the kernel has only small hooks:
- SVC dispatch in `arch/arm64/kernel/syscall.c`;
- the TLS register and counter access on context switch;
- fork, exit and exec hooks;
- a fault hook in `arm64_force_sig_fault()` for user exceptions;
- `eventfd_file_create()`, for events the kernel makes;
- three fields in `task_struct`.

Commit 7 replaced the 5.10-era implementation. That implementation had its own futex copy,
scheduling class, soft-dirty bits and file-table changes. The new one uses what the kernel already has:
- **Threads and handles:**
  - Threads are ordinary tasks.
  - Handles are file descriptors (handle = fd + 1).
  - Thread handles retain task identity after exit and use thread pidfds (`PIDFD_THREAD`) for exit polling.
  - `CreateEvent` returns distinct readable and writable handles to a shared manual-reset event. Linux service-bridge events still use eventfds.
- **Scheduling:**
  - Horizon priorities map onto `SCHED_FIFO` 1..49.
  - That needs `CAP_SYS_NICE` or an `RLIMIT_RTPRIO` above 0. Without `CAP_SYS_NICE` the priority is capped at `RLIMIT_RTPRIO`.
  - With `RLIMIT_RTPRIO` 0, threads stay `SCHED_NORMAL`.
- **Synchronization:** `ArbitrateLock`, the condition variables and `WaitForAddress`/`SignalToAddress` follow Horizon's `KConditionVariable` and `KAddressArbiter`. Waiters are ordered by effective priority. Contended mutexes donate priority along the owner chain, including condition-variable reacquisition; the arbiter's internal lock uses Linux RT-mutex priority inheritance.
- **Memory:**
  - Heap, data segments, shared memory, transfer memory and physical memory use shmem.
  - Shared-memory handles retain owner and remote permissions. Transfer and code-memory objects borrow existing heap pages without copying, lock the source range, and restore its permissions when the object is released.
  - `ControlCodeMemory` maps writable code output and readable or executable generated code, with instruction-cache maintenance before execution.
  - Services map them (`HZN_SCTL_MAP_MEMORY`) without copying.
  - `svcMapMemory` aliases with plain `mmap()`.
  - `svcSetMemoryPermission` maps the same pages again with the new protection. Ranges locked with `MemoryAttribute_PermissionLocked` (RELRO, by rtld and `nn::ro`) are kept per process.
- **User exceptions:** As on Horizon, a fault of a Horizon thread enters the program at its entry point, with x0 the exception type and x1 the exception info in the process local region; rtld passes it to nnSdk's handler. `svcReturnFromException` resumes the thread, or lets the fault become its signal. One thread of a process handles an exception at a time. Breakpoints a debugger set, hardware breakpoints, watchpoints and single steps stay signals.
- **Memory watching:** `HZN_SCTL_MEMWATCH_GET[_CLEAR]` write-protects PTEs, because arm64 has no soft-dirty bit. It never touches the dirty bit.
- **Counter:** Programs read `CNTPCT_EL0` directly; it is enabled per task in `CNTKCTL_EL1`. This works under Gunyah on the Odin 3.
- **Unstarted threads:** A created but unstarted thread waits in a killable, freezable task work. Exit, exec and suspend work while it waits.
- **Process identity:** `HZN_SCTL_GET_PROCESS_ID` returns the process ID, not the thread ID.
- **Named ports:** Ports are per user. A process only connects to services of its own real user ID.
- **Native server IPC:** `CreateSession`, `CreatePort`, `ConnectToPort`, `AcceptSession` and both `ReplyAndReceive` variants provide native HIPC endpoints. Messages transfer copy/move handles, caller PID, X/C pointer buffers and A/B/W mapped buffers. Aligned shared buffers reuse their pages; unaligned or private buffers use bounded bounce mappings that preserve page offsets and protect neighboring bytes.
- **Linux services:** Ordinary GNU C/Linux programs continue to register named services through Linux syscall `horizon_servctl`, using `HZN_SCTL_REGISTER_NAMED_SERVICE`, `GET_CMD` and `PUT_CMD`. A process using this bridge does not need the Horizon personality.
- **Cache:** `FlushEntireDataCache` runs real data-cache set/way maintenance on all CPUs. This is a system-wide operation, intended for occasional use.

It works with transparent huge pages.

### Built with `CONFIG_HORIZON=n`

Built with `odin3_defconfig` alone, the kernel has the same code and data as `7.2.0-odin3`. Only these differ (checked object by object against the shipped build):
- the release string: `7.2.0-horizon-odin3`, from `EXTRAVERSION = -horizon`;
- the embedded configuration;
- source path prefixes;
- `__LINE__` values.

### Limitations

- Only 64-bit programs run; 32-bit Horizon programs are refused.
- Memory watching can miss a write in two cases, as soft-dirty clearing can:
  - a write to a page that is reclaimed before the next query;
  - a write that races with the query that clears it.
- `svcSetMemoryPermission` works on the heap and on module data, not on physical memory.
- The Linux service bridge receives the first page of a user-buffer request. Native HIPC endpoints validate the full user buffer and parse its message and buffer descriptors.
- `svcSendAsyncRequestWithUserBuffer` still completes synchronously before returning an already-signalled event. It does not provide asynchronous execution.
- Light sessions and ports can be created, but regular HIPC calls reject light endpoints. The separate light IPC SVCs are not implemented.
- Horizon waits are killable, not interruptible. A Horizon SVC cannot be restarted after a signal.
  - The cgroup v2 freezer, which systemd uses to freeze `user.slice` before suspend, only freezes such a thread once its wait ends.
  - Job-control stops behave the same way.
  - The suspend freezer and the cgroup v1 freezer do freeze these threads.

### Tests

`tools/testing/selftests/horizon` runs a freestanding Horizon program through the loader and serves its IPC the way a mizu service does:

```sh
make -C tools/testing/selftests TARGETS=horizon run_tests
```

The original tests passed:
- under QEMU, including with lockdep, KASAN and kmemleak;
- on the Odin 3 as a user;
- on the Odin 3 as root, which also checks `SCHED_FIFO` and the freezer.

The P3 changes were validated on 2026-10-02 with a four-CPU ARM64 QEMU guest,
KASAN, lockdep and kmemleak. Root passed all 15 TAP cases, with 537 checks in
each of the 39-bit and 36-bit guest address spaces. UID 1000 passed 14 cases
and skipped the privileged freezer case, with 515 checks per address space.
The suite includes cross-process native HIPC, copied process/thread handles,
moved event handles, pointer and mapped buffers, permission checks, source
restoration, generated-code execution, and Linux bridge compatibility. No
kernel warnings or leak reports were observed. This validation ran under QEMU;
device deployment is a separate step. It does not establish SDK, NVN or window
support.

GDB uses Linux ptrace to debug Horizon programs. New Horizon threads report
clone events, including their initial stop before `StartThread`. Debugger
stops preserve blocked SVCs and their deadlines; queued task work is deferred
while handling a stop to avoid recursive pause callbacks. `GetInfo(8)` reports
whether any process thread is traced, and traced `svcBreak` calls stop with
SIGTRAP, including notification-only breaks. Thread handles are initialized
in TLS before publication. These interfaces do not implement Horizon debug SVCs.

The optional GDB smoke test exercises all-stop thread tracking, software and
hardware breakpoints, single stepping, registers, memory, a hardware watchpoint,
and resuming both break kinds. Run it from the selftest output directory:

```sh
timeout 90 gdb -q -batch -x gdb-smoke.gdb \
    --args ./hzn_test ./hzn_guest --gdb-inferior
```

The selftest also provides optional microbenchmarks:

```sh
./hzn_test ./hzn_guest --bridge-benchmark
./hzn_test ./hzn_guest --p3-benchmark
```

With an identical debug configuration, four QEMU CPUs and 1 GiB RAM, the
median of ten samples for 100 create/close operations on 1 MiB transfer memory
fell from 10.391 seconds at `c91403dd8` to 0.123 seconds by reusing the original
pages. Profiling native IPC identified allocation bookkeeping as a hot path;
allocating descriptors only when present and skipping unused receive-buffer
snapshots reduced 1,000 native round trips from 2.415 to 1.615 seconds. The
legacy bridge showed no regression in this run. These debug/QEMU measurements
are not estimates of Odin 3 performance.

## Built-in firmware (not in this repository)

`CONFIG_EXTRA_FIRMWARE` links six files into the kernel image, because the GPU driver
(`DRM_MSM=y`) and cfg80211 (`CFG80211=y`) request them before the root file system is mounted.
They are not committed here. Put them in `firmware/` at the top of the source tree
(`CONFIG_EXTRA_FIRMWARE_DIR="firmware"` is relative to the source tree), and don't commit them:

| file | from | sha256 |
|---|---|---|
| `qcom/gen80000_sqe.fw` | linux-firmware, tag `20260309` | `30ee3301534f95799d4afaf73864f3c369baa5a5dea8481e7f0cb8cc941f396c` |
| `qcom/gen80000_aqe.fw` | linux-firmware, tag `20260309` | `67562ad5baae3a133c92a6e48a9d1b1a9a867fe2f85b49324e244e963f3b89c5` |
| `qcom/gen80000_gmu.bin` | linux-firmware, tag `20260309` | `1ad8175e6cd01ea76c64d20b83600038c52dd29844b48d57b11b769b038863a5` |
| `qcom/sm8750/gen80000_zap.mbn` | linux-firmware, tag `20260309` | `0e3ae03f7dd3170621e7b231802639535410bbc2a1c3ba0829c15ab4cf3996cf` |
| `regulatory.db` | wireless-regdb (Ubuntu 26.04 package `2026.05.30-0ubuntu1~26.04.1`) | `2fb33ca0074db573e05ef7dd50bb45b63c0ff98b7e852e1105ebad536fae8e6b` |
| `regulatory.db.p7s` | wireless-regdb, same package | `c941c08f51c93e46722293b85631604c3740d86c3de0c75f79aef50d2e919179` |

For example:

```sh
git clone --depth 1 --branch 20260309 \
    https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git /tmp/linux-firmware
for f in qcom/gen80000_sqe.fw qcom/gen80000_aqe.fw qcom/gen80000_gmu.bin qcom/sm8750/gen80000_zap.mbn; do
    install -Dm644 "/tmp/linux-firmware/$f" "firmware/$f"
done
(cd /tmp && apt-get download wireless-regdb && dpkg-deb -x wireless-regdb_*.deb regdb)
install -m644 /tmp/regdb/lib/firmware/regulatory.db /tmp/regdb/lib/firmware/regulatory.db.p7s firmware/
(cd firmware && sha256sum qcom/gen80000_* qcom/sm8750/gen80000_zap.mbn regulatory.db*)
```

To use firmware from elsewhere instead, override `CONFIG_EXTRA_FIRMWARE_DIR` with an absolute path.
