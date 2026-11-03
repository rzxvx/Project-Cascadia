# P105 screenpad — on-screen game controls, drawn by the kernel (2026-10-23)

`screenpad` puts a game controller on the glass: the buttons, d-pads and
sticks of a preset the user makes, semi-transparent over whatever runs, and
worked by touch. `screenpad-editor`, a GTK window in XFCE, makes the presets
on the iPad itself.

## Where it is drawn

A fullscreen game under XFCE bypasses the compositor (M40's xfwm4 patch):
its window is unredirected and xfwm4's overlay is unmapped, so nothing
composites over it. An X window on top would stop the bypass, at M40's cost
in frames, or, with the bypass kept, show no translucency at all. A program
on KMS (`tux-gpudemo`, SDL's KMSDRM) has no X at all.

Every frame reaches the glass through the kernel: apple-sgx's KMS copies the
plane onto the scanout memory with the 2D engine (M16, turned by M41). So
the controls are drawn there, **in the same job as the copy**:

- **The job**: the background object, then the copy's quad (the plane's
  framebuffer, turned), then a second quad: the overlay's box, turned the
  same way, in a pixel program that blends. That program is sgx2d's
  `pixel_blend` SOP2 without its modulate: `o0 = pa0 * a + o0 * (1 - a)`
  (`SGX_BLT_PROG_BLEND`). The region's control stream gets a second
  (header, object) pair before its end word. The copy costs a quad more,
  and only where the overlay is.
- **The interface** is a render-node ioctl, `DRM_IOCTL_APPLE_SGX_OVERLAY`.
  It takes a buffer of the render node (BGRA, straight alpha) the plane's
  size, in the plane's own coordinates, and up to 16 rectangles saying
  where the overlay is; outside them the buffer has to be transparent.
  It is root only and one file at a time, and goes when that file closes,
  so a crashed `screenpad` leaves nothing on the screen. `QUERY` returns
  the plane's size and turn, which is how `screenpad` follows `rotate`.
  It is not a KMS plane: only the DRM master sets planes, and X is the
  master.
- **The composition**: each damaged rectangle's copy draws the box of the
  overlay's rectangles that meet it (`sgx_kms_over()`). Setting the overlay
  copies again the screen under its old and its new rectangles. With
  nothing on KMS (the text console) it is not shown.

### Three wrong turns

1. **A blend over what the screen has.** First the overlay was a job of its
   own after each copy, blending over the destination: the job's background
   object reads it. The boxes came out full of pieces of the other boxes.
   With an overlay of alpha 0, the box showed a flat colour and a sheared
   piece of the desktop's diagonal line instead of the icons under it. The
   background object does not read the destination pixel for pixel: its
   triangle (0, 0), (0, 2h), (2w, 0) takes the texture's corners in order,
   as a quad does, giving a squeezed, sheared copy. A copy or a fill covers
   it entirely, so it never showed before.
2. **Two quads, both opaque.** Then the job drew the destination onto itself
   1:1 with a quad (the quad's mapping is proven) and the overlay over that.
   A red overlay at alpha 0.5 then came out in blocks of every strength, from
   0.5 up to almost solid. The SGX is a deferred renderer: per pixel the ISP
   shades only the front object, unless the object is marked as blending.
   Every transfer quad's ISP word is `0x01d80000` (compare ALWAYS, opaque),
   so the first quad was dropped and the overlay blended over the
   background object's read, round after round. The overlay's quad needs
   bit 25 as well, the bit sgx2d's translucent quads carry:
   `0x03d80000` (`overlay_isp`, a module parameter, writable, for trying
   others without a new kernel). With it, and the framebuffer as the first
   quad instead of the screen, nothing reads the screen at all.
3. **The interface's version.** The ioctl was first announced as
   `APPLE_SGX_PARAM_UAPI_VERSION` 4. Mesa (`sgx_device.c`) refuses a version
   it does not know, and gallium fell back to softpipe: XFCE drew in
   software, Xorg took 172 s of CPU in 15 minutes ("glamor X acceleration
   enabled on softpipe" in its log). The version stays 3, since the overlay
   only adds and Mesa does not use it; `screenpad` asks for the ioctl itself
   (QUERY, `-EINVAL` on a kernel without it). The header says so.

### Checked

Pixel by pixel, the screen read back through `/dev/fb0` before and with an
overlay rectangle (`tools/screenpad/ovtest.c`, `tools/desktop/xshot.py
--screen`):

| overlay | pixels changed | against the blend computed on the CPU |
|---|---|---|
| alpha 0, 192 x 224 over the desktop's icons | 0 of 43 008 | — |
| red, alpha 128, the same box | 43 008 of 43 008 | worst difference 0 |

The SuperTux preset over the desktop and over the game, turned left: the
controls stay unchanged while X redraws under them (the cursor moved across
them, a window opened under them).

**What it costs.** SuperTux 0.7.0's main menu, fullscreen, `SGX_DEBUG=fps`,
40 s each, one run after the other: 38.1–40.4 fps (about 39.5) without the
controls, 38.1–39.9 (about 38.7) with them, about 2%.

## The program

`tools/screenpad/screenpad.c`, static, one file (stb_truetype for the
labels, DejaVu Sans Bold from the system):

- **Drawing**: on the CPU into a canvas the screen's size, with
  signed-distance shapes (anti-aliased circles, rounded rectangles, the
  d-pad's plus as one shape), then copied into the overlay's buffer. A
  press redraws only that control's box, clipped to it, and the controls
  that reach into it.
- **Touch**: the touchscreen is grabbed while the controls show. A finger
  that lands on a control belongs to `screenpad`. On a button it presses
  whichever button it is over, so it can slide from one to the next; on a
  d-pad or a stick it works that control wherever it goes (eight ways, a
  dead middle; a stick's knob follows it). Every other finger goes on to
  `screenpad touch`, a uinput touchscreen whose coordinates are already the
  turned screen's (0..4095). X maps a new touchscreen onto its whole screen,
  so it needs no matrix of its own and `rotate` leaves it alone.
- **Output**: `screenpad keys`, a uinput keyboard, and a uinput Xbox 360 pad
  (xpad's IDs, buttons and axes), which SDL maps as one.
- **Turns**: every 250 ms it asks the kernel for the plane. When the screen
  is turned the preset's way (landscape or portrait) and something is on
  KMS, the controls show. Otherwise they hide and the grab is let go.
- **`-- COMMAND`**: runs a game with the controls and ends with it.

Checked with `tools/screenpad/touchtest.py`: a uinput touchscreen handed to
`screenpad` (`SCREENPAD_TOUCH`; the real one cannot be faked, as `screenpad`
grabs it and the kernel drops what another file writes into a grabbed
device), the output read back:

| fingers | keys | X |
|---|---|---|
| tap Jump | SPACE down, up | — |
| tap Menu | ESC down, up | — |
| d-pad left, slide up, let go | LEFT down, UP down, LEFT up, UP up | — |
| Jump, slide onto Run, let go | SPACE down, up, LEFTCTRL down, up | — |
| Run and d-pad right together | LEFTCTRL down, RIGHT down, both up | — |
| a tap on the game at (500, 300) | — | a touch at 2000, 1600 of 4095 |

The gamepad preset: A, Y, Start; the d-pad as the hat (`HAT_Y` 1 for
down); the stick 0 in the middle, `X` 29 864 pushed right, (−22 757,
−23 575) far up-left (held to the circle), 0 again let go. In SuperTux the
d-pad moves the main menu's selection.

## Presets and the editor

```json
{ "name": "SuperTux", "screen": "landscape", "controls": [
  { "type": "dpad", "x": 36, "y": 440, "w": 290, "h": 290, "color": "#ffffff",
    "opacity": 0.32, "up": "key:UP", "down": "key:DOWN", "left": "key:LEFT",
    "right": "key:RIGHT" },
  { "type": "button", "shape": "circle", "x": 866, "y": 590, "w": 132, "h": 132,
    "color": "#3b82f6", "opacity": 0.42, "label": "Jump", "action": "key:SPACE" } ] }
```

The types are `button` (`circle` or `rect` with a `radius`), `dpad` and
`stick`. Actions are `key:NAME` (Linux's `KEY_` names) and `pad:A B X Y LB RB
LT RT BACK START GUIDE LS RS UP DOWN LEFT RIGHT`. A stick's analogue actions
are `pad:LSTICK` / `pad:RSTICK`; with four actions instead it works as a
d-pad that follows the finger. Coordinates are the screen's pixels,
1024 x 768 or 768 x 1024. `presets/` ships `supertux` (arrows, Space, Ctrl,
Esc) and `gamepad`. A user's presets are in `~/.config/screenpad` and come
first.

**`screenpad-editor`** (`tools/screenpad/screenpad-editor.py`, GTK 3 through
PyGObject) is a window laid out as its web mock-up was: the screen with
the controls drawn as `screenpad` draws them, a panel with the selected
control's shape, label, place and size, colour (swatches, or GTK's chooser),
opacity and action. Save writes `~/.config/screenpad/NAME.json`. "Try it"
runs `screenpad` with the preset over the screen and a Stop button above it;
the touches off the controls still reach X, so the button can be pressed.

Two things the A5 taught it:

- **A list is expensive.** The action was first two combo boxes, kind and
  which. GTK builds the menu of a combo box with a wrap width whole: a
  hundred keys took **4.1 s**, and a d-pad has four. The action is now a
  button that opens a chooser (a dialog of buttons, keyboard and gamepad
  pages), built once in 0.16 s and shown again; the panel takes 0.1 s.
- **A panel's width.** In a box, the panel took its rows' natural width (the
  colour swatches' row, some 650 px) at every selection and squeezed the
  screen. In a `Gtk.Paned` with the divider set, it has exactly 330 px.
