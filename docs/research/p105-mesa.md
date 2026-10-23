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
| `0x9a000000` +8 MiB | the USSE code zone; `USE_CODE_BASE_3` and `_5` point at its start (8 MiB: a DOUTU names code by a 20-bit index of 8-byte words) |
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

## M17: more than one process -- the frame built, the kernel's parameter buffer

Every process that drew loaded sgx2d's pack at its fixed addresses, so a
second one could not start while weston ran. Now (2026-10-08):

- **the frame is Mesa's own** (`sgx_template.c`): what `frame.py`, `pds.py`
  and `programs.py` put in the pack -- the PDS block, the state area, the
  stream's tail, the programs, the TA command, the GL words of the 3D block
  -- built at addresses given; `mesa/host/tmpl-test.py` builds it at the
  pack's and compares, byte for byte: the same. `sgx_frame.c` builds it in
  buffers where the kernel puts them; only the GL driver's event program
  still comes from the pack's image. `SGX_FRAME=pack` keeps the old way.
- **the parameter buffer is the kernel's** (UAPI 3): one for all clients,
  laid out as `rgen.py`'s `pb_image()`, written again for each microkernel
  start that uses it; a submit with `pb_va` 0 takes it. Clients taking
  turns no longer cost the microkernel a restart.
- **render target data in the TA's heap** (`APPLE_SGX_BO_TA_HEAP`), where
  the kernel picks: the 3D block names the state buffer by its offset from
  the TA's base, 24 bits of 16 bytes.

Two things the first kernels got wrong. The descriptor's word 14 is the
first block's *address* (`rgen.py` takes it from its list of addresses; the
port wrote the offset): core 0's `BIF_FAULT 0x00020010`, a read at
0x00020000. And where the buffer is matters: from Mesa, with the same image
in a buffer of its own at 4 MiB steps, a single clear hangs with the buffer
at 0x8cc00000-0x8ef00000 (or at 0x8d000000-0x8f400000 now and then:
`gltri` failed at two the clear passed), and works everywhere else from
0x88c00000 to 0x96c00000 (0x97000000 runs past the TA's 256 MiB). Render
target data in that range are fine. Not understood; the kernel's buffer is
at 0x89000000, near iOS's 0x88000000, and the full set of tests passes with
a buffer there.

On the device (kernel #299): every test right with the kernel's buffer and
the frame built (glclear, gltri, glfs, gltex, glblend, gldepth, glsize,
glseam, glpersp; glspeed 10.3k draws a second, kmscube 194 fps), and not
one microkernel restart. **Four GL processes at once** (glspeed, gltex,
glfs, gldepth): all right, no restart, no timeout. Each builds its frame
where the kernel puts it; weston and its clients each have their own.

**GL clients under weston.** Mesa had been built with no window-system
platform (`-Dplatforms=`): EGL on Wayland found no configs. With `wayland`
(and `wayland-dev`, `wayland-protocols` in the build image), es2gears_wayland
and weston-simple-egl draw in weston's windows, three processes on the GPU
at once: the clients render into their buffers, weston samples them as
dma-bufs (linear) and its output goes to the screen through KMS. es2gears
takes 64% of the CPU -- the vertex shaders are still the draw module's
(M18 moves them to the GPU).

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

**Done (2026-10-08): render targets of any size, 1x1 to 4096x4096.**
`mesa/files/src/gallium/drivers/sgx/sgx_rt.c` is the kext's render target
code rewritten from its behaviour (headless Ghidra on the four functions,
their literals read from the kernelcache). Two helpers it calls were not
what they looked like: `0x80bfe330` is ARM code, `__umodsi3`, used for the
macrotiles' first regions; `0x80bf6e78` fills, once per boot, the table of
context areas at `0x80c18b04` that the init's first buffer is sized from
(zeros in the kernelcache: it is computed). What the CPU writes, per size:

| buffer | size | contents |
|---|---|---|
| context | 0x11a0 for two pipes | none: two areas per copy (0x6f0 apart), two per pipe, for the microkernel |
| regions, copy 0 and 1 | per pipe: 12 bytes a tile of the macrotiles, padded to 4 KiB | none (the TA's) |
| tail pointers | per pipe: (next power of two of the macrotiles' side)^2 x 8 | none |
| render details | 0x180 | two copies 0x84 apart (where everything is, per pipe), then each copy's first region of each macrotile |
| state | 0x200 + 64 a region per pipe | a full-screen object (the background's, presumably: (0, 0), (w, 0), (0, h) as 12.4 fixed point biased by 0x4000, half scale past 2048), a tile-sized one, a stream entry per region pointing at it |

and the words that point at them: the 3D block's (`+0x28` big, `+0x2c` the
region arrays, `+0x3c`..`+0x48` sizes and macrotile bounds, `+0x88` the state
buffer from the TA's base in 16 bytes -- which is why it has to be within
256 MiB above 0x87800000 --, `+0x10c`/`+0x110` the details) and the TA
command's (`+0x50`..`+0x58`, `+0xc0`..`+0xcc`, the arrays and tail pointers
per pipe, `+0x104` the last pixel, `+0x114` big). Tiles are 32 x 32 pixels
and the macrotiles 2 x 2, each side a multiple of 4 tiles; "big" is a side
over 2048.

`mesa/host/rt-test.py` runs `rtemu.py` for 61 sizes (every pair of powers of
two from 32 to 2048, and 1x1, 33x33, 100x50, 300x200, 480x320, 767x1023,
768x1024, 1024x768, 1000x1, 2049x64, 4096x16, 4096x4096) and compares every
buffer, the payload words, the 3D block and the TA command (both another
size's with this size's words written over them: the rest are the GL
driver's): all 61 the same, byte for byte.

Three more places hold the size, found in iOS's captures at other sizes
(`logs/ios/size`, a 256x128 target: `80000007 00000003`, viewport 128, 64):
the state's words 7 and 8 (the last tile across and down, bit 31 set on the
first), words 9..12 (the viewport: half the width twice, half the height
twice), and the VDM stream's terminate PDS program, whose data `+0x10` is
the last tile across << 16 | down (`0017001f` in the pack, `00070003` for
256x128).

In the driver (`sgx_frame.c`), each size a render goes to gets its set of
buffers and a 3D block (the pack's, with the size's words) in a slot of 4
MiB at 0x89000000 + slot x 4 MiB -- the kernel picks addresses far above
the TA's base -- eight slots, the least recently used given up, all of them
after a render hangs. The TA command is the pack's, copied and written over
per render; the terminate data are written in place. `SGX_FRAME=packrt`
takes the pack's set instead (its size only). The texture size limit is
now iOS's 4096.

On the device: `glsize` (a gradient and a square, every pixel checked) 15
of 15 -- 64x64, 100x50, 256x128, 128x256, 300x200, 33x17, 1x1, 768x1024,
1024x768, 512x512, 2048x2048 and back to earlier sizes after the slots ran
out -- and 2049x100, 4096x16, 100x4096, 4096x4096 right; everything before
unchanged at the screen's size, now through our set (glfs 23/23, gltex
12/12, glblend 14/14, gldepth 9/9, glpersp exact, gltri 11/11; glspeed 9.6k
draws a second).

Still from the kernelcache: the pack's own render target data (`rtemu.py` at
`./cascadia gpu`; sgx2d uses them) and the parameter buffer image; neither
is needed by Mesa now. Not ported: the multi-sample paths, the 4x4
macrotiles (an RT field the init sets to -1), and copy 1 of everything,
which renders here never use.

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

**The tools (2026-10-04).** `gltrace corpus OUTDIR FILE.glsl...` draws each
case once with iOS's GL driver -- every attribute fed (the one named `p` a
triangle over the 64x64 target, the others `0.25 * (location + 1) + 0.0625
* vertex + 0.015625 * component`), every uniform given a value (uniform `i`:
`i + 1 + 0.0625 * component`; ints `16 * (i + 1) + component`; samplers a
4x4 texture or cube map, a unit each) -- and writes the pages of the
driver's GPU buffers (the process's IOKit mappings) that the draw changed,
after a baseline of every page that was not zero before the first case.
No GPU-to-CPU address map is needed: the new programs are the new bytes.
`tools/iosgpu/corpus/` holds the first 91 cases (`make.py` writes them; all
pass glslang as GLSL ES 1.00): 40 arithmetic, 6 precision, 11 varying, 8
vertex-side, 10 uniform-shape, 10 texture and 6 control-flow cases.
`tools/shadercap.sh` builds gltrace if needed (macOS, Xcode, ldid), runs
the corpus on the iPad's iOS, fetches the pages into
`logs/ios/corpus/<date>/` (out of git) and runs `tools/iosgpu/corpus.py`,
which rebuilds memory case by case and lists, per case, the USSE programs
found in the changed bytes (from a PHAS to the next one or a zero word;
bit 50 marked, not trusted as the end: an old note has the driver's
fragment preamble carry it on its second instruction) and the other
changed runs as hex.

**First run (2026-10-04): 91 of 91 cases drawn**, centre pixels as the
values predict (`f06_div` `80 83 87 8a` = 1/2, 1.0625/2.0625, ...;
`f20_rcp` `ff f0 e3 d7`; `v00_vec4` the attribute). What the programs say:

- The driver writes the current draw's programs over the last ones, at a
  few fixed places (CPU `0x951de8` vertex, `0x951e40` pixel, `0x951e80`
  secondary, in this run; a second buffer at `0x926e..` in some cases), so
  a program's PHAS is often unchanged and what follows its end is the rest
  of an older one. `corpus.py` takes a program from the PHAS before a
  changed run to the instruction that ends it.
- **Bit 50 ends a program only on some instructions**: VBW, VPCK, VMOV,
  SOP2, LIMM, NOP and the emits (the layouts' `e`). On V16NMAD/V32NMAD,
  VMAD2, VTST, VCOMP it is another field: iOS's programs have V16NMADs
  with it set mid-program. (`usse.py`'s note that bit 50 ends every
  instruction but PHAS held only for what sgx2d uses.) The old note of a
  fragment "preamble" `PHAS / VBW 50850009e0000300 / NOP / VTST ...` was
  two programs: `PHAS; VBW (end)`, then an older program's tail.
- **A shader is up to three programs.** A **vertex** program (ends with the
  vertex emit `fb275000a0200000`: `PHAS; mov.f32 o0.xy, pa0 rpt2; emit`
  for a pass-through position, some draws with SMLSIs around a 4-repeat
  VMOV); a **pixel** program, ending in a write of `o0`; and a
  **secondary** program, run once per draw, which does the arithmetic that
  only depends on uniforms. `gl_FragColor = u0 + u1` is: secondary `PHAS;
  NOP; VTST; VLDST; SPEC` (the never-taken load, as our `programs.py` has
  it), `add.f16 sa6, sa8, sa6`, `pck.u8.f16 sa6, sa6 scale (end)`; pixel
  `PHAS; or o0, sa6, #0 (end)` (`50850009e0000300`) -- the secondary
  program's `pa` bank is the pixel program's `sa`. `u0 * u1` differs in
  one word, the V16NMAD's op2 (`...41103` add, `...40103` mul). A varying:
  pixel `PHAS; VPCK o0 <- pa0 (end)`, secondary empty (`PHAS; NOP (end)`).
  A non-dependent texture read: pixel `PHAS; VBW o0 <- pa0 (end)` -- the
  PDS fetched the texel, as in sgx2d's frame -- and the vertex program
  computes `t` with two VMAD2s.
- `corpus.py --catalog` prints every case's programs by kind, words and
  disassembly: the test vectors for our compiler. A kind the draw wrote no
  program of is only named (the driver used one already in GPU memory, or
  the shader has none; the capture cannot tell which).

**Reading them: the operands (2026-10-04).** `usse-dis.py` now decodes
the operands of the ALU, move, pack, test, load, sample and branch
instructions the way Vita3K's translator does (`usse-dis.py words HEX...`
for loose words). What that took, and what the arithmetic cases confirm:

- **Register fields of vector operands count 64-bit registers**: F16/F32
  operands of V16NMAD/V32NMAD, VMAD2, VCOMP, a float VPCK source and a
  float VMOV are doubled into 32-bit numbers (V16NMAD's dest field 3 is
  sa6 -- the register the pixel program's VBW, which is not doubled,
  reads). An F16 vec4 is two registers (sa6 = x,y; sa7 = z,w). The top
  temporaries a field can name (last 4, or 8 doubled) are the FP internal
  registers i0..i3, which hold F32 vec4s.
- **VPCK's formats are `u8 s8 o8 u16 s16 f16 f32 c10`** (the table had
  them wrong: what printed as `u16 -> f32` is f16 -> u8). The colour is
  packed by `pck.u8.f16 ... scale` (or `.f32` from an internal register):
  four bytes in one register, which the pixel program moves to o0.
- **mediump is F16 throughout**: uniforms sit in the secondary attributes
  as F16, the ALU work is V16NMAD / VMAD2 `d=1` / VCOMP `.f16`. `sin`,
  `cos` and `ceil` go to F32 in the internal registers.
- **Constants** come from the hardware's table as a special-bank operand
  (Vita3K's table: in F16, `c15.y` = 1.0, `c0.x` = 0, `c4.y` = 2.0, `c34` =
  1.0 in every half -- the halves of the F32 table's words) or from a
  secondary attribute the PDS loads (`exp` multiplies by sa5's low half,
  `log` by its high half: log2(e) and ln(2)).
- **Where uniforms land is the compiler's choice per shader**, not a fixed
  layout: `u0 - u1` reads u0 from sa8 and u1 from sa6, `mod(u0, u1)` u0
  from sa6; a uniform used only by component goes wherever there is room,
  even split (`u0 / u1` takes u1 as sa5 and sa8). The PDS program that
  loads them says where; for our compiler the layout is ours to pick.

What each `f*` case became (secondary program; the pixel program is `or o0,
saN, #0` with N the VPCK's destination):

| GLSL | iOS's secondary program (after the 5-word preamble) |
|---|---|
| `vec4(0.25, 0.5, 0.75, 1.0)` | `pck.u8.f16 sa8, sa6` (the constant loaded into sa6) |
| `u0 + u1`, `*`, `min`, `max` | `add/mul/min/max.f16 sa6, sa8, sa6` -- V16NMAD op2 1/0/5/6 |
| `u0 - u1` | `add.f16 sa6, -sa6, sa8` (src1 modifier `m=1`) |
| `u0 * u1 + u2` | `mad.f16 sa6, sa10, sa8, sa6` (VMAD2, `d=1`) |
| `u0 / u1`, `1.0 / u0` | four `rcp.f16` (VCOMP op2 0, one component each), then `mul.f16` |
| `inversesqrt`, `exp2`, `log2` | four VCOMP `rsq` / `exp` / `log` (op2 1/3/2) |
| `sqrt(u0)` | `rsq` then `rcp`, per component |
| `pow(u0, u1)` | `log`, `mul`, `exp` |
| `dot(u0.xyz, u1.xyz)` | `dp.f16 sa6.xyzw, sa8.xyz0, sa6.xyz1` -- the swizzles' 0 and 1 drop w; the dest mask broadcasts |
| `length`, `normalize` | `dp` to `.x`, `rsq`; `rcp` for length, `mul` for normalize |
| `clamp(u0, 0.0, 1.0)` | nothing: the u8 pack saturates |
| `abs`, `-u0` | `mul.f16 sa6, \|sa6\|, c15.yyyy` / `-sa6` (times 1.0, with a modifier) |
| `floor`, `fract` | `frc` (VFRC is `a - floor(b)`), then `add -frc + x` for floor |
| `ceil` | `mul.f16 i0, -x, 1.0`; `frc.f32 i0, c0 (0), i0` = 0 - floor(-x) |
| `mix(u0, u1, u2.x)` | two VMAD2s: `u1 * a + u0`, then `u0 * -a + that` |
| `step(u0, u1)` | `tstmsk sa6 = sub(sa8, sa6) ... ? 1 : 0` (VTSTMSK; the comparison's sign still to pin down) |
| `smoothstep` | sub, sub, 4 rcp, mul, max c0, min c15.y, `mad (1 - t) * 2 + 1`, mul, mul |
| `sign` | `mul -1 * 1`, then two conditional VMOVs (`< 0 ? -1 : 0`, `<= 0 ? that : 1`) |
| `cross` | `mul sa10.xyz, sa8.zxy, sa6.yzx`; `mov sa6, sa6.zxyw`; `mad sa8.yzx * sa6 - sa10` |
| `mod`, `reflect` | rcp, mul, frc, add, `mad -y`; `dp`, `mul 2.0`, `mad -n` |
| `u0.wzyx` | `pck.u8.f16 sa6, sa6.wzyx` (the pack's own component select) |
| `vec4(normalize(u0.xyz), 1.0)` | ..., `pck.u8.f16 sa6.xyz, ...` and `pck.u8.f16 sa6.w, c15.xxxy` (1.0) |
| `sin`, `cos` | F32 in i0/i1: range reduction (`mad`, `frc`), a polynomial of VMAD2s on table constants c28..c30, VDUAL and VMAD (not decoded yet) |

`tan` is the exception: a 43-instruction pixel program with per-channel
predicates (VTST into p0..p3, predicated VPCKs) reading sa8..sa20; it is
long enough that the driver put the secondary program after it, at CPU
`0x951fc0`, across a page boundary -- `corpus.py` cut programs at page
ends until it read each page with its neighbours.

**The other groups (2026-10-04).** `usse-dis.py` also decodes SMLSI, the
GPI forms VMAD (`d = gpi0 * s1 + gpi1`) and VDP (`d = dot(s1, gpi0)`), and
names VDUAL's two operations. What the `v`, `x`, `p`, `u`, `t` and `c`
cases show:

- **Vertex programs** copy and compute into `o`: position in o0..o3 (F32),
  the varyings after it (o4.., one register per component, F32). A vec4 is
  `mov.f32 oN.xy, paM rpt2` (each repeat moves a 64-bit pair; F32 ALU
  instructions work on two lanes, F16 on four). The attributes sit in pa
  in an order of the driver's: `a` at pa0, `p` at pa4 in the two-attribute
  cases, and an SMLSI with per-iteration offsets (`s1=[2,3,0,1]`) lets one
  repeated VMOV take p then a. `v = a * 2.0` is two `mul.f32 o.xy` per
  vec4 with the constant from the table (c4.x = 2.0, c5.x = 8.0) or a
  secondary attribute; `p * 0.5 + 0.5` is `mad.f32 o4.xy, pa0, c12.x,
  c12.x`. `m * p` (mat4): `pck.f32.f32 i0 <- p`, `mul.f32 i1, m0, i0.xxxx`,
  then VMADs `i1 = i0.y * m1 + i1` ... the last writing o0..o3, the
  matrix's columns in secondary attributes (F32). `dot(a, k)`: `dp4.f32`
  into o4 and o6.
- **Varyings reach the pixel program as F16 in pa** (mediump; the
  iterators convert): a vec4 in two registers, a vec2 in one, eight vec4s
  in pa0..pa15. highp varyings come as F32 (`pck.u8.f32 o0, pa0, pa2`);
  lowp ones like mediump. `gl_FragColor = v` is `pck.u8.f16 o0, pa0
  scale`, alone.
- **Precision**: lowp arithmetic is fixed point on packed bytes -- `u0 +
  u1` and `u0 * u1` in lowp are one SOP2M each, on u8x4 uniforms -- and
  highp is F32 (`pck.f32.f32 i0 <- sa`, `add.f32`, `pck.u8.f32`). For us
  lowp can be mediump: GLSL ES allows more precision.
- **Uniforms**: F16 (mediump) or F32 (highp) in secondary attributes,
  packed where the compiler likes (a `mat2` and a `vec4.xy` share sa6..sa7,
  a float takes half a register); ints arrive converted to float (`float(i)`
  is a bare pack); arrays load only the elements used. `m * u0` for mat2,
  mat3, mat4 is a `mul` and `mad`s by column, in the secondary program.
- **Uniform conditions stay in the pixel program**, as predicates: `b ?
  vec4(1.0) : vec4(0.0)`, `if (u0.x > 1.5)`, `u0.x > u1.x ? u0 : u1` are
  `tst p0 = ...; or o0, saA; !p0? or o0, saB`, the secondary program
  packing both values. A loop with a constant count is unrolled into the
  secondary program; one with a `break` is a real loop in the pixel
  program (`br` relative, in instructions: `p0? br +21` out, `br -21`
  back), indexing the uniform array through the index registers and
  shifts. `discard` is a second phase (`PHAS ... next at 974`) and an
  unknown special instruction `f9340426c0000280` after a VTST into p1.
- **Textures**: a non-dependent `texture2D` (also with bias, also
  `texture2DProj`) is fetched by the PDS: pixel `or o0, pa0`. A dependent
  one is `smp2d` in the pixel program -- coordinates in pa (F16), the
  texture's state words in sa6.., `drc0`, then `wdf 0` (the `SPEC
  f920000000000000`) before the result is read; `t.yx` and `t * 2.0` are
  dependent; a cube map is `smp` with dim 3D after `t.xyz - 0.5`. A
  texture read with uniform coordinates moves to the secondary program
  (`smp ... lod`). `texture2D(s, t) * v` with a lowp `v` (sgx2d's case) is
  one SOP2M on bytes, like sgx2d's own program.
- `c02_if_varying` caught the driver's internal programs too (81): moves
  of sa0.. to o0.. with every repeat count 1..16 (clears/loads), the
  colour-mask programs (`mov.f16 i0, o0; mov.f16 i0.<mask>, sa0; mov.f16
  o0, i0`) for each mask, SOP2Ms (blend), and a LIMM setup program.

**The TA's full state, read (2026-10-04).** Every case's draw writes the
21-word block sgx2d's frame has (`tools/sgx/frame.py`'s `full_state`;
`corpus.py --state` finds it by word 17, `f32(1e-5)`), in one of two
buffers in turn. Across the `v`, `t` and `x` cases (64x64 target, no
blending):

| word | value | what it is |
|---|---|---|
| 0, 1 | `0000dfc7`, `01d00300` | ISP A, B (sgx2d's B `03d00300` has bit 25, blending) |
| 4 | `1180ac24` / `..26` / `..28` | the pixel program's secondary loader (PDS), tag in bits 31:28 |
| 5 | `0803e000`; `1303e000` two varyings; `2183e000` eight; `0003e000` none | pixel PDS info: the top bits grow with the varyings (not decoded yet) |
| 6 | `0980ac1e`; `1180ac1e` two varyings or a texture fetch; `1980f51e` eight | the pixel program's PDS; bits 28:27 a size class (1, 2, 3) |
| 7, 8 | `80000001`, `1` | tile clip: last tile (1, 1) |
| 9..14 | 32.0 x4, 0.5 x2 | viewport |
| 16 | `NN001000` | **bits 31:24: the vertex's size in words** -- 4 for the position plus the varyings: 8 (vec4), 6 (vec2, and a float: it takes two), 7 (vec3), 10 (vec4 + vec2: sgx2d's `0a001000`), 36 (eight vec4s), 4 (none) |
| 19 | `7`, `1`, `3`, `39`, `ffffff`, `0` | **three bits a varying, in output order**: `001` two components (or a float), `011` three, `111` four. vec4 + vec2 is `0x39` = vec2 first, then vec4 (sgx2d's `0x39`) |
| 20 | `1`, `3`, `ff`, `0` | **a bit a varying: F16** (mediump and lowp 1, highp 0) |

So a vertex program's outputs are the position, then each varying padded
to two components, in the order words 19 and 20 list them; the iterators
hand the pixel program each varying as F16 or F32 by word 20. The PDS
programs: iOS's DOUTU word is `(address - code base) / 8 << 4 | 8` with
the code base at CPU `0x950000` in this run (`0x3c88` starts the pixel
program at `0x951e40`), which is how `corpus.py --pds` finds the PDS
program that starts each USSE program. A pixel PDS for a float varying:
data `{doutu 0x3d08, temps 6, 0, 0x2f40000f}`, code `doutu row 0, iterate
(control: word 3) 07040c02, end` -- sgx2d's textured program has the
iterate `07040c12` and a texture fetch after; a vec2's control word is
`0x2f00000f`. The secondary loader: `{0x980f521c, 5, 0, 0}, {doutu 0x3d88,
2, 0, 0}`, `dma row 0, doutu row 1, end` (frame.py's `dma_then_usse`),
which DMAs six words of constants into the secondary attributes.

**The PDS programs, read (2026-10-04).** `corpus.py --pds` finds them by
their DOUTU words. They are frame.py's shapes, with the varyings' iterators
added:

- **Vertex fetch** (`vertex_fetch`): a row per attribute `{address,
  control, stride (first row only), 0}` -- control `first pa << 8 | words -
  1`: `a` `0x003` (pa0, four words), `p` `0x403` (pa4) at +16, stride 32 --
  then `{doutu, 0, 0, 0}`; code `fetch index 67800072`, `fetch attribute`
  a row each (`2f0191a3`, `2f0591a3`, `2f0991a3`), `doutu (row n, vertex)
  030n01f5`, `end`.
- **The state block's loader**: `{block, 0x14, 0, 0}, {doutu, 0, 0, 0}`,
  `dma row 0; doutu row 1; end` -- the 21 words to a USSE program that
  emits them (frame.py's `state_21`).
- **The pixel program's PDS**: `{doutu, temps, 0, control 0}, {control 1,
  ...}`, code `doutu row 0 (070001b5)`, one **iterate** per varying, `end`.
  An iterate DOUT `0x07RRRWSS`: its control word is word `4 * row + word`
  of the data (row in bits 17:12, word in 11:10; bits 23:18 grow with it
  too: 1 for words 3..5, 2 for 6..9, 3 for 10), and it writes the next
  primary attributes, one register (`..02`), two (`..12`) or four
  (`..32`). Eight vec4s: `07040c12 07041112 07041612 07081b12 07081c12
  07082112 07082612 070c2b12`, control words 3..10.
- **A varying's control word**: `0x2fc0000f` is F16 (bits 29:28 = 2; F32
  is 0, `0x0fc0000f` with highp), the last iterate (bit 25; `0x2dc0...`
  for the others), four components (bits 23:22 = components - 1:
  `0x2f00000f` a float, `..40..` a vec2, `..80..` a vec3), varying 0 (bits
  15:12: which of the vertex's varyings, in words 19/20's order; 13 is the
  position, for `gl_FragCoord`). The pixel side's order is the iterates':
  vec4 + vec2 iterates varying 1 (the vec4) into pa0..pa1, then varying 0
  into pa2; eight vec4s come in the order 3 6 2 4 1 5 7 0.
- A texture read the PDS does: an iterate with a different control word
  (`0x0c00f900`; `0x0c00fa00` for `texture2DProj` -- the divide; sgx2d's
  `0x1fc01900`), then `texture fetch, state row 1` with `{format, size,
  address, 0}` (`03fe0090` -- `04fe0090` with a bias of 1.0 --,
  `0c020002` for 4x4, the texture's GPU address). A pixel program with no
  varyings: `doutu row 0; end`. temps: 2 without iterates, 6 with, 10 with
  a texture fetch.

For M13 this is enough to draw by hand: a vertex fetch PDS for the
attributes, a vertex program `mov.f32 o0.xy, paP rpt2; [varyings]; emit`,
the full state with word 16 = vertex size, 19/20 = the varyings, a pixel
PDS with an iterate per varying, and a pixel program `pck.u8.f16 o0, pa0
scale` (a varying colour) or `or o0, saN, #0` (a colour a secondary
program packed). Word 5 (the pixel PDS info: `0803e000` with one varying
or one temporary, `1303e000` with two varyings, `2183e000` with eight,
`0003e000` with neither) is still to be decoded; iOS's values can be
copied for the shapes above.

## M12: the Mesa driver's skeleton (2026-10-03, done 2026-10-04)

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

With the context right, every clear took 2.2 s and left the target at zero:
the kernel's 2 s timeout, every time. The driver's code buffer (the replace
and end-of-tile programs) had been given `0x9aff6000` -- the kernel made the
code zone 16 MiB and handed out its top first -- and a DOUTU names a program
by a 20-bit index of 8-byte instructions from the code base: 8 MiB of reach
(the kernel's own address-map comment said as much about its buffers). The
index was cut to `0xfec20`, the USSE jumped to nothing, the render hung. The
driver now puts its programs at a fixed address just above the pack's code
page (`0x9a010000`), whatever the kernel; the kernel's code zone is 8 MiB.

With the programs in reach the renders still hung. `SGX_FRAME` puts the
pack's own pieces back: `fb,blend` -- sgx2d's draw 0 with our vertices and
texture block -- ran in 45 ms and turned the screen red, so the submit, the
buffers, the VDM stream and the vertices are right; `fb` alone (our replace
program) hung, and with everything ours the 3D pass faulted near the target
(`BIF_FAULT 0xefcf0020`: page `0xefcf0000`, 60 KiB below the target at
`0xefcff000`, the low bits presumably the requester). The replace program
was the pack's blend with a different SOP2: `cmod1` and `amod1` set, bits
the pack's programs never use and nothing had checked. It is now the pack's
SOP2M, then `o0 = pa0` -- the very instruction of the pack's background
program -- and our programs live where the pack's run, in the free first
KiB of its code page (`0x9a001000`, seven end-of-tile slots).

`SGX_FRAME` (comma-separated) to bisect on the device, and
`sgx-gl frame-bisect` to run them all, one fresh red clear each, with the
screen's centre pixel read back from `/dev/fb0` where the clear goes there
and the kernel's verdict from `dmesg`:

| switch | what changes |
|---|---|
| `fb` | the pack's end of tile and background: to the screen |
| `blend` | the pack's pixel program (texel x colour, blended) |
| `screen` | our end of tile and background, aimed at the screen |
| `codebo` | our programs in a buffer of their own (`0x9a010000`) |
| `sop2` | replace by the SOP2 with `cmod1`/`amod1` |
| `align` | render targets at 1 MiB-aligned addresses (`0xc0000000` up) |

After a render times out, the driver loads the pack's buffers again before
the next one (the hang leaves the parameter buffer half-used).

The second bisect (2026-10-04) answered the pieces and raised a new
question. `fb` and `fb,codebo` drew red: the MOV replace works, in the
pack's page and in a buffer of its own. `align,blend` gave `target right`:
our end of tile and background fill a render target. But `fb,blend`, good
the day before, hung this time, and nearly every failure was the same:
`TA not done`, the render queue's write offset past its read offset -- the
microkernel took the TA command from the kernel's queue and never read the
render from the context's. Twice core 1 faulted at `0x87800050`, page 0
counted from `TA_REQ_BASE` (rgen.py's `0x87800000`): a parameter buffer page
the free list should never hand out. So something outside the frame makes
renders fail at random, and the suspects are what differs from sgx2d, which
draws thousands of frames in one process: each glclear is a new process
that loads the pack (parameter buffer included) afresh at the same
addresses, renders once, and goes. `frame-bisect` now runs each variant
four ways to tell: as it is; with `SGX_CC=8` (every kick also has the
microkernel drop the GPU's data caches, `SGXMKIF_CC_INVAL_DATA`); after a
fresh start of the microkernel (debugfs `apple-sgx/boot`, allowed while the
render node has no clients); and with `SGX_NOCC=1` (every buffer mapped for
the GPU without the cache-consistent bit: the CPU writes them
write-combined, and a GPU read through a cache could see an old line). The
known-good draw also runs four times in one process.

That run (56 lines) read clearly once each hang was put next to the render
queue's offsets:

- **Every** new process whose first render followed a good render without
  a restart in between hung, with a lockup and the fault at parameter
  buffer page 0 -- 16 times out of 16 -- and renders in one process never
  did. The microkernel (its DPM) keeps a parameter buffer's state between
  renders; a new client loads a fresh one at the same address, and the two
  disagree. After a start of the microkernel (`boot`), never.
- About one start in three, the first render after it was left in the
  queue: taken from the kernel CCB, never read (`ccb 120/0`), nothing
  started. `cc8` and `nocc` changed nothing, so neither the GPU's data
  caches nor the cache-consistent mappings are it.
- With the target at `0xefcff000`, our end of tile and background always
  faulted at `0xefcf0000` once the TA was through: the target's address
  loses its low 16 bits. At 1 MiB-aligned addresses (`align`) the target
  came out right. (Later: it was the BIF's tiled window, which moves
  accesses around within 64 KiB -- M13a, "Solved".)

The kernel now works around all three: a render whose parameter buffer is
another buffer object than the last one's (each object has a serial)
starts the microkernel again first (`apple_sgx.pb_restart`); the poller
sends TA again for a render left unread and unstarted after 20 ms
(`apple_sgx.rekick_ms`, up to five times); and addresses it picks for
buffers of 64 KiB or more are 64 KiB aligned. `frame-bisect` now checks:
each variant three times as it is, and 20 clears in one process, with the
kernel's "restarted for the PB" and "TA sent again" in the report.

**M12 on the device (2026-10-04).** With that kernel:

    sgx-gl glclear 100
    100 clears with read-back in 6.094 s: 60.9 ms each; 0 wrong

and every `frame-bisect` line right -- all ten variants three times each,
screen variants red, target variants `target right`, 20 clears in one
process right -- with "restarted for the PB" once per process and not one
"TA sent again". The first render of a process takes 70-95 ms (the
microkernel's start, firmware load included, is in it); after that a
clear plus the read-back of 3 MiB through a write-combined mapping is
47-60 ms, most of it the read-back. `fb,sop2` drew too: the SOP2 with
`cmod1`/`amod1` was never wrong, its hang the day before was the parameter
buffer's. The MOV replace stays (fewer unknowns).

What M12 leaves for later: the parameter buffer and the render target
data still come from the pack (M10 replaces them, and with them the
restart per client); one render at a time; targets of the screen's size
only; no draws.

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

**M13a: the first triangles (2026-10-04).** Before the compiler, a way to
draw at all, the way i915 has always drawn: the vertex shader runs on the
CPU, in Gallium's draw module (NIR to TGSI, `tgsi_exec`), which also clips,
culls and turns strips, fans, lines and points into triangles; the
triangles go to the GPU through the template frame (`sgx_frame_draw`: up to
8190 vertices a render, `r g b a u v x y` each, the draw word `0x81c00000 |
count`), whose programs colour each pixel by interpolating between the
vertices. The fragment shader is not run but read (`sgx_draw.c`): when its
colour is, channel by channel, a constant or a component of a varying or a
uniform, that colour is worked out per vertex -- `gl_FragColor = v`,
`vec4(v.rgb, 1.0)`, `= u` are drawn right, anything else in grey. Each
draw is a render, over what the target holds; screen-sized B8G8R8A8 targets
only, no depth, blending or textures. `tools/sgx/gl/gltri.c` checks three
draws (a triangle with a colour per vertex, a strip in a uniform's colour,
a triangle turned by a `mat2` in the vertex shader); on the host, through
drm-shim, every vertex comes out of the draw module where it should and in
the right colour (`SGX_DEBUG_DRAW=1`). Varyings have to be looked up the
way `nir_to_tgsi` names the vertex shader's outputs (`VARn` is `GENERIC n`,
not shifted by 9 as `tgsi_get_gl_varying_semantic` has it).

First run on the iPad (2026-10-04): the triangles are where they should be
(the turned yellow one exactly; the colour-per-vertex one's blue channel
within a few steps of the true barycentric where it was drawn), but the
pack's pixel side gets colours that vary across a triangle wrong -- each
pixel's red + green right, the two mixed -- the pixel near the blue corner
is black, and a strip in a uniform's colour did not show. sgx2d only
ever drew one colour per quad, so this was never tried. Draws now use the
pixel side iOS builds for `gl_FragColor = v` (the corpus's `v00_vec4`): a
PDS program `{doutu, temps 6, 0, 0x2fc0100f}` / `070001b5 07040c12
af000000` (iterate varying 1, the pack's colour, as F16 into pa0..pa1) and
the pixel program `PHAS; pck.u8.f16 o0, pa0 scale` (`40850a3da01d8000`),
the state's word 6 `1 << 27 | va >> 4` (one data row); clears keep the
pack's, and `SGX_FRAME=packpixel` puts draws back on it. `gltri --ppm FILE`
saves what it read back.

Second run: byte for byte the same, with iOS's pixel side and with the
pack's (`packpixel`) -- so what is wrong is most likely before the pixel
program: the varyings as the pack's vertex program and state words 16, 19
and 20 hand them to the TA, or the vertices. (It was neither: see
"Solved" below.)

**Solved (2026-10-04, third session): nothing was wrong with the draw.**
The two read-backs as images (`gltri.ppm`, `gltri-pack.ppm`) were the whole
scene, in the right colours, cut into 64-pixel pieces and shuffled: four
narrow copies side by side, striped. Rendering the same three draws into
the framebuffer instead of the target (`SGX_FRAME=screen`, then `cat
/dev/fb0`) gave a perfect picture -- the gradient triangle, the orange
rectangle, the yellow triangle. The same pixels, 283838 of them not black
in both, only in other places. Matching every 256-byte piece of the one
against the other (4690 that match exactly one place) gave the rule
without exception: within each 64 KiB, bits 11:8 and 15:12 of the address
trade places, so each 4 KiB of memory holds a 256-byte x 16-line tile of a
4096-byte-stride surface.

That is the BIF's tiling. The kernel sets `BIF_TILE1 = 0x0beffe00` and
`BIF_TILE2 = 0x0cffff00`, as iOS does: in the DDK's layout, 0xe0000000 to
0xefffffff tiled with configuration 0xb (stride 4096) and 0xf0000000 up
with 0xc. The render node picked addresses from 0xf0000000 down, so every
target Mesa made was at 0xefcf0000 -- inside window 1. Clears are uniform,
and a uniform clear looks the same in any layout: M12's "every pixel
right" over 100 clears could not see it. Nor could sgx2d, which writes the
framebuffer at 0x90000000. The fault that made the kernel align buffers
to 64 KiB (0xefcf0000 for a target at 0xefcff000, M12) was the same
swizzle. The "R and G mixed by position" fit of the second run was
pixels from other places.

Checked: `SGX_FRAME=align` (targets at 0xc0000000) gave 11 of 11 with no
rebuild. Fixed on both sides:

- the kernel picks addresses below 0xe0000000 (`SGX_TILED_VA_START`); an
  address in the windows can still be asked for, for tiled surfaces later;
- Mesa, for kernels before that one: a buffer the kernel put in the window
  is made again below it at an address of its own (`untiled_bo`,
  sgx_device.c, next fit).

With the new Mesa on the old kernel: `sgx-gl gltri` 11 of 11, the target
at 0xdfcf0000; `glclear 20` 0 wrong.

**M13a is done.** The colours per vertex, the uniform's colour and the
turned triangle are all right, through the pack's vertex side and iOS's
pixel side. Next is M13b.

**M13b: our own vertex side (2026-10-04, done the same day).** Draws no
longer go through the pack's vertex half (`r g b a u v x y`, a two-phase
vertex program, three fetched attributes). A vertex is now the position
(`x y z w`) and then N varyings of four floats each, N from 1 to 8, each
kept as F16 or F32 (`struct sgx_frame_layout`, sgx_frame.h). For each N
the frame has, written once:

- a vertex program in the pack's code page: `PHAS`, `SMLSI` +1, then
  `vmov.f32 o2k.xy, pa2k rpt4` until the 4 + 4N words are moved, then the
  vertex emit (`fb275000a0200000`). This is iOS's own shape (v00_vec4: there
  the `SMLSI` has the swizzle form `[2,3,0,1]` because the app's position
  is the second attribute, fetched into pa4..pa7; ours comes first). VMOV
  repeats at most 4 times and its register fields count pairs, so `o3` in
  the encoding is o6. The programs for N = 0..8 take 52 instructions; the
  end-of-tile slots shrank to 11 instructions (`0x58`), six of them;
- a vertex fetch PDS program in the EXT window: one attribute a vec4,
  `{address, first pa << 8 | 3, stride (row 0 only), 0}`, then the
  vertex program's DOUTU row; code `67800072`, `2f0n91a3` per row,
  `030R01f5`, `af000000` (pds.py's `vertex_fetch`).

Per render: state word 16 = (4 + 4N) << 24 | `0x001000`, word 19 = `111`
per varying, word 20 = one bit per F16 varying, and the pixel side's
iterate PDS is the one for the colour varying, F16 (`2fc0V00f`, two
registers, `pck.u8.f16 o0, pa0`) or F32 (`0fc0V00f`, `07040c32` four
registers, `pck.u8.f32 o0, pa0, pa2` = `40840c3da01d8002`, v07_highp).
`SGX_FRAME=packvertex` goes back to the pack's vertex side.

The one real unknown was the VDM word after the fetch's address. With the
pack's `0x07800604` and three data rows the render hung (TA not done);
with only its low bits changed, positions came out of the wrong words:
the TA took 12 words a vertex whatever word 16 said. A vertex of 12 words
(N = 2) drew right at once, which pointed at the word, and iOS's template
captures settled it -- three layouts, three words:

| capture | attributes | vertex | VDM fetch word |
|---|---|---|---|
| logs/ios/depth | 1 (pa 4) | 4 words | `0x03800202` |
| tmpl/vcolor, tex, size | 2 (pa 8) | 6 or 8 words | `0x05800403` |
| mod (the pack) | 3 (pa 12) | 10 words | `0x07800604` |

So: attributes in bits 26:25 and up (at least to bit 28: N = 8 is 9 << 25
and works), the primary attributes the vertex program gets in 11:7, the
fetch's data rows in 6:0, `0x01800000` always. Our word is `(1 + N) << 25 |
0x01800000 | (4 + 4N) << 7 | (N + 2)`; `SGX_FETCH=word` overrides it. The
address word's tag differs too (`0xf` for one or two attributes, `9` for
three, `7` for a two-word attribute of the GL driver's own); we keep the
pack's 9, and `SGX_FETCH_TAG=15` drew the same, so its meaning is open
(M30: the vertices a vertex task takes, less one -- 9 overlapped vertices
of three varyings and more).

Checked on the iPad: `SGX_DRAW_LAYOUT=N[,K][,f32]` (sgx_draw.c) sends N
varyings with the colour the K-th and the rest a filler colour that would
fail the checks. `gltri` 11 of 11 with N = 1..8, the colour first, in the
middle or last, F16 and F32; the default (N = 1) 11 of 11;
`packvertex` and `packpixel` 11 of 11; `glclear 20` 0 wrong.

**State word 5 (the pixel PDS info), partly read** from the corpus by
what each pixel program uses: bits 31:27 = (primary attributes +
temporaries) / 4, rounded up -- v00 (2 pa) 1, v07 (4 pa) 1, v04 (3 pa + 2
temps) 2, v05 (16 pa) 4, c04 (no pa, 10 temps) 3, no pa and no temps 0.
Bits 26:23 (0, 6, 3, 4 in those cases) are not read yet (M30: 12 / fours,
but at least 1); 17:13 = `0x1f` always. M13c's programs will need it.

**M13c, step 1: the first fragment compiler (2026-10-04).** Fragment
shaders now run on the GPU as compiled USSE programs; M13a's per-vertex
colour is the fallback for what the compiler does not take yet.

*The encoder* (`sgx_usse.c/.h`, no Mesa headers): V32NMAD (mul, add,
min, max, frc), VMAD2, VCOMP (rcp, rsq, log2, exp2), VMOV and its
conditional form, LIMM, and VPCK `u8.f32`. Registers are given in 32-bit
units; the encoder turns them into the 64-bit register and lane the float
instructions address (masks and swizzles), and the banks into their
two-bit selects and extension bits. Field positions are taken from
iOS's programs; the code is ours (MIT). `mesa/host/usse-test.py` builds
it with `cc` and checks 21 cases with `usse-dis.py`, five of them against
iOS's own words bit for bit (`pck.u8.f32 o0, pa0, pa2` from v07, four
`rcp.f32` from p05).

*The compiler* (`sgx_compiler.c`) is deliberately plain:

- NIR: io lowered to slots, then scalarised, loops unrolled, ifs
  flattened into selects (`nir_opt_peephole_select`, any size), ints and
  bools lowered to floats (as nir_to_tgsi does for float-only GPUs, with
  `fcsel`); floor, ceil, trunc, round, sign and sin/cos lowered by NIR's
  options to `ffract` and arithmetic. A shader that keeps control flow is
  not compiled.
- Every value is a scalar F32 in an even temporary (lane x); varyings are
  read where the PDS put them (input i in pa4i..pa4i+3, F32) and uniforms
  where the loader put them (word n in san), any lane, through swizzles.
- A temporary is taken when a value is made and given back after its last
  use. Constants are loaded with LIMM where used. fneg and fabs fold into
  operands that take them (V32NMAD's first source both, its second abs
  only, so commutative operands swap).
- Comparisons (`slt sge seq sne`) subtract and test the difference with
  the conditional move; `fcsel` is the conditional move itself.
- The colour is moved into four temporaries in a row and packed with
  `pck.u8.f32 o0, rA, rA+2 scale`, which clamps to 0..1 (glfs `saturate`).

*The frame* (`sgx_frame.c`) places a program at its first draw: the code in
a 256 KiB code buffer of its own (filled from the start, never reused --
the USSE caches code), its PDS program in the EXT window: data `{doutu,
temps, 0, control words...}`, code `070001b5`, one iterate DOUT per input,
`af000000`. An F32 vec4 iterate is control `0dc0V00f` (`0fc0V00f` the
last) and DOUT size `0x32`; the DOUT word for the control word at data
word d is `0x07000000 | ((d+2) >> 2) << 18 | d << 10 | ((d+1) & 3) << 8 |
size` -- it fits all nine iterate words in the corpus (v04, v05). The
registers follow one another from pa0 in DOUT order. Per draw: state word
6 the program's PDS, word 5 = `fours << 27 | (fours > 1 ? 12 / fours : 0)
<< 23 | 0x0003e000` with fours = (4 * inputs + temps) / 4 rounded up -- the
three corpus values (2, 3, 4 fours) and now 6 on the device (glfs `mad`:
4 pa + 17 temps) -- and, if the program reads uniforms, word 4 a loader:
`{uniforms, count - 1, 0, 0}, {doutu of an empty program, 2, 0, 0}` with
`07018113 070401a5 af000000` (iOS's for u00), the words DMA'd into sa0...

*Tested* by `tools/sgx/gl/glfs.c`: 23 fragment shaders over a full-screen
quad with two affine varyings and three uniforms, each against the same
arithmetic in C on a 24 x 24 grid. On the iPad all 23 within one or two
steps of 255: varyings, uniforms (vec4 and float), add, mul, mad, min,
max, clamp, fract/floor/ceil, division, sqrt and inversesqrt, exp2/log2/
exp/log, pow, sin/cos, dot/length/normalize, comparisons and the ternary,
step, mix, smoothstep, abs/sign/negation, an if/else, a loop, out-of-range
colours, a 39-instruction expression. `gltri` stays 11 of 11.

Two lessons on the way. The first run kept the colour's values only until
`store_output`, so the output block could land on them; and a test's
varying `s * t` is not interpolated as `s * t` -- varyings are affine over
each triangle (glfs's are now).

`gl_FragCoord` (2026-10-08) is one more iterate: the PDS iterates the
pixel's position like a varying, source 13 in the control word's bits
15:12 (`0x0fc0d00f`), the fragment shader reads it as an input like any
other (glfs 25 of 25 with two cases on it). discard and gl_FrontFacing:
M19.

Not yet: textures (M14), real control flow (M20), F16 for mediump, two lanes an instruction, constants from
the hardware's table, more than 128 uniform words (one DMA so far; glfs
loads at most 9 words, the pack's state program 21 the same way).
`SGX_NOCOMPILE=1` draws the M13a way; `SGX_DEBUG_SHADER=1` prints each
program's words for `usse-dis.py words`.

## M14: textures, blending, depth and the rest of GLES 2.0's state

Every GLES 2.0 texture format (RGBA8, RGB565, RGBA4444, RGBA5551, L8, A8, LA8,
and PVRTC, which the hardware has), wrap and filter modes, mipmaps (the
transfer queue made iOS's), non-power-of-two rules; blend modes through
`nir_lower_blend`; depth and stencil through the ZLS registers (depth seen in
M5, stencil not yet); scissor, viewport, culling, polygon offset, points and
lines. Done when `glmark2-es2` runs its scenes and SuperTux's GL renderer
draws.

**M14, step 1: textures in compiled shaders (2026-10-08).** `texture2D`
(with a bias or a level; `texture2DProj` lowered by NIR) compiles to
`SMP`, the dependent-read path iOS uses for computed coordinates (corpus
t01, t07): the coordinates into a register pair, the texel into four
temporaries as F32, `WDF` before it is read. iOS's non-dependent reads go
through the PDS (t00: iterate, texture DOUT, the raw texel in pa0); ours
all go through SMP, one path for every case.

- `smp2d.f32.f32`: the texel comes back as the channels' integer values
  (0..255 for RGBA8), so the compiler scales it by 1/255. Seen first as
  every non-zero channel saturating to 255 -- an RGB565 2x2 of 0s and
  255s passed and nothing else did.
- The four state words, in sa after the uniforms (aligned to four): word
  0 `0x03fe0000` with the sampler's bits, word 1 `0x0c << 24 | log2 w <<
  16 | log2 h`, word 2 the address, word 3 0.
- Word 0's sampler bits, the wrap from the corpus and the filters by
  flipping bits one at a time under `gltex` (`SGX_TEX_WORDn=mask` flips
  word n's bits): `1 << 4` / `1 << 7` clamp s / t (iOS's `0x90` for
  CLAMP_TO_EDGE; 0 is REPEAT), bits 13:12 the magnification filter and
  11:10 the minification filter, 0 point, 1 (or 2) bilinear, 3 point
  again. Bits 8, 17-20, 27 and 31 broke the lookup (format and size),
  words 1's top byte and word 3 did nothing visible.
- **skipinv**: with every instruction carrying it (bit 55, as iOS sets it
  on most), bilinear pixels along the quad's diagonal came out point
  sampled: the pixels just outside a triangle skip the moves into the
  coordinate registers, and the sampler takes its level of detail from the
  2x2 block's coordinates -- garbage there, so minification and point
  sampling. iOS clears skipinv on the instructions that make a dependent
  read's coordinates (t01's `mul.f16`, t07's `pck.f16`); a program of ours
  that samples has it clear on every instruction.
- Textures stay linear in their buffers (CPU maps, render targets); a
  sampled one gets a twiddled copy made by the CPU when its content has
  changed (`seq`: a write through a map, a render or a clear into it):
  always RGBA8, any format through `util_format_unpack_rgba_8unorm`,
  Morton order with y in the even bits, rectangles as a row of squares,
  padded to powers of two with the last row and column repeated. Before
  rewriting a copy the driver waits for the last render, which may still
  sample it; renders take the copies' buffers along.

`tools/sgx/gl/gltex.c` checks lookups against the texel each pixel should
land on (pixels near a texel's edge left out): RGBA8 4x4, a 64x16
rectangle, REPEAT, CLAMP_TO_EDGE beyond 0..1, a swizzle, a texel times a
uniform plus a varying, two textures, L8, RGB565, texture2DProj, bilinear
magnification of a 2x2 (within 4 of 255: the hardware's weights are
coarse) and bilinear minification of a 64x64 at about two texels a pixel
(within 8). On the iPad 12 of 12, most with no difference at all; glfs
23 of 23, gltri 11 of 11.

Not yet: mipmaps (and the mip filter, probably bits 9:8), sizes that are
not powers of two (the padded copy is sampled with unscaled coordinates),
MIRRORED_REPEAT (bits 3 and 6 did something), cube maps, render targets
sampled without a CPU round trip.

**M14, step 2: blending (2026-10-08).** GL's blending is done by the
compiled fragment shader, as on every tile-based GPU that lets a pixel
program read the tile: o0 holds the tile's colour when the program starts
(the background object loads the target into it; an earlier triangle of
the same render has written its own), packed RGBA8.

- `nir_lower_blend` builds the equation, the factors, the constant
  colour and the colour mask into the shader (after `nir_lower_fragcolor`,
  which it needs: it does not take FRAG_RESULT_COLOR); the destination
  comes from `load_output` with `fb_fetch_output`, which the compiler turns
  into o0 unpacked to F32.
- The unpack is `pck.f32.u8 rD.xy, o0 scale` twice (channels 0, 1 into
  rD, rD+1 and 2, 3 into rD+2, rD+3). Found with `SGX_DEBUG_FBFETCH=word`
  (the compiler unpacks a LIMM'd word instead of o0): one VPCK with an
  F32 destination writes one 64-bit register -- with mask xyzw, z and w
  stayed unwritten -- and the first channel select reaches channel 2 but
  reads 3 as 1, so channel 3 has to come in as the second of a pair.
  Before that, every case blended RGB right and took the destination's
  alpha from its green.
- The blend colour is four more words of sa after the textures' state
  (`load_blend_const_color_{r,g,b,a}_float`; NIR's intrinsics are not in
  rgba order).
- Each blend state is a variant of the shader, compiled at the first draw
  that needs it (eight a shader, the oldest dropped); no blending and a
  full colour mask is the shader as first compiled.

`tools/sgx/gl/glblend.c`: a gradient, then a second full-screen quad
blended over it, against the same equation in C on the gradient as the
8-bit target holds it -- no blending, SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ONE/ONE,
DST_COLOR/ZERO, premultiplied, SUBTRACT, REVERSE_SUBTRACT, CONSTANT_COLOR
and CONSTANT_ALPHA, separate RGB and alpha functions, DST_ALPHA,
SRC_ALPHA_SATURATE, ZERO/ONE, a colour mask, and the quad twice in one
draw (blending onto itself within a render). On the iPad 14 of 14, within
one step; glfs 23, gltex 12, gltri 11, glclear right.

**M14, step 3: depth, and draws gathered into renders (2026-10-08).**
The draw's ISP state B carries GL's depth test (compare in bits 24:22, bit
20 set when depth is not written; M5's capture), vertices carry the draw
module's z, and a tile starts at the far depth. The test holds within a
render; to make it hold across draws, draws are no longer a render each:

- the context gathers them (`struct sgx_batch`: vertices, secondary
  attributes, the uploaded program's copy, ISP state, the textures'
  copies) and `sgx_frame_render` draws up to 200 in one render: a state
  block, state program and secondary attributes in a 1 KiB slot each
  (EXT window), every draw's vertices one after another, each draw's
  indices taken from the pack's 0, 1, 2, ... buffer at its first vertex
  (rounded to eight for a 16-byte boundary), ten VDM words a draw;
- what renders the gathered draws: a flush, a map of the target or of a
  sampled texture, another framebuffer, any clear (a render starts at the
  far depth, so a depth clear starts a new one; a clear value other than
  1.0 is not done yet), a texture whose copy must be made again while a
  gathered draw still reads it, a full batch;
- `SGX_FRAME=packvertex|packpixel` still draws a render at a time.

`tools/sgx/gl/gldepth.c`: two overlapping quads, near (green) and far
(red), in one draw and in two -- the test off, LESS and LEQUAL in either
order, ALWAYS, depth writes off. On the iPad 8 of 8 (two draws was the
one wrong before the gathering); glfs 23, gltex 12, glblend 14, gltri 11,
glclear right.

Speed (`tools/sgx/gl/glspeed.c`, small quads, a uniform colour each, a
glFinish a frame): 1 draw 3.1 ms a frame, 10 draws 3.4 ms, 100 draws 10.2
ms, 400 draws 36.7 ms -- about 10 000 draws a second. A render a draw
(`packvertex`) is as fast (100 draws 9.1 ms): a render costs little on the
GPU (tiles without geometry seem to be passed over), and the time is the
CPU's for each draw -- the draw module running the vertex shader, the
state, the copies. That is M15's to look at; the gathering is for depth.

**M14, step 4: perspective and the depth clear value (2026-10-08).**

- Vertices reach the TA as clip coordinates, as a vertex shader would
  hand them over: the draw module keeps 1 / w in the position's w, so the
  driver multiplies x, y, z (back in -1..1) by w and gives w. The TA
  divides and interpolates with it: `tools/sgx/gl/glpersp.c`, a quad with
  w 1 on the left and 3 on the right, matches the perspective-correct
  varying exactly (69 steps from the affine one).
- The depth a render's tiles start at is register `0x4b8` in the 3D
  block (1.0 as the kext fills it, M4; the block's address is in the TA
  command at `+0x50`, the register at `+0x80` in it) -- the background
  object's depth. A gathered render starts at the last depth clear's
  value; `gldepth`'s `clear_half` (cleared to 0.5, the far quad at 0.75
  fails LESS) passes. Without loading depth from memory, a render after a
  flush in the middle of a frame starts from that value again.

gldepth 9 of 9; glfs, gltex, glblend, glpersp, gltri, glclear unchanged.

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

**Step 1 (2026-10-08): KMS in the render node's own device.** Not
simpledrm and kmsro after all: nothing programs the display pipe in either
case (iBoot set it up to scan out its framebuffer; both would only copy
frames into that memory), and the copy is better done by the GPU than by
simpledrm's CPU reading shmem pages the GPU wrote (no cache maintenance
for its own buffers on ARMv7, and 3 MiB a frame). So `apple-sgx`'s primary
node, `card0`, is now a KMS device too (`apple_sgx_kms.c`): one plane, CRTC
and connector with the framebuffer's mode (768x1024, a nominal 60 Hz, 119 x
159 mm); framebuffers are any of the render node's buffers (Mesa's through
GBM, or dumb buffers, pitches of 64 bytes); showing one is a copy onto the
screen by the 2D engine (`apple_sgx_fb_show()`, the fbcon engine's blit),
the whole plane on a flip and the damaged rectangles (widened to 16
pixels) otherwise, after the plane's implicit fences -- the renders that
write the buffer put theirs on it. No vblank interrupt: a flip completes
when its copy has (`drm_atomic_helper_fake_vblank`). simplefb keeps the
console on the same memory. A full-screen copy takes ~1.9 ms (200
`copy 0 0 0 0 768 1024` through debugfs: 0.38 s).

Nothing changed in Mesa for it: GBM on `card0` loads the driver by the
kernel's name (`apple_sgx`), scanout buffers are its linear render targets
and `resource_get_handle` gives KMS their GEM handles. `tools/sgx/gl/glkms.c`
is the test: EGL on GBM, a turning square over a gradient, each frame shown
with a page flip, then a still frame and `/dev/fb0` checked against it.
Kernel #293: **118 frames a second, every pixel of the screen right**; the
rest of the tests unchanged.

**Step 2 (2026-10-08): dma-buf import, linear textures, weston.** The kernel
(#294) says where a handle's buffer is (`GEM_INFO`, UAPI version 2): every
buffer is the render node's own, so a dma-buf's handle names one already
mapped. Mesa keeps its buffer objects by handle (a dma-buf of ours imported
again in the same process comes back as the same handle) and takes dma-bufs
in `resource_from_handle`; `samplerExternalOES` is a 2D sampler. kmscube
`-M rgba` (an ABGR8888 dma-buf through `EGL_EXT_image_dma_buf_import`): 142
frames a second, the colour bars in order.

Textures written by the GPU or by others, and those not a power of two in
size, are sampled as they are, linear -- the CPU's twiddled copy would read
them back from write-combined memory each time, or is padded. The state
words are the 2D engine's for iOS's IOSurfaces: the stride in 16 bytes less
2 at bit 16, bits 11:9 all set (every other value of the three skews the
rows; tried on gltex), `0xcc000000 | (w - 1) << 12 | (h - 1)`, the address,
`0x10000000`. The sampler reads bytes as B G R A; R8G8B8A8 textures and X8
ones are swizzled back in the shader (`nir_lower_tex`, a variant key). What
is lost: the minification filter (point sampling), and formats but the 8-bit
RGBA orders (L8 and 565 keep the copy). gltex 14 of 14 (with two NPOT cases).

weston 14 (DRM backend, GL renderer, through seatd) then runs on the GPU:
the desktop shell, weston-terminal. It showed a bug of the compiler's: a
program that samples clears skipinv everywhere, so that the pixels around a
triangle compute the 2x2 blocks' derivatives -- and their output writes
landed too, blended twice along every shared edge (dashes on the panel,
exactly one blend darker). The output writes keep skipinv now; `glseam`
checks a blended textured rectangle's every pixel. The panel is now the same
as weston's pixman renderer draws it.

What is left for a desktop: GL clients under weston. Every process that
draws loads the template frame at the same fixed addresses (the pack's
parameter buffer, PDS block, state, code page), so a second one cannot start
while weston runs (and it crashes instead of failing: to fix). That needs the
parameter buffer to become the kernel's, shared by everyone (the microkernel
keeps its state, M12), and the rest of the frame made by Mesa at addresses
the kernel picks.

## M18: vertex shaders on the GPU

Until now the draw module ran every vertex shader on the CPU (M12) and
handed the TA clip coordinates through the pack's vertex program. Now the
compiler (`translate()`, shared by both stages) makes USSE code of the
vertex shader too, and the TA runs it.

**Step 1 (2026-10-08): the shader, its fetch, its uniforms.**

- Registers: attribute n in pa4n..pa4n+3, as the vertex fetch puts it
  (one F32 vec4 each: the CPU unpacks any vertex format with
  `util_format_unpack_rgba`), the temporaries in pa after them, the
  uniforms in sa from sa0. Outputs from o0: the position, then the
  varyings in the order the fragment shader iterates them; then
  `EMIT_VERTEX` (`0xfb275000a0200000`). A vertex shader is compiled once
  for each list of varyings a fragment shader wants.
- The vertex fetch (PDS, in the frame's code heap): a DMA row an attribute
  (`{vb + 16 i, 4 i << 8 | 3, i ? 0 : stride, 0}`), the index fetch, a
  DOUTU of the shader. The VDM word after its address is `(1 + varyings) << 25 |
  0x01800000 | regs << 7 | (attributes + 1)`, regs = 4 x attributes +
  temporaries rounded up to four. Bits 31:25 count the vertex's *output*
  vec4s, not its attributes -- with the attribute count there the
  varyings came from the wrong words of the vertex.
- The uniforms: a loader per draw before the state (`0x1000e100 |
  ceil(words / 4)` after its address in the VDM stream), the words in sa.
  A uniform's offset in NIR becomes a float in `int_to_float` unless it is
  folded into the base first (`fold_uniform_offset`; a `mat2` turned the
  triangle by garbage without it).
- Indices: strips and fans are made lists on the CPU (a strip's odd
  triangles with their first two vertices swapped), the indices go into
  the frame (up to 65536 vertices a draw), the primitive word `0x81c00000
  | count`.

Every test right on the GPU's vertex side (gltri 11, glfs 25, gltex 14,
glblend 14, gldepth 9, glsize 15, glseam, glpersp); glspeed **22 975
draws a second against 9 545** with the draw module (`SGX_CPU_VS=1`
forces it), 44 000 with 400 draws a frame.

**Step 2 (2026-10-09): the viewport and culling.** `tools/sgx/gl/glcull.c`:
a triangle anticlockwise and one clockwise with culling off, back, front,
front clockwise; the windings swapped between the sides; a quad in a
viewport off the origin; two depth ranges -- into a texture, then into a
pbuffer (the window system's kind of buffer: gallium flips y with a
negative scale). The draw module's way is the reference: 17 of 17.

- The viewport: state words 9..14 are **translate, then scale**, for x,
  y and z. The whole target's (half the width twice, half the height
  twice) cannot tell the order; the off-origin viewport with the words
  the other way round put the quad's left and bottom edges at -192 and
  -128 (clipped), the pbuffer's flipped y put everything above row 0.
  Depth: 0.5..1 puts z 0 behind a clear to 0.5, 0..0.5 in front.
- Culling: state word 18 (`0x00088000` in the pack's state), **bits 1:0**:
  1 drops the triangles clockwise in the target as the TA sees it (row 0
  at the top), 2 the anticlockwise ones -- gallium's sense of the winding,
  so `cull_face` and `front_ccw` map straight to it, flipped targets
  included. Found by flipping state bits (`SGX_STATE=word:mask[,...]`
  xors every draw's state words) and drawing one triangle a render, four
  places, both windings, three rotations of its vertices. On the way:
  word 16's bits 3..6 each clip the right half away whatever the winding
  (clip planes, presumably), bit 9 gave the left triangle the right one's
  colour, bit 13 timed the render out; word 18's bit 16 drops
  everything.
- A bug of the driver's under it: binding a rasterizer state calls
  `draw_set_rasterizer_state`, whose flush has the draw module's wide
  point stage bind the *previous* state again (it restores its own
  change). The driver kept that one, so the GPU's vertex side went by the
  last draw's culling after every draw module draw -- the bind is now
  recorded after the draw module's.

glcull 17 of 17 with nothing left to the draw module; the rest of the
tests unchanged; glspeed 23 209 draws a second. kmscube's vertices are the
GPU's now: its last frame is the draw module's to within 1 in a channel,
180 frames a second either way (36 vertices a frame: the copy to the
screen is the limit). es2gears under weston takes **19% of the CPU
against 55%**, the gears lit and culled as before; weston's own drawing
(a flipped target) goes the same way.

The draw module still does what the GPU's vertex side does not: points
and lines, flat shading, polygon modes other than fill, clip planes,
primitive restart, culling both faces.

## M19: discard and gl_FrontFacing

**gl_FrontFacing (2026-10-09)** is bit 0 of special register g16, as iOS's
v09_frontfacing reads it: `VTST p0 = and(g16, #1) ne 0` (`0x488b0281600c2801`).
The compiler makes `load_front_face` `0 < load_front_face_fsign` and the sign
`LIMM -1; VTST p0; p0? LIMM +1` (LIMM's predicate in bits 43:41). The bit is
set for triangles anticlockwise in the target as the TA sees it -- gallium's
sense -- so a shader that reads it is compiled for the rasterizer's
`front_ccw` (a variant key). glcull's `facing` cases (red the front, green the
back, both windings, texture and pbuffer): right the first time, and wrong
with the sense flipped, so the test tells. On the draw module's path they
were wrong at first: the driver's vbuf render turned every triangle
anticlockwise, a precaution from M12 against the pack's state culling --
which it does not (word 18's bits 1:0 are 0, M18). The triangles now reach
the TA as the draw module winds them. glcull 21 of 21 either way.

**discard (2026-10-09).** iOS's c00_discard (`if (u0.x > 1.5) discard`) is
the one draw in the corpus with ISP state A `0x09d00300` -- bit 27 set, the
other 61 have `0x01d00300` -- and a program of two phases: the first
(`PHAS wait 1, next at` the second) tests into predicates (`p0` the
condition, `p1.x` true, `p0? p1.y = ...`), moves the colour into pa0 and ends
with `f9340426c0000280`, which Vita3K's tables read as `p1? KILL` (with the
end bit, bit 50); the second (`PHAS wait 7`) is `or o0, pa0`. The PDS program
is the usual one. Rebuilt in our compiler (`usse_vtst_ne0`, `usse_kill`; the
encoder test has iOS's word bit for bit), on the iPad (`gldiscard`):

- one phase, KILL anywhere, bit 27 set: nothing killed, the colour written,
  no depth written (the ISP waits for a feedback that never comes);
- two phases with iOS's `wait 1`: the second phase never runs -- nothing
  written without the depth test, black and depth written everywhere with
  it; the same with a constant colour made in the second phase;
- `wait 0` or 5: the phases chain (as a vertex program's do) and the colour
  is written, KILL still kills nothing; 2, 3, 4 and 6 time the render out;
- no change from the KILL's predicate (always, p0, p1, !p0), the test's
  sense or channel (iOS's `p1.x` / `p1.y`), the bits where iOS's word and
  Vita3K's pattern differ (37, 29:28), or ISP state A's bits 25..27 in any
  combination (iOS's exact `0x09d00300` included).

So the punch-through pass wants something outside the draw's state and
program -- likely in the render's 3D registers, which the GL payloads
captured on iOS (none with a discard) cannot show.

(Retried after M20 found the F32 VTST these tests used setting its
predicate wrong: with the bitwise test, the predicate as `kill`, as
`!kill`, iOS's `p1.x` true then `p1.y` the test, `p1.x` alone, bit 27 or
iOS's exact ISP state A, a constant colour in the second phase -- every
pixel of the punch-through draw stays invisible and writes no depth. The
KILL's predicate is not what is missing.) Until then **discard
keeps the tile's colour**: the conditions are gathered into a temporary,
and at the end each channel is `kill != 0 ? dst : colour`, dst being o0
unpacked as the blending reads it (every draw is a translucent object,
shaded in order). The colour is right in every case, blending included; the
depth is not: a discarded pixel still writes it when depth writes are on.
`gldiscard`: a varying's half, two discards, a uniform both ways, an alpha
test from a texture, blended -- 6 of 6 on both vertex paths, the depth case
reported as known wrong.

## M20: branches -- loops and ifs that stay

Until now every shader had to come down to one basic block: ifs flattened
into selects, loops unrolled, anything else drawn the M13a way.

**The hardware's branch** (iOS's c04_loop_break, a loop with a break):
`p0? br +21` out, `br -21` back -- `0xf800004000000000` with the predicate in
bits 58:56 (extended: 1 p0, 5 !p0) and a 20-bit offset in instructions from
the branch itself (bit 38: relative). That program starts with `PHAS mode 1`
(`0xfa44270000000000`), its PDS's DOUTU data word is 3 where the others have
2 (bit 0), and **skipinv is clear on every instruction** -- as in programs
that sample. All three are needed or wanted:

- Without clearing skipinv, the renders hung now and then: the pixels around
  a triangle skip the test of a loop's condition but not the branch, and go
  round on a predicate left from before. Which ones depends on the pixels
  in each group -- the same shader hung or not from run to run, which for a
  while looked like PHAS's temps field mattering (it does not). The output
  write keeps skipinv set (M16).
- A hand-made program in place of a shader (red where a branch went as
  meant; a throwaway, not kept) showed the plain `br`, `p0? br`, `!p0? br`
  and a backward loop all right -- once the test feeding them was. The F32 test
  (`src - #0` by VSUB, sign test "none", zero test "zero") set the predicate
  wrong; the bitwise form of iOS's facing test, `and(r, r) eq 0`, is right:
  a boolean is 0.0 or 1.0 here, so its bits are 0 or not. (VTST's sign and
  zero tests combine by bit 39: OR, the old encoder's, makes "none" always
  true.)

**The compiler** keeps NIR's structure: ifs of up to 32 instructions a side
are still flattened (`SGX_FLATTEN_LIMIT`), the rest and every loop NIR does
not unroll (up to 32 iterations it does) become branches:

- out of SSA for the phis only (`nir_convert_from_ssa`): a phi web is a
  register, a temporary for the whole program; `load_reg` copies it (the
  register changes while the value may still be read), `store_reg` moves
  into it;
- three walks over the control flow that number alike (an instruction, an
  if's test and a loop's end marker an index each): defs and loop spans,
  then the liveness -- a value read in a loop it was made before lives to
  that loop's end -- then the code;
- an if: `VTST p0 = and(c, c) eq 0`, `p0? br` past the then-list, `br` past
  the else-list; a loop: its body, `br` back to its start; break and
  continue: `br` past the loop or to its start;
- discard's flag is 0 from the program's start (a discard in a branch may not
  run); an output written in control flow, or branches in a vertex shader,
  still fail (the draw module's way for the latter);
- a pixel's registers, primary attributes and temporaries, at most 64: an
  unrolled loop with a break, flattened whole, took 65 and hung the GPU --
  which is why the flattening has a limit now.

`glfs` grows seven cases: a loop to a uniform's count, a loop to a
varying's (each pixel its own count), break, continue, nested loops, a loop
in an if, and a Mandelbrot set (24 iterations with a break, unrolled into
branches) -- 32 of 32, on both vertex paths; the rest of the tests
unchanged. The shadertoy sphere (24 ray-marching steps) and plasma under
kmscube draw as before, 14 and 32 frames a second.

## M21: uniform arrays read through an index

`a[i]` with an `i` the compiler does not know -- a loop's counter to a
uniform's count, a varying's value, a vertex's -- used to fail (the draw
module's way, or M13a's grey).

**The index register** (iOS's c04_loop_break reads its array so): `shr
index1, r0, #5` -- VBW with destination bank INDEX (extended bank 2, number
1) -- then `or r1, idx1101, ...`: a source in bank INDEXED1 (extended bank 0)
whose 7-bit number is the bank indexed in bits 6:5 (0 temp, 1 output, 2 pa,
3 sa) and an offset in 4:0, in 32-bit words, index1 added. (Vita3K's
decoder has the bank and offset; that index1 counts 32-bit words for a
32-bit move, iOS's array code shows -- and the tests.) The encoder makes
VBW's `and`/`or` with an immediate (iOS's `or o0, sa14, #0` and `and r2,
r0, #0x1f`, bit for bit).

**The compiler**: an indirect `load_uniform` loads its array's whole range
into sa, then `fmad t = offset * 4 + (base + 2^23)` -- the offset is an
integer held as a float here, so t's low bits are the word as an integer --
`and index1, t, #0xffff`, and a `or rN, idx1(sa + c), #0` for each
component c. NIR's `ftrunc` (from `int(x)`, made after the options' own
lowering ran) is lowered by the driver: sign times `|x| - fract |x|`.

**33 uniform words and up** never worked in a pixel program, indexed or
not: state word 5's `0x3e000` is the count of secondary attributes less 1
in bits 21:13 -- 31 in every iOS state, so sa32 and up read nothing (a
second DMA, tried first, changed nothing). The count is now the words
rounded up to 32, less 1: 32 exactly (33 registers) hangs the GPU, 63
works. One DMA loads them all (128 words at most; the uniforms now go
before their loader in a draw's slot). A vertex program's count is its VDM
word's, in fours, and was right.

`glfs`: an index from a varying, from a uniform, a loop to a uniform's
count summing the array, two indexed reads in one shader, 33 words without
an index, and a vertex shader indexing by its vertex (`vs_array`) -- 38 of
38 on both vertex paths; kmscube's frame the same as the draw module's.

## M22: vertex shaders that branch

M20's branches in a vertex program, on the GPU: the same PHAS mode 1 and
skipinv clear on all but the vertex's output; and bit 0 of the vertex
fetch's DOUTU data word, as the pixel side's -- without it a loop whose
count differs between the vertices of a batch comes out wrong (a corner
of `vs_loop_vertex` lost its iterations), so the bit is what lets the
instances go each their own way. A vertex's registers (attributes and
temporaries in pa) are held to 64, as a pixel's.

`glfs` grows two vertex shaders: a loop to a uniform's count summing the
uniform array, and a loop to a count each vertex has its own (6s + 3t,
linear, so the interpolated result is exact) -- 40 of 40 on both vertex
paths, nothing left to the draw module.

What the draw module still does for GLES 2.0: points and lines (the TA's
primitive types are not known yet). The rest of its cases -- flat
shading, polygon modes, clip planes, primitive restart -- are not GLES
2.0's.

## M23: stencil; depth and stencil in memory (half of it)

**The stencil test** is ISP state B, the draw state's word 3 -- the
template's `0x0e000000` is ALWAYS (7) in bits 27:25 and nothing else -- laid
out as the later PowerVRs' ISPB: the compare in 27:25 (gallium's order, as
the depth compare in ISP A), the ops on stencil fail, depth fail and pass in
24:22, 21:19, 18:16, the compare mask in 15:8, the write mask in 7:0. The
reference is ISP state A's low byte. The ops' order is keep, zero, replace,
incr (saturating), decr, invert, incr wrap, decr wrap -- gallium's has the
last three another way round. Right the first time on everything
`glstencil` tries: replace then equal / not equal, incr twice, decr wrap,
invert, zero, the write and compare masks, the depth-fail op, never, less.

Its clear: a render's tiles start with stencil 0, and no register for
another value turned up (the 3D block's words and the TA command's flipped
with `SGX_BLK=off:xor` and `SGX_CMD=off:xor`). A clear to another value is
a quad first: a pixel program that writes nothing, depth ALWAYS without
writes, stencil ALWAYS, REPLACE by the value through the clear's mask. A
pixel program that writes nothing also came out of a colour mask of
nothing (nir_lower_blend takes the output away): it is `NOP` with the end
bit, o0 keeping the tile's colour -- before, such a shader was drawn
without its mask. The CPU's clear of the depth/stencil buffer's memory is
gone: nothing reads it (3 MiB a clear for nothing).

**Depth in memory -- the ISP's z load/store**, from iOS's depth capture run
through the kext's 3D block builder (`rtemu.py` with the depth payload): a
GL depth attachment puts the ZLS base (1 MiB aligned, BIF_ZLS_REQ_BASE
`0xcb0` in the open kernel driver's `sgx543defs.h`) at the 3D block's
+0x14, ZLSCTL `0x0015100c` at +0x6c and the load and store offsets from the
base (`0xe4000`) at +0x70, +0x74. Tried on the iPad with a buffer of our
own:

- **the store works**: the tiles land 32 x 32 F32, row by row, 4 KiB a tile,
  tile (x, y) at `(x + y * S) * 4 KiB` -- found with a depth plane whose
  value encodes the pixel (`d = 0.5 + 0.45 x + 0.0005 y`), the stored words
  decoded back into pixels;
- S, the row of tiles' stride, is ZLSCTL bits 10:4: `2 * (v + 1)` tiles
  (iOS's word has 0: S = 2 whatever the target -- its depth capture never
  loaded); bit 2 off stores nothing, bits 3, 16 and 19 change the format,
  bit 18 off stores nothing; tiles a render draws nothing in are not stored;
- **the load was not found**: no single bit of ZLSCTL (or pair tried), nor
  of the 3D block's +0x84, makes a render start from what the last stored.

So depth and stencil still live in the tiles only: a frame split into two
renders loses them (`gldepth`'s `two_renders`, counted as known). The ZLS
code is not kept; the layout and the stride are, here. (The load: M24.)

## M24: dEQP-GLES2, and what it found

dEQP-GLES2 (VK-GL-CTS `opengl-es-cts-3.2.9.3`, its GLES 2 module for the
surfaceless EGL platform) built for armhf Alpine under ARM emulation
(`tools/sgx/deqp/build.sh`, which also puts it on the iPad) and run there
with `tools/sgx/deqp-run.py`: a process per group, a case that crashes or
goes a minute without output marked so and the next process going on from
the case after; a pbuffer of 256 x 256, `rgba8888d24s8ms0`. All 19723
cases take about half an hour. What the first runs found, and what came of
it:

**Depth and stencil clears inside a render; no flush at a shader's
deletion.** Every clear ended the render, and depth and stencil live in
the tiles: what was drawn before them was lost. A colour clear still ends
the render; depth and stencil are cleared at a render's start for free (the
depth the tiles start at, stencil 0), anywhere else by a quad (a pixel
program that writes nothing, depth ALWAYS and written, stencil ALWAYS,
REPLACE by the value through the clear's mask), scissored ones too. And st
deletes the last program at the next `glUseProgram`: deleting a vertex
shader flushed the gathered draws, which point at its GPU variants, in the
middle of a frame; the variants live until the flush instead.

**Programs' GPU memory given back.** Most failures came in long runs: from
some case on, every case after it failed until the process ended. The code
heap (256 KiB) and the PDS programs' window were filled from the start and
never reused, so a process that made enough programs -- dEQP makes a few a
case -- had every later program refused and its draws dropped. Both are
`util_vma_heap`s now; a program deleted is retired with the last render
submitted, and its places are free again once that render is done (at
worst a new program waits for it). The fragment shaders' variants wait for
the gathered draws to be rendered as the vertex shaders' do. `glchurn`
makes, draws and deletes 3000 programs, each with its own colour (and one
in three its own vertex shader), going round the code heap some 37 times:
no program ran another's code, so the USSE's code cache does not keep a
place's old program across renders. Before, the 78th program was refused.

**Viewport and scissor.** The TA clips to a guard band of exactly 1.5
times its viewport -- a quad four times clip space drawn through six
viewports, odd sizes and fractional centres among them, comes out to the
pixel (the top-left rule at the edges) -- and to nothing narrower, so a
viewport smaller than the target let geometry past its edges, and the
scissor was not applied at all. Words 7 and 8 of the draw state are the
region clip: the first tile in bits 16 and up, the last in the low bits
(word 7 has bit 31 set) -- whole tiles only. So each draw's rectangle -- the
viewport's (a vertex shader's draw: the draw module clips to it itself),
the scissor's, the target's -- shrunk 1.5 times becomes the TA's viewport,
and the vertices are moved to land where they would have: x' = a x + b w,
y' likewise, a and b from the draw's viewport and the rectangle. A vertex
shader gets four uniform words after its own for them (two multiplies and
an add for x and y); the draw module's vertices get it on the CPU. Word
16's bits 3..6 look like clip planes, but the distances they read could
not be pinned down; they were not needed.

**Polygon offset** stays with the draw module (the TA's was not found),
which a vertex shader's draw with an offset goes back to. Its unit is a
24-bit depth buffer's times four: the draw module's own for the tiles' F32
depth -- a bit of it -- is lost between its z and the ISP's interpolated
one; two bits survive (dEQP's displacement cases), four for room.

**Points and lines** go through the draw module's wide point and line
stages, as triangles: it needs a fragment shader bound (it asserted),
and it leaves a line of width 1 alone, so width 1 is made 1 + 1/1024.

**Depth textures.** GLES 2 has `OES_depth_texture` whatever the driver
says, and with no depth format sampleable Mesa asserted in
`glTexImage2D` -- every `fbo.completeness` case with a depth texture,
`clip_control`. Z16, Z24X8 and Z24S8 are sampleable now: their copy is
grey, eight bits of the depth.

**Depth and stencil in memory: the load, found.** ZLSCTL, register `0x480`
(the 3D block's +0x6c), from iOS's store-only `0x0015100c` and flipping its
bits with a probe of two renders (a quad at depth 0.25 on the left, a
`glFinish`, then one over everything at 0.5 with LESS: loaded, the left
stays red):

| bits | |
|---|---|
| 2 | store depth (3 with it) |
| 17 | store stencil (16 with it), into the top byte of the 24-bit format |
| 14 | load depth (1 with it) |
| 13 | load stencil |
| 25:24 | the format stored: 0 F32, 1 24-bit with stencil above, 2 16-bit |
| 22:21 | the format loaded, the same way |
| 10:4 | the rows of tiles in twos, less 1 (M23) |
| 12, 18 | needed (iOS's); 26 hangs the render |

The first tries in M23 set bit 0 or 1 alone: bit 1 needs 14, and bit 0
is no load at all -- with it set the tiles' colour comes out of the
previous tile. Bit 8 of the 3D block's +0x84 (register `0x4bc`, `0x300`)
turns off the background object, which reloads each tile's colour from
the target and starts its depth: then nothing does.

Tiles a render draws nothing in are neither loaded nor stored, so a render
that starts from a clear and stores gets a quad over the target with depth
NEVER first: every tile is in it and stored at the clear's depth. A render
stores when a draw writes depth or stencil (or a clear's quad does), and
loads when there is something stored, no clear came first, and a draw reads
or writes them; a clear no draw uses is only noted, and the next render
starts from it. Depth alone goes F32 (what the ISP keeps), depth with
stencil the 24-bit format with stencil above. The buffer is 4 bytes a pixel,
in 32 x 32 tiles, rows of an even number of tiles. Across a render, depth
compared EQUAL comes out in diagonal bands: the ISP's interpolated depth
differs between two quads at the same z by a bit or so, varying with the
tile, F32 or not -- the hardware's, not the store's.

**Two-sided stencil**: a draw whose front and back faces have different
stencil states (or references) is two passes, the TA culling the other
faces in each.

**Texture wrap modes**: t is in bits 5:3 of the texture state's word 0, s
in 8:6, each 0 repeat, 1 mirrored, 2 clamped (iOS's `0x90`, both clamped,
matched the old guess of single bits with the axes the other way round).
A texture not a power of two in size and not 8-bit RGBA had a twiddled
copy padded to one, its coordinates not scaled: it gets a linear BGRA copy
instead, sampled as the RGBA ones are.

What dEQP-GLES2 says now (the first run's figures in brackets; NotSupported
not counted):

| group | pass | fail | crash |
|---|---|---|---|
| shaders | 9836 (4101) | 364 (5987) | 0 (112) |
| fragment_ops | 1922 (508) | 1 (1411) | 0 (4) |
| uniform_api | 1109 (1109) | 15 (15) | 0 (0) |
| texture | 311 (208) | 534 (637) | 0 (0) |
| fbo | 544 (451) | 0 (30) | 0 (63) |
| clipping | 597 (37) | 5 (554) | 0 (11) |
| draw | 100 (42) | 1 (40) | 0 (19) |
| rasterization | 37 (39) | 15 (11) | 0 (2) |
| polygon_offset | 14 (6) | 0 (8) | 0 (0) |
| depth_stencil_clear | 11 (1) | 0 (10) | 0 (0) |
| state_query, implementation_limits | 410 (410) | 2 (2) | 0 (0) |
| clip_control | -- (0) | -- (0) | 0 (5) |
| the rest (vertex_arrays, negative_api, ...) | 1136 (1136) | 0 (0) | 0 (0) |
| **all** | **16027 (8048)** | **937 (8705)** | **0 (216)** |

(clip_control is NotSupported now: its crashes were the depth texture
assert. Lines are drawn now -- `primitives.lines` and the like pass -- and
the wide line stage's interpolation is not dEQP's: the `interpolation`
line cases went from passing on nothing drawn to failing.) The last two
shader subgroups' crashes (atan's `copysign` built of integer bit
operations, which `nir_lower_int_to_float` asserts on) went with the
compiler options' `no_integers`, as GLES 2-only drivers have it.

Left: mipmaps (the twiddled copy is level 0 only) and cube maps, most of
the texture group; the interpolation along lines and wide points past the
viewport's edge (the draw module's stages); `shaders`' 364 -- arrays of
varyings and temporaries indexed, matrix arithmetic, texture lookups in
the random group's fragment shaders, `?:` on vectors, some division and
common functions; `uniform_api`'s 15.

New tests: `glchurn`, `glwrap`; `gldepth`'s `two_renders` cases and
`glstencil`'s are counted now. `SGX_DEBUG=state` prints each draw's state
words, `SGX_DEBUG=cmd` the TA command and the 3D block, `SGX_ZLS=0` keeps
depth and stencil in the tiles, `SGX_ZLS_CTL` sets ZLSCTL.

## M25: speed, measured with SuperTux

SuperTux 0.6.3 through the driver: its SDL renderer, SDL 3 (Alpine's SDL 2
is sdl2-compat) on GLES 2, on KMS (`tools/sgx/supertux-mesa.sh`). Two
things to see it at all: SuperTux links the system's libGL (through GLEW),
which brings the system's Mesa, whose libgallium then took our libEGL's
calls -- `sgx-gl` preloads ours with `SGX_PRELOAD=1`; and a `timeout` on
the script, not the program, left SuperTux holding DRM master.

Measured with `SGX_DEBUG=fps` -- every two seconds the frame rate, renders
and draws a frame, the process's CPU, the time waiting for fences, buffer
ioctls a frame, the time between a frame's end and the next one's first
draw, and where the renders were ended from; `sync` adds each render's
time on the GPU (waiting for it). `SGX_TRACE=1` prints a timeline (flushes,
kicks, waits, each render's end from a thread that waits for it), and
`SGX_TRACE_WAITS=1` who waited a millisecond or more.

It started at 6 frames a second. In order:

| | fps | why |
|---|---|---|
| start | 6.2 | 7 renders and 1040 draws a frame, 118 ms of GPU, 6000 buffer waits |
| 2048 draws a render | 4.8 | one render a frame -- GPU-bound, no faster |
| blending by SOP2 | 7.8 | the frame's GPU time 118 -> 41 ms |
| no waits for buffers | 8.8 | |
| native fence fds | 9.4 | |
| colour clears in the render | 13.8 | CPU and GPU in parallel at last |
| float attributes copied | 14.9 | |
| buffers in CPU memory | 15.7 | |

**A render took 200 draws** (the pack's EXT window: 256 KiB of 1 KiB draw
slots). The built frame's EXT window is 8 MiB now -- the pack's 4 MiB
layout, then 2048 draw slots and their vertex shaders' uniforms -- with a
VDM stream of 128 KiB (10 words a draw), and a render's buffer list takes
256 textures (the kernel takes 4096 buffers). One render a frame -- and no
faster: the GPU was the bound.

**Fill.** A benchmark of full-target quads (`build/probe/glfill`, out of
git) put the costs at 3.4 ms for a render of one opaque quad (the
background reload and the end of tile), 2 ms for each more opaque layer,
11 for a blended one, 19 for a blended textured one -- and 768 small quads
instead of one cost the GPU 1.4 ms more (the CPU 16 ms). Blending was
`nir_lower_blend`'s F32 arithmetic on the tile's colour unpacked: clamps,
constants loaded again and again, the colour packed at the end -- 50
instructions for SDL's textured blend against 19 without. iOS blends with
one SOP2 on bytes (M6, M8), and so does the driver now: the colour packed
to bytes (`pck.u8.f32`, which clamps), then SOP2 with o0 -- the selects
from Vita3K: colour `op(sel1 x src1, sel2 x src2)`, each select zero, either
source's colour or alpha, or the saturated alpha, a modifier taking 1 -
it; alpha likewise; add, subtract, min, max. Every GL blend function and
factor but the constant colour's maps onto it (reverse subtract by
swapping the sources); a colour mask, the constant and discard keep the
old way. `glblend` 14 of 14 (within a bit or two), dEQP's blend cases all
pass. A blended layer 11 -> 2.5 ms, a textured one 19 -> 6.7; SuperTux's
frame 118 -> 41 ms.

**Waits.** Every buffer map waited for the GPU (GEM_WAIT), and SDL maps
its vertex buffer for each draw: 6000 ioctls a frame. No render reads or
writes a buffer -- a draw's vertices, indices and uniforms are copied into
the frame's buffers -- so a buffer map does not wait, and buffers are
CPU memory (an SDL vertex buffer made anew each frame had cost an ioctl and
the cache cleaned over all of it).

**The flip.** With the waits gone a frame still spent 48 ms blocked between
its end and the next one's start, on `DRM_IOCTL_MODE_ATOMIC`: SDL's
atomic commit blocks until the plane's fence signals unless it can hand
the kernel an in-fence, which needs `EGL_ANDROID_native_fence_sync`. The
driver exports a render's syncobj as a sync file now (`fence_get_fd`),
imports one (`create_fence_fd`), and `fence_server_sync` makes the next
submit wait for it (the submit's in-syncs, a copy of the syncobj through a
sync file: the kernel's syncobjs are binary).

**Clears.** The timeline then showed the next frame starting only when the
last render ended: SDL clears each frame, and a colour clear was a render
of its own, whose start waits for the last render (the frame's buffers are
the last render's until it is done). A colour clear of all four channels
is a quad in the render now -- a program `or o0, sa0, #0` with the colour
packed in sa0 -- scissored ones too (they were the CPU's), after the
depth and stencil clears so a render's start stays free for those.

**The CPU.** Float vertex attributes (SDL's) are copied with 0, 0, 0, 1
filling in, not unpacked a vertex at a time through `util_format`; a
linear texture drawn twice no longer ends the render (it has no copy to
make again). `mesa_glthread` was slower (13.9).

That made 15.7 frames a second, the CPU the bound (63 ms a frame, the
GPU's 41 overlapping), a third of it SDL 3's: a hash table's lookups --
SDL 3 checks every object handed to it against a table of all of them,
and `SDL_INVALID_PARAM_CHECKS=1` (null checks only) takes that away: 19.7.
Then:

| | fps | why |
|---|---|---|
| SDL's light checks | 19.7 | |
| a release build of Mesa | 21.4 | `MESA_BUILD=release mesa/build.sh` (no assertions): 6 % |
| textures sampled raw | 20.0 (debug) | the frame's GPU time 41 -> 37 ms |
| no byte-wise memcmp | 22.0 (debug) | |

**Textures raw.** The sampler handed F32 channels back as 0..255, each
then scaled by 1/255. It hands the RGBA8 texel back as it is now (`smp`'s
raw format, one register) and two `pck.f32.u8 ... scale` make the four
channels 0..1: a blended textured layer 6.7 -> 5.2 ms.

**The CPU, again.** A draw whose state words would be the last draw's --
the same program, uniforms, textures (their state is in the secondary
attributes), layout and ISP state -- points at the last one's state slot
instead of writing its own, and a vertex shader's uniforms likewise; the
draw module's buffers are mapped only when a draw goes that way; only the
uniform words a program reads are cleared. And musl's `memcmp` goes a byte
at a time: the comparisons on the draw path (variant keys, those above)
are word by word, which alone took the debug build 20 -> 22.

**Draws merged.** Counting what the renders' draws had in common showed it:
of SuperTux's ~1050 draws a frame, ~990 have the state of the draw before
them -- the same program, texture, uniforms -- and draw one tile each (6
or 7 indices): SDL issues a tile a draw. Such a draw's vertices and
triangles are added to the last draw now instead of being a draw of their
own: ~1050 draws a frame become ~60 in the render, 22 -> 24 frames a
second. The GPU hardly notices (36 ms; objects were cheap for it); the
cost of a draw is before the driver.

At 24 frames a second (debug build) the CPU is still the bound: SDL 3 a
quarter, musl a quarter (memcpy, memcmp and string functions, much of it
Mesa's and SDL's), Mesa's state tracker and the driver a third. The old way
to the screen (M8's `libsgxsdl`, SDL's renderer replaced by the pack's
programs) ran SuperTux at 60 -- a draw there cost the CPU a few words, not
GL's state machine.

## M26: the vertex path, done properly

**What the PDS vertex fetch does with several attributes (2026-10-10).**
Its program: `fetch index` (`67800072`), then a `fetch attribute`
(`2f0091a3 | (4 n + 1) << 16`) a row, each row `{address, first pa << 8 |
words - 1, stride, 0}` -- iOS's always had one interleaved stream, the
stride in row 0 only. Tried with each attribute an array of its own
(`SGX_FETCH_TEST`, not kept):

- each attribute's **address is its own row's**: attributes in separate
  arrays with one stride draw right (`gltri`, `glcull` all right);
- the **stride is one for the whole fetch**, row 0's: a stride in another
  row is not taken (nor in the control word's top bits, 12..26 tried);
- `fetch index`'s low four bits name the data word the stride is in (2
  works, 6 takes row 1's word 2 instead, for every attribute); bits 6:4
  do not matter (`52`, `62`, `72` alike); its bits 23:16 and 11:8 do;
- a second `fetch index` before an attribute's fetch -- low byte 0..255,
  bits 23:16 0..255, each tried -- changes nothing, and nor do the `fetch
  attribute`'s bits 15:8 with one writing elsewhere (many of those hang).

So a vertex's attributes are read from `address_n + index x stride`, one
stride: a draw whose attributes share a stride (one interleaved buffer,
most engines') can be fetched from the application's buffers as they are;
any other is made one stream first.

**The vertex path (2026-10-10).** Until now every draw's vertices were
unpacked by the CPU into F32 vec4s, copied into the batch and again into
the frame, and the fetch -- one program a vertex shader -- read them from
there. Now:

- **Buffers have a GPU copy.** A buffer's CPU copy (M25) stays what maps
  see; its GPU copy, a buffer object, is made at the first draw that reads
  the buffer from there and brought up to date at each draw after
  (`sgx_buffer_bo`) with the bytes written since -- what unmaps say, or for
  a map with `FLUSH_EXPLICIT` (`u_upload_mgr`'s) what it flushes. It is
  written in place when no render reads it, gathered (the batch's number
  stamped on the buffer object) or running (`GEM_WAIT` with no timeout),
  or when only unsynchronized maps wrote it; else a new one is taken and
  filled -- the gathered draws hold a reference to the old one, the kernel
  one for the running render. Persistent coherent maps are off now (the
  default cap said yes): nothing would tell of a write through one. The
  buffer objects come from a cache of the ones dropped (size classes an
  eighth of a power of two apart, the oldest idle one first, 32 MiB and
  two seconds at most): a buffer made anew each frame no longer costs an
  ioctl and its pages cleared.
- **The vertex shader takes attributes as they are in memory.** A vertex
  shader is compiled for the formats of its attributes too (`struct
  sgx_vs`'s `attr[]`): `F32` with one to four components, the fetch reading
  that many words; unsigned normalized bytes, one word, made floats by
  `pck.f32.u8 ... scale` -- the top two channels first, then the bottom
  two over the word itself; the components the format lacks set to 0, 0, 1
  by `limm`. An attribute with a stride of 0 (a constant, GL's current
  value) is not fetched at all: its four floats go in sa after the
  uniforms, the shader reading them there. Other formats are made F32 by
  the CPU.
- **A fetch a draw.** Each draw's fetch program goes in a slot of its own
  in the frame (384 bytes, after the vertex shaders' uniforms): a row for
  each attribute fetched, its address for the draw's vertex 0, the stride
  in row 0. When every attribute fetched is in a buffer, in a format the
  fetch takes, 4-byte aligned, and they share a stride, the addresses are
  the buffers' GPU copies' (`glvtx`: SDL's floats at stride 24, byte
  colours at stride 12, attributes in two buffers): nothing is copied.
  Otherwise the CPU makes the draw's vertices one stream (the formats as
  they are, the rest F32) into the render's.
- **Triangles.** A list of up to 8190 vertices is drawn from the frame's 0,
  1, 2, ... buffer; anything else -- indices, strips, fans -- as 16-bit
  indices from the draw's vertex 0. A draw whose vertices follow on from
  the last draw's, with the same state, is merged into it as before (M25):
  the addresses the same number of vertices on for every attribute.
- **Client arrays as they are** (`user_vertex_buffers`). SDL 3's GLES 2
  renderer draws from client memory, not buffers: `u_vbuf` copied each
  array of each draw into its upload buffer, a draw's two arrays one after
  the other, so no draw's vertices followed on from the last one's and
  nothing merged (17.6 frames a second). The driver takes them now and
  makes them one stream itself: one copy fewer, and SDL's tiles merge
  again.

A `getenv` on the draw path (the switch `SGX_REPACK=1`, which makes every
draw's vertices one stream) cost 4 % of SuperTux's CPU: musl's `getenv`
`strncmp`s its way through the environment. `glvtx` (12 cases: the
layouts above, a constant colour, a buffer written again between two draws
of a render, indices from a buffer at an odd offset and from memory, a
strip from vertex 4, RGB bytes, short colours, 64 quads a draw each) is
right, and so is everything else: the `gl*` tests, dEQP's `vertex_arrays`
(571), `buffer` (49), `attribute_location` (57) and `draw` (100 of 101, as
before). SuperTux, the same scene, debug builds: **20.0 -> 24.5 frames a
second**.

## M27: renders that do not wait for each other

Every render was written into the same memory -- the template frame's
EXT window (its draws' state, uniforms, fetches, vertices, indices), its
VDM stream, the 3D pass's PDS block, the stream's end, the render target
set's 3D block -- so each one waited for the last to be done before it
could start being written (`begin_render`). The kernel runs one render at
a time (the scheduler's credit limit is 1), so what the GPU itself fills
in, the render target data, can be shared; what the CPU writes for a
render cannot.

- **A render's own memory.** Everything the CPU writes for one gathered
  render goes in memory of its own (`struct arena`): chunks of 256 KiB (or
  as large as one piece needs) from the device's cache of buffer objects
  (M26), listed with the render for the kernel and given back after the
  submit -- the cache hands a chunk out again once the render is done. Each piece is allocated as large as it is: no slots of
  fixed size, no 2048-draw, 1.25 MiB-vertex or 256 Ki-index windows; a
  batch is bounded by 8 MiB of vertices and indices
  (`SGX_FRAME_MAX_BYTES`) and 2048 draws. The draw module's draws get a
  fetch of their own, at their vertices, as vertex shaders' do (M26).
- **Its own copies of the frame's words a render sets**: the 3D pass's PDS
  block (the end of tile's DOUTU at `+8`, the background's descriptor at
  `+0x130`), the stream's terminate program (the tiles at `+0x10`) and the
  render target set's 3D block (the depth the tiles start at, the ZLS
  words) in the render's memory; the TA command's background program
  (`+0x08`, `+0x10`), stream (`+0xd4`) and 3D block (`+0x50`), the 3D
  block's event and pixel PDS programs (`+0x98`, `+0xfc`) and the stream's
  tail pointed at them.
- **A draw's state is 21 words.** The renders hung at first, the BIF
  faulting at `0x87800040` and the like (the TA's base, `0x87800000`, plus
  a little), and the hangs stayed with every copy above turned off, with
  every piece aligned to 4 KiB, with the render's memory one buffer of its
  own: the state program's DMA control word says 21 words (it is words less
  one, 20, as the uniform loaders' are), and the template's block is 20
  long (`0x50` bytes). The 21st -- word 20, which varyings the pixels get as
  F16 -- came from whatever followed the block: zeros in the EXT window,
  which nothing else wrote there, but the last render's leftovers in memory
  used again. All 21 are written now.
- **What is shared waits for what used it**: an end-of-tile program's slot
  is written over once the last render that ran it is done; a render
  target set given up stays with its renders (the kernel's reference).
- **At most three renders queued**: the fourth waits for the first, so the
  CPU is ahead of the GPU by that much and no more.
- **Is a buffer object idle?** `GEM_WAIT` with a zero timeout, which the
  buffer cache and the buffers' GPU copies (M26) asked, waits a jiffy for a
  busy one (`dma_resv_wait_timeout` turns 0 into 1): a cached buffer object
  keeps the fence of the last render that listed it now
  (`sgx_bo_set_busy`), and a syncobj's wait with no timeout returns at
  once.

The template's own draws (`sgx_frame_draw`, `sgx_frame_clear`: the pack's
sides, `SGX_FRAME=pack*`) still write the frame's buffers after waiting
for the last render, and so do gathered renders with `SGX_FRAME=fb` or
`packrt` (the pack's end of tile and render target data).

`glspeed DRAWS FRAMES flush` ends each frame with a `glFlush` (the last
with a `glFinish`), two targets in turn, so the CPU goes on while the GPU
draws: 400 draws a frame 6.75 -> 5.80 ms, 100 a frame 3.98 -> 3.69 ms
(with `glFinish` a frame, 9.1 and 4.7 ms either way). SuperTux, one render
a frame and the CPU the bound, is where it was (24). Every `gl*` test is
right, and dEQP-GLES2 as before but for one case,
`shaders.matrix.add_assign.dynamic_mediump_mat3_fragment`, which fails run
alone with the build before as well (it passes or not with what ran
before it).

## M28: mipmaps and cube maps

The sampled copy was level 0 only, and cube maps were not sampled at all:
most of dEQP's texture group (534 failures).

- **Levels.** iOS's layout was in the mipmap transfers captured in M7
  (p105-gpu.md): a 64x64's levels at `+0, +0x4000, +0x5000, +0x5400, ...`
  -- one after another, each twiddled at its own size, nothing between
  them. The copy is that now, every level the texture has. Word 0's bits
  20:17 are the last level the sampler goes down to: the levels less one
  with a mip filter, 0 without (GL's `NEAREST` and `LINEAR` take level 0
  whatever the size; iOS's one-level cube map has 0, its 2D textures 15).
  Bit 9 is the linear mip filter, a level and the next mixed -- found by
  flipping bits under `glmip` (half red, half green at level 0.5; without
  it the nearest level). Bit 26 moves the level by four or so: a bias,
  not needed.
- **Cube maps.** iOS's `textureCube` (the corpus's `t04_cube`) is `smp`
  with dimension 2 (`3d`, bits 43:42; `2d` is 1), the direction as three
  coordinates; its state's word 1 has bit 30 set (`0x4c020002` for a 4x4)
  and word 0 bits 20:17 clear. The compiler takes `samplerCube` now, the
  direction in three registers in a row. The faces are in GL's order, each
  a whole chain of levels down to 1x1 -- whatever levels the texture has
  -- and from 16x16 up rounded to 2 KiB: found with `SGX_TEX_PROBE=1`,
  which fills a cube map's copy with each texel's own index, and `glmip
  --probe N...`, which reads back the least index a quad over each face at
  each level brings: faces 21 texels apart at 4x4, 85 at 8x8, 512 at
  16x16 (a chain of 341), 1536 at 32x32, 5632 at 64x64, 22016 at 128x128,
  87552 at 256x256. A copy laid out otherwise was read past its end: the
  BIF faulted, the render hung.
- **`glGenerateMipmap`** is the CPU's (`generate_mipmap`: each level a 2x2
  box of the one above, through `util_format`'s floats): `util_gen_mipmap`
  blits, and the driver's blits did not scale. Blits that convert, scale or
  flip are the CPU's now too, nearest (`glCopyTexImage2D` from a B8G8R8A8
  target into an RGB texture is one: dEQP's `copyteximage2d` and
  `copytexsubimage2d` cases had nothing copied).

- **The same words as the PS Vita's.** The Vita's GPU is an SGX543 too,
  and Vita3K's `SceGxmTexture` (`vita3k/gxm/include/gxm/types.h`) is these
  four words: word 0 the t and s wrap (5:3, 8:6), the mip filter (9), the
  minification and magnification filters (11:10, 13:12), the levels less
  one (20:17), the LOD bias (26:21, 31 none); word 1 the size (log2 w in
  19:16 and log2 h in 3:0, or w - 1 in 23:12 and h - 1 in 11:0), the
  format (28:24) and the type (31:29: 0 twiddled, 2 a cube map, 3 linear,
  4 tiled, 5 twiddled of any size, 6 strided -- what the M16 linear
  textures are --, 7 a cube map of any size); word 2 the address. Its
  texture cache lays a cube map's faces out with the rules found above: a
  whole chain each, 2 KiB-aligned for 32-bit texels from 16x16 up.
- **Sizes not a power of two.** A 2D texture is twiddled too now, type 5:
  the copy padded to powers of two (its memory's layout), the size in word
  1 as it is -- so the minification filter works, which the linear way
  (M16) lost: only a texture the GPU renders into or that is shared is
  sampled linear still. Type 7 does not do here what Vita3K says (the
  sampler read past the copy, the renders hung): a cube map's faces are
  scaled up to a power of two by the CPU instead, the nearest texel.

`tools/sgx/gl/glmip.c`: a 64x64 with a colour a level drawn at each level's
size, with biases of 1 and 2, with no mip filter, with the linear one; a
4x4 cube map, each face's texels told apart by the direction through
them; a 32x32 cube map with every level, each face at each level; a 24x12
read texel by texel, a 48x48 minified with the linear filter, a 6x6 cube
map. 15 of 15. dEQP-GLES2's texture group **311 -> 783** passing (534 ->
62 failing); `uniform_api` (15 -> 4 failing) and `shaders` (363 -> 312)
gain the cube maps' cases; nothing else changes. All of dEQP-GLES2:
**16 562 passing, 402 failing** (16 027 and 937 after M24). What is left:

- `mipmap.2d.basic` and `.projected` (36): a few rows of a cell take the
  level next to GL's, where the level of detail is near a level's
  boundary. dEQP allows 8 bits of LOD for 2D (6 for cube maps, which
  pass): the sampler's own LOD is coarser than that.
- cube maps not a power of two with the linear filter, and `size.cube`
  at 15x15 (20): the scaled faces.

## M29: rendering into any level and face

A render's target was a resource's level 0: a draw into a cube map's face
(`glFramebufferTexture2D` with `GL_TEXTURE_CUBE_MAP_*`) or into another
level (`OES_fbo_render_mipmap`) was dropped. The frame takes a target
(`struct sgx_frame_target`: the resource, a level and a layer) now -- the
end of tile and the background aimed at the level's and layer's pixels,
the render target data for the level's size -- and the gathered draws end
when the framebuffer's level or face changes. The pixel back end takes a
64-byte-aligned address (a level's offset in its texture): `glmip` draws
into each face of a 16x16 cube map and into levels 1 and 3 of a 32x32
and samples them back, 17 of 17. dEQP unchanged (its GLES 2 cases render
into level 0 only).

## M30: three varyings and more

dEQP's `shaders.matrix` cases with a `mat3` or `mat4` varying failed
(`dynamic_*_mat3_fragment` and the like: the linker packs their floats into
three vec4 varyings or more). `tools/sgx/gl/glvary.c` showed the shape of
it: with one or two vec4 varyings every pixel right, with three a third of
them wrong, with four most -- and, drawn as a picture, not pixels at
random but whole triangles, right in some tiles and wrong in the next
(the edges the triangles' and the 32x32 tiles'). So not the pixel side,
though everything there was tried first (more registers a pixel, more
temporaries in the DOUTU, the code aligned, the PDS data padded, NOPs,
the inputs iterated as F16): the vertex side.

- **The vertices a vertex task takes.** The VDM word with the vertex
  fetch's address has a tag in bits 31:28 that M13b left open (the pack's
  9 kept). Every draw in iOS's captures has it as floor(128 / w) - 1 for
  vertices of w words to the tiler: 15 for 8 (the position and a vec4:
  v07 and most), 9 for 12 (v04, the pack), 2 for 36 (v05's eight
  varyings) -- as many vertices as their outputs fit in 128 words of the
  output buffer, less one. Ours was 9 whatever the vertex: ten vertices of
  16 words (three varyings) are 160, and the last ones' outputs overlapped
  the next task's. Swept on the device: three varyings right with 7 and
  less and wrong with 9, four right with 5 and less and wrong with 7 --
  the formula's 7 and 5. `vdm_fetch_tag()` works it out now
  (`SGX_FETCH_TAG=n` overrides it).
- **State word 5's bits 26:23** are 12 / fours (M13b, from iOS's 2, 3
  and 4 fours) -- 0 past 12 fours, and a program of more than 48
  registers then read 0 for every input (`glvary`'s eight varyings: 32
  primary attributes and 28 temporaries, all black). 1 there is right (2
  as well); `pixel_word5()`.
- **A vertex shader's outputs early.** GLSL's compiler stores them all at
  the shader's end, and every value for them was alive until then: seven
  or eight vec4 varyings took more than the 64 temporaries there are (the
  shader ran on the CPU). `store_outputs_early()` moves each store (and the
  vec4 it is made of) up to just after the values it reads -- after the
  stores before it to the same output, and not at all in a shader that
  reads an output back -- and a varying goes into its output register
  where it is stored (the position still at the end, its x and y moved
  into the TA's clip rectangle there). `nir_opt_move` does not do it: it
  moves an ALU instruction only when it has one source not a constant.

Not kept: iterating mediump varyings as F16 as iOS does (control word bits
29:28 2, DOUT size `0x12`, a vec4 in two registers, unpacked by `VPCK`
with source format 5). An F16 input after an F32 one came back right, an
F32 one after an F16 one did not (its z and w its x and y): the inputs stay
F32.

`glvary`: 1 to 8 vec4 varyings, each component its own linear function of
the position, read back one at a time (the rest added times a uniform 0,
so that the linker keeps them) and averaged; a `mat3` attribute and a float
through varyings (dEQP's shape) -- every pixel of a 4 x 4 grid of quads
checked, 30 of 30 (25 wrong with `SGX_FETCH_TAG=9`).

dEQP-GLES2's shaders group **312 -> 68** failing: all 126 of `matrix`, 58
of `operator`, 49 of `indexing` (varying and temporary arrays, matrix
subscripts), 8 of `loops`; nothing else changes. Renders that time out
during the whole run 46 -> 16 (all 16 of them among the 46). All of
dEQP-GLES2: **16 792 passing, 158 failing** (16 562 and 402 after M28),
the same twice. What is left of the shaders: an `ivec4` divided in a
fragment shader (21), `select_iteration_count` loops (18: the 9 fragment
ones time out -- the run's 16 timeouts are those, 6 of `random` and
`discard.function_static_loop_always`), a temporary array of constants read
through an index in a fragment shader (9).

## M31: ints, nested loops, a select on a uniform

Three compiler bugs from what M30 left of dEQP's shaders group, each found
by putting the failing case's shader into `glvary` and reading the program:

- **`int()` of a whole number that comes a little short.** dEQP's
  `div.*ivec4_fragment` cases divide `ivec4(v_in0)` by `ivec4(v_in1)`, 24
  at every vertex by -1 .. -4. The quotients came one nearer 0 everywhere:
  `ivec4(v_in0)` was 23 -- the varying, 24 at every vertex, comes into a
  pixel 23.9995 in dEQP's 128 x 112 viewport (2e-5 of itself: the TSP's
  plane is not exact unless the triangle's size is a power of two -- in
  128 x 128 it was 24 to the last bit). No bit of the iterate's control
  word changes it (bit 10 hangs). An int division, too, is
  `trunc(x * rcp(y))`, and 24 * rcp(-3) is -7.9999995. Ints are floats
  here (M13c), so `int(x)` -- `ftrunc`, `lower_ftrunc()` -- makes |x| larger
  by 2^-13 of itself (1/16 at most) first: a whole number a bit short is
  that number, a value further than that below one stays below it, and a
  whole number never goes to the next (the 1/16).
  `lower_ftrunc` is off in NIR's options now: `vec4(ivec4(v))` became an
  `ftrunc` that NIR lowered itself, before ours saw it.
- **Nested loops.** The numbering pass recorded a loop's span when it
  ended, the other passes took the k-th loop by the order loops start: an
  outer loop got its inner loop's span, and a value made before the outer
  loop and read in it lived only to the inner loop's end -- `i++` at the
  outer loop's end took its register (dEQP's `loops.*.nested*_vertex`,
  which passed by luck before: one instruction more anywhere and they
  failed). The span is recorded when the loop starts now.
- **A select on a uniform.** `VMOV`'s conditional move with its test in
  sa read 0 (`u_t ? 0.2 : 0.6` drew 0, alpha too): `select_iteration_count`
  loops ran `ub_true ? 3 : 0` times -- 0 in a vertex shader, and in a
  fragment shader long enough that the render timed out. iOS tests a
  uniform with `VTST` and predicates (the corpus's `c01`, `c05`); ours
  moves the test into a temporary first.

`glvary` 39 of 39: whole numbers through a varying as floats and as ints,
in 128 x 128 and 128 x 112, 24 divided by ints from uniforms and from a
varying, dEQP's division shader as it is, a select on a uniform bool.

dEQP-GLES2's shaders group **68 -> 21** failing (the 21 int divisions, 18
`select_iteration_count` loops, `nested_sequence`, `side_effects.affect_*`,
two nested struct arrays in loops, `linkage.varying_7`, a matrix from
ivec4s); timeouts 16 -> 7. All of dEQP-GLES2: **16 842 passing, 108
failing** (16 792 and 158 after M30), nothing worse. Left of the shaders:
temporary arrays of 40 constants read through an index (8, and a written
one), 6 of `random`, `scoping.valid.local_variable_hides_function_parameter`
(GLSL's compiler, 2), `pointcoord`, `discard.dynamic_loop_texture`,
`texture2dproj_vec4_bias`, a `mat4` written and read through indices.

## M32: uniforms in memory -- the USSE's loads

A program's uniforms were its secondary attributes and nothing else: 128
words at most, the textures' state words and the blend colour among them.
GLES 2 wants 128 vec4s (512 words) for a vertex shader, and GLSL's linker
makes a constant array uniforms (dEQP's `tmp_array.*const_write*`: 40
vec4s, read through an index -- "uniforms past word 128", drawn grey).

**VLDST.** The PS Vita's GXM reads its uniform buffers with VLDST (Vita3K's
`vldst`: `111 oo ppp s n m y c r b a kkkk dd ee t g ff i h j l qq uu` and
four 7-bit register numbers), and iOS's end-of-tile program has one --
`eot_program()`'s `usse_dummy_load`, `0xe9a30084a0000000`: on p0, a load
into pa0 from the address in pa0, then `WDF0`. Found with `glldr --probe` (`SGX_LDR_PROBE=word,...`
makes the fragment program those words, `WDF0` and r0 into o0 as it is;
the 4 x 4 texture's state words are in sa0..3, the address in sa2, texel
i the bytes i, 0x40 + i, 0x80 + i, 0xc0 + i, so a pixel's bytes are the
word loaded):

- op 1 is a load, src0 the base (sa: extended bank 1), src1 an offset --
  an immediate counts words; a register is its high half times its low
  half, bytes (`0x10004`: 4; `0x20004`: 8; `0x1003c`: 60; `0x40001`: 4;
  `4` alone: 0). src2 is not added to a load.
- Bits 47:44 are the words less one: four in a row into r0..r3 -- but only
  with MOE expansion (bit 53) off, and then 4 bytes more are added (as
  Vita3K says); with it on, more than one word loaded nothing.
- `WDF0` waits for it, as for a texture sample.
- The register offset is made as M21's index: 4 x the offset + the base +
  2^23 as a float, `and` 0xffff, then `or` 4 rotated left by 16 (VBW's
  src2 rotation, bits 42:38: its immediate is 16 bits).

**The driver.** When a program's uniform words with the rest of its sa do
not fit in 128, the first ones stay in sa, and all of them go into the
render's memory too, after the sa words the loader DMAs (`arena_uniforms`):
the block's address less 4 in an sa word (`ubuf_sa`). A word past sa's is
read by `VLDST` (`memory_uniform()`): an immediate up to word 127, a
register (LIMM'd) past it, the index made as above for an array read
through an index whose end is past sa's. The draws' sa words go on with
the block's (`prog.nubuf`, `vs->nubuf`), so gathered draws compare them as
before. The vertex shader's constant buffer is 137 vec4s: the state
tracker keeps 9 of them for clip planes and the point size, and with 128
GL_MAX_VERTEX_UNIFORM_VECTORS was 119 (dEQP's `implementation_limits`).

`tools/sgx/gl/glldr.c`: a fragment shader's 40 vec4s through a uniform
index (elements 0, 13, 26, 39) and through one from a varying (each column
of pixels its own element), straight in sa and past it; 40 constants
through an index; a vertex shader's 120 vec4s through an index and
straight (element 113); an element changed between two draws of a render
and over three renders (the block is the render's own memory; nothing
stale came back) -- 15 of 15. dEQP-GLES2: the 8 `tmp_array.*const_write*`
cases still failing pass, and `max_vertex_uniform_vectors` (both),
`uniform_api.random.74`: **16 852 passing, 98 failing** (16 842 and 108
after M31), nothing worse.

## M33: texture lookups in programs that branch; derivatives

dEQP's `discard.dynamic_loop_texture` hung the GPU (and, after it,
`function_static_loop_always` was the case the timeout landed on): a
texture lookup in a loop of a uniform count. `glldr --probe` with
`GLLDR_FS` (the probe's fragment shader replaced) took it apart: a lookup
in a loop of a uniform count hung, before a loop too (4 of 4), after one
now and then; in an if on a uniform it did not; in an if/else on a varying
it did. With an explicit level of detail (`SMP`'s LOD mode) none hung and
all came out right -- so the level of detail from the 2x2 block. And
`dFdx` (new: `DSX`, `DSY`, OES_standard_derivatives) came out wrong in a
quarter of the pixels in a loop: the block's pixels were not there
together.

**PHAS's mode.** M20 made a program that branches PHAS mode 1 (bit 45,
iOS's `c04_loop_break`) with the DOUTU data word's bit 0 -- its pixels each
on its own (Vita3K's PHAS fields: `mode`, then `rate_hi`, `rate_lo`).
Mode 0 runs them together, and a branch then goes for all of them: BR's
bit 20 (Vita3K's `all_inst`; `any_inst`, bit 21, the same here) taken
when all the pixels take it. With that every case above came out right,
no hang, the lookups with their own level of detail. SMP's `syncstart`
(bit 52) did not help: everything hung with it.

So the compiler runs NIR's divergence analysis on a fragment shader with
control flow: when every if's test (a loop's breaks among them) is the
same for all pixels -- loops of a uniform or constant count, ifs on
uniforms -- the program is mode 0, its branches `all_inst`, the DOUTU bit
clear. Otherwise it stays mode 1, and its lookups take level 0 (exact for
a texture without levels whose filters agree; GLSL leaves derivatives
undefined where pixels part) -- with a warning; one with a bias the level
the bias names (dEQP's `random.all_features.fragment.34`: a biased lookup
in an if on a varying hung, and the timeout landed on the next case).
NIR's LICM moves what a loop does not change out of it first.

A program with `dFdx`/`dFdy` clears skipinv (bit 55) on all but its
output's writes, as one that samples does (M14): along the edge between
the quad's two triangles the 2x2 blocks' pixels on the far side skipped
the instructions, and DSY read what was in their registers.

`tools/sgx/gl/glflow.c`: a lookup in an if on a uniform, in a loop of a
uniform count (at fixed coordinates and at the loop's), before and after
one, in nested loops, in an if/else on a varying; `dFdx`, `dFdy`, and in a
loop; a 128 x 128 mipmap's level 1 at 64 x 64, in a loop -- 10 of 10.
dEQP-GLES2: `discard.dynamic_loop_texture` and six of `random` pass, and
the whole run has no render that timed out (7 after M32): **16 859
passing, 91 failing** (16 852 and 98), nothing worse.

## M34: speed -- where SuperTux's frame goes, the pixel programs shorter

SuperTux (SDL 3's GLES 2 renderer, 1 100 draws a frame) ran 24.5 fps,
100 % of a core. `perf` on the device (no call graphs: the libraries have
no frame pointers, and DWARF unwinding lost the maps; binutils and Alpine's
`sdl3-dbg` installed from packages fetched in docker, `perf annotate
--no-source` with `addr2line` run on the Mac's copy of the library) split
the CPU's time: SDL 3 30 % (`SetDrawState`, its command queue, the hash
tables and properties it reads every draw), musl 26 % (malloc, memcmp,
strlen -- mostly SDL's), libgallium 29 % (the driver about 13 %, Mesa's GL
side the rest), SuperTux 6 %. And `SGX_DEBUG=fps,sync` gave the GPU's:
**35.5 ms a frame** -- the CPU's and the GPU's both at the limit, the
renders in flight (M27) overlapping them.

`glspeed` has SDL's way now (`sprites`: textured quads blended, 8 textures
in turn, client arrays; `fill`: each the whole target; `GLSPEED_FS`,
`_BLEND`, `_TEX`, `_VBO`). A full-target layer cost 1.9 ms for a program
of 6 instructions, 4.4 ms for 12, 5.8 ms for SDL's 16 (texture times
colour): about 2 G instructions a second, what two cores of four USSE
pipelines at 250 MHz do -- the pixels cost their instructions, the texture
less than its moves. iOS's `t06` (a texel times a uniform) is 3: PHAS,
`mul.f16 pa0.xyzw`, `pck.u8.f16 o0` -- the PDS fetched the texel into pa0
(M14's non-dependent read) and the mediump arithmetic is F16, four
channels an instruction. Ours, `texture2D(t, uv) * c`: two moves of the
coordinates into a pair, SMP, WDF, two unpacks, four F32 muls, four moves
of the results into the colour's registers, the pack.

- **The colour made where it goes.** The four registers the pack reads are
  taken when the first value of the colour is made, and an ALU result that
  is a channel of it is made there (`colour_id`, `out_base`). Only
  V32NMAD's (add, mul, min, max): the compares', selects' and RCP's
  encodings take an even destination, and two of the four are odd (glfs
  and glcull caught it). Taken at the start, the four registers were held
  through the whole program: 67 a pixel for dEQP's
  `tmp_array.vec3_*_dynamic_read`.
- **The coordinates where they are**: a varying's x and y are a pair of
  primary attributes from an even one -- SMP reads them there.

SDL's program 16 -> 10 instructions, a layer 5.8 -> 3.6 ms; SuperTux's
GPU time 35.5 -> 25.5 ms a frame, and the CPU the limit again (26 fps).

**glthread** (`mesa_glthread=true`) would put Mesa and the driver on the
second core: it needs `map_unsynchronized_thread_safe` and
`allow_mapped_buffers_during_execution`. The driver has them now: a map
glthread makes in the application's thread takes nothing of the context's
(its transfer `CALLOC`'d, not the context's slab), a buffer's dirty bytes
are under a lock, and a buffer mapped while draws read it (glthread's 1 MiB
upload buffer, written on and on) has each draw take what it reads of the
CPU's copy (`live_maps`; `sgx_buffer_bo()` takes the range). SuperTux does
not gain: it makes textures every frame (its text), and glthread waits for
the other thread at every `glGenTextures`, `glTexImage2D` and
`glTexSubImage2D` from the application's memory -- 33 a frame (counted
with a print in glthread's `_mesa_glthread_finish_before`). Mesa leaves it
off on fewer than four cores anyway.

The CPU's stream of vertices from an application's array laid out as the
stream is (SDL's x y, r g b a, u v at stride 32) is one copy now, not a
memset and a memcpy an attribute a vertex.

**Captures for what is next** (iOS, `tools/shadercap.sh`): gltrace's
corpus mode takes a case's way of drawing from its file now -- `// mode:`
(points, lines, strips, fans), `// width:`, `// blend:`, `// depth:` (the
target has a depth buffer), `// tex2d: W H [mip] [linear]`, `// cube: N
[linear]`, `// draws: N` (another texture every other draw), `//
indexed`, `// pages: all` (every page that is not zero, not only those that
changed: t09's pixel PDS never showed in the changed ones). The new cases:
`s00`..`s11` the PDS's texture fetch (SDL's own shader, highp and
mediump coordinates, the colour varying first, two textures, `.zw`, a
bias, a cube map, `texture2DProj`, lowp, three draws with the texture
another each), `h00`..`h04` opaque, depth-tested, blended and punch-through
draws (the ISP's pass types: hidden surface removal), `l00`..`l10` lines,
strips, loops, wide lines, points (`gl_PointSize`, `gl_PointCoord`, a size
an attribute), triangle strips and fans, indexed lines and triangles,
`k00`..`k03` cube maps of 15 and 6, a 64 x 64 with every level, a 24 x 12.

Release builds: `glspeed 1000 60 sprites` 49.5 -> 36.8 ms a frame (each
waits for the GPU: both sides), 8 blended layers 53 -> 34.8 ms; SuperTux
24.5 -> 26 fps, the GPU 24.7 ms of its frame. dEQP-GLES2 the same,
16 859 / 91, no render timed out.

## M35: texture lookups by the PDS; the system's OpenGL

**The PDS's lookups.** iOS draws SDL's textured sprite with three
instructions (`mul.f16 pa0, pa2, pa0; pck.u8.f16; sop2`): the texel is
in the primary attributes before the program starts, fetched by the PDS
program, not by an SMP in it. The 2026-10-10 corpus (`s00`..`s11`, and
`t05`, `t09` of the older one) shows how:

- An iterate DOUT's control word has two halves. The upper one is what
  M11 found, an iterate into the registers: format in bits 29:28 (0 F32,
  1 U8, 2 F16), bit 25 the last such, bits 23:22 components - 1, bits
  15:12 the varying (15: none). The lower one sends a varying to the
  texture unit for the texture DOUT that follows: bits 3:0 the varying
  (15: none) whose x, y (and z) are the coordinates, bits 10:8 the kind
  (1 2D, 2 projective, 3 a cube map), bit 11 the last lookup. SDL's sprite
  is one control word, `2fc01900`: the colour as F16 into the registers
  and varying 0 to the texture unit.
- The texture DOUT (`07041004`: kind 004, its four state words from data
  word (17:10), (s + 2) >> 2 in 23:18) hands back the texel as it is, one
  register; kind 024 with bit 31 of state word 3 set, as F16, two.
- The iterated values go first, then the texel (`t05`: the coordinates
  in pa0, the texel in pa1). A bias is the state's (word 0's bias
  field: `03fe0090` -> `057e0090` for 1.5); a lookup at `.zw`, or at
  coordinates the program works out, is an SMP in the program.
- Data word 1 of a program with lookups has bit 3 set (iOS's `0x0a`;
  `0x0e` with iterates too, bit 2).

The compiler (`plan_inputs()`) now hands every 2D lookup at a varying's x
and y -- implicit level of detail, no bias, four at most -- to the PDS:
its texel is in pa(4 * inputs + k), unpacked as an SMP's was. A varying
only lookups read is not iterated into the registers at all (it stays in
the layout: `sgx_fs.varying_slot[]`). The PDS program is the draw's now
(`fetch_pds()`, in the render's memory): the texture's state words are in
its data, after the iterates' control words, iOS's order -- the iterates,
then for each lookup its control word and the texture DOUT. The TAG takes
the level of detail from the iterated coordinates of the 2x2 block, so a
lookup in a branch that pixels take apart needs no level 0 any more, and a
program whose only lookups are the PDS's keeps skipinv (iOS's t00).

One thing broke on the way: state word 5's registers a pixel takes, in
fours, counted the texels with the temporaries -- one texel and 27
temporaries made 28 registers, and the temporaries start at a four after
the primary attributes: the last ones ran into the next pixel's texel,
which read 0 (glflow's lookup in a loop, as soon as the loop's
temporaries reached r24). The primary attributes are rounded up to a four
now.

`tools/sgx/gl/glfetch.c`: one texture, times a colour, the coordinates
read besides, two, four, a fifth in the program, one texture twice,
beside a dependent lookup, at `.zw`, with `gl_FragCoord`, in an if/else
on a varying, in a loop, a mipmap's level -- 14 of 14, the same with
`SGX_NOFETCH=1` (in the program). SDL's program 10 -> 8 instructions;
SuperTux's GPU time 23.9 -> 21.1 ms a frame, `glspeed` textured layers
33.4 -> 29.0 ms (debug builds). dEQP-GLES2 the same 16 859 / 91, the same
cases, no render timed out. `corpus.py --pds` finds PDS programs by their
shape now (this corpus's code base is not 64 KiB-aligned, and the DOUTU
search found nothing) and names both halves of a control word.

**The system's OpenGL.** Mesa is built as a whole OpenGL now -- the X11
and Wayland platforms, GLX (`libGL`), GLES 1 and 2, EGL, GBM, softpipe (the
software rasteriser, for comparison: `LIBGL_ALWAYS_SOFTWARE=1
GALLIUM_DRIVER=softpipe`) and driconf -- and `mesa/install.py` makes it the
system's: `/etc/ld-musl-armhf.path` puts `/usr/local/lib/sgx-mesa/lib`
ahead of `/usr/lib`, so every program loads it, Alpine's Mesa left in
place (the file taken away, it is back). No `sgx-gl`, no preloading:
`eglinfo` says OpenGL 2.1 on the PowerVR SGX543MP2, and SuperTux's GLEW
finds the right libGL.

**OpenGL 2.1.** Mesa said 2.0 for want of EXT_texture_sRGB. sRGB textures
are kept as their bytes and decoded by the program after the lookup
(`nir_lower_tex`'s `lower_srgb`, a bit a unit in the variant key): no
gamma bit of the sampler's turned up (word 0's bits 0..2, 14..16 and
27..31 flipped, the sampler's output raw and F32), and iOS on the A5 has
no sRGB to capture. Decoded after filtering rather than before -- exact
for nearest sampling. 1D textures are 2D ones a texel high
(`lower_1d`), rectangle and 1D textures render targets like 2D ones.
`tools/sgx/gl/gldesk.c` (desktop GL through EGL): a 1D texture, sRGB RGBA
and luminance textures, decoded and not (EXT_texture_sRGB_decode), the
fixed-function pipeline from glBegin and from client arrays -- 6 of 6.

**piglit.** The OpenGL test suite: `tools/piglit/build.sh` builds it in
an Alpine armv7 image (`tools/piglit/Dockerfile`; 1831 programs),
`tools/piglit/run.sh` runs its `quick_gl` profile on the iPad without what
an OpenGL 2.1 part can never have (GLSL 1.30 on, GL 3 and 4 and their
extensions): 4691 tests, about 40 minutes. Through GBM (waffle's
surfaceless platform aborts). The first run stopped a sixth of the way: the
OOM killer took the runner while `streaming-texture-leak` made, drew with
and deleted 5000 textures of 4 MiB in one render -- all of them referenced
until it was made, in kernel pages no process is charged with. A render
that holds more than 48 MiB of textures goes first now
(`SGX_BATCH_TEX_BYTES`); the test passes alone in 25 minutes (each upload
converted and twiddled by the CPU) and `run.sh` leaves it out. The first
whole run (debug build): **722 pass, 233 fail, 8 crash**, 3727 skip -- M36
is what it found.

## M36: what piglit found first

Read case by case from the first run, debug and release builds side by
side; `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe` tells a test's
own mistake from ours (gldesk's fixed-function case drew unit 1's texture
from unit 0's coordinates on both). gdb is on the iPad now (Alpine's armhf
packages, as binutils were).

- **Depth and stencil for the CPU.** glReadPixels, glDrawPixels and
  glCopyPixels of depth or stencil, depth textures read back and blits of
  depth went through maps of the depth buffer, whose linear copy nothing
  ever wrote: depth lives in the ISP's tiles (M24). A map of level 0 now
  turns the stored tiles -- 32 x 32, F32 or 24-bit depth with stencil in
  the top byte, M24's layout -- into the resource's format
  (`zs_to_linear()`), or the clear a render has not done yet; an unmap that
  wrote turns it back into tiles for the next render to load
  (`linear_to_zs()`). A depth clear at the start of a render with no draws
  is handed to the buffer when it is mapped. The CPU's blit does depth and
  stencil too. One bug on the way: depth 1.0 to 24 bits in floats is
  16777215.5, which rounds to 2^24 -- a carry into the stencil byte (in
  doubles now).
- **Depth alone.** A framebuffer with no colour buffer -- a shadow map's
  -- was dropped ("draws only go into B8G8R8A8 targets"), and its clears
  dereferenced the missing colour buffer: the render's tiles now end in a
  colour target of the depth buffer's size that nothing reads
  (`ctx->no_colour`). A program that writes no colour is `NOP` with the end
  bit (o0 keeps the tile's colour), as a colour mask of nothing was.
- **Texture levels and LOD.** GL's base and maximum levels and its minimum
  and maximum LOD were ignored. The sampler's own are the copy's level 0
  and word 0's last level, so the copy starts at the base level -- or m
  levels further for a minimum LOD m: the sampler's LOD, from that level's
  size, is then GL's less m, max(lambda, m) in all -- and the last level is
  the maximum level's or LOD's (`sampled_levels()`; exact for whole
  numbers; a texture's copy is made again when the base moves). The
  sampler's LOD bias is word 0's bits 26:21, 31 + eighths (iOS's t03 and
  s06: 39 for 1.0, 43 for 1.5). levelclamp, lodclamp, mipmap-setup,
  texture-base-level pass.
- **Rectangle textures** were refused by the compiler. With
  `caps.texrect` off the state tracker scales their coordinates by
  `load_texture_scale` (`has_texture_scaling`) and leaves the sampler's
  dimension Rect: the compiler samples them as 2D, and two words a texture
  after the samplers' state words hold 1 / width, 1 / height (`scale_sa`,
  the draw's). (`nir_lower_tex`'s own `lower_rect` on top scaled them
  twice.) gldesk has a 6 x 3 one.
- **Values made where they are used.** piglit's varying-packing vertex
  shaders store 32 sums of a uniform: NIR made all 32 first and stored them
  after, and a value is a register pair here -- more than the temporaries
  hold, run on the CPU. `nir_opt_move` now puts constants, uniforms,
  inputs, comparisons and single-source ALU operations just before their
  first use. (`nir_schedule`'s least-pressure order made the fragment
  shader worse: 32 comparisons first, then the chain that reads them.) The
  tests still fail: they compare a varying that is the same at every vertex
  with `!=`, and the TSP's plane is not exact unless the triangle's size is
  a power of two (M31) -- flat interpolation would make it exact. The new
  order is not always the better one: dEQP's
  `tmp_array.vec3_dynamic_write_dynamic_read_fragment` came to 67 registers a
  pixel with it (64 at most) and was drawn the M13a way -- the one case the
  whole run (r37) had worse. A shader short of registers is compiled again
  in NIR's own order now (`keep_order`); r38's shaders, texture and
  fragment_ops groups are r36's again, that case passing.
- `SGX_DEBUG_NIR=1` prints the shader as the compiler takes it;
  `SGX_ITER_XOR=mask` flips bits of every iterate's control word.

The second whole run (release build, before the last three fixes): **774
pass, 187 fail, 1 crash**, 1 timeout -- 56 fixed, 4 broken (the carry
above), fixed since. A debug build crashes on Mesa's own assertions where a
release build goes on (`nir_lower_passthrough_edgeflags`, softpipe too;
`maxLevels != 0` for a 3D proxy texture): the system's build should be the
release one.

**X on the GPU.** With the driver's Mesa the system's OpenGL, Xorg's
modesetting driver takes glamor on it -- "glamor: Using OpenGL 2.1 context
... X acceleration enabled on PowerVR SGX543MP2" -- and XFCE comes up drawn
by the GPU, its programs' GLX direct (glxinfo: OpenGL 2.1, the SGX543MP2;
glxgears at the display's 60 Hz), AIGLX the same driver (the X server
looks for it in /usr/lib/dri: a link there). `mesa/install.py` puts the
configuration in /etc/X11/sgx.conf.d (modesetting, glamor) and `desktop`
(tools/desktop/xfce-setup.sh) starts X from it when it is there --
`DESKTOP_FB=1` for fbdev as before. xwd and xdotool are on the iPad for
screenshots.

**Tools that were the session's own** are the repository's now:
`mesa/devbuild.sh [release]` (a build beside the system's, in
`/root/devbuilds`, with `/root/sgx-dev` to run programs with it and
`/root/hangs.sh` to count timed-out renders since a point),
`tools/device-apk.sh PKG...` (Alpine's armhf packages onto the iPad without
its network: binutils, gdb, waffle, mako, xwd and xdotool went that way),
`tools/piglit/install.sh` and `pigsum.py`, `tools/desktop/xshot.py` (X
screenshots, as PNG). `./cascadia mesa` builds and installs the release
build now (`MESA_BUILD=debug` for assertions).

dEQP-GLES2 after all of it: 16 859 pass, 91 fail, as before M35.

**What the rest needs** -- most of it the hardware's, not found by flipping
bits:

- **Flat shading** (glShadeModel, provoking vertex, GLSL's `flat`):
  neither a single bit of state words 0..3 and 16..20 nor of the iterate's
  control word makes a triangle one vertex's colour (`tools/sgx/gl/glflat.c`
  probes it: two triangles, each vertex its own colour, `SGX_STATE`); bits
  17 and 18 of the control word make x and y 1.0, 28..30 the format, 25 the
  last. Also what would make the varying-packing cases exact.
- **Discard with depth** (punch-through, M19) and **gl_FragDepth**: iOS's
  program is known, the render's setting is not -- in the render command,
  which the corpus did not record.
- **Logic ops, the alpha test, user clip planes, point sprites, polygon
  offset, two-sided colour, occlusion queries.** OpenGL ES 1 has the first
  five (and ES 2's extensions the last ones): iOS's driver shows how.
- **Border colours** (GL_CLAMP_TO_BORDER), **3D textures**, **shadow
  compare** and depth texture modes, line stipple and smooth lines, the
  front buffer and swap behaviour on GBM, logic ops' xor cursors.

**For the next capture** (`tools/shadercap.sh`, the same command): gltrace
records each case's render commands now (header, the 55-word payload, the
records -- what the kext builds the 3D registers from, `rtemu.py`), lists
the extensions, and takes new directives -- `// texfmt:` (565, 4444, 5551,
RGB8, L8, A8, LA8, half and float, BGRA, sRGB, depth 16 and 32, depth and
stencil), `// compare`, `// maxlevel:`, `// aniso:`, `// wrap:`, `// rt:`
(565, 4444, 5551, RGBA8, half-float targets), `// query: occlusion`, `//
offset: F U`. The cases `m00`..`m31`: opaque, discarding with and without
depth, blended (their render commands side by side), depth textures,
`shadow2DEXT`, `gl_FragDepthEXT`, a texture of each format, a maximum
level, anisotropy, the wrap modes, a target of each format, an occlusion
query, polygon offset, `texture2DLodEXT`. And **`gltrace es1`**: OpenGL ES
1's fixed function on the same driver, 25 cases -- smooth and flat (with a
texture, and lines), every kind of logic op, the alpha test (with depth),
a clip plane, point sprites and point sizes, linear fog, smooth lines,
polygon offset, two-sided lighting, texture env ADD, plain points and
lines; shadercap runs it after the corpus, into `es1/`.

## M37: what iOS's fixed function showed; uploads that wait for nothing

**The capture** (2026-10-15, `tools/shadercap.sh`: the corpus's m00..m31
and the OpenGL ES 1 cases e00..e24, each case's render commands logged)
answered most of what M36 left, mostly as state words (`corpus.py --state`
side by side, the first block of each case the draw's own):

- The render commands of an opaque draw, a discarding one with and
  without depth, and a blended one (m00..m03) are the same but for
  addresses, and rtemu.py's 3D block for iOS's payload is ours but for
  them and the ZLS: **punch-through is not in the render's registers** --
  still not found (ISP state A bit 27 and iOS's two-phase program, M19).
- ISP state A (word 1): bits 16:15 the object -- 01 lines (e18, e23, e24),
  11 points (e16, e22), 10 point sprites (e15); bit 14 counts the samples a
  draw passes (m29, below), bit 18 only whether any did; bit 9 (set in
  every state) turns word 2's depth bias on.
- Word 2: the depth bias, bits 25:21 the slope's factor and 20:16 the
  units (below).
- Word 16 besides the vertex's size: bit 11 the TA's base colour, 10 its
  offset colour (e20, two-sided lighting's two), 9 fog (e17: a word,
  TSP varying 12), 8 a point size (e16, e22), bits 3:0 the user clip planes
  (e14: 09001801, a word more in the vertex).
- Word 18 bits 18:17 are 3 for flat shading (e01, e02, e24) -- and bit 26
  of the colour's iterate control word clear (0fc0a00f -> 0bc0a00f): below.
- Texture state words: the formats and swizzles of PS Vita's SceGxmTexture
  (m10..m20; word 1 bits 28:24, word 3 bits 30:28) -- U8 0x00, U4U4U4U4
  0x02, U1U5U5U5 0x04, U5U6U5 0x05, U8U8 0x07, U8U8U8U8 0x0c, F16x4 0x1b;
  565 swizzled 1RGB, 4444 and 5551 ARGB, L8 1RRR, A8 R000, LA8 GRRR, RGB8 as
  RGBX 1BGR; word 3 bit 31 hands the texel back as F16 (half floats, sRGB,
  which is word 0 bit 27, GXM's gamma mode). Depth textures are the depth
  buffer's own tiles, type 4 (tiled), F32M 0x13 (m04..m06).
- The pixel back end's formats (m25..m28): r0 of the end-of-tile program,
  0x34110000 565, 0x54110000 4444, 0x44110000 5551, 0x07010000 half floats
  (8888 is our 0x00110000); r2 the width in pixels / 2 - 1.
- Logic ops (e03..e10), the alpha test (e11..e13: punch-through and a
  discard), two-sided lighting (e20: `gl_FrontFacing`'s bit), shadow
  samplers (m07, m08: the reference clamped, the depth raw, `tex - ref >=
  0`) are the program's. iOS 8.4.1 on the A5 has no EXT_frag_depth (m09 did
  not compile).

**Logic ops** as iOS's: the colour packed to bytes, then one VBW with the
tile's -- `xor o0, pa0, o0`, NOT as `and o0, c56, ~x` (c56 is all ones).
The encoder has VBW between registers now (`usse_vbw`, iOS's eight words bit
for bit in usse-test); the variant key the op; `output()` the op, then a
colour mask on the bytes (o0 is B G R A) and a discard (the result and the
tile's unpacked, chosen, packed again). nir_lower_blend's logic ops are
integer operations, which the compiler has not. piglit's gl-1.0-logicop,
gl-1.1-xor and its copypixels case pass.

**Why XFCE dragged** -- a window moved at two or three steps a second on
glamor, when fbdev's CPU drawing kept up. The X server at 85-100 % CPU, and
its ioctls over one drag (120 steps): 3382 renders, 1644 buffers made and
freed, 7032 syncobj waits. perf: 80 % of the X server's time in the texture
copy (`sgx_resource_texture` 60 %, A8 to RGBA8 15 %, divisions in the
twiddle 6 %). glamor draws glyphs and masks from A8 textures and uploads
into them between draws; each such upload flushed the render (the draws
read the texture), waited for it, and the copy the sampler reads was made
again whole, RGBA8, after `sgx_frame_finish` waited for the GPU to go idle.
And at every step of a drag glamor makes new A8 textures of the window's
size and fills them once (`SGX_DEBUG=copies`). Now:

- A copy a render may still read -- gathered or running -- is not written
  over: a new buffer from the cache takes the new one, the old one stays
  with the render (the batch references each copy it samples, `tex_bo[]`,
  busy with the render once made). A map of a texture whose linear content
  no render reads or writes (sampled through its copy) neither flushes nor
  waits.
- A texture of one level written in boxes keeps a shadow of its copy in
  cached memory (`tw_cpu`): only the box is made again, and the range of
  the copy it falls in (Morton order is monotonic in x and in y) copied
  over; a small box of a big copy waits for its renders rather than copying
  all of it again. Morton order by tables, no divisions.
- The formats the sampler has are copied as they are, 1, 2 or 4 bytes a
  texel, by the words above (`native_format()`): RGBA8, BGRA8 (its
  swizzle, no swap in the program any more), RGBX, BGRX, 565, 4444, 5551,
  L8, A8, LA8. The sampler hands each texel back as an RGBA8 texel's four
  bytes (iOS's program for 565 is `or o0, pa0`), so programs are as they
  were. A cube map of one-byte texels is the exception (its faces are not
  where the 2 KiB rule puts them: dEQP's size.cube.16x16_l8), made RGBA8.
- A texture of one level in such a format is sampled linear -- no copy at
  all -- when its sampler takes the nearest texel both ways: nothing is
  lost then (the linear way loses the minification filter; minified alone,
  dEQP's nearest_linear_clamp_*_npot went wrong). The linear way takes the
  same formats and swizzles (GXM's linear strided type 6): the B G R A it
  "read by itself" (M16) was swizzle 1.

`tools/sgx/gl/glupload.c`: a texture written box by box, each version drawn
before the next box in one render (RGBA 128 x 64 and 64 x 128, two Morton
squares; ALPHA; 100 x 60, padded), each format the sampler reads as it is;
13 of 13 either way (`SGX_TEX_NATIVE=0`, `SGX_LINEAR_TEX=1`). A 1024 x 1024
upload and draw 230 -> 24 ms; 64 boxes of 16 x 16, a draw after each,
13.3 s -> 62 ms. The X server over the first drag of a new window: 15 s of
CPU -> 3.7 s (the xdotool steps' own pace now), a second drag 2.3 s, two
copies made in all. `SGX_DEBUG=fps,flushes` reports per flush, for a
process that never swaps (the X server: ~85 renders a second, one or two
draws each, flushed at glFlush and at each framebuffer change).

Found on the way: piglit's map_invalidate cases passed or failed with
where the heap was -- a buffer's CPU memory is 64-byte aligned now (GL's
MIN_MAP_BUFFER_ALIGNMENT; musl's malloc gives 16).

**Flat shading.** OpenGL ES 1's colour is not a varying but the TA's base
colour (word 16 bit 11), after the position in the vertex as a varying
would be, TSP varying 10, iterated into one register -- its bytes as o0's,
or as F16 into two (e20) -- and flat when bit 26 of its iterate's control
word is clear. Bits 18:17 of word 18 are 3 then, as iOS has them; the TSP
takes the last vertex's colour whatever they say (gldesk), and no other
varying is shaded flat by anything found (M36's bits, e01's on varying 0).
So a flat input -- st's nir_lower_flatshade makes GL's flat colours so --
is one of the TA's two colours (10 and 11): the layout's first varyings,
iterated after the others into a register each, unpacked as o0 is for
blending (`colour_of_slot[]`; the layout's `ncolours`, word 16's bits, the
others' TSP numbers less the colours'). ARB_provoking_vertex's first vertex
by turning each triangle round (b, c, a) to end with it. gldesk: last and
first vertex, textured; piglit: glsl-fs-flat-color, gl-1.0-dlist-shademodel,
provoking-vertex, arb-provoking-vertex-clipped-geometry-flatshading.

**Polygon offset, found, left off.** e19 and m30 (glPolygonOffset(1, 2):
word 2 0x0000f000 -> 0x0022f000) and `gloffset --probe` flipping bits: bits
25:21 the factor and 20:16 the units, each a signed 5-bit integer; a unit is
32 of 24-bit depth's at depths 0.5..1, a power of two less below (floating
point, by the depth of the triangle's last vertex), the factor exact on
`gloffset`'s slopes. With `SGX_ISP_OFFSET=1` the GPU's vertex path takes it
for whole factors and units -16..15 (the draw module's unit made the same,
both paths 12 of 12). Not by default: the ISP does not clamp the depth it
offsets to 0..1 (`gloffset --clamp`; every bit of words 1, 2 and 18 near
flipped), and dEQP's result_depth_clamp and factor_1_slope cases want it --
the draw module's offset it stays, dEQP's polygon_offset 14 of 14.

**Occlusion queries.** There were none: no `create_query`, Mesa said 0
counter bits and every query 0 samples (everything hidden, to a program
that culls by them). m29: ISP state A bit 14 counts the samples a draw
passes into the visibility buffer that the 3D block's first two words name
-- the kext's from GL's payload words 42 and 43, 0x640 apart: a count for
each core. No bit picks one of several counts (bits 15..23 of word 1 are
the object's type and the depth test), so a render goes at each begin and
end, every render a query is active in has a buffer of its own (from the
cache, zeroed), and the result is theirs added; it is ready when the last
render before the query's end is (piglit's occlusion_query_order). gldesk:
a quad's 1536 samples exactly, none behind another. Known: a discarded
pixel is counted (discard keeps the tile's colour, M19 -- piglit's
occlusion-query-discard, and glBitmap's meta_fragments), and with depth
writes off the ISP counts some samples the depth test fails, though it
draws them right (conform's multi2).

**Depth textures and shadow samplers.** A depth texture's copy is F32 now
(format 0x12, nearest -- the sampler filters no F32, iOS has no
OES_texture_float_linear), not 8-bit grey; a depth buffer the GPU wrote is
read out of the ISP's tiles first (`zs_to_linear`), and its copy is made
again after every render that stores it: shadow maps work. The variant key
has the depth units, their views' swizzles (GL's depth texture modes) and
the compared ones' functions; nir_lower_tex_shadow compares in the program
as iOS does (ALWAYS until a variant says), and a depth texel is d, 0, 0, 1
through the swizzle. The key is built from the shader's textures_used, not
its compiled form's samplers: a lookup compared ALWAYS is a constant there.
gldesk: read, LEQUAL and GREATER against 0.5, a shadow map the GPU drew;
piglit: depth-tex-compare, depth-tex-modes, depth-tex-modes-glsl, texdepth,
fbo-depthtex.

**Two-sided colour.** GL's back colours never reached the program: st's
nir_lower_two_sided_color picks gl_FrontFacing's between COL0/1 and
BFC0/1, and the compiler took no input past the texture coordinates but
the generic ones. It takes the back colours as it does the colours now:
piglit's two-sided-lighting cases and vertex-program-two-side's pass.

dEQP-GLES2 groups re-run: texture 769 / 62 and polygon_offset 14 / 14 as
r37/r38, fbo 544 / 0.

**piglit p5** (the release build, `/root/piglit-results/p5`, before the
two-sided colour): **807 pass, 161 fail**, 1 crash, 1 timeout -- p4's 774 /
187. 38 fixed (the logic ops, flat shading and provoking vertex, depth
textures and shadow compare, rectangle textures, drawpixels and readpixels
of depth and stencil, map alignment, ...), 7 query tests run that were
skipped (5 pass). It broke 10, fixed since: a flat varying that is not a
colour (the linker's, glsl-sin and glsl-cos) and linear sampling that
repeats (texwrap 2d, texwrap formats). With those and two-sided colour's 9,
the next whole run should read about 826. What fails is mostly border
colours (14), 3D textures (10), depth and stencil copies and draws of
pixels, punch-through's (discard with depth, its queries), pbo, line
stipple and smooth lines.

**What the rest needs:**

- Punch-through (a discard that writes no depth; the alpha test with
  depth; queries of discarding draws): not in the render's registers nor
  in anything per draw that differs from ours -- still M19's question.
- From the capture, ready to do: user clip planes in the TA (word 16 bits
  3:0, a word in the vertex after the varyings, written by a VDP with its
  clip flag -- e14's `dp4.f32 o8.x, pa4.x000, i1.xyzw clip0`; an encoder
  for it first: no draw module for them then), points and point
  sprites as the ISP's objects (word 1 bits 16:15, word 16 bit 8, e15's
  sprite coordinates are lookup kind 5), fog (bit 9, varying 12), the
  pixel back end's 565/4444/5551/half formats (the end-of-tile words above),
  depth textures sampled straight from the ISP's tiles (type 4, F32M: no
  copy for shadow maps).
- Border colours: GXM's border modes (4..7) clamp to texels of the texture's
  own, there is no border colour -- the program's, if anything.
- Polygon offset's clamp; gl_PointCoord (VARYING_SLOT_PNTC); 3D textures;
  F16 arithmetic.

## M38: F16 colours; what SuperTux's frames take to the screen; a vblank

**Where SuperTux's frame went** (world 1's first level, Tux running right:
`tools/sgx/ukbd.py`, a uinput keyboard, on KMS; xdotool under X). Two
measurements had misled:

- On KMS SuperTux had drawn into a corner of the screen: its config kept a
  test's fullscreen size, 384 x 512. `SGX_DEBUG=fps` prints the target's
  size and the last viewport now (768 x 1024; SDL's viewport 922 x 1152,
  the picture right).
- SDL batches draws only when no renderer is named or
  `SDL_RENDER_BATCHING=1`: supertux-mesa.sh named GLES 2's, and a frame
  was 355 draws on KMS against 31 under X. Batched, SuperTux on KMS runs
  at **66.7 fps**, its own limit (64 logical frames a second, a step of
  1000 / 64 ms rounded down to 15), with 29 % of a core and no wait for
  the GPU (unbatched: 87 %); every frame is a flip (251 atomic commits in
  4 s). Its render takes the GPU 7.2 ms (`SGX_DEBUG=fps,sync`), the
  flip's copy of the plane 2.2 ms (debugfs `copy`: 768 x 512 in 1.15 ms).

Under X (batched, desktop GL) it was 42.5 fps, three quarters of the time
waiting for a fence: `SGX_TRACE_WAITS=1` (the caller of every wait of 1 ms
or more) shows `dri_flush`'s throttle, the render of the frame before. The
GPU is shared, and the gpu_scheduler and dma_fence tracepoints (`perf
record -a -e gpu_scheduler:drm_run_job ...`) show with what: SuperTux's
window is composited. xfwm4 4.20 leaves a fullscreen window out of the
composition only when `_NET_WM_BYPASS_COMPOSITOR` is 1 on the window it
composites -- the frame it made -- and SDL sets it on its own window, the
frame's child. So the X server copies each frame into the window's pixmap
(glamor, 2.5 ms of GPU), xfwm4 draws the screen with GL (the root's
texture from pixmap, a rectangle texture with an alpha of 1, in the
program: 7.4 ms) and flips it: per 33 ms two SuperTux renders, xfwm4's,
three copies, the GPU full -- and 30 frames a second on the screen (the X
server's page flips, xfwm4's: `perf record -e raw_syscalls:sys_enter -e
raw_syscalls:sys_exit --filter 'id == 54' -p $(pidof Xorg)`, the ioctls
and their results). With compositing off SuperTux under X is at its 66.7
too, every frame shown (66 DIRTYFB a second, no wait for the GPU). (Off
means through the session's D-Bus: `xfconf-query` from ssh without it
changes nothing, and the first measurements here were all composited.)

**The screen's vblank.** The kernel's screen had none
(`CRTC_GET_SEQUENCE`: EOPNOTSUPP): Present ran without an MSC, GL programs
under X unpaced. A timer at the mode's 60 Hz stands for the interrupt now
(`drm_vblank_init`, `enable_vblank` starts an hrtimer that calls
`drm_crtc_handle_vblank`), and the driver's own commit tail sends a
commit's event as soon as its copy is done, with the count it is, and waits
for no vblank before the old framebuffers go. glxgears under X: 60.0 fps;
SuperTux on KMS a commit a frame, 66.7, as before.

**F16 colours.** iOS's SDL sprite (M35) is all F16, four channels an
instruction:

- The PDS iterates the varyings as F16 (the iterate's format 2, bits
  29:28; two registers a varying, DOUT size 0x12) and fetches the texels as
  F16 (state word 3 bit 31; iOS's `07040c52`: the iterate DOUT 0x52 and
  the texture DOUT's kind 0x024).
- V16NMAD (opcode 0x02) has V32NMAD's fields, each operand a register pair
  of four F16: `mul.f16 pa0.xyzw, pa2.xyzw, pa0.xyzw` (`usse_hop`).
- VPCK from F16 (source format 5): `pck.u8.f16 o0.xyzw, pa0.xyzw scale`
  (`usse_pack_unorm8_f16`). Five of iOS's words in usse-test, bit for bit.

The compiler makes a fragment program so (`f16_colour()`) when its colour
is, channel for channel, one tree of fmul and fadd over varyings and the
PDS's lookups (no swizzles, at most 16 nodes, each an instruction into a
temporary pair) and the program reads nothing else: no uniform, constant,
gl_FragCoord, TA colour, discard, branch or logic op. Blending stays
SOP2's after the pack. F16's 11 bits are mediump's; a highp colour made
so loses what F16 loses in sums that nearly cancel, and goes to 8 bits
anyway. `SGX_NO_F16=1`: none so.

SDL's sprite 8 -> 3 instructions (9 -> 4 blended). Release builds:
`glspeed 8 30 fill` (eight blended layers of the whole 768 x 1024 target)
30.0 -> 16.1 ms a frame, 2 ms a layer, unblended 26.8 -> 16.2;
`glspeed 1000 30 sprites` 34.6 -> 25.8 ms. SuperTux's render on the GPU
12.0 -> 7.2 ms; under X it draws 42.5 -> 53 fps, and 58-62 with the X
server and xfwm4 on the build too -- of which the screen shows 30 composited, 66.7 not;
on KMS 66.7 either way (unbatched, the heavy parts' GPU waits go: 58-60 ->
62-64). dEQP-GLES2 16 859 / 91 (r37's 16 858 / 92 and one more pass:
`tmp_array.vec3_dynamic_write_dynamic_read_fragment`), no new failure, no
timeout; the regression tests 18 of 18, their F16 programs (glfetch,
gltex, gldesk, glvary) among them.

**What is next:**

- Composition that costs less: xfwm4's program (a rectangle texture's
  scale in it, an alpha of 1) as an F16 PDS lookup -- or fullscreen games
  left out of it (xfwm4 reads the bypass hint from its frame).
- F16 for more programs: uniforms (iOS's t06, `mul.f16 pa0, pa0, sa6`:
  the uniforms written as F16 into sa), constants, an X8 texture's alpha
  of 1 (glamor's copies of a 24-bit window), swizzles.
- Tux stood still under X this time (strun's xdotool keys did not move
  him): the X numbers are of the level's start.

## M39: projective and rectangle lookups by the PDS -- xfwm4's composition

xfwm4 composites with GL: the root's buffer as a texture from pixmap, a
rectangle texture, drawn with the fixed function by `glBegin(GL_QUADS)`.
st's fragment program for it (`SGX_DEBUG_NIR=1` with `xfwm4 --replace`):
`texture2DRect` of `TEX0.xy * scale / TEX0.w` -- NIR's lower_txp
and the rectangle's load_texture_scale -- 17 instructions, an SMP in the
program, 7.4 ms a frame of 768 x 1024.

**Coordinates the vertex side makes ready** (`lower_fetch_coords()`).
Where a lookup's x and y are a varying's x and y times the rectangle's
scale and 1 / its z or w, and nothing else reads the varying, the lookup
is made at the varying's own x and y -- the PDS's -- and the vertex side
does the rest (`sgx_fs.vary_fix`, `SGX_FIX_*`): x and y times 1 / width, 1
/ height (`fix_sa`, two sa words a varying, the draw's), the divisor
moved to w. The PDS's projective lookup is kind 2 of the control word's
lower half (`0x0c00f200`, iOS's s08 `0c00fa00`), and it **divides by w**:
glproj with the divisor in z draws wrong, and the GLSL linker had packed a
vec4 whose z nothing read into a vec3 (x, y, w), the divisor in z and w
1. On the GPU's vertex path such a varying's values are kept to the end,
as the position's (`vertex_output()`); on the CPU's (the draw module:
quads, xfwm4's) `fix_varyings()` does it to the vertices. A lookup that
gets ready but does not become the PDS's fails the compile rather than
sample unscaled. `SGX_NO_PROJ_FETCH=1`: none so.

xfwm4's program is then a texel and the pack -- F16 (M38), 2 instructions
-- and its frame 7.4 -> 2.8 ms of GPU. SuperTux composited: **30 -> 41-45
frames a second on the screen** (204 and 227 page flips in 5 s, the
second with all of X on the system's Mesa), 59-66 drawn. The
rest is four passes over the screen for each frame shown: SuperTux's
render (7.2 ms), the X server's copy of it into the window's pixmap
(Present, 2.5 ms: glamor's copy is already F16, 2 instructions -- the
fill's floor), glamor's composition of the window into xfwm4's buffer
(2.5) and xfwm4's own (2.8): about 850 ms of GPU a second, so 60
composited is out of reach. Uncomposited (M38) every frame is shown.

`tools/sgx/gl/glproj.c`: texture2DProj of a vec4 (z junk) and of a vec3, a
vec4 whose z the program reads besides (left alone), w across the quad
(against the division in the program), texture2DRect, texture2DRectProj,
two lookups of one varying, the fixed function's rectangle texture by
quads and by triangles (RGB, alpha from the colour), q 2 on a rectangle
and a 2D texture -- 13 of 13, and with `SGX_NO_PROJ_FETCH=1`. dEQP-GLES2
16 859 / 91, the same cases as M38's run; the regression tests 19 of 19.

**What is next:** fullscreen windows out of the composition -- xfwm4
reads `_NET_WM_BYPASS_COMPOSITOR` from its frame, SDL sets it on its own
window (a patched xfwm4, or the hint copied to the frame before it maps);
quads on the GPU's vertex path (lists, as strips and fans are).

## M40: fullscreen games out of xfwm4's composition

What M39 left of composited SuperTux was the composition itself: four
passes over the screen for each frame shown. A fullscreen window that asks
to bypass the compositor (`_NET_WM_BYPASS_COMPOSITOR` 1, SDL's) should not
be composited at all, and xfwm4 4.20 never left one out:

- It read the hint from the window it composites, the frame it made, which
  no client sets -- the hint is on the client's own window (`xwinfo.py`:
  1 on SuperTux's window, none on its frame). And the frame is added to the
  compositor at its CreateNotify, before its client is known, so even
  reading the client's window there would find no client.
- `map_win()` unredirected such a window but never counted it: the X
  server neither unmaps nor maps a window whose redirection changes
  (composite/compalloc.c), and only a window *mapped* unredirected is
  counted and has the overlay unmapped over it.

`tools/desktop/xfwm4/xfwm4-bypass-client-window.patch`: the hint read from
the client's window when the frame gets its client (`compositorSetClient`)
and when the client's window changes it; `bypass_win()` unredirects such a
fullscreen window and counts it as map_win() counts one mapped
unredirected (the overlay unmapped while one is out), from map_win(),
resize_win() (fullscreen after the map) and the hint's change;
`unbypass_win()` redirects it again when it stops being fullscreen, loses
the hint or is unmapped. `tools/desktop/xfwm4/build.sh install`: Alpine's
APKBUILD and patch from aports (3.24-stable) with this one, pkgrel + 101,
built by abuild in docker for armhf and installed on the iPad (4.20.0-r102;
the only compiler warning in compositor.c is xfwm4's own, line 304).

With compositing on, SuperTux's window is out of it: the X server shows
each frame (a copy and a DIRTYFB, no page flip -- 334 in 5 s), **66.7
frames a second on the screen**, no wait for the GPU, twice in a row; the
desktop is composited again when it exits (a screenshot: panel, icons,
shadows).

## M41: the screen turned by the copy onto it

The first step towards turning the screen with an accelerometer: the
turn itself. X's own (`xrandr --rotate left`) works on modesetting, but as
a shadow buffer glamor redraws turned every frame, page flips off:
SuperTux 24 fps, about 17 of them shown (DIRTYFB stalls).

**In the kernel.** The screen is a copy anyway -- the 2D engine's quad
from the framebuffer onto the scanout memory (M16) -- and the quad's
vertex order is its texture's corners (top-left, bottom-left,
bottom-right, top-right, p105-gpu.md): a quarter turn anticlockwise is
those corners moved round by one (`__sgx_blt_run`'s `rot`), a copy that
costs what it did. The plane has a `rotation` property (0, 90, 180, 270);
a damaged rectangle (framebuffer coordinates) goes to the screen through
`drm_rect_rotate()` -- what DRM's own plane checks use -- its edges out to
16 pixels in the framebuffer's x and, turned a quarter, in its y (then the
screen's x). Framebuffers may be the screen on its side (the minimum size
the shorter side: 1024 x 768 was refused at first).

`tools/sgx/gl/kmsrot.c` (no X; the text console unbound while it runs --
fbcon blinks its cursor into the same memory): each rotation committed
with a buffer whose every pixel is its own, the screen read back through
`/dev/fb0` against `drm_rect_rotate`'s place for each pixel, then a box
changed and shown with DIRTYFB alone -- all four rotations right, whole
and damaged.

**In X.** The kernel refuses an X server atomic modesetting (`broken
atomic modeset userspace detected` -- modesetting's `Option "Atomic"`
does nothing), and without it modesetting never sees the primary plane.
`tools/desktop/xorg-server/xorg-server-plane-rotation.patch`: the
primary plane and its rotation found with universal planes (allowed);
a RandR rotation the plane has is its own (`driverIsPerformingTransform`:
no shadow buffer); a change of turn is the CRTC off, the plane's rotation
set, then SetCrtc with the buffer of the turned size (the kernel's SetCrtc
takes the plane's turn into account, and checks a turn against the buffer
shown -- apart, either is refused); and the DIRTYFB clips are the screen's
own, not turned as for a shadow (first: the screen's top quarter never
updated). `tools/desktop/xorg-server/build.sh install`: Alpine's
xorg-server 21.1.25 with it, 21.1.25-r103 (LTO links two threads each --
four at once, fifteen threads each, ran docker's 8 GB out).

`rotate [normal|left|inverted|right]` (`tools/desktop/rotate.sh`, on the
iPad in /usr/local/bin) turns the screen and the touch screen with it
(libinput's Coordinate Transformation Matrix: for left x' = 1 - v, y' =
u). Turned left SuperTux runs in its own landscape, 1024 x 768, at **62-66
fps, every frame shown**; inverted 64-66. The desktop right in all four
(`/dev/fb0` read back: xwd shows the screen unturned).

**What is next:** the accelerometer (an IIO device, a mount matrix),
iio-sensor-proxy and a session daemon calling `rotate`; the side switch as
the rotation lock; touch checked by hand in each turn.

## M42: a draw's vertices written once, into GPU memory

SuperTux's first level draws little; its main menu draws more, and at one
moment (the menu's background scrolling past its busiest part, about 20 s
in) the frame rate falls -- to ~37 fps one boot, ~45 the next (the same
build, a different start: compare runs from one boot only).

**Who copies.** perf's call graphs do not work here (no frame pointers;
uprobes break Thumb code). `tools/sgx/memcpy-who.c`, an LD_PRELOAD memcpy
and memmove that count bytes by caller (`LD_PRELOAD=/root/memcpy-who.so
MEMCPY_WHO_TOP=N PROGRAM`; library + offset, resolved with addr2line in
the cascadia-mesa image -- a Thumb return address is odd, one less). In 56
s of the menu, through the dip, three callers in libgallium copied the
same 562 MB each: SDL's client arrays (vertices in the program's memory)
were repacked by `gpu_vs_draw` into the context's scratch stream, copied
from it into the gathered render's stream by `add_vs_draw` (or
`merge_vs_draw`), and from that into GPU memory by `sgx_frame_render`'s
arena -- three copies of every vertex, ~3 KB a draw, 80-100 draws a frame.

**One.** The stream is written where the GPU reads it: a gathered render
has up to 40 vertex chunks, 256 KiB buffers from the BO cache (or one a
draw's size), and `gpu_vs_draw` repacks a draw straight into the last
chunk's next free bytes (`vtx_reserve`, 16-byte aligned). Gathering the
draw (`vtx_commit`) only moves the chunk's end past it -- or, if a flush
came between (a full render, a new chunk), copies it into the render now
gathering, the one case of a second copy. A draw merged into the last one
needs its vertices to follow the last one's in the same chunk (they do,
written one after another). `sgx_frame_render` takes a draw with
`vdata_va` as already placed (no arena copy) and the chunks' handles come
after the buffers' in the render's list (256 at most, counted with the
rest); after the render the chunks are marked busy with its fence and
given back to the cache. The context's scratch stream and the batch's are
gone.

Measured (the same boot, SuperTux 0.6.3 in its menu, landscape, under X):

| | before | after |
|---|---|---|
| copies in 56 s, all callers | 2042 MB | 940 MB |
| the vertices' copies | 3 x 562 MB | 1 x 559 MB |
| CPU before the dip | 71% | 65% |
| CPU in the dip | 84-86% | 75-77% |
| fps in the dip | 44.9-46.9 | 46.1-48.0 |

What is left of the dip is the GPU's: `SGX_DEBUG=fps,sync` (each render
waited for) shows 11-12 ms of GPU a render before it and **16 ms** in it.
The rest of the copies are SDL's: libSDL2 itself (30 million copies of 4
bytes: `GL_QueueGeometry`'s colour of each vertex, an `SDL_memcpy` call
per vertex -- SDL_render_gl.c:1021, named by gdb with `sdl2-dbg`) and
SDL_image's loading.

Checked: the regression programs (glclear, gltri, glfs, gltex, glblend,
gldepth, glcull, gldiscard, glstencil, glwrap, glvtx, glmip, glvary,
glldr, glflow, glfetch, gldesk, glupload, gloffset, glproj) all pass;
glspeed's sprites 25.3 ms a frame (GPU-bound, as before); dEQP's
vertex_arrays, draw, buffer, attribute_location, default_vertex_attrib,
clipping and rasterization groups as in M38.

### SuperTux 0.7.0; a render's textures

Alpine 3.24 has SuperTux 0.6.3; 0.7.0 (March 2026) is built here as a
package: `tools/desktop/supertux` -- Alpine's APKBUILD brought to 0.7.0's
CMake build with upstream's choices (GLEW), pkgrel 100+, `sh
tools/desktop/supertux/build.sh` (abuild in docker for armhf, ~12 min;
the 292 MB source kept in `build/supertux/distfiles`), `build.sh install`
(the packages through `/var/tmp`: the data are 283 MB and the iPad's /tmp
is 128 MB of RAM; libraqm, new in 0.7.0, by `tools/device-apk.sh`). One
patch, for the C library and not the driver:
`supertux-musl-no-execinfo.patch` (the crash handler's `backtrace()` is
glibc's; Alpine no longer has libexecinfo).

**The game as it is.** The rule: programs are not fitted to the driver,
the driver is made right for them. A first package (r100-r101) had
libepoxy and a patch that let the OpenGL renderer run on OpenGL 2.1; r102
is upstream's code again, and what it finds is the driver's to answer:

| `--renderer` | what it asks | here |
|---|---|---|
| `sdl` (the default) | SDL's renderer, OpenGL 2.1 | 43 fps |
| `opengl20` | an OpenGL 2.0 context | 33 fps |
| `opengl_auto` | 3.3 core, else the SDL renderer | "GlewError: Missing GL version", the SDL renderer: 43 fps |
| `opengl` | an OpenGL 3.3 core context | exits: there is none (GL 2.1, GLSL 1.20) |

So OpenGL 3.3 is what is missing (the 3.3 core context fails, and 0.7.0
then reads the version of no context: GLEW's error). What an epoxy build
met on the way is the same gap: SuperTux then sets
`SDL_HINT_VIDEO_X11_FORCE_EGL`, and SDL's EGL path (`SDL_EGL_CreateContext`,
SDL_egl.c:1075) asks a desktop context for `GL_MAJOR_VERSION` -- a GL 3.0
query, `GL_INVALID_ENUM` on 2.1 as the specification wants (found with
gdb on `_mesa_error`, `sdl2-dbg` for SDL's names).

**Three renders a frame.** 0.7.0's menu samples more than 96 MiB of
textures a frame (contents and the sampler's copies), and past
`SGX_BATCH_TEX_BYTES` (48 MiB, M35) a render was made and another begun:
three renders, the screen stored and loaded between them, 3 x 7.4 ms of
GPU. `SGX_DEBUG=fps` now says what a render made for want of room ran out
of (`renders from sgx_draw.c:1456 148 ... (no room for: draws 0, buffers
0, textures 148, bytes 0)`). The limit is for textures that only the
render keeps -- piglit's streaming-texture-leak makes, draws with and
deletes 5000 of 4 MiB -- so only those count now: a texture whose last
reference is the render's, and a copy an upload replaced. One render a
frame, 18.8 ms of GPU:

| SuperTux 0.7.0's menu | before | after |
|---|---|---|
| `--renderer sdl` | 33 fps, 3 renders | 43 fps, 1 render |
| `--renderer opengl20` (GLX) | 29 fps, 3 renders | 33 fps, 1 render |

streaming-texture-leak for 150 s: the iPad's available memory stays at
235-250 MB (renders still go for the deleted ones). The regression
programs all pass; glspeed's sprites 26.0 ms.

Found on the way: the fixed function's room check did not count the
vertex chunks among the render's buffers (handles past 256 written over
`bos[]` when both kinds share a render), and a new chunk now also needs a
handle's room.

**Page flips; screenshots.** A fullscreen window out of the composition
(M40) presenting with vsync (an EGL build's `--renderer opengl20`, its
`vsync 1`) is page flipped: strace on Xorg shows a `MODE_PAGE_FLIP` and its
event every ~30 ms, the game's pace, and the kernel copies each flipped
buffer onto the screen whole. What looked like "nothing shown" was the
screenshot: xwd reads X's front buffer, which a flip leaves behind (the
desktop under the game), and a `/dev/fb0` read that went wrong.
`tools/desktop/xshot.py --screen` reads the glass itself -- `/dev/fb0`,
the scanout's memory, turned as X is (`rotate`).

**Merging.** 0.7.0's SDL renderer has no draw merged into the last one.
`SGX_DEBUG=fps` now says why a draw was not: here always another texture
(SDL already joins draws with the same texture) or another program, which
no render can merge. On the way: M42's stream reservations were aligned to
16 bytes, and SDL's quads (6 vertices of 20 bytes) then left a gap after
each draw, which kept draws with the same state from merging; dword
aligned now (the fetch reads dwords; applications' buffers have any
offset). dEQP's vertex_arrays, draw, buffer, attribute_location,
default_vertex_attrib, clipping and rasterization as before.

### Towards OpenGL 3: the plan

Programs ask for OpenGL 3 more and more (SuperTux 0.7.0's own renderer is
3.3 core; SDL's EGL path asks a GL 3.0 query), and the answer is the
driver's. glxinfo: GL 2.1, GLSL 1.20, no core profile. Mesa's
`compute_version()` (src/mesa/main/version.c) wants for 3.0 GLSL 1.30, 4
colour attachments, 4 samples (or its fake MSAA) and 18 extensions; 3.1,
3.2 and 3.3 another 7, 8 and 8 (3.2's GLSL 1.50 brings geometry shaders).
The driver has 10 of the 39: draw_elements_base_vertex,
explicit_attrib_location, fragment_coord_conventions, half_float_vertex,
map_buffer_range, sync, texture_rectangle, provoking_vertex, texture_sRGB,
vertex_array_bgra. Today it is a GLES 2 part's driver: one render target,
no 3D or array textures, float vertex buffers only, 8 varyings.

What the SGX543 has for the rest (from what is known: PS Vita's GXM on the
same core, iOS's captures, this driver) and what it does not -- where it
does not, Mesa's own way for such parts (the draw module on the CPU, a
conversion at upload, a shader doing the fixed function's work) is the
answer, as r300, i915 and lima do it:

| | what | how, on the SGX |
|---|---|---|
| 1 | ARB_framebuffer_object (`mixed_framebuffer_sizes`) | renders over the attachments' common area; the depth buffer's tiles by its own size |
| 1 | ARB_texture_rg, EXT_texture_snorm | GXM's U8, U8U8, S8 formats and swizzles |
| 1 | EXT_texture_swizzle | the texture state's swizzle (word 3) |
| 1 | ARB_texture_float, half floats, ARB_color_buffer_float | F16x4 textures and the PBE's half float target known; F32 formats to capture; clamping in the program |
| 1 | EXT_packed_float, EXT_texture_shared_exponent, RGTC | converted at upload (to F16, to R8/RG8) |
| 1 | ARB_depth_buffer_float | the ISP keeps depth as F32 already |
| 1 | EXT_framebuffer_sRGB | the program converts its output (sRGB textures: GXM's gamma mode, M37) |
| 1 | ARB_vertex_type_2_10_10_10_rev, other vertex formats | repacked by the CPU, as client arrays are (M26) |
| 1 | ARB_draw_instanced, ARB_instanced_arrays | a draw an instance, the instance's attributes as constants |
| 1 | NV_primitive_restart | the CPU splits the indices |
| 1 | NV_conditional_render | the CPU waits for the query |
| 1 | ARB_timer_query | the kernel's render times |
| 1 | ARB_uniform_buffer_object | uniforms in memory already (M32, VLDST) |
| 1 | ARB_blend_func_extended, ARB_depth_clamp | blending is the program's anyway; depth clamped in it |
| 2 | GLSL 1.30-3.30 | the compiler: integers and bit operations (the USSE has them), division by NIR's lowering, `texelFetch`, `textureSize`, `textureLod` in pixel programs (SMP's LOD mode, M33), `gl_VertexID`, `switch` |
| 2 | EXT_texture_integer, rgb10_a2ui | integer sampling: GXM's U16/U32 formats, to capture; integer targets through the program's packing |
| 3 | 4 colour attachments, EXT_draw_buffers2 | the PBE writing more than one surface at the tile's end: to find (capture or try); else a pass a target |
| 3 | 4 samples | iOS has APPLE_framebuffer_multisample on the A5: hardware MSAA, to capture |
| 3 | EXT_texture_array (and GL 1.2's 3D textures) | a layer's texture state chosen by the program (if SMP takes its state from registers) -- else layers in one image, the program addressing them |
| 3 | EXT_transform_feedback | vertex programs on the CPU (the draw module's stream output) while it is on |
| 4 | geometry shaders (3.2), multisample textures, seamless cube maps, texture buffers | the draw module again; the rest to find |

Order: the first rows (each small, each with its piglit and dEQP groups)
make M43; the compiler M44; then the hardware to find (several targets,
MSAA, arrays) with an iOS capture for each; 3.0, then 3.3 core -- SuperTux's
`--renderer opengl` as it is the measure of the last.

## M43: the first small items towards OpenGL 3

Each item of M42's plan, then piglit's groups for it, the build before
against this one (the system's M42 Mesa against `mesa/devbuild.sh
release`): what passed must pass, what is new is looked at.

**ARB_framebuffer_object** wants `caps.mixed_framebuffer_sizes`:
attachments of different sizes render over what they have in common (the
framebuffer's width and height, Mesa's minimum). `sgx_frame_target` has an
area now (w, h; 0 the level's), which the end of tile's clip and the
render target data take; the depth buffer's tiles keep their own width, so
a depth buffer larger than the colour is stored in place. A depth clear at
a render's start, and the colour clear of a whole surface, are taken only
when the framebuffer covers the attachment -- else a quad in the render.

**glBlitFramebuffer** as GL has it, on the CPU: each destination pixel's
centre taken back into the source (any sign of either box: mirrored
blits), nearest or between the four texels around it, clamped to the
source's edges; nothing written where the centre falls outside the source
or the scissor; blended with the source's alpha when st asks; colour, then
depth and stencil, when the mask has both (it used to give up: "a blit
scissored ... not done yet").

**A depth buffer's tiles for each level and layer.** The ISP's tiles were
one buffer a resource, at level 0's size: a depth texture's levels
rendered to (`depthstencil-render-miplevels`, `fbo-clear-formats`) went
into level 0's. `sgx_resource_zls(res, level, layer)`: a buffer, valid and
cleared flags and the clear's depth for each, made at their first use;
maps, the sampler's copy and renders take the level's. And a depth clear
no draw followed was dropped when the framebuffer changed (the batch
flushed with no draws) -- or taken by the next framebuffer's render: it is
noted for its level now.

**EXT_texture_swizzle**: a view's swizzle in the program, after the
texture unit's own channel order (`sgx_blend_key.tex_swizzle`, through
`nir_lower_tex`'s swizzles); depth views keep theirs (M37).

**Textures wider than 8 bits a channel.** The sampler's copy was RGBA8 for
every format the hardware had not: floats, signed and 16-bit channels lost
their range and sign. The texture formats are PS Vita's
(`SceGxmTextureBaseFormat`: U8 0x00, U8U8 0x07, U8U8U8U8 0x0c, F32 0x12,
F32M 0x13, F16F16F16F16 0x1b -- every one this driver knew is in that
list), and F16x4 is filtered: a format with a float, signed or wider than
8-bit channel, RGB10_A2, R11G11B10F and RGB9E5 are copied as four half
floats (`sgx_resource_copy_f16`; util_format unpacks them, `_mesa_float_
to_half` packs), and sampled with SMP's F32 return -- the four channels as
F32, nothing to unpack or scale (`sgx_blend_key.tex_f16`; not by the PDS's
lookups, whose texel is bytes). Floats keep F16's precision (11 bits, up
to 65504): the sampler filters no F32 (iOS has no
OES_texture_float_linear). ARB_texture_float, EXT_texture_snorm and
EXT_texture_shared_exponent are the driver's now. `tools/sgx/gl/glfmt.c`:
2 x 2 textures of each, sampled at texel centres and filtered, scaled into
[0, 1] by the program and read back against what GL says the format keeps
-- 32 of 32.

**Render targets of R8 and RG8** (ARB_texture_rg wants them renderable,
not just sampled). The pixel back end's format word is GXM's colour format
too (`SceGxmColorFormat`: the base format in bits 31:28 and 24:23 -- U8
0x80800000, U8U8 0xb0800000 -- the swizzle in 21:20) with bit 16 set, as
all of iOS's captures have it. Bit 26, which those captures' 565, 4444 and
5551 had, is GXM's *tiled* surface: with it glfmt's half quad landed in
whole rows of memory, each 32 x 32 tile's pixels one after another (tile
(1, 0) at 2 KiB, (1, 1) at 6 KiB). Linear: R8 0x80810000, RG8 0xb0810000,
the line stride in the target's own pixels. The tile holds the program's
colour as RGBA bytes whatever the target, and the PBE takes R (and G) from
it; the background object loads the target's texels back as a texture of
its format (U8 with L8's 1RRR swizzle, U8U8 with GR). No two-channel
swizzle gives alpha 1, so blending treats a target without alpha as alpha
1 (gallium's rule: DST_ALPHA one, INV_DST_ALPHA and SRC_ALPHA_SATURATE
zero). `sgx_frame.c`'s `rt_format()`; `SGX_PBE_R8=...` and `SGX_BG_R8=...`
try others. glfmt's targets: cleared, a render later half drawn (the left
half loaded from memory), added to, multiplied by the destination's alpha,
read back and sampled -- RGBA8, R8, RG8, 6 of 6.

piglit: FBO, blit, depth, stencil, shadow and swizzle groups 149 -> 243
passes; texture formats 109 -> 139; with R8 and RG8 targets, the
texture_rg, RGTC, FBO, blending and texwrap groups 113 -> 219 (and
`fbo-blending-formats` passes); nothing that passed fails.

**A cube map's faces.** dEQP found the half float copy's cube maps wrong
(`random_teximage2d.cube_7`, an 8 x 8 one of GL_ALPHA half floats): the
faces' spacing rule -- 2 KiB aligned from 16 x 16 (M28, glmip) -- is in
bytes: aligned once level 0 is 512 bytes or more, 16 x 16 of four-byte
texels, 8 x 8 of eight (`SGX_CUBE8_*` tried 1 KiB, 4 KiB and other sizes:
only this passes), 32 x 32 of one -- which is why one-byte cube maps
(dEQP's size.cube.16x16_l8) had their faces elsewhere and were copied as
RGBA8; they are as they are now, and pass. dEQP-GLES2 whole: as at M38
but for that case, fixed (texture.specification 192 of 192).

**Float render targets.** A target of half floats keeps 64 bits a
pixel: iOS's m28 (EXT_color_buffer_half_float) has `mov.f16 o0.xyzw,
pa0.xyzw` for gl_FragColor -- o0 and o1, R G and B A -- and the PBE word
0x07010000 (less the tiled bit: F16F16F16F16 0x01000000). What it took:

- The program packs its colour with one VPCK F32 to F16 (the unorm8
  pack's word with dest format 5, no scale, mask xyzw): o0 and o1 at
  once. Two moves of raw words, `or o0, sa0` then `or o1, sa1` (the clear
  quad's first form), or a LIMM into o1, never reached the target: o1
  written alone is lost, so every float clear is a VPCK from four F32 in
  sa too.
- The tile keeps one register a pixel unless the 3D block's word +0x4c is
  1: until then the PBE wrote o0's halves twice (R G R G). Which word was
  found by running iOS's kext: `rtemu.py` (unicorn, the user's 8.4.1
  kernelcache) runs its 3D block builder (0x80bf5eec) on the render
  commands the capture logged for m25 (565) and m28 (half floats); the
  blocks differ in that one word (the payload's word 34).
- Blending reads the tile back with VPCK F16 to F32, R G from o0 and B A
  as channels 2, 3 of the same pair (naming o1 read o0 again); SOP2's byte
  blending, the F16 program path (M38) and logic ops are not for floats,
  and nir_lower_blend is told the target is float (no clamping).
- The background loads the target as a texture of its format handed back
  as F16 (state word 3 bit 31) into two registers -- the PDS's iterate
  size 0x52 and texture DOUT kind 0x024 of iOS's F16 lookups, patched into
  the render's own copy of the PDS block (+0x144, +0x148) -- and our
  program, iOS's `mov.f16 o0.xyzw, pa0.xyzw` (+0x120's DOUTU).

The other kinds of tile (`enum sgx_rt_kind`): two half floats in o0 (RG16F
F16F16 0x00810000, R16F F16 0xf0010000), an F32 in o0 (R32F F32
0x10810000), two F32 in o0, o1 (RG32F F32F32 0x11010000; the colour and
the background moved by a VPCK F32 to F32 of two channels, iterate size
0x12). R11G11B10F (APPLE_color_buffer_packed_float on iOS): the tile's four
half floats packed by the PBE with F11F11F10 0x21010000 and bit 25 --
without it the PBE wrote o0's bits as they are (0x38003400 read back as
2.0, 5.7e-6, 0.0039: R and G's halves). RGBA32F has no colour format of
GXM's (64 bits at most): not a target yet. RGB16F is R16G16B16X16_FLOAT,
its alpha 1 to blending (a target without alpha: Mesa leaves the
destination alpha to drivers that have no per-target blending). glfmt:
RGBA16F, RG16F, R16F, R32F, RG32F, R11G11B10F -- cleared, half drawn a
render later, added to, times the destination's alpha, values past [0, 1]
read back as floats.

**sRGB targets** (EXT_framebuffer_sRGB, `caps.dest_surface_srgb_control`):
the same bytes; the program encodes its colour and decodes the tile's for
blending (nir_lower_blend leaves both to the driver: `lower_srgb_target`,
nir_format_linear_to_srgb and back), a clear's colour encoded. And R G B A
in memory (R8G8B8A8, GXM's ABGR: 0x00010000, loaded with swizzle 0).

**Intensity, luminance and alpha targets.** With R8G8B8A8 renderable Mesa
put GL_INTENSITY renderbuffers in it, and blending took the alpha drawn,
not the intensity (piglit's fbo-blending-formats: 0.85 for 0.81); Mesa's
own fix for that needs per-target blending, and makes the alpha 1. So
they are targets of their own: I8 (written as R8, loaded RRRR -- the
sampler's swizzle 3, found from L8's 5 and A8's 6), L8 (1RRR), L8A8 (U8U8
written R then A: 0xb0b10000), A8 (U8 written from A: 0x80910000); an
intensity target's colour goes R R R R into the tile, so a blend later in
the same render reads the intensity as alpha. All of fbo-blending-formats
passes now, the alpha, luminance and intensity formats that were
incomplete before among them.

**The rest of the list**, the draw module's where the hardware has not:

- Instanced draws, instanced attributes, primitive restart (ARB_draw_
  instanced, ARB_instanced_arrays, NV_primitive_restart): the vertex
  shader on the CPU, the draw module's (`gpu_vs_can_draw` sends them
  there). iOS has EXT_draw_instanced and EXT_instanced_arrays on the A5:
  the GPU's way is there to find.
- NV_conditional_render: the query waited for (or not, as the mode says),
  draws, clears and blits skipped when its result says so.
- ARB_timer_query: a render's GPU time is its fence's signal time -- the
  kernel's dma-fence timestamp, read through a sync file
  (SYNC_IOC_FILE_INFO) -- or the call's, if later (an idle GPU);
  CLOCK_MONOTONIC both, `get_timestamp` the same. (ES 2.0 has no
  GL_TIMESTAMP glGet in Mesa: ext_disjoint_timer_query's simple fails
  there.)
- ARB_vertex_type_2_10_10_10_rev: the packed formats are vertex formats
  now, read by the draw module's translate.
- ARB_blend_func_extended: blending is the program's; nir_lower_blend takes
  the second colour in, and without blending it is dropped.
- ARB_color_buffer_float (`caps.vertex_color_unclamped`).
- ARB_uniform_buffer_object: constant buffers 1..15, read from memory
  with VLDST as M32's uniforms are -- each buffer's address in an sa word
  after the uniforms, the offset VLDST's immediate (words 0..127), a LIMM
  past that, or the program's (a float: made a word index as
  memory_uniform() makes one). NIR's block k is Mesa's constant buffer
  k + 1 (lower_uniforms_to_ubo is off), and with the integers lowered to
  floats a constant offset may come as a float's bits (16 bytes,
  0x41800000). A block holds 64 KiB; one a program reads that is not
  bound drops the draw. Both stages' default uniforms are OpenGL 3's 1024
  components now (256 vec4s; the fragment shader had 64): past sa's words
  in memory, as before.

piglit, the build before against this one: the FBO, blending, sRGB,
float, instancing, restart, conditional render, timer query, 10-10-10-2
and dual-source groups 274 -> 409 passes; arb_uniform_buffer_object 33
more (all of quick_gl's that run), with the glsl-1.10, glsl-1.20,
shaders, ARB program and OpenGL 2 groups as they were; nothing that
passed fails. dEQP-GLES2 whole: two cases more (texture.specification's
cube_7, indexing.tmp_array's vec4_const_write_dynamic_read_fragment) and
one less, fbo.completeness.renderable.texture.color0.rgb_half_float_oes
-- Mesa's, not the driver's: EXT_color_buffer_half_float makes RGBA
textures of type HALF_FLOAT_OES colour-renderable and not RGB ones, and
Mesa takes the RGB16F it makes of them for the sized format. Exposed
once RGB16F is a target (Mesa's CI lists the case as a failure for
etnaviv, r300, crocus, panfrost T720 and a2xx); fixed in Mesa's
gles_check_float_renderable(), carried in `mesa/mesa.patch` -- the fbo
group 544 of 544 again.

**ARB_depth_buffer_float.** The tiles hold F32 depth whatever the
buffer's format (M23), so Z32_FLOAT is the ISP's F32 store, as Z16 and
Z24X8 were. With stencil, ZLSCTL's 24-bit format would round the depth;
the formats' field has a fourth value, 3 (bits 25:24 stored, 22:21
loaded), which stores and loads both: F32 depth and the stencil in 5
bytes a pixel, 5 KiB a tile. Its layout, from a pattern of known depths
and stencils (`SGX_ZLS_*` knobs of a build of the time, a stencil
1 + tile and single pixels marked; every pixel of a 256 x 256 buffer
decoded right): each row of 32 pixels a stream of 160 bytes -- for each
pixel the stencil, then the depth's bits as a key that sorts as an
integer (a positive float's sign bit flipped, a negative one's every
bit), most significant byte first -- each 16 bytes of the stream stored
reversed; the tiles where the other formats have theirs, 5 KiB apart.
`sgx_resource.c`'s CPU copies go through it both ways.

What did not work: vitasdk's GXM depth/stencil formats read as ZLSCTL's
bits (S8D24 0x01266000, D16 0x02444000, DF32M 0x00044000, DF32M_S8
0x00066000), the last with a stencil surface of its own. Format 0 with
the stencil bits, and a stencil buffer at the 3D block's +0x78 and +0x7c
-- the microkernel loads ZLS registers 0x480..0x490 from +0x6c..+0x7c,
0x48c and 0x490 the stencil's load and store bases by the kext's names;
the kext fills both from iOS's payload word 32 (`rtemu.py` with each
payload word marked), 0 on iOS -- stored no stencil there and loaded
none, as an offset from the ZLS base or as an address, with bits 16 and
17 either way.

gldesk: GL_DEPTH32F_STENCIL8 and GL_DEPTH_COMPONENT32F over two renders,
read back, and depths 1e-7 apart from the CPU through a render that loads
and stores them, read back exactly (a 24-bit store would round them); 28
of 28. piglit's depth and stencil groups 110 -> 174 passes, nothing that
passed fails; what fails fails for 24-bit depth too (glDrawPixels and
glCopyPixels of depth: gl_FragDepth; the border colour; depth mipmaps).

**ARB_depth_clamp** is the draw module's, its near and far clipping off.
The ISP clamps no depth (`gloffset --clamp`), so `render_triangle` cuts a
triangle where its window z crosses an end of the depth range -- a line
across it on the screen, window z being linear there -- and draws the
parts past the ends at the end's depth, a fan each: the clamped depths
exactly. New vertices' varyings are perspective-correct (1 / w and v / w
linear on the screen); with flat shading every vertex has the provoking
one's colours. Without clamping the same keeps depth in 0..1, where the
draw module's polygon offset may take it past. The draw module's wide
line and point stages bind a rasterizer state of their own for their
triangles, depth clipping and clamping both off, so the context keeps the
program's `depth_clamp` apart (the first try clamped quads but not lines
and points). piglit's depth_clamp and depth-clamp-range pass; the clip,
depth range and polygon offset groups, and dEQP's clipping,
rasterization, polygon_offset, draw and vertex_arrays, as before.

That is the last of the plan's first rows. Left for later from them: the
GPU's own instancing (iOS has EXT_draw_instanced and EXT_instanced_arrays
on the A5), RGBA32F targets (no GXM colour format of 128 bits), and the
gaps below; next is the compiler (M44).

**Gaps the new tests ran into** (they had been skipped): 3D textures --
GL 1.2's, not there at all (`fbo-incomplete`, `fbo-generatemipmap-3d`);
the border colour (every `texwrap formats bordercolor`); `gl_FragDepth`
(`depth-clear-precision-check`); a sampler LOD bias past +4 (word 0's field
is -3.875 .. 4: `tex-miplevel-selection` with a bias of 5); gl_PointCoord;
MSAA (`ext_framebuffer_multisample` with 0 samples).

## Testing, without and with the device

- **Host, every change:** the kernel driver builds with `W=1` against the
  pinned v6.12 (`make LLVM=1 ARCH=arm drivers/gpu/drm/apple-sgx/`), the
  userspace with `-Wall`; the USSE encoder against captured programs; the
  render target code against `rtemu.py`.
- **Device:** `sgxinfo`, sgx2d's demos and SuperTux on every kernel change;
  Mesa's own tests once there is a Mesa driver.
- **iOS:** gltrace, for anything the hardware does that the corpus does not
  show yet.
