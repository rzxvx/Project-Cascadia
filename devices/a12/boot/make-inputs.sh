#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$A12/boot/fetch-device-inputs.py" "$@"
