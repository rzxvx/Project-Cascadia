# P105 desktop: missing CPU OpenGL renderer

The desktop recipe installs Xorg and libGL through its dependencies, but not
Mesa's DRI renderer. On Alpine 3.24 armhf, Xorg's framebuffer screen reported:

```text
AIGLX error: dlopen of /usr/lib/dri/swrast_dri.so failed
AIGLX error: unable to load driver swrast
GLX: could not load software renderer
GLX: no usable GL providers found for screen 0
```

Install `mesa-dri-gallium` explicitly. The signed Alpine 3.24 armhf package
`26.1.6-r0` supplies `usr/lib/dri/swrast_dri.so`; `apk info -L` confirms the
path. It enables CPU OpenGL fallback, not SGX hardware acceleration. Xorg
loads the provider when the server starts, so an already running server is
not repaired just by adding the package.

## Physical iPad validation — 14 October 2026

The frozen RAM kernel at `1ff01fad8280632fa0a3f25ee9e4cd023a4a0be0` and
Alpine 3.24 armhf NFS chroot were used. The visible XFCE server stayed running
throughout. A separate authenticated, TCP-disabled Xvfb server used a 64x64
virtual screen, without opening the physical framebuffer. The same bounded
`glxinfo -B` probe was run before and after installing the renderer:

- Before: `Error: couldn't find RGB GLX visual or fbconfig`; no renderer or
  OpenGL version reported.
- After: `OpenGL renderer string: llvmpipe (LLVM 22.1.3, 128 bits)`, Mesa
  26.1.6, OpenGL 4.6 compatibility profile; `Accelerated: no`.
- Both `glxinfo` executions exited **zero**. Exit status alone is not a
  sufficient acceptance check; inspect the renderer and version output.
- `LIBGL_ALWAYS_SOFTWARE=1` restricted the client to software rendering;
  `LP_NUM_THREADS=1` bounded the llvmpipe worker pool. No benchmark was run.
- The temporary Xvfb processes were stopped; the original Xorg PID stayed
  unchanged. The device still had 265 MiB available RAM, 79% charge and
  31.3 degrees Celsius after the probe.

Only the dedicated NFS root received packages. A package-world backup and
guarded rollback were prepared. This is an equivalent live package repair,
not a fresh full-PR-head boot, a restart of the framebuffer Xorg server, or
qualification of desktop performance. The normal desktop recipe adds only
the renderer; Xvfb and mesa-utils were diagnostic prerequisites for this test.

Primary references: [Alpine package contents](https://pkgs.alpinelinux.org/contents?branch=v3.24&arch=armhf&name=mesa-dri-gallium&repo=main),
[Mesa environment variables](https://docs.mesa3d.org/envvars.html).
