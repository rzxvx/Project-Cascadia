#!/bin/sh
# metal-isa.sh SRC.metal FUNC... -- compile Metal compute kernels offline for
# the A13's GPU (applegpu_g12p) and, to compare, G13 (applegpu_g13g), and
# print each kernel's machine code (G5, devices/a13/docs/research/gpu.md). Needs
# Xcode's Metal Toolchain (xcodebuild -downloadComponent MetalToolchain).
# Output files go to $OUT (default devices/a13/build/gpu/isa).
set -e
SRC=$1; shift
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../../../build/gpu/isa}
mkdir -p "$OUT"
NAME=$(basename "$SRC" .metal)
xcrun -sdk iphoneos metal -std=metal3.0 -mios-version-min=16.4 -c "$SRC" -o "$OUT/$NAME.air"
xcrun -sdk iphoneos metallib "$OUT/$NAME.air" -o "$OUT/$NAME.metallib"
{
    printf '{ "libraries": { "paths": [ { "label": "lib", "path": "%s" } ] },\n' "$OUT/$NAME.metallib"
    printf '  "pipelines": { "compute_pipelines": [\n'
    sep=""
    for f in "$@"; do
        printf '%s    { "compute_function": "alias:lib#%s" }' "$sep" "$f"
        sep=",
"
    done
    printf '\n  ] } }\n'
} > "$OUT/$NAME.mtlp-json"
for arch in applegpu_g12p applegpu_g13g; do
    xcrun metal-tt -arch $arch -platform_version ios 16.4 16.4 \
        "$OUT/$NAME.metallib" "$OUT/$NAME.mtlp-json" -o "$OUT/$NAME.$arch.metallib"
    echo "=== $arch ($*)"
    python3 "$HERE/gpumacho.py" "$OUT/$NAME.$arch.metallib"
done
