# Runs ON THE iPad, NFS root and ./cascadia net on:
#   ssh root@10.55.0.2 sh -s < tools/desktop/xfce-setup.sh
# then, on the glass or over ssh:
#   desktop
#
# XFCE with touch, as walked on the iPad on 2026-09-25 -- all software
# rendered on one core, since the GPU has no Linux driver.  About 280
# packages, ~490 MB on the NFS root, ~6 minutes over the cable.  Running it
# again changes nothing that is already right.
#
# What each piece is for:
#   xorg-server, xf86-video-fbdev   X on /dev/fb0 -- there is no DRM driver
#   xf86-input-libinput, eudev      touch.  Xorg finds input devices through
#                                   udev only; stage 2 starts eudev at boot
#                                   when it is installed.  libinput keeps the
#                                   touchscreen multitouch, so GTK can scroll
#                                   by swiping
#   touchegg                        a two-finger tap is a right click: libinput
#                                   has no long-press for touchscreens, and
#                                   evdev's EmulateThirdButton never runs for
#                                   one that reports multitouch axes -- X
#                                   emulates the pointer from touches itself
#   svkbd                           the on-screen keyboard inside X (fbkeyboard
#                                   draws straight onto the framebuffer, over
#                                   X).  A button on the top panel shows and
#                                   hides it.  Ctrl is the "^" key
#   xinit                           startxfce4 needs it and nothing pulls it in
#
# Not used, although older recipes have them: setup-xorg-base (alpine-conf,
# which also drags in openrc), xf86-video-vesa (x86 only; for armv7 it does not
# exist, and one missing name fails the whole apk add), xf86-input-evdev (see
# touchegg above).
set -e

apk add --no-progress xorg-server xf86-video-fbdev xf86-input-libinput eudev \
    xinit xfce4 xfce4-terminal svkbd touchegg

mkdir -p /etc/X11/xorg.conf.d /usr/local/bin /usr/share/applications \
         /usr/share/icons/hicolor/scalable/apps /etc/xdg/autostart

cat > /etc/X11/xorg.conf.d/99-fbdev.conf <<'END'
Section "Device"
    Identifier "Framebuffer Card"
    Driver     "fbdev"
    Option     "fbdev" "/dev/fb0"
EndSection

Section "Screen"
    Identifier "Framebuffer Screen"
    Device     "Framebuffer Card"
EndSection
END

# A touchscreen class that forces evdev (an earlier recipe had one) takes the
# touchscreen away from libinput, and with it multitouch and touchegg.
for f in /etc/X11/xorg.conf.d/*.conf; do
    if grep -qs 'S5L8940X' "$f" && grep -qs '"evdev"' "$f"; then
        mv "$f" "$f.disabled"
        echo "note: $f forced evdev on the touchscreen; moved to $f.disabled"
    fi
done

cat > /usr/local/bin/kbd-toggle <<'END'
#!/bin/sh
# Show or hide the on-screen keyboard in X: svkbd, docked at the bottom.
# -H 20: one key row is 1/20 of the screen height (~51 px, ~8 mm on this
# 163 ppi panel) -- the default took over a third of the screen.
# pkill -f, not -x: the kernel keeps 15 characters of a process name, and
# "svkbd-mobile-intl" is 17, so -x never matches it.
pkill -f '^svkbd-mobile-intl( |$)' && exit 0
exec svkbd-mobile-intl -d -H 20 -fn "DejaVu Sans:size=13"
END

cat > /usr/local/bin/desktop <<'END'
#!/bin/sh
# Start XFCE on the glass, in the background, from the glass shell or ssh.
if pidof Xorg >/dev/null; then echo "X is already running"; exit 1; fi
# fbkeyboard draws straight onto the framebuffer and would paint over X.
pkill -x fbkeyboard 2>/dev/null
# The session's PATH is whatever started it; ssh's lacks /usr/local/bin,
# where kbd-toggle lives.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export XDG_RUNTIME_DIR=/tmp/xdg-$(id -u)
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
# The system bus: some XFCE parts ask for it.  Nothing in this boot starts one.
if [ ! -S /run/dbus/system_bus_socket ]; then
    mkdir -p /run/dbus
    dbus-uuidgen --ensure
    dbus-daemon --system
fi
# touchegg is two halves: this daemon reads the touchscreen, the client in
# the session (XFCE autostart) turns gestures into clicks.
pidof touchegg >/dev/null || nohup touchegg --daemon >/tmp/touchegg.log 2>&1 </dev/null &
# A solid background unless one was chosen already.  XFCE 4.20's default
# wallpaper is an SVG, and rasterising it on this one core kept the screen
# black for over a minute at every login; a colour takes a second.  Written
# as xfconf's own file before the session starts, so even the first login is
# spared.  The monitor is "default": fbdev has no RandR outputs, and X names
# the one it makes up "default".
x="$HOME/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml"
if [ ! -e "$x" ]; then
    mkdir -p "$(dirname "$x")"
    cat > "$x" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitordefault" type="empty">
        <property name="workspace0" type="empty">
          <property name="color-style" type="int" value="0"/>
          <property name="image-style" type="int" value="0"/>
          <property name="rgba1" type="array">
            <value type="double" value="0.114"/>
            <value type="double" value="0.196"/>
            <value type="double" value="0.247"/>
            <value type="double" value="1"/>
          </property>
        </property>
      </property>
    </property>
  </property>
</channel>
XML
fi
nohup startxfce4 >/tmp/desktop.log 2>&1 </dev/null &
echo "XFCE is starting -- about half a minute; the first start takes longer (log: /tmp/desktop.log)"
END

cat > /usr/local/bin/cascadia-xfce-firstrun <<'END'
#!/bin/sh
# XFCE autostart, for each user until it has done its work: the keyboard
# toggle on the top panel, next to the clock.  On a first login the panel
# writes its default layout into xfconf while this runs, so it waits for that.
[ -n "$(ls "$HOME"/.config/xfce4/panel/launcher-*/kbd-toggle.desktop 2>/dev/null)" ] && exit 0
n=0
until ids=$(xfconf-query -c xfce4-panel -p /panels/panel-1/plugin-ids 2>/dev/null | tail -n +3) && [ -n "$ids" ]; do
    n=$((n + 1)); [ $n -gt 30 ] && exit 0
    sleep 1
done
max=0
for k in $(xfconf-query -c xfce4-panel -l | sed -n 's|^/plugins/plugin-\([0-9]*\)$|\1|p'); do
    [ "$k" -gt "$max" ] && max=$k
done
new=$((max + 1))
mkdir -p "$HOME/.config/xfce4/panel/launcher-$new"
cp /usr/share/applications/kbd-toggle.desktop "$HOME/.config/xfce4/panel/launcher-$new/"
xfconf-query -c xfce4-panel -p /plugins/plugin-$new -n -t string -s launcher
xfconf-query -c xfce4-panel -p /plugins/plugin-$new/items -n -a -t string -s kbd-toggle.desktop
args=""
placed=""
for id in $ids; do
    if [ -z "$placed" ] && [ "$(xfconf-query -c xfce4-panel -p /plugins/plugin-$id)" = clock ]; then
        args="$args -t int -s $new"; placed=1
    fi
    args="$args -t int -s $id"
done
[ -n "$placed" ] || args="$args -t int -s $new"
# shellcheck disable=SC2086
xfconf-query -c xfce4-panel -p /panels/panel-1/plugin-ids $args
xfce4-panel -r
END
chmod 755 /usr/local/bin/kbd-toggle /usr/local/bin/desktop /usr/local/bin/cascadia-xfce-firstrun
# The first version of this script called it cascadia-panel-kbd.
rm -f /usr/local/bin/cascadia-panel-kbd /etc/xdg/autostart/cascadia-panel-kbd.desktop

cat > /usr/share/icons/hicolor/scalable/apps/cascadia-keyboard.svg <<'END'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">
  <rect x="1.5" y="5" width="21" height="14" rx="2" fill="none" stroke="#e8e8e8" stroke-width="1.6"/>
  <g fill="#e8e8e8">
    <rect x="4" y="8" width="2.4" height="2.2"/><rect x="7.6" y="8" width="2.4" height="2.2"/>
    <rect x="11.2" y="8" width="2.4" height="2.2"/><rect x="14.8" y="8" width="2.4" height="2.2"/>
    <rect x="18.4" y="8" width="1.8" height="2.2"/>
    <rect x="4" y="11.6" width="2.4" height="2.2"/><rect x="7.6" y="11.6" width="2.4" height="2.2"/>
    <rect x="11.2" y="11.6" width="2.4" height="2.2"/><rect x="14.8" y="11.6" width="2.4" height="2.2"/>
    <rect x="18.4" y="11.6" width="1.8" height="2.2"/>
    <rect x="7" y="15.2" width="10" height="2"/>
  </g>
</svg>
END
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true

# Exec by absolute path: see PATH in desktop above.
cat > /usr/share/applications/kbd-toggle.desktop <<'END'
[Desktop Entry]
Type=Application
Name=Keyboard
Comment=Show or hide the on-screen keyboard
Exec=/usr/local/bin/kbd-toggle
Icon=cascadia-keyboard
Terminal=false
Categories=Utility;Accessibility;
END

cat > /etc/xdg/autostart/cascadia-xfce-firstrun.desktop <<'END'
[Desktop Entry]
Type=Application
Name=Cascadia: keyboard button on the panel
Exec=/usr/local/bin/cascadia-xfce-firstrun
OnlyShowIn=XFCE;
NoDisplay=true
END

# udev has to be running before X starts.  Stage 2 starts it at boot once
# eudev is installed; on the boot that installed it, nobody has yet.
if ! pidof udevd >/dev/null; then
    udevd --daemon
    udevadm trigger --action=add
    udevadm settle -t 10 || true
fi

echo
echo "==> done.  Start it with:  desktop"
echo "    tap = click, two-finger tap = right click, swipe = scroll;"
echo "    the keyboard button sits next to the clock"
