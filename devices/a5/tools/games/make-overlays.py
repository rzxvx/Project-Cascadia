#!/usr/bin/env python3
"""make-overlays.py OUTDIR: the touch controls for RetroArch on the iPad 2's screen.

RetroArch overlays (one per system) for the 1024x768 landscape screen, drawn
here so the artwork is the project's own: the game picture sits in the middle
at an integer scale (GBA x3, 720x480; Game Boy x4, 640x576 -- the matching
viewports are in games-setup.sh) and the controls fill the margins.  Each
control is its own image, so RetroArch lights it up while it is pressed.

  OUTDIR/gba.cfg  OUTDIR/gb.cfg  OUTDIR/*.png

Needs Pillow.  Positions are screen pixels, turned into RetroArch's normalized
coordinates (x, y in 0..1, the half-sizes too) when the .cfg is written.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 1024, 768
SS = 4                                  # supersampling for smooth edges
FILL, EDGE, TEXT = (255, 255, 255, 70), (255, 255, 255, 200), (255, 255, 255, 230)

FONTS = ["/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
         "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"]


def font(px):
    for f in FONTS:
        if os.path.exists(f):
            return ImageFont.truetype(f, px * SS)
    sys.exit("make-overlays.py: no DejaVuSans-Bold.ttf found")


def finish(im):
    return im.resize((im.width // SS, im.height // SS), Image.LANCZOS)


def label(d, box, text, px):
    f = font(px)
    l, t, r, b = d.textbbox((0, 0), text, font=f)
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
    d.text((cx - (r - l) / 2 - l, cy - (b - t) / 2 - t), text, font=f, fill=TEXT)


def round_button(size, text, px):
    s = size * SS
    im = Image.new("RGBA", (s, s))
    d = ImageDraw.Draw(im)
    w = 3 * SS
    d.ellipse((w, w, s - w, s - w), fill=FILL, outline=EDGE, width=w)
    label(d, (0, 0, s, s), text, px)
    return finish(im)


def pill(width, height, text, px):
    sw, sh = width * SS, height * SS
    im = Image.new("RGBA", (sw, sh))
    d = ImageDraw.Draw(im)
    w = 3 * SS
    d.rounded_rectangle((w, w, sw - w, sh - w), radius=sh // 2 - w, fill=FILL, outline=EDGE, width=w)
    label(d, (0, 0, sw, sh), text, px)
    return finish(im)


def dpad(size):
    s = size * SS
    im = Image.new("RGBA", (s, s))
    d = ImageDraw.Draw(im)
    w, arm = 3 * SS, s // 3
    lo, hi = (s - arm) // 2, (s + arm) // 2
    cross = [(lo, w), (hi, w), (hi, lo), (s - w, lo), (s - w, hi), (hi, hi), (hi, s - w),
             (lo, s - w), (lo, hi), (w, hi), (w, lo), (lo, lo)]
    d.polygon(cross, fill=FILL, outline=EDGE, width=w)
    a = arm // 3                         # the arrows
    c = s // 2
    for (tx, ty), (dx, dy) in (((c, w + a), (0, -1)), ((c, s - w - a), (0, 1)),
                               ((w + a, c), (-1, 0)), ((s - w - a, c), (1, 0))):
        px_, py_ = -dy, dx               # perpendicular
        tip = (tx + dx * a * 0.6, ty + dy * a * 0.6)
        base = (tx - dx * a * 0.4, ty - dy * a * 0.4)
        d.polygon([tip, (base[0] + px_ * a * 0.6, base[1] + py_ * a * 0.6),
                   (base[0] - px_ * a * 0.6, base[1] - py_ * a * 0.6)], fill=TEXT)
    return finish(im)


# (name, RetroArch key, shape, centre x, centre y, half-width, half-height, image)
def layout(system):
    if system == "gba":                  # picture 720x480 at (152, 144)
        ctl = [("dpad", "dpad_area", "radial", 78, 470, 76, 76),
               ("a", "a", "radial", 972, 420, 46, 46),
               ("b", "b", "radial", 900, 500, 46, 46),
               ("l", "l", "rect", 76, 220, 66, 32),
               ("r", "r", "rect", 948, 220, 66, 32),
               ("select", "select", "rect", 440, 696, 62, 26),
               ("start", "start", "rect", 584, 696, 62, 26),
               ("menu", "menu_toggle", "rect", 450, 40, 56, 24),
               ("ff", "toggle_fast_forward", "rect", 574, 40, 56, 24)]
    else:                                # picture 640x576 at (192, 96)
        ctl = [("dpad", "dpad_area", "radial", 96, 470, 86, 86),
               ("a", "a", "radial", 950, 420, 50, 50),
               ("b", "b", "radial", 870, 500, 50, 50),
               ("select", "select", "rect", 440, 722, 62, 26),
               ("start", "start", "rect", 584, 722, 62, 26),
               ("menu", "menu_toggle", "rect", 450, 46, 56, 24),
               ("ff", "toggle_fast_forward", "rect", 574, 46, 56, 24)]
    return ctl


def image(name, hw, hh):
    if name == "dpad":
        return dpad(2 * hw)
    if name in ("a", "b"):
        return round_button(2 * hw, name.upper(), int(hw * 0.9))
    text = {"l": "L", "r": "R", "select": "SELECT", "start": "START", "menu": "MENU", "ff": "▶▶"}[name]
    return pill(2 * hw, 2 * hh, text, 22 if len(text) > 2 else 26)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for system in ("gba", "gb"):
        ctl = layout(system)
        lines = ["# RetroArch overlay: %s touch controls for the iPad 2 (1024x768 landscape)," % system.upper(),
                 "# made by tools/games/make-overlays.py", "overlays = 1",
                 'overlay0_name = "%s-landscape"' % system, "overlay0_full_screen = true",
                 "overlay0_normalized = true", "overlay0_range_mod = 1.5", "overlay0_alpha_mod = 2.0",
                 "overlay0_descs = %d" % len(ctl)]
        for i, (name, key, shape, x, y, hw, hh) in enumerate(ctl):
            png = "%s-%s.png" % (system, name)
            image(name, hw, hh).save(os.path.join(out, png))
            lines.append('overlay0_desc%d = "%s,%.5f,%.5f,%s,%.5f,%.5f"' % (i, key, x / W, y / H, shape, hw / W, hh / H))
            lines.append('overlay0_desc%d_overlay = "%s"' % (i, png))
        with open(os.path.join(out, system + ".cfg"), "w") as f:
            f.write("\n".join(lines) + "\n")
    print("overlays in", out)


if __name__ == "__main__":
    main()
