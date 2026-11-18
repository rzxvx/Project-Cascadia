# Runs ON THE iPad (or, unchanged, inside the root ./cascadia a5 desktop-root
# builds), after tools/desktop/labwc/build.sh install:
#   ssh root@10.55.0.2 sh -s < tools/desktop/labwc/labwc-setup.sh
# then, on the glass or over ssh:
#   desktop-labwc
#
# The Wayland desktop: labwc, a stacking compositor on wlroots, drawing
# with the GPU (GLES2 through our Mesa) straight onto KMS -- no X server
# between the programs and the screen, one process composing.  X programs
# still run, through Xwayland, which labwc starts only when the first one
# connects.
#
# What each piece is for:
#   labwc            the compositor and window manager (0.20.3 from
#                    tools/desktop/labwc/build.sh: Alpine's 0.20.0 leaves the
#                    screen black with no hardware cursor)
#   seatd            gives labwc the GPU and the input devices
#   waybar           the panel: the apps, the open windows, the keyboard and
#                    rotation buttons, battery, clock
#   wvkbd            the on-screen keyboard (a layer of the compositor's, so
#                    it never covers the window being typed into)
#   fuzzel           the apps' list, from the panel's button
#   foot             the terminal
#   swaybg           the background: one colour (a single-pixel buffer the
#                    compositor scales: no picture to draw)
#   wlr-randr        the screen's turn (rotate, autorotate: tools/desktop/
#                    install.sh, run after this)
#   grim             screenshots
set -e

apk add --no-progress labwc seatd xwayland waybar wvkbd fuzzel foot swaybg \
    wlr-randr grim font-dejavu font-awesome dbus adwaita-icon-theme

mkdir -p /etc/xdg/labwc /etc/xdg/waybar-cascadia /etc/xdg/fuzzel /usr/local/bin

# --- labwc ---------------------------------------------------------------
# No shadows, no rounded corners: each costs the compositor a pass over
# every window it draws.  The touch screen sends touches (not a pretend
# mouse) to the programs, which scroll and press as their toolkits do;
# where there is no program -- the title bars, the background -- labwc
# takes them as clicks.
cat > /etc/xdg/labwc/rc.xml <<'END'
<?xml version="1.0"?>
<labwc_config>
  <core>
    <decoration>server</decoration>
    <gap>0</gap>
  </core>
  <placement>
    <policy>center</policy>
  </placement>
  <theme>
    <name>Cascadia</name>
    <cornerRadius>0</cornerRadius>
    <dropShadows>no</dropShadows>
    <font place="ActiveWindow"><name>DejaVu Sans</name><size>12</size><weight>bold</weight></font>
    <font place="InactiveWindow"><name>DejaVu Sans</name><size>12</size></font>
    <font place="MenuItem"><name>DejaVu Sans</name><size>14</size></font>
    <font place="OnScreenDisplay"><name>DejaVu Sans</name><size>13</size></font>
  </theme>
  <touch deviceName="S5L8940X Capacitive TouchScreen" mapToOutput="Unknown-1" mouseEmulation="no"/>
</labwc_config>
END

# Title bars a finger can hit: labwc's own theme, its buttons and padding
# larger.
mkdir -p /usr/share/themes/Cascadia/openbox-3
cat > /usr/share/themes/Cascadia/openbox-3/themerc <<'END'
padding.height: 8
window.button.width: 34
window.button.spacing: 4
window.active.title.bg.color: #1d323f
window.inactive.title.bg.color: #3a4a55
window.active.label.text.color: #ffffff
window.inactive.label.text.color: #c8d0d6
window.active.button.unpressed.image.color: #ffffff
window.inactive.button.unpressed.image.color: #c8d0d6
window.active.border.color: #1d323f
window.inactive.border.color: #3a4a55
border.width: 1
menu.items.padding.y: 10
END

# A tap on the background opens this menu (labwc's root menu)
cat > /etc/xdg/labwc/menu.xml <<'END'
<?xml version="1.0" encoding="UTF-8"?>
<openbox_menu>
  <menu id="root-menu" label="">
    <item label="Apps"><action name="Execute" command="fuzzel"/></item>
    <item label="Terminal"><action name="Execute" command="foot"/></item>
    <item label="Files"><action name="Execute" command="thunar"/></item>
    <item label="Keyboard"><action name="Execute" command="kbd-toggle"/></item>
    <separator/>
    <item label="Reconfigure"><action name="Reconfigure"/></item>
    <item label="Log out"><action name="Exit"/></item>
  </menu>
</openbox_menu>
END

cat > /etc/xdg/labwc/environment <<'END'
XCURSOR_SIZE=24
XKB_DEFAULT_LAYOUT=us
END

cat > /etc/xdg/labwc/autostart <<'END'
swaybg -c '#1d323f' >/dev/null 2>&1 &
waybar -c /etc/xdg/waybar-cascadia/config.jsonc -s /etc/xdg/waybar-cascadia/style.css >/tmp/waybar.log 2>&1 &
wvkbd-mobintl --hidden -H 300 -L 220 >/tmp/wvkbd.log 2>&1 &
[ -x /usr/local/bin/autorotate ] && autorotate >/tmp/autorotate.log 2>&1 &
END

# --- the panel -------------------------------------------------------------
cat > /etc/xdg/waybar-cascadia/config.jsonc <<'END'
{
    "layer": "top",
    "position": "top",
    "height": 40,
    "spacing": 0,
    "modules-left": ["custom/apps", "wlr/taskbar"],
    "modules-right": ["custom/kbd", "custom/rotate", "battery", "clock"],
    "custom/apps": {
        "format": "",
        "tooltip": false,
        "on-click": "pkill -x fuzzel || fuzzel"
    },
    "wlr/taskbar": {
        "format": "{title}",
        "max-length": 18,
        "tooltip": false,
        "on-click": "activate",
        "on-click-middle": "close"
    },
    "custom/kbd": {
        "format": "",
        "tooltip": false,
        "on-click": "kbd-toggle"
    },
    "custom/rotate": {
        "exec": "if [ -e /run/rotation-lock ]; then echo \uf023; else echo \uf2f1; fi",
        "interval": 2,
        "tooltip": false,
        "on-click": "rotation-lock toggle",
        "exec-on-event": true
    },
    "battery": {
        "bat": "battery",
        "interval": 30,
        "format": "{icon} {capacity}%",
        "format-charging": " {capacity}%",
        "format-icons": ["", "", "", "", ""],
        "tooltip": false
    },
    "clock": {
        "format": "{:%H:%M}",
        "tooltip": false
    }
}
END

cat > /etc/xdg/waybar-cascadia/style.css <<'END'
* {
    font-family: "DejaVu Sans", "Font Awesome 6 Free";
    font-size: 15px;
    min-height: 0;
    border: none;
    border-radius: 0;
}
window#waybar { background: #1d323f; color: #ffffff; }
button { padding: 0 12px; color: #ffffff; background: transparent; }
#custom-apps, #custom-kbd, #custom-rotate { padding: 0 14px; font-size: 18px; }
#taskbar button { padding: 0 10px; color: #c8d0d6; }
#taskbar button.active { background: #3a4a55; color: #ffffff; }
#battery, #clock { padding: 0 10px; }
END

cat > /etc/xdg/fuzzel/fuzzel.ini <<'END'
[main]
font=DejaVu Sans:size=15
terminal=foot
lines=14
width=36
line-height=34
icons-enabled=no
[colors]
background=1d323fee
text=ffffffff
selection=3a4a55ff
selection-text=ffffffff
border=ffffff40
END

# --- commands --------------------------------------------------------------
# The on-screen keyboard shown or hidden: wvkbd's own signal for it
cat > /usr/local/bin/kbd-toggle <<'END'
#!/bin/sh
# kbd-toggle -- the on-screen keyboard shown or hidden: wvkbd under the
# Wayland desktop (its signal for a turn), svkbd under X.
if pidof labwc >/dev/null && ! pidof Xorg >/dev/null; then
    pkill -RTMIN -x wvkbd-mobintl && exit 0
    exec wvkbd-mobintl -H 300 -L 220 >/tmp/wvkbd.log 2>&1
fi
# X: svkbd, docked at the bottom (-H 20: a key row 1/20 of the screen)
pkill -f '^svkbd-mobile-intl( |$)' && exit 0
exec svkbd-mobile-intl -d -H 20 -fn "DejaVu Sans:size=13"
END
chmod 755 /usr/local/bin/kbd-toggle

cat > /usr/local/bin/desktop-labwc <<'END'
#!/bin/sh
# Start the Wayland desktop (labwc) on the glass, in the background, from
# the glass shell or ssh.  Log: /tmp/labwc.log.
if pidof labwc >/dev/null; then echo "labwc is already running"; exit 1; fi
if pidof Xorg >/dev/null; then echo "X is running (XFCE): log out of it first"; exit 1; fi
# fbkeyboard draws straight onto the framebuffer and would paint over it
pkill -x fbkeyboard 2>/dev/null
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export XDG_RUNTIME_DIR=/tmp/xdg-$(id -u)
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
# the system bus, for the programs that want it (as desktop does for XFCE)
if ! timeout 3 dbus-send --system --type=method_call --print-reply --reply-timeout=2000 \
        --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
    if [ ! -e /run/dbus/dbus.pid ] && [ ! -e /run/dbus/system_bus_socket ]; then
        mkdir -p /run/dbus && dbus-uuidgen --ensure && dbus-daemon --system
    else
        echo "note: the system bus is not answering; starting without it" >&2
    fi
fi
# seatd hands labwc the GPU and the input devices (and the console's VT)
if ! pidof seatd >/dev/null; then
    rm -f /run/seatd.sock
    seatd -g seat >/tmp/seatd.log 2>&1 &
    n=0; while [ ! -S /run/seatd.sock ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
fi
export LIBSEAT_BACKEND=seatd
# the way out from the glass (stage 2 starts it; an image from before it
# did not)
if [ -x /usr/local/bin/task-killer ] && ! pidof task-killer >/dev/null; then
    setsid /usr/local/bin/task-killer >/tmp/task-killer.log 2>&1 </dev/null &
fi
# When the session ends the text console is back, with fbkeyboard as stage
# 2 started it.  setsid: the session outlives the ssh it was started from.
setsid sh -c '
    dbus-run-session labwc >/tmp/labwc.log 2>&1 </dev/null
    if [ -s /run/fbkeyboard.args ] && ! pidof fbkeyboard >/dev/null; then
        exec fbkeyboard $(cat /run/fbkeyboard.args) >/tmp/fbkeyboard.log 2>&1 </dev/null
    fi
' </dev/null >/dev/null 2>&1 &
echo "labwc is starting -- a few seconds (log: /tmp/labwc.log)"
END
chmod 755 /usr/local/bin/desktop-labwc

echo "labwc desktop set up: desktop-labwc starts it"
