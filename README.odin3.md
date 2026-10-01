# odin3-horizon: Linux 7.2 for Ubuntu 26.04 on the AYN Odin 3

This branch is the exact source of the `7.2.0-odin3` kernel of the Ubuntu 26.04
image for the AYN Odin 3 (Qualcomm SM8750 / CQ8725S), shipped as
`linux-image-7.2.0-odin3_7.2.0-2_arm64.deb` and `linux-headers-7.2.0-odin3_7.2.0-2_arm64.deb`.
It has no history in common with the Rockchip branches of this repository.

Every file is byte-identical to the tree those packages were compiled from,
except this README and `arch/arm64/configs/odin3_defconfig`.

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

Each patch commit ends with a `Source:` line naming the file it came from. Patches that `git am`
cannot import (no mail header, no author address, or hunks that need fuzz) were applied with
`patch -p1 --forward`, as the build does, and carry an `Applied-with:` line saying why.

## Configuration

`odin3_defconfig` is `make savedefconfig` of the configuration the packages were built with:
ROCKNIX `devices/SM8750/linux/linux.aarch64.conf`, merged with the Ubuntu fragment of the
ubuntu-odin3 build (`LOCALVERSION="-odin3"`, AppArmor/Yama/Landlock, compressed firmware loading,
dm-crypt, USB/IP, no debug info, ...), then `olddefconfig`. The only change is
`CONFIG_EXTRA_FIRMWARE_DIR`, which now points at `firmware/` in the source tree instead of the
build host's `/build/kernel/external-firmware`.

## Building

Cross-compiled on Ubuntu 26.04 (x86_64) with its GCC 15.2 (`gcc-aarch64-linux-gnu`
15.2.0-16ubuntu1). Put the built-in firmware in place first (next section), then:

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- odin3_defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" \
     LOCALVERSION= DTC_FLAGS=-@ KDEB_PKGVERSION=7.2.0-2 bindeb-pkg
```

- `LOCALVERSION=` keeps the release at `7.2.0-odin3`; in a git checkout the kernel otherwise
  appends `+`.
- `DTC_FLAGS=-@` keeps the symbols DT overlays need, like the shipped DTBs.
  The Odin 3 boots `qcom/cq8725s-ayn-odin3.dtb`.
- The shipped packages were built with `KBUILD_BUILD_USER=odin3 KBUILD_BUILD_HOST=odin3-build`.
- On Ubuntu 26.04 put GNU coreutils (`/usr/bin/gnu*`, e.g. `gnuinstall` linked as `install`)
  first in `PATH`: the default uutils `install -D` races in the parallel `dtbs_install` step
  of `bindeb-pkg`.

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
