#!/usr/bin/env bash
#
# kDFU: from the iPad's own jailbroken iOS into pwned DFU, with no extra
# hardware -- what ./cascadia a5 flash --kdfu runs first.
#
#   bash tools/kdfu.sh                         iPad booted into its jailbroken iOS, on USB
#   IOS_HOST=192.168.1.20 bash tools/kdfu.sh   the same over Wi-Fi
#
# kloader (winocm's, from ios-kexec-utils) runs on the iPad as root and boots
# a raw iBSS in place of iOS: our patched one (build/firmware/iBSS.patched,
# ./cascadia a5 firmware), whose signature checks are off, so the iBEC that
# follows is accepted.  iOS needs tfp0 for it, which EverPwnage (iOS 8.0-9.3.6
# on A5) gives.  kloader has no licence, so it is not in this repository: it
# is fetched once from Legacy iOS Kit's repository at a pinned commit and
# checked by hash (build/cache/kloader).
#
# The iPad leaves USB when kloader jumps.  It comes back in DFU by itself, or
# after the cable is unplugged and plugged in again, or Home is pressed --
# this waits for it either way.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
IBSS="$ROOT/build/firmware/iBSS.patched"
KLOADER="$ROOT/build/cache/kloader"
KLOADER_URL="https://raw.githubusercontent.com/LukeZGD/Legacy-iOS-Kit/23622f8fe72c09952089d5145a7b008cfcc90d9b/resources/kloader/kloader"
KLOADER_SHA256=bec212b4d99a1e3b9fa4add663fa884a2435045d1e47051415f4e26e197f8132
WAIT="${KDFU_WAIT:-90}"
USB=(python3 "$REPO/lib/appleusb.py")

fail() { echo "error: $*" >&2; exit 1; }
# sha256sum where there is one: shasum is perl, and a perl with a broken
# locale fills the screen with warnings
if command -v sha256sum >/dev/null 2>&1; then sha256() { sha256sum "$1" | awk '{print $1}'; }
else sha256() { shasum -a 256 "$1" | awk '{print $1}'; }; fi

[ -s "$IBSS" ] || fail "no $IBSS -- ./cascadia a5 firmware makes it"
# raw ARM code (a branch at 0), not an img3: kloader copies it as it is
[ "$(od -An -tx1 -N4 "$IBSS" | tr -d ' \n')" = "0e0000ea" ] \
    || fail "$IBSS is not a raw iBSS -- ./cascadia a5 firmware again"

if [ ! -s "$KLOADER" ] || [ "$(sha256 "$KLOADER")" != "$KLOADER_SHA256" ]; then
    echo "==> fetching kloader (once)"
    mkdir -p "$(dirname "$KLOADER")"
    curl -fsSL --retry 3 -o "$KLOADER.part" "$KLOADER_URL" || fail "could not download kloader from $KLOADER_URL"
    [ "$(sha256 "$KLOADER.part")" = "$KLOADER_SHA256" ] || { rm -f "$KLOADER.part"; fail "kloader's hash does not match -- not using it"; }
    mv "$KLOADER.part" "$KLOADER"
fi

. "$ROOT/tools/ios-ssh.sh"
ios_setup kdfu
ios_check_model
# rm before writing and a fresh path: iOS caches a binary's signature by
# vnode, and a file rewritten in place is killed on sight ("Killed: 9")
T="/tmp/cascadia-kdfu.$$"
echo "==> sending kloader and the iBSS"
ios "rm -rf $T && mkdir $T && cat > $T/kloader && chmod 755 $T/kloader" < "$KLOADER"
ios "cat > $T/ibss" < "$IBSS"

echo "==> running kloader -- the iPad's screen goes dark and it leaves USB"
echo "    as soon as the screen goes dark, press Power or Home once"
# In the background: the connection dies with iOS, and ssh would sit on it.
ios "$T/kloader $T/ibss" >"$WORK/kloader.log" 2>&1 &
KPID=$!

told=0
end=$((SECONDS + WAIT))
while [ $SECONDS -lt $end ]; do
    if [ "$("${USB[@]}" mode 2>/dev/null)" = DFU ]; then
        kill "$KPID" 2>/dev/null || true
        echo "==> the iPad is in DFU (kDFU)"
        "${USB[@]}" query 2>/dev/null | grep -E '^(CPID|SRTG|MODE)' | sed 's/^/    /' || true
        exit 0
    fi
    if [ $told = 0 ] && [ $SECONDS -gt $((end - WAIT + 15)) ]; then
        echo "    not back yet: press Power or Home once more, or unplug the cable and plug it in again"
        told=1
    fi
    sleep 1
done
kill "$KPID" 2>/dev/null || true
echo "kloader said:" >&2; sed 's/^/    /' "$WORK/kloader.log" >&2
fail "the iPad did not come back in DFU within $WAIT s.  If iOS is still running,
kloader did not run (above) -- EverPwnage jailbroken this boot?  If the screen is
dark, unplug and replug, then: ./cascadia a5 flash --skip-pwn --ibec build/firmware/iBEC.patched.autogo.plain.dfu"
