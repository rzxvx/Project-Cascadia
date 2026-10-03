# mesa/ — the SGX543MP2's Mesa driver

A Gallium driver for the iPad mini's GPU, built into Mesa 26.1.8 for the
device. The plan and the state of it: [docs/research/p105-mesa.md](../docs/research/p105-mesa.md)
(M12 on). It runs OpenGL ES 2.0 programs today, but only clears reach the
GPU: there is no shader compiler yet, so draws are dropped.

| path | what |
|---|---|
| `files/` | new files, copied verbatim into the Mesa tree: the driver (`src/gallium/drivers/sgx`), its winsys (`src/gallium/winsys/sgx/drm`), the render node's interface (`include/drm-uapi/apple_sgx_drm.h`, a copy of the kernel's) |
| `mesa.patch` | the lines that register the driver with Mesa (meson option, subdirectories, DRM driver descriptor, pipe loader, dril) |
| `fetch.sh` | Mesa at the pinned tag into `build/mesa/src`, on the host (git under emulation is far too slow) |
| `build.sh` | put the driver in, configure, build, install, build `glclear` — inside the `cascadia-mesa` image |
| `Dockerfile` | that image: Alpine 3.24 armv7 with Mesa's build tools |
| `install.py` | onto the iPad, over ssh or into the NFS root, as `tools/sgx/mkpack.py` does it |
| `sgx-gl` | on the device: run a program with this Mesa instead of the system's |

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
```

## Without an iPad

Mesa's drm-shim pretends to be the render node, so the driver can be run on
any Linux machine (renders do nothing there, so only CPU clears come back
right):

```bash
meson setup build -Dgallium-drivers=sgx -Dtools=drm-shim -Dplatforms= -Dglx=disabled \
    -Dvulkan-drivers= -Dllvm=disabled -Dxmlconfig=disabled -Dexpat=disabled ...
ninja -C build
LD_PRELOAD=build/src/gallium/drivers/sgx/drm-shim/libsgx_noop_drm_shim.so \
    LD_LIBRARY_PATH=build/src/egl:build/src/mesa/glapi/es2api:build/src/gallium/targets/dri \
    ./glclear 6
```

## Licence

The driver's files are MIT, as Mesa's are, so they could go upstream; the rest
of this repository is GPL-2.0. `include/drm-uapi/apple_sgx_drm.h` is the
kernel's header and MIT too.
