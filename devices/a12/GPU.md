# A12 GPU integration

One `linux-0020-agx.patch` supplies the shared Asahi DRM interface and the
firmware-specific backends. The kernel builder selects `CONFIG_DRM_AGX=y`:
there is no production `insmod` step or probe-time compute/render test.

| Profile | Firmware | Boot integration |
|---|---|---|
| XR / N841 | GPU not enabled | Existing simpledrm path retained |
| XS Max / D331p | iOS 16.6.1, 20G81 | Contributor's BadApple m1n1 series, GFX upload, XS backend |
| iPad 8 / J171a | iPadOS 17.1.1, 21B91 | Versioned backend, reserved segments and live power-table handoff |

The cleaned, built-in integration has booted on iPad 8 with the Omarchy NFS
root. Updated Mesa passed clear/triangle pixel readback, original display
DMA-BUF checks, a user-confirmed visible window, and a temporary GLES2 labwc
desktop using the shared runtime. The desktop reported the Apple A12 renderer
with completed GPU jobs and no firmware crash. Native display remains separate.
This does not establish unlimited-session stability, X11/GLX or Vulkan support,
or hardware revalidation of the merged XR/XS paths.

## Build and boot

Select one profile consistently, for example:

```sh
export A12_DEVICE=ipad8       # xsmax, or A12_BOARD=d331p, selects XS Max
IM4M=/private/apticket.im4m ./cascadia a12 inputs
./cascadia a12 build m1n1
JOBS=8 ./cascadia a12 build kernel
./cascadia a12 build dtb
./cascadia a12 build initramfs
IM4M=/private/apticket.im4m ./cascadia a12 pack
./cascadia a12 boot
```

For native Linux m1n1 builds, install Rust and run `rustup component add rust-src` first.
Existing SysCfg, radio preparation and NFS-root requirements still apply.
`inputs` range-extracts and pins the selected GFX IM4P; it does not download
a whole IPSW. Packaging adds the ticket without substituting XS firmware on
iPad. Boot uploads GFX after the trust cache and before Linux/`bootx`.

The iPad-only m1n1 overlay validates and reserves the GFX segments, copies
live performance/policy properties, and then enables its GPU/mailbox nodes.
XR does not apply this overlay. XS retains its own m1n1 implementation.
Both firmware backends refuse a firmware restart after a fault; reboot is
required. iPad remains limited to the currently validated serial rendering
budget (4096 jobs, at most 2048x2160 targets). This is not unrestricted desktop
or Vulkan support. XS rendering limits and replacing/single-page VM binds
are retained from the contributor's driver.

## Shared Mesa

The patch is pinned to BadApple commit
`61bd066feed4406828f17fc612ca96fe9f36cea1` and Mesa base
`24a0a3bddf5588d7f087e8f40658ba5e17af6094`. This is one build for A12, not
separate XS and iPad Mesa copies.

```sh
MESA_JOBS=2 ./cascadia a12 build mesa
sudo python3 devices/a12/mesa/runtime.py install \
    devices/a12/build/mesa-stage --root /path/to/debian-root
```

The builder requires native Linux arm64, Meson/Ninja, Rust/bindgen, LLVM,
Clang, libclc, SPIR-V tools/LLVM translator, libdrm and Wayland development
packages plus Mesa's ordinary C/C++ dependencies. Compiler version selection
may be passed through the standard PATH/PKG_CONFIG_PATH environment. No host
packages or host graphics libraries are replaced by this script.

Default: EGL/GLES/GBM/Wayland. `A12_MESA_X11=1` requests an additional X11/GLX
build (X11 development dependencies required); **the X11 build is not yet
validated on the iPad**. Rusticl and Vulkan are not enabled by this recipe.
Changing these feature flags requires a separate source/build configuration;
the script does not silently reuse a build with different settings.

Install into a compatible ARM64 target distribution: a runtime linked on a
new Kali host is not automatically compatible with older Debian. The installer
checks the target loader and dependencies before creating its activation
wrapper, and leaves distro Mesa intact. A missing LLVM or GLIBC ABI must be
resolved by building against the target distribution, not copying arbitrary
host libc into it.

To include an already prepared stage in the normal Debian builder, set
`A12_INSTALL_GPU_MESA=1`. Otherwise that builder preserves its existing Mesa.
The GPU driver itself still starts automatically when its firmware/handoff
is present; the Mesa installation is a separate userspace operation.

Installed applications/sessions can select it with `cascadia-gpu COMMAND`.
No labwc-specific dependency exists. This recipe does not rewrite LightDM,
XFCE, Omarchy or the user's compositor/session settings. Selecting the
accelerated desktop session remains explicit until the combined build is
validated. `cascadia-gpu` sets matching EGL/GBM paths and removes forced
software rendering for that command only.

## Diagnostics and tests

iPad command/queue/event diagnostics default **off**; opt in with `agx.j171_render_diag=1`.
Errors, timeouts and firmware crash reporting remain available.
The root-readable GPU status and copied raw firmware crashlog are exposed
through sysfs. The powered MMIO fault probe remains a separate explicit option,
not part of routine diagnostics. No test shader or custom bring-up ioctl is
included in the integrated driver.

```sh
python3 devices/a12/tests/test-gpu-integration.py
python3 devices/a12/tests/test-device-separation.py
```

These are offline regression checks, not hardware GPU tests. External GLES,
compute and compositor tests should be run after boot, like BadApple's
`cl-test`/`gl-test` workflow; they are not part of kernel probe. The hardware
checks above used local test tools, not an automated public test suite.

Native display is separate: the iPad still uses simpledrm. Detailed reverse-
engineering journals and private captures remain local and are not required
to build this patch. Firmware/tickets/calibration and compiled artifacts must
not be committed.

Credits: BadApple/kieroid for the common G11 DRM/Mesa foundation and XS path;
dr1408's iPad bring-up for the separately validated J171/21B91 implementation.
