#!/bin/sh
# rotation-lock [on|off|toggle] -- the screen's rotation lock: while it is on
# autorotate leaves the screen as it is.  No argument: on or off.  On the
# iPad as /usr/local/bin/rotation-lock, the panel's button runs it with
# toggle, and so does a long press of Home (p105-keys, by itself).  The lock
# is the file /run/rotation-lock: gone at the next boot.
LOCK=/run/rotation-lock
case "${1:-}" in
    "")     if [ -e "$LOCK" ]; then echo on; else echo off; fi ;;
    on)     : > "$LOCK" ;;
    off)    rm -f "$LOCK" ;;
    toggle) if [ -e "$LOCK" ]; then rm -f "$LOCK"; else : > "$LOCK"; fi ;;
    *)      sed -n '2,3p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
