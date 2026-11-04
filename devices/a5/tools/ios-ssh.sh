# ssh into the iPad's own jailbroken iOS -- sourced by kdfu.sh, mtcal.sh and
# gpucap.sh, not run.
#
#   . "$ROOT/tools/ios-ssh.sh"
#   ios_setup NAME          a work directory /tmp/cascadia-NAME.*, the
#                           connection settings, cleanup on exit
#   ios COMMAND...          run COMMAND on the iPad (stdin/stdout pass through)
#   ios_check_model         iPad2,5 or fail
#
# Over USB it goes through usbmuxd with lib/usbmux.py as ssh's ProxyCommand:
# no iproxy, no local port.  usbmuxd is part of macOS; on Linux it is the
# usbmuxd package, which starts when an iOS device is plugged in.
# IOS_HOST=<the iPad's IP> (IOS_PORT=, default 22) goes over Wi-Fi instead.
# One ssh connection is shared by every step, so iOS's root password is
# asked for once -- "alpine" unless it was changed.  The sshd (OpenSSH from
# Cydia) has only ssh-rsa host keys: tools/mtdump/ssh_config allows them.

IOS_REPO="$(cd "$ROOT/../.." && pwd)"

ios_setup() {
    # /tmp and not $TMPDIR: the ssh control socket's path has to fit in a
    # sockaddr_un, and macOS's TMPDIR alone takes half of it.
    WORK=$(mktemp -d "/tmp/cascadia-$1.XXXXXX")
    CM="$WORK/cm"
    trap ios_cleanup EXIT
    if [ -n "${IOS_HOST:-}" ]; then
        IOS_SSH=(-p "${IOS_PORT:-22}" "root@$IOS_HOST")
        echo "==> iOS at $IOS_HOST:${IOS_PORT:-22}"
    else
        if [ "$(uname -s)" = Linux ] && ! pgrep -x usbmuxd >/dev/null 2>&1; then
            fail "usbmuxd is not running.  It starts by itself when an iOS device is plugged in
once the usbmuxd package is installed (Arch/Fedora: usbmuxd, Debian/Ubuntu:
usbmuxd); or: sudo systemctl start usbmuxd.  Or over Wi-Fi: IOS_HOST=<the iPad's IP>"
        fi
        python3 "$IOS_REPO/lib/usbmux.py" list >/dev/null \
            || fail "usbmuxd does not answer (above).  Or over Wi-Fi: IOS_HOST=<the iPad's IP>"
        [ -n "$(python3 "$IOS_REPO/lib/usbmux.py" list)" ] \
            || fail "no iOS device on USB: the iPad booted into iOS, unlocked, on the cable?
(Or over Wi-Fi: IOS_HOST=<the iPad's IP>)"
        IOS_SSH=(-o "ProxyCommand=python3 '$IOS_REPO/lib/usbmux.py' connect ${IOS_PORT:-22}" root@ios)
        echo "==> iOS over USB (usbmuxd)"
    fi
}

ios_cleanup() {
    ssh -o ControlPath="$CM" -O exit x 2>/dev/null || true
    rm -rf "$WORK"
}

ios() {
    ssh -F "$ROOT/tools/mtdump/ssh_config" -o ControlMaster=auto -o ControlPath="$CM" \
        -o ControlPersist=300 -o ConnectTimeout=10 "${IOS_SSH[@]}" "$@"
}

ios_check_model() {
    echo "==> connecting (iOS root password, 'alpine' unless changed)"
    IOS_MODEL=$(ios uname -m) \
        || fail "no ssh to iOS -- is the iPad in iOS, unlocked, with OpenSSH installed (Cydia)?"
    [ "$IOS_MODEL" = "iPad2,5" ] || fail "this is a $IOS_MODEL; this port is for iPad2,5 (p105ap)"
    echo "    $IOS_MODEL"
}
