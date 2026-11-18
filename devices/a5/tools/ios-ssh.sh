# ssh into the iPad's own jailbroken iOS -- sourced by kdfu.sh, mtcal.sh and
# gpucap.sh, not run.
#
#   . "$ROOT/tools/ios-ssh.sh"
#   ios_setup NAME          a work directory /tmp/cascadia-NAME.*, the
#                           connection settings, cleanup on exit
#   ios COMMAND...          run COMMAND on the iPad (stdin/stdout pass through)
#   ios_check_model         $BOARD_MODEL (default iPad2,5) or fail
#
# Over USB it goes through usbmuxd with lib/usbmux.py as ssh's ProxyCommand:
# no iproxy, no local port.  usbmuxd is part of macOS; on Linux it is the
# usbmuxd package, which starts when an iOS device is plugged in.
# IOS_HOST=<the iPad's IP> (IOS_PORT=, default 22) goes over Wi-Fi instead.
# One ssh connection is shared by every step, so iOS's root password is
# asked for once, and only when a key does not get in: Enter is "alpine",
# iOS's own default; another is typed.  It reaches ssh through SSH_ASKPASS
# (OpenSSH 8.4+), so no sshpass.  The sshd (OpenSSH from
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

IOS_EXTRA=()
ios() {
    ssh -F "$ROOT/tools/mtdump/ssh_config" -o ControlMaster=auto -o ControlPath="$CM" \
        -o ControlPersist=300 -o ConnectTimeout=10 ${IOS_EXTRA[@]+"${IOS_EXTRA[@]}"} \
        "${IOS_SSH[@]}" "$@"
}

# The shared connection, opened once: a key if one gets in, else the password.
ios_connect() {
    echo "==> connecting to iOS"
    IOS_EXTRA=(-o BatchMode=yes -o PreferredAuthentications=publickey)
    if ios true </dev/null 2>/dev/null; then IOS_EXTRA=(); return 0; fi
    printf '#!/bin/sh\nprintf "%%s\\n" "$CASCADIA_IOS_PW"\n' > "$WORK/askpass"
    chmod 700 "$WORK/askpass"
    IOS_EXTRA=(-o NumberOfPasswordPrompts=1 -o PreferredAuthentications=keyboard-interactive,password)
    local n=0 pw
    while [ $n -lt 3 ]; do
        pw=
        if [ -t 0 ]; then
            read -r -s -p "    iOS root password [Enter = alpine]: " pw || pw=
            echo
        fi
        if CASCADIA_IOS_PW="${pw:-alpine}" SSH_ASKPASS="$WORK/askpass" SSH_ASKPASS_REQUIRE=force \
                ios true </dev/null 2>"$WORK/ssh.err"; then
            IOS_EXTRA=(); return 0
        fi
        grep -q -i 'permission denied' "$WORK/ssh.err" || break      # not the password
        echo "    not that one"
        [ -t 0 ] || break
        n=$((n + 1))
    done
    IOS_EXTRA=()
    sed 's/^/    /' "$WORK/ssh.err" >&2
    return 1
}

ios_check_model() {
    ios_connect \
        || fail "no ssh to iOS -- is the iPad in iOS, unlocked, with OpenSSH installed (Cydia)?"
    IOS_MODEL=$(ios uname -m) \
        || fail "no ssh to iOS -- is the iPad in iOS, unlocked, with OpenSSH installed (Cydia)?"
    # BOARD_MODEL: the board ./cascadia was asked for (iPad2,1 for k93)
    [ "$IOS_MODEL" = "${BOARD_MODEL:-iPad2,5}" ] \
        || fail "this is a $IOS_MODEL; this build is for ${BOARD_MODEL:-iPad2,5} (./cascadia k93 ... for an iPad 2 Wi-Fi, ./cascadia a5 ... for an iPad mini 1)"
    echo "    $IOS_MODEL"
}
