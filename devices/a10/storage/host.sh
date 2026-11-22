#!/usr/bin/env bash
# host.sh - ./cascadia a10 storage: drive cascadia-storage on the iPad's own
# iPadOS over SSH, through usbmux (iproxy). Runs on the Mac.
#
#   ./cascadia a10 storage inspect     read-only: GPT, APFS container, the
#                                      smallest size APFS can shrink to
#   ./cascadia a10 storage linux [GB]  give Linux a partition of GB gigabytes
#                                      (asked; Enter = 10, at most 14
#                                      recommended): APFS shrunk by iPadOS
#                                      itself, then a "Linux" GPT entry.
#                                      Asks for your backup and a typed
#                                      RESIZE first; nothing is formatted.
#
# Required from you: the iPad booted into a jailbroken iPadOS (palera1n,
# Dopamine, ...) with OpenSSH installed. Cascadia does not jailbreak it.
# The helper runs as root through sudo: you type the SSH password and the
# sudo password once per run (one shared SSH connection; the sudo password
# is passed to sudo -S, never stored).
#   SSH_USER=mobile   the user to log in as (root: no sudo)
#   SSH_PORT=22       the iPad's SSH port
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
HELPER="$A10/build/storage/cascadia-storage"
LPORT=${LPORT:-2222}
SSH_USER=${SSH_USER:-mobile}
SSH_PORT=${SSH_PORT:-22}

say()  { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || fail "this runs on the Mac"
for t in iproxy idevice_id ideviceinfo ssh; do
    command -v "$t" >/dev/null || fail "missing $t (brew install libimobiledevice)"
done
[ -x "$HELPER" ] || bash "$HERE/build.sh" >/dev/null

need_jb="boot the iPad into a jailbroken iPadOS (palera1n, Dopamine, ...) with OpenSSH"
udid=$(idevice_id -l | head -1)
[ -n "$udid" ] || fail "no iPad in iPadOS over USB -- $need_jb"
info() { ideviceinfo -s -u "$udid" -k "$1"; }
case "$(info ProductType)" in
    iPad7,11|iPad7,12) ;;
    *) fail "$(info ProductType) is not an iPad 7" ;;
esac
say "$(info ProductType), iPadOS $(info ProductVersion) ($(info BuildVersion))"

# the iPad's SSH, forwarded to localhost; one shared connection, so the
# password is asked once
iproxy -u "$udid" "$LPORT:$SSH_PORT" >/dev/null 2>&1 &
iproxy_pid=$!
CTL="${TMPDIR:-/tmp}/cascadia-a10-ssh-$$"
cleanup() {
    ssh -o ControlPath="$CTL" -O exit "$SSH_USER@127.0.0.1" >/dev/null 2>&1 || true
    kill "$iproxy_pid" 2>/dev/null || true
}
trap cleanup EXIT
sleep 1
SSHOPTS=(-p "$LPORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
         -o LogLevel=ERROR -o ConnectTimeout=8
         -o ControlMaster=auto -o ControlPath="$CTL" -o ControlPersist=120)
ssh "${SSHOPTS[@]}" "$SSH_USER@127.0.0.1" true </dev/null \
    || fail "no SSH as $SSH_USER on the iPad's port $SSH_PORT -- $need_jb"
say "ssh $SSH_USER on the iPad's port $SSH_PORT"
run() { ssh "${SSHOPTS[@]}" "$SSH_USER@127.0.0.1" "$@"; }

SUDO=; [ "$SSH_USER" = root ] || SUDO=sudo
DEST=/var/tmp/cascadia-storage
run "cat > $DEST && chmod 755 $DEST" < "$HELPER"

# the helper as root: sudo's password asked once here (not echoed) and given
# to sudo -S on each call -- sudo on iPadOS remembers it per terminal only
SUDOPW=
if [ -n "$SUDO" ]; then
    read -r -s -p "    sudo password for $SSH_USER on the iPad: " SUDOPW || SUDOPW=
    echo
fi
helper() {
    if [ -n "$SUDO" ]; then
        printf '%s\n' "$SUDOPW" | ssh "${SSHOPTS[@]}" "$SSH_USER@127.0.0.1" "sudo -S -p '' $DEST $*"
    else
        ssh "${SSHOPTS[@]}" "$SSH_USER@127.0.0.1" "$DEST $*" </dev/null
    fi
}
gb() { awk -v b="$1" 'BEGIN { printf "%.2f GB", b / 1e9 }'; }

DEFAULT_GB=10
RECOMMENDED_MAX_GB=14

case "${1:-}" in
    inspect)
        say "inspect (read-only)"
        helper inspect
        ;;
    linux)
        want=${2:-}
        if [ -z "$want" ]; then
            read -r -p "    Linux partition size in GB [$DEFAULT_GB] (at most $RECOMMENDED_MAX_GB recommended): " want || want=
            want=${want:-$DEFAULT_GB}
        fi
        case "$want" in ''|*[!0-9]*) fail "a whole number of GB, please" ;; esac
        bytes=$((want * 1000000000))
        say "plan for a $want GB Linux partition (read-only)"
        out=$(helper plan "$bytes" | tr -d '\r')
        printf '%s\n' "$out" | grep -E '^(part|apfs|plan|error)' | sed 's/^/    /'
        printf '%s\n' "$out" | grep -q '^error=' && fail "refused -- nothing was changed"
        apfs=$(printf '%s\n' "$out" | sed -n 's/^plan apfs.bytes=\([0-9]*\).*/\1/p')
        linux=$(printf '%s\n' "$out" | sed -n 's/.* linux.bytes=\([0-9]*\).*/\1/p')
        minimal=$(printf '%s\n' "$out" | sed -n 's/^plan apfs.minimal=\([0-9]*\).*/\1/p')
        echo
        echo "    iPadOS (APFS) keeps $(gb "$apfs")   (its own minimum now: $(gb "$minimal"))"
        echo "    Linux gets           $(gb "$linux")   (a new, unformatted partition)"
        echo
        if [ "$want" -gt "$RECOMMENDED_MAX_GB" ]; then
            echo "    More than $RECOMMENDED_MAX_GB GB leaves iPadOS little room for its caches and updates."
            read -r -p "    Go above the recommended maximum anyway? Type yes: " ans || ans=
            [ "$ans" = yes ] || fail "stopped -- nothing was changed"
        fi
        echo "    This changes the iPad's storage layout. A failure can cost the iPad's data."
        read -r -p "    Have you backed up the iPad (Finder, or idevicebackup2)? [y/N] " ans || ans=
        case "$ans" in [yY]*) ;; *) fail "back up first -- nothing was changed" ;; esac
        read -r -p "    Type RESIZE to shrink APFS and add the Linux partition: " ans || ans=
        [ "$ans" = RESIZE ] || fail "stopped -- nothing was changed"
        say "apply: APFS first, then the GPT (do not unplug or lock the iPad)"
        helper apply "$bytes" --i-have-a-backup
        say "done. Reboot iPadOS before using it, so it sees its smaller partition."
        ;;
    *)
        fail "storage what? inspect, linux [GB]"
        ;;
esac
