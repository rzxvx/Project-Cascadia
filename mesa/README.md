# mesa/ — the SGX543MP2's Mesa driver

A Gallium driver for the iPad mini's GPU, built into Mesa 26.1.8 for the
device. The plan and the state of it: [docs/research/p105-mesa.md](../docs/research/p105-mesa.md)
(M12 on). It runs OpenGL ES 2.0 programs: clears on the GPU (`sgx-gl
glclear 100`: every pixel right, ~60 ms a clear with its read-back), and
draws with the vertex shader on the CPU (Gallium's draw module) and the
triangles and their pixels on the GPU. Fragment shaders are compiled to
the GPU's own code (M13c: float arithmetic, varyings, uniforms,
comparisons, unrolled loops and flattened ifs; `sgx-gl glfs`); one the
compiler does not take yet is drawn in a colour worked out per vertex
(M13a). Textures, blending and the depth test work (M14); render targets
are any size from 1x1 to 4096x4096, the render target data the kext would
compute for each made by the driver (M10, `sgx-gl glsize`). It needs a kernel from 2026-10-04 or later (the render node's
workarounds for the microkernel, M12; buffers out of the BIF's tiled
window, M13a).

| path | what |
|---|---|
| `files/` | new files, copied verbatim into the Mesa tree: the driver (`src/gallium/drivers/sgx`), its winsys (`src/gallium/winsys/sgx/drm`), the render node's interface (`include/drm-uapi/apple_sgx_drm.h`, a copy of the kernel's) |
| `mesa.patch` | the lines that register the driver with Mesa (meson option, subdirectories, DRM driver descriptor, pipe loader, dril) |
| `fetch.sh` | Mesa at the pinned tag into `build/mesa/src`, on the host (git under emulation is far too slow) |
| `build.sh` | put the driver in, configure, build, install, build `glclear`, `gltri` and `glfs` — inside the `cascadia-mesa` image |
| `Dockerfile` | that image: Alpine 3.24 armv7 with Mesa's build tools |
| `install.py` | onto the iPad, over ssh or into the NFS root, as `tools/sgx/mkpack.py` does it |
| `sgx-gl` | on the device: run a program with this Mesa instead of the system's |
| `frame-bisect` | on the device: a clear through each `SGX_FRAME` variant, to find a wrong piece of the template frame |
| `host/` | the driver on a Linux PC through drm-shim: `build.sh [arm]`, `run`, a fake template frame, drm-shim's patch for armhf glibc; `usse-test.py`, the USSE encoder against the disassembler; `rt-test.py`, the render target code against the kext's (`rtemu.py`), 61 sizes |

## Building and installing

With the iPad running a kernel that has the render node, booted from NFS,
and the template frame installed (`./cascadia gpu`):

```bash
./cascadia mesa                  # build, then install where it fits
./cascadia mesa --install HOST   # over ssh only
./cascadia mesa --root DIR       # into a root tree only
```

Mesa's source is cloned on the host first (`mesa/fetch.sh`, ~120 MB;
`MESA_URL=` names another mirror). The build then runs Alpine's own armv7
compilers under the host's ARM emulation (Docker Desktop has it; on a Linux
host, install qemu-user-static's binfmt handlers). The first build takes a
long time — expect the better part of an hour on a laptop; after that only
what changed is compiled again, unless the build options changed (then
everything is). Everything lands in `build/mesa` (out of git): `src` (Mesa),
`build`, `install`.

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
sgx-gl glfs                      # the fragment compiler's 23 cases (M13c)
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
