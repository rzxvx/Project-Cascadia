# P105 Mesa: where the work stands (handoff, 2026-10-04)

Written so a new session can pick the work up where the last one stopped.
Read this file first. Then read [p105-mesa.md](p105-mesa.md): it is the plan
and the record, M9 to M16, and its M11 and M13 sections have the details.
After that, [../../mesa/README.md](../../mesa/README.md).

The short version:

- **M12 is done.** OpenGL ES clears run on the GPU through Mesa.
- **M11 is read.** iOS's compiled shaders, state words and PDS programs are
  decoded.
- **M13a (first triangles) is done.** A colour per vertex, a uniform's
  colour and a triangle turned by the vertex shader all draw right
  (`sgx-gl gltri`: 11 of 11). The bug that held it up was not in the draw:
  render targets sat in the BIF's tiled window (section 2).
- **M13b (our own vertex side) is done**: 1 to 8 varyings, F16 or F32,
  no pack in the vertex half of a draw.
- **Next: M13c**, the compiler (section 7).

## 1. How to work in this repository

- **Branch.** Since 2026-10-04 the work is on `main` of
  rzxvx/Project-Cascadia (the `claude/nice-ptolemy-sc3zlz` branch was
  fast-forwarded into it). Commit there; **the user pushes**, the session
  does not. Do not open a PR unless asked.
- **Commit messages** end with the attribution trailer the session's system
  reminder gives. The last session used:

  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: <the session URL>
  ```
- **No model names or IDs** in anything committed.
- **Apple-derived data never goes into git.** That covers the kernelcache,
  firmware, the template frame "pack" built from iOS captures (it contains
  the GL driver's event program), and iOS GPU captures (`logs/ios/...`).
  `build/` and `logs/` are gitignored.
- **Languages.** The user writes in Russian; answer in Russian. Code,
  comments, commits and docs are in English.
- **Doc style.** Plain, dated record entries; keep the failed attempts.
  Code style is in the surrounding code: Mesa style in `mesa/files`, short
  docstrings in Python.
- **The user's side.**
  - The working copy is `~/Desktop/ipad-mini-linux` on an Apple-silicon
    Mac (`~/Desktop/cascadia-mesa` is the older checkout of the branch).
    The kernel tree `build/linux` is a link to `~/Desktop/linux-kernel`,
    shared by both.
  - They build with `./cascadia mesa`. This builds
    Mesa in an Alpine armv7 Docker image and installs it on the iPad over
    ssh or into the NFS root.
  - The iPad is an iPad mini 1, iPad2,5 (`p105ap`), with a PowerVR
    SGX543MP2. It boots Linux from NFS, with a kernel that has the
    `apple-sgx` render node.
  - The same iPad also boots jailbroken iOS 8.4.1 (12H321), which is used
    for GPU captures.
  - The user pastes device output into the chat.
- **This side.** Since 2026-10-04 the session runs on the user's Mac and
  reaches the iPad itself: `ssh cascadia` (root@10.55.0.2), the Mesa
  build is `docker run ... cascadia-mesa sh mesa/build.sh` then
  `mesa/install.py --install root@10.55.0.2` (what `./cascadia mesa`
  does), and runs are `ssh cascadia 'PATH=/usr/local/lib/sgx-mesa/bin:$PATH
  sgx-gl gltri'`. Flashing a kernel needs the user. A session without the
  device tests on a host through drm-shim (section 6).

## 2. The M13a bug, solved: the BIF's tiled window

The full account is in p105-mesa.md, M13a, "Solved". In short:

- The GPU drew everything right. The same draws into the framebuffer
  (`SGX_FRAME=screen`, then `cat /dev/fb0`) are a perfect picture.
- The render target was at 0xefcf0000, which the render node picked top
  down from 0xf0000000. `BIF_TILE1 = 0x0beffe00` (iOS's value, set by
  the kernel) makes 0xe0000000-0xefffffff a tiled window, stride 4096:
  within each 64 KiB, address bits 11:8 and 15:12 trade places, so the CPU
  saw the picture cut into 64-pixel pieces and shuffled. `BIF_TILE2`
  makes 0xf0000000 up another (configuration 0xc).
- Uniform clears look the same in any layout, which is why M12 passed.
  The colour "mixing" numbers in the earlier version of this file were
  pixels from other places.
- Fixed in the kernel (addresses below 0xe0000000, `SGX_TILED_VA_START`)
  and in Mesa for the kernel on the device until it is flashed
  (`untiled_bo` in sgx_device.c).
- Lesson for later checks: test with pictures that are not uniform, and
  look at a whole read-back as an image before reading numbers off it.

## 3. How the code draws (M13a, M13b)

The driver lives in `mesa/files/src/gallium/drivers/sgx/`. It is copied
into Mesa 26.1.8 by `mesa/build.sh`; `mesa/mesa.patch` registers it.

- **`sgx_draw.c/.h`**, the CPU vertex path (M13a).
  - `sgx_draw_init` sets up Gallium's draw module:
    - `draw_create_no_llvm`;
    - a `vbuf_render` backend (`struct sgx_render`) with `need_pipeline`;
    - wide lines and points turned into triangles; no stipple or sprites.
  - Vertex shaders run on the CPU: NIR → TGSI (`nir_to_tgsi` frees its
    input, so pass a clone) → `tgsi_exec`.
  - `update_vertex_info`: position EMIT_4F, then one EMIT_4F per varying.
    Varying names follow `nir_to_tgsi`'s: `VARn` is `GENERIC n`.
  - `emit_vertex` writes the frame's layout (M13b): `x y z w`, then the
    colour as varying 0:
    - x and y in NDC from window coordinates, z = 0, w = 1;
    - colour per channel from the FS analysis (grey 0.5 when it is not
      understood), clamped to 0..1;
    - `SGX_DRAW_LAYOUT=N[,K][,f32]` sends N varyings, the colour the K-th,
      the rest a filler colour (a test of the vertex side).
  - `render_index` makes every triangle anticlockwise.
  - `sgx_fs_colour_analyse` reads the fragment shader instead of running
    it:
    - clone; `nir_lower_io`; `nir_opt_copy_prop`; constant folding; DCE;
    - then each output colour channel is followed back to a constant, an
      input component, `load_uniform` or `load_ubo`;
    - control flow or discard means "not ok".
  - `sgx_draw_vbo`:
    - maps the vertex, index and constant buffers (constant buffer 0 is the
      VS's; constant buffer 1 becomes `fs_constants`);
    - calls `draw_vbo` and `draw_flush`;
    - `submit()` sends the triangles in chunks of
      `sgx_frame_max_vertices()` (8190) and keeps the last fence.
  - `SGX_DEBUG_DRAW=1` logs each submit's first triangle.
- **`sgx_context.c/.h`** hold the hooks that forward state to the draw
  module: bind and create for VS, FS, rasterizer and vertex elements;
  viewport, clip, constant and vertex buffers. Also the fences and
  `p->resource_release = u_default_resource_release`, without which context
  destroy crashes.
- **`sgx_frame.c/.h`**, the template frame from sgx2d's pack: one render per
  clear or draw, over what the target holds, because the background object
  reloads the target.
  - Our programs go in the pack's code page at `code_base + 0x1000`, in the
    free 1 KiB, all written once:
    - the replace program at +0, the iterated pixel programs (F16 at
      +0x20, F32 at +0x30);
    - the vertex programs for 0..8 varyings from +0x40 (52 instructions);
    - six end-of-tile slots of 0x58 from +0x1e0.
  - In the EXT window:
    - the white 4x4 texture's block at +0;
    - the iterated PDS per varying and format, 0x20 each, from +0x100;
    - the vertex fetch per number of varyings, 0x100 each, from +0x400;
    - vertices at +0x280000;
    - per-render state at +0x3c0000.
  - Draws use our vertex side (`struct sgx_frame_layout`; state words 16,
    19, 20; the VDM fetch word `(1+N) << 25 | 0x01800000 | (4+4N) << 7 |
    (N+2)`), clears the pack's. p105-mesa.md, M13b.
  - The VDM draw word is `0x81c00000 | count`, over the pack's identity
    index buffer of 8192 entries.
  - `render(f, rt, layout, ...)`: no layout = the pack's vertex side
    (clears, `SGX_FRAME=packvertex`); clears use the pack's pixel side,
    draws the iterated one unless `SGX_FRAME=packpixel`.
  - State word 6 is `iterated ? 1<<27 | iter_pds>>4 : p27(texblock)`.
  - `SGX_FRAME=` takes comma-separated switches: `fb`, `blend`, `screen`,
    `codebo`, `sop2`, `align`, `packpixel`, `packvertex` (sgx_frame.h).
    `SGX_FETCH=word`, `SGX_FETCH_TAG=n` override the VDM fetch.
    `SGX_DEBUG=frame` logs every word.
- **Tests.**
  - `tools/sgx/gl/glclear.c` takes `N` and `--fb`.
  - `tools/sgx/gl/gltri.c` takes `--fb` and `--ppm FILE`.
  - Both are built by `mesa/build.sh` and installed into
    `/usr/local/lib/sgx-mesa/bin`. Run them with `sgx-gl PROGRAM`.
- **On the iPad:**
  - `sgx-gl glclear [N] [--fb]`;
  - `sgx-gl gltri [--fb] [--ppm F]`;
  - `SGX_DEBUG=frame`, `SGX_DEBUG_DRAW=1`;
  - `sgx-gl frame-bisect [quick|full]`;
  - `SGX_CC=8`, `SGX_NOCC=1` (cache experiments, M12).

## 4. What is decoded (M11), in one place

These come from the corpus of iOS shader captures. The full treatment is in
p105-mesa.md, M11. The tools are in `tools/iosgpu/`.

### USSE operands

As Vita3K reads them; `usse-dis.py` implements this.

- **Registers.**
  - Vector-operand register fields are doubled into 32-bit register
    numbers (V16NMAD `dn=3` → `sa6`).
  - The top temporaries are the internal registers `i0..i3`.
  - In a secondary program, `pa` means `sa`.
- **Special bank.**
  - `c<n>` are hardware FP constants. In F16, `c<n>.x/.y` are the halves of
    F32 bank-0 word n, and `.z/.w` are bank 1's.
  - Constants seen so far: `c15.y` = 1.0, `c0.x` = 0, `c4.y` = 2.0, `c34` =
    1.0, `c12.x` (F32) = 0.5.
  - With bit 6 set, the operand is a global `g<n>`.
- **Formats and operations.**
  - VPCK formats: u8 s8 o8 u16 s16 f16 f32 c10.
  - VMOV types: s8 s16 s32 c10 f16 f32 u8 u16.
  - NMAD op2: mul add frc dsx dsy min max dp.
  - VCOMP: rcp rsq log exp.
  - VMAD2: `d = s0*s1 + s2`.
  - GPI VMAD: `d = gpi0*s1 + gpi1`.
  - VDP: `dot(s1, gpi0)`.
  - SMLSI: per-repeat increments or swizzle offsets.
- **Branches and ends.**
  - BR is relative (bit 38), in instructions, 20 bits signed.
  - Bit 50 means "end" only on VBW, SOP2, SOP2M, SOP3, VPCK, VMOV, LIMM,
    NOP and SPEC.

### iOS's compiler

- Precision: mediump → F16; highp → F32, in internal registers; lowp →
  SOP2M on bytes.
- Math that depends only on uniforms is hoisted into a per-draw secondary
  program. The pixel program then reads the result: `or o0, saN`.
- Vertex outputs: position o0..o3, then the varyings.
- Varyings reach the pixel program in `pa` as packed F16 (a vec4 takes 2
  registers).
- The uniform SA layout is the compiler's choice.
- Control flow: loops with `break` use BR; uniform conditions become
  predicates.
- Textures: a non-dependent read is done by the PDS; a dependent read uses
  `smp`.

### The TA's full state block

21 words; word 17 is the marker `f32(1e-5)` = 0x3727c5ac.

| word | meaning |
|---|---|
| 0 | ISP A (0xdfc7) |
| 1 | ISP B (`01d00300`; bit 25 = blending, set in the pack's `03d00300`) |
| 4 | secondary-loader PDS |
| 5 | pixel PDS info, not decoded: `0803e000` one varying, `1303e000` two, `2183e000` eight, `0003e000` none |
| 6 | pixel PDS: `data rows << 27 \| (va >> 4 & 0x07ffffff)` |
| 7–8 | tile clip |
| 9–14 | viewport |
| 16 | bits 31:24 = vertex size in words |
| 19 | 3 bits per varying, in output order: 001 = 2 components or a float, 011 = 3, 111 = 4 |
| 20 | one bit per varying: F16 |

### PDS programs

- **Layout.** A data segment in 16-byte rows, then code words.
- **iOS's DOUTU** is `(addr − 0x950000)/8 << 4 | 3`, or `| 8`. 0x950000 is
  the CPU-side code base. Ours is `doutu()` in sgx_frame.c.
- **Code words.**

  | word | meaning |
  |---|---|
  | `070001b5` | doutu row 0, then iterate or fetch |
  | `07000185` | doutu only |
  | `070401a5` | doutu row 1, after |
  | `07018113` | DMA row 0 |
  | `07041004` | texture fetch, state in row 1 |
  | `67800072` | fetch index |
  | `2f0n91a3` | fetch attribute |
  | `030n01f5` | vertex doutu |
  | `af000000` | end |
- **The iterate DOUT `0x07RRRWSS`.**
  - The index of its control word is `row*4 + word` (bits 17:12 and
    11:10).
  - Size: low byte `0x02`, `0x12` or `0x32` = 1, 2 or 4 registers.
- **The iterate control word**, e.g. `0x2fc0000f`:
  - bits 29:28: 2 = F16, 0 = F32 (the pack's is 1);
  - bit 25: last;
  - bits 23:22: components − 1;
  - bits 15:12: the source varying (13 = position).

  Texture-coordinate iterates look different: `0x0c00f900`, and
  `0x0c00fa00` for projected.
- **The vertex fetch row** is `{address, first_pa << 8 | words − 1, stride,
  0}`.

### Open items in the decoding

- Word 5's encoding.
- VDUAL is only partly decoded.
- `corpus.py`'s guess for a program continued on a page mapped elsewhere
  still misses `f30_tan`'s secondary program.

## 5. Tools

- **`tools/shadercap.sh [CASES]`** runs the shader oracle on iOS: each
  `tools/iosgpu/corpus/*.glsl` case is compiled by iOS's driver and the GPU
  memory each draw changed comes back.
  - It needs macOS to build `gltrace`.
  - The output goes to `logs/ios/corpus/<date>/` and stays out of git.
  - The cases are named by family:
    - `c*` control flow, `f*` functions, `p*` precision, `t*` textures;
    - `u*` uniforms, `v*` varyings, `x*` vertex shaders.
- **`tools/iosgpu/corpus.py DIR [mode] [CASES|patterns]`** reads a corpus
  run. Modes:

  | mode | output |
  |---|---|
  | `--brief`, `--own` | the programs each case changed |
  | `--list` | a line per program |
  | `--catalog` | each case's programs, disassembled |
  | `--diff A B` | memory after case A against after case B, without programs and bookkeeping |
  | `--state` | the TA state blocks, decoded |
  | `--pds` | the PDS programs, with iterate and fetch decoded |

  The user has run it as `python3 tools/iosgpu/corpus.py
  logs/ios/corpus/<date> --catalog 'f*'`.
- **`tools/iosgpu/usse-dis.py`** has three commands:
  - `words [--second] HEX...` disassembles words;
  - `dis FILE OFF N`;
  - `scan FILE`.
- **`tools/sgx/usse.py`** is the assembler; `programs.py`, `pds.py` and
  `frame.py` are the pack's programs, PDS and frame (sgx2d's). Their output
  is checked byte-identical to iOS's.

## 6. Testing without the iPad

`mesa/host/` builds the driver on a Linux PC against Mesa's drm-shim. All
GPU work is faked: renders do nothing, but every word up to the kick can be
checked.

```sh
bash mesa/host/build.sh            # x86-64 debug build -> build/mesa-host/host
bash mesa/host/build.sh arm        # armhf cross build, under qemu -> build/mesa-host/arm
mesa/host/run gltri                # 0 of 11: renders do nothing (SGX_PACK=/nonexistent: 4,
                                   # the clears done on the CPU)
SGX_DEBUG_DRAW=1 SGX_DEBUG=frame mesa/host/run gltri
mesa/host/run --arm glclear 2
```

- The script header lists the packages it needs. On a fresh Ubuntu
  container this means `apt-get install` for a few packages and `pip
  install meson mako pyyaml`.
- Mesa's source comes from `mesa/fetch.sh`, into `build/mesa/src`.
- `fakepack.py` builds a template frame from `tools/sgx/frame.py` with a
  zeroed event program: nothing in it is Apple's, so `SGX_DEBUG=frame`
  works anywhere.
- The x86 build took about 10 minutes on 4 cores (checked on 2026-10-04);
  the ARM build takes longer. Both were run through the scripts:
  `run gltri` gives 4 of 11 with `SGX_PACK=/nonexistent` on either.
- `drm-shim-t64.patch` makes drm-shim work with armhf glibc's 64-bit
  `time_t` symbols. It is only for these host builds; the device build does
  not build drm-shim.
- When regenerating `mesa/mesa.patch` from a tree that has it, exclude
  `src/drm-shim` (`git diff -- . ':!src/drm-shim'`).

The device build is `./cascadia mesa` on the user's Mac: an Alpine armv7
image under Docker's ARM emulation. The first build takes nearly an hour,
later builds are incremental. Always build with `-mtls-dialect=gnu`: the
gnu2 dialect breaks the current-context TLS on 32-bit ARM.

## 7. The road after the bug

From p105-mesa.md, M13 to M16.

- **M13b: our own vertex side** -- done 2026-10-04 (p105-mesa.md).
- **M13c: the compiler**, NIR → USSE:
  - an encoder in C (the tables of `usse.py`, Vita3K's bits);
  - a register allocator over the banks;
  - uniforms through the secondary attributes and secondary programs as
    iOS does it;
  - `programs.py`'s 16 programs as unit tests;
  - the M11 corpus as the oracle for every construct.

  Done when `es2gears` turns. The CPU vertex path (M13a) can stay as a
  fallback.
- **M10**, which is not done: render targets of any size without the
  kext's code. Today only screen-sized 768x1024 B8G8R8A8 targets render,
  and the render target data comes from the pack.
- **M14**: textures (the corpus `t*` cases), blending through
  `nir_lower_blend`, depth and stencil, scissor, culling.
- **M15**: dEQP-GLES2 and speed.
- **M16**: KMS, a compositor and the desktop.

## 8. Where the last session stopped

- **Last work (2026-10-04):** M13a solved (the tiled window) and M13b
  done: our own vertex side, 1 to 8 varyings, F16 or F32, checked on the
  iPad (`gltri` 11 of 11 for every layout, `glclear 20` 0 wrong).
- **On the device:** the new Mesa is installed. The kernel there (#291)
  is the old one; a kernel with the fix is built (`./cascadia build`) and
  waits for the user to flash it. Mesa works on both.
- **Next:** M13c, the compiler (section 7), starting with fragment
  shaders: the vertex side now carries any varyings to the pixels.
