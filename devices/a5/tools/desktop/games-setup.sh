# Runs ON THE iPad (or, unchanged, inside the root ./cascadia a5 desktop-root
# builds on the host -- its `games` step):
#   ssh root@10.55.0.2 sh -s < tools/desktop/games-setup.sh
# after RetroArch (tools/desktop/retroarch/build.sh install) and the touch
# controls (tools/games/overlays in /usr/share/cascadia/overlays; the
# `games` step copies both).  Then, in XFCE: Games on the desktop or in the
# menu, or
#   games [GAME]
#
# Game Boy, Game Boy Color and Game Boy Advance games, played by touch:
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
# The games are the user's own, in ~/Games (gb, gbc, gba).  Running it again
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
    /usr/share/icons/hicolor/scalable/apps /root/Desktop /root/Games/gb /root/Games/gbc /root/Games/gba

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

cat > /usr/local/bin/games <<'END'
#!/bin/sh
# games [GAME] -- the games in ~/Games (gb, gbc, gba), picked by touch and
# played full screen with touch controls (tools/desktop/games-setup.sh).
# GBA_CORE=gpsp: the lighter GBA emulator instead of mGBA.
G="$HOME/Games"
T=Games
export DISPLAY="${DISPLAY:-:0}" PATH="/usr/local/bin:$PATH"
say() {
    if [ -n "$DISPLAY" ]; then yad --error --title=$T --center --width=480 --text="$1" --button=OK:0 2>/dev/null
    else echo "games: $1" >&2; fi
}
# Game Boy (Color) or Game Boy Advance, by the name's ending
system() {
    case "$(echo "${1##*.}" | tr A-Z a-z)" in
        gba) echo gba ;;
        gb|gbc|sgb) echo gb ;;
        *) return 1 ;;
    esac
}
play() {
    sys=$(system "$1") || { say "Not a Game Boy or Game Boy Advance game: $(basename "$1")"; return 1; }
    if [ "$sys" = gba ]; then core=${GBA_CORE:-mgba}; else core=gambatte; fi
    # RetroArch reads the touch screen itself, in the screen's own landscape
    # (Home on the left): the screen is held that way while the game runs
    locked=""
    [ -e /run/rotation-lock ] && locked=1
    was=$(rotate 2>/dev/null || echo normal)
    touch /run/rotation-lock
    [ "$was" = normal ] || rotate normal
    retroarch -L "/usr/lib/libretro/${core}_libretro.so" \
        --appendconfig="/usr/share/cascadia/retroarch/ipad.cfg|/usr/share/cascadia/retroarch/$sys.cfg" "$1"
    if [ -n "$locked" ]; then [ "$was" = normal ] || rotate "$was"
    else rm -f /run/rotation-lock; fi       # autorotate turns it as it is held
}
[ -n "${1:-}" ] && { play "$1"; exit; }
mkdir -p "$G/gb" "$G/gbc" "$G/gba"
while :; do
    list=$(find "$G" -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.sgb' -o -iname '*.gba' \) | sort)
    if [ -z "$list" ]; then
        say "No games yet.  Copy your Game Boy (.gb, .gbc) and Game Boy Advance (.gba) games into $G, then open Games again."
        exit 0
    fi
    # rows and buttons big enough for a finger
    game=$(echo "$list" | while IFS= read -r f; do
               case "$(echo "${f##*.}" | tr A-Z a-z)" in
                   gba) echo "Game Boy Advance" ;; gbc) echo "Game Boy Color" ;; *) echo "Game Boy" ;;
               esac
               n=$(basename "$f"); echo "${n%.*}"; echo "$f"
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
Comment=Game Boy and Game Boy Advance games, by touch
Exec=games
Icon=cascadia-games
Terminal=false
Categories=Game;
END
cp /usr/share/applications/games.desktop /root/Desktop/games.desktop
chmod 755 /root/Desktop/games.desktop
echo "games: ready -- your games go in /root/Games (gb, gbc, gba)"
