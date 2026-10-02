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

That one command gets whatever it is missing, and then builds and installs:

- **the decrypted iOS 8.4.1 kernelcache**, out of your IPSW
  (`./cascadia firmware`, if not run yet). The GPU kext's own code is run
  from it in an emulator, to compute the kernel's per-frame data.
- **the 2D templates, once, from the iPad's jailbroken iOS** (`./cascadia
  gpucap`): a prebuilt helper, `gltrace`, draws a textured quad with OpenGL
  ES there and saves the GPU memory iOS's GL driver set up for it; its
  compiled shaders and command templates are what the GPU runs under Linux.
  `./cascadia flash --kdfu` takes them on the way by itself, while iOS is up.
  Otherwise, if the iPad is in Linux when they are needed, the command says
  so: boot it into iOS once and run it again.
- **the build**, in the same `cascadia-build` image as the kernel: no ARM
  emulation, and Apple Silicon and x86_64 hosts give byte-identical results.
- **the install**: over ssh into the running iPad's root, with SuperTux
  itself from `apk`; or, if the iPad is not up, into the root this host
  exports (SuperTux is then added the next time the command runs with the
  iPad up).

The kernelcache and the templates are Apple's code and data, so neither is
in this repository; both come from what every user of this port already has.
iOS places its GL buffers differently from run to run, so the templates are
found by content and their pointers moved to one reference layout
(`tools/sgx/capture-layout.json`: offsets and hashes, no Apple data); a
capture it cannot place is refused with a message, not half-used.

Needs a kernel with the `apple-sgx` driver from this tree, build #263 or
later (`./cascadia build`); the command warns if the running one has none.

**By hand**, the pieces separately: `./cascadia gpucap` (iPad in iOS; over
Wi-Fi with `IOS_HOST=<its IP>`), `./cascadia gpu --install [HOST]` (over ssh
only) or `./cascadia gpu --root [DIR]` (into a root tree only). The NFS root
lives on the host that exports it: a device booted from another host's root
needs it installed there too. Everything built stays in `build/sgx2d`, out of
git.

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
- **The shaders** are iOS's: compiled USSE and PDS programs taken from a
  capture of iOS's GL driver. A frame is assembled from that capture's
  blocks: the first draw carries the whole GPU state, every later draw only
  a 4-word delta naming the 3D PDS block that loads its texture.
- **Blend modes** are three copies of one tiny USSE program (on the SGX the
  fragment program does the blending); only its last instruction differs.
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
| `tools/sgx/capture-layout.json` | where the pointers are in the iOS capture, for relocating it |
| `tools/sgx/lib/cross.sh` | the device's binaries, cross-built against Alpine's armhf packages |
| `tools/gpucap.sh` | the capture from iOS (`./cascadia gpucap`) |
| `tools/sgx/rpack.py`, `rgen.py` | the template pack, from a capture |
| `tools/iosgpu/rtemu.py` | runs the kext's render-target code under unicorn |
| `tools/iosgpu/gltrace.m` | the iOS capture tool (`prebuilt/gltrace`; `build.sh` needs Xcode) |
| `tools/sgx/lib/{rectest,pbtest,cnttest,bigtest,replay,sdlshot}.c` | diagnostics |
