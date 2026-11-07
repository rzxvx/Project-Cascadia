#!/usr/bin/env bash
# Compatibility entry point: XR only, with strict per-device manifests.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/profile.sh"
[ "$A12_DEVICE" = xr ] || { echo 'XR entry point refuses iPad profile' >&2; exit 2; }
[ -z "${XR_LINUX_IMG4:-}" ] || { echo 'Use the selected boot directory and verified linux.json, not a legacy payload override' >&2; exit 2; }
exec python3 "$A12/boot/boot-device.py" "$@"
