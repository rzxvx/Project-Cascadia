# Runs ON THE iPad (or, unchanged, inside the root ./cascadia a5 desktop-root
# builds on the host -- its `youtube` step):
#   ssh root@10.55.0.2 sh -s < tools/desktop/youtube-setup.sh
# then, in XFCE: YouTube in the menu (Multimedia) or on the desktop, or
#   youtube [WORDS... | URL]
#
# YouTube without a browser.  A browser that runs YouTube's site does not fit
# in 512 MB on two A9 cores; a player does.  What each piece is for:
#   mpv          the player.  Alpine's armhf FFmpeg carries NEON decoders,
#                picked at run time (11 827 NEON instructions in libavcodec),
#                so H.264 up to 480p decodes in software in real time
#   yt-dlp       finds the streams.  YouTube now hides them behind a
#                JavaScript challenge: yt-dlp-ejs holds the solver, nodejs
#                runs it (deno, yt-dlp's first choice, has no armhf build;
#                QuickJS would take minutes per video on an A9).  The
#                cascadia_ejs_cache plugin keeps the parsed player between
#                videos: ~30 s to the picture after a boot, ~15 after that
#   yad          the search box and the list of results -- GTK3, as XFCE
#                is, so it brings almost nothing of its own, and its rows
#                are big enough to touch
# Running it again changes nothing that is already right.
set -e

apk add --no-progress mpv yt-dlp yt-dlp-ejs nodejs yad

mkdir -p /etc/mpv /usr/local/bin /usr/share/applications /root/Desktop

# How the iPad plays it (measured on the iPad 2, 2026-11-09):
#   - H.264 (avc1) up to 480p and 30 fps: FFmpeg decodes 480p at 2.5 times
#     real time on the two A9s.  VP9 and AV1, what YouTube offers first, do
#     not keep up.  Stereo AAC: the best m4a is often 5.1, decoded and mixed
#     down for nothing.
#   - fbo-format=rgba8 (and the fast profile): mpv's GPU output renders
#     through float framebuffers by default, which the SGX's Mesa copies on
#     the CPU at every frame -- 30 s of video took 150 s to show.  With 8-bit
#     ones it is real time, ~1 frame in 700 dropped.
#   - the demuxer's cache bounded: 512 MB of RAM, a desktop and the GPU.
cat > /etc/mpv/mpv.conf <<'END'
ytdl-format=bv*[vcodec^=avc1][height<=480][fps<=30]+ba[acodec^=mp4a][audio_channels<=2]/b[vcodec^=avc1][height<=480]/b[height<=480]/b
hwdec=no
profile=fast
fbo-format=rgba8
fs=yes
keep-open=no
cache=yes
demuxer-max-bytes=32MiB
demuxer-max-back-bytes=8MiB
# the on-screen controller, bigger: fingers, not a mouse
script-opts=osc-scalefullscreen=1.6,osc-scalewindowed=1.6,osc-hidetimeout=2500
END

cat > /usr/local/bin/youtube <<'END'
#!/bin/sh
# youtube -- search YouTube and play what you pick; made for the touch screen
# (tools/desktop/youtube-setup.sh).  Needs the network (wifi).
#   youtube              a search box, then the results
#   youtube WORDS...     the results for WORDS
#   youtube URL          play it
# Play starts the video in ~30 s the first time after a boot and ~15 s after
# that: yt-dlp has YouTube's JavaScript challenge solved by Node, on this CPU
# most of the wait (the parse of the player is kept between videos: the
# cascadia_ejs_cache plugin).  The app clients that skip the challenge get no
# video without a token now (android_vr: HTTP 403, ios: images only).  In the
# player: tap for the controls, two-finger tap or q to stop.
export PATH=/usr/local/bin:/usr/bin:/bin
T=YouTube
TAB=$(printf '\t')
say() {
    if [ -n "$DISPLAY" ]; then yad --error --title=$T --center --width=480 --text="$1" --button=OK:0 2>/dev/null
    else echo "youtube: $1" >&2; fi
}
play() {
    exec mpv --force-window=immediate --title="$T" "$1"
}
case "${1:-}" in
    http://*|https://*) play "$1" ;;
esac
q="$*"
if [ -z "$q" ]; then
    q=$(yad --entry --title=$T --center --width=640 --text="Search YouTube:" \
            --button=Search:0 --button=Cancel:1 2>/dev/null) || exit 0
fi
[ -n "$q" ] || exit 0
list=$(mktemp) || exit 1
trap 'rm -f "$list" "$list.err"' EXIT
yt-dlp --flat-playlist --no-warnings \
    --print "%(id)s$TAB%(title)s$TAB%(duration_string)s$TAB%(channel)s" "ytsearch20:$q" \
    > "$list" 2> "$list.err" &
pid=$!
if [ -n "$DISPLAY" ]; then
    ( while kill -0 $pid 2>/dev/null; do echo "#Searching for: $q"; sleep 1; done ) |
        yad --progress --pulsate --auto-close --no-buttons --title=$T --center --width=480 2>/dev/null
fi
wait $pid || { say "The search failed: $(tail -n 1 "$list.err")"; exit 1; }
[ -s "$list" ] || { say "Nothing found for \"$q\"."; exit 1; }
if [ -z "$DISPLAY" ]; then
    cut -f 2- "$list"
    exit 0
fi
id=$(tr "$TAB" '\n' < "$list" |
     yad --list --title="$T: $q" --center --width=1000 --height=700 \
         --column=id:HD --column=Title --column=Length --column=Channel \
         --print-column=1 --separator='' \
         --button=Play:0 --button=Cancel:1 2>/dev/null) || exit 0
[ -n "$id" ] || exit 0
play "https://www.youtube.com/watch?v=$id"
END
chmod 755 /usr/local/bin/youtube

mkdir -p /etc/yt-dlp/plugins/cascadia/yt_dlp_plugins/extractor
cat > /etc/yt-dlp/plugins/cascadia/yt_dlp_plugins/extractor/cascadia_ejs_cache.py <<'END'
# Cascadia (tools/desktop/youtube-setup.sh): yt-dlp keeps its preprocessed
# YouTube player between videos.  Solving YouTube's JavaScript challenge
# parses the 2.5 MB player in Node first -- 20 s on the iPad 2's A9s, for
# every video.  yt-dlp can keep that parse (once per player version) but
# ships with it off, the files being large (~4 MB) and never rotated.  Here
# the cache is in a home that lives in RAM and is gone at the next boot: the
# first video of a boot starts in ~30 s, the ones after in ~15.
try:
    from yt_dlp.extractor.youtube.jsc._builtin.ejs import EJSBaseJCP

    EJSBaseJCP._ENABLE_PREPROCESSED_PLAYER_CACHE = True
except Exception:  # a yt-dlp without it: nothing to do
    pass
END

# Its own icon: the theme's applications-multimedia is not there, and the
# desktop showed a blank page.  A play button, not anybody's logo.
mkdir -p /usr/share/icons/hicolor/scalable/apps
cat > /usr/share/icons/hicolor/scalable/apps/cascadia-video.svg <<'END'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48">
  <rect x="3" y="8" width="42" height="32" rx="8" fill="#1e6fd9"/>
  <path d="M20 16.5 L33 24 L20 31.5 Z" fill="#ffffff"/>
</svg>
END
gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true

cat > /usr/share/applications/youtube.desktop <<'END'
[Desktop Entry]
Type=Application
Name=YouTube
Comment=Search YouTube and play videos (mpv and yt-dlp)
Exec=/usr/local/bin/youtube
Icon=cascadia-video
Categories=AudioVideo;Video;Player;
Terminal=false
END
cp /usr/share/applications/youtube.desktop /root/Desktop/youtube.desktop
chmod 755 /root/Desktop/youtube.desktop

echo "==> done: YouTube in the menu (Multimedia) and on the desktop; or: youtube WORDS"
