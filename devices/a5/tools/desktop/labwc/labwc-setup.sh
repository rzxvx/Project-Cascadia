# Runs ON THE iPad (or, unchanged, inside the root ./cascadia a5 desktop-root
# builds), after tools/desktop/labwc/build.sh install:
#   ssh root@10.55.0.2 sh -s < tools/desktop/labwc/labwc-setup.sh
# then tools/desktop/install.sh (desktop, rotate, autorotate), and on the
# glass or over ssh:
#   desktop                  (desktop-labwc; stage 2 starts it at boot too,
#                             unless `desktop auto off`)
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
#   waybar           the panel, grey and see-through: the apps, the open
#                    windows, the keyboard and rotation buttons, Wi-Fi and
#                    Bluetooth (their menus: wifi-menu, bt-menu), battery,
#                    clock
#   wvkbd            the on-screen keyboard (a layer of the compositor's, so
#                    it never covers the window being typed into)
#   fuzzel           the apps' list, from the panel's button, with their
#                    icons; the Wi-Fi and Bluetooth menus are fuzzel's too
#   foot             the terminal
#   pcmanfm          the files
#   swaybg           the background: a field of stars, drawn here once
#                    (py3-cairo) square at the screen's longer side, so
#                    either turn of the screen is covered
#   wlr-randr        the screen's turn (rotate, autorotate: tools/desktop/
#                    install.sh, run after this)
#   grim             screenshots
set -e

# The image's own packages, baked in without apk (scripts/add-base-packages.sh):
# named in apk's world, so that no `apk del` takes them for unneeded -- one
# took dropbear, Wi-Fi and Bluetooth with what it removed (2026-11-18)
apk add --no-progress dropbear dropbear-scp nfs-utils libtirpc-conf busybox-static iw wpa_supplicant fbkeyboard font-dejavu bluez dbus i2c-tools alsa-utils e2fsprogs eudev

apk add --no-progress labwc seatd xwayland waybar wvkbd fuzzel foot swaybg \
    wlr-randr grim font-dejavu font-awesome dbus adwaita-icon-theme py3-cairo \
    pcmanfm hicolor-icon-theme

mkdir -p /etc/xdg/labwc /etc/xdg/waybar-cascadia /etc/xdg/fuzzel /usr/local/bin \
         /usr/local/share/applications /usr/share/backgrounds/cascadia

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
    <item label="Wi-Fi"><action name="Execute" command="wifi-menu"/></item>
    <item label="Bluetooth"><action name="Execute" command="bt-menu"/></item>
    <item label="Terminal"><action name="Execute" command="foot"/></item>
    <item label="Files"><action name="Execute" command="pcmanfm"/></item>
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
XDG_CURRENT_DESKTOP=labwc:wlroots
END

cat > /etc/xdg/labwc/autostart <<'END'
if [ -f /usr/share/backgrounds/cascadia/stars.png ]; then
    swaybg -i /usr/share/backgrounds/cascadia/stars.png -m fill >/dev/null 2>&1 &
else
    swaybg -c '#0b0e13' >/dev/null 2>&1 &
fi
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
    "modules-right": ["custom/kbd", "custom/rotate", "network", "bluetooth", "battery", "clock"],
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
    "network": {
        "interface": "wlan0",
        "format-wifi": "\uf1eb {essid}",
        "format-ethernet": "\uf1eb",
        "format-linked": "\uf1eb \u2026",
        "format-disconnected": "\uf1eb",
        "format-disabled": "\uf1eb",
        "max-length": 14,
        "tooltip": false,
        "on-click": "wifi-menu"
    },
    "bluetooth": {
        "format": "\uf293",
        "format-off": "\uf293",
        "format-disabled": "\uf293",
        "format-no-controller": "\uf293",
        "format-connected": "\uf293 {num_connections}",
        "tooltip": false,
        "on-click": "bt-menu"
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
window#waybar { background: rgba(46, 50, 56, 0.72); color: #ffffff;
                border-bottom: 1px solid rgba(255, 255, 255, 0.08); }
button { padding: 0 12px; color: #ffffff; background: transparent; }
#custom-apps, #custom-kbd, #custom-rotate { padding: 0 14px; font-size: 18px; }
#taskbar button { padding: 0 10px; color: #c8ced4; }
#taskbar button.active { background: rgba(255, 255, 255, 0.16); color: #ffffff; }
#network, #bluetooth { padding: 0 12px; font-size: 17px; }
#network.disconnected, #network.disabled, #network.linked,
#bluetooth.off, #bluetooth.disabled, #bluetooth.no-controller { color: #7c838b; }
#battery, #clock { padding: 0 10px; }
END

# The apps' list: large rows a finger can pick, the apps' icons, the
# panel's grey.  Sizes in pixels (dpi-aware off: at the panel's 164 dpi the
# font's points came out twice the size).  filter-desktop: other desktops'
# own entries (OnlyShowIn) stay out of it.
cat > /etc/xdg/fuzzel/fuzzel.ini <<'END'
[main]
dpi-aware=no
filter-desktop=yes
font=DejaVu Sans:size=19
prompt="  "
icon-theme=Adwaita
icons-enabled=yes
terminal=foot
lines=10
width=24
horizontal-pad=28
vertical-pad=24
inner-pad=14
line-height=60px
image-size-ratio=0.8
layer=overlay
anchor=center
[colors]
background=2e3238f0
text=e8ecefff
prompt=9aa2aaff
input=ffffffff
match=8ab4f8ff
selection=ffffff26
selection-text=ffffffff
selection-match=8ab4f8ff
border=ffffff1a
[border]
width=1
radius=18
selection-radius=10
END

# Entries that are another one's helper or the panel's own buttons, kept
# out of the list under labwc (NotShowIn)
for d in foot-server footclient pcmanfm-desktop-pref kbd-toggle rotation-lock; do
    f=/usr/share/applications/$d.desktop
    [ -f "$f" ] || continue
    sed '/^NotShowIn=/d; /^\[Desktop Entry\]/a NotShowIn=labwc;' "$f" > /usr/local/share/applications/$d.desktop
done
# pcmanfm as "Files", with Adwaita's folder for its icon, by its path:
# Adwaita 50 has no system-file-manager, the icon its entry names, and
# fuzzel did not find "folder" by name
[ -f /usr/share/applications/pcmanfm.desktop ] &&
    sed 's|^Name=.*|Name=Files|; s|^Icon=.*|Icon=/usr/share/icons/Adwaita/scalable/places/folder.svg|; /^Name\[/d' \
        /usr/share/applications/pcmanfm.desktop \
        > /usr/local/share/applications/pcmanfm.desktop

# --- the background: stars ---------------------------------------------------
# Drawn once, here, from a fixed seed (the same picture every time): many
# small faint stars, a few bright ones with a glow, some tinted blue or gold.
# Square at the screen's longer side: swaybg's fill covers the screen
# however it is turned.
if [ ! -f /usr/share/backgrounds/cascadia/stars.png ]; then
    python3 - <<'STARS'
import cairo, math, random
S = 1024
r = random.Random(0x5ca5)
im = cairo.ImageSurface(cairo.FORMAT_RGB24, S, S)
c = cairo.Context(im)
c.set_source_rgb(0.016, 0.020, 0.031)
c.paint()
for i in range(1500):
    x, y = r.random() * S, r.random() * S
    m = r.random() ** 4.5
    rad = 0.3 + m * 1.5
    a = 0.18 + 0.82 * min(1, m * 1.4 + r.random() * 0.45)
    t = r.random()
    col = (0.80, 0.88, 1.0) if t < 0.18 else (1.0, 0.93, 0.80) if t < 0.28 else (1.0, 1.0, 1.0)
    if m > 0.75:
        g = cairo.RadialGradient(x, y, 0, x, y, rad * 4.5)
        g.add_color_stop_rgba(0, col[0], col[1], col[2], 0.16 * a)
        g.add_color_stop_rgba(1, col[0], col[1], col[2], 0)
        c.set_source(g)
        c.arc(x, y, rad * 4.5, 0, 2 * math.pi)
        c.fill()
    c.set_source_rgba(col[0], col[1], col[2], a)
    c.arc(x, y, rad, 0, 2 * math.pi)
    c.fill()
im.write_to_png("/usr/share/backgrounds/cascadia/stars.png")
STARS
fi

# --- commands --------------------------------------------------------------
# The on-screen keyboard shown or hidden: wvkbd's own signal for it
cat > /usr/local/bin/kbd-toggle <<'END'
#!/bin/sh
# kbd-toggle -- the on-screen keyboard shown or hidden: wvkbd (its signal
# for a turn), started if it is not running.
pkill -RTMIN -x wvkbd-mobintl && exit 0
exec wvkbd-mobintl -H 300 -L 220 >/tmp/wvkbd.log 2>&1
END
chmod 755 /usr/local/bin/kbd-toggle

# Wi-Fi from the panel: the networks in range in a menu (fuzzel), the
# password asked for when the network wants one; `wifi` does the joining
cat > /usr/local/bin/wifi-menu <<'END'
#!/bin/sh
# wifi-menu -- Wi-Fi from the panel (waybar's Wi-Fi icon, or the desktop's
# menu): the networks in range, strongest first, the one joined ticked.  A
# network asks for its password only the first time: `wifi` remembers it.
pkill -x fuzzel && exit 0
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
NETS=/etc/wifi/networks
TAB=$(printf '\t')
hexof() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }
# a line while something takes its time, gone when it is done
busy() { fuzzel --dmenu --prompt-only "$1" </dev/null >/dev/null 2>&1 & BUSY=$!; }
unbusy() { kill "$BUSY" 2>/dev/null; wait "$BUSY" 2>/dev/null; }
say() { printf 'OK\n' | fuzzel --dmenu --lines 1 --prompt "$1  " >/dev/null 2>&1; }
cur=$(iw dev wlan0 link 2>/dev/null | sed -n 's/^[[:space:]]*SSID: //p')
busy "Looking for networks…"
# wifi scan: "-48 dBm  2437 MHz  WPA2  name", strongest first; one line a name
list=$(wifi scan 2>/dev/null | awk -v OFS='\t' '
    { sig = $1; sec = $5; ssid = $0
      sub(/^ *-?[0-9]+ dBm +[0-9]+ MHz +[^ ]+ +/, "", ssid)
      if (ssid == "(hidden)" || ssid == "" || seen[ssid]++) next
      print sig, sec, ssid }')
unbusy
[ -n "$list" ] || { say "No networks in range"; exit 0; }
menu=$(printf '%s\n' "$list" | while IFS="$TAB" read -r sig sec ssid; do
    # the bars all four wide (a low one where the signal is not)
    if   [ "$sig" -ge -55 ]; then b="▂▄▆█"
    elif [ "$sig" -ge -67 ]; then b="▂▄▆▁"
    elif [ "$sig" -ge -78 ]; then b="▂▄▁▁"
    else b="▂▁▁▁"; fi
    lock=""; [ "$sec" != open ] && lock="  · $sec"
    [ "$ssid" = "$cur" ] && lock="$lock  · connected"
    printf '%s   %s%s\n' "$b" "$ssid" "$lock"
done)
[ -n "$cur" ] && menu=$(printf '%s\nDisconnect from %s' "$menu" "$cur")
i=$(printf '%s\n' "$menu" | fuzzel --dmenu --index --minimal-lines --lines 10 --width 30 --prompt "Wi-Fi  ") || exit 0
[ "$i" -ge 0 ] 2>/dev/null || exit 0
n=$(printf '%s\n' "$list" | wc -l)
if [ "$i" -ge "$n" ]; then
    busy "Disconnecting…"; wifi down >/tmp/wifi-menu.log 2>&1; unbusy; exit 0
fi
line=$(printf '%s\n' "$list" | sed -n "$((i + 1))p")
sec=$(printf '%s' "$line" | cut -f2)
ssid=$(printf '%s' "$line" | cut -f3-)
[ "$ssid" = "$cur" ] && exit 0
pass=
if [ "$sec" != open ] && [ ! -f "$NETS/$(hexof "$ssid").conf" ]; then
    pkill -USR2 -x wvkbd-mobintl            # the keyboard up for it
    pass=$(fuzzel --dmenu --password --prompt-only "Password for $ssid:  " </dev/null)
    pkill -USR1 -x wvkbd-mobintl
    [ -n "$pass" ] || exit 0
fi
busy "Joining $ssid…"
if [ -n "$pass" ]; then wifi connect "$ssid" "$pass"; else wifi connect "$ssid"; fi >/tmp/wifi-menu.log 2>&1
r=$?
unbusy
[ $r = 0 ] || say "Could not join $ssid"
END
chmod 755 /usr/local/bin/wifi-menu

# Bluetooth from the panel: on and off, the paired devices connected or
# not, new ones found and paired; `bluetooth` does the work
cat > /usr/local/bin/bt-menu <<'END'
#!/bin/sh
# bt-menu -- Bluetooth from the panel (waybar's Bluetooth icon, or the
# desktop's menu): on and off, the paired devices (a tap connects or
# disconnects), new ones looked for and paired.
pkill -x fuzzel && exit 0
export PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
TAB=$(printf '\t')
busy() { fuzzel --dmenu --prompt-only "$1" </dev/null >/dev/null 2>&1 & BUSY=$!; }
unbusy() { kill "$BUSY" 2>/dev/null; wait "$BUSY" 2>/dev/null; }
say() { printf 'OK\n' | fuzzel --dmenu --lines 1 --prompt "$1  " >/dev/null 2>&1; }
pick() { fuzzel --dmenu --index --minimal-lines --lines 10 --width 30 --prompt "Bluetooth  "; }
ctl() { timeout 6 bluetoothctl "$@" 2>/dev/null; }
connected() { ctl info "$1" | grep -q "Connected: yes"; }
if ! pidof bluetoothd >/dev/null; then
    printf 'Turn Bluetooth on\n' | pick >/dev/null || exit 0
    busy "Starting Bluetooth…"; bluetooth on >/tmp/bt-menu.log 2>&1; unbusy
    exec "$0"
fi
paired=$(ctl devices Paired | awk -v OFS='\t' '$1 == "Device" { a = $2; $1 = $2 = ""; sub(/^ +/, ""); print a, $0 }')
menu=$(printf '%s\n' "$paired" | while IFS="$TAB" read -r a name; do
    [ -n "$a" ] || continue
    if connected "$a"; then printf '● %s  · connected\n' "$name"; else printf '○ %s\n' "$name"; fi
done)
np=0; [ -n "$menu" ] && np=$(printf '%s\n' "$menu" | wc -l)
i=$({ [ -n "$menu" ] && printf '%s\n' "$menu"; printf 'Look for new devices\nTurn Bluetooth off\n'; } | pick) || exit 0
[ "$i" -ge 0 ] 2>/dev/null || exit 0
if [ "$i" -lt "$np" ]; then
    a=$(printf '%s\n' "$paired" | sed -n "$((i + 1))p" | cut -f1)
    if connected "$a"; then
        busy "Disconnecting…"; bluetooth disconnect "$a" >/tmp/bt-menu.log 2>&1; unbusy
    else
        busy "Connecting…"; bluetooth connect "$a" >/tmp/bt-menu.log 2>&1; r=$?; unbusy
        [ $r = 0 ] || say "Could not connect"
    fi
elif [ "$i" -eq "$np" ]; then
    busy "Looking for devices (yours in its pairing mode)…"
    found=$(bluetooth scan 8 2>/dev/null | awk '$1 ~ /^([0-9A-F][0-9A-F]:){5}[0-9A-F][0-9A-F]$/')
    unbusy
    [ -n "$found" ] || { say "Nothing found"; exit 0; }
    j=$(printf '%s\n' "$found" | awk '{ a = $1; $1 = ""; sub(/^ +/, ""); print $0 "  · " a }' | pick) || exit 0
    a=$(printf '%s\n' "$found" | sed -n "$((j + 1))p" | cut -d' ' -f1)
    busy "Pairing…"; bluetooth pair "$a" >/tmp/bt-menu.log 2>&1; r=$?; unbusy
    [ $r = 0 ] || say "Pairing failed"
else
    busy "Turning Bluetooth off…"; bluetooth off >/tmp/bt-menu.log 2>&1; unbusy
fi
END
chmod 755 /usr/local/bin/bt-menu

cat > /usr/local/bin/desktop-labwc <<'END'
#!/bin/sh
# Start the Wayland desktop (labwc) on the glass, in the background, from
# the glass shell or ssh.  Log: /tmp/labwc.log.
if pidof labwc >/dev/null; then echo "labwc is already running"; exit 1; fi
# the console's on-screen keyboard: its arguments, as stage 2 started it,
# for when the session ends (a root's first boot into the desktop may come
# before stage 2 wrote them)
[ -s /run/fbkeyboard.args ] || {
    for n in /sys/class/input/event*; do
        [ "$(cat "$n/device/name" 2>/dev/null)" = "S5L8940X Capacitive TouchScreen" ] &&
            echo "-d /dev/input/${n##*/} -f /usr/share/fonts/dejavu/DejaVuSans.ttf" > /run/fbkeyboard.args && break
    done
}
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

echo "labwc desktop set up: desktop (tools/desktop/install.sh) or desktop-labwc starts it"
