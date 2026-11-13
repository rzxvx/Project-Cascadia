# A12 GPU: implementation, research and iPad port

This guide combines the shared BadApple G11 foundation with the iPad 8 port's
verified findings and Project Cascadia integration. It is a curated technical
record, not the full bring-up journal. Results below distinguish source analysis,
historical experiments and tests repeated on the integrated build.

**Upstream foundation:** [kieroid (kiki)'s BadApple GPU guide](https://github.com/kieroid/badapple/blob/main/docs/gpu.md)
is the primary reference for the XS implementation, G11 command encoding,
shader/compiler work and its graphics/Vulkan results. Read it alongside this
file: this document explains what we share, what changed for iPad, and what we
actually tested. BadApple's current results are not automatically iPad results.

## Scope and firmware versions

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
with completed GPU jobs and no firmware crash. Native H11P scanout and an
interactive G11 labwc desktop have since passed local hardware tests; see
[DISPLAY.md](DISPLAY.md) for normal-profile integration and its validation status.
This does not establish unlimited-session stability, X11/GLX or Vulkan support,
or hardware revalidation of the merged XR/XS paths.

## Architecture and startup

The GPU path has several independent layers:

1. **Boot firmware and memory:** package the selected GFX firmware with a
   matching ticket; iBoot supplies its segment ranges in the live Apple device
   tree. m1n1 validates and reserves these ranges before Linux can allocate them.
2. **Power and handoff:** preserve the selected board's power/performance data
   and enable the corresponding GPU/mailbox nodes. Shared T8020 hardware does
   not make every board's boot memory layout interchangeable.
3. **Firmware bootstrap:** the driver brings up RTKit, starts endpoints `0x20`
   and `0x21`, publishes versioned InitData and waits for acknowledgement.
   InitData includes channel descriptors, MMU configuration, HWData and the
   Region B/C runtime state. ACK proves initialization, not shader execution.
4. **DRM and memory:** the shared Asahi-compatible interface provides GEM
   buffers, GPU virtual address spaces, queues and synchronization. GPU and
   firmware virtual addresses are distinct even when they refer to one buffer.
5. **Jobs:** compute uses its own command/microsequence. Rendering requires
   parameter-manager resources, TA work and then 3D work. A working compute
   command does not validate the TA/3D ABI.
6. **Userspace and display:** patched G11 Mesa produces shader and rendering
   state. The iPad profile now uses guarded native H11P scanout; GPU rendering
   and display control remain separate driver capabilities.

G11's tile-store limitations are handled by BadApple's Mesa work: the G11 path
writes render targets to memory through its background/fragment programs rather
than assuming stock Mesa's tile-store path works. Correct kernel completion
alone cannot compensate for a missing G11 userspace patch.

## What is shared, and what was ported

| Layer | Shared foundation | iPad-specific work |
|---|---|---|
| DRM | BadApple driver, Asahi uAPI, GEM/VM and scheduling infrastructure | Versioned `agx_21b91_*` backend and command serializers |
| Firmware | G11 RTKit transport and channel/job concepts | 21B91 layouts, channel records, InitData and microsequences |
| Boot | Cascadia profile/build/USB infrastructure | J171 GFX reservation, live policy handoff and bounded RAM mapping |
| Mesa | One pinned BadApple G11 patch/build recipe | Target ABI installation checks and iPad validation, not a separate shader ISA fork |
| Display | BadApple H11P scanout infrastructure | Guarded J171 takeover, verified DMA-BUF sharing and SPMI/SMC backlight; see DISPLAY.md |

The production driver is in
[`linux/patches/linux-0020-agx.patch`](linux/patches/linux-0020-agx.patch).
The iPad boot changes are under [`m1n1/ipad8/`](m1n1/ipad8/), and shared Mesa
build/install tools are under [`mesa/`](mesa/). XR's GPU stays disabled; the
XS backend and its own m1n1 series remain separate from J171/21B91 handling.

### Comparison method

The port was not a global replacement of XS offsets. For each disputed field,
we compared four sides:

- XS native iOS kernel producer and its 16.6.1 firmware consumer;
- BadApple's Linux construction for that XS ABI;
- iPad native iPadOS kernel producer and its 17.1.1 firmware consumer;
- our Linux construction for the iPad ABI.

Kernel disassembly establishes what gets allocated and written; firmware
analysis establishes how it is consumed. Live iOS allocation captures helped
check address-space relationships. Linux traces then tested the hypothesis
against the actual submitted command and retirement state. Neither a plausible
field name nor a successful XS boot proves an iPad offset.

Here, **native** means the reference iOS/iPadOS implementation, not a requirement
to replay all of its policies. BadApple constructs a minimal Linux runtime; the
iPad port follows the same approach with version-specific producers/consumers.
A minimal bootstrap that passes a test is not a reconstruction of all native
HWData or a guarantee for every workload.

## Findings that made the iPad render path work

Offsets in this section describe **J171 / 21B91**, unless explicitly marked XS.
They are evidence for this backend, not portable G11 constants.

### 1. Parameter-manager capacity, units and both address aliases

The final resource setup uses an 8 MiB parameter heap: 64 active 128 KiB blocks
and 128 descriptor slots. A 16 KiB allocation granule, a 128 KiB heap block and
an address encoded in 4 KiB units are different quantities. Treating the native
allocation granule as `1` produced an invalid page count. The page list also
needed its own aligned storage rather than overlapping scene/control data.

Increasing capacity alone did **not** fix the failing clear. InitBuffer could
populate descriptors while active TA still faulted. The decisive missing wiring
was the parameter-table GPU alias:

| Table reference | XS / 16.6.1 | iPad / 21B91 |
|---|---|---|
| GPU virtual address in Region B | `+0x21c` | `+0x2c8` |
| Firmware virtual address in Region B | `+0x224` | `+0x2d0` |

Both fields must refer to the same owned allocation in their respective address
spaces. The iPad code initially supplied only the firmware alias, allowing
InitBuffer to populate the table while the firmware programmed a zero GPU base.
Publishing and validating both aliases eliminated that active-TA failure in the
hardware retest. This moved the failure downstream: TA/3D retired, but the target
buffer was still black.

### 2. Start3D scalar versus pointer

The iPad Start3D microsequence field at `+0x94` takes the **value** from command
`+0x4`, not the address of that field. The old rewrite supplied a firmware
pointer. The firmware's scalar comparison then selected a bypass path: it could
retire bookkeeping without issuing the useful 3D kick.

Correcting the producer, rather than forcibly clearing firmware-owned status,
produced the first verified red clear. The actual render-target BO changed and
`glReadPixels` matched. This established why “stamp retired” was not sufficient
evidence of rendering.

### 3. Event counters are not completion stamps

A repeated-render crash came from confusing the event-control completion count
with the job stamp. For iPad FinalizeTA, `+0x44` identifies the counter address
and `+0x4c` supplies its count. The port wrote the stamp there instead of the
single-command count `1`. The event node stayed linked after apparent retirement;
reusing its storage corrupted the next insertion.

The correction keeps counter and stamp semantics separate. Buffer reuse waits
for the expected stamp, queue retirement, matching event count and event/list
unlink state. XS implements the same logic at different field offsets.

### 4. Draining outbound channels is part of forward progress

An early sustained test passed 128 renders and then stopped consuming TA input.
The render-only path was missing event/log draining that the compute wait path
already performed. The exact saturated channel was not captured in that failed
run, so saturation was not directly measured there; the missing consumer was a
confirmed source defect.

The corrected path drains while waiting as well as on deferred notifications,
with serialized consumers. This avoids a worker waiting on a lock held by the
job waiter that itself needs outbound progress. A later 300-render experiment
passed pixel checks across ring wrap. Endpoint `0x42` is a request to inspect
outbound channels, not proof that a particular job completed.

### 5. BO contents distinguish rendering from readback faults

We checked two independent observations: the actual target GEM buffer before/
after completion, and the application's `glReadPixels` output. An unchanged BO
localized the original black output upstream of readback. Later DMA-BUF tests
checked the display-owned buffer itself as well as GL readback, validating the
import/mapping path rather than trusting only a fence.

### 6. Boot memory is part of the port too

The first cleaned integration failed in m1n1 before Linux. Comparing it with the
working experiment exposed a missing J171 identity-map bound: the bootloader's
usable `ram_size` must constrain this iPad mapping instead of the rounded
`mem_size_actual`. The restored iPad-only overlay avoids mapping protected upper
DRAM as ordinary cacheable RAM. This was separate from the render bugs; it did
not require changing XR/XS memory behavior or embedding more firmware in initramfs.

## Validation and limits

### Repeated on the built-in Cascadia integration

With the Omarchy NFS root and diagnostics disabled by default:

- RTKit startup and InitData ACK completed; the Asahi render node appeared.
- A 300×200 clear and triangle passed exact/defined-region pixel checks using
  the Apple A12 G11P renderer.
- A display-owned DMA-BUF passed both original-buffer and GL readback checks.
- A visible window was confirmed on the iPad. That presentation used a CPU copy
  into the existing Wayland session: it was **not** zero-copy native scanout.
- A separate temporary labwc session used the shared Mesa runtime for GPU
  compositing. The observation recorded 397 completed renders, 794 consumed
  events and no reported firmware crash. This was not an unlimited-duration test.

The separate experimental phase also exercised compute, VM/context isolation,
remapping and repeated render/ring-wrap cases. Those historical results should
not be described as a full rerun of every test after production cleanup.

The current iPad backend is serial, with bounded waits for each job but no
lifetime job cap. It waits for TA retirement
before submitting 3D; it does not yet implement BadApple's newer firmware-side
polling-barrier overlap. It also does not inherit BadApple's XS Vulkan/CTS results.
Native display/backlight results are covered in [DISPLAY.md](DISPLAY.md);
audio, broader workload coverage and a validated Vulkan/Rusticl recipe remain
separate work. The tested root lacked Xwayland, so the working
Wayland session does not establish X11 application support.

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
required. iPad keeps serial rendering and at most2048x2160 targets, but no
4096-job lifetime stop. Hardware endurance of that cap removal is pending; this
is not a claim of unrestricted desktop stability or Vulkan support. XS rendering limits and replacing/single-page VM binds
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

Diagnose the **first failing stage**, not just the last visible symptom:

| Observation | Next evidence to inspect |
|---|---|
| No InitData ACK | Firmware identity, reserved segments, RTKit and InitData layout |
| InitBuffer succeeds, active TA stalls | PM aliases/descriptors, allocation units, TA register inputs and fault evidence |
| Stamps retire, BO unchanged | Actual kick/bypass conditions and render state; retirement is not a pixel test |
| BO correct, GL readback wrong | Mesa readback, mappings and cache coherency |
| First frame works, repeats fail | Event count/unlink, queue reuse and outbound consumers |

Do not infer that the last suspected field caused a fault without a controlled
retest. A larger heap is not proof that pointers are correct; no crashlog is not
proof of completion. Keep firmware build, driver version and test result paired.


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

## Credits and references

- **[kieroid / kiki](https://github.com/kieroid),
  [BadApple](https://github.com/kieroid/badapple):** A12/G11 reverse engineering,
  the shared DRM/Asahi-uAPI foundation, XS firmware/power/queue implementation,
  and G11 Mesa compiler, Gallium and Vulkan work. The
  [upstream GPU guide](https://github.com/kieroid/badapple/blob/main/docs/gpu.md)
  explains that implementation and its evolving results in more depth.
- **dr1408's iPad bring-up:** J171/21B91 kernel/firmware and live-capture analysis,
  versioned handoff/InitData/command adaptation, the iPad fixes described above,
  profile/build integration and iPad hardware validation.
- **Project Cascadia, HoolockLinux, Asahi Linux and Mesa contributors:** the
  boot, kernel, userspace and tooling foundations on which this work depends.

The shared Mesa snapshot is the
[BadApple patch at the pinned commit](https://github.com/kieroid/badapple/blob/61bd066feed4406828f17fc612ca96fe9f36cea1/patches/mesa-0001-asahi-g11.patch).
The upstream guide tracks a moving branch; new upstream capabilities require
review and target validation before being claimed here. Existing source licenses
and attribution remain applicable. This guide summarizes the shared design;
it does not replace upstream documentation or redistribute private captures.

## Sustained desktop submissions

An inherited experiment-only4096-render lifetime budget froze the desktop after
exactly4096 completions, even though the firmware had not crashed. That cap and
its `j171_render_jobs` parameter have been removed from the J171 backend. Queue
rings are reused only after stamp, cursor and EVC-link retirement checks. Stamp
validity is separate from its u32 value, so zero at natural wrap is not treated
as an error. Per-render input logging is debug-only.

`tests/test-gpu-sustained-render.py` compiles the shipped queue/stamp C against
owned host fixtures and checks20000 jobs, ring reuse, wrap and failure guards.
These host tests pass; hardware endurance beyond4096 is pending the new boot.
No XS scheduling/stamp policy was changed by this fix.
