# P105 GPU: Mesa and OpenGL -- the plan (2026-10-03)

The SGX543MP2 draws under Linux (`p105-gpu.md`, M1-M8): iOS's microkernel
runs, renders go through the TA and the 3D pass, and `sgx2d` plays SuperTux
at 60 fps. All of that is one fixed frame: one shader pair per blend mode,
written by hand, one render target, the screen. This file is the plan for
the rest -- a Mesa driver, so that OpenGL ES 2.0 and then desktop OpenGL 2.1
programs run on the GPU -- and the record of each step as it is taken.

The milestones carry on the numbering of `p105-gpu.md`.

## Where it starts

What is known well enough to build on, and where it was found:

| piece | state | where |
|---|---|---|
| power, clocks, init, MMU, microkernel boot | done, in the kernel | `apple_sgx_hw.c`; p105-gpu.md, STATUS 09-30 |
| kernel CCB, POWER/HWPERF/TRANSFER/TA commands | done | p105-gpu.md, "Commands" |
| transfer queue (2D blits, fills) | done, in the kernel | M1-M3 |
| render: TA command, render context, PB, render details | done for one frame | M4-M6 |
| VDM control stream: state blocks, draws, vertex fetch | the words sgx2d uses | M6, `frame.py`, `sgx2d.c` |
| PDS programs | built by shape, 12 kinds | M8, `pds.py` |
| USSE: encodings, an assembler | the 16 programs of a frame | M8, `usse.py`, `programs.py`; Vita3K's table |
| textures: twiddled RGBA8, linear BGRA | done | M6, M7 |
| depth test: compare and write bits | known, replayed | M5 |
| blending | in the fragment program (SOP2 on o0) | M6, M8 |
| frame fence: render details +0x24 | used by sgx2d | M6 |

What still comes from Apple at run time:

- **the microkernel** (`sgx543.fw`, from the IPSW) -- stays: writing one is not
  on the plan;
- **the 3D pass's event PDS program** (168 bytes from the GL driver, via the
  IPSW) -- could be rebuilt from its decoding, low priority;
- **the render target data** -- region arrays, tail pointers, render details,
  the 3D register block -- which `rtemu.py` computes by running the kext's own
  code under unicorn, once per screen size, when the pack is built. A GL
  driver needs render targets of any size at run time, so this has to become
  our own code (M10).

What is not known yet is listed under each milestone.

## The shape of it

```
GL application
   |  OpenGL ES 2.0 / OpenGL 2.1
Mesa: GL state tracker (src/mesa) -> Gallium driver "sgx"
   |    state -> VDM stream, PDS programs, ISP/TSP words, TA command
   |    shaders: GLSL -> NIR -> USSE (our backend), PDS by shape
   |    render targets: our code for what the kext computes
   |  ioctls (include/uapi/drm/apple_sgx_drm.h)
kernel: apple_sgx render node -- buffers in the GPU's address space,
   |    renders queued to the microkernel, syncobjs
iOS's microkernel -> TA -> 3D -> render target
   |
display: the framebuffer (now), simpledrm + kmsro (later)
```

Choices made, and why:

- **A DRM render node**, not more debugfs. Mesa's loader, EGL, GBM and every
  window system expect a DRM fd; syncobjs, dma-bufs and implicit
  synchronisation come with it.
- **Userspace builds the work, the kernel queues it.** The TA command, the VDM
  stream, every PDS program and state word are built by Mesa, as by sgx2d
  now; the kernel copies the command into the render queue, points the
  completions at its own memory and kicks. iOS splits it differently (its
  kernel builds the render target data and the 3D block), but with one
  address space for everyone that buys no protection, and every change on
  the userspace side is a rebuild instead of a new kernel on the device.
- **One GPU address space in v1.** The hardware render context carries a page
  directory, so per-process address spaces are possible later (the kernel's
  own buffers then shared into every directory, as the DDK does).
- **GLES 2.0 first, then GL 2.1.** The SGX543 is a GLES 2.0 part; desktop GL
  2.1 on top of the same features is what Mesa's lima driver offers on the
  Mali-400, the closest comparison. GLES 3 is not a goal: Series5XT is a
  GLES 2.0 generation, and no driver anywhere offers more on it.
- **Blending in the shader** (`nir_lower_blend`): on the SGX the fragment
  program does it anyway (M8), so every blend mode is a shader variant.
- **A compiler from NIR**, not a port of anything: there is no open
  GLSL -> USSE compiler. Vita3K's decoder is the reference for encodings and
  semantics; iOS's compiler is the oracle (M11).

## M9: the render node (2026-10-03)

`drivers/gpu/drm/apple-sgx/` (the driver moved out of `drivers/misc`; config
symbol `CONFIG_DRM_APPLE_SGX`): `apple_sgx_hw.c` is the old driver, unchanged
in what it does; `apple_sgx_drm.c` the render node,
`/dev/dri/renderD128`, driver name `apple_sgx`. Interface:
`include/uapi/drm/apple_sgx_drm.h`, experimental.

| ioctl | what |
|---|---|
| `GET_PARAM` | core id and revision, cores, clock, microkernel up, address ranges, code base, framebuffer, counters |
| `GEM_CREATE` | a shmem buffer, pinned and mapped into the GPU's address space for its life: at an address the kernel picks (top down from `0xa8000000`, a guard page after it), in the code zone, or at a fixed address |
| `GEM_MMAP_OFFSET` | to mmap it, write-combined |
| `GEM_WAIT` | until no render that lists it is running |
| `SUBMIT` | one render: the TA command (as `rkick` takes it), the PB descriptor, the render details' buffer and offset, every buffer it touches, in/out syncobjs |

The GPU's address map (`apple_sgx.h`):

| GPU address | what |
|---|---|
| `0x00001000` | the 2D engine's code page |
| `0x80000000-0x807fffff` | the microkernel's buffers and the kernel's queues |
| `0x80800000-0xefffffff` | render node buffers (and debugfs `map`) |
| `0x90000000` +16 MiB | the framebuffer, mapped by the kernel |
| `0x9a000000` +16 MiB | the USSE code zone; `USE_CODE_BASE_3` and `_5` point at its start |
| `0xa8000000-` | where the kernel places buffers, top down |

How a render runs: a DRM scheduler with room for one job. `run_job` copies
the command into the render CCB (`apple_sgx_render_queue()`, shared with
debugfs `rkick`), sets the PB descriptor in the render context, writes the
context into the render details, points the command's completion at the
scratch buffer (`+0x40`) and sends TA. An hrtimer then polls, every 200 us
while a render is on the GPU: first the completion value (the TA is done),
then the render details' `+0x24` going back to 0 (the 3D pass is done). The
interrupt is not used: it has never been seen to fire. A render not done in
2 s restarts the microkernel and fails with `ETIMEDOUT`; mappings survive.

Buffers mapped after the microkernel's boot need the GPU's MMU caches
invalidated. The kernel marks the page tables dirty on every change, and the
next kick carries `SGXMKIF_CC_INVAL_BIF_PT | _PD | _SL` in its cache-control
word, the DDK's way (`apple_sgx.mmu_inval_cc`, default 7): its `mmu.c` adds
the system level cache on multi-core parts, where the SLC holds PDEs and PTEs
(TI's omap5 DDK, `services4/srvkm/devices/sgx/mmu.c`, the values in
`sgx_mkif_km.h`). It works on the device as it stands: sgx2d's buffers are
all created after the boot now, and its first frame (and every one after)
renders.

The first client is `sgx2d`: when a render node is there, every `map` of
its pack becomes a buffer at that address, mapped into the process; `img`
lines are copied in; a frame is a `SUBMIT` with a syncobj that the next frame
waits on. `SGX2D_DEBUGFS=1` forces the old way. The pack's code base moved to
`0x9a000000` to match the kernel's (`rpack.py`; sgx2d refuses a pack built
for another). `sgxinfo` prints the parameters and checks three buffers.

On the device (kernel from this tree, `./cascadia build`; `./cascadia gpu` for
the new pack and binaries):

1. `dmesg | grep -i sgx` -- `render node: buffers at GPU 0x80800000-0xf0000000`
   and `microkernel is up`.
2. `/usr/local/lib/sgx2d/sgxinfo` -- `all good`.
3. `/usr/local/lib/sgx2d/sprites /usr/local/lib/sgx2d 200 600` -- the same
   frame rate as through debugfs (~220 fps); `renders done` in sgxinfo
   counts them.
4. `supertux-gpu`.

**On the device, 2026-10-03: it works.** The render node registers next to
the old driver (`[drm] Initialized apple_sgx 0.1.0 ... on minor 0`), the
microkernel starts as before, and:

- `sgxinfo`: every parameter as expected (CORE_ID `0x01194201`, rev 1.2.2,
  2 cores, 102.6 MHz, framebuffer 768x1024 at GPU `0x90000000`); a buffer the
  kernel placed at `0xeffef000` (the top of the range, below its guard
  page), one in the code zone at `0x9affb000`, one at the fixed `0xc0000000`,
  each written and read back through its mapping.
- `sprites 200 600` through `/dev/dri`: 207 fps (217-221 through debugfs).
  The few percent are the completion poll and the one-render-at-a-time
  scheduler, both on the list for M15.
- SuperTux: 62-64 fps, 1571-1608 quads in 56-62 draws a frame, none dropped
  -- the same as through debugfs. The first start after the boot showed no
  frame for a while (the game reading its data over NFS, cold); the second
  came up at once.

SDL now finds a DRM device and asks Mesa for EGL before falling back
(`ZINK: vkCreateInstance failed`, `failed to create dri2 screen`): Mesa has
no driver for `apple_sgx` yet. Harmless, and gone once M12 gives it one.

What to look for if it does not work: a first frame that hangs (the MMU
invalidation: try `echo 3 > /sys/module/apple_sgx/parameters/mmu_inval_cc`,
or 0), frames that tear or flicker (the render details' `+0x24` is not yet
set when the TA's completion is written, so a frame looks done too early),
and the render node's counters (`renders timed out`).

Not in M9: transfers from userspace (Mesa will blit with renders at first),
per-process address spaces, dma-buf import (display), the interrupt.

A lead for doing without the `+0x24` poll: the DDK's TA command
(`SGXMKIF_CMDTA_SHARED` in TI's `sgx_mkif_km.h`) carries status values for
both passes -- `ui32NumTAStatusVals`, `ui32Num3DStatusVals` and arrays of
`{address, value}` the microkernel writes when the TA and the 3D pass are
done. The completion the kernel sets in iOS's command (`+0x64 |= 1`, then
`+0x68` address, `+0x6c` value) has exactly that shape for the TA; if iOS's
layout has the 3D array too, a render's end becomes one word the kernel
owns. To find with a capture of a render command whose status counts are
not zero.

## M10: render targets without iOS's code

The kext's render target setup (`0x80bf7aac` init, `0x80bf6fd8` sizes and
allocations, `0x80bf74f4` fills, `0x80bf5eec` the submit's payload words and
3D register block) rewritten in C, as a small library Mesa and sgx2d share,
and the parameter buffer's layout from `rgen.py` (`pb_image()`) with it.

Validation is offline, against the code it replaces: `rtemu.py` for a sweep of
sizes (every power of two from 32 to 1024 on each side, the odd sizes a window
gets, 768x1024 and 1024x768) dumps every buffer and the payload; the C
version must give the same bytes at the same addresses. Then `./cascadia gpu`
no longer needs the kernelcache.

Unknowns: what each buffer is for beyond its size (region arrays per core,
tail pointers, the "state buffer" `+0x44`), which payload words depend on
anything but the size, and the multi-sample paths (`samples` in the init
call; one sample only to begin with).

## M11: the shader oracle

iOS's GL driver compiles GLSL ES to USSE, and gltrace can already capture
what reaches the GPU for a draw. Extended to take a vertex and a fragment
shader from files, draw with them, and dump the USSE programs, their PDS
programs, the state words and the uniform and varying layout, it turns the
compiler question into differential reverse engineering: change one thing in
the source, see what changes in the binary.

A corpus to run through it, one feature at a time:

- every ALU operation GLSL ES has, in each precision (`lowp`/`mediump`/`highp`
  map to fixed point, half and full float on the USSE), and the
  transcendentals (`rcp`, `rsq`, `exp2`, `log2`, `sin`, `cos`);
- 1..8 varyings of each size: how the vertex program's outputs are laid out,
  what the TA state words that describe them hold (`0x0a001000`,
  `0x00088000`, `0x39`, `0x3` in today's frame, "GL's values"), and how the
  pixel side's iterators (`TEX_ITERATE 0x1fc01900`) load them;
- uniforms: scalars, vectors, matrices, arrays, how they land in the
  secondary attributes and what the DMA control words (`dma_then_usse`)
  carry;
- texture sampling: the non-dependent fetch the PDS does today, a dependent
  one (the `SMP` instruction), `texture2DProj`, bias and LOD, cube maps;
- `discard`, `gl_FragCoord`, `gl_FrontFacing`, `gl_PointCoord`,
  `gl_PointSize`, branches and loops with uniform and varying conditions.

`usse-dis.py` decodes opcodes and a few operand layouts; for this it needs
every operand of every instruction the corpus produces.

## M12: the Mesa driver's skeleton (2026-10-03)

`mesa/` holds it: the driver's files (`mesa/files`, copied into a Mesa tree),
the few lines that register it with Mesa (`mesa/mesa.patch`: the
`gallium-drivers` choice `sgx`, the meson subdirectories, the
`apple_sgx` DRM driver descriptor for the pipe loader, the dril entry point),
and the build (`mesa/build.sh`, `./cascadia mesa`). Mesa is 26.1.8, at the
tag, checked by commit; `mesa/fetch.sh` clones it on the host (freedesktop.org,
a GitHub mirror if that does not answer).

| file | what |
|---|---|
| `sgx_device.c` | the render node: parameters, buffers (mmap'd write-combined), submits, syncobj fences |
| `sgx_screen.c` | the screen: a GLES 2.0 part's caps, formats, NIR options |
| `sgx_resource.c` | resources, all linear for now; maps wait for the renders that use the buffer |
| `sgx_context.c` | state (kept, unused), clears, CPU copies and blits, fences; `draw_vbo` drops draws |
| `sgx_frame.c` | the template frame: clears on the GPU (below) |
| `winsys/sgx/drm` | `sgx_drm_screen_create()`, one screen per device |
| `drm-shim/sgx_noop.c` | a pretend render node for running the driver without an iPad (Mesa's drm-shim) |

**Clears on the GPU, through sgx2d's frame.** Until the driver builds its own
renders, it borrows sgx2d's pack (`/usr/local/lib/sgx2d`, from `./cascadia
gpu`; `SGX_PACK` names another): every window of it becomes a buffer at its
GPU address, and a clear is sgx2d's draw 0 -- one full-screen quad, the white
texture times the clear colour -- with two programs of the driver's own:

- **end of tile**, one per render target, written once at its own address in a
  code-zone buffer (never rewritten under a running render): the PBE's six
  words `0x00110000` (linear, B8G8R8A8 from the shader's RGBA), the target's
  address, its stride in pixels / 2 - 1, 0, 0, `(h-1) << 12 | (w-1)`; the 3D
  pass's event program names it (its data word 2);
- **replace** instead of blend: `PHAS; SOP2M (texel x vertex colour); SOP2
  o0 = r * (1 - 0) + o0 * 0`, the selector fields as Vita3K decodes SOP2
  (`c` cmod1, `aa` asel1, `m` cmod2, `f` amod1, `ll` asel2, `ggg` csel1,
  `hhh` csel2, `i` amod2), words from `usse.py`.

The background object's descriptor (3D PDS block `+0x130`) points at the
target too, as FB_LOAD points at the framebuffer for sgx2d. Only targets of
the pack's size (the screen's, 768x1024), B8G8R8A8 or X8, level 0, can be
cleared this way; everything else, scissored clears and depth/stencil go
through the CPU. `SGX_DEBUG=frame` logs every word a clear sets.

**Tested on the host, through the drm-shim** (no GPU work runs there, renders
are "done" at once): EGL finds the device and picks `apple_sgx` by itself,
`GL_RENDERER` is `PowerVR SGX543MP2`, `GL_VERSION` `OpenGL ES 2.0 Mesa
26.1.8`; `glclear` gets every pixel right with CPU clears; with a synthetic
pack (frame.py's objects at their addresses) the GPU path builds the frame
and submits it, every word as sgx2d's frame has it (the VDM stream, the
background descriptor `00be0e90 cc2ff3ff ...` = FB_LOAD for 768 pixels).
`mesa/build.sh` runs end to end the same way (configure, build, install,
glclear linked against the result).

**On the device** (kernel with the render node; `./cascadia gpu` for the pack,
then `./cascadia mesa`, which builds in an Alpine armv7 container -- slow the
first time, under ARM emulation -- and installs to `/usr/local/lib/sgx-mesa`
with `sgx-gl` to run things with it):

    sgx-gl glclear                  # one clear, read back and checked
    SGX_DEBUG=frame sgx-gl glclear 6
    sgx-gl glclear 100              # how long a clear and its read-back take
    echo 0 > /sys/class/vtconsole/vtcon1/bind; sgx-gl glclear 4 --fb

The driver says at start whether clears go to the GPU (`template frame
768x1024 from ...`) or the CPU (no pack, or its buffers taken -- SuperTux
running, say). `0 wrong` from glclear on the GPU path is the milestone.

**First run on the device (2026-10-03).** Two things went wrong before a
clear could.

- The clone of Mesa, inside the armv7 container, sat for over an hour on a Mac
  (git under emulation, quiet with `-q`). It is done on the host now,
  natively, with progress, a minute's stall as the limit and a mirror to fall
  back on; the container only builds.
- The driver came up (`template frame 768x1024`, `clears on the GPU`), and
  then GL had no context: `glGetString` returned NULL with `GL_INVALID_OPERATION
  in Inside glBegin/glEnd` and a spurious `1 similar GL_NONE errors`, and
  glclear died of a bus error before its first clear. The context pointer
  itself was wrong. Mesa's `meson.build` turns on TLS descriptors
  (`-mtls-dialect=gnu2`) whenever the compiler takes the flag, and on 32-bit
  ARM the current context (`_mesa_glapi_tls_Context`) then reads back as
  garbage. The same Mesa, cross-built for armhf (glibc) and run under
  `qemu-arm` with the drm-shim (which needed wrappers for glibc's 64-bit-time
  `__ioctl_time64`, `__fcntl_time64`, `__stat64_time64`, `__fstat64_time64`
  there), crashes at that read in `_mesa_make_current`; with
  `-mtls-dialect=gnu` it gives the strings and `0 wrong`. `build.sh` passes
  that now, and reconfigures a tree built with other options.

Not yet: draws (`draw_vbo` says so once and drops them), textures in any
layout but linear, scanout (EGL has the surfaceless and GBM platforms; the
picture reaches the screen only through glclear's `--fb` copy), desktop GL
(no GLX, and GL 2.1 waits for the queries and the rest of M14).

## M13: the first triangle, then the compiler

A triangle through Mesa with hand-written programs (today's frame's), then
the first compiler: NIR in, USSE out -- attribute fetch from the PDS, the
vertex shader's arithmetic (`VMAD`, `VDP`, the transcendentals through
`VCOMP`), its outputs to the TA, varyings into the fragment shader through
the iterators, uniforms through the secondary attributes, constant colour
out. An encoder in C (the tables of `usse.py`, the bits of Vita3K's
decoder), a register allocator over the USSE's banks, and `programs.py`'s 16
programs as the first unit tests: assembled byte-identical to iOS's. Done
when `es2gears` turns.

## M14: textures, blending, depth and the rest of GLES 2.0's state

Every GLES 2.0 texture format (RGBA8, RGB565, RGBA4444, RGBA5551, L8, A8, LA8,
and PVRTC, which the hardware has), wrap and filter modes, mipmaps (the
transfer queue made iOS's), non-power-of-two rules; blend modes through
`nir_lower_blend`; depth and stencil through the ZLS registers (depth seen in
M5, stencil not yet); scissor, viewport, culling, polygon offset, points and
lines. Done when `glmark2-es2` runs its scenes and SuperTux's GL renderer
draws.

## M15: conformance and speed

`dEQP-GLES2` and the GLES parts of `piglit`. Control flow, `discard`,
precision. Speed: several renders in flight (the hardware overlaps the TA of
one with the 3D of the last), the interrupt instead of polling, state deltas
instead of full state per draw, the parameter buffer's size.

## M16: on the screen properly

`simpledrm` in place of `simplefb` (KMS on the same framebuffer), Mesa's
`kmsro` pairing it with the render node, dma-buf import in the kernel, GBM;
then a compositor (weston or sway), Xorg with glamor, and desktop GL 2.1
exposed for the programs that want it. XFCE with a GPU behind it.

## Testing, without and with the device

- **Host, every change:** the kernel driver builds with `W=1` against the
  pinned v6.12 (`make LLVM=1 ARCH=arm drivers/gpu/drm/apple-sgx/`), the
  userspace with `-Wall`; the USSE encoder against captured programs; the
  render target code against `rtemu.py`.
- **Device:** `sgxinfo`, sgx2d's demos and SuperTux on every kernel change;
  Mesa's own tests once there is a Mesa driver.
- **iOS:** gltrace, for anything the hardware does that the corpus does not
  show yet.
