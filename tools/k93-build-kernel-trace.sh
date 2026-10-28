#!/usr/bin/env bash
# Run inside the existing cross-toolchain container. Reuse configured RAM root.
set -euo pipefail
cd "$(dirname "$0")/.."
TRACE_FLAGS=-DK93_EARLY_TRACE
TRACE_OUT=build/k93/kernel-trace
INSTRUMENT_ARGS=()
case "${1:-}" in
    '') ;;
    --watchdog-stages)
        TRACE_FLAGS="$TRACE_FLAGS -DK93_WDT_STAGE_TRACE"
        TRACE_OUT=build/k93/kernel-trace-staged ;;
    --mmu-stages)
        TRACE_FLAGS="$TRACE_FLAGS -DK93_WDT_STAGE_TRACE -DK93_MMU_STAGE_TRACE"
        TRACE_OUT=build/k93/kernel-trace-mmu ;;
    --init-stages|--late-stages)
        TRACE_FLAGS="$TRACE_FLAGS -DK93_WDT_STAGE_TRACE -DK93_MMU_STAGE_TRACE -DK93_INIT_STAGE_TRACE"
        TRACE_OUT=build/k93/kernel-trace-init
        INSTRUMENT_ARGS=("$1")
        if [[ "$1" == --late-stages ]]; then
            TRACE_OUT=build/k93/kernel-trace-late
        fi ;;
    *) echo 'Unknown trace build option'; exit 2 ;;
esac
exec 9>build/.build.lock
flock -n 9 || { echo 'Kernel build already active'; exit 1; }
cmp build/linux/.config build/out/kernel-config
python3 - <<'PY'
from pathlib import Path
c = Path('build/linux/.config').read_text().splitlines()
assert not {'CONFIG_MTD=y', 'CONFIG_DRM_APPLE_SGX=y'} & set(c), 'Unexpected storage/GPU config'
assert 'CONFIG_ARCH_APPLE_S5L=y' in c
assert '# CONFIG_THUMB2_KERNEL is not set' in c, 'Trace requires ARM-state kernel'
assert 'CONFIG_KASAN=y' not in c, 'Diagnostic watchdog mapping needs non-KASAN clearing path'
PY
python3 tools/k93-instrument-kernel.py "${INSTRUMENT_ARGS[@]}"
make -C build/linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
    KAFLAGS="$TRACE_FLAGS" -j8 zImage
mkdir -p "$TRACE_OUT"
cp build/linux/arch/arm/boot/zImage "$TRACE_OUT/zImage"
cp build/linux/.config "$TRACE_OUT/kernel-config"
echo 'K93 early-trace kernel built; not hardware-tested'
