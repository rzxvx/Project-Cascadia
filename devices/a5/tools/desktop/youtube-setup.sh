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
#   - fbo-format=rgba8 (and the fast profile): mpv's GPU output renders
#     through float framebuffers by default, which the SGX's Mesa copies on
#     the CPU at every frame -- 30 s of video took 150 s to show.  With 8-bit
#     ones it is real time, ~1 frame in 700 dropped.
#   - the demuxer's cache bounded: 512 MB of RAM, a desktop and the GPU.
#   - vo=gpu with OpenGL through EGL on X, named (2026-11-17): left to
#     itself, mpv 0.41 tries Vulkan twice and libplacebo's OpenGL (gpu-next)
#     before it settles on this, 1.3 s of every start.
#   - hls-bitrate: an HLS playlist mpv opens itself (yt-dlp could not read
#     its page) gets a variant of at most 1.6 Mbit/s, about 480p, where mpv
#     takes the biggest by default -- 1080p, a slide show here.
#   - over 30 frames a second, every other frame, evenly (the profile at the
#     end): the screen shows ~30, and mpv left to drop what it cannot show
#     dropped unevenly and more -- 360p60 H.264 showed 292 frames of 1200,
#     VP9 604; halved, 562 and 583 of 600 (2026-11-17).
#   - the stream (ytdl-format, below): one the two A9s decode at twice its
#     speed or more, which is what plays without a dropped frame.
#
# The stream, for any site yt-dlp reads (YouTube, and the browser's videos:
# tools/desktop/web-setup.sh).  Decoding speed on the iPad 2 (FFmpeg, both
# cores, 2026-11-16), in times real time:
#             240p   360p30  360p60  480p30  480p60  720p30
#   H.264       -     -       2.34    2.57    1.66    1.34
#   VP9       5.13*  3.87    2.31    2.36    1.34     -       (* 240p60)
#   AV1       7.90   3.87     -      2.30     -       -
#   VP8         -    8.74     -      4.80     -       -
#   HEVC        -    2.39     -      1.35     -       -
# and the screen shows ~30 frames a second, so 60 fps buys nothing but the
# decoding.  In this order, the first that the source has:
#   1. H.264 up to 480p and 30 fps        (YouTube's, and most sites')
#   2. VP9, VP8, AV1 up to 480p and 30 fps
#   3. H.264, VP9, VP8 up to 360p, any frame rate
#   4. AV1, HEVC up to 360p and 30 fps
#   5. any codec up to 480p and 30 fps    (what the site does not name)
#   6. anything up to 240p, and at last the smallest there is
# "480p" is the short side, so a portrait video (a phone's) gets its 480x854
# rather than a 270x480: each step tries portrait sizes first (narrow and
# tall, which no landscape stream is) and then landscape ones.  A frame rate
# that is not known counts as over 30 -- a Wikimedia Commons film at 60 fps
# says nothing of it, and at 480p dropped three frames in four -- except for
# HLS, whose spec asks a stream over 30 fps to say so (RFC 8216, FRAME-RATE).
# Audio: stereo AAC first (the best m4a is often 5.1, decoded and mixed down
# for nothing), then any stereo.
A="(ba[acodec~='^(mp4a|aac)'][audio_channels<=?2]/ba[audio_channels<=?2]/ba)"
tier() {  # CODECS FPS SHORT LONG: portrait sizes, then landscape ones
    p="[width<=?$3][height>=$(( $3 * 4 / 3 ))][height<=$4]"
    l="[height<=?$3][width<=?$4]"
    printf '%s/' "bv$1$2$p+$A" "b$1$2$p" "bv$1$2$l+$A" "b$1$2$l"
}
H264="[vcodec~='^(avc|h264)']"
LIGHT="[vcodec~='^(vp0?9|vp0?8|av0?1)']"
ANY60="[vcodec~='^(avc|h264|vp0?9|vp0?8)']"
HEAVY="[vcodec~='^(av0?1|hev|hvc|h265)']"
F30="[fps<=30]"
HLS30="[protocol^=m3u8][fps<=?30]"
FORMAT="$(tier "$H264" "$F30" 480 864)$(tier "$H264" "$HLS30" 480 864)"
FORMAT="$FORMAT$(tier "$LIGHT" "$F30" 480 864)$(tier "$LIGHT" "$HLS30" 480 864)"
FORMAT="$FORMAT$(tier "$ANY60" "" 360 640)$(tier "$HEAVY" "[fps<=?30]" 360 640)"
FORMAT="$FORMAT$(tier "" "[fps<=?30]" 480 864)$(tier "" "" 240 432)wv*+$A/w"

cat > /etc/mpv/mpv.conf <<END
# tools/desktop/youtube-setup.sh writes this file: what each line is for is there
ytdl-format=$FORMAT
hwdec=no
profile=fast
vo=gpu
gpu-api=opengl
gpu-context=x11egl
fbo-format=rgba8
hls-bitrate=1600000
fs=yes
keep-open=no
cache=yes
demuxer-max-bytes=32MiB
demuxer-max-back-bytes=8MiB
# the on-screen controller, bigger: fingers, not a mouse
script-opts=osc-scalefullscreen=1.6,osc-scalewindowed=1.6,osc-hidetimeout=2500

[cascadia-half-rate]
profile-desc=over 30 frames a second: every other one, evenly
profile-cond=(p["container-fps"] or 0) > 31
profile-restore=copy
vf-add=fps=fps=source_fps/2
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
