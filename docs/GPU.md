# The GPU: SuperTux at 60 fps on the SGX543MP2

The iPad mini 1's PowerVR SGX543MP2 draws under Linux. There is no GL driver:
what exists is a 2D drawing library, `sgx2d` (textured quads, colour
modulation, three blend modes, full screen straight into the framebuffer),
and `sgxsdl`, a shim that runs SDL2's 2D renderer on it. SuperTux 0.6.3 runs
through it at a stable 60 fps, played with a touch gamepad.

How it was found, step by step and with every wrong turn, is
[research/p105-gpu.md](research/p105-gpu.md). This page is how to use it and
how the pieces fit.

## What you need

Everything the rest of this port needs, and nothing more on the host: macOS
or Linux, Docker, python3 and the IPSW. The build runs in the same
`cascadia-build` image as the kernel (no ARM emulation), and Apple Silicon
and x86_64 hosts give byte-identical results.

- A kernel with the `apple-sgx` driver from this tree, build #263 or later
  (`./cascadia build`; the GPU memory allocator described below needs it).
- The device booted from an NFS root (`./cascadia nfs on`) with SuperTux
  installed on it: `apk add supertux` on the device, with `./cascadia net on`
  on the host. A RAM root has no room for it, and anything installed into
  one is gone at the next boot.
- Two things that cannot be in this repository, because they are Apple's
  code and data. Both come from what you already have:
  - **the decrypted iOS 8.4.1 kernelcache**: `./cascadia firmware` leaves it
    in `build/firmware/kernelcache.12H321.macho`, next to the GPU's
    microkernel it extracts from the same file. The GPU kext's own code is
    run from it in an emulator, to compute the kernel's per-frame data;
  - **the 2D templates, from your iPad's iOS**: `./cascadia gpucap`, once.
    It needs the iPad in its jailbroken iOS 8.4.1 with OpenSSH — the same
    one `./cascadia flash --kdfu` and `./cascadia mtcal` use — on the cable
    (or `IOS_HOST=<its IP>` over Wi-Fi). It runs `gltrace` there (prebuilt
    in `tools/iosgpu/prebuilt`), which draws a textured quad with OpenGL ES
    and saves the GPU memory iOS's GL driver set up for it; the compiled
    shaders and command templates in it are what the GPU runs under Linux.
    About 9 MB comes back into `logs/ios/mod/`. The same iOS build should
    give the same capture on any iPad mini 1; so far it has been taken on
    one.

## Build and install

```bash
./cascadia firmware          # if not done yet: also leaves the kernelcache
./cascadia gpucap            # once; iPad in iOS, on the cable
./cascadia gpu --root        # build, and install into this host's NFS root
```

`--root` writes into the root filesystem `./cascadia nfs` exports from this
host (`~/cascadia-root` on a Mac, `/srv/cascadia-root` on Linux, through
`sudo` where it is root's), or into another tree given as `--root DIR`. The
device does not have to be on. Alternatively `./cascadia gpu --install`
copies it over ssh into whatever root the running device has
(`root@10.55.0.2`, or `--install HOST`). Either way it lands in
`/usr/local/lib/sgx2d`, with the command `/usr/local/bin/supertux-gpu`.

The NFS root lives on the host that exports it: a device booted from another
host's root (say, a Mac's and then an Arch box's) needs it installed there
too.

What `./cascadia gpu` does, in the build image: splits the capture, runs the
kext's render-target code under unicorn (`tools/iosgpu/rtemu.py`), builds the
template pack (`tools/sgx/rpack.py`) and cross-compiles `libsgxsdl.so` and the
demos against Alpine's own armhf musl and SDL packages
(`tools/sgx/lib/cross.sh`). All of it stays in `build/sgx2d`, out of git.

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
| `tools/sgx/lib/cross.sh` | the device's binaries, cross-built against Alpine's armhf packages |
| `tools/gpucap.sh` | the capture from iOS (`./cascadia gpucap`) |
| `tools/sgx/rpack.py`, `rgen.py` | the template pack, from a capture |
| `tools/iosgpu/rtemu.py` | runs the kext's render-target code under unicorn |
| `tools/iosgpu/gltrace.m` | the iOS capture tool (`prebuilt/gltrace`; `build.sh` needs Xcode) |
| `tools/sgx/lib/{rectest,pbtest,cnttest,bigtest,replay,sdlshot}.c` | diagnostics |
