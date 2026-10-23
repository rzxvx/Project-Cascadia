# The GPU: OpenGL on the SGX543MP2, and a demo of it

The iPad mini 1's PowerVR SGX543MP2 draws under Linux. Programs reach it
through **Mesa**: a Gallium driver of this repository's ([mesa/](../mesa/)),
the system's OpenGL -- OpenGL ES 1.1 and 2.0, desktop OpenGL 2.1 with GLSL
1.20 (its compiler takes GLSL 1.30's integers since M44), through EGL, GLX
and GBM. XFCE draws with it (glamor), weston and its clients, SuperTux
through SDL's GL renderer. **`tux-gpudemo`** shows it off: Tux going round
on the screen, his light baked, the logo over his head glowing.

Under Mesa sits `sgx2d`, the small 2D library the GPU was first brought up
with (textured quads, colour modulation, three blend modes, full screen
straight into the framebuffer), its frame the template Mesa's own frames are
built from.

How it was found, step by step and with every wrong turn, is
[research/p105-gpu.md](research/p105-gpu.md) (the GPU, sgx2d) and
[research/p105-mesa.md](research/p105-mesa.md) (Mesa and OpenGL). This page
is how to use it and how the pieces fit.

## Setup

With the iPad running this port from an NFS root (`./cascadia nfs on`):

```bash
./cascadia gpu
```

```bash
./cascadia mesa
```

Then, on the iPad: `tux-gpudemo`.

Nothing from the iPad's iOS is needed, only the IPSW the port is built from
anyway. `./cascadia gpu` builds and installs the GPU's pack, in
`/usr/local/lib/sgx2d`:

- it runs `./cascadia firmware` if it has not been run: besides the boot
  chain and the GPU's microkernel, it takes two more things out of the IPSW
  -- **the decrypted kernelcache**, whose GPU kext code is run in an
  emulator to compute the kernel's per-frame data, and **one PDS template
  from iOS's GL driver** (the 3D pass's event program, 168 bytes read out of
  the dyld shared cache, hash-checked). Both are Apple's, so neither is in
  this repository.
- it builds the pack -- the template frame and sgx2d's demos -- in the same
  `cascadia-build` image as the kernel: no ARM emulation, and Apple Silicon
  and x86_64 hosts give byte-identical results. The GPU programs and the
  frame's state are this repository's own (see "How it works").
- it installs it: over ssh into the running iPad's root, or, if the iPad is
  not up, into the root this host exports.

`./cascadia mesa` then builds Mesa with the driver (the first time takes
the better part of an hour, under ARM emulation) and makes it the iPad's
OpenGL, the demo with it: [mesa/README.md](../mesa/README.md).

Needs a kernel with the `apple-sgx` driver and its render node from this
tree (`./cascadia build`); `./cascadia gpu` warns if the running one has
none. Run `./cascadia gpu` again after updating the kernel.

**By hand**: `./cascadia gpu --install [HOST]` (over ssh only) or
`./cascadia gpu --root [DIR]` (into a root tree only), the same for
`./cascadia mesa`. The NFS root lives on the host that exports it: a device
booted from another host's root needs them installed there too. Everything
built stays in `build/`, out of git.

## The demo

On the device's console (the on-screen keyboard), type `tux-gpudemo`. Over
ssh, use the full path, `/usr/local/bin/tux-gpudemo` (a non-login ssh shell
does not have `/usr/local/bin` in its `PATH`). It needs the screen to
itself: not under X.

The camera goes round Tux once in 12 s, at the height the artist's camera
has. Leave with the **EXIT** button in the top right corner: touch it and
let go. While the demo runs the text console is unbound from the
framebuffer and `fbkeyboard` is stopped (both draw on the screen); however
it ends -- EXIT, Ctrl-C over ssh, kill -- both come back, `fbkeyboard`
started the way stage 2 starts it.

If the keyboard ever does not come back:

```bash
setsid sh -c 'exec fbkeyboard $(cat /run/fbkeyboard.args)' >/dev/null 2>&1 &
```

Options: `tux-gpudemo [SECONDS [PERIOD]]` (a run of SECONDS, a turn in
PERIOD), and as environment variables in front of it:

- `TUX_BLOOM=0` — no glow; `TUX_BLUR=2` — a cheaper one (two renders a
  frame fewer, a faint ring)
- `TUX_NOFLIP=1` — frames drawn but not shown: the GPU's own pace
- `TUX_SHOT=file.ppm` — the last frame of a timed run, as a picture
- `TUX_TOUCH_FLIP=x|y|xy` — if touches land mirrored

On the M44 driver it draws **57-58 frames a second** with the glow and 129
without (768 x 1024, 10 572 triangles, a 2048 x 2048 texture); unshown, 69
and 202. The glow is five renders of its own a frame (the logo alone into a
128 x 128 target, then blurred across and up twice), and a render costs the
SGX some 2 ms whatever its size.

**The scene** is [teutekeune](../README.md#credits)'s Tux, modelled in
Blender. `tools/sgx/tux/make_tux.py`, run headless on a copy of the
`.blend`, bakes its light in Cycles -- the lamps inside the logo's glass, as
the render has them -- into a 2048 x 2048 texture on a second UV map, the
whole colour, display-referred: the demo draws it as it is, unlit, one
lookup a pixel; the logo becomes an emissive material. Out come
`tools/sgx/tux/assets/tux.glb` (read by cgltf) and `tux-baked.png` (by
stb_image); `preview_tux.py` renders what the demo will show next to the
Cycles render.

sgx2d's own demos are in `/usr/local/lib/sgx2d`: `sprites
/usr/local/lib/sgx2d 200 600` (bouncing sprites, prints fps) and `demo2
/usr/local/lib/sgx2d` (streaming texture, fills, blend modes, rotation);
`sgxinfo` shows what the render node reports and checks that its buffers
map where they should.

## How it works

```
a program ── OpenGL ES / OpenGL ──> Mesa: the sgx Gallium driver
                                         │   NIR compiled to USSE code; each
                                         │   render built from the template frame
                                         v
              /dev/dri/renderD128   (sgx2d's demos: straight to it)
                                         │   buffers at the pack's GPU addresses,
                                         │   mmapped; a submit per render
                                         v
apple-sgx ── render queue: context + CCB + TA command ──> iOS's microkernel
                                         │   TA (geometry -> tiles) then 3D
                                         v   (tiles -> pixels -> target)
                              the target, or the screen through KMS
```

- **The microkernel** is Apple's, from the IPSW; the driver boots it with
  iOS's own register sequence and talks to it through the same command
  buffers iOS uses. It manages the parameter buffer (the tiler's memory)
  itself.
- **The GPU programs are ours.** sgx2d's 16 USSE programs (vertex shader,
  one pixel shader per blend mode, end of tile, background reload, state
  loaders) are `tools/sgx/programs.py`, assembled by `tools/sgx/usse.py`;
  `tools/sgx/pds.py` builds the PDS programs (the data movers that start
  them) by shape, as iOS's GL driver does; `tools/sgx/frame.py` lays out the
  frame's state. All of it was checked word by word against what iOS's GL
  driver sets up for the same quad, and on the device frame by frame. The
  one exception is the 3D pass's 22-instruction event program, which the GL
  driver keeps as a template; it comes from the IPSW. Mesa's programs are
  its compiler's (`mesa/files/.../sgx_compiler.c`, NIR to USSE).
- **The kernel's part of a frame** (render-target buffers, the 3D register
  block) was first computed by running the kext's own code in an emulator,
  once, when the pack is built; Mesa computes it itself for any target size
  (M10).
- **Textures** are in the GPU's twiddled layout (Morton order; a rectangle
  is a row of Morton squares). GPU memory outside the driver's own buffers
  is allocated page by page, so it is not limited by the 64 MiB CMA pool.
- **A render's end** is seen in memory: the microkernel clears a word of the
  render target's details when the 3D pass is over. The kernel watches it
  and signals the render's fence, so the CPU builds the next while the GPU
  draws this one.
- **The overlay**: a buffer of the render node that the kernel blends into
  each copy onto the screen, turned with it (a second quad in the 2D
  engine's job, a SOP2 pixel program; the render node's `OVERLAY` ioctl).
  `screenpad`'s touch controls are drawn with it, over X, over a game that
  bypasses the compositor, and over KMS programs:
  [research/p105-screenpad.md](research/p105-screenpad.md).
- **The render node** (`/dev/dri/renderD128`, driver `apple_sgx`) is how
  Mesa and sgx2d reach the GPU, side by side: each of the pack's GPU memory
  windows is a buffer at that address, mapped into the process, and a
  render is one submit. [research/p105-mesa.md](research/p105-mesa.md), M9,
  has the interface.

## Limits

- OpenGL 2.1, not 3: what is missing is in
  [research/p105-mesa.md](research/p105-mesa.md) (M42's plan, M43, M44).
- sgx2d has no render targets and no custom shaders, and about 390 draws
  fit in one of its frames: it is a test bench now, the programs' path is
  Mesa's.

## The pieces

| file | what |
|---|---|
| `patches/files/drivers/gpu/drm/apple-sgx/` | the kernel driver: power, clocks, MMU, microkernel, queues, debugfs (`apple_sgx_hw.c`), the render node (`apple_sgx_drm.c`) |
| `patches/files/include/uapi/drm/apple_sgx_drm.h` | the render node's interface |
| `mesa/` | the Mesa driver, its build and install (`./cascadia mesa`): [mesa/README.md](../mesa/README.md) |
| `tools/sgx/tux/` | the GPU demo: `tuxorbit.c`, its launcher `tux-gpudemo.sh`, the scene (`assets/`), its bake (`make_tux.py`, `preview_tux.py`) |
| `tools/sgx/lib/sgx2d.{c,h}` | the 2D library |
| `tools/sgx/mkpack.py` | build and install the pack (`./cascadia gpu`) |
| `tools/sgx/programs.py`, `usse.py` | the USSE programs, and their assembler |
| `tools/sgx/pds.py` | the PDS programs |
| `tools/sgx/frame.py` | the template frame: state, layout, render command |
| `tools/sgx/rpack.py`, `rgen.py` | the pack, from the frame |
| `tools/sgx/lib/cross.sh` | the device's binaries, cross-built against Alpine's armhf packages |
| `tools/iosgpu/rtemu.py` | runs the kext's render-target code under unicorn |
| `tools/sgx/usse-probe.py` | USSE instructions tried on the GPU, one program each (M44) |
| `tools/sgx/capmap.py` | research: what of a frame a capture of iOS reaches |
| `tools/gpucap.sh`, `tools/iosgpu/gltrace.m` | research: capture what iOS's GL driver sets up (jailbroken iOS; `frame.py --check` compares) |
| `tools/sgx/lib/sgxinfo.c` | the render node's parameters, and a check of its buffers |
| `tools/sgx/lib/{rectest,pbtest,cnttest,bigtest,replay}.c` | diagnostics |
