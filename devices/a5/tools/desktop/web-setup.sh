# Runs ON THE iPad (or, unchanged, inside the root ./cascadia a5 desktop-root
# builds on the host -- its `web` step), after youtube-setup.sh and with
# tools/desktop/web/ copied to /usr/share/cascadia/web:
#   tar -C tools/desktop/web -cf - . | ssh root@10.55.0.2 \
#       'mkdir -p /usr/share/cascadia/web && tar -C /usr/share/cascadia/web -xf -'
#   ssh root@10.55.0.2 sh -s < tools/desktop/web-setup.sh
# then, in XFCE: Web in the menu (Internet) or on the desktop, the browser
# links open in, or
#   cascadia-web [URL | WORDS...]
#
# A web browser whose videos play in mpv, from any site, as the YouTube tool
# plays YouTube's.  What each piece is for:
#   cascadia-web   the browser (web/cascadia-web): WebKitGTK, for touch.  In
#                  each page, web/hook.js stops a video the moment it starts
#                  and hands it on
#   cascadia-play  the player (web/cascadia-play): mpv, through yt-dlp, which
#                  knows ~1800 sites, reads HLS and DASH, and picks the stream
#                  this iPad decodes in real time (ytdl-format in
#                  /etc/mpv/mpv.conf, youtube-setup.sh's); the page's own
#                  video address when yt-dlp knows nothing of the page
#   GStreamer's    for WebKit to say it plays MP4 and H.264 at all: sites
#   good and libav show no play button to a browser that says it does not
#   plugins        (nothing is decoded there: the tap goes to the player)
#   ARMv7 WebKit   Alpine's armhf WebKit is built for ARMv6, which has no
#                  JIT: its JavaScript runs in an interpreter.  The ARMv7
#                  build of the same version, which has one, runs on the very
#                  same libraries (one musl, one C++ ABI): JavaScript 2.3 times
#                  as fast in a page (1.85 s against 4.3 s, a benchmark of
#                  objects, JSON, maps, regexps and DOM, 2026-11-16).  Its two
#                  libraries go in /usr/lib/cascadia-web, not over apk's, and
#                  the launcher uses them only while the system's WebKit is the
#                  version they came with (an upgrade: this script again)
#   ad servers     Peter Lowe's list (pgl.yoyo.org), made a WebKit content
#                  blocker: an ad's scripts and videos cost this iPad more
#                  than the page
# Running it again changes nothing that is already right.
set -e
W=/usr/share/cascadia/web
J=/usr/lib/cascadia-web

for f in cascadia-web cascadia-play cascadia-ytdl hook.js start.html; do
    [ -s $W/$f ] || { echo "no $W/$f: copy tools/desktop/web/ there first (above)" >&2; exit 1; }
done
grep -q '^ytdl-format=' /etc/mpv/mpv.conf 2>/dev/null ||
    { echo "no /etc/mpv/mpv.conf of youtube-setup.sh's: run tools/desktop/youtube-setup.sh first" >&2; exit 1; }

apk add --no-progress py3-gobject3 webkit2gtk-4.1 gst-plugins-good gst-libav mpv yt-dlp yad alpine-keys

chown -R root:root $W
chmod 755 $W $W/cascadia-web $W/cascadia-play $W/cascadia-ytdl
chmod 644 $W/hook.js $W/start.html
mkdir -p /usr/local/bin /usr/share/applications /root/Desktop
ln -sf $W/cascadia-play /usr/local/bin/cascadia-play

# -- WebKit with a JIT --------------------------------------------------------
# the ARMv7 package of the installed version, from the same mirror, checked
# against Alpine's ARMv7 key (alpine-keys has every architecture's)
armv7_webkit() {
    ver=$(awk '/^P:/ { p = substr($0, 3) } /^V:/ && p == "webkit2gtk-4.1" { print substr($0, 3); exit }' \
          /lib/apk/db/installed)
    repo=$(grep -m 1 -E '^[^#]*/community/?$' /etc/apk/repositories | sed 's|/*$||')
    [ -n "$ver" ] && [ -n "$repo" ] || { echo "    no webkit2gtk-4.1 version or community repository"; return 1; }
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp" $J.new' EXIT
    echo "    fetching $repo/armv7/webkit2gtk-4.1-$ver.apk (~45 MB)"
    wget -q -O "$tmp/wk.apk" "$repo/armv7/webkit2gtk-4.1-$ver.apk" || { echo "    the download failed"; return 1; }
    apk verify --keys-dir /usr/share/apk/keys/armv7 "$tmp/wk.apk" >/dev/null 2>&1 ||
        { echo "    its signature did not check out against Alpine's ARMv7 key"; return 1; }
    mkdir "$tmp/x"
    tar -xzf "$tmp/wk.apk" -C "$tmp/x" usr/lib 2>/dev/null || true
    rm -rf $J.new
    mkdir -p $J.new
    for l in libwebkit2gtk-4.1 libjavascriptcoregtk-4.1; do
        f=$(cd "$tmp/x/usr/lib" && ls $l.so.0.*.* 2>/dev/null | head -n 1)
        [ -n "$f" ] || { echo "    no $l in the package"; return 1; }
        cp "$tmp/x/usr/lib/$f" $J.new/
        ln -s "$f" $J.new/$l.so.0
    done
    # they load, and the JIT runs: JavaScript through them before they are used
    out=$(LD_LIBRARY_PATH=$J.new python3 -c '
import gi
gi.require_version("JavaScriptCore", "4.1"); gi.require_version("WebKit2", "4.1")
from gi.repository import JavaScriptCore, WebKit2
c = JavaScriptCore.Context()
print(c.evaluate("let s = 0; for (let i = 0; i < 100000; i++) s += i % 7; s", -1).to_int32())' 2>&1) || true
    [ "$out" = 299995 ] || { echo "    they did not run here: $out"; return 1; }
    echo "$stamp" > $J.new/matches
    rm -rf $J
    mv $J.new $J
    rm -rf "$tmp"
    trap - EXIT
}
lib=$(readlink -f /usr/lib/libwebkit2gtk-4.1.so.0)
stamp=$(stat -c '%n %s %Y' "$lib")
if [ "$(cat $J/matches 2>/dev/null)" = "$stamp" ] && [ -s $J/libwebkit2gtk-4.1.so.0 ]; then
    echo "==> WebKit with a JIT: in $J already, for this version"
elif armv7_webkit; then
    echo "==> WebKit with a JIT: in $J"
else
    echo "==> WebKit with a JIT: not this time (above): the system's WebKit is used"
fi

cat > /usr/local/bin/cascadia-web <<'END'
#!/bin/sh
# cascadia-web -- a web browser whose videos play in mpv
# (tools/desktop/web-setup.sh).  It runs on WebKit's ARMv7 build, which has a
# JIT, when /usr/lib/cascadia-web holds the one for the version installed;
# on the system's otherwise.  CASCADIA_WEB_ARMHF=1: the system's anyway.
J=/usr/lib/cascadia-web
if [ -z "$CASCADIA_WEB_ARMHF" ] && [ -r $J/matches ] &&
   [ "$(stat -c '%n %s %Y' "$(readlink -f /usr/lib/libwebkit2gtk-4.1.so.0)")" = "$(cat $J/matches)" ]; then
    export LD_LIBRARY_PATH="$J${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec python3 /usr/share/cascadia/web/cascadia-web "$@"
END
chmod 755 /usr/local/bin/cascadia-web

# -- ad servers ---------------------------------------------------------------
# one WebKit rule a server, for the requests a page makes to it from another
# site; written only when the list changed (the browser compiles it again
# when the file is new)
list=$(mktemp)
if wget -q -T 30 -O "$list" 'https://pgl.yoyo.org/adservers/serverlist.php?hostformat=nohtml&showintro=0&mimetype=plaintext' &&
   python3 - "$list" "$W/blocklist.json.new" <<'END'
import json, re, sys
rules = []
for line in open(sys.argv[1]):
    d = line.strip().lower()
    if re.fullmatch(r"[a-z0-9-]+(\.[a-z0-9-]+)+", d):
        rules.append({"trigger": {"url-filter": "^https?://([^/]*\\.)?" + d.replace(".", "\\.") + "[/:]",
                                  "load-type": ["third-party"]},
                      "action": {"type": "block"}})
if len(rules) < 500:
    sys.exit("only %d servers in the list" % len(rules))
json.dump(rules, open(sys.argv[2], "w"), separators=(",", ":"))
print("==> ad servers: %d" % len(rules))
END
then
    if cmp -s "$W/blocklist.json.new" "$W/blocklist.json"; then rm "$W/blocklist.json.new"
    else mv "$W/blocklist.json.new" "$W/blocklist.json"; fi
else
    rm -f "$W/blocklist.json.new"
    echo "==> ad servers: the list did not come$( [ -s $W/blocklist.json ] && echo ": the one from last time stays")"
fi
rm -f "$list"

# -- the menu, the desktop, and the browser links open in --------------------
# Its own icon: a globe and a play button, not anybody's logo.
mkdir -p /usr/share/icons/hicolor/scalable/apps
cat > /usr/share/icons/hicolor/scalable/apps/cascadia-web.svg <<'END'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <circle cx="21" cy="21" r="17" fill="#1e6fd9"/>
  <g fill="none" stroke="#ffffff" stroke-width="2">
    <ellipse cx="21" cy="21" rx="7" ry="17"/>
    <path d="M4 21h34M7.5 12.5h27M7.5 29.5h27"/>
  </g>
  <circle cx="35" cy="35" r="11" fill="#ffffff"/>
  <circle cx="35" cy="35" r="9" fill="#e5483b"/>
  <path d="M32 30.5 L40.5 35 L32 39.5 Z" fill="#ffffff"/>
</svg>
END
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true

cat > /usr/share/applications/cascadia-web.desktop <<'END'
[Desktop Entry]
Type=Application
Name=Web
GenericName=Web Browser
Comment=Browse the web; videos play in the video player
Exec=/usr/local/bin/cascadia-web %U
Icon=cascadia-web
Categories=Network;WebBrowser;
MimeType=text/html;application/xhtml+xml;x-scheme-handler/http;x-scheme-handler/https;
StartupNotify=true
Terminal=false
END
cp /usr/share/applications/cascadia-web.desktop /root/Desktop/cascadia-web.desktop
chmod 755 /root/Desktop/cascadia-web.desktop
update-desktop-database -q /usr/share/applications 2>/dev/null || true

# The browser links open in -- unless one was chosen already, which stays:
# XFCE's preferred applications (exo-open, the panel's browser button) read
# helpers.rc, xdg-open and GIO mimeapps.list.  Settings -> Default
# Applications -> Web Browser: Web, to make it this one then.
mkdir -p /root/.config/xfce4 /usr/share/xfce4/helpers
cat > /usr/share/xfce4/helpers/cascadia-web.desktop <<'END'
[Desktop Entry]
NoDisplay=true
Version=1.0
Type=X-XFCE-Helper
X-XFCE-Category=WebBrowser
X-XFCE-Commands=/usr/local/bin/cascadia-web
X-XFCE-CommandsWithParameter=/usr/local/bin/cascadia-web "%s"
Icon=cascadia-web
Name=Web
END
h=/root/.config/xfce4/helpers.rc
if grep -q '^WebBrowser=' $h 2>/dev/null; then
    echo "==> links: the web browser chosen in XFCE stays ($(sed -n 's/^WebBrowser=//p' $h)):"
    echo "    Settings -> Default Applications -> Web Browser: Web, to make it this one"
else
    echo "WebBrowser=cascadia-web" >> $h
fi
m=/root/.config/mimeapps.list
if ! grep -q '^x-scheme-handler/http=' $m 2>/dev/null; then
    grep -q '^\[Default Applications\]' $m 2>/dev/null || printf '[Default Applications]\n' >> $m
    for t in application/xhtml+xml text/html x-scheme-handler/https x-scheme-handler/http; do
        sed -i "/^\[Default Applications\]/a $t=cascadia-web.desktop" $m
    done
fi

echo "==> done: Web in the menu (Internet) and on the desktop; or: cascadia-web [URL | WORDS]"
