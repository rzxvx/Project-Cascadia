#!/usr/bin/env bash
#
# Apply patches/tree/*.patch to the kernel source.
#
#   bash scripts/apply-kernel-edits.sh --tree build/linux
#   bash scripts/apply-kernel-edits.sh --tree build/linux --revert
#
# These are the edits to files that already exist in the kernel.  Whole new
# files are copied by apply-kernel-patches.py instead; the two mechanisms do
# not overlap.
#
# Why a patch and not anchor matching, which is what this replaces: an anchor
# that no longer matches is skipped silently, and the one it skipped first was
# the apple_aic1_rearm() call in init/main.c -- without which nothing in the
# system ever takes an interrupt.  The build then succeeds, boots, and does
# nothing, which is the most expensive kind of failure.  A patch applies or
# explains itself.  That is only possible because the kernel is pinned
# (scripts/fetch-kernel.sh); with a floating version, anchors were the lesser
# evil.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/linux"
REVERT=0

while [ $# -gt 0 ]; do
    case "$1" in
        --tree) TREE="$2"; shift 2 ;;
        --revert) REVERT=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

fail() { echo "error: $*" >&2; exit 1; }
[ -f "$TREE/Makefile" ] || fail "$TREE does not look like a kernel tree"
TREE="$(cd "$TREE" && pwd)"     # git -C would resolve a relative record path inside it

shopt -s nullglob
patches=("$ROOT"/patches/tree/*.patch)
[ ${#patches[@]} -gt 0 ] || fail "no patches in $ROOT/patches/tree/"

# git inside the build container may see this repository as owned by someone
# else (Docker Desktop maps ownership, a Linux daemon runs us as the caller),
# and refuses to read a repository it thinks is not ours.
rgit() { git -c safe.directory='*' -C "$ROOT" "$@"; }

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

# Patches are stacked: a later one may change lines next to an earlier one's,
# and then the earlier one's reverse check fails although it is in -- the
# loop below would take it for missing and stop.  So: when every patch is in
# exactly as it is now (its record says so), touch nothing; otherwise take out
# every recorded patch, newest first, and apply the whole set again in order.
if [ "$REVERT" = 0 ]; then
    unchanged=1
    for p in "${patches[@]}"; do
        cmp -s "$p" "$TREE/.cascadia-applied-$(basename "$p")" || { unchanged=0; break; }
    done
    for rec in "$TREE"/.cascadia-applied-*.patch; do
        [ -e "$ROOT/patches/tree/${rec##*/.cascadia-applied-}" ] || unchanged=0
    done
    if [ "$unchanged" = 1 ]; then
        echo "  all ${#patches[@]} patches already applied"
        exit 0
    fi
    recs=("$TREE"/.cascadia-applied-*.patch)
    for (( i = ${#recs[@]} - 1; i >= 0; i-- )); do
        rec=${recs[$i]}
        if git -C "$TREE" apply --reverse --check "$rec" 2>/dev/null; then
            git -C "$TREE" apply --reverse "$rec"
            echo "  took out ${rec##*/.cascadia-applied-} (to apply the set again)"
            rm -f "$rec"
        else
            echo "error: ${rec##*/.cascadia-applied-} is recorded as applied but does not come out" >&2
            git -C "$TREE" apply --reverse --check "$rec" >&2 || true
            exit 1
        fi
    done
fi

for p in "${patches[@]}"; do
    name=$(basename "$p")
    # The version this script last applied, kept in the tree it applied it to.
    record="$TREE/.cascadia-applied-$name"
    if [ "$REVERT" = 1 ]; then
        if git -C "$TREE" apply --reverse --check "$p" 2>/dev/null; then
            git -C "$TREE" apply --reverse "$p"; echo "  reverted $name"
            rm -f "$record"
        else
            echo "  $name was not applied"
        fi
        continue
    fi

    # Already applied?  Reverse-check is the standard way to ask, and it makes
    # the build idempotent instead of failing on the second run.
    if git -C "$TREE" apply --reverse --check "$p" 2>/dev/null; then
        echo "  already applied: $name"
        cp "$p" "$record"
        continue
    fi

    # Not applied as it stands.  If an OLDER version of it is, take that out
    # first.  This patch changes as the port does, and without this a tree
    # patched by last week's version refuses this week's -- the only way out
    # being a 2.5 GB re-fetch of the kernel.  The record says which version
    # went in; a tree patched before records existed is matched against this
    # repository's own history of the file instead.
    if ! git -C "$TREE" apply --check "$p" 2>/dev/null; then
        old=""
        if [ -f "$record" ] && git -C "$TREE" apply --reverse --check "$record" 2>/dev/null; then
            old="$record"
        elif rgit rev-parse --git-dir >/dev/null 2>&1; then
            for rev in $(rgit log --format=%H -- "patches/tree/$name" 2>/dev/null); do
                rgit show "$rev:patches/tree/$name" > "$tmp" 2>/dev/null || continue
                if git -C "$TREE" apply --reverse --check "$tmp" 2>/dev/null; then
                    old="$tmp"
                    echo "  the tree carries $name as of ${rev:0:7}"
                    break
                fi
            done
        fi
        if [ -n "$old" ]; then
            git -C "$TREE" apply --reverse "$old"
            echo "  reverted the older $name"
        fi
    fi

    if ! git -C "$TREE" apply --check "$p" 2>/dev/null; then
        echo "error: $name does not apply to $TREE" >&2
        echo >&2
        git -C "$TREE" apply --check "$p" >&2 || true
        echo >&2
        echo "This patch is generated against the pinned kernel version." >&2
        echo "If your tree is a different version, re-fetch it:" >&2
        echo "    rm -rf build/linux && ./cascadia a5 kernel" >&2
        exit 1
    fi
    git -C "$TREE" apply "$p"
    cp "$p" "$record"
    echo "  applied $name"
done
