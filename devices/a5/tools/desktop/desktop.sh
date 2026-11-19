#!/bin/sh
# desktop -- the desktop on the glass, started in the background from the
# glass shell or ssh.  On the iPad as /usr/local/bin/desktop
# (tools/desktop/install.sh).
#
#   desktop               the Wayland desktop (labwc: desktop-labwc)
#   desktop auto on|off   start the desktop at boot, or the text console only
#   desktop auto          whether it starts at boot
#   desktop --boot        what stage 2 runs at the end of a boot: the desktop,
#                         unless `desktop auto off` said otherwise
#
# The way back out of a desktop that will not let go: Home pressed three
# times (task-killer).
OFF=/etc/cascadia/desktop-boot-off
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

default() {
    if [ -x /usr/local/bin/desktop-labwc ]; then echo labwc
    elif [ -x /usr/local/bin/desktop-xfce ]; then echo xfce
    fi
}

case "${1:-}" in
    auto)
        case "${2:-}" in
            on)  rm -f "$OFF"; echo "The desktop starts at boot." ;;
            off) mkdir -p "${OFF%/*}" && : > "$OFF" && echo "No desktop at boot: the text console." ;;
            "")  if [ -e "$OFF" ]; then echo off; else echo on; fi ;;
            *)   echo "usage: desktop auto on|off" >&2; exit 2 ;;
        esac
        exit 0 ;;
    --boot)
        [ -e "$OFF" ] && exit 0
        set -- ;;
    -h|--help|help)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac
# (`desktop xfce`, desktop-xfce: the iPad 2's root, whose desktop is still
# XFCE -- tools/desktop/xfce-setup.sh)

which=${1:-$(default)}
case "$which" in
    labwc|wayland)
        [ -x /usr/local/bin/desktop-labwc ] ||
            { echo "the Wayland desktop is not set up (tools/desktop/labwc/labwc-setup.sh)" >&2; exit 1; }
        exec desktop-labwc ;;
    xfce|x|x11)
        [ -x /usr/local/bin/desktop-xfce ] ||
            { echo "XFCE is not set up (tools/desktop/xfce-setup.sh)" >&2; exit 1; }
        exec desktop-xfce ;;
    "")
        echo "no desktop is set up (tools/desktop/labwc/labwc-setup.sh)" >&2; exit 1 ;;
    *)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2 ;;
esac
