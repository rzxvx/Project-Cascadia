#!/usr/bin/env python3
"""screenpad-editor -- screenpad's presets, made and changed on the iPad.

    screenpad-editor [PRESET]

A window in XFCE: on the left the screen as the preset has it, landscape
(1024 x 768) or portrait, with its controls drawn as screenpad draws them;
on the right the selected control's shape, label, size, colour, opacity
and what it does.  Drag a control to move it, its blue handle to resize
it.  Save writes ~/.config/screenpad/NAME.json, which `screenpad NAME`
runs; "Try it" runs it over the screen at once, with a Stop button above
it (the touches off the controls still reach X).

GTK 3 through PyGObject (py3-gobject3, py3-cairo); tools/screenpad/build.sh
installs it as /usr/local/bin/screenpad-editor.  The drawing follows
screenpad.c's draw_control, the action names its tables.
"""
import json
import math
import os
import re
import signal
import subprocess
import sys

import cairo
import gi

gi.require_version("Gdk", "3.0")
gi.require_version("Gtk", "3.0")
gi.require_version("PangoCairo", "1.0")
from gi.repository import Gdk, GLib, Gtk, Pango, PangoCairo  # noqa: E402

HERE = os.path.dirname(os.path.realpath(sys.argv[0]))
SHARE = os.environ.get("SCREENPAD_SHARE", os.path.join(HERE, "..", "share", "screenpad"))
USER = os.path.join(os.path.expanduser("~"), ".config", "screenpad")
SCREENPAD = os.path.join(HERE, "screenpad") if os.path.exists(os.path.join(HERE, "screenpad")) else "screenpad"

# screenpad.c's keys (without its aliases) and the Xbox 360 pad's buttons
KEYS = ("ESC 1 2 3 4 5 6 7 8 9 0 MINUS EQUAL BACKSPACE TAB Q W E R T Y U I O P LEFTBRACE "
        "RIGHTBRACE ENTER LEFTCTRL A S D F G H J K L SEMICOLON APOSTROPHE GRAVE LEFTSHIFT "
        "BACKSLASH Z X C V B N M COMMA DOT SLASH RIGHTSHIFT LEFTALT SPACE CAPSLOCK F1 F2 F3 F4 "
        "F5 F6 F7 F8 F9 F10 F11 F12 RIGHTCTRL RIGHTALT HOME UP PAGEUP LEFT RIGHT END DOWN "
        "PAGEDOWN INSERT DELETE PAUSE LEFTMETA RIGHTMETA KPPLUS KPMINUS VOLUMEUP VOLUMEDOWN "
        "MUTE").split()
KEY_NAMES = {"ESC": "Esc", "LEFTCTRL": "Left Ctrl", "RIGHTCTRL": "Right Ctrl",
             "LEFTSHIFT": "Left Shift", "RIGHTSHIFT": "Right Shift", "LEFTALT": "Left Alt",
             "RIGHTALT": "Right Alt", "SPACE": "Space", "ENTER": "Enter",
             "BACKSPACE": "Backspace", "UP": "↑ Up", "DOWN": "↓ Down", "LEFT": "← Left",
             "RIGHT": "→ Right", "PAGEUP": "Page Up", "PAGEDOWN": "Page Down",
             "LEFTMETA": "Left Super", "RIGHTMETA": "Right Super", "LEFTBRACE": "[",
             "RIGHTBRACE": "]", "SEMICOLON": ";", "APOSTROPHE": "'", "GRAVE": "`",
             "BACKSLASH": "\\", "COMMA": ",", "DOT": ".", "SLASH": "/", "MINUS": "-",
             "EQUAL": "=", "KPPLUS": "Keypad +", "KPMINUS": "Keypad -",
             "CAPSLOCK": "Caps Lock", "VOLUMEUP": "Volume up", "VOLUMEDOWN": "Volume down",
             "TAB": "Tab", "HOME": "Home", "END": "End", "INSERT": "Insert",
             "DELETE": "Delete", "PAUSE": "Pause", "MUTE": "Mute"}
PAD = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("LB", "LB (left shoulder)"),
       ("RB", "RB (right shoulder)"), ("LT", "LT (left trigger)"), ("RT", "RT (right trigger)"),
       ("BACK", "Back"), ("START", "Start"), ("GUIDE", "Guide"), ("LS", "Left stick click"),
       ("RS", "Right stick click"), ("UP", "D-pad ↑"), ("DOWN", "D-pad ↓"),
       ("LEFT", "D-pad ←"), ("RIGHT", "D-pad →")]
DIRS = ("up", "down", "left", "right")
DIR_LABELS = {"up": "↑ Up", "down": "↓ Down", "left": "← Left", "right": "→ Right"}
DIR_PRESETS = [("Arrows", ["key:UP", "key:DOWN", "key:LEFT", "key:RIGHT"]),
               ("WASD", ["key:W", "key:S", "key:A", "key:D"]),
               ("Gamepad d-pad", ["pad:UP", "pad:DOWN", "pad:LEFT", "pad:RIGHT"])]
SWATCHES = ["#ffffff", "#3b82f6", "#ef4444", "#22c55e", "#eab308", "#a855f7", "#f97316",
            "#14b8a6"]
SNAP = 4
PANEL_W = 330

CSS = b"""
.panel { padding: 12px; }
.panel button, .panel spinbutton, .panel combobox, .panel entry { min-height: 38px; }
.toolbar { padding: 6px; }
.toolbar button, .toolbar combobox, .toolbar entry { min-height: 38px; }
.title { font-weight: bold; font-size: 15px; margin-top: 4px; }
.dim { opacity: 0.7; }
.swatch { min-width: 30px; min-height: 30px; padding: 0; border-radius: 15px; }
.stop { font-size: 20px; font-weight: bold; min-height: 56px; min-width: 150px; }
"""


def rgb(hexs):
    hexs = (hexs or "#ffffff").lstrip("#")
    try:
        n = int(hexs, 16)
    except ValueError:
        n = 0xffffff
    return ((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255


def lighter(c, t):
    return tuple(v + (1 - v) * t for v in c)


def slug(name):
    s = re.sub(r"[^a-z0-9_-]+", "-", (name or "preset").lower()).strip("-")[:40]
    return s or "preset"


def default_preset():
    return {"name": "My preset", "screen": "landscape", "controls": [
        {"type": "dpad", "x": 36, "y": 440, "w": 290, "h": 290, "color": "#ffffff",
         "opacity": 0.32, "up": "key:UP", "down": "key:DOWN", "left": "key:LEFT",
         "right": "key:RIGHT"},
        {"type": "button", "shape": "circle", "x": 866, "y": 590, "w": 132, "h": 132,
         "color": "#3b82f6", "opacity": 0.42, "label": "A", "action": "key:SPACE"}]}


def list_presets():
    names = []
    for d in (USER, os.path.join(SHARE, "presets")):
        try:
            for f in sorted(os.listdir(d)):
                if f.endswith(".json") and f[:-5] not in names:
                    names.append(f[:-5])
        except OSError:
            pass
    return names


def read_preset(name):
    for d in (USER, os.path.join(SHARE, "presets")):
        p = os.path.join(d, name + ".json")
        if os.path.exists(p):
            with open(p) as f:
                return json.load(f)
    raise FileNotFoundError(name)


def normalise(obj):
    if not isinstance(obj, dict) or not isinstance(obj.get("controls"), list):
        raise ValueError("not a screenpad preset (no controls)")
    p = {"name": obj.get("name") or "preset",
         "screen": "portrait" if obj.get("screen") == "portrait" else "landscape",
         "controls": []}
    for c in obj["controls"]:
        c = dict(c)
        c.setdefault("type", "button")
        c.setdefault("w", c.get("size", 120))
        c.setdefault("h", c["w"])
        if c["type"] == "button":
            c.setdefault("shape", "circle")
        p["controls"].append(c)
    return p


def to_json(p):
    out = {"name": p["name"], "screen": p["screen"], "controls": []}
    for c in p["controls"]:
        o = {}
        for k in ("type", "shape", "x", "y", "w", "h", "radius", "color", "opacity", "label",
                  "action") + DIRS:
            v = c.get(k)
            if v is None or v == "":
                continue
            if k == "shape" and c["type"] != "button":
                continue
            if k == "radius" and (c["type"] != "button" or c.get("shape") != "rect"):
                continue
            o[k] = round(v, 2) if isinstance(v, float) else v
        out["controls"].append(o)
    return json.dumps(out, indent=2) + "\n"


# ---- drawing, as screenpad.c's draw_control ------------------------------------

def rounded_rect(cr, x0, y0, x1, y1, r):
    r = min(r, (x1 - x0) / 2, (y1 - y0) / 2)
    cr.new_sub_path()
    cr.arc(x1 - r, y0 + r, r, -math.pi / 2, 0)
    cr.arc(x1 - r, y1 - r, r, 0, math.pi / 2)
    cr.arc(x0 + r, y1 - r, r, math.pi / 2, math.pi)
    cr.arc(x0 + r, y0 + r, r, math.pi, 3 * math.pi / 2)
    cr.close_path()


def plus_path(cr, cx, cy, h, t, r):
    """the d-pad's plus, its arms' ends rounded"""
    pts = [(cx - t / 2, cy - h), (cx + t / 2, cy - h), (cx + t / 2, cy - t / 2),
           (cx + h, cy - t / 2), (cx + h, cy + t / 2), (cx + t / 2, cy + t / 2),
           (cx + t / 2, cy + h), (cx - t / 2, cy + h), (cx - t / 2, cy + t / 2),
           (cx - h, cy + t / 2), (cx - h, cy - t / 2), (cx - t / 2, cy - t / 2)]
    radii = [r, r, 0] * 4
    n = len(pts)
    cr.new_sub_path()
    for i in range(n):
        p, a, b = pts[i], pts[i - 1], pts[(i + 1) % n]
        ra = min(radii[i], math.dist(a, p) / 2)
        rb = min(radii[i], math.dist(b, p) / 2)
        da, db = math.dist(a, p), math.dist(b, p)
        s = (p[0] + (a[0] - p[0]) * ra / da, p[1] + (a[1] - p[1]) * ra / da)
        e = (p[0] + (b[0] - p[0]) * rb / db, p[1] + (b[1] - p[1]) * rb / db)
        cr.line_to(*s)
        cr.curve_to(p[0], p[1], p[0], p[1], *e)
    cr.close_path()


def label(cr, text, cx, cy, px, maxw, alpha):
    """text centred, px high from ascent to descent (DejaVu's is 1.164 em),
    narrowed to maxw"""
    if not text:
        return
    layout = PangoCairo.create_layout(cr)
    desc = Pango.FontDescription("DejaVu Sans Bold")
    desc.set_absolute_size(px / 1.164 * Pango.SCALE)
    layout.set_font_description(desc)
    layout.set_text(text, -1)
    w, h = layout.get_pixel_size()
    if w > maxw:
        desc.set_absolute_size(px / 1.164 * maxw / w * Pango.SCALE)
        layout.set_font_description(desc)
        w, h = layout.get_pixel_size()
    cr.set_source_rgba(1, 1, 1, alpha)
    cr.move_to(cx - w / 2, cy - h / 2)
    PangoCairo.show_layout(cr, layout)


def draw_control(cr, c):
    a = float(c.get("opacity", 0.4))
    ba, la = min(1, a * 1.8), min(1, a * 2.2 + 0.1)
    col = rgb(c.get("color"))
    hi = lighter(col, 0.45)
    x0, y0, w, h = c["x"], c["y"], c["w"], c["h"]
    cx, cy = x0 + w / 2, y0 + h / 2
    if c["type"] == "button":
        r = min(w, h) / 2 if c.get("shape", "circle") == "circle" else \
            min(c.get("radius", min(w, h) * 0.25), min(w, h) / 2)
        rounded_rect(cr, x0 + 1.5, y0 + 1.5, x0 + w - 1.5, y0 + h - 1.5, r)
        cr.set_source_rgba(*col, a)
        cr.fill_preserve()
        cr.set_source_rgba(*hi, ba)
        cr.set_line_width(3)
        cr.stroke()
        circle = c.get("shape", "circle") == "circle"
        label(cr, c.get("label", ""), cx, cy, min(w * 0.8, h * 0.42), w * (0.7 if circle else 0.8), la)
    elif c["type"] == "dpad":
        s = min(w, h)
        t, H = s / 3, s / 2
        plus_path(cr, cx, cy, H - 1.5, t, t * 0.25)
        cr.set_source_rgba(*col, a)
        cr.fill_preserve()
        cr.set_source_rgba(*hi, ba)
        cr.set_line_width(3)
        cr.stroke()
        k = t * 0.22
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            ax, ay = cx + dx * H * 0.75, cy + dy * H * 0.75
            cr.move_to(ax + dx * k, ay + dy * k)
            cr.line_to(ax - dx * k - dy * k, ay - dy * k + dx * k)
            cr.line_to(ax - dx * k + dy * k, ay - dy * k - dx * k)
            cr.close_path()
            cr.set_source_rgba(1, 1, 1, la)
            cr.fill()
    else:
        s = min(w, h)
        R, kr = s / 2, s * 0.21
        cr.arc(cx, cy, R - 1.5, 0, 2 * math.pi)
        cr.set_source_rgba(*col, a * 0.7)
        cr.fill_preserve()
        cr.set_source_rgba(*hi, ba)
        cr.set_line_width(3)
        cr.stroke()
        cr.arc(cx, cy, kr, 0, 2 * math.pi)
        cr.set_source_rgba(*col, min(1, a * 1.6))
        cr.fill_preserve()
        cr.set_source_rgba(*hi, ba)
        cr.set_line_width(2)
        cr.stroke()
        label(cr, c.get("label", ""), cx, cy, kr * 0.9, kr * 1.6, la)


def pretty(value):
    """an action as the panel shows it"""
    kind, _, name = (value or "").partition(":")
    if kind.lower() == "key":
        return "Key: " + KEY_NAMES.get(name.upper(), name.upper())
    if kind.lower() == "pad":
        return "Gamepad: " + dict(PAD).get(name.upper(), name.upper())
    return "Nothing" if not value else value


class Chooser(Gtk.Dialog):
    """Every action, as buttons a finger can hit: keys on one page, the
    gamepad's on the other.  Made once (a list of a hundred keys took the
    A5 four seconds as a combo box) and shown again for each choice."""

    def __init__(self, parent):
        super().__init__(title="What it does", transient_for=parent, modal=True)
        self.set_default_size(min(900, parent.get_allocated_width() - 40),
                              min(560, parent.get_allocated_height() - 40))
        self.value = None
        self.add_button("Nothing", 1)
        self.add_button("Cancel", Gtk.ResponseType.CANCEL)
        book = Gtk.Notebook()
        self.get_content_area().pack_start(book, True, True, 0)
        for title, items in (("Keyboard", [("key:" + k, KEY_NAMES.get(k, k)) for k in KEYS]),
                             ("Gamepad", [("pad:" + n, t) for n, t in PAD])):
            flow = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, homogeneous=True,
                               max_children_per_line=8, min_children_per_line=3)
            for v, t in items:
                btn = Gtk.Button(label=t)
                btn.connect("clicked", self.on_pick, v)
                flow.add(btn)
            sc = Gtk.ScrolledWindow()
            sc.set_policy(Gtk.PolicyType.NEVER, Gtk.PolicyType.AUTOMATIC)
            sc.add(flow)
            book.append_page(sc, Gtk.Label(label=title))
        self.book = book
        self.get_content_area().show_all()

    def on_pick(self, _b, v):
        self.value = v
        self.response(Gtk.ResponseType.OK)

    def choose(self, current):
        """the action chosen, "" for none, None if cancelled"""
        self.book.set_current_page(1 if (current or "").lower().startswith("pad:") else 0)
        self.value = None
        r = self.run()
        self.hide()
        return self.value if r == Gtk.ResponseType.OK else "" if r == 1 else None


# ---- the editor -----------------------------------------------------------------

class Editor(Gtk.Window):
    def __init__(self, name=None):
        super().__init__(title="screenpad editor")
        self.preset = normalise(default_preset())
        self.sel = -1
        self.drag = None
        self.press = None
        self.dirty = False
        self.trying = None
        self.updating = False
        self.set_default_size(1000, 700)
        self.connect("delete-event", self.on_close)

        css = Gtk.CssProvider()
        css.load_from_data(CSS)
        Gtk.StyleContext.add_provider_for_screen(Gdk.Screen.get_default(), css,
                                                 Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

        outer = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.add(outer)
        outer.pack_start(self.toolbar(), False, False, 0)
        # (a Paned and not a Box: it gives the panel exactly PANEL_W, where a
        # box gives it its natural width -- the colour swatches' row, twice
        # that -- and the screen beside it shrank at every selection)
        self.main = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        outer.pack_start(self.main, True, True, 0)

        self.canvas = Gtk.DrawingArea()
        self.canvas.set_can_focus(True)
        self.canvas.add_events(Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
                               Gdk.EventMask.POINTER_MOTION_MASK | Gdk.EventMask.KEY_PRESS_MASK)
        self.canvas.connect("draw", self.on_draw)
        self.canvas.connect("button-press-event", self.on_press)
        self.canvas.connect("motion-notify-event", self.on_motion)
        self.canvas.connect("button-release-event", self.on_release)
        self.canvas.connect("key-press-event", self.on_key)
        self.main.pack1(self.canvas, True, False)

        self.hint = Gtk.Label(xalign=0.5)
        self.hint.get_style_context().add_class("dim")
        outer.pack_start(self.hint, False, False, 4)

        scroll = Gtk.ScrolledWindow()
        # (EXTERNAL: no wider than it is given -- NEVER makes it as wide as
        # its rows' minimum)
        scroll.set_policy(Gtk.PolicyType.EXTERNAL, Gtk.PolicyType.AUTOMATIC)
        self.panel = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.panel.get_style_context().add_class("panel")
        scroll.add(self.panel)
        self.scroll = scroll
        self.main.pack2(scroll, False, True)
        self.connect("size-allocate", self.on_size)

        self.refresh_presets()
        if name:
            self.open_preset(name)
        self.render_panel()
        self.update_hint()
        self.maximize()

    # -- the top bar
    def toolbar(self):
        bar = Gtk.Box(spacing=6)
        bar.get_style_context().add_class("toolbar")
        self.presets = Gtk.ComboBoxText()
        self.presets.connect("changed", self.on_open)
        bar.pack_start(self.presets, False, False, 0)
        self.name = Gtk.Entry(placeholder_text="Preset name", width_chars=14)
        self.name.connect("changed", self.on_name)
        bar.pack_start(self.name, False, False, 0)
        self.land = Gtk.RadioButton.new_with_label_from_widget(None, "Landscape")
        self.port = Gtk.RadioButton.new_with_label_from_widget(self.land, "Portrait")
        for b in (self.land, self.port):
            b.set_mode(False)
            b.connect("toggled", self.on_screen)
        box = Gtk.Box()
        box.get_style_context().add_class("linked")
        box.pack_start(self.land, False, False, 0)
        box.pack_start(self.port, False, False, 0)
        bar.pack_start(box, False, False, 0)
        for text, kind in (("+ Button", "button"), ("+ D-pad", "dpad"), ("+ Stick", "stick")):
            b = Gtk.Button(label=text)
            b.connect("clicked", lambda _b, k=kind: self.add_control(k))
            bar.pack_start(b, False, False, 0)
        save = Gtk.Button(label="Save")
        save.get_style_context().add_class("suggested-action")
        save.connect("clicked", lambda _b: self.save())
        bar.pack_end(save, False, False, 0)
        try_ = Gtk.Button(label="▶ Try it")
        try_.connect("clicked", lambda _b: self.try_it())
        bar.pack_end(try_, False, False, 0)
        return bar

    def refresh_presets(self, select=None):
        self.updating = True
        self.presets.remove_all()
        self.presets.append("", "Open…")
        self.presets.append("+new", "New preset")
        for n in list_presets():
            self.presets.append(n, n)
        self.presets.set_active_id(select or "")
        self.updating = False

    def size(self):
        return (768, 1024) if self.preset["screen"] == "portrait" else (1024, 768)

    # -- the canvas
    def geometry(self):
        a = self.canvas.get_allocation()
        W, H = self.size()
        k = min((a.width - 40) / W, (a.height - 40) / H)
        k = max(k, 0.05)
        ox, oy = (a.width - W * k) / 2, (a.height - H * k) / 2
        return k, ox, oy

    def on_draw(self, _w, cr):
        W, H = self.size()
        k, ox, oy = self.geometry()
        cr.set_source_rgb(0.067, 0.075, 0.09)
        cr.paint()
        # the iPad's bezel, then the screen in its pixels
        rounded_rect(cr, ox - 14, oy - 14, ox + W * k + 14, oy + H * k + 14, 18)
        cr.set_source_rgb(0, 0, 0)
        cr.fill()
        cr.save()
        cr.translate(ox, oy)
        cr.scale(k, k)
        cr.rectangle(0, 0, W, H)
        cr.clip()
        g = cairo.LinearGradient(0, 0, 0, H)
        g.add_color_stop_rgb(0, 0.165, 0.29, 0.42)
        g.add_color_stop_rgb(1, 0.094, 0.188, 0.286)
        cr.set_source(g)
        cr.paint()
        cr.set_source_rgba(1, 1, 1, 0.05)
        cr.set_line_width(1 / k)
        for x in range(64, W, 64):
            cr.move_to(x, 0)
            cr.line_to(x, H)
        for y in range(64, H, 64):
            cr.move_to(0, y)
            cr.line_to(W, y)
        cr.stroke()
        cr.rectangle(0, H * 0.78, W, H * 0.22)
        cr.set_source_rgba(0.247, 0.42, 0.227, 0.55)
        cr.fill()
        for c in self.preset["controls"]:
            draw_control(cr, c)
        if 0 <= self.sel < len(self.preset["controls"]):
            c = self.preset["controls"][self.sel]
            cr.set_source_rgb(0.31, 0.55, 1)
            cr.set_line_width(2 / k)
            cr.set_dash([8 / k, 6 / k])
            cr.rectangle(c["x"] - 4, c["y"] - 4, c["w"] + 8, c["h"] + 8)
            cr.stroke()
            cr.set_dash([])
            hx, hy = c["x"] + c["w"] + 4, c["y"] + c["h"] + 4
            cr.arc(hx, hy, 16 / k, 0, 2 * math.pi)
            cr.set_source_rgb(0.31, 0.55, 1)
            cr.fill_preserve()
            cr.set_source_rgb(1, 1, 1)
            cr.set_line_width(3 / k)
            cr.stroke()
        cr.restore()

    def to_screen(self, ex, ey):
        k, ox, oy = self.geometry()
        return (ex - ox) / k, (ey - oy) / k, k

    def on_press(self, _w, ev):
        if ev.type != Gdk.EventType.BUTTON_PRESS:
            return True
        self.canvas.grab_focus()
        x, y, k = self.to_screen(ev.x, ev.y)
        cs = self.preset["controls"]
        # (the screen's place as it was at the press: selecting can relayout
        # the window, and the drag goes on in the same pixels)
        self.press = (ev.x, ev.y, self.geometry(), False)
        if 0 <= self.sel < len(cs):
            c = cs[self.sel]
            if math.hypot(x - (c["x"] + c["w"] + 4), y - (c["y"] + c["h"] + 4)) < 28 / k:
                self.drag = ("size", c, x, y, c["w"], c["h"])
                return True
        for i in range(len(cs) - 1, -1, -1):
            c = cs[i]
            if c["x"] <= x < c["x"] + c["w"] and c["y"] <= y < c["y"] + c["h"]:
                self.drag = ("move", c, x - c["x"], y - c["y"])
                if i != self.sel:
                    self.sel = i
                    self.render_panel()
                self.canvas.queue_draw()
                return True
        self.sel = -1
        self.render_panel()
        self.canvas.queue_draw()
        return True

    def on_motion(self, _w, ev):
        if not self.drag:
            return False
        px, py, (k, ox, oy), moving = self.press
        if not moving:			# a tap is not a drag: from 6 pixels on
            if math.hypot(ev.x - px, ev.y - py) < 6:
                return True
            self.press = (px, py, (k, ox, oy), True)
        x, y = (ev.x - ox) / k, (ev.y - oy) / k
        W, H = self.size()
        c = self.drag[1]
        if self.drag[0] == "move":
            c["x"] = max(-c["w"] / 2, min(W - c["w"] / 2, round((x - self.drag[2]) / SNAP) * SNAP))
            c["y"] = max(-c["h"] / 2, min(H - c["h"] / 2, round((y - self.drag[3]) / SNAP) * SNAP))
        else:
            w = max(48, round((self.drag[4] + x - self.drag[2]) / SNAP) * SNAP)
            h = max(48, round((self.drag[5] + y - self.drag[3]) / SNAP) * SNAP)
            if c["type"] != "button" or c.get("shape", "circle") == "circle":
                w = h = max(w, h)
            c["w"], c["h"] = w, h
        self.changed(panel=False)
        self.sync_numbers()
        return True

    def on_release(self, _w, _ev):
        self.drag = None
        return True

    def on_key(self, _w, ev):
        cs = self.preset["controls"]
        if not 0 <= self.sel < len(cs):
            return False
        c = cs[self.sel]
        step = 10 if ev.state & Gdk.ModifierType.SHIFT_MASK else 1
        k = Gdk.keyval_name(ev.keyval)
        moves = {"Left": (-step, 0), "Right": (step, 0), "Up": (0, -step), "Down": (0, step)}
        if k in moves:
            c["x"] += moves[k][0]
            c["y"] += moves[k][1]
            self.changed(panel=False)
            self.sync_numbers()
        elif k in ("Delete", "BackSpace"):
            self.remove()
        else:
            return False
        return True

    def on_size(self, _w, alloc):
        # the panel beside the screen, or under it on a narrow window
        narrow = alloc.width < 900
        want = Gtk.Orientation.VERTICAL if narrow else Gtk.Orientation.HORIZONTAL
        if self.main.get_orientation() != want:
            self.main.set_orientation(want)
        a = self.main.get_allocation()
        pos = (a.height - 330) if narrow else (a.width - PANEL_W)
        if pos > 100 and self.main.get_position() != pos:
            self.main.set_position(pos)

    def update_hint(self):
        scr = Gdk.Screen.get_default()
        land = scr.get_width() > scr.get_height()
        s = "Drag to move, the blue handle resizes; arrows nudge, Delete removes."
        if land != (self.preset["screen"] == "landscape"):
            s = ("The iPad is %s now: this preset shows when it is turned %s."
                 % ("landscape" if land else "portrait", self.preset["screen"]))
        self.hint.set_text(s)

    # -- changes
    def changed(self, panel=True):
        self.dirty = True
        self.canvas.queue_draw()
        if panel:
            self.render_panel()

    def add_control(self, kind):
        W, H = self.size()
        if kind == "button":
            c = {"type": kind, "shape": "circle", "w": 120, "h": 120, "color": "#3b82f6",
                 "opacity": 0.42, "label": "A", "action": "key:SPACE"}
        elif kind == "dpad":
            c = {"type": kind, "w": 260, "h": 260, "color": "#ffffff", "opacity": 0.32,
                 "up": "key:UP", "down": "key:DOWN", "left": "key:LEFT", "right": "key:RIGHT"}
        else:
            c = {"type": kind, "w": 240, "h": 240, "color": "#ffffff", "opacity": 0.3,
                 "action": "pad:LSTICK"}
        c["x"] = round((W - c["w"]) / 2 / SNAP) * SNAP
        c["y"] = round((H - c["h"]) / 2 / SNAP) * SNAP
        self.preset["controls"].append(c)
        self.sel = len(self.preset["controls"]) - 1
        self.changed()

    def remove(self):
        if 0 <= self.sel < len(self.preset["controls"]):
            del self.preset["controls"][self.sel]
            self.sel = -1
            self.changed()

    def duplicate(self):
        cs = self.preset["controls"]
        if 0 <= self.sel < len(cs):
            c = json.loads(json.dumps(cs[self.sel]))
            W, H = self.size()
            c["x"] = min(c["x"] + 24, W - c["w"])
            c["y"] = min(c["y"] + 24, H - c["h"])
            cs.append(c)
            self.sel = len(cs) - 1
            self.changed()

    def to_front(self):
        cs = self.preset["controls"]
        if 0 <= self.sel < len(cs):
            cs.append(cs.pop(self.sel))
            self.sel = len(cs) - 1
            self.changed()

    def on_name(self, e):
        if not self.updating:
            self.preset["name"] = e.get_text()
            self.dirty = True

    def on_screen(self, b):
        if self.updating or not b.get_active():
            return
        want = "landscape" if b is self.land else "portrait"
        if want == self.preset["screen"]:
            return
        W0, H0 = self.size()
        self.preset["screen"] = want
        W1, H1 = self.size()
        # each control kept by the screen's corner it is nearest
        for c in self.preset["controls"]:
            if c["x"] + c["w"] / 2 > W0 / 2:
                c["x"] += W1 - W0
            if c["y"] + c["h"] / 2 > H0 / 2:
                c["y"] += H1 - H0
            c["x"] = max(0, min(W1 - c["w"], c["x"]))
            c["y"] = max(0, min(H1 - c["h"], c["y"]))
        self.update_hint()
        self.changed()

    def load(self, obj):
        self.preset = normalise(obj)
        self.sel = -1
        self.dirty = False
        self.updating = True
        self.name.set_text(self.preset["name"])
        (self.port if self.preset["screen"] == "portrait" else self.land).set_active(True)
        self.updating = False
        self.update_hint()
        self.changed()
        self.dirty = False

    def confirm_leave(self):
        if not self.dirty:
            return True
        d = Gtk.MessageDialog(transient_for=self, modal=True, message_type=Gtk.MessageType.QUESTION,
                              buttons=Gtk.ButtonsType.YES_NO,
                              text="Leave the changes to \"%s\" unsaved?" % self.preset["name"])
        ok = d.run() == Gtk.ResponseType.YES
        d.destroy()
        return ok

    def open_preset(self, name):
        try:
            self.load(read_preset(name) if not name.endswith(".json") else json.load(open(name)))
        except (OSError, ValueError) as e:
            self.error("Could not open %s: %s" % (name, e))

    def on_open(self, combo):
        if self.updating:
            return
        n = combo.get_active_id()
        if not n:
            return
        if self.confirm_leave():
            if n == "+new":
                self.load(default_preset())
            else:
                self.open_preset(n)
        self.updating = True
        combo.set_active_id("")
        self.updating = False

    def save(self):
        name = slug(self.preset["name"])
        os.makedirs(USER, exist_ok=True)
        path = os.path.join(USER, name + ".json")
        try:
            with open(path + ".new", "w") as f:
                f.write(to_json(self.preset))
            os.replace(path + ".new", path)
        except OSError as e:
            self.error("Could not save: %s" % e)
            return
        self.dirty = False
        self.refresh_presets()
        self.hint.set_text("Saved as %s — `screenpad %s` runs it." % (path, name))

    def error(self, text):
        d = Gtk.MessageDialog(transient_for=self, modal=True, message_type=Gtk.MessageType.ERROR,
                              buttons=Gtk.ButtonsType.OK, text=text)
        d.run()
        d.destroy()

    # -- trying it: screenpad over the screen, a Stop button above it
    def try_it(self):
        if self.trying:
            return
        path = "/tmp/screenpad-try-%d.json" % os.getpid()
        with open(path, "w") as f:
            f.write(to_json(self.preset))
        try:
            proc = subprocess.Popen([SCREENPAD, path], stderr=subprocess.PIPE,
                                    stdin=subprocess.DEVNULL, start_new_session=True)
        except OSError as e:
            self.error("Could not run screenpad: %s" % e)
            return
        stop = Gtk.Window(type=Gtk.WindowType.TOPLEVEL, title="screenpad")
        stop.set_decorated(False)
        stop.set_keep_above(True)
        stop.set_skip_taskbar_hint(True)
        stop.set_type_hint(Gdk.WindowTypeHint.DIALOG)
        b = Gtk.Button(label="■  Stop")
        b.get_style_context().add_class("stop")
        b.get_style_context().add_class("destructive-action")
        b.connect("clicked", lambda _b: self.stop_trying())
        stop.add(b)
        scr = Gdk.Screen.get_default()
        stop.show_all()
        w, _h = stop.get_size()
        stop.move((scr.get_width() - w) // 2, 70)
        self.trying = (proc, stop, path)
        self.hide()
        GLib.child_watch_add(GLib.PRIORITY_DEFAULT, proc.pid, self.on_tried)

    def stop_trying(self):
        if self.trying:
            try:
                os.killpg(self.trying[0].pid, signal.SIGTERM)
            except OSError:
                pass

    def on_tried(self, _pid, status):
        proc, stop, path = self.trying
        self.trying = None
        stop.destroy()
        err = proc.stderr.read().decode(errors="replace") if proc.stderr else ""
        try:
            proc.wait(timeout=1)
        except subprocess.TimeoutExpired:
            pass
        try:
            os.unlink(path)
        except OSError:
            pass
        self.show_all()
        self.present()
        if proc.returncode not in (0, -signal.SIGTERM, None):
            msgs = [l for l in err.splitlines() if "preset" not in l and "the screen" not in l]
            self.error("screenpad stopped: " + ("\n".join(msgs[-3:]) or "exit %s" % proc.returncode))

    def on_close(self, *_a):
        self.stop_trying()
        if not self.confirm_leave():
            return True
        Gtk.main_quit()
        return False

    # -- the panel
    def render_panel(self):
        for w in self.panel.get_children():
            self.panel.remove(w)
        self.numbers = {}
        cs = self.preset["controls"]
        if not 0 <= self.sel < len(cs):
            t = Gtk.Label(label="Preset", xalign=0)
            t.get_style_context().add_class("title")
            self.panel.pack_start(t, False, False, 0)
            n = len(cs)
            text = ("%d control%s on a %s screen (%d × %d).\n\nAdd a button, a d-pad or a stick "
                    "from the top bar, then drag it into place. Select one to set its look "
                    "and what it does.\n\n▶ Try it runs the preset over the screen with a "
                    "Stop button above it; Save keeps it as `screenpad %s`."
                    % (n, "" if n == 1 else "s", self.preset["screen"], *self.size(),
                       slug(self.preset["name"])))
            l = Gtk.Label(label=text, xalign=0, wrap=True)
            l.set_max_width_chars(36)
            self.panel.pack_start(l, False, False, 0)
            self.panel.show_all()
            return
        c = cs[self.sel]
        title = Gtk.Label(label={"button": "Button", "dpad": "D-pad", "stick": "Stick"}[c["type"]],
                          xalign=0)
        title.get_style_context().add_class("title")
        self.panel.pack_start(title, False, False, 0)
        grid = Gtk.Grid(column_spacing=8, row_spacing=8)
        self.panel.pack_start(grid, False, False, 0)
        row = [0]

        def put(text, widget):
            l = Gtk.Label(label=text, xalign=0)
            l.get_style_context().add_class("dim")
            grid.attach(l, 0, row[0], 1, 1)
            widget.set_hexpand(True)
            grid.attach(widget, 1, row[0], 1, 1)
            row[0] += 1

        if c["type"] == "button":
            circle = Gtk.RadioButton.new_with_label_from_widget(None, "Circle")
            rect = Gtk.RadioButton.new_with_label_from_widget(circle, "Rounded")
            (circle if c.get("shape", "circle") == "circle" else rect).set_active(True)
            box = Gtk.Box()
            box.get_style_context().add_class("linked")
            for b, v in ((circle, "circle"), (rect, "rect")):
                b.set_mode(False)
                b.connect("toggled", lambda b, v=v: b.get_active() and self.set_field(c, "shape", v, True))
                box.pack_start(b, True, True, 0)
            put("Shape", box)
        if c["type"] != "dpad":
            e = Gtk.Entry(text=c.get("label", ""), placeholder_text="A, Jump, ☰ …", max_length=24)
            e.connect("changed", lambda e: self.set_field(c, "label", e.get_text()))
            put("Label", e)
        for key, text in (("x", "X"), ("y", "Y"), ("w", "Width"), ("h", "Height")):
            s = Gtk.SpinButton.new_with_range(-2000 if key in "xy" else 48, 2000, 1)
            s.set_value(c[key])
            s.connect("value-changed", lambda s, key=key: self.on_number(c, key, s))
            self.numbers[key] = s
            put(text, s)
        if c["type"] == "button" and c.get("shape") == "rect":
            sc = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0, max(1, min(c["w"], c["h"]) // 2), 1)
            sc.set_value(c.get("radius", min(c["w"], c["h"]) * 0.25))
            sc.connect("value-changed", lambda sc: self.set_field(c, "radius", sc.get_value()))
            put("Corners", sc)
        sw = Gtk.FlowBox(max_children_per_line=8, min_children_per_line=4,
                         selection_mode=Gtk.SelectionMode.NONE, homogeneous=True)
        for hexs in SWATCHES:
            b = Gtk.Button()
            b.get_style_context().add_class("swatch")
            prov = Gtk.CssProvider()
            prov.load_from_data(("button { background: %s; }" % hexs).encode())
            b.get_style_context().add_provider(prov, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
            b.connect("clicked", lambda _b, hexs=hexs: self.set_field(c, "color", hexs, True))
            sw.add(b)
        put("Colour", sw)
        rgba = Gdk.RGBA()
        rgba.parse(c.get("color", "#ffffff"))
        cb = Gtk.ColorButton.new_with_rgba(rgba)
        cb.connect("color-set", lambda cb: self.set_field(c, "color", "#%02x%02x%02x" % tuple(
            round(v * 255) for v in (cb.get_rgba().red, cb.get_rgba().green, cb.get_rgba().blue)), True))
        put("", cb)
        op = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 5, 100, 1)
        op.set_value(round(float(c.get("opacity", 0.4)) * 100))
        op.connect("format-value", lambda _s, v: "%d %%" % v)
        op.connect("value-changed", lambda s: self.set_field(c, "opacity", s.get_value() / 100))
        put("Opacity", op)

        sub = Gtk.Label(label="Does", xalign=0)
        sub.get_style_context().add_class("title")
        self.panel.pack_start(sub, False, False, 6)
        grid2 = Gtk.Grid(column_spacing=8, row_spacing=8)
        self.panel.pack_start(grid2, False, False, 0)
        row2 = [0]

        def put2(text, widget):
            l = Gtk.Label(label=text, xalign=0)
            l.get_style_context().add_class("dim")
            grid2.attach(l, 0, row2[0], 1, 1)
            widget.set_hexpand(True)
            grid2.attach(widget, 1, row2[0], 1, 1)
            row2[0] += 1

        def four():
            for d in DIRS:
                put2(DIR_LABELS[d], self.action_picker(c.get(d), lambda v, d=d: self.set_field(c, d, v)))
            box = Gtk.Box(spacing=4)
            for text, acts in DIR_PRESETS:
                b = Gtk.Button(label=text)
                b.connect("clicked", lambda _b, acts=acts: self.set_dirs(c, acts))
                box.pack_start(b, False, False, 0)
            put2("", box)

        if c["type"] == "button":
            put2("Action", self.action_picker(c.get("action"), lambda v: self.set_field(c, "action", v)))
        elif c["type"] == "dpad":
            four()
        else:
            mode = Gtk.ComboBoxText()
            mode.append("pad:LSTICK", "Gamepad left stick")
            mode.append("pad:RSTICK", "Gamepad right stick")
            mode.append("dirs", "Four directions")
            analogue = (c.get("action") or "").upper() in ("PAD:LSTICK", "PAD:RSTICK")
            mode.set_active_id("pad:" + c["action"].upper()[4:] if analogue else "dirs")
            mode.connect("changed", lambda m: self.set_stick(c, m.get_active_id()))
            put2("Stick", mode)
            if not analogue:
                four()

        tools = Gtk.Box(spacing=6)
        for text, fn, cls in (("Duplicate", self.duplicate, None), ("To front", self.to_front, None),
                              ("Delete", self.remove, "destructive-action")):
            b = Gtk.Button(label=text)
            if cls:
                b.get_style_context().add_class(cls)
            b.connect("clicked", lambda _b, fn=fn: fn())
            tools.pack_start(b, True, True, 0)
        self.panel.pack_start(tools, False, False, 10)
        self.panel.show_all()

    def action_picker(self, value, on_change):
        """a button saying what it does; the chooser on a tap"""
        b = Gtk.Button(label=pretty(value))
        b.set_halign(Gtk.Align.FILL)

        def pick(_b):
            v = self.chooser().choose(value)
            if v is not None:
                b.set_label(pretty(v))
                on_change(v or None)

        b.connect("clicked", pick)
        return b

    def chooser(self):
        if not getattr(self, "_chooser", None):
            self._chooser = Chooser(self)
        return self._chooser

    def set_field(self, c, key, value, panel=False):
        if value is None or value == "":
            c.pop(key, None)
        else:
            c[key] = value
        if key == "shape" and value == "circle":
            c["w"] = c["h"] = max(c["w"], c["h"])
        self.changed(panel=panel)

    def set_dirs(self, c, acts):
        for d, a in zip(DIRS, acts):
            c[d] = a
        self.changed()

    def set_stick(self, c, mode):
        if mode == "dirs":
            c.pop("action", None)
            for d, a in zip(DIRS, DIR_PRESETS[0][1]):
                c.setdefault(d, a)
        else:
            c["action"] = mode
            for d in DIRS:
                c.pop(d, None)
        self.changed()

    def on_number(self, c, key, spin):
        if self.updating:
            return
        v = spin.get_value()
        c[key] = v
        if key in ("w", "h") and (c["type"] != "button" or c.get("shape", "circle") == "circle"):
            c["w"] = c["h"] = v
            self.sync_numbers()
        self.changed(panel=False)

    def sync_numbers(self):
        cs = self.preset["controls"]
        if not 0 <= self.sel < len(cs):
            return
        c = cs[self.sel]
        self.updating = True
        for k, s in self.numbers.items():
            s.set_value(c[k])
        self.updating = False


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else None
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    win = Editor(name)
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
