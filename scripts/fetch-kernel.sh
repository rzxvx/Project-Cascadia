#!/usr/bin/env bash
#
# Put the kernel source at build/linux, pinned to the version this port is
# written against.
#
#   bash scripts/fetch-kernel.sh              # clone it
#   KERNEL_SRC=/path/to/linux bash scripts/fetch-kernel.sh   # use an existing tree
#
# The pin matters more here than in most projects.  The patches are applied by
# anchor matching (scripts/apply-kernel-patches.py, apply-p105-boot-hacks.py)
# and a missed anchor is skipped rather than reported -- so a tree at some other
# version can build cleanly and boot without the patch that makes interrupts
# work.  build-kernel.sh checks the resulting symbols and stamps for exactly
# that reason, but starting from the right source is cheaper than catching it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/linux"
KERNEL_TAG="${KERNEL_TAG:-v6.12}"
KERNEL_URL="${KERNEL_URL:-https://github.com/torvalds/linux.git}"

fail() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have git || fail "git is not installed"

# An existing tree, either pointed at by KERNEL_SRC or already cloned here.
check_tree() {
    [ -f "$1/Makefile" ] || return 1
    v=$(awk -F' = ' '/^VERSION/{a=$2} /^PATCHLEVEL/{b=$2} /^SUBLEVEL/{c=$2} END{print a"."b"."c}' "$1/Makefile")
    echo "$v"
}

if [ -n "${KERNEL_SRC:-}" ]; then
    [ -d "$KERNEL_SRC" ] || fail "KERNEL_SRC=$KERNEL_SRC does not exist"
    v=$(check_tree "$KERNEL_SRC") || fail "$KERNEL_SRC does not look like a kernel tree"
    echo "==> using $KERNEL_SRC (Linux $v)"
    case "$v" in
        6.12.*) : ;;
        *) echo "warning: this port is written against ${KERNEL_TAG#v}.x, not $v." >&2
           echo "         Patches are applied by anchor match and a missed anchor is" >&2
           echo "         SKIPPED, not reported.  Expect the build's own symbol and" >&2
           echo "         stamp checks to catch it -- or not." >&2 ;;
    esac
    mkdir -p "$ROOT/build"
    # A symlink, not a copy: the tree is ~1.5 GB and the build writes into it.
    [ -e "$TREE" ] && [ ! -L "$TREE" ] && fail "$TREE exists and is not a symlink; move it aside first"
    ln -sfn "$KERNEL_SRC" "$TREE"
    echo "==> build/linux -> $KERNEL_SRC"
    exit 0
fi

if v=$(check_tree "$TREE" 2>/dev/null); then
    echo "==> build/linux already present (Linux $v)"
    exit 0
fi

echo "==> cloning $KERNEL_URL at $KERNEL_TAG into build/linux"
echo "    (shallow: one commit, no history -- still a few hundred MB)"
mkdir -p "$ROOT/build"
git clone --depth 1 --branch "$KERNEL_TAG" "$KERNEL_URL" "$TREE"
v=$(check_tree "$TREE") || fail "clone produced something that is not a kernel tree"
echo "==> done: Linux $v at $TREE"
