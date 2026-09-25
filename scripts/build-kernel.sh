#!/usr/bin/env bash
#
# Phase 4: cross-compile an armhf kernel for iPad2,5 (P105AP).
#
# Idempotent: safe to re-run.  Pass --clean to start from scratch.
#
#   ./scripts/build-kernel.sh [--clean] [-j N]
#
# Produces:
#   build/linux/arch/arm/boot/zImage
#   build/out/zImage
#   build/out/kernel-config
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/linux"
OUT="$ROOT/build/out"
LOGS="$ROOT/logs"

export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-

JOBS="$(nproc)"
CLEAN=0
while [ $# -gt 0 ]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        -j) JOBS="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -f "$TREE/Makefile" ] || { echo "no kernel tree at $TREE" >&2; exit 1; }
mkdir -p "$OUT" "$LOGS"

# Two makes in one kernel tree corrupt each other in confusing ways: you get
# "fixdep: error opening file .foo.o.d" and "Error 123" scattered across
# unrelated subdirectories, which looks like a toolchain problem and is not.
# Take an exclusive lock so a stray background invocation cannot do that.
exec 9>"$ROOT/build/.build.lock"
if ! flock -n 9; then
    echo "another build is already running in $TREE (build/.build.lock held)" >&2
    exit 1
fi

echo "==> Installing Apple S5L platform support"
python3 "$ROOT/scripts/apply-kernel-patches.py" --tree "$TREE" --files-only
bash "$ROOT/scripts/apply-kernel-edits.sh" --tree "$TREE"

# Copy sandcastle-port driver sources verbatim over the tree so master edits in
# patches/files-sandcastle-port/ are actually picked up on every rebuild.
if [ -d "$ROOT/patches/files-sandcastle-port" ]; then
    echo "==> Installing sandcastle-port drivers"
    ( cd "$ROOT/patches/files-sandcastle-port" && \
        find . -type f -exec sh -c '
            for f; do
                d="'"$TREE"'/$f"
                mkdir -p "$(dirname "$d")"
                cp "$f" "$d"
                echo "    installed $f"
            done
        ' sh {} + )
fi

if [ "$CLEAN" = 1 ]; then
    echo "==> make mrproper"
    make -C "$TREE" mrproper >/dev/null
fi

# Before compiling anything: the fragment enables what this board needs in one
# section and trims what multi_v7_defconfig drags in somewhere else, and the two
# can contradict each other.  merge_config takes the last word, silently.
echo "==> Checking the config fragment for self-contradictions"
awk -f "$ROOT/scripts/check-config-fragment.awk" "$ROOT/config/p105ap.config" \
    || { echo "config/p105ap.config contradicts itself; see above" >&2; exit 1; }
echo "    ok: no symbol is both enabled and disabled"

echo "==> Configuring (multi_v7_defconfig + config/p105ap.config)"
make -C "$TREE" multi_v7_defconfig > "$LOGS/kernel-config.log" 2>&1
"$TREE/scripts/kconfig/merge_config.sh" -m -O "$TREE" \
    "$TREE/.config" "$ROOT/config/p105ap.config" >> "$LOGS/kernel-config.log" 2>&1

# Build the rootfs into the kernel rather than shipping it separately.  iBoot's
# "go" jumps without setting r0-r2, so there is no way to hand the kernel an
# initrd pointer, and a second irecovery upload would have to land on an address
# iBEC has not already claimed.  One blob sidesteps both problems.
INITRAMFS_DIR="${INITRAMFS_DIR:-$ROOT/build/initramfs-root}"
if [ -d "$INITRAMFS_DIR" ]; then
    echo "==> Embedding initramfs from $INITRAMFS_DIR"
    frag="$(mktemp)"
    # ROOT_UID/GID -1: whoever runs this build becomes root in the image.
    # gen_initramfs records each file's real owner, and on a Linux host the
    # container runs as the caller, so without the mapping every file in the
    # image belonged to uid 1000 -- and dropbear, correctly, refuses a root
    # authorized_keys that root does not own, so ssh fell back to a password
    # that does not exist.  On macOS Docker Desktop presents the bind mount as
    # root's anyway, which is why the Mac never showed it.
    {
        echo "CONFIG_INITRAMFS_SOURCE=\"$INITRAMFS_DIR\""
        echo "CONFIG_INITRAMFS_COMPRESSION_GZIP=y"
        echo "CONFIG_INITRAMFS_ROOT_UID=-1"
        echo "CONFIG_INITRAMFS_ROOT_GID=-1"
    } > "$frag"
    "$TREE/scripts/kconfig/merge_config.sh" -m -O "$TREE" \
        "$TREE/.config" "$frag" >> "$LOGS/kernel-config.log" 2>&1
    rm -f "$frag"
else
    echo "==> No initramfs at $INITRAMFS_DIR (run scripts/build-initramfs.sh first)"
fi

make -C "$TREE" olddefconfig >> "$LOGS/kernel-config.log" 2>&1

# merge_config warns but does not fail when a symbol it was asked for did not
# survive olddefconfig, usually because a dependency is unmet.  That is exactly
# the class of mistake worth failing on, so check the ones we cannot boot
# without by hand.
echo "==> Verifying critical config symbols"
missing=0
# ARM_GLOBAL_TIMER used to be here.  It is gone on purpose: the A9 global
# timer is dead on this SoC (PERIPHCLK is not supplied) and requiring it only
# taught us to ignore this list.  What IS load-bearing now is the USB path --
# the system tick rides on the dwc2 SOF interrupt, so a kernel without a
# working peripheral-mode gadget is a kernel with frozen jiffies.
for sym in CONFIG_ARCH_APPLE_S5L CONFIG_APPLE_AIC1 CONFIG_SERIAL_SAMSUNG \
           CONFIG_SERIAL_EARLYCON CONFIG_FB_SIMPLE CONFIG_BLK_DEV_INITRD \
           CONFIG_SPI_SPIDEV CONFIG_INPUT_UINPUT CONFIG_DEVMEM \
           CONFIG_USB_DWC2_PERIPHERAL CONFIG_PHY_APPLE_S5L_USB \
           CONFIG_USB_CDC_COMPOSITE CONFIG_UNIX98_PTYS CONFIG_TMPFS \
           CONFIG_NFS_FS CONFIG_NFS_V3; do
    if ! grep -q "^${sym}=y" "$TREE/.config"; then
        echo "    MISSING: $sym"
        missing=$((missing + 1))
    else
        echo "    ok: $sym"
    fi
done
[ "$missing" -eq 0 ] || { echo "$missing required symbol(s) missing" >&2; exit 1; }
# Wanted, not required: the image boots without Wi-Fi, but a dependency that
# quietly dropped one of these should be seen here, not on the device.
for sym in CONFIG_USB_EHCI_HCD_PLATFORM CONFIG_CFG80211 CONFIG_BRCMFMAC_USB; do
    if grep -q "^${sym}=y" "$TREE/.config"; then
        echo "    ok: $sym"
    else
        echo "    note: $sym did not survive olddefconfig -- no Wi-Fi"
    fi
done

echo "==> Building zImage with -j$JOBS (log: logs/kernel-build.log)"
make -C "$TREE" -j"$JOBS" zImage > "$LOGS/kernel-build.log" 2>&1

cp "$TREE/arch/arm/boot/zImage" "$OUT/zImage"
cp "$TREE/.config" "$OUT/kernel-config"

echo "==> Verifying boot stamps in vmlinux (zImage is compressed)"
VMLINUX="$TREE/vmlinux"
[ -f "$VMLINUX" ] || { echo "error: missing $VMLINUX" >&2; exit 1; }
# grep -a on the ELF — avoid `strings | grep -q` under pipefail (SIGPIPE false fail)
if grep -aFq 'rest_clone' "$VMLINUX"; then
    echo "error: vmlinux still contains rest_clone (stale bisect kernel)" >&2
    exit 1
fi
# irq_rearm is the stamp for apple_aic1_rearm().  It is listed FIRST and it is
# the one that matters: kernel_init() quiesces the AIC immediately before
# do_initcalls(), so without the re-arm nothing in the system ever takes an
# interrupt -- no USB, no tick, no touch.  apply-p105-boot-hacks.py installs it
# by anchor match and skips silently when an anchor drifts, which is exactly how
# a clean tree can build green and boot dead.  Fail the build instead.
for need in irq_rearm pre_spawn umt_go aic1q34 irq_try; do
    if ! grep -aFq "$need" "$VMLINUX"; then
        echo "error: vmlinux missing stamp '$need' — boot hacks not applied?" >&2
        exit 1
    fi
done
echo "    ok: irq_rearm pre_spawn umt_go aic1q34 irq_try (no rest_clone)"

echo "==> Done"
ls -l "$OUT/zImage"
"${CROSS_COMPILE}size" "$TREE/vmlinux" 2>/dev/null || true
