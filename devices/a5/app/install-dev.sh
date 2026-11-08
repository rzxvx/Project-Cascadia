#!/usr/bin/env bash
# Development install over USB, with iOS up and unlocked: the package, the boot
# files, and the manifest that names them.  RELEASE=tag records which release
# the boot files came from; without it the update prompt stays off.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/pcless-cablefree/out
DECK=$ROOT/build/app/cascadia.deb
fail() { echo "error: $*" >&2; exit 1; }
. "$ROOT/tools/ios-ssh.sh"
ios_setup appdev
ios_check_model
D=/var/root/cascadia
sha() { openssl sha1 "$1" | sed 's/.*= //'; }

ios "mkdir -p $D; which dpkg uicache; rm -f /kernelcache; cat > /kernelcache" < "$OUT/kernelcache" || fail "kernelcache upload"
ios "cat > $D/kloader && chmod 755 $D/kloader" < "$ROOT/build/cache/kloader" || fail "kloader upload"
ios "cat > $D/ibec" < "$ROOT/build/firmware/iBEC.upgrade.stay" || fail "iBEC upload"
ios "cat > /tmp/cascadia.deb" < "$DECK" || fail "package upload"
ios "dpkg -i /tmp/cascadia.deb 2>&1; rm -f /tmp/cascadia.deb; ls -l /usr/libexec/cascadia-helper" </dev/null | cut -c1-110

ios "cat > $D/manifest && chown root:wheel $D/manifest && chmod 644 $D/manifest" <<MANIFEST_END
$(sha "$OUT/kernelcache") /kernelcache
$(sha "$OUT/ramdisk") /ramdisk
$(sha "$OUT/devicetree") /devicetree
$(sha "$ROOT/build/cache/kloader") $D/kloader
$(sha "$ROOT/build/firmware/iBEC.upgrade.stay") $D/ibec
MANIFEST_END
if [ -n "${RELEASE:-}" ]; then
    ios "cat > $D/release" <<< "$RELEASE"
else
    ios "rm -f $D/release" </dev/null
fi
ios "sync; sleep 40; sync; sleep 5; sync; echo synced" </dev/null || fail "sync"
ios "su mobile -c '/usr/libexec/cascadia-helper status'" </dev/null | cut -c1-600
