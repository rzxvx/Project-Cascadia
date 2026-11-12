# Runs ON THE iPad (or, unchanged, inside the root ./cascadia a5 desktop-root
# builds on the host -- its `games` step):
#   ssh root@10.55.0.2 sh -s < tools/desktop/games-setup.sh
# after RetroArch (tools/desktop/retroarch/build.sh install), PPSSPP
# (tools/desktop/ppsspp/build.sh install) and the touch controls
# (tools/games/overlays in /usr/share/cascadia/overlays; the `games` step
# does all three).  Then, in XFCE: Games on the desktop or in the
# menu, or
#   games [GAME]
#
# Game Boy, Game Boy Color, Game Boy Advance and PSP games, played by touch:
#   RetroArch    the emulator's frontend -- our build: the touch screen is a
#                multi-touch one there, every finger a pointer, so the
#                on-screen controls can be held together (a stock build
#                takes one finger; tools/desktop/retroarch/build.sh)
#   cores        Gambatte (Game Boy, Game Boy Color) and mGBA (Game Boy
#                Advance) from Alpine; gpSP too, the lighter GBA one
#   overlays     the touch controls, drawn for this screen
#                (tools/games/make-overlays.py): the game in the middle at
#                an integer scale, the controls in the margins
#   MaterialUI   RetroArch's touch menu: its icons and font only, out of
#                retroarch-assets (184 MB, themes and wallpapers we do not
#                use)
#   PPSSPP       the PSP: its own SDL frontend, menus and touch controls
#                made for phones, multi-touch through SDL (our build for the
#                A5: Alpine has none); started with the iPad's settings
#                the first time, its own menu's after
# The games are the user's own, in ~/Games (gb, gbc, gba, psp).  Running it again
# changes nothing that is already right.
set -e

command -v retroarch >/dev/null ||
    { echo "games-setup: no retroarch -- tools/desktop/retroarch/build.sh install first" >&2; exit 1; }
[ -f /usr/share/cascadia/overlays/gba.cfg ] ||
    { echo "games-setup: no overlays in /usr/share/cascadia/overlays" >&2; exit 1; }

# openssh-sftp-server: how the games get here -- scp (OpenSSH 9 and later
# speaks SFTP) and file managers' sftp:// both need it, and dropbear runs it
# from /usr/lib/ssh/sftp-server
apk add --no-progress libretro-gambatte libretro-mgba libretro-gpsp libretro-core-info openssh-sftp-server

# MaterialUI's icons and the fonts, streamed out of the package (it would not
# fit in the iPad's /tmp)
A=/usr/share/libretro/assets
if [ ! -f $A/glui/back.png ] || [ ! -f $A/pkg/fallback-font.ttf ]; then
    apk fetch --stdout -q retroarch-assets |
        tar -xzf - -C / usr/share/libretro/assets/glui usr/share/libretro/assets/pkg/fallback-font.ttf \
            usr/share/libretro/assets/pkg/osd-font.ttf usr/share/libretro/assets/COPYING 2>/dev/null
    [ -f $A/glui/back.png ] || { echo "games-setup: retroarch-assets gave no MaterialUI icons" >&2; exit 1; }
fi

mkdir -p /usr/share/cascadia/retroarch /usr/local/bin /usr/share/applications \
    /usr/share/icons/hicolor/scalable/apps /root/Desktop /root/Games/gb /root/Games/gbc /root/Games/gba /root/Games/psp \
    /usr/share/cascadia/ppsspp

# What every game starts with (games passes it with --appendconfig, so a
# setting changed in RetroArch's menu that is here lasts until the game ends).
# Measured on the iPad 2 (2026-11-11), unthrottled: mGBA 64 fps with the
# video on the emulator's thread, 98 fps with a thread of its own -- the A5
# has two cores; Gambatte 68 and 440.
cat > /usr/share/cascadia/retroarch/ipad.cfg <<'END'
# RetroArch on the iPad 2 (tools/desktop/games-setup.sh)
video_driver = "gl"
video_fullscreen = "true"
video_windowed_fullscreen = "true"
video_threaded = "true"
video_vsync = "true"
video_smooth = "false"
# _NET_WM_BYPASS_COMPOSITOR: xfwm4 does not copy every frame again
video_disable_composition = "true"
aspect_ratio_index = "23"
audio_driver = "alsathread"
audio_latency = "64"
# the touch screen read directly (multi-touch), and no gamepad needed
input_driver = "udev"
input_joypad_driver = "udev"
input_overlay_enable = "true"
input_overlay_opacity = "0.7"
input_overlay_hide_in_menu = "true"
input_overlay_hide_when_gamepad_connected = "true"
input_overlay_auto_rotate = "false"
input_overlay_show_mouse_cursor = "false"
menu_driver = "glui"
menu_pointer_enable = "true"
menu_mouse_enable = "false"
assets_directory = "/usr/share/libretro/assets"
libretro_directory = "/usr/lib/libretro"
libretro_info_path = "/usr/share/libretro/info"
rgui_browser_directory = "~/Games"
pause_nonactive = "false"
# "Close Content" in the menu ends RetroArch: back to the list of games
quit_on_close_content = "1"
END
# the game's picture where its overlay leaves room for it: x3 (720x480) for
# the GBA, x4 (640x576) for the Game Boy, in the middle of the 1024x768 screen.
# RetroArch 1.22 places a custom viewport centred and adds custom_viewport_x/y
# to that (video_driver.c: padding x bias) -- 0 is the middle.
cat > /usr/share/cascadia/retroarch/gba.cfg <<'END'
input_overlay = "/usr/share/cascadia/overlays/gba.cfg"
custom_viewport_x = "0"
custom_viewport_y = "0"
custom_viewport_width = "720"
custom_viewport_height = "480"
END
cat > /usr/share/cascadia/retroarch/gb.cfg <<'END'
input_overlay = "/usr/share/cascadia/overlays/gb.cfg"
custom_viewport_x = "0"
custom_viewport_y = "0"
custom_viewport_width = "640"
custom_viewport_height = "576"
END

# PPSSPP's settings the first time (the launcher copies them in if there are
# none yet; after that, PPSSPP's own menu keeps them): full screen, its touch
# controls (off by default on a desktop), OpenGL, the PSP's own resolution,
# no update checks, and for this GPU (measured with the PSPSDK's samples on
# the iPad 2, 2026-11-11):
#   VerticalSync       off: with it 55 of 60 frames, without 60
# Hardware transform stays PPSSPP's default, on: with depth clamping hidden
# (the driconf below) its vertex shaders run on the GPU, as fast as PPSSPP's
# own transform on the CPU or faster (zbufferfog 91 -> 95% speed, spharm
# 95 -> 100%) and with half the CPU (God of War).
#   AudioBufferSize    1024 (23 ms) for 256: slack for the sound's thread
#                      when both cores are busy (ALSA underran while PPSSPP
#                      first started, compiling its shaders)
cat > /usr/share/cascadia/ppsspp/ppsspp.ini <<'END'
[General]
FirstRun = False
CheckForNewVersion = False
[Graphics]
GraphicsBackend = 0 (OPENGL)
FullScreen = True
InternalResolution = 1
VerticalSync = False
[Sound]
AudioBufferSize = 1024
[Control]
ShowTouchControls = True
END

# PPSSPP turns depth clamping on for its 3D draws when the driver has it,
# and the SGX's Mesa clamps depth on the CPU (the draw module cuts triangles
# where they cross the depth range, M43 of research/p105-mesa.md) -- so not
# one of PPSSPP's draws had its vertex shader on the GPU.  Without the
# extension PPSSPP draws as it does on phones (OpenGL ES 2.0 has none):
# spharm 21 -> 57 frames/s, God of War's menu 3 -> 8.5% speed.  Mesa's
# driconf hides it from PPSSPP alone, however it is started.
if [ -d /usr/local/lib/sgx-mesa ]; then
    mkdir -p /usr/local/lib/sgx-mesa/share/drirc.d
    cat > /usr/local/lib/sgx-mesa/share/drirc.d/50-cascadia-ppsspp.conf <<'END'
<?xml version="1.0" standalone="yes"?>
<!-- tools/desktop/games-setup.sh: PPSSPP without depth clamping, whose
     draws the SGX's Mesa would make on the CPU -->
<driconf>
  <device>
    <application name="PPSSPP" executable="PPSSPPSDL">
      <option name="mesa_extension_override" value="-GL_ARB_depth_clamp -GL_NV_depth_clamp -GL_AMD_depth_clamp_separate -GL_EXT_depth_clamp"/>
    </application>
  </device>
</driconf>
END
fi

cat > /usr/local/bin/games <<'END'
#!/bin/sh
# games [GAME] -- the games in ~/Games (gb, gbc, gba, psp), picked by touch
# and played full screen with touch controls (tools/desktop/games-setup.sh).
# PSP games too big for the iPad stream from a computer: its address (as
# tools/games/psp-share.py prints it) in ~/Games/psp-server.
# GBA_CORE=gpsp: the lighter GBA emulator instead of mGBA.
G="$HOME/Games"
T=Games
export DISPLAY="${DISPLAY:-:0}" PATH="/usr/local/bin:$PATH"
say() {
    if [ -n "$DISPLAY" ]; then yad --error --title=$T --center --width=480 --text="$1" --button=OK:0 2>/dev/null
    else echo "games: $1" >&2; fi
}
# Game Boy (Color), Game Boy Advance or PSP, by the name's ending
system() {
    case "$(echo "${1##*.}" | tr A-Z a-z)" in
        gba) echo gba ;;
        gb|gbc|sgb) echo gb ;;
        iso|cso|chd|pbp) echo psp ;;
        *) return 1 ;;
    esac
}
play() {
    sys=$(system "$1") || { say "Not a Game Boy, Game Boy Advance or PSP game: $(basename "$1")"; return 1; }
    if [ "$sys" = gba ]; then core=${GBA_CORE:-mgba}; else core=gambatte; fi
    if [ "$sys" = psp ] && ! command -v PPSSPPSDL >/dev/null; then
        say "PSP games need PPSSPP, which this root does not have."; return 1
    fi
    # RetroArch reads the touch screen itself, in the screen's own landscape
    # (Home on the left): the screen is held that way while the game runs
    locked=""
    [ -e /run/rotation-lock ] && locked=1
    was=$(rotate 2>/dev/null || echo normal)
    touch /run/rotation-lock
    [ "$was" = normal ] || rotate normal
    if [ "$sys" = psp ]; then
        ini="${XDG_CONFIG_HOME:-$HOME/.config}/ppsspp/PSP/SYSTEM/ppsspp.ini"
        [ -f "$ini" ] || { mkdir -p "$(dirname "$ini")" && cp /usr/share/cascadia/ppsspp/ppsspp.ini "$ini"; }
        # --pause-menu-exit: Exit in its pause menu ends the game here, in Games
        PPSSPPSDL --fullscreen --pause-menu-exit "$1"
    else
        retroarch -L "/usr/lib/libretro/${core}_libretro.so" \
            --appendconfig="/usr/share/cascadia/retroarch/ipad.cfg|/usr/share/cascadia/retroarch/$sys.cfg" "$1"
    fi
    if [ -n "$locked" ]; then [ "$was" = normal ] || rotate "$was"
    else rm -f /run/rotation-lock; fi       # autorotate turns it as it is held
}
[ -n "${1:-}" ] && { play "$1"; exit; }
mkdir -p "$G/gb" "$G/gbc" "$G/gba" "$G/psp"
while :; do
    # the computer's PSP games, if one is named and answers within 5 s
    remote=""
    if [ -s "$G/psp-server" ]; then
        url=$(head -n 1 "$G/psp-server"); url=${url%/}
        # disc images only: PPSSPP runs a .pbp from its folder, which a URL has not
        remote=$(wget -q -T 5 -O - "$url/" 2>/dev/null | grep -i -E '\.(iso|cso|chd)$' | sed "s|^/*|$url/|")
    fi
    list=$( { find "$G" -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.sgb' -o -iname '*.gba' \
                  -o -iname '*.iso' -o -iname '*.cso' -o -iname '*.chd' -o -iname '*.pbp' \)
              [ -n "$remote" ] && echo "$remote"; } | sort)
    if [ -z "$list" ]; then
        say "No games yet.  Copy your Game Boy (.gb, .gbc), Game Boy Advance (.gba) and PSP (.iso, .cso) games into $G, then open Games again."
        exit 0
    fi
    # rows and buttons big enough for a finger
    game=$(echo "$list" | while IFS= read -r f; do
               case "$(echo "${f##*.}" | tr A-Z a-z)" in
                   gba) echo "Game Boy Advance" ;; gbc) echo "Game Boy Color" ;;
                   iso|cso|chd|pbp) case "$f" in http*) echo "PlayStation Portable (network)" ;;
                                                  *) echo "PlayStation Portable" ;; esac ;;
                   *) echo "Game Boy" ;;
               esac
               # a homebrew's EBOOT.PBP goes by its folder's name; a URL's %20s are spaces again
               n=$(basename "$f"); [ "$(echo "$n" | tr a-z A-Z)" = EBOOT.PBP ] && n=$(basename "$(dirname "$f")").pbp
               case "$f" in http*) n=$(printf '%b' "$(echo "$n" | sed 's/%\([0-9A-Fa-f][0-9A-Fa-f]\)/\\x\1/g')") ;; esac
               echo "${n%.*}"; echo "$f"
           done | yad --list --title=$T --center --width=1000 --height=700 \
                      --css='treeview.view { font-size: 18pt; padding: 12px 8px; } button { font-size: 16pt; padding: 10px 28px; }' \
                      --column=System --column=Game --column=File:HD --print-column=3 --separator= \
                      --button=Play:0 --button=Close:1 2>/dev/null) || exit 0
    [ -n "$game" ] && play "$game"
done
END
chmod 755 /usr/local/bin/games

# a gamepad, not anybody's logo
cat > /usr/share/icons/hicolor/scalable/apps/cascadia-games.svg <<'END'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">
  <rect x="4" y="18" width="56" height="30" rx="15" fill="#3b4a6b"/>
  <rect x="14" y="30" width="14" height="6" rx="1" fill="#fff"/>
  <rect x="18" y="26" width="6" height="14" rx="1" fill="#fff"/>
  <circle cx="44" cy="36" r="4" fill="#e5484d"/>
  <circle cx="51" cy="29" r="4" fill="#f5c542"/>
</svg>
END
gtk-update-icon-cache -q -f /usr/share/icons/hicolor 2>/dev/null || true
cat > /usr/share/applications/games.desktop <<'END'
[Desktop Entry]
Type=Application
Name=Games
Comment=Game Boy, Game Boy Advance and PSP games, by touch
Exec=games
Icon=cascadia-games
Terminal=false
Categories=Game;
END
cp /usr/share/applications/games.desktop /root/Desktop/games.desktop
chmod 755 /root/Desktop/games.desktop
echo "games: ready -- your games go in /root/Games (gb, gbc, gba, psp)"
