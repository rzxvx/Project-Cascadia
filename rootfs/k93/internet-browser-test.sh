#!/bin/sh
# Explicit single-host HTTPS diagnostic in the disposable RAM overlay.
# Static name mapping isolates TLS/routing from DNS; this is not Wi-Fi proof.
set -eu
target=$1
case "$target" in ''|*[!0-9.]*) exit 1;; esac
test "$(id -u)" = 0
grep -q '^overlay / overlay ' /proc/mounts
test -c /dev/fb0
test -z "$(busybox ip -4 route show default)"
test ! -e /run/k93-browser-online.sh
test "$(busybox sha256sum /k93-browser-test.sh | busybox cut -d ' ' -f 1)" = 9d8657317c0a89f6e57e1c7f8f35ee1329de456df673b98c458cd4a0c3ba55d7
! busybox grep -q 'example.com' /etc/hosts
busybox ip route add default via 10.93.0.1 dev usb0
printf '%s example.com\n' "$target" >>/etc/hosts
# Retain the exact proven UID/sandbox/cookie/process/framebuffer checks.
# Replace the offline test's URL/title and omit its local-page input test.
busybox sed -e 's,file:///k93-browser-test.html,https://example.com/,' \
    -e 's/K93_JS_READY/Example Domain/g' \
    -e '/^echo K93_BROWSER_JS_READY$/,/^echo K93_BROWSER_XTEST_INPUT_PASS$/c\echo K93_BROWSER_HTTPS_READY' \
    /k93-browser-test.sh >/run/k93-browser-online.sh
echo K93_HTTPS_TEST_BEGIN
/bin/sh /run/k93-browser-online.sh
echo K93_HTTPS_TEST_END
