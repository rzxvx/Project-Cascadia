# The GPU: SuperTux at 60 fps on the SGX543MP2

The iPad mini 1's PowerVR SGX543MP2 draws under Linux. There is no GL driver:
what exists is a 2D drawing library, `sgx2d` (textured quads, colour
modulation, three blend modes, full screen straight into the framebuffer),
and `sgxsdl`, a shim that runs SDL2's 2D renderer on it. SuperTux 0.6.3 runs
through it at a stable 60 fps, played with a touch gamepad.

How it was found, step by step and with every wrong turn, is
[research/p105-gpu.md](research/p105-gpu.md). This page is how to use it and
how the pieces fit.

## Setup

With the iPad running this port from an NFS root (`./cascadia nfs on`) and
the host sharing its internet (`./cascadia net on`):

```bash
./cascadia gpu
```

Then, on the iPad: `supertux-gpu`.

Nothing from the iPad's iOS is needed, only the IPSW the port is built from
anyway. That one command:

- runs `./cascadia firmware` if it has not been run: besides the boot chain
  and the GPU's microkernel, it takes two more things out of the IPSW --
  **the decrypted kernelcache**, whose GPU kext code is run in an emulator
  to compute the kernel's per-frame data, and **one PDS template from iOS's
  GL driver** (the 3D pass's event program, 168 bytes read out of the dyld
  shared cache, hash-checked). Both are Apple's, so neither is in this
  repository.
- builds the pack, in the same `cascadia-build` image as the kernel: no ARM
  emulation, and Apple Silicon and x86_64 hosts give byte-identical results.
  The GPU programs and the frame's state are this repository's own (see
  "How it works").
- installs it: over ssh into the running iPad's root, with SuperTux itself
  from `apk`; or, if the iPad is not up, into the root this host exports
  (SuperTux is then added the next time the command runs with the iPad up).

Needs a kernel with the `apple-sgx` driver from this tree, build #263 or
later (`./cascadia build`); the command warns if the running one has none.

**By hand**: `./cascadia gpu --install [HOST]` (over ssh only) or
`./cascadia gpu --root [DIR]` (into a root tree only). The NFS root lives on
the host that exports it: a device booted from another host's root needs it
installed there too. Everything built stays in `build/sgx2d`, out of git.

## Play

On the device's console (the on-screen keyboard), type `supertux-gpu`. Over
ssh, use the full path, `/usr/local/bin/supertux-gpu` (a non-login ssh shell
does not have `/usr/local/bin` in its `PATH`).

Hold the iPad in landscape. The buttons are drawn over the game and light up
while touched:

| where | button |
|---|---|
| bottom left | ← → (with ↑ above and ↓ below) |
| bottom right | jump (the big one), action to its left |
| top right | Esc (menu) and Enter |

Leave with Esc → Quit. While the game runs the text console is unbound from
the framebuffer and `fbkeyboard` is stopped (both draw on the screen, and
`fbkeyboard` would type touches meant for the game into the console); when the
game exits both come back, `fbkeyboard` started the way stage 2 starts it.

If the keyboard ever does not come back:

```bash
setsid sh -c 'exec fbkeyboard $(cat /run/fbkeyboard.args)' >/dev/null 2>&1 &
```

Options, as environment variables in front of `supertux-gpu`:

- `SGXSDL_FPS=1` — print the frame rate every 100 frames
- `SGXSDL_TOUCH_FLIP=x|y|xy` — if touches land mirrored
- `SGXSDL_PAD=0` — no gamepad
- `SGXSDL_ROTATE=0` — no rotation (portrait; then pass `--geometry 768x1024`)

Two demos come along: `/usr/local/lib/sgx2d/sprites /usr/local/lib/sgx2d 200 600`
(bouncing sprites, prints fps) and `/usr/local/lib/sgx2d/demo2
/usr/local/lib/sgx2d` (streaming texture, fills, blend modes, rotation).

## How it works

```
SuperTux ── SDL2 2D renderer calls ──> libsgxsdl.so (LD_PRELOAD)
                                         │  textures, quads, colour, blend mode
                                         v
                                       sgx2d ── builds each frame:
                                         │   VDM control stream, per-draw state
                                         │   deltas, vertex fetch blocks, vertices
                                         v
                    /sys/kernel/debug/apple-sgx/{mem,cmd}
                                         │   pwrite at the GPU address; "rkick"
                                         v
apple-sgx.c ── render queue: context + CCB + TA command ──> iOS's microkernel
                                         │   TA (geometry -> tiles) then 3D
                                         v   (tiles -> pixels -> framebuffer)
                                      the screen
```

- **The microkernel** is Apple's, from the IPSW; the driver boots it with
  iOS's own register sequence and talks to it through the same command
  buffers iOS uses. It manages the parameter buffer (the tiler's memory)
  itself.
- **The GPU programs are ours.** `tools/sgx/programs.py` is the source of
  the 16 USSE programs a frame runs (vertex shader, one pixel shader per
  blend mode, end of tile, background reload, state loaders), assembled by
  `tools/sgx/usse.py`; `tools/sgx/pds.py` builds the PDS programs (the data
  movers that start them) by shape, as iOS's GL driver does; `tools/sgx/
  frame.py` lays out the frame's state. All of it was checked word by word
  against what iOS's GL driver sets up for the same quad, and on the device
  frame by frame. The one exception is the 3D pass's 22-instruction event
  program, which the GL driver keeps as a template; it comes from the IPSW.
- **A frame** is the first draw carrying the whole GPU state and every
  later draw only a 4-word delta naming the PDS block that loads its texture.
- **Blend modes** are three versions of one 3-instruction pixel shader (on
  the SGX the fragment program does the blending); only the factors of its
  last instruction differ.
- **The kernel's part of a frame** (render-target buffers, the 3D register
  block) is computed by running the kext's own code in an emulator, once,
  when the pack is built.
- **Textures** are RGBA8 in the GPU's twiddled layout (Morton order; a
  rectangle is a row of Morton squares), at power-of-two sizes, in a 64 MiB
  heap. GPU memory outside the driver's own buffers is allocated page by
  page, so it is not limited by the 64 MiB CMA pool.
- **A frame's end** is seen in memory: the microkernel clears a word of the
  render target's details when the 3D pass is over. `sgx2d_end()` waits for
  the previous frame there, so the CPU builds frame N+1 while the GPU draws
  frame N.

## Limits

- No render targets: SuperTux's lightmap reads as white, so dark levels are
  not darkened.
- Blend mode NONE is drawn as BLEND (the same for opaque textures).
- No custom shaders — that would need a GLSL to USSE compiler — so no
  OpenGL: programs that use SDL's 2D renderer work, GL programs do not.
- No sound (there is no audio device yet).
- It runs as root, through debugfs. A proper device node (submit and wait
  ioctls, mmap) is the next step for anything beyond games.
- About 390 draws (texture or blend-mode changes) fit in one frame; quads
  beyond that are dropped. SuperTux's title screen uses ~60.

## The pieces

| file | what |
|---|---|
| `patches/files/drivers/misc/apple-sgx.c` | the kernel driver: power, clocks, MMU, microkernel, queues, debugfs |
| `tools/sgx/lib/sgx2d.{c,h}` | the 2D library |
| `tools/sgx/lib/sgxsdl.c` | the SDL2 renderer shim and the touch gamepad |
| `tools/sgx/lib/supertux.sh` | the launcher (`supertux-gpu`) |
| `tools/sgx/mkpack.py` | build and install (`./cascadia gpu`) |
| `tools/sgx/programs.py`, `usse.py` | the USSE programs, and their assembler |
| `tools/sgx/pds.py` | the PDS programs |
| `tools/sgx/frame.py` | the template frame: state, layout, render command |
| `tools/sgx/rpack.py`, `rgen.py` | the pack sgx2d loads, from the frame |
| `tools/sgx/lib/cross.sh` | the device's binaries, cross-built against Alpine's armhf packages |
| `tools/iosgpu/rtemu.py` | runs the kext's render-target code under unicorn |
| `tools/sgx/capmap.py` | research: what of a frame a capture of iOS reaches |
| `tools/gpucap.sh`, `tools/iosgpu/gltrace.m` | research: capture what iOS's GL driver sets up (jailbroken iOS; `frame.py --check` compares) |
| `tools/sgx/lib/{rectest,pbtest,cnttest,bigtest,replay,sdlshot}.c` | diagnostics |
