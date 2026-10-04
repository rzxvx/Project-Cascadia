# P105 Mesa: where the work stands (handoff, 2026-10-04)

Written so a new session can pick the work up where the last one stopped.
Read this file first. Then read [p105-mesa.md](p105-mesa.md): it is the plan
and the record, M9 to M16, and its M11 and M13 sections have the details.
After that, [../../mesa/README.md](../../mesa/README.md).

The short version:

- **M12 is done.** OpenGL ES clears run on the GPU through Mesa.
- **M11 is read.** iOS's compiled shaders, state words and PDS programs are
  decoded.
- **M13a (first triangles) is in progress.** Triangles draw on the iPad in
  the right places, and constant colours come out right. Colours that vary
  across a triangle come out wrong, and one test draw does not show at all.
  Section 2 has the data and the next experiments.

## 1. How to work in this repository

- **Branch.** Work on `claude/nice-ptolemy-sc3zlz` of rzxvx/Project-Cascadia,
  then commit and push there. Do not open a PR unless asked.
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
  - The working copy is `~/Desktop/cascadia-mesa` on an Apple-silicon Mac.
  - They update and build with `git pull && ./cascadia mesa`. This builds
    Mesa in an Alpine armv7 Docker image and installs it on the iPad over
    ssh or into the NFS root.
  - The iPad is an iPad mini 1, iPad2,5 (`p105ap`), with a PowerVR
    SGX543MP2. It boots Linux from NFS, with a kernel that has the
    `apple-sgx` render node.
  - The same iPad also boots jailbroken iOS 8.4.1 (12H321), which is used
    for GPU captures.
  - The user pastes device output into the chat.
- **This side.** The cloud container has no device. Test on the host
  through drm-shim (section 6) before asking the user to run anything.

## 2. The open bug: M13a's colours on the device

### What runs

`sgx-gl gltri` (`tools/sgx/gl/gltri.c`) draws three things into a 768x1024
RGBA framebuffer object, reads each one back and checks pixels:

| test | what it draws | shader |
|---|---|---|
| 1 | a triangle at (-0.8,-0.8) red, (0.8,-0.8) green, (0,0.8) blue | a colour per vertex, `gl_FragColor = v` |
| 2 | a strip rectangle at x -0.9..-0.5, y 0.5..0.9, 4 vertices, so 2 triangles | `gl_FragColor = u`, orange (1, 0.5, 0.25, 1) |
| 3 | a thin triangle (0.05,0.5) (0,0.9) (-0.05,0.5), turned by `uniform mat2 m` | constant yellow |

Read-back pixel (0,0) is the bottom left, as GL has it.

### What the device gave

Five of 11 checks were right. The results were byte-identical in all three
runs:

- the first build, with the pack's pixel side;
- commit 6b303d9's default, iOS's pixel side for an iterated colour;
- `SGX_FRAME=packpixel`.

| point | read (R G B) | true colour (barycentric × 255) |
|---|---|---|
| near the red corner (96,117) | 54 a4 07 = 84 164 7 | 244.5 5.8 4.7 |
| near the green corner (672,117) | a5 56 04 = 165 86 4 | 5.4 244.9 4.7 |
| centroid (384,375) | 05 a5 55 = 5 165 85 | 84.8 85.2 85.0 |
| near the blue corner (384,896) | 00 00 00 | 3.7 4.1 247.2 |
| test 2, inside the rectangle (115,870) | 00 00 00 | 255 128 64 |
| test 3, the turned triangle (384,819) | ff ff 00 | ff ff 00 ✓ |
| every "outside" check | 00 00 00 | 00 00 00 ✓ |

Clears are right in every colour: `glclear`, and the black clear before
test 1.

### What the numbers say

- **Blue is about right** at the three drawn points (7/4.7, 4/4.7, 85/85).
- **R + G is right** at each of the three points (248/250, 251/250,
  170/170). R and G are mixed with each other, and how they mix depends on
  position. A fixed colour transform cannot explain it, because constant
  colours come out right.
- **A correction to the record.** The M13a paragraph in p105-mesa.md called
  blue "the true barycentric to the last bit". That overstates it. A linear
  fit through the three points:

  ```
  R_obs = 0.345R + 0.684G - 0.97B
  G_obs = 0.643R + 0.318G + 0.98B
  ```

  It is exact for three points, so it validates nothing. It is also
  contradicted by the blue-corner pixel, which reads all black: the fit
  predicts a green near 0.95 and B is near 0.97.
- **Parts are not drawn.**
  - Near the blue apex (the top of the triangle), the pixel is black. It
    should be bright blue whatever the mixing.
  - Test 2's rectangle (6 vertices, constant orange) does not show.
  - Test 3's triangle does show, in the same y range (768..973).
  - So a plain "y above 768 is not drawn" does not explain it.
- **Timings.** Test 1 took 35 ms (first draw: setup), tests 2 and 3 took
  about 1.2 ms each. That is not necessarily suspicious: the clear's 60 ms
  is mostly its 3 MB read-back from uncached memory.
- **The photo is not evidence.** A photo of the screen with `gltri --fb`
  showed a repeated striped pattern that does not match the read-back. The
  `--fb` copy to `/dev/fb0` may have the wrong stride or format.
- **Why the pixel program is probably not the cause.** Two different pixel
  sides gave identical bytes:
  - the pack's: texture block PDS, iterate `0x1fc01900` (format 1, varying
    1), then SOP2M texel × colour, then MOV;
  - ours: PDS `{doutu, 6, 0, 0x2fc0100f}` plus `070001b5 07040c12 af000000`
    (iterate varying 1 as F16), then `PHAS; pck.u8.f16 o0, pa0`.

  Either the varying data is already wrong when it reaches the pixel stage
  (the vertex program, the TA's varying setup, or the vertex data), or the
  hardware did not take our state word 6 at all. M12 did point word 6 at
  our own texture block at a new address and that worked, so word 6 is
  normally taken.

### Next experiments, in order

1. **Get the two read-backs as images.** Ask the user for `/tmp/gltri.ppm`
   and `/tmp/gltri-pack.ppm` from the last runs: `scp` from the iPad, then
   `sips -s format png` on the Mac. The whole picture shows the triangle's
   shape, the colour field and what is missing. This is cheap and should
   come first.
2. **Prove the iterated pixel side is the one that runs.** Run
   `SGX_DEBUG=frame sgx-gl gltri` and check that the log says "pixel
   program: the colour iterated as F16 and packed (iOS's)". The only debug
   log the user has sent was from the first build. Then temporarily make the
   program at `code_va + ITER_PROG` write a fixed colour: for example LIMM
   into o0, or a MOV from a constant. If draws do not change colour, word 6
   is not reaching the hardware.
3. **Isolate one channel at a time.** Add a mode to gltri, or a new test:
   - a full-screen quad where only R varies (0 at left, 1 at right), G = B
     = 0; then only G, only B, only A;
   - the same with vertical gradients;
   - read back a grid and fit planes.

   This shows which output component ends up where, and whether the
   vertex-to-colour association is wrong. That is the "mixing depends on
   position" signature: for example, the TA computing component planes
   from a permuted vertex order, or reading F32 outputs as F16 pairs. Word
   20 = 0x3 says both varyings are F16.
4. **Log test 2's six vertices** with `SGX_DEBUG_DRAW=1`. Only the first
   triangle is logged now. Check whether the strip's two triangles both
   survive the anticlockwise reordering, and whether a 6-vertex draw that
   is not full-screen works at all: try two triangles of test 1's kind.
5. **Read the pack's vertex side against iOS's.**
   - The pack (`tools/sgx/frame.py`, `programs.py: vertex()`):
     - fetch control `0x003` (rgba → pa0..3), `0x401` (uv → pa4..5),
       `0x801` (xy → pa8..9), stride 32;
     - phase 1 sets pa10 = 0 and pa11 = 1.0;
     - phase 2 has two VMOVs with SMLSI increments into o0..o9;
     - state word 16 = `0x0a001000` (10 output words), word 19 = 0x39,
       word 20 = 0x3.

     sgx2d only ever drew one colour per quad, so per-vertex colour was
     never tested.
   - iOS: `corpus.py DIR --state 'v0*' 'x00*'` and `--catalog v00_vec4`
     show its vertex program and words 16/19/20 for one vec4 varying.
   - Does iOS's vertex program write the varying as F32 or as packed F16?
     Does our output layout match words 19/20? Disassemble the pack's
     vertex program with `usse-dis.py words`, from `programs.py`'s
     assembled words.
6. **If the pack's vertex side is at fault, go to M13b now** (section 7).
   Write our own vertex program (position o0..o3, colour o4..o7 as F32 or
   F16 to match word 20), our own vertex fetch PDS, state words 16/19/20 to
   match, and the iterated pixel side we already have. This replaces the
   pack's vertex half.

## 3. M13a: how the code draws

The driver lives in `mesa/files/src/gallium/drivers/sgx/`. It is copied
into Mesa 26.1.8 by `mesa/build.sh`; `mesa/mesa.patch` registers it.

- **`sgx_draw.c/.h`**, the M13a path.
  - `sgx_draw_init` sets up Gallium's draw module:
    - `draw_create_no_llvm`;
    - a `vbuf_render` backend (`struct sgx_render`) with `need_pipeline`;
    - wide lines and points turned into triangles; no stipple or sprites.
  - Vertex shaders run on the CPU: NIR → TGSI (`nir_to_tgsi` frees its
    input, so pass a clone) → `tgsi_exec`.
  - `update_vertex_info`: position EMIT_4F, then one EMIT_4F per varying.
    Varying names follow `nir_to_tgsi`'s: `VARn` is `GENERIC n`.
  - `emit_vertex` writes `r g b a u v x y`:
    - colour per channel from the FS analysis (grey 0.5 when it is not
      understood), clamped to 0..1;
    - u = v = 0;
    - x and y in NDC from window coordinates.
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
    free 1 KiB:
    - the replace program at +0, then the iterated pixel program at +0x20;
    - end-of-tile programs in 0x80 slots after +0x40.
  - In the EXT window:
    - the white 4x4 texture's block at +0;
    - the iterated PDS at +0x100;
    - vertices at +0x280000;
    - per-render state at +0x3c0000.
  - The VDM draw word is `0x81c00000 | count`, over the pack's identity
    index buffer of 8192 entries.
  - `render(..., iterated)`: clears use the pack's pixel side; draws use
    the iterated one unless `SGX_FRAME=packpixel`.
  - State word 6 is `iterated ? 1<<27 | iter_pds>>4 : p27(texblock)`.
  - `SGX_FRAME=` takes comma-separated switches: `fb`, `blend`, `screen`,
    `codebo`, `sop2`, `align`, `packpixel` (sgx_frame.h, M12 in the doc).
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
  the ARM build takes longer, and the ARM path was checked by hand, not
  through the script.
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

- **M13b: our own vertex side.**
  - Our own vertex program and vertex fetch PDS.
  - State words 16, 19 and 20 to match.
  - Several varyings, and a colour that is not just varying 1.
  - This also frees the vertex layout from sgx2d's `r g b a u v x y`.
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

- **Last commits:**
  - 6b303d9: M13a with iOS's pixel side for an interpolated colour, and
    `gltri --ppm`;
  - af7b2d8: M13a draws;
  - 970b8bd and ab06d0e: M11 PDS and state decoding.
- **Task in progress:** M13a. The device results are in section 2.
- **What the user was last asked for:** nothing is pending. The last runs
  were `sgx-gl gltri --ppm /tmp/gltri.ppm` and `SGX_FRAME=packpixel sgx-gl
  gltri --ppm /tmp/gltri-pack.ppm`, both 5/11 with identical bytes.
- **Start the next session** by asking for those two images, or for a
  fresh run with `SGX_DEBUG=frame` (section 2, steps 1 and 2).
