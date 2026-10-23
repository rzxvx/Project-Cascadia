# mesa/ — the SGX543MP2's Mesa driver

A Gallium driver for the iPad mini's GPU, built into Mesa 26.1.8 for the
device. The plan and the state of it: [docs/research/p105-mesa.md](../docs/research/p105-mesa.md)
(M12 on). It is the system's OpenGL once installed -- OpenGL ES 1 and 2,
desktop OpenGL 2.1 through GLX and EGL (M35), dEQP-GLES2 16 859 / 91 and
piglit's GL profile measured (M35, M36) -- and runs OpenGL ES 2.0 programs:
clears on the GPU (`sgx-gl
glclear 100`: every pixel right, ~60 ms a clear with its read-back), and
draws on the GPU: vertex shaders (M18: with viewports and culling; points,
lines, flat shading and clip planes still go through Gallium's draw module
on the CPU) and fragment shaders are compiled to the GPU's own code (M13c:
float arithmetic, varyings, uniforms, comparisons, ifs and loops -- M20;
`sgx-gl glfs`); a fragment shader the compiler does not
take yet is drawn in a colour worked out per vertex (M13a). Textures, blending and the depth test work (M14); render targets
are any size from 1x1 to 4096x4096, the render target data the kext would
compute for each made by the driver (M10, `sgx-gl glsize`). On the screen
through KMS and GBM on `/dev/dri/card0` (M16: `sgx-gl glkms`, a kernel from
2026-10-08); weston runs on it, its GL clients too, any number of processes
at once (M17: a kernel with UAPI 3). It needs a kernel from 2026-10-04 or later (the render node's
workarounds for the microkernel, M12; buffers out of the BIF's tiled
window, M13a).

| path | what |
|---|---|
| `files/` | new files, copied verbatim into the Mesa tree: the driver (`src/gallium/drivers/sgx`), its winsys (`src/gallium/winsys/sgx/drm`), the render node's interface (`include/drm-uapi/apple_sgx_drm.h`, a copy of the kernel's) |
| `mesa.patch` | the lines that register the driver with Mesa (meson option, subdirectories, DRM driver descriptor, pipe loader, dril) |
| `fetch.sh` | Mesa at the pinned tag into `build/mesa/src`, on the host (git under emulation is far too slow) |
| `build.sh` | put the driver in, configure, build, install, build `glclear`, `gltri` and `glfs` — inside the `cascadia-mesa` image |
| `Dockerfile` | that image: Alpine 3.24 armv7 with Mesa's build tools |
| `install.py` | onto the iPad, over ssh or into the NFS root, as `tools/sgx/mkpack.py` does it; `/etc/ld-musl-armhf.path` makes it the system's OpenGL (M35) |
| `sgx-gl` | on the device: run a program with this Mesa (or `SGX_MESA=dir`'s) instead of the system's |
| `devbuild.sh` | a build beside the system's, unpacked on the iPad into `/root/devbuilds` (M36) |
| `sgx-dev` | on the device (`/root/sgx-dev`, devbuild.sh puts it there): a program with `/root/sgx-mesa-cur` |
| `frame-bisect` | on the device: a clear through each `SGX_FRAME` variant, to find a wrong piece of the template frame |
| `host/` | the driver on a Linux PC through drm-shim: `build.sh [arm]`, `run`, a fake template frame, drm-shim's patch for armhf glibc; `usse-test.py`, the USSE encoder against the disassembler; `rt-test.py`, the render target code against the kext's (`rtemu.py`), 61 sizes; `tmpl-test.py`, the template frame Mesa builds against sgx2d's pack |

## Building and installing

With the iPad running a kernel that has the render node, booted from NFS,
and the template frame installed (`./cascadia gpu`):

```bash
./cascadia mesa                  # build (release), then install where it fits
./cascadia mesa --install HOST   # over ssh only
./cascadia mesa --root DIR       # into a root tree only
MESA_BUILD=debug ./cascadia mesa # with Mesa's assertions
```

The install makes it **the system's OpenGL** (M35): `/etc/ld-musl-armhf.path`
puts `/usr/local/lib/sgx-mesa/lib` ahead of `/usr/lib`, so every program --
SDL's, GLEW's, Xorg's glamor, weston -- loads this Mesa and no wrapper is
needed; Alpine's Mesa stays installed, and deleting the file brings it back.
It also puts X's configuration for the GPU in `/etc/X11/sgx.conf.d`
(modesetting with glamor: `desktop` takes it) and links the driver where the
X server's GLX looks for it.

**Trying a change** without touching the system's: `mesa/devbuild.sh` (or
`mesa/devbuild.sh release`) builds and unpacks into `/root/devbuilds/N` on
the iPad, `/root/sgx-mesa-cur` (`-rel`) pointing at the newest; then
`/root/sgx-dev PROGRAM` runs a program with it (`SGX_MESA=dir` another
build), and `/root/hangs.sh` counts the renders that timed out since a
point. dEQP-GLES2 (`tools/sgx/deqp-run.py`) and piglit (`tools/piglit`)
take a build the same way (`SGX_WRAP=/root/sgx-dev`, `WRAP=/root/sgx-dev`).

Mesa's source is cloned on the host first (`mesa/fetch.sh`, ~120 MB;
`MESA_URL=` names another mirror). The build then runs Alpine's own armv7
compilers under the host's ARM emulation (Docker Desktop has it; on a Linux
host, install qemu-user-static's binfmt handlers). The first build takes a
long time — expect the better part of an hour on a laptop; after that only
what changed is compiled again, unless the build options changed (then
everything is). Everything lands in `build/mesa` (out of git): `src` (Mesa),
`build-release`, `install-release` -- and `build`, `install` for a debug
build (`MESA_BUILD=debug`, assertions on: a program can stop on one of
Mesa's own where a release build goes on).

On the iPad:

```sh
sgx-gl glclear                   # one clear, read back and checked
sgx-gl glclear 100               # timing
SGX_DEBUG=frame sgx-gl glclear   # every word a clear sets
sgx-gl gltri                     # three draws, read back and checked (--fb to see them)
SGX_DEBUG_DRAW=1 sgx-gl gltri    # the triangles each draw hands the GPU
SGX_DRAW_LAYOUT=8,3,f32 sgx-gl gltri   # the vertex side with 8 varyings, the colour
                                 # the 4th, F32 (the rest fillers; docs, M13b)
sgx-gl gltri --ppm /tmp/x.ppm    # the last read-back as a picture: look at it
sgx-gl glfs                      # the compiler's 40 cases (M13c; M20-M22 loops, arrays)
sgx-gl glcull                    # culling, viewports, depth ranges, gl_FrontFacing (M18, M19)
sgx-gl gldiscard                 # discard (M19: the tile's colour kept; depth still written)
sgx-gl glstencil                 # the stencil test, its ops, masks and clear (M23; M24
                                 # across renders)
sgx-gl gldepth                   # the depth test, within a render and across two (M24)
sgx-gl glchurn                   # 3000 programs made, drawn and deleted: GPU memory reused
sgx-gl glwrap                    # texture wrap modes, each axis (M24)
sgx-gl glvtx                     # the vertex path: layouts, formats, buffers, indices (M26)
sgx-gl glspeed 400 40 flush      # draws a second, a glFlush a frame: renders queued (M27)
sgx-gl glspeed 1000 60 sprites   # SDL's way: textured blended quads from client arrays (M34)
GLSPEED_FS=4 sgx-gl glspeed 8 30 fill   # 8 full-target layers: the GPU's cost a pixel
sgx-gl glmip                     # mipmaps, mip filters, cube maps (M28)
SGX_TEX_PROBE=1 sgx-gl glmip --probe 16 32   # where the sampler reads a cube map's levels
sgx-gl glvary                    # 1 to 8 vec4 varyings, each read back; a mat3 varying (M30);
                                 # whole numbers through varyings as ints, int division (M31)
sgx-gl glldr                     # uniforms past sa's 128 words, read from memory (M32)
sgx-gl glflow                    # texture lookups, dFdx/dFdy in programs that branch (M33)
sgx-gl glfetch                   # texture lookups the PDS makes before the program (M35);
                                 # SGX_NOFETCH=1: all in the program
sgx-gl gldesk                    # desktop OpenGL 2.1: 1D, sRGB and rectangle textures, the
                                 # fixed-function pipeline (M35, M36); depth textures, shadow
                                 # samplers, flat shading, occlusion queries, logic ops (M37)
sgx-gl glupload                  # textures written between draws that read them, each format
                                 # the sampler reads as it is, upload times (M37)
sgx-gl gloffset                  # glPolygonOffset's units and factors (M37); --probe with
                                 # SGX_STATE=2:mask, --clamp: whether a depth past 1 is clamped
SGX_ISP_OFFSET=1 sgx-gl gloffset # the ISP's polygon offset instead of the draw module's
SGX_DEBUG=copies sgx-gl glupload # each texture copy the CPU makes: whole, a box, a new buffer
SGX_DEBUG=fps,flushes desktop    # the per-2 s report for the X server (it never swaps): its
                                 # renders, where they were flushed from (M37)
SGX_NO_F16=1 sgx-gl glfetch      # the colour as F32 even where it could be F16 (M38)
sgx-gl kmsrot                    # the plane's four turns (M41), the screen read back through
                                 # /dev/fb0, a DIRTYFB box too; with X stopped
sgx-gl glproj                    # projective and rectangle lookups by the PDS, the vertex
                                 # side scaling and moving the divisor (M39);
                                 # SGX_NO_PROJ_FETCH=1: in the program
SGX_DEBUG=fps PROGRAM            # also the target's size and the viewport at the frame's end
python3 ukbd.py 20 & supertux-mesa.sh LEVEL   # SuperTux on KMS, Tux run right by a uinput
                                 # keyboard (tools/sgx; M38)
SGX_TEX_NATIVE=0 sgx-gl glupload # every texture copied as RGBA8 (M36's way)
SGX_LINEAR_TEX=1 sgx-gl glupload # every texture of one level sampled linear (0: never)
SGX_STATE=16:0x200 sgx-gl glflat # which vertex's colour a triangle takes (finding flat
                                 # shading, M36)
SGX_DEBUG_NIR=1 sgx-gl glfs mad  # the NIR the compiler takes (M36)
SGX_ITER_XOR=0x20000 sgx-gl glflat   # flip bits of the iterates' control words (M36)
/root/piglit/run.sh NAME         # piglit's GL profile, ~40 min (tools/piglit)
python3 /root/pigsum.py /root/piglit-results/NAME   # what did not pass
SGX_LDR_PROBE=e8870004a0008080 sgx-gl glldr --probe   # a program of those words: VLDST
SGX_FETCH_TAG=9 sgx-gl glvary    # the vertices a vertex task takes, less one (M30: 9 is
                                 # what overlapped the outputs of three varyings and more)
SGX_REPACK=1 sgx-gl glvtx        # every draw's vertices made one stream by the CPU
SGX_DEBUG_DRAW=1 sgx-gl glvtx    # each vertex shader draw: fetched or made, its addresses
SGX_CPU_VS=1 sgx-gl glcull       # the same with the vertex shaders on the CPU
SGX_STATE=18:1 sgx-gl glcull     # xor bits into every draw's state words (finding them)
SGX_DEBUG=state sgx-gl gltri     # each draw's state words (cmd: the TA command, 3D block)
SGX_ZLS=0 sgx-gl gldepth         # depth and stencil in the tiles only (no z load/store)
SGX_DEBUG=fps supertux-mesa.sh   # SuperTux on the driver, the frame rate every 2 s (M25;
                                 # tools/sgx; ,sync: each render's GPU time)
SGX_TRACE=1 sgx-gl ...           # a timeline: flushes, kicks, waits, renders' ends
SGX_DEBUG_SHADER=1 sgx-gl glfs mad   # a compiled program's words (tools/iosgpu/usse-dis.py words)
SGX_NOCOMPILE=1 sgx-gl gltri     # without the compiler: M13a's per-vertex colour
sgx-gl frame-bisect              # one clear through every SGX_FRAME variant (docs, M12)
```

## Without an iPad

Mesa's drm-shim pretends to be the render node, so the driver can be run on
any Linux machine. Renders do nothing there, so only CPU clears come back
right, but draws can be followed with `SGX_DEBUG_DRAW=1`, and every word a
render would hand the GPU with `SGX_DEBUG=frame` (against a fake template
frame, `host/fakepack.py`, which has nothing of Apple's in it):

```bash
bash mesa/host/build.sh            # build/mesa-host/host: x86-64, debug
bash mesa/host/build.sh arm        # build/mesa-host/arm: armhf, run under qemu-arm-static
mesa/host/run gltri
SGX_DEBUG_DRAW=1 SGX_DEBUG=frame mesa/host/run gltri
mesa/host/run --arm glclear 2
```

The packages it needs are in `host/build.sh`'s header.

The USSE encoder alone, on any host with a C compiler (macOS too):
`python3 mesa/host/usse-test.py` checks what it writes against the
disassembler and against iOS's own words.

## Licence

The driver's files are MIT, as Mesa's are, so they could go upstream; the rest
of this repository is GPL-2.0. `include/drm-uapi/apple_sgx_drm.h` is the
kernel's header and MIT too.
