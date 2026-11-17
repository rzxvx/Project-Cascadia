#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
export A12_DEVICE="${A12_DEVICE:-ipad8}"
source "$A12/profile.sh"
[ "$A12_DEVICE" = ipad8 ] || { echo 'SmartIO preparation is J171-only' >&2; exit 2; }
exec python3 "$A12/boot/fetch-device-inputs.py"
