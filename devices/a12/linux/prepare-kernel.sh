#!/usr/bin/env bash
# Fetch the exact source/config baseline used for XR bring-up.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
K="$A12/build/linux"
DOCS="$A12/build/hoolock-docs"
KREPO=https://github.com/HoolockLinux/linux.git
KCOMMIT=6831bc701a6ce059e71e5aaa9488c9195bea6927
DREPO=https://github.com/HoolockLinux/docs.git
DCOMMIT=23ebe1fbc375599221553a7e1815e5de182a6b42
CONFIG_SHA=a5cbcb0d3fe655888106672e6fe03477b63d51bd36bbcfc4f5145627d906b208
if [ "${1:-}" = --check ]; then
  [ "$(git -C "$K" rev-parse HEAD)" = "$KCOMMIT" ] || {
    echo 'kernel checkout is not the pinned baseline; use explicit build kernel-clean' >&2; exit 1;
  }
  [ "$(git -C "$DOCS" rev-parse HEAD)" = "$DCOMMIT" ] || {
    echo 'config checkout is not the pinned baseline; use explicit build kernel-clean' >&2; exit 1;
  }
  printf '%s  %s\n' "$CONFIG_SHA" "$DOCS/config_16k" | sha256sum -c -
  exit 0
fi
mkdir -p "$A12/build"
fetch_clean() {
  local repo="$1" commit="$2" dest="$3"
  if [[ -e "$dest" && ! -d "$dest/.git" ]]; then
    echo "refusing non-git directory: $dest" >&2; exit 1
  fi
  if [[ ! -d "$dest/.git" ]]; then
    git init -q "$dest"
    git -C "$dest" remote add origin "$repo"
  fi
  [[ "$(git -C "$dest" remote get-url origin)" == "$repo" ]] || {
    echo "unexpected remote for $dest" >&2; exit 1;
  }
  git -C "$dest" fetch --depth=1 --no-tags origin "$commit"
  # build/ is ignored and owned by these scripts.  Recreate a clean baseline
  # after an earlier build left the downstream patch series and object files.
  git -C "$dest" checkout -q -f --detach FETCH_HEAD
  git -C "$dest" clean -qfdx
  [[ "$(git -C "$dest" rev-parse HEAD)" == "$commit" ]]
}
fetch_clean "$KREPO" "$KCOMMIT" "$K"
fetch_clean "$DREPO" "$DCOMMIT" "$DOCS"
printf '%s  %s\n' "$CONFIG_SHA" "$DOCS/config_16k" | sha256sum -c -
echo "XR Linux baseline ready; run devices/a12/linux/build-kernel.sh"
