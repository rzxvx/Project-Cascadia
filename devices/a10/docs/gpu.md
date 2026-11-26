# GPU (Apple A10 / T8010, "G9P") -- plan and what is known

Status 2026-11-25: **G0-G6 done (our own shaders and programs, the whole
screen; the host owns the GPU's power state, P+0xd34 = 0xd3c = 0); G7 done
in its core: the powervr driver's Apple backend runs the GPU through the
powervr UAPI -- a user program creates VMs, BOs, contexts, free lists and
HWRT datasets and submits compute and render jobs with syncobjs (G5's
store, G6's clear + triangle), with fault recovery and idle power-off
("G7 progress" .. "G7 step 6"); the GPU work runs under Linux**
(`GPU_BACKEND=linux`, see "GPU work under Linux"): core ID 0x011b0602 /
revision 0x000d0a00, the GFX firmware boots and takes init data with the
real RGX, stable. **The first job ran on the GPU** (G4 run 29-31): the
firmware dispatched a terminate-only compute job, the CDM read its control
stream through our page tables, both completion stamps were written; from
a cold GPU powered up by the firmware itself, with no MMU fault (runs 43, 45).
G5: our offline-compiled `store_const` writes a buffer through a DMA-bound
binding (runs 97-100). G6: a TA + 3D pass clears a 64x64 target through the
background object, and the iPadOS triangle renders byte for byte as on
iPadOS ("G6 step 4") -- since "G6 legalization" from our own shaders,
programs and state, with nothing taken from the captures. Next: the
screen, then G7 (the kernel driver).
Under m1n1 the core's registers hang the bus (not explained). This page is what the iPad 7's own ADT, kernelcache and firmware
say, what the reference projects say, and the plan in checkable steps, in the
style of the iPad mini 1's SGX work and the A13's AGX work
(`devices/a13/docs/research/gpu.md`).

**Approach:** Cascadia's A10 GPU driver is its own.
Every value our code uses is derived from our own firmware build (22H355) and
kernelcache or measured on our iPad.

## The hardware in one paragraph

The A10's GPU is a **PowerVR Rogue core** (Imagination's fixed-function side:
Rogue MMU, CDM / TA / 3D data masters, PDS) with an **Apple shader core** (not
Rogue's USC ISA) and **no Imagination firmware**: Apple runs its own **GFX
firmware** (RTKit, ARMv7) on a small ARM coprocessor, **gfx-kf**, inside the GPU
block. The host talks to that firmware over the RTKit mailbox and shared rings,
like Asahi's AGX, not like Imagination's KCCB. A GPU-side power and config
block, **Fender**, sits between PMGR and the Rogue core: PMGR powers the GFX
domain, Fender powers the core on request.

## What is known

### ADT (`build/adt/j171.txt`, the iPad's own, 2026-11-22)

| | |
|---|---|
| `sgx` | `compatible = gpu,t8010`, `device_type = sgx`. **reg 0: 0x201000000 +0x100000** (Rogue registers, "RGX"); **reg 1: 0x201d00000 +0x20000** (Fender, shared with gfx-kf). IRQs 185, 186, 187, 188, 193 (AIC 0x20e100000). `power-gates` / `clock-gates` [148, 150], `clock-ids` [265]. `has-kf = 1`, `meta-sw-interrupt` (a leftover of Imagination's META firmware core; Apple does not use META here). `opengl-standard = 768`, `metal-standard = 256`. |
| `gfx-kf` | `compatible = iop-gfx,t8010`, `role = GFX`, `iop-version = 1`. **reg 0: 0x201d20000 +0x10000** (the A7-style IOP: remap window, CPU control, mailbox at +0x4000), reg 1: 0x201d00000 +0x20000 (Fender again). IRQs 190, 189, 192, 191. `power-gates` / `clock-gates` [149], `clock-ids` [265], `force-timebase-sync`. |
| `iop-gfx-nub` | `compatible = iop-nub,rtbuddy`, `firmware-name = GFX`, `power-managed`. **No `segment-ranges`, no `pre-loaded`**: iBoot does not load this firmware (unlike the A13's `gfx-asc`); the kernel does. |
| DART | **none for the GPU.** The coprocessor sees physical memory through its own remap window; the Rogue core has its own MMU (page tables in memory). |
| PMGR | One real power domain, **`GFX` (id 124), PS register 0x20e080328** (pmgr reg 0 + ps-regs[4].offset 0x80300 + psidx 5 * 8). `GFX-SGX` 148, `GFX-KF` 149, `GFX-BUSY` 150 are virtual (`no_ps`); 148/149 have parent 124. PMGR also has `gfx-tvm = 1`, `voltage-states9` (GPU) and `voltage-states9-sram`, and `GFX_TVM_TEMP_0..3` sensors. |
| perf states (MHz / mV, SRAM mV) | 0 / 450 (600), 378 / 628 (700), 440 / 659 (728), 630 / 728 (809), 756 / 787 (868), 852 / 843 (934), **1008 / 950** (1046); `gpu-num-perf-states = 6`. |
| power controller | `gpu-perf-*`, `gpu-pwr-*` gains and filter constants, `gpu-device-max-power = 3348` mW, `gfx-qos`: the firmware's DVFS parameters (init data). |

### Kernel side (kernelcache 22H355, `build/fwsrc/22H355__iPad7,11/`)

Old-style prelinked kernelcache (not a fileset; `ipsw kernel extract` refuses
it -- kexts located through `__PRELINK_INFO`):

| kext | version | role |
|---|---|---|
| `com.apple.AGXG9P` | 328.5 | the GPU driver (`AGXAccelerator`, Fender maps "FenderCfg", "GFenderCfg Regs", "FenderScratchRam", "Dynamic Power Control Regs", power zones, "GPU uses 16K page size but CPU does not") |
| `com.apple.AGXFirmwareKextG9PRTBuddy` | | **holds the GFX firmware** (`requestFirmwareBinary`, `getBootConfigOffset`, `getRuntimeBinSize`, `getBuild` = "Release") |
| `com.apple.AGXFirmwareKextRTBuddy` | | RTBuddy glue |
| `com.apple.iokit.IOGPUFamily` | 104.6.3 | |
| `com.apple.driver.AppleA7IOP`, `AppleA7IOP-AKFWrap-v2`, `RTBuddy` | | the coprocessor (A7 IOP v2) and RTKit transport |

### Firmware

`AGXFirmwareKextG9PRTBuddy`'s `__TEXT,__const` (kext header at kernelcache
file offset 0xe4ae80, `__const` at +0x9f3) is the build string `Release`
followed by the image. **`bash tools/gpu/gfx-fw.sh`** extracts it (offset and
SHA-256 pinned) into `build/firmware/apple/gfx-t8010-22H355.bin`:

- **0xa4000 bytes, ARMv7 (A32 vector table at 0, then Thumb-2), load base 0.**
  `RTKit-2784.140.9.release`.
- Followed in the kext by a 0x60-byte `fwsg` trailer (version 1, image size
  0xa4000, 2 segments): **`__TEXT` 0..0x31c48; `__DATA` at 0x33000, 0x71000
  bytes in the file, 0xa4c84 in memory** (ends at 0xd7c84).
- **RTKit boot-argument table at 0x81000** (tag byte-reversed, u32 size,
  value, packed), as on the A13: `STKG` 0xaff, **`RTSZ` 0xd8000** (runtime
  region: image + zeroed BSS/heap), `DSRL`, `DSVS`, `DSCL`, `CBES`, `DSZS`,
  `DSZL`, `TUNS`, `TUNZ`, `RTTO` 0, `McRA` 0x4000, `SOC_` / `SOCR` 0xffffffff
  (to fill: 0x8010 / revision), `CpAd`, `WrAd`, `DEVf` 0, `pFLG` 2, `NClk`
  0x16e3600 (24 MHz).
- Strings: Rogue register names (`RGX_CR_MMU_ENTRY`, `RGX_CR_MMU_ENTRY_STATUS`),
  Fender power ("GPU is not powered on after power kick!", "Fender power
  transition"), `RTK_POWER_SLEEP` / `NAP`, "Unable to grab init data from host",
  "Unable to register init data at PA_%llX", data-master callbacks (TA, 3D).

The firmware is loaded by the host, not by iBoot, so **the build is our
choice** (no signature check is expected on this path, to be confirmed in G2).
Offsets and init-data layouts are per build; we use 22H355 (the iPad's own
iPadOS) throughout.

### Shader ISA: offline reference compiler

Xcode's Metal Toolchain (already installed for the A13) targets the A10:
`applegpu-nt -available-archs ios 16.4` lists **`applegpu_g9p`**. With the A13's
`devices/a13/tools/gpu/isa/g5-kernels.metal` (output in `build/gpu/isa/`):

| kernel | applegpu_g9p |
|---|---|
| `empty` | `0140 0100 0000` then `00c0` padding, **ends with `0080`** (0x42 bytes) |
| `store_const` | 0xc2 bytes, same `00c0` padding and `0080` end |

So G9P's shader code is not the A13's G12/G13 encoding (G12 ends with `0801`,
G13 with `8800`); whether any of it parses as Rogue USC code (Mesa's `pco`) is
to check, the `00c0` padding / `0080` end already look unlike Rogue's. A new
ISA to decode, with an offline compiler as the oracle -- the same method as on
the A13.

### References

- **Upstream** `drivers/gpu/drm/imagination` (Rogue registers, MMU format) and
  Mesa `src/imagination` (`csbgen`, PDS encodings, `device_info`): Rogue
  reference. **Mesa's PowerVR README declines LLM-generated contributions**,
  like Asahi's m1n1: our Mesa work stays in Cascadia (as `mesa/mesa.patch`
  did for the SGX), never proposed upstream.
- **Asahi** m1n1 proxyclient (`fw/agx`, `hw/akf.py` -- the A7-A10 IOP
  mailbox, `rtkit.py`): our m1n1 tree (MIT), allowed for Cascadia work
  (`cascadia-m1n1-ai-policy`).

## Plan

Each step ends in something checkable. Percentages: chance the step works as
planned. Steps G1-G6 run from the Mac through m1n1's proxy (`./cascadia a10
boot proxy`, one DFU boot per run), as on the A13; G7 on is Linux.

**G0 -- reconnaissance. DONE 2026-11-23.** This page.

**G1 -- power and core ID. DONE 2026-11-23.** `tools/gpu/gpu-id.py`: PMGR
GFX on, Fender +0x10 = 7, +0x2058 = 1 (held while off), +0x1000 = 0x11 ->
state 1, then RGX +0x18 / +0x20 = **0x011b0602 / 0x000d0a00** -- under
Linux (`GPU_BACKEND=linux`). Under m1n1 the same read hangs the bus (13
runs, "G1 results", "G1 answered from Linux").

**G1.5 -- static: how iOS configures Fender. DONE 2026-11-23** (see "G1.5
results" and runs 4-7): the firmware's and the kext's power-up, the iBSS's
GPU DVFM setup and the fabric bridges are decoded and replayed; the core
still hangs on RGX reads from a cold m1n1. Next: G2. In the
AGXG9P kext: the Fender regions it maps (offsets inside sgx reg 1 and
elsewhere) and what it writes at probe / power-up; in the firmware: the reset
sequence and the perf-state wait, and which I/O region each touches. Check: a
list of register writes that we can replay from the proxy.

**G2 -- the firmware runs on gfx-kf. DONE 2026-11-23** ("G2: the recipe", "G2 results"; `tools/gpu/gfx-boot.py`). Proxy script: a physically
contiguous, 16 KiB-aligned 0xd8000 buffer (RTSZ) with the image at 0 and the
rest zero; boot-argument table filled (`SOC_` 0x8010, `SOCR`, `WrAd`...);
the coprocessor's remap window, mailbox, timebase sync (`force-timebase-sync`)
and run bit as `AppleA7IOP` / `AppleA7IOP-AKFWrap-v2` and the AGX kexts in our
kernelcache program them (to read out: which registers in gfx-kf reg 0 and
in Fender); m1n1's `hw/akf.py` for the mailbox (its V2 register map has two
swapped entries to fix). Check: RTKit HELLO, the endpoint map, the crashlog
buffer request, power acks.

**G3 -- init data, the firmware idle and stable. DONE 2026-11-23** ("G3 results"; `tools/gpu/gfx-init.py`; the core's ID came in G1 under Linux). Find what the
22H355 firmware wants on EP 0x20 by reading it (Ghidra on the 0xa4000 image,
load base 0) and the AGXG9P kext; crash-driven iteration (the firmware names
the field it misses). Check: "ready", the firmware's own tasks running, no
crashlog, Stats / log channels moving; then (with Fender configured by the
firmware or by G1.5's replay) CORE_ID / CORE_REVISION at RGX +0x18 / +0x20. Open: can iPadOS 18.7.9 (jailbroken,
rootless) give us a live reference (kernel read of the init data) the way
Dopamine's kmem did on the A13? palera1n does not hand out kernel r/w like
libjailbreak; to check.

**G4 -- the core under the firmware, an empty compute job. DONE
2026-11-23** ("G4 progress", runs 15-31): Rogue page tables for one GPU
context, the firmware's queue for compute, a terminate-only CDM stream.
Check: the completion stamp moves (done, both stamps), no firmware crash
(done), no MMU fault (done, runs 43 and 45, once the firmware's own cold
power-up runs).

**G5 -- a compute store. DONE 2026-11-23** ("G5 DONE": runs 97-100; the
tiling ranges P+0x0..0xc were the blocker). The `store_const` kernel compiled offline
for `applegpu_g9p`, its PDS program (Rogue PDS encodings), the buffer bound.
Check: the value lands in memory; moving the buffer moves the write.

**G6 -- a clear, then the first triangle (30 %).** TA + 3D with no draws
(background object writes a colour into the render target), then a VDM stream
with vertex and fragment shaders. Check: the pixels in memory, then on the
screen. **DONE in memory (2026-11-23 / 24): the clear ("G6 clear DONE")
and the triangle ("G6 step 4"), byte for byte as iPadOS draws it, from our
own shaders, PDS programs and state ("G6 legalization").** The screen is
next.

**G7 -- kernel driver (35 %).** In our kernel, in C: `apple-rtkit` with a
T8010 A7-IOP mailbox backend, GEM shmem, `drm_gpuvm`, `drm_sched`. **The UAPI is upstream
powervr's** (`drivers/gpu/drm/imagination`)
with a backend for Apple's firmware -- Mesa's pvr winsys unchanged, the
kernel translating powervr's geometry / fragment / compute jobs, free lists
and HWRT datasets into the TA / 3D / CL commands of G4-G6. No GPU-side
cost: the translation is per job on the CPU (register values copied into
the firmware's blocks); the GPU runs the same work either way. First
step, needed whatever the UAPI: the kernel boots the firmware itself
(RTKit on the A7-IOP mailbox, init data, rings, GPU page tables, the host
owning the power state) and runs G5's / G6's jobs without the Python
harness. Check: a user program allocates, submits, waits. **DONE
2026-11-25** (steps 1-6 below): `tools/gpu/pvr-uapi-test` allocates, submits
compute and render jobs through the powervr UAPI and waits on syncobjs,
results checked. Left for later: the items under "G7 step 6" / "Open".

**G8 -- Mesa and the desktop (30 %).** Our G9P shader backend (decoded from
`applegpu_g9p` output), Rogue state packing from upstream Mesa, in a Gallium
driver of its own (decided 2026-11-26, "G8" below: less CPU per frame and
fewer required features than Vulkan + Zink). Yardstick:
labwc and SuperTux on the GPU instead of pixman / llvmpipe. **First step
done 2026-11-26: SuperTux on the GPU through a 2D compute compositor**
(g9p2d, "2D on the GPU"), as on the A5 before its GL driver.

## G7 progress (2026-11-25): the kernel boots the firmware and runs the G5 / G6 jobs

**Layout: a backend inside upstream
`drivers/gpu/drm/imagination`**, so the powervr UAPI, GEM, `drm_gpuvm` +
`pvr_mmu` (the same Rogue MMU format) and `drm_sched` stay upstream's and
Mesa's pvr winsys needs no change. The Apple parts are new files; upstream
files get small hooks only. In the tree:

| where | what |
|---|---|
| `kernel/patches/0003-soc-apple-mailbox-...` | `apple,t8010-a7iop-mailbox`: a variant of the kernel's Apple mailbox driver for the A7-style IOP v2 (single 64-bit messages, endpoint in bits 63:56, payload 55:0; inbox +0x4008 status / +0x4010 send, outbox +0x4020 / +0x4038; at start: +0x4000 / +0xc00 = 0x1111, +0x4004 / +0xc04 = 0x1 then 0x1000, outbox status bit 0 = receive interrupt; polled send). The kernel's `apple-rtkit` runs over it unchanged (protocol 12). |
| `kernel/patches/0004-arm64-dts-...` | `gfx_mbox` (gfx-kf 0x201d20000, IRQs 190 / 189 / **192 = recv-not-empty** / 191, `ps_gfx`) and `gpu` (`apple,t8010-gpu`: reg `rgx`, `fender`, `iop`; `mboxes`; `firmware-name`), enabled in `t8010-ipad7.dtsi` |
| `kernel/patches/0005-drm-imagination-...` | the hooks: `CONFIG_DRM_POWERVR_APPLE`, the compatible, probe / remove and runtime PM handed to the backend, `pvr_device.apple` |
| `kernel/overlay/drivers/gpu/drm/imagination/pvr_apple.[ch]` | the firmware layer: GFX power (`ps_gfx` through runtime PM), the image into a fresh 0xd8000 runtime region, RTKit (timebase, both remap windows, Fender run bit, `apple_rtkit_wake`, EP 0x20 / 0x21), the init data, the host-held perf state, the firmware -> host rings (drained on EP 0x20 type 0x42), kicks on EP 0x21 |
| `kernel/overlay/.../pvr_apple_test.c` | the self-tests (debugfs `pvr-apple/selftest`), the G5 / G6 jobs in C |
| `tools/gpu/g9p-selftest.py`, `g9pshaders.py` | our Metal source (`tri_vs`, `tri_fs`, `bg_fs`, `store_const`) through the offline compiler into `build/firmware/apple/g9p-selftest.bin` (compiler output, not in git); `gfx-init.py` takes `OWN_SRC` from `g9pshaders.py` now |
| `./cascadia a10 modules [gpu]` (`tools/modules-sync.sh`) | the kernel's modules + the GFX firmware and the shader file onto the iPad's root; `gpu`: rebuild `powervr.ko` from the overlay in the kernel volume and reload it, no DFU boot |

`build-kernel.sh` now copies `kernel/overlay/` over the tree after the
patches and builds the modules (`build/payload/modules.tar.gz`); the
fragment makes powervr a module (`CONFIG_DRM_POWERVR=m`,
`CONFIG_DRM_POWERVR_APPLE=y`, `CONFIG_MODULE_UNLOAD=y`, no module
compression: busybox modprobe). Runs used `EXTRA_BOOTARGS="mem=2944M
cma=64M"` (the default 16 MiB CMA is untested with the 4 MiB heap).

The backend is the harness, word for word: the same heap (4 MiB at firmware
VA 0x80400000, regions and offsets as `gfx-init.py`'s), the same init data
(the harness defaults + the "base" arguments of G4 runs 43-45 + `--power
POWER_STATE=6 P+0xd34=0 P+0xd3c=0`), the same jobs (`--job --store`; `--job
--kick-ta --pm --own --tri --clear --kick-3d --va0`), one cold firmware boot
per test (the GFX domain is switched off in between; genpd shows `gfx` off).
**The AP touches RGX only through `pvr_apple_rgx_read()`**, which refuses
unless the host holds the power state (P+0xd34 = 0xd3c = 0) and Fender
still reports it -- the firmware never powers the core down then.

    modprobe powervr                     # autoloads at boot too; probe only maps, nothing is powered
    mount -t debugfs none /sys/kernel/debug
    echo store  > /sys/kernel/debug/pvr-apple/selftest    # G5 ("store N": N jobs; "storeq N": N queued at once)
    echo render > /sys/kernel/debug/pvr-apple/selftest    # G6: clear + triangle, 64x64 ("render N")
    cat /sys/kernel/debug/pvr-apple/selftest
    echo stop   > /sys/kernel/debug/pvr-apple/selftest    # firmware and GFX domain off

(This first version booted the firmware cold for every test; since "G7
step 1" it stays up.)

**Results (2026-11-25, interrupt-driven mailbox, `store render store
render` in a row):** every run PASS --

    store: PASS -- STAMP+0 0x1 (1), +0x10 0xc0de (0xc0de); output cafe0000 abadbeef abadbeef abadbeef
    render: PASS -- TA STAMP+0x20 0x1 +0x30 0x7a, 3D +0x40 0x1 +0x50 0x3d; pixels: 3072 background
            ff4080bf, 1024 triangle ffff0000, 0 untouched, 0 other; px(0,0) ff4080bf px(0,63) ffff0000

with RGX afterwards (read under the gate): MMU fault status 0xe150 = 0, CDM
0x4a0 = 1, PDS / USC base 0x610 / 0x4008 = the code base, the tiling range at
0x12e0, JONES_IDLE 0x8328 = 0x3f7f. The firmware takes the init data in
~0.2 s (A+0x4 / +0x10 / +0x14 = 1); the job events ring shows one 0x1 per
completed command and no 0x4 (fault).

What it took, beyond porting:

- **The mailbox's receive interrupt is the third of gfx-kf's four, 192**
  (as SmartIO's IOP: its third, 213). With 191 RTKit timed out (-ETIME); a
  polled run (`mbox_poll=1`, kept as a module parameter) proved the rest.
- **FinalizeCompute's full path is needed when the host holds the power
  state.** With the "already finished" fast path (the harness's, G4 run
  29) and P+0xd34 = 0 the store job completed (both stamps, CDM done, no
  fault) but the buffer stayed 0xabadbeef; with the firmware owning power
  (`store-fw`, G5's way) the same job wrote 0xcafe0000. Probes
  (`store_probe=1..4`, unmapped offsets): the kernel's and the DMA task's
  PDS code and the USC code all fault when unmapped -- the USC task runs --
  but an unmapped *buffer* raised no fault. **USC stores sit in a GPU cache
  and reach the MMU only when it is flushed**: the firmware's flush before
  each idle power-off (fw 0x17cb0) wrote them out when it owned power; with
  the host holding it nothing did. FinalizeCompute's full path (CDM stop,
  RGX 0x11f00 / 0x688 / 0x1720 / 0xe230) flushes: then the store lands and
  the unmapped buffer faults (0xe154 = 0x50000, VA 0x5000000). Render was
  never affected (the PBE writes the pixels, not a USC store). Now the
  default (`cp_fast=0`).
- Ruled out on the way: the perf state (1, 3, 6), P+0xd34 / +0xd3c mixed
  (either one 1 without the host's power state: no job runs; with it: no
  store), Fender's reset armed before the host's state request (+0x2058 = 1,
  also +0x10 = 7), building the job before the init message. The RAM
  stand-in for RGX (`rgx_standin=1`, debugfs `rgx-standin`, as the harness's
  `P+0x8c0=%R`) shows the same RGX writes either way but one:
  **RGX+0x6330 = 1** only when the firmware powers the core (not in the
  public headers; reads back 0; writing it from the host changed nothing).
  What it does: to check.
- Fender is outside the GFX domain: +0x10 written once kept its value
  across GFX power cycles.

### G7 step 1 (2026-11-25): one firmware boot, many jobs, fences

The firmware now stays up: booted on the first self-test, stopped by
`stop` or module removal. `pvr_apple_job.c` adds:

- **a pool** for firmware objects: the heap's first MiB keeps the static
  structures (X, P, T, B, ..., the rings), the other 3 MiB are handed out
  in 4 KiB units (the firmware's VA window 2 is only 8 MiB, so firmware
  objects must stay compact; GPU memory is not mapped for the firmware at
  all and lives elsewhere);
- **queues** (`pvr_apple_queue_create(a, group, dm)`: queue object, state,
  command ring, scheduling entries, status in one 32 KiB block) and **jobs**
  built from raw register blocks (`pvr_apple_job_cp / _ta / _3d` with the
  Start ops' A / B blocks, `pvr_apple_hwrt` for the free-list descriptor,
  the PM slot and the TA's large object) -- the shape the UAPI translation
  will fill;
- **submission**: command into the queue's command ring, a work-ring entry
  {queue, data master, write index}, the channel's host write index, the
  kick; **completion**: each command's completion record writes the job's
  sequence number into a word of its own; on every EP 0x20 type 0x42 (and
  a 10 ms fallback) finished jobs signal their `dma_fence`.

The self-tests run on that (`echo "store 600"`, `"storeq 64"`, `"render
50"`, `stop`): **600 store jobs in a row (the rings wrapped twice), 64 store
jobs queued at once, 50 renders, all on one firmware boot, all PASS**;
about 0.2-0.3 ms per store job, 0.8 ms per 64x64 render (TA + 3D, each
waited for), 16 ms for 64 queued stores. Stop / start and module reloads
work; the GFX domain goes off.

What the firmware would not take (each checked on the iPad):

- **Two commands of one queue in the firmware at once** crash it: "DAG: Non
  Sequential Stamp Updates seen entryIdx 0x0 roots.dag 0x1 stampIdx 0x0
  stampValue N+1 channel <queue object> channelRingCommandIndex N" -- it
  takes the queue's second work-ring entry before it has checked the
  first. Neither a completion word per job nor shared CMD+0x298 / +0x29c
  words changed it. So a queue holds **one command in the firmware**; the
  host keeps the rest and pushes the next on completion (`q->inflight`).
  Whether one ring entry may cover several commands, or the host should
  write the queue state, is to find.
- **Only one queue per data master is served**: a second compute queue --
  in group 1 (channel 5) or a second queue object on channel 2 -- never
  ran its command, and that stuck command held up compute for every
  queue. What registers a queue with the firmware (an id in the queue
  object? the queue table at fw 0xa4e90) is to find; until then one
  firmware queue per data master, the host multiplexing contexts on it.
- **The work-ring entry's write index wraps with the command ring**
  (state +0x50 = 256): an index of 0x100 left the firmware at read 0xff for
  good (job 256 ran, its completion never came).

### G7 step 2 (2026-11-25): upstream's VM / MMU code, several VMs

- **The kernel's VM context is upstream's `pvr_vm` / `pvr_mmu`** (backend
  sets `virtual_address_space_bits = 40`; 16 KiB CPU pages give **16 KiB GPU
  pages**). The self-tests' memory is now shmem GEM objects mapped through
  it (`test_pvr_vm=1`, default); the PM free list is built from their
  scattered pages. store, storeq, render: PASS.
- **The PDS / USC execution bases are the UAPI's heaps**: P+0x10 =
  0xDA_0000_0000, P+0x18 = 0xE0_0000_0000 (the test's code region mapped
  there). PASS.
- **MMU invalidation** (fw 0x19f0e, read with Ghidra): at each dispatch the
  firmware XORs the context's generation (T+0x7e8) with its copy (X+0x198);
  the changed bits pick the depth from the table at fw 0x2fee0: < 0x1000 ->
  RGX+0xe138 = 0x1 (PT), < 0x400000 -> 0x2 (PD), else 0x4 (PC), | context
  << 3. `pvr_mmu_flush_exec` (patch 0005 hook) adds 0x400000: `remap` (one VA
  mapped in turn onto two objects, 100 times) PASS; + 1 left a stale
  directory entry (MMU fault).
- **Further hardware MMU contexts do not work (yet); one context serves
  every VM.** fw 0x19d18 loads context 0 and every context named by the
  masks T+0x7f0..0x80c (bit + 32 k, catalog from Y[id]) into RGX+0xe140 /
  +0xe148 -- at init and after a recovery power cycle (fw 0x249b2) only.
  Slots 1..31 with catalog pages of ours, the context in StartCompute
  +0x14 (-> RGX+0xf07a, CONTEXT_MAPPING0's CDM byte): RGX+0xe148 for
  context 1 read back right, yet every job on context 1 faulted (CDM,
  context 1), even with the kernel's own VM mirrored there and full
  invalidations (RGX+0xe138 = 0x7 | ctx << 3) from the host; context 0 on
  such a page of ours (`ctx0_slot`, a mirror of the kernel VM's root) works.
  What a context > 0 needs is to find. **The design now**: context 0's
  catalog is a page of the host's holding the root table of the *active*
  VM; each job names its VM; the host switches the active VM (root copied,
  generation moved: a full invalidation at the next dispatch) only while no
  job is in the firmware. Jobs of one VM still run on TA / 3D / CP at once.
- **The fence page in every VM.** FinalizeCompute's full path (and the
  flush before a power-off) writes to the GPU VA in X+0x4 (RGX+0xe240) in the
  context running then; with that page only in the kernel's VM a second
  VM's job ran (its store landed) but never completed. The backend maps
  one page at the end of the general heap (0x9F_FFFF_C000) into every VM
  (`pvr_apple_vm_map_fence`); the heap size reported to userspace must
  leave it out (step 3).
- `vm2`: a user VM (`pvr_vm_create_context(..., true)`, heap-checked) with
  the test's objects in the general heap and at the code bases, its own
  object at one VA, the kernel's VM another one there; store jobs of the
  two VMs in turn, 100 times: **PASS**, each store in its own VM's object.
  With store, storeq 64, remap, render 10: all PASS.

### G7 step 3 (2026-11-25): the device on the powervr UAPI

The driver registers its DRM device (`/dev/dri/renderD128`, name
`powervr`); the firmware comes up with the first open (`pvr_apple_start`:
the UAPI's PDS / USC bases, the kernel's VM behind context 0, one queue per
data master; restarted after a crash). The self-tests run on that same
firmware now.

- **GPU id 13.10.6.2**, read when the firmware starts, under the host-held
  power: CORE_REVISION 0x000d0a00 (the older layout: major 13, minor 10)
  and CORE_ID 0x011b0602's config (N 6, C 2), as upstream's fallback
  decodes them. No other RGX offset is read (0x24, upstream's PBVNC high
  half, untried).
- Features the queries need: 40-bit VA and physical bus, 32x32 tiles
  (measured in G6); `num_clusters` 6, `isp_max_tiles_in_flight` 4,
  `max_partitions` 4, `common_store_size_in_dwords` 16384 are to check
  (they only feed RUNTIME_INFO's common-store numbers, which our shader
  core will not use as Rogue's).
- Heaps: upstream's table; the general heap reported 16 KiB short (the
  fence page at its end), and VM_MAP / VM_UNMAP refuse that range.
- Patch 0005 adds: the first-open start, every user VM mapping the fence
  page and leaving context 0 on release, no Imagination debugfs, and
  CREATE_CONTEXT / CREATE_FREE_LIST / CREATE_HWRT_DATASET / SUBMIT_JOBS
  answering -EOPNOTSUPP until the backend has them (steps 4 / 5).
- `tools/gpu/pvr-uapi-test.c` (`sh tools/gpu/pvr-uapi-test.sh run`): from
  userspace, ioctls only -- GPU_INFO (13.10.6.2, 2 phantoms), RUNTIME_INFO,
  HEAP_INFO (6 heaps, 16 KiB pages), CREATE_VM_CONTEXT, CREATE_BO, mmap
  with CPU write / read back, VM_MAP / VM_UNMAP in the general heap, VM_MAP
  of the fence page refused: **PASS**; the self-tests (store, storeq,
  remap, vm2, render) still PASS on the shared firmware.

### G7 step 4 (2026-11-25): compute jobs through the UAPI

`pvr_apple_uapi.c`: CREATE_CONTEXT / DESTROY_CONTEXT (compute) and
SUBMIT_JOBS (compute) on the backend. A context keeps its VM (a reference)
and its static state (upstream's stream format,
`pvr_static_compute_context_state_stream`); a job's command stream
(`pvr_cmd_compute_stream`) is decoded by upstream's `pvr_stream_process`
and translated into StartCompute's blocks:

| UAPI value | StartCompute | register |
|---|---|---|
| cmd `cdm_ctrl_stream_base` | CA[4..5] | 0x480 (stream) |
| cmd `cdm_context_state_base_addr` (else the kernel's) | CA[0..1] | 0x498 |
| ctx `cdmreg_cdm_terminate_pds` | CA[2..3] | 0x4b8 |
| ctx `cdmreg_cdm_terminate_pds1` | CDR[0] | 0x4c0 |
| ctx `cdmreg_cdm_context_pds0` / `_pds1` | CTX[0xe..0xf] / [0x10] | 0x4a8 / 0x4b0 |
| (the kernel's scratch) | CA[6..17] | six slots via 0x11040 |

Not used yet (no register found for them on G9P, to check):
`tpu_border_colour_table`, `cdm_resume_pds1`, the resume PDS programs.
The kernel's reserved object at the general heap's end grew to 64 KiB:
the fence page, the six scratch slots (+0x4000), a default CDM context
state (+0x8000); every VM maps it, userspace may not. Sync ops: waits are
done synchronously in the ioctl (10 s cap), signals attach the job's fence
to the syncobj (binary or timeline). A job holds a reference to its VM.

**`tools/gpu/pvr-uapi-test`** now runs G5's store through the UAPI from a
user VM (code BO mapped at the PDS and USC heap bases, data in the general
heap, store_const from `apple/g9p-selftest.bin`): CREATE_CONTEXT, then 100
jobs one by one and 16 in one SUBMIT_JOBS, each waited on through a
syncobj (DRM_IOCTL_SYNCOBJ_WAIT): **116 of 116 wrote 0xcafe0000, ~0.23 ms
per job including the ioctls -- PASS**. The self-tests still PASS.

### G7 step 5 (2026-11-25): rendering through the UAPI

`pvr_apple_uapi.c` takes free lists, HWRT datasets, render contexts and
geometry / fragment jobs (one handle space per file; datasets keep their
free list, jobs their VM):

- **Free list**: `initial_num_pages` PM pages (4 KiB, a GEM object of the
  kernel's; no growth yet: a PM out-of-memory stops the TA), their page
  numbers written into userspace's free-list BO at `free_list_gpu_addr`,
  the firmware descriptor (fw 0x17a10) over it.
- **HWRT dataset**: the kernel computes the tiling registers from the size
  as upstream `pvr_hwrt.c` does (32x32 tiles, 4 x 4 macrotiles: PPP_SCREEN,
  TE_SCREEN, TE_MTILE1 / 2, the macrotile stride, ISP_MTILE_SIZE, ISP_RGN);
  one PM slot (MLIST, macrotile array, VHEAP table) per render-target data
  set; the global free list if given, else the local one. One sample, one
  layer for now. A data set's geometry job waits for its previous fragment
  job, a fragment job for its geometry job (in the ioctl, for now).
- **Geometry -> StartTA**: `vdm_ctrl_stream_base` -> 0x408, `ppp_ctrl` ->
  0xc88, `te_psg` -> 0xc28, region headers -> 0xc38, TPC -> 0xc40, the
  dataset's screen / tiling registers; 0xc90 PDS_CTRL stays 0x1e3ce508
  (not in the UAPI without BRN 56279); 0x428 / 0xcc0 and the six 0x11030
  slots on the kernel's object; **RGX+0x470 = PDS heap +0x40, inside
  upstream's VDM sync static area**: userspace puts the program there (data
  {DOUTU -> a one-instruction USC task}, code at +0x60).
- **Fragment -> Start3D**: `isp_bgobjdepth` -> 0xf80, `isp_bgobjvals` ->
  0xf88, `isp_aa`, `isp_ctl` -> 0xf38, `event_pixel_pds_data` / `_info` ->
  0x620 / 0x628, **the end-of-tile PDS code at PDS heap +0x80, upstream's
  EOT static area** (0x618), `pds_bgnd[0..2]` -> 0x6a0 / 0x6b0 / 0x6b8; the
  depth / stencil load-store, scissor, depth bias, occlusion queries and
  PBE words are not used yet (the end-of-tile program loads its own state).
- **What the TA stores in a 0x11030 slot**: a VDM stream fragment (`0x300,
  <PPP terminate VA>, 0xc0000000`) -- with the slot 0 that went to GPU VA 0.
  Something at the 3D's end still writes to VA 0 (PDS requester; none of
  the zero Start3D / StartTA slots, nor RGX 0x11d0..0x11f8, is it -- to
  find): **every VM maps a page of the kernel's object at VA 0**, and
  userspace may not map that range.
- **Image state (G8 note)**: word 2 holds address bits 32:1 only; **bits 33..
  go in word 3's low bits** -- a target in the general heap (0x80_0000_0000)
  wrote to its address truncated to 33 bits (an MMU fault at 0x4501c0)
  until word 3 got `addr >> 33`.

**`pvr-uapi-test` renders G6's clear + triangle through the UAPI** from a
VM of its own (code BO at the PDS / USC heap bases, data in the general
heap): CREATE_FREE_LIST (128 pages), CREATE_HWRT_DATASET (64x64),
CREATE_CONTEXT (render), then 20 x SUBMIT_JOBS {geometry, fragment} with a
syncobj: **20 of 20 right (3072 background, 1024 triangle pixels), ~0.5-0.6
ms per render -- PASS**, with the compute part and the self-tests.

### G7 step 6 (2026-11-25): faults, hangs, idle power-off

- **Recovery**: a job event 0x4 in the firmware's job ring (an MMU fault:
  its own recovery leaves the core unusable, PDS / USC bases 0) or a job 5 s
  in the firmware: the pending jobs end with -EIO, the firmware and the GFX
  domain go off, the next job starts them again (`recover_work`). The
  self-test `fault` (a store into an unmapped buffer): the job ends with
  -EIO, one recovery, a store right after it PASS -- twice in a row.
- **Idle power-off** (`idle_ms`, default 5000): with no job for that long
  the firmware and the GFX domain go off (genpd `gfx` off); the next job
  boots the firmware again (~0.25 s).
- **The render targets' firmware objects survive a restart**: free-list
  descriptors and PM slots belong to one firmware boot's pool; they keep
  their parameters and are written again at the next job after a restart.
  `pvr-uapi-test`: 7 s idle (firmware off), then 5 more renders with the
  same free list and dataset: PASS.
- Submissions from the UAPI hold the driver's lock from the (re)start to
  the kick, so the idle power-off cannot slip in between.

Open / next: runtime PM through the PM core (the driver switches the GFX
domain itself now), free-list growth, transfer jobs, MSAA / layers /
depth, several firmware queues per data master and hardware MMU contexts
> 0, `drm_sched` instead of the in-ioctl waits; a firmware ping (the kernel's RTKit has no ping call); what the sgx node's
IRQs 185-188 / 193 are for (nothing needs them so far).

## G8 (2026-11-26): the plan, and the shader ISA

**The driver: a Gallium driver of its own** (GLES2/3, GL 2.1-3.x), not
Vulkan + Zink: the A10's CPU (two logical cores) pays for every layer
between the application and the GPU, and a Gallium driver needs far fewer
features than a Vulkan driver that Zink could run on. Mesa's API-neutral
`src/imagination` parts (csbgen and its XML, the PDS generator, the Rogue
hardware definitions) carry over; Vulkan stays possible later on the same
compiler. Steps:

1. **The ISA** (G8.1): the encoding of applegpu_g9p, measured with Apple's
   offline compiler as the oracle, every finding checked by running our own
   encoding on the GPU (compute jobs through the UAPI, `g9p-probe`).
2. **The compiler** (G8.2): NIR to G9P: instruction selection, register
   allocation, the program frame, checked kernel by kernel against the CPU.
3. **The 3D state** (G8.3): textures and samplers, varyings, blending,
   depth / stencil, on top of G6's render path.
4. **The Gallium driver** (G8.4), then labwc, SuperTux's GL renderer.
5. Separately: a display driver with page flips (vblank), for tearing-free
   output.

No disassembler for applegpu_g9p ships with Xcode's Metal toolchain:
`applegpu-nt -S` (also with the G9-G12 plugin `libapplegpuG9G12-nt.dylib`
loaded by `-load`) answers "Plugin interface not implemented:
AIRNTEmitAssembly"; `-mllvm -print-after-all` shows only the AIR front end's
passes; `AGC_ENABLE_STATUS_FILE=1` (the plugin also holds a string "driver
disassembler") makes the translator crash.

### G8.1: the encoding so far (`tools/gpu/isa/`)

`g9pisa.py`: `corpus()` builds one library per kernel (metal-tt does not
keep a library's function order -- an earlier one-library corpus mapped
kernels wrongly) in parallel, cached in `build/gpu/isa/c/`; `split()`,
`unique_bits()` (the encoding bits each operand bit sets, among variants of
the same length).

- **Instructions are runs of 16-bit little-endian words up to the first
  word with bit 8 clear**; the opcode is the first word's low byte. The code
  ends with the word 0x8000 at the start of a 64-byte block, after 0xc000
  padding words.
- **Move immediate (opcode 0x98)**, `0198 03dd W2 W3 [W4] 0154 0038`
  (`imm-map.py`, one kernel `o[0] = 1 << i` per bit):

  | value bits | encoding |
  |---|---|
  | 0-5 | W2 bits 1-6 |
  | 6-8 | W3 bits 1-3 |
  | 9 | W0 bit 13 |
  | 10 | W3 bit 4 |
  | 11-13 | W3 bits 12-14 |
  | 14, 15 | W3 bits 6, 7 |
  | 16-23 | W4 bits 0-7 |
  | 24 | W3 bit 15 |
  | 25-31 | W4 bits 9-15 |

  W3 bit 0 set = the 32-bit form: W4 is there only then. **Checked on the
  GPU** (`mov-check.py`): the compiler's code for `o[0] = 1` and `o[0] =
  0x10000`, the value field rewritten by our encoder, run through
  `g9p-probe` (`tail=1`: a fence record at the end of the job): 0xbeef,
  0x1234, 0x8001, 0x200, 0xdeadbeef, 0x12345678, 0x80000001, 0x01010101 --
  each written as given.

### G8.1: memory, ALU, the program frame -- our own assembler on the GPU

`g9pasm.py` assembles whole compute programs: the compiler's frame around
a body built by our encoders. `isa-check.py`, `isa-check2.py`,
`isa-check3.py` build programs for `g9p-probe` (in[0..3] loaded to
registers, one instruction under test, the result stored), `isa-run.sh`
runs them on the iPad and compares: **69 of 69 programs right**
(2026-11-26; `isa-check4`-`7` added control flow, workgroup IDs, unary
operations and banks; `isa-check8` six more: breaks, continues, long
jumps). The integer tests load one-hot inputs (1, 0x10, 0x100,
0x1000), so a result names the registers that were read.

**The frame** (`program()`), as the compiler lays out a compute kernel:

| words | |
|---|---|
| `1181 81hh 4100 acb7` | compare r0 (the local ID) with the threads per group, common-store half hh / 2 (`8189` / `8191`: half 4 / 8) |
| `01c8 03ss 0101 0120 46d1` | start the execution-mask stack in register ss / 2, the one after the program's, with that compare: lanes past the group size off |
| `0900 01oo 0000` | jump to the epilogue, oo bytes ahead, when no lane is active |
| body | |
| `21c8 03ss 01mm 46d2` | pop the stack (mm = 0x80 + 4 x its register) |
| `0100 01pp 0000` | jump to the end word, pp bytes ahead |
| `c000`..., `8000` | padding, the end word at the start of a 64-byte block |

Jumps take a 32-bit byte offset over W1 and W2, offset bits 8 / 24 kept in
bits 0 of the words (bit 8 is the continuation bit): `0900 0354 0000` jumps
0x254 bytes ahead (`isa-check8`: jump-none over 0x180 bytes of body, with
some lanes active and with none: right).

**Device loads and stores** (opcode 0x12), `W0 W1 W2 [W3]`:

- W0 bits 11-13: size -- 1 u8, 2 u16, 4 u32, 5 / 6 / 7 = 2 / 3 / 4 x u32;
- W1: bit 7 = store, bits 1-6 = the data register (first of the vector),
  bits 10.. = the buffer's **slot**: the compiler packs the buffers a kernel
  uses into consecutive common-store slots in argument order (a kernel
  using only `[[buffer(4)]]` reads slot 0, the same code as one using
  `[[buffer(0)]]`) -- so the four-buffer probe of "2D on the GPU" wrote to
  slots its table did not have;
- W2 bits 0-7 = the element index, bit 15 set; past 0xff W2 bit 8 and W3
  = index >> 8;
- after loads the compiler waits with `004d` (`044d` with another load
  still in flight: a counter, to check).

**Two-source ALU** (opcode 0x99, also 0x9d), `W0 W1 W2 W3`: W1 bits 1-6 =
destination; W2 bits 2-6 = the first source, bits 10-14 = the second
(r0-r31 checked in all three fields); W3 low byte = the operation: iadd
0xa1, isub 0xa5 (first - second), imul 0xa2, and 0xc8, or 0xce, xor 0xc6,
fadd 0x44, fmul 0x48, fmul saturated 0x4a; W3 bit 14 negates the second
source (fsub, -a * b).

**Register banks** (`isa-check7`): a register number names two registers.
W1 bit 9 selects the destination's bank, W0 bit 13 the first source's, bit
12 the second's (set: bank 1, where compute programs keep their values; r4
= 5 in bank 1 and r4 = 0xa0 in bank 0 read back as 5 and 0xa0). With a
bank bit clear by mistake an operand reads bank 0's unwritten register:
garbage. W2 bits 7 / 15 and opcode 0x9d instead of 0x99 change nothing in
these tests (the compiler sets the bits on some last reads: to check).

**FMA** (opcode 0x9a), `399a 03dd W2 W3 004c`: d = first * second + third,
the third in W3 bits 2-6 (W3 bit 7 set), W0 bit 11 its bank bit.

**Control flow on 32 lanes** (`isa-check4`, r0 = the lane, every lane
storing to out[lane]):

- **addresses**: `1189 03dd (1|idx<<2)<<8|0x81|4*u 4100|(u>>5)<<1 01a1
  0600|shift<<13` = r[d]:r[d+1] = the 64-bit common-store dwords u, u + 1 (a
  buffer's base) + (r[idx] << shift); `ss10 (1|pair<<2)<<8|st<<7|reg<<1
  8800` loads / stores at that pair (size in W0 bits 11-13 as above);
- **compares**: two registers `3181 W2 a0cc` (float: `60cc`), a register
  and an immediate below 1024 `1181 ... a1cc 0070`; conditions ult 0xb6,
  ugt 0xb7, eq 0xbd, slt 0xbe, sgt 0xbf, flt 0x59, fgt 0x5d, feq 0x55;
- **the mask stack** in a register (0xc8): push on the last compare
  (`46d1`, `66d1` inverted), else (`46d4`), pop n levels (`06d2 | n << 14`);
  if / else with lanes 0-9 taking one side: right;
- **select**: `399e dd W2 W3 60cc` sets d where the compare holds, `21f8 dd
  ...` the other lanes; right;
- **loops**: `86d0` opens two levels, `86d7` (`a6d7` on true) takes the
  lanes whose compare failed out, a jump `0101 W1 W2` (back while any lane
  is active; `0900` forward when none is) with a 32-bit byte offset whose
  bits 8 / 24 sit in bits 0 of the two words; a loop each lane leaves after
  `lane` iterations: right.
- **break and continue inside ifs** (`isa-check8`, the form Apple's
  compiler emits for a break nested in ifs): `0188 03ss 0101|n<<1 0120 86d6`
  sets the active lanes' level to the immediate n (0x98-style field) --
  break = the enclosing ifs inside the loop + 2, continue = + 1; the pops
  of the ifs bring broken lanes down to 2 (the loop's final `86d2` frees
  them) and continued lanes to 1. Lanes at level 1 stay off through the back
  jump: the loop's end needs an always-true compare and `86d7` to bring them
  back (without it a loop whose lanes all continued once ends there; with
  it, right). Breaks one and two ifs deep, a continue every other iteration:
  right.

**Workgroup IDs** (`isa-check5`): `2314 01dd 8100|w 0200|w>>8` loads
threadgroup-memory word w; word 0 is the workgroup ID, written there by the
kernel's PDS program (Apple's `f0000a01` in its shared phase): 4 groups of
32 lanes (K3 = 3, record word 6 = 1) each wrote `wg * 1000 + lane` right.

**Unary operations, conversions, shifts** (`isa-check6`): `219c dd 01ss op`
-- floor 0x40, rcp 0x80, rsqrt 0x81, log2 0x84, exp2 0x85; `1199 dd
0101|op|ss<<10 0120 046x` -- f2u 0x10, f2i 0x12 (truncating), u2f 0x14,
i2f 0x16 (0x0468 when converting to float); shifts by a register `199e dd
.. 04f0` (left) / `04f4` (logical right) and by an immediate (`119a` /
`519e`, the count in a 0x98-style field): every value right.

Compiler-only so far (not yet on the GPU): fp16 add / mul (opcode 0x89,
ops 0x04 / 0x08), integer min / max (5 words, ops 0xb2 / 0xb3 / 0xba),
float abs (an extra word, bit 14), sin / cos.

### G8.1: the fragment side -- varyings and textures from our encodings

`tools/gpu/g9p-render.c` draws one triangle into a 64x64 BGRA8 target
through the powervr UAPI (G7's render path, the clear and end of tile as
before) with the shaders and the per-draw state words supplied in a pack;
`isa/render-check.py` builds the tests with `g9pasm` and compares every
pixel with the CPU (barycentric interpolation at pixel centres, edges and
texel borders left out); `render-check.sh` runs them. **All four right,
3783 of 3783 pixels each** (2026-11-26): a constant colour, eight vertex
outputs, the vertex colours interpolated, an 8x8 texture sampled.

- **Vertex shader**: r0 = the vertex index (the vertex kick's DOUTW);
  ours builds the address of vertex r0 from the buffer address the PDS put
  in common-store dwords 0-1 and loads it like compute code. Outputs:
  `151e 01rr ss` writes r[rr / 2] to output slot ss (0-3 the clip-space
  position, then the varyings' components); the last one, `155e c1rr ss`,
  stands in the end word's place at the start of a 64-byte block.
- **Its register budget**: VDM_STATE5 `0x1081` gives a vertex task 8
  registers (unified size field 4) for 32 instances; **a vertex shader
  using 12 registers crashed the firmware** (the TA never finished; with 8
  it runs). A larger shader needs a larger field and fewer instances per
  task, registers x instances within 256: 16 registers (unified size 8,
  DOUTU temps `0x20`) with STATE5 bits 25-29 (the instances) = 16 or 8 run
  right (`vs16_*` in render-check); 32 x 8 is what the compiler asks for
  past 16 (to check).
- **The varying PDS program** (PPP VARYINGBASE = its data, the code right
  after): data `{03000000, 0}` (the 1/w iterate), then per
  perspective-correct varying of n components from component c on
  `{03 << 24 | ((n - 1) << 2 | 1) << 16 | L << 8 | L, 0x00400000 | (c / 2)
  << 23 | last << 31}` with L = 2 + 2c, padded to 16 bytes; code one DOUTI
  (`f0000004 | q << 3`) per data pair, END on the last, HALT. PPP: VARYING0
  f32_linear = the components, OUTPUT_SEL vertex size and VDM_STATE4 = 4 +
  the components, SIZEINFO1 PDS varying size (16-byte units) and USC
  varying size (2 for a vec4, 1 for a vec2).
- **Fragment iterate**: `0s0a (1 + c) << 9 | 0x100 | hh 0080` writes n
  components (W0 bits 11-12 = n mod 4, bit 9 perspective) as f16 halves
  from half hh; `0s08` the f32 form (W2 0x8000 for texture coordinates).
  `211c 08hh` emits the colour from four f16 halves from hh (BGRA8 target:
  stored as B, G, R, A). Fragment code uses bank 0 (W1 bit 9 clear) for
  its values.
- **Textures**: the texture state (8 dwords) and sampler (2) DMA'd into the
  fragment's common store by the TEXUNICODEBASE program (`DOUTD` q0 with
  control 0x10000008 -> dwords 0-7, `DOUTD END` q1 with 0x90010002 -> 8-9,
  HALT; its data at PPP UNIFORMDATABASE, header bit 9), SIZEINFO1 uniform
  size 2, SIZEINFO2 USC shared size 1. State: `0x0344050a | (w - 1) << 27`
  (RGBA8, R first), `(w - 1) >> 5 | (h - 1) << 9`, address >> 1 in dwords
  2-3; sampler `000e0000 00c00780` (nearest, clamp). Texels in Morton order
  with the y bit first. `f518 0100 8108 0700 0200` samples (state at
  dword 0, sampler at 8, coordinates r0-r1, into halves 0-3), `004d` waits.
  The fields, from the offline compiler's two-texture shader and checked
  by the compiled tests: W1 = `0x0100 | state dword << 8 | the result's
  first half`, W2 = `0x8100 | the coordinates' first half << 8 | sampler
  dword`; `f51a` returns four f32 registers instead of f16 halves. With
  several textures the states come first (8 dwords each), the samplers
  after them (2 each).

### G8.1: depth and blending

`render-check.sh` now draws several triangles per draw (the INDEX_LIST
count) with the ISP words per test: **13 of 13 tests right** (2026-11-26).

- **Depth test**: plain Rogue ISP state (Mesa `ppp.xml`): ISPA (front;
  ISPCTL `bpres` set, the back faces' own word after the front ISPB)
  `0x00120f00 | cmp << 17` -- compare mode bits 17-19 (1 LESS, 7 ALWAYS),
  bit 21 = depth write off (clear: on), bit 20 set as the captures have it.
  The depth starts at the fragment job's background depth (1.0). Two
  overlapping triangles, the second further (z 0.7 vs 0.5) or nearer
  (0.3): LESS keeps the nearer one either way, ALWAYS draws in order.
- **Blending is the fragment shader's work.** The offline compiler, given
  blend states in the pipeline description (`blending_enabled`,
  `source_rgb_blend_factor`... -- the format is in the toolchain's
  `metal-pipelines-script(5)`), emits **`211c 28hh`**: the colour emit's
  form with W1 bit 13 reads the tile's current colour into four f16 halves
  from hh; f16 arithmetic (opcode 0x89: `0189 01dd W2 01op 8000`, add 0x04,
  mul 0x08, first source half in W2 bits 1-6, second in bits 9-14) blends,
  `211c 0800` writes. ISPA's pass type must be **TRANSLUCENT** (bits 24-26
  = 1): with OPAQUE the hardware removes hidden surfaces first and runs only
  the top object's shader per pixel (not even the clear's), so the tile
  read sees nothing drawn before. Checked: the compiler's add, alpha and
  multiply shaders and our own add and multiply over a red triangle and the
  clear, every pixel right.

### G8.2: the compiler (2026-11-26): GLSL compute shaders on the GPU

`devices/a10/mesa/` builds Mesa 26.1.8 (the A5 driver's pin; `fetch.sh`
clones it from the A5's tree when present) in the `cascadia-a10-mesa`
image (Alpine 3.24 aarch64: native on an Apple Silicon Mac, a few minutes
from scratch, seconds incremental) with `-Dtools=g9p`. `files/src/g9p/`
is ours: `g9p_compile.c` (NIR -> G9P, the encoders twins of `g9pasm.py`'s)
and `g9p-cc` (GLSL -> Mesa's standalone GLSL front end -> NIR ->
`gl_nir_lower_buffers` -> the backend -> a G9PT pack). `mesa.patch`: the
`g9p` tool option, the GLSL compiler built without a Gallium driver, the
standalone front end's compute shader options and buffer-block limits
(left at 0 for these versions); OpenGL and GLES2 stay enabled in the
configuration, or `HAVE_OPENGL` / `HAVE_OPENGL_ES_2` make every GLSL
version "unsupported".

The first backend (commit 3efd6ba): compute shaders, one dimension, one
block: lowering (vars to SSA, compute system values with the local ID
from the index, buffer blocks to `load_ssbo` / `store_ssbo` with
32-bit-index-offset addresses, scalarized), NIR's optimizations, then a
linear scan over SSA defs (registers freed after their last use, address
pairs as scratch). ABI: r0 = the local index, threadgroup word 0 = the
workgroup ID, common-store dwords 2b / 2b + 1 = the buffer at binding b,
dword 30 = the threads per group (the prologue's lane test).

`tools/gpu/isa/cc-check.py` compiles each test kernel, runs it on 2 x 32
lanes through `g9p-probe` and compares every word with Python: **11 of 11
right** -- integer multiply-add, logic, shifts by a register, subtraction,
float multiply-add, fma, subtraction, conversions, reciprocal, exp2 /
log2, floor (`ineg` / `fneg` / `fabs` through a scratch register: 0 - a,
a ^ sign, a & ~sign).

**Control flow** (the same day). NIR's ifs and loops go onto the mask
stack as `isa-check4` / `8` measured them: an if = compare, push (inverted
for the failing side), a jump-none past the then-side, else, a jump-none
past the else-side, pop; a loop = `86d0`, the body, the continue fixup when
the loop has a continue, a jump back while any lane is active, pop 2; break
/ continue = the level-setting `86d6` with the ifs counted since the loop.
Compares used once by an if or a select are emitted there (`3181`, the
register-and-immediate form against 0 for a 0 / ~0 value); other compares
become 0 / ~0 through select (`399e` + `21f8`); min / max (integer,
unsigned, float) through select too, extract_u8 / u16 through the
immediate shift. Before the backend: `nir_opt_if`, peephole select
(branches of up to 8 instructions without loads become selects),
booleans as 0 / ~0 (`nir_lower_bool_to_int32`), then out of SSA (phi webs
into NIR registers, copies at the branch ends).

**Registers** are given over live intervals in program order: the program
is translated twice by the same code -- first only to number the
instructions and record each value's first and last position (a value live
into a loop stays live to its end, the back edge), then a linear scan gives
registers, then the second translation emits. Program order and not the
control-flow graph, because lanes split: a value written in one branch's
lanes keeps its register through the other branch. Scratch registers (an
address pair, a constant) are values of one instruction. Jumps take the
long form anywhere (the frame's jump to the epilogue included).

`cc-check.py`: **26 of 26 kernels right** -- the 11 straight-line ones and
if / else, nested ifs, a loop of lane-dependent length, a break out of a
64-iteration loop at a lane-dependent point, nested loops, the Collatz
sequence (a while loop with an if inside, each lane its own trip count),
unsigned / signed / float min and max, clamp, compares as values and as
select conditions, extract, and a 40-iteration loop inside an if (a jump
past 0x100 bytes). NIR rewrote every test's `continue` into if / else; the
continue path itself is checked at the ISA level only (`isa-check8`).
`isa-check2`'s case with registers r28-r31 now times out on the GPU with
the same program bytes as when it passed (to check).

**Vertex and fragment shaders** (the same day). `g9p-cc` compiles a
vertex and a fragment shader together (`FILE.vert FILE.frag`): the vertex
shader's compile lays out the varyings, the fragment shader's reads that
layout. `render-check.py` builds its `cc_*` tests from GLSL ES 3.00 that
way and derives the draw's state from the compiler's numbers (vertex
output dwords, the varying PDS program from the varyings' sizes and
interpolation, SIZEINFO1, VDM_STATE5 and both DOUTU temps words from the
register counts).

- **Vertex**: registers in bank 1 like compute; attributes are loaded as
  vectors (`4x32`) from one interleaved buffer at common-store dwords 0-1,
  attribute a at vertex x stride + 16a (the stride, in vec4s, is the
  buffer's: `g9p-cc -a`); outputs scalarized, `151e` each, the last output
  store of the last block left for the frame's `155e` at the end (its
  value kept live to the end); gl_PointSize dropped.
- **Fragment**: registers in bank 0 as Apple's fragment code has them (the
  iterates and the colour emit work there; the same encoders with the bank
  bits clear). Varyings iterated as f32 vectors; the colour converted to
  four f16 halves by the f32 multiply by 1.0 with an f16 result (opcode
  0x89, op word bit 9 -- from the offline compiler's `half(a * b)`) and
  emitted with `211c 08hh`. Textures: `f51a` into four f32 registers.
- **Blending** in the shader: `nir_lower_blend` with the blend state
  (`g9p-cc -b add|alpha|mul`) reads the destination through `load_output`,
  which becomes the tile read (`211c 28hh`) and four f16 -> f32
  conversions: an f32 multiply whose op word bit 10 marks the first source
  as an f16 half (the offline compiler's `float(h) * float(h2)` sets bits
  10 and 11).
- Register values are vectors now (loads, iterates, samples, `vecN`
  copies) in consecutive registers, at even registers; constants only
  used as I/O offsets or buffer indices take none.

**render-check: 30 of 30 right**, the 14 compiled ones among them: the
vertex colour, a constant colour, arithmetic and swizzles with clamp,
varyings of 2 and 3 components (packed into one slot by the GLSL linker),
a vertex transform, a select, a loop of a pixel-dependent count (the mask
stack in bank 0), division / sqrt / fract / pow, a fragment shader of 14
registers, a texture sampled directly and at computed coordinates, and
additive, alpha and multiplicative blending over a red triangle.

Next: the Gallium driver (uniforms and the real vertex formats, the draw
state from these numbers); on the compiler side flat varyings, discard,
several render targets, depth output, fp16 arithmetic, spilling.

## G9 (2026-11-26): the Gallium driver

`devices/a10/mesa` builds, besides `g9p-cc`, Mesa with **a Gallium driver
of Cascadia's own, `g9p`** (`files/src/gallium/drivers/g9p`, its winsys in
`files/src/gallium/winsys/g9p/drm`; `build.sh driver` -> prefix
`/usr/local/g9p`, built into `build/mesa/install-drv`, EGL / GBM / OpenGL
ES 2 / OpenGL, the surfaceless EGL platform only so far). `mesa.patch`
registers it for the kernel driver `powervr` (pipe loader, dri / dril
targets), where Mesa sends `powervr` to Zink otherwise; the shader compiler
of G8.2 is linked in.

**G9.1 (2026-11-26): OpenGL ES 2.0 draws on the GPU.**
`tools/gpu/gl/g9p-gltri.c` (EGL surfaceless, a framebuffer object W x H, a
clear, a triangle with a colour per vertex, glReadPixels, every pixel
against the CPU's interpolation): **right at every size tried** -- 1x1,
33x97, 64x64, 300x200, 1024x768 and the whole screen's 1620x2160 (3 480 732
of 3 480 732 pixels, 7.2 ms a frame with the read-back); drawn as arrays,
with a 16-bit index buffer, with 8-bit indices from client memory, as a
strip and as a fan. ~0.6 ms a frame at 64x64.

How it is built (the frame of `g9p-render`, generalised):

- **`g9p_device.c`**: the UAPI -- every resource one buffer object mapped
  into one VM (64 KiB granules, a bump allocator in the general heap); **one
  8 MiB code buffer at both the PDS and the USC heap's base**: the static
  programs at fixed places (the 0x470 task's in the VDM sync area at +0x40,
  the end-of-tile PDS code at +0x80, their USC programs, the vertex kick
  and a DOUTD program), shader code kept from +0x10000, and from +4 MiB
  each render's PDS data and background program (reset after it). One
  free list (1024 pages), one render-target dataset per target size (8
  kept; region headers, TPC, macrotile array, MLIST, VHEAP table in one
  1 MiB buffer each), one render context; a render = geometry + fragment
  job with a syncobj, **waited for**.
- **`g9p_context.c`**: a render gathers the draws into one target (the
  framebuffer's first colour buffer, BGRA8): the clear sets the
  background program (an f16 colour emitted, `g9p_clear_program`), every
  draw puts into the VDM stream its vertex state (the kick, output size,
  VDM_STATE5 from the vertex shader's registers), the common-store DOUTD
  of the attributes' addresses, and PPP blocks -- varyings, the pixel kick
  and varying PDS, OUTPUT_SEL, ISP (both faces, depth ALWAYS without
  writes, TRANSLUCENT when blending), viewport and region clip from the
  target's tiles, PPP_CTRL, DBSC -- then the index list (`0x6e44...` with
  the topology; with an index buffer `0x7e40...`, the address, 8 / 16 /
  32-bit indices); the end: the terminate block, the background and the
  end-of-tile image state of the target (stride in 64-byte units). Sent
  at a flush, a target change, before any CPU map.
- **Shaders**: the state tracker's NIR (inputs and outputs already
  intrinsics: `nir_io_has_intrinsics`) compiled per variant at the draw:
  the vertex shader per vertex-element layout (attribute i at common-store
  dwords 2i / 2i + 1 = the buffer's address + offset + first vertex x
  stride, loaded at r0 x stride: `g9p_vertex_attrib`), the fragment shader
  per the vertex shader's varying layout and the blend state
  (`nir_lower_blend`, the colour mask); the varying PDS program from their
  layout and interpolation.

**G9.2 (2026-11-26): uniforms and textures.** `tools/gpu/gl/g9p-glcheck.c`
(the same frame as `g9p-gltri`): a matrix in the vertex shader and a tint
in the fragment shader (`uniform`, also one stage at a time), a texture
over the whole target sampled nearest (`tex TW TH`), and a texture times a
uniform colour (`texmod`) -- **every pixel right**: uniforms at 64x64,
300x200 and 1620x2160 (3 484 620 pixels), textures 8x8, 16x4, 5x3,
33x17, 64x64, 100x60 and 256x256 (non-powers of two too) into targets up
to 1024x768.

- **Uniforms are common-store operands**, as the offline compiler reads
  them (`isa-check9`, and its fragment shaders with a `constant float4 &`
  argument): an ALU's first source `0x81 | 4u` in W2's low byte with the
  extension word `0x4100 | (u >> 5) << 1` reads common-store dword u --
  the form `addr` takes its base in. The state tracker's uniforms are
  constant buffer 0 (`lower_uniforms_to_ubo`); `load_ubo` at a constant
  offset becomes `iadd d, u, r(zero)` per component.
- **Each task's common store is one DOUTD** of an image the driver builds
  per draw (the DOUTD control word is Rogue's: bit 31 last, bit 28 the
  common store, bits 13-25 the destination dword, bits 0-11 the count --
  the texture program's `0x90010002` = 2 dwords to dword 8): the vertex
  task's = the attributes' addresses, then the uniforms (from dword
  align(2 x attributes, 4)); the fragment task's = the texture states (8
  dwords each), the samplers (2 each), the uniforms -- by the static DOUTD
  program as the "texunicode" program (PPP pointer 3 = its data,
  SIZEINFO2's shared size).
- **The vertex task's common-store size is PDS_STATE0's** (bits 16-24): a
  shader reading dwords up to 19 crashed the firmware with 3 there and ran
  with 4, 5, 6 or 8 (the unit is not Rogue's 64 bytes: to check; the driver
  gives a unit per 4 dwords and one more). A size in VDM_STATE5's common
  field crashed it with a 32-register shader whatever PDS_STATE0 said: it
  stays 0. **A vertex shader's DOUTU temps must be its budget's** (8, 16
  or 32 registers, VDM_STATE5's unified size): temps for 20 with a unified
  size of 32 crashed the firmware too. 24 and 32 registers with 8 (and 4)
  instances a task ran in `render-check` (`vs24_*`, `vs32_*`).
- **Textures**: sampled from a twiddled copy of level 0 (Morton order, y
  bit first, up to the smaller side, the larger side's other bits above;
  the sides rounded up to powers of two -- the state keeps the real ones),
  made by the CPU when the texture changed (a CPU write, or a render into
  it); BGRA swapped to RGBA, X made opaque. Nearest and clamped only: the
  sampler words (`000e0000 00c00780`) do not follow Rogue's layout, their
  filter and wrap fields are still to find.

**G9.3 (2026-11-26): depth, stencil, culling.** The ISP words the frame
already used decode as Rogue's (Mesa `csbgen/rogue/ppp.xml`): ISPA
`0x003e0f00` = compare ALWAYS, depth write off, the depth test enabled
(bit 20), line width 0xf; ISPB `0x0e000000` = stencil ALWAYS; PPP_CTRL
`0x4a0` = clip mode 1, flat shading from vertex 0, w clamp. So the driver
writes them as Rogue's: ISPA `dcmpmode` / `dwritedisable` / `sref`, ISPB
compare, the three stencil operations (Rogue's order: INCR_SAT, DECR_SAT,
INVERT, INCR, DECR) and masks, ISPCTL `two_sided` with the back face's own
words, PPP_CTRL `cullmode`; the clears go into the fragment job's
background depth and stencil (`isp_bgobjdepth`, `bgobjvals` = 0x300 |
stencil). `g9p-glcheck depth FUNC`, `cull FACE`, `stencil`: **every pixel
right** -- all eight compare functions and a depth test without writes,
two overlapping triangles (one sloped through the other) up to 1620x2160;
back, front and no culling with either front face; a stencil written by a
triangle and tested EQUAL 1 / EQUAL 0 by two quads.

- **The hardware clips depth to 0..w** (as Direct3D / Vulkan): a triangle
  at clip-space z -0.4 lost the part in front of z = 0. The vertex shader
  makes GL's -w..w that (z = (z + w) / 2 at the position's store, the
  compiler's `clip_halfz`), the viewport's z offset / scale become t - s /
  2s; then the depth tests were right.
- **The GL counter-clockwise face is the hardware's counter-clockwise one**
  (cullmode 2) when the viewport does not flip y (framebuffer objects;
  window-system buffers flip: the driver swaps then, to check on a
  display).
- Depth and stencil live in the tiles for one render: the depth buffer
  itself is neither loaded nor stored yet (the ISP's ZLS), so a render
  that is cut in two (a CPU read in the middle of a frame) loses them.

**G9.4 (2026-11-26): on the screen, in a window on the desktop.** The
desktop (labwc on simpledrm, `WLR_RENDERER=pixman`) offers `wl_shm` and no
`linux-dmabuf`, so Mesa's usual Wayland path (buffers the compositor
imports) has nothing to hand over. Mesa's software window-system path does:
a hardware driver can sit under it as d3d12 does. `mesa.patch` lets
`sw_helper.h` create g9p for `GALLIUM_DRIVER=g9p` (with
`LIBGL_ALWAYS_SOFTWARE=1`); `g9p_create_sw_screen` opens the powervr render
node itself, display-target resources get the winsys's display target
too, and `flush_frontbuffer` sends the render, copies its rows there and
has the winsys show it (EGL's Wayland software path: a `wl_shm` buffer,
labwc composites it). `build.sh driver` now has the Wayland platform and
softpipe (which brings the software path); `mesa/install.sh` puts the
build at `/usr/local/g9p` on the iPad, `g9p-run PROGRAM` runs a program
with it.

- **`g9p-glwl`** (`tools/gpu/gl/wl`, an xdg-shell window, GLES 2): the
  triangle in a window on the desktop, the right way up, ~340 frames per
  second at 640x480, ~670 at 320x240 (nothing throttles this path yet).
- **mesa-demos' `es2gears_wayland`**, unmodified (depth, culling,
  lighting, libdecor's decorations): **right, ~500 frames per second**
  in its 300x300 window, softpipe ~154. Its first run showed every face at
  the ambient colour: the driver turned the cull face round for
  window-system buffers (whose viewport flips y), which the state tracker
  already does (`front_ccw`) -- the back faces were drawn instead of the
  front ones. With that gone: the gears as softpipe draws them
  (193, 24, 0 on the red gear's face; read from the display's memory).
- **`g9p-glref`**: scenes of real programs into a framebuffer object,
  written out to compare with softpipe pixel by pixel -- es2gears's frame
  (its own gear and matrix code, mesa-demos, MIT): 30 of 90 000 pixels
  differ (edges), also after three frames.
- `G9P_DEBUG=1` writes each shader variant's code to
  `/tmp/g9p-{vs,fs}-<address>.bin`; 2 logs the attributes' addresses per
  draw, 3 the vertex uniforms; `G9P_NIR=1` prints the NIR.

Not yet: linear filtering, wrap modes, mipmaps, depth / stencil load and
store between renders, frame pacing (frame callbacks) and fewer copies on
the display path, desktop OpenGL (`libGL`: no GLX in this build), points and lines, scissors, a frame that does not start with a
clear (its background should load the target), uniforms at variable
indices, vertex formats other than 32-bit floats, instancing, KMS /
Wayland (display), fences that do not wait, other targets than BGRA8.

## 2D on the GPU (2026-11-25/26): g9p2d and SuperTux

The first use of the G7 driver: SDL2's 2D renderer drawn by the GPU, as on
the A5 (sgx2d + an SDL shim). **SuperTux 0.6.3 runs on it at 24-40 frames
per second on the full 1620x2160 display** (`supertux-gpu`, the game's
window 1080x810 turned and scaled 2x; 600-2200 quads a frame, its lightmap
drawn into a render target), checked from a screenshot of the display's
memory.

**A compute compositor, not the 3D pipeline.** Compute jobs already ran
through the UAPI; texturing, varyings and blending in the TA / 3D path were
not known yet. g9p2d cuts the target into 64x32 tiles, sorts each frame's
quads into the tiles they touch on the CPU, and runs one workgroup per tile:
each thread reads its pixels once, applies the tile's quads in call order
(inverse-affine parallelograms, nearest texel, colour modulation, SDL's
NONE / BLEND / ADD / MOD) and writes them back. No ordering problem between
overlapping quads, every pixel read and written once per frame.

### The display in every VM (kernel)

`pvr_apple_find_screen()` takes simpledrm's framebuffer from the device
tree (`/chosen/framebuffer`: PA 0x8_bc2dc000, 1620x2160, stride 6528,
x8r8g8b8; outside RAM, so no struct pages) and builds a one-entry DMA-only
scatterlist; `pvr_apple_vm_map_fence()` now also maps it with `pvr_mmu_map()`
(device-cache bypass) at **`PVR_APPLE_SCREEN_VA` = general heap + 128 GiB -
32 MiB** in every VM, and the VM's release unmaps it. The general heap is
reported 32 MiB short (`PVR_APPLE_HEAP_TAIL`), the whole tail is refused to
VM_MAP. A new helper `pvr_vm_get_mmu_context()` gives the backend the VM's
MMU context. Userspace finds the display right after the reported heap.

### The A10 compute ABI, as far as g9p2d needs it (`tools/gpu/g9p-probe`)

`g9p-probe.c` runs one compute job with every word settable (CDM record
words, PDS data and code words, the common-store table, buffer contents);
`g9p-probe-shaders.py` builds probe kernels. Findings:

- **Buffer arguments** are read from the USC common store: `buffer(i)`'s
  64-bit address at dwords 2i / 2i+1 (two-buffer kernels checked; a
  four-buffer kernel wrote nowhere we could see -- to check). g9p2d uses two:
  its tile records and the arena.
- **Registers**: the kernel's `__GPU_METADATA` (a flatbuffer) holds the
  temporary register count in the word after 0x01010000 (8 for
  `store_const`, 48 for g9p2d's kernel). It goes into the DOUTU word
  (TEMPS, 4-register units) and K0 bits 12-17. **48 temps x 256 threads do
  not fit**: the job never ends; 128 threads per workgroup do.
- **Workgroup size**: record word 7 bits 20+ = threads - 1 (up to 512
  checked with 8 temps), word 6 = 1 (checked for 128-thread groups; with
  4-thread groups and word 6 = 1 every group saw the same ID -- to check).
- **G9P's DOUT encoding differs from Mesa's Rogue one**: bits 3-7 = the
  64-bit source (const64 index: data dwords 2i, 2i+1), bits 10-15 = the data
  dword holding the control word, bits 8-9 = 2 for the hardware's own
  source (`f0000a01`, `f0000e01` in Apple's kernel program), dst in bits
  0-2 as Mesa's (0 DOUTD, 1 DOUTW, 2 DOUTU, 5 DOUTC). `f0001000` /
  `f4001808` (Apple's DMA program) fit: addresses dwords 0 / 2, controls
  dwords 4 / 6.
- **Apple's kernel PDS program** `c2c00003 f0000a01 d3000000 f0000e01
  fc000002 d3000000`: BRA if not IF1 to 3; shared phase: DOUTW (control
  dword 2, 0x90000000: common store) of a hardware value, which is
  `threadgroup_position_in_grid`; instance phase: DOUTW (dword 3,
  0x80000000: unified store) of the local ID, DOUTU. The shared phase's
  source is the hardware's workgroup ID: **one kernel record with K3 =
  groups - 1** gives every group its own `threadgroup_position_in_grid`
  (64 groups x 128 threads, every word right). With `f0000811` there
  instead (source const64 #2) the position is the record's own PDS data
  dwords 4-5 (one record per group; g9p2d's first version, as fast).
- **Per-record DMA does not work for many records**: the next record's DMA
  task overwrites the common store while earlier workgroups still read it
  (tiles skipped or done twice, sometimes a hang). One DMA record per job.
- **The end of a job: fence records** (Apple's capture of `store_const`
  ends with K0 4 / 2 / 0xc0000003 / 3 / 0xc0000003; the 0xc0000003 ones
  run the PDS program `c2c00002 d3000000 f0000005 d3000000`, DOUTC = a
  fence back to the CDM, K1 = 16 zero bytes | 0x4). Without one the job is
  signalled done while workgroups still run -- they are cut off (a tile with
  384 of 2048 pixels, the rest never written). g9p2d ends every job like the
  capture minus its first record: a one-thread empty kernel (K0 2, K1 | 2),
  a fence record, a fenced one-thread empty kernel (K0 3, K1 | 4), a fence
  record. `g9p2d-test stress` (every frame a clear and 20-200 translucent
  fills over a 1024x1024 target, every tile checked, 100 frames of up to
  105 ms) loses no tile with that tail nor with the single fence record
  (`G9P2D_TAIL=1`).
- **K0 0x103** on the kernel record: without bit 0 (fence) the same frames
  take 6 times as long, without bit 1 or with neither the jobs hang.
- **A fence does not make one pass's writes visible to the next** (a render
  target read half as zeros); the end of a job does. g9p2d submits one job
  per pass (target), all of a frame's jobs in one SUBMIT.

### Where a frame's time goes (2026-11-26, `G9P2D_PROF=1`)

SuperTux's title level, 1080x810 turned and scaled 2x: **41 ms a frame**,
of which the CPU waits 25 ms for the GPU, bins and submits for 2-4 ms and
runs the game for 12 ms -- the GPU is the limit, ~29 ms a frame. A frame
is ~1800 quads, ~27000 (quad, tile) entries.

- **Every entry costs its whole 64x32 tile**: only 34% of the pixels the
  kernel evaluates lie inside the quads' bounding boxes. Skipping, for a
  whole group, the eight-row quarters a quad's box misses is in the kernel
  now (no measurable gain in SuperTux); a per-pixel box test costs more
  than it saves (48.6 instead of 41.7 ms a frame), 2 instead of 4 pixels
  a thread (40 temps) changes nothing.
- **Opaque quads drop what is under them** on the CPU (an axis-aligned
  quad, turned by 90 degrees or not, of mode NONE, or BLEND with alpha 1
  and a texture whose texels are all opaque: the entries before it in the
  tiles it covers wholly are left out). SuperTux's title level has nothing
  of the kind: its 1280x720 background has transparent texels (the sky
  gradient, ~1500 one-row fills, shows through).
- **The GPU is busy, not idle: ~1.7 G (pixel, quad) evaluations a second
  is the kernel's rate.** `g9p2d-test stress 20 20 S` takes ~25 us more per
  64x32 tile (21 quads) once there are a handful of tiles, with workgroups of
  32, 64 or 128 threads alike (with 32 threads, 2 and 8 tiles take the same
  time: they run side by side), with the quads read through the constant
  address space or not. Peak arithmetic for comparison
  (`g9p-probe fma`: 8 independent FMA chains, 8192 FMAs a thread, 128-thread
  groups, a fence record at the end): +1009 us from 96 to 192 groups =
  ~100 G FMA/s, ~200 GFLOPS fp32 -- all six clusters at work (one could not
  do a third of it). So the compositor spends ~60 instruction slots per
  pixel per quad (unpacking, the inside test, the blend, the loop), where the
  3D pipeline's rasterizer, texture unit and on-chip blending would do most
  of that in fixed function. The init data's compute unit mask (P+0x1ac4 =
  1, 3 or 0x3f) changes nothing.

### g9p2d (`tools/g9p2d/`)

| file | what |
|---|---|
| `g9p2d.h`, `g9p2d.c` | the library: `g9p2d_open`, textures (RGBA8, any size, first fit in 32 MiB BOs), `begin` / `target` / `color` / `blend` / `quad` / `quad_uv` / `quad4` / `fill` / `fill4` / `end` / `finish` |
| `g9p2d-shaders.py` | the Metal kernel `comp` -> `build/gpu/g9p2d.bin` (G9PT: the code and its temps count) |
| `g9p2d-test.c` | every pixel of an offscreen frame against a CPU reference; a target drawn and sampled in one frame; 400 sprites on the display; the display's memory checked through `/dev/mem`; `stress N L [S]`: lost tiles and time per frame; `shot` dumps the display |
| `g9psdl.c` | the LD_PRELOAD shim (after the A5's): SDL's drawing calls into g9p2d, landscape windows turned and scaled onto the display, render targets, the touch gamepad (axis ranges from EVIOCGABS) |
| `supertux.sh` | `supertux-gpu`: the shim around `supertux2 --renderer sdl --geometry 1080x810`; the console unbound and labwc / swaybg / waybar / fbkeyboard stopped (SIGSTOP) while it runs |
| `build.sh` | docker (Alpine aarch64) build; `install` / `test` on the iPad (`/usr/local/lib/g9p2d`) |

Memory: the arena is 1 GiB of GPU VA right below the display; two frame
slots of 8 MiB each (ops, tile lists, tile records; the CDM streams and
PDS data through a second mapping of the same BO in the PDS heap), then the
texture BOs. The kernel addresses all of it with 32-bit offsets from one
pointer. The code BO sits at the PDS and USC heap bases.

Results (2026-11-26): `g9p2d-test check`: 0 of 60000 pixels off by more
than 2 (max 1), the two-pass frame right, 10 of 10 runs; `screen`: 70
frames per second with 400 blended 64x64 sprites over a full-screen
textured quad, one full-screen textured quad alone 10.3 ms; 100116 of
100116 sampled display pixels as the texture says.

`./cascadia a10 modules gpu` unloads powervr, and labwc (wlroots) exits
when the GPU's DRM device goes away; the script starts the desktop again if
it was running.

Open: fewer instructions per (pixel, quad) -- half precision, smaller
tiles (34% coverage), drawing the game at its own size and scaling once --
or the 3D path; thin stripes seen in
SuperTux, mostly at scene changes -- tearing (the display is drawn while it
is scanned out: no vblank with simpledrm) or something else, to check; one
SuperTux run ended in SIGBUS after ~2450 frames (later runs of 11000+
frames did not; cause to check); linear filtering; the 3D path (TA / 3D
with textures) for real GL.

## G2: the recipe (from the 22H355 kernelcache, 2026-11-23)

`tools/gpu/gfx-boot.py`. Every step is read from our own binaries:

- **AGXFirmwareKextG9PRTBuddy** (kext text at kernelcache file 0x1f88000):
  for core config type 3, `requestFirmwareBinary` returns the image at
  0xfffffff00548b87c, 0xa4060 bytes (image + `fwsg` trailer);
  `getBootConfigOffset` 0 (the image goes in unpatched); `getRuntimeBinSize`
  **0xd8000** (= the image's `RTSZ`); `getBuild` "Release".
- **AppleA7IOP-AKFWrap-v2** (AppleA7IOPV2 / V2GFX; parent vtable
  0xfffffff006fe4498):
  - mailbox init (slot +0x670): IOP +0x4000 = 0x1111, IOP +0xc00 = 0x1111;
    interrupt masks: IOP +0x4000 / +0x4004 and +0xc00 / +0xc04;
  - inbox status IOP +0x4008: bit 17 empty, bit 16 full, bits 18-19 error;
    send IOP +0x4010, receive IOP +0x4038 (64-bit), outbox status +0x4020;
  - `_mapFirmware`: GFX copy in Fender (+0x2014 / +0x2018 IOP address,
    +0x201c length & ~0xfff, +0x2020 = 0, +0x200c / +0x2010 physical), then
    the V2 copy in the IOP block (+0x18 / +0x20 IOP address, +0x28 length,
    +0x30 = 0, +0x08 / +0x10 physical, **+0x38 = 1**);
  - run: Fender +0x2048 bit 0 (V2's own run bit, IOP +0x44 bit 4, is
    overridden for GFX);
  - timebase (`force-timebase-sync`): IOP +0x80c &= ~2, IOP +0xc030 / +0xc038
    = counter lo / hi, IOP +0x80c |= 2;
  - slot +0x6c8: IOP +0xc004 = 0x11, IOP +0xc024 = 1 (not used in run 1).
- RTKit management (HELLO, EPMAP, IOP / AP power 0x20) over that mailbox:
  m1n1's `fw/akf`, with our own window / run / timebase (its base AKF writes
  a "CPU_CONTROL" at IOP +0x28, which is the V2 window's length here).

## G2 results

**Run 1** (2026-11-23, `build/gpu/g2-run1-*.log`, `tools/gpu/gfx-boot.py 5`):
**the GFX firmware boots.** Runtime region 0x80fd60000 +0xd8000 (image
0xa4000, rest zero), window in both copies, mailbox / timebase as above, run
bit -> within **0.02 s**: HELLO `0x100000000c000c` (**RTKit protocol 12**
only), EPMAP **endpoints 0, 1, 0x20, 0x21**, the crashlog endpoint asks for a
buffer (`0x110200000000000`: 2 pages, no address) and takes one, IOP power
ack 0x20, AP power ack 0x20. 5 s more: no message, no crash; m1n1 alive.
IOP after boot: +0x48 0x79 -> 0x6f, +0x80c 0x2 -> 0x60000003, inbox status
0x20001 -> 0x29901, outbox 0x20001 -> 0x26601.

The same proxy session, firmware still running, no init data sent:
Fender +0x1000 = 0x11 -> 1, then **RGX +0x18 hung m1n1** (`g2-run1-coreid.log`).
A booted firmware without init data does not open the core either.

Order difference found when comparing with the firmware's power path: it
writes Fender **+0x2058 = 1 before any power state is up** (its poll then
times out while the core is off), and the host's state request follows; we
always wrote +0x2058 after state 1, where it clears at once. Run 10 (G1,
cold, `gpu-id.py --req --arm-reset`): +0x10 = 7, **+0x2058 = 1 at state 0**,
then +0x1000 = 0x11, wait for +0x2058 to clear, RGX.

**Run 10** (2026-11-23, `build/gpu/g1-run10-*.log`): +0x10 = 7; **+0x2058 =
1 at state 0 stays 1** (the request is held while the core is off); +0x1000
= 0x11 -> 1 in 0.2 ms and **+0x2058 clears 0.4 ms later** -- Fender runs the
reset sequence on power-up, exactly as the firmware's path expects. RGX
+0x18 still hung m1n1.

The last Fender bit the firmware touches, **+0x2044 bit 0**, is its idle
path (fw 0x2d900 / 0x2d97c around the RTOS idle: fn 0x1d824(0) waits for bit
17 of a word at [fw 0x37590] + 0xba0 and sets the bit, fn 0x1d824(2) /
0x17404 clear it when the image's flag at 0x812b0 is nonzero -- 0 in
22H355): gating while gfx-kf sleeps, not the core's access path.

So the cold power sequence is now fully known and behaves as designed
(state, request bits, reset held and executed on power-up), and RGX reads
still stall. What the working path has on top is the firmware **with its
init data** (its I/O regions and power structures). That is G3.

## G1 answered from Linux (2026-11-23)

**The Rogue core answers when the same cold sequence runs under our Linux
instead of m1n1** (`build/gpu/g1-linux-mmio-*.log`; `tools/gpu/mmio.c`,
built static by `tools/gpu/mmio.sh`, since Alpine's busybox has no devmem):
`ps_gfx` (0x20e080328) 0x0f000b00 -> 0x0f0003ff, Fender +0x10 = 7, +0x2058 =
1, +0x1000 = 0x11 -> state 1, +0x14 = 2, +0x2058 back to 0, then

    RGX +0x18 (CORE_ID)       = 0x011b0602
    RGX +0x20 (CORE_REVISION) = 0x000d0a00

No GFX firmware involved. So the power sequence is right, and the hangs of
G1 runs 1-10 and G3 are a property of the m1n1 environment: something the
Linux kernel sets up at boot (PMGR domains, fabric / memory performance
state, ...) that m1n1's proxy leaves undone. `build/gpu/pmgr-linux.txt` is
PMGR's state under Linux (PS registers 0x20e080000..+0x500, SoC perf
0x20e068000, GPU DVFM, PLL5) for a diff against m1n1.

What the m1n1 / Linux difference is **not** (runs 11-13, `pmgr-diff-*.log`,
`g1-run1[123]-*.log`):

- **PMGR**: `tools/gpu/pmgr-diff.py` (GFX on, every word of the Linux dump):
  24 of 407 words differ; GFX itself is identical (0x0f0003ff). Linux's
  pmgr-pwrstate sets AUTO_ENABLE (bit 28) on the domains that are on at boot:
  SBR, PMS, MCC, DCS0-3, SMX, SF, DISP0_FE, SEP; RTMUX / SIO / SIO_BUSIF sit
  in auto state 4; PCIe, UART1/4/6, I2C3, CPU1 differ for unrelated reasons;
  PLL5 bit 29 and the GPU DVFM word differ only because the Linux dump was
  taken in state 1. Setting AUTO_ENABLE on SBR, PMS, MCC, DCS0-3, SMX and SF
  under m1n1, then the cold sequence: still hangs.
- **Mapping attributes**: m1n1's identity MMIO map is Device-nGnRnE, as is
  Linux's /dev/mem O_SYNC mapping; reading through m1n1's nGnRE alias
  (0xf2_0100_0018) hangs too.
- **Time**: 1 s between state 1 and the read (Linux's read came hundreds of
  ms later): still hangs. With m1n1's exception guard off, nothing reaches the
  UART: a bus stall, not an SError.

Left: what differs on the CPU side (which core / cluster and its
performance state -- under Linux cpufreq runs the Hurricane / Zephyr pairs,
m1n1 stays where iBoot left the boot CPU) and anything else Linux's boot
touches outside PMGR. Under Linux everything works, so the GPU bring-up can
continue there (userspace through /dev/mem for registers; a kernel module
for the firmware's memory).

## GPU work under Linux (2026-11-23)

The GPU tools now run against the iPad's Linux as well as m1n1's proxy
(`GPU_BACKEND=linux`): under Linux the core answers, under m1n1 it does not.

- `tools/gpu/mmiod.c`: a static aarch64 daemon (built with `mmio.c` by
  `tools/gpu/mmio.sh` in an arm64 Alpine container), started over ssh into
  /run; a binary request / reply protocol on stdin / stdout: 32 / 64-bit
  reads and writes, block reads / writes, CNTVCT, ping. All accesses go
  through /dev/mem opened O_SYNC (Device memory: no CPU caches), as aligned
  32- / 64-bit words.
- `tools/gpu/linuxproxy.py`: the Mac side, the same `p` / `u` / `iface`
  objects the tools use with m1n1 (the ADT from `build/adt/j171.bin`,
  `pmgr_adt_power_enable` done in Python, a heap over the reserved RAM,
  CNTPCT through the daemon). `tools/gpu/gpusetup.py` picks the backend;
  gpu-id.py, gfx-boot.py, gfx-init.py, gfx-softcrash.py and the survey tools
  use it.
- **RAM for the firmware**: Linux boots with `mem=2944M`
  (`EXTRA_BOOTARGS="mem=2944M" ./cascadia a10 pack`); System RAM then ends
  at 0x8b7ffffff and 0x8b8000000..0x8ba4dc000 (36 MiB, top of RAM) is
  physically contiguous memory Linux never touches -- the backend's heap
  (`GPU_RAM`, default 0x8b8000000:0x2000000). No kernel module needed.

**First run** (`build/gpu/linux-gpu1-*.log`): Linux reports 2899 MiB; through
the backend `gpu-id.py --req --arm-reset` reads **CORE_ID 0x011b0602,
CORE_REVISION 0x000d0a00 (13.10.0)**; `gfx-init.py` boots the firmware
(RTKit up in 0.01 s, ping), hands it the init data with the **real RGX** at
P+0x8c0, and it runs 5 s with no crash, main task done (A+0x4 / +0x10 /
+0x14 = 1), ping answered. G1, G2 and G3's "firmware idle and stable" now
all hold on the platform the driver will live on.

## G4 progress (2026-11-23, under Linux): a compute kick reaches the CL data master

`gfx-init.py --channels --kick-cp` (logs `build/gpu/g4-run*.log`), crash-driven
as in G3. The firmware now takes a kick on the compute channel, powers the
GPU itself, sets up the MMU context, initializes its scheduler and hands our
command to the CL data master's pre-processing callback, which stops on our
zeroed command: **"AGFCLDataMasterPreProcessingCallback :: Unexpected command
type 0"** (run 10).

How work reaches the firmware (22H355):

- **Doorbell** (EP 0x21, fw 0x1db94): message type in bits 53:48. Type 3 =
  kick, low 2 bits = data master (0 TA, 1 3D, 2 CP): sets that bit in the
  pending-work mask and wakes the main loop; type 4 sets bit 31; type 5
  checks B+0x49f0 / +0x49f4; type 6 is a power event for agx_power.
- **Main loop** (fw 0x1dcc6): dispatch table at fw 0x300b8, 11 entries of
  {mask_lo, mask_hi, fn, arg1, arg2}; bits 0 / 1 / 2 (and 17) -> fn
  0x2167c(dm) (the scheduler); bits 24-30 -> 0x1e468 / 0x1e9a5 / 0x1f1cf.
- **Channels** in X: channel ch = 16 bytes at X+0xc + 0x10 * ch: {&firmware
  read index, &copy, &host write index, ring base}; ch = 3 * group + dm for
  ch 0..11 (four groups of TA / 3D / CP), ch 12 = device control (0x24-byte
  entries, its type 3 writes RGX+0xe140 / +0xe148 from T+0x7ec). Work rings:
  256 entries of 0x14 bytes {+0, +4, **+8 queue object**, **+0xc data master
  (must match: 2 for CP)**, **+0x10 u16 the queue's command write index**,
  +0x12 u8 priority (0..63), +0x13 u8 "invalidate the object's cache lines"}.
- **Queue object** (0x8c+ bytes): +0 state (+0x30 = command write index,
  copied from the ring entry; +0x50 ring size), +4 command ring (fw VAs of
  commands), +0xc scheduling entries (0x2c bytes each: +8 command & ~0x1f,
  +0xc ...), +0x44 unit mask, +0x88 status. The scheduler enqueues it by
  priority (fw 0xf008) and calls the data master's handler from the table at
  fw 0xb0460 (fw 0x1071e).
- **Power**: on work the agx_power task runs the firmware's power-up path
  (fw 0x6ce4) -- it needs the Fender I/O region; with P+0xd34 / +0xd3c
  nonzero the firmware requests the perf state itself (Fender +0x1000), as
  it must before it touches RGX. Then fw 0x1c72e ("GPU is not powered on
  after power kick!" if the power struct's +0x38 is 0).
- **I/O regions**: a table at **P+0x640 + 0x20 * type** (41 types listed at
  fw 0x2feec), entry {+0 PA lo, +4 PA hi, +8 firmware VA (written back), +0xc
  size, +0x10 / +0x14 / +0x18 copied, **+0x1c nonzero = mapping attribute
  0x21b, else 0x209**}; mapped by fw 0x1bc94 into fw 0x3214c + 0x20 * type
  (VAs at 0xe0000000..). Type 0x14 = RGX (P+0x8c0, the "RGX" fields used
  since G3), **type 0x1c = Fender (P+0x9c0: 0x201d00000, 0x4000, +0x1c = 1)**,
  type 0x24 = FenderScratchRam (P+0xac0: the G3 "scratch RAM" is this region,
  0x201e10000 on the hardware; a RAM page so far).
- **MMU**: T+0x7ec -> u64 page-catalog PA per context (context 0 at +0;
  T+0x7f0 / +0x7f4 masks of further contexts), written to RGX+0xe140
  (context) / +0xe148 (PC >> 12) by fw 0x19d18; T+0x7e8 -> u32 MMU
  generation per context, X+0x198 -> the firmware's copy; a difference makes
  it invalidate through RGX+0xe138 (fw 0x19f0e).
- **Mapping ring**: B+0x49e8 -> control {+0 firmware index, +0x10 host
  index}, B+0x49ec -> 256 entries in the memory-descriptor format (flag bit
  0: map, else unmap): the host can map more firmware memory at run time
  (fw 0x19bc4).
- **Scheduler init**: **B+0x4a24 nonzero** makes fw 0x1c72e fill the
  per-data-master handler table (fw 0xeec2) and clear the flag; without it
  the first command jumps to address 0.
- X+0x150 -> a status struct (+0 == 2 checked).

**Runs 11-12: the firmware takes a compute command and starts it.** With
X+0x144 / +0x148 / +0x14c pointing at pages (X+0x144's +0xc is a state, 0 or
5, read by the interrupt task) a type-3 command whose microsequence starts
with StartCompute runs **with no crash, ping answered**: channel 2's read
index reaches the write index (1), the queue's scheduling entry 0 becomes
{1, 0x00c00000, command | 0x12, ..., microsequence position at +0x18 /
+0x1c}, and **Fender is in state 1 -- the firmware powered the GPU itself**.

The compute command (CL callback fw 0x14f38; type 3, also 7 / 9 on CL):
+0x0 type, **+0x14 -> a 0x90-byte context state**, **+0x1e8 microsequence,
+0x1ec its size**, +0x298 / +0x29c and on: pointers the firmware writes.

Microsequence interpreter (fw 0xb49c, `switch (op & 0x3f)`, "Unknown SKU
command type" otherwise):

- **op 0 WaitIRQ**: waits for event bits `op >> 6` (set by agx_interrupt);
  for the compute data master it also waits for RGX+0x4a0 bit 0 (CDM done);
  4 bytes.
- **op 0xb StartCompute** (0x4c bytes on 22H355): +4 -> two words to RGX
  +0x4c0 / +0x4c8; **+8 -> the CDM arguments: [0..1] RGX+0x498, [2..3]
  +0x4b8, [4..5] RGX+0x480 (the CDM control stream base)**, [6..0x11] six
  64-bit slots through RGX+0x11040 / +0x11044 indexed by +0x11ff8 = 0..5;
  +0xc -> a unit table (0x18-byte entries: +0 = 0, +0xc = 1, +0x10 = the
  op's +0x10); +0x14 byte -> RGX+0xf07a; **+0x2c -> context state: [0xe..0x10]
  -> RGX+0x4a8 / +0x4ac / +0x4b0; if its word 0 is 0, RGX+0x478 = 1 (CDM
  start)**, else a context resume (fw 0x17550); +0x3c -> a flag that must be
  0 (else the op only sets it to 1); the 64-bit +0x40 / +0x44 must not
  exceed the one at *(+0x48) (a cancel threshold).
- **op 0xc FinalizeCompute**: stops the CDM (RGX+0x11f00, +0x688, +0x1720,
  +0xe230 = (+0xc) << 2 | 0x803 ...), stores context through fw 0xecfc only
  when RGX+0x4a0 bit 1 says so, +0x20 / +0x24 -> the completion stamp
  (address, value; kept at fw 0xa4cf8 / 0xa4cfc), +0x28 = the bytes to skip
  to the next op.
- **op 2 End**: completes the command (fw 0x11898 with the stamp above).

Rogue MMU format (upstream drm/imagination `pvr_rogue_mmu_defs.h`, 4 KiB
pages): PC entry u32 = (PD PA >> 12) << 4 | 1, index VA[39:30]; PD entry u64
= PT PA | 1 (page size 0), index VA[29:21]; PT entry u64 = page PA | 1 (+2
read-only, +4 coherent, +8 SLC bypass), index VA[20:12].

**Runs 13-14: the terminate-only job is queued but not dispatched.**
`gfx-init.py --job` builds it: context 0's page tables (GPU VA 0x200000..
0x204000 onto the heap), a CDM stream that is one terminate word
(0x80000000), a PDS HALT (0xd3000000) at +0x1000, the CDM arguments and
context state pointing there, and the microsequence StartCompute (0x4c) ->
WaitIRQ (event bit 2) -> FinalizeCompute (0x34; stamp STAMP+0 = 1) -> End.
The firmware takes the kick, powers the GPU (Fender 1; RGX+0x18 reads
0x011b0602 from the AP meanwhile) and puts the command in its queue's
scheduling entry -- but the entry's microsequence position stays at the
start (the unit table is untouched, RGX+0x480 / +0x4a0 stay 0, no stamp),
and a crash dump requested on the running firmware shows every task idle
(agx_background at 0x1ddb8, agx_interrupt at 0x22e48, agx_power at
0x23c8c). Enabling the engines (P+0x1ab0 = 7: bits 0 / 1 / 2 = TA / 3D / CP,
read through fw 0x32b68 by fw 0xaf00) and their unit masks (P+0x1abc /
+0x1ac0 / +0x1ac4 = 1, fw 0x32b74 / 0x32b78 / 0x32b7c) did not change it.

The interpreter (fw 0xb49c) is only called from fw 0x2920 -- the
interrupt-side dispatcher, entered from agx_interrupt (fw 0x2339c ->
0x3b02) -- so the next step is what makes that path pick a compute job up
(its conditions on fw 0xd75f4 / 0xd75d0 and the data master's state).

**Runs 15-31: the job runs on the GPU and completes.** One blocker after the
other, each found by reading the firmware and checked on the iPad:

- **Seeing the firmware's own state.** The coprocessor's D-cache hides its
  writes from DRAM; a soft-crash request (EP 1, 0x40) makes it clean its
  cache first, so `gfx-init.py --crash` prints the scheduler's globals
  (FW_STATE: wake bits 0xd75d0 / 0xd75f4, per-data-master slots at 0xa4c98
  + 0x70 * n, queue masks at 0xb0320.., the queue table at 0xa4e90) before
  and after it. Page 0x32000 (RGX VA at 0x323cc = 0xe0000000, Fender VA at
  0x324cc, the I/O VA table 0x3214c + 0x20 * type) lives in FenderScratchRam
  (our region S), not in the image's copy.
- **Firmware -> host rings** (run 15, a cold GPU): X+0xdc / +0xe0 (job
  events, fw 0x95b0 / 0x1bb7a), X+0xf4 / +0xf8 (power events, fw 0x92d0) --
  256 entries of 0x3c bytes each -- and X+0xec / +0xf0 (trace, 512, gated by
  fw 0x32044); control {+0 host read index, +0x20 firmware write index}.
  Without X+0xf4 a cold power-up faults on address 0x20. The firmware
  signals new entries with **0x2042 << 48 on EP 0x20**; the host must move
  its read index or the firmware blocks on a full ring ("Waiting for space",
  run 23). gfx-init.py now sets the rings up and drains them.
- **The compute slot's unit mask** (run 16): the interrupt-side dispatcher
  (fw 0x2920) runs the interpreter (fw 0xb49c) for each data-master slot
  (slot 2 = compute); the scheduling entry is picked by fw 0x128f4 / 0x11cd0
  only if `(slot available ^ busy units) & queue+0x44 & entry+0x28` is
  nonzero. Slot 2's units come from **P+0x1ac4** (fw 0x32b7c, via fw 0xaf00),
  and entry+0x28 is copied from the slot by the CL pre-processing callback.
  P+0x1abc / +0x1ac0 / +0x1ac4 = 1 -> the job is picked (run 17).
- **StartCompute's cancel threshold** (runs 17-19, the RAM stand-in for RGX
  showing that no CDM register was written): the op programs the CDM only
  when the 64-bit op+0x40 is **strictly below** the one at *(op+0x48);
  otherwise it sets *(op+0x3c) = 1 and starts nothing. Threshold ~0 ->
  **the CDM runs** (run 20): RGX+0x480 = 0x200000 (our stream), RGX+0x4a0 =
  1 (done).
- **A cold GPU** (runs 22-25): GFX power-cycled through PMGR (`--cold`) the
  Rogue core loses its sticky state; the firmware's own cold power-up needs
  the power struct's H+0x4c40 (else it reads I/O type 5, TempSensorRegister,
  which the kext leaves unmapped on G9P) and then loops on DVFS events;
  **with `--power` (Fender state 1 before boot, as the kext does) it is
  stable.**
- **The MMU fault** (runs 20-28): RGX+0xe150 (ROGUE_CR_MMU_FAULT_STATUS) =
  0x000ff251: fault, write, level 1 (page directory), context 0, address 0,
  requester 9 = **PDS** (Imagination DDK `rgxdebug.c`, MMU requester tags).
  Not the stream: with a WaitIRQ that never ends (no FinalizeCompute) the
  job leaves no fault (run 28). It comes from FinalizeCompute's CDM stop /
  context store (RGX+0x11f00, 0x688, 0x1720, 0xe230 / 0xe238), which waits
  forever on RGX+0xe238 bit 0.
- **FinalizeCompute's fast path**: a nonzero word at *(op+0x2c) skips the
  stop / context store (meant for a job that finished by itself; on iOS
  presumably set by the kernel's end, our terminate-only stream has none,
  so the host sets it). **STAMP+0 = 1** (run 29).
- **The command's completion record** (run 29 crashed after the stamp): the
  CL post-processing callback (fw 0x15750, "AGFCLDataMasterPostProcessing
  Callback") copies CMD+0x26c.. and writes *(CMD+0x26c) = CMD+0x274 through
  fw 0x95b0; with CMD+0x26c = STAMP+0x10, 0x274 = 0xc0de: **STAMP+0x10 =
  0xc0de, no crash, ping answered** (runs 30-31).

Rogue register names from upstream `pvr_rogue_cr_defs.h`: 0x4a0
CDM_CONTEXT_STORE_STATUS (bit 0 complete; the firmware's WaitIRQ uses it as
"CDM done"), 0x4a8 / 0x4b0 CDM_CONTEXT_PDS0 / 1 (the context-store PDS
program), 0x4b8 / 0x4c0 CDM_TERMINATE_PDS / 1; the PDS HALT 0xd3000000 is
Mesa's encoding. 0x478 / 0x480 / 0x498 / 0x11040 are not in the public
headers (0x480 = the control stream base, as upstream's fw interface
`cdm_ctrl_stream_base`).

**Runs 32-43: the fault is not the job's; the firmware's own power-up was
being skipped.**

- With StartCompute cancelled (threshold 0: the CDM never runs, RGX+0x480
  stays 0) the PDS fault still comes (run 37); without a kick it does not
  (run 27). Neither PDS program is fetched first (both pointed at unmapped
  VAs, the fault stays at VA 0: run 33). With the RAM stand-in for RGX and
  no WaitIRQ (`--no-wait`, run 35) the firmware's last writes after the job
  are RGX+0x11f00 = 1 and +0x688 = 7: **fw 0x17cb0, the cache flush before
  an idle power-off** (fw 0x61a0, from the main loop when P2+8, P3+0x24 and
  P4+0xc show nothing busy): 0x11f00 = 1, 0x688 = 7, SLC 0x1720 = 0x1f, MMU
  0xe230 = 0xc03.
- That flush writes a fence to **RGX+0xe240**, which the firmware programs
  at every GPU power-up (fw 0x5660) from fw 0xd6c90 -- the two words at
  **X+0x4 (region E)**, copied at init (G3 run 5). Left 0: the write to VA
  0. gfx-init.py `--job` now sets E = GPU VA 0x203800 (mapped by the job).
- But with `--power` RGX+0xe240 stayed 0: **the firmware's power-up never
  ran** -- no power event 0x11, none of fw 0x5660's RGX setup (0xe200..
  0xe20c SLC, 0x690, 0x730, 0xe240). The kext's "Fender state 1 before
  boot" makes the firmware treat the GPU as already up, and our core ran
  uninitialized. So: `--cold` without `--power`.
- The cold power-up then needs: I/O type 5 (**TempSensorRegister**,
  P+0x6e0: PA, +0xc size) -- a RAM page (region TS) does, H+0x4c40 is not
  needed; and **H+0x4 = the DVFS sample period** (the power task re-arms
  its timer at now + H+0x4, fw ~0x24886 -> fw 0xab50 -> 0x6730, power event
  0x19); 0 made it fire back to back (8000+ events in 4 s, the firmware too
  busy to answer a ping, runs 24 / 41). H+0x4 = 240000 (10 ms at 24 MHz).
- **Run 43: the job completes with no MMU fault**: STAMP+0 = 1, +0x10 =
  0xc0de, RGX+0xe150 = 0, RGX+0xe240 = 0x203800, fw 0x5660's SLC pattern at
  RGX+0xe208. Then the iPad hung: `--watch` kept reading RGX after the
  firmware had powered the idle GPU off: an RGX read on a powered-off core
  is an **SError panic on the AP** (UART: `L2C_ERR_ADR: 0x184002010004a0`,
  comm mmiod). Run 44 gated the reads on Fender's state and still lost the
  race: with a cold GPU the firmware powers the idle core off and on every
  few tens of ms (Fender +0x1000 0x11 / 0x0). So the AP no longer reads
  RGX while the firmware manages power (`--rgx` to force it at the end);
  an MMU fault is seen through the firmware itself: its recovery (power
  event 0x16, B+0x4a00 = 1) reads RGX+0xe150.
- **Run 45 (cold GPU, no RGX reads from the AP): the job completes with no
  MMU fault and no recovery** -- STAMP+0 = 1 and +0x10 = 0xc0de 7 ms after
  the kick, no power event 0x16, B+0x4a00 stays 0, no firmware crash, ping
  answered, the iPad alive. Job event ring: one event 0x1 (event 0x4 came
  only with the fault). Power events in 4 s: 0x11 (up) x405, 0x12 (down)
  x405, 0x19 (DVFS) x810 -- the idle GPU is powered up and down every
  H+0x4 (10 ms): the DVFS sample tick wakes it. To find: the setting that
  lets an idle GPU stay off between samples (iOS surely does not wake it
  100 times a second).

The arguments of runs 43-45 (`build/gpu/g4-run45.log`): `P+0xac0=%S P+0xac4=8
X+0x4=@E X+0x8=@F X+0x311=@H B+0x49e8=@L B+0x49ec=@M P+0x9c0=0x01d00000
P+0x9c4=2 P+0x9cc=0x4000 P+0x9dc=1 X+0x150=@N P+0xd34=1 P+0xd3c=1
T+0x7ec=@Y Y+0x0=%Z Y+0x4=8 T+0x7e8=@GEN X+0x198=@XG B+0x4a24=1
X+0x144=@P2 X+0x148=@P3 X+0x14c=@P4 P+0x1abc=1 P+0x1ac0=1 P+0x1ac4=1
P+0x6e0=%TS P+0x6e4=8 P+0x6ec=0x4000 H+0x4=240000`, with `--cold
--channels --kick-cp --job`.

## G6 progress (2026-11-23): the render side of the firmware

The microsequence interpreter (fw 0xb49c) has, besides compute's 0xb / 0xc,
render ops that program the TA (VDM / PM) and 3D (ISP / PBE) registers:

| op | role (from the registers it writes) |
|---|---|
| 5 | **StartTA**: RGX 0x408-0x474 (VDM stream / TA; 0x438-0x460 are upstream's `VDM_CONTEXT_STORE/RESUME_TASK*`), 0xc00-0xd20 (PM / parameter buffer), 0xf038-0xf074, six 64-bit slots through 0x11030 (+0x11ff8 index), 0xf078 (context mapping) |
| 6 | FinalizeTA (0x11f00, 0x688, 0x1720, 0xe230: the stop / flush pattern of FinalizeCompute) |
| 7, 9 | **Start3D** (two variants): RGX 0xf00-0xfd8 (upstream `ISP_RENDER` 0xf08, `ISP_CTL` 0xf38), 0x6a0-0x6d0, 0x618-0x628, 0x3e00, 0x4070, 0x8388 |
| 8, 10 | Finalize3D |

StartTA's arguments (op word +4 -> struct A, +8 -> struct B, as StartCompute's
CA): A[0..7] -> RGX 0xc78 / 0xc88 / 0xc98 / 0xc18 / 0xc08 / 0xc10 / 0xc20 /
0xc28; B, 64-bit pairs: [0] 0xc90, [2] 0xc38, [4] 0xc40, [6] 0xc00, [8]
0xc80, [0xa] 0x428, [0xc] 0xcc0, [0xe] 0x438, [0x10] 0x440, [0x12] 0x448,
[0x14] 0x450, [0x16] 0x458, [0x18] 0x460, [0x1a] 0x470, [0x1c] 0x418,
[0x1e] 0x408, [0x20] 0xf058, [0x22] 0xf060, [0x24] 0xf068, [0x26] 0xf070,
[0x28] 0xf038, [0x2a] 0xf040, [0x2c..0x37] the 0x11030 slots; the op's
+0x20 byte -> RGX+0xf078, +0x2c -> fw 0x1866c (context switch), +0x1c a
cancel flag and +0x5c.. a threshold as in StartCompute. Which register is
the VDM stream base, which the PM free list etc.: to read off an iPadOS
render capture (`gpucap OUTDIR clear | tri`: a clear-only pass and a
one-triangle pass into a linear 64x64 BGRA8 target, three snapshots).

**The firmware's TA and 3D commands** (the pre-processing callbacks in the
per-data-master table at fw 0xb0460: TA 0x1315a, 3D 0x13eb0, CL 0x14f38;
"Unexpected command type" names them AGFTA- / AGF3D- /
AGFCLDataMasterPreProcessingCallback):

| command | type | microsequence | other pointers it reads |
|---|---|---|---|
| TA | 0 (only) | +0x464 / +0x468 (pointer, size) | +0x14 -> 0x90 bytes (context state, as compute's), +0x20 -> 0x74 bytes, +0x24 -> a large object (+0x3300 used: PM state?); an optional second argument of type 5 (fw 0x19f0e / 0x18d20: MMU / PM work, likely free-list growth) |
| 3D | 1 | +0x14 / +0x18 | +0x1c -> 0x90 bytes, +0x24 -> 0xcc bytes |
| 3D | 2 | +0x498 / +0x49c | (a second 3D form) |
| any | 7, 9 | +0x18 of the command header | (shared with CL) |

The queue's data master picks the field: TA queues read +0x464, 3D +0x14
(or +0x498), CL +0x1e8.

**Events and the TA ops' layouts.** The interrupt task ORs the RGX event
status (masked, bit 9 `MMU_PAGE_FAULT` dropped; fw 0x22d86) into fw
0xd75f4, so WaitIRQ's bits are Rogue's `ROGUE_CR_EVENT_STATUS`: compute's
bit 2 is `COMPUTE_FINISHED`; a TA waits for bit 5 `TA_FINISHED` (0x20), a
3D for bit 3 `PIXELBE_END_RENDER` (0x8); bit 7 `PM_OUT_OF_MEMORY` and bit 6
`TA_TERMINATE` go to the dispatcher (fw 0x2920).

- StartTA (op 5, 0x68 bytes): +0x4 -> 8 x u32 (RGX 0xc78 ...), +0x8 -> the
  64-bit block (0xc90 ... 0x408, the 0x11030 slots), +0xc -> a word copied
  to the unit table, +0x10 -> an object (+0xa0 / +0xa8 / +0xb0 -> RGX
  0x2d8 / 0x240 / 0x248 masked to 16 bytes, +0xb8 non-zero = resume via fw
  0xe99c, +0xbc, +0xc4), +0x14 / +0x18 unit table and its value, +0x1c a
  cancel flag (as StartCompute), +0x20 byte -> RGX+0xf078, +0x2c -> fw
  0x1866c, +0x40 -> context state (word 0 = 0: fresh start: RGX 0x3d4 = 1,
  0x3d0 = 0x100, then 0x290, 0x198, 0x258, 0xcb8 each set to 1 and polled
  clear, 0xc68 = 1), +0x48 non-zero sets RGX+0x80 bit 30; the kick is
  **RGX+0x400 = 1** (0x478 for compute).
- FinalizeTA (op 6, 0x40 bytes): +0x4 -> that object (+0xb8 / +0xbc), +0x8
  -> a counter (+0x10), +0xc unit table, +0x14 -> "already finished" flag
  (non-zero: no stop / flush, as FinalizeCompute's +0x2c), +0x18 the
  context for RGX+0xe230, +0x30 / +0x34 the stamp address / value.

**G6 step 1: an empty TA pass runs (2026-11-23, `build/gpu/g6-run*.log`).**
`gfx-init.py --job --kick-ta` builds a TA queue on channel 0 (queue object
KT), a type-0 command (CMDT: +0x14 / +0x20 / +0x24 objects, microsequence at
+0x464) and StartTA -> WaitIRQ(`TA_FINISHED`) -> FinalizeTA -> End, with a
VDM stream that is one STREAM_TERMINATE (0xc0000000, its base in StartTA's
B+0x78 -> RGX+0x408). What it took:

- run 1: StartTA's +0xc is a **pointer** (the word it points to goes into the
  unit table); the PM start-up sequence (RGX 0x290 / 0x198 / 0x258 / 0xcb8 /
  0xc68) already completed;
- runs 2-3: the TA kicks and writes: an MMU fault, write, requester 7
  (Tiling) at VA 0, then with every slot at a distinct unmapped VA: a write
  by requester 6 (**PM**) at **TAO+0xa8 (-> RGX+0x240) + 0x40**;
- run 4 (`--ta-probe`: GPU VA 0x300000.. on real pages, one 0x7000 slice per
  slot): **STAMP+0x20 = 1, no fault** -- the TA finished. Written slices:
  B+0x8 (-> RGX+0xc38): `00000060 00006000 ...` (0x20 bytes) and TAO+0xa8
  (-> RGX+0x240): a table of `{n << 24, 0x3070}` pairs (0x7c bytes); then a
  crash in the TA post-processing callback (fw 0x13758,
  "AGFTADataMasterPostProcessingCallback");
- run 5: **the TA command's completion record is CMDT+0x554 (address) /
  +0x55c (value)** (CL's: +0x26c / +0x274): STAMP+0x30 = 0x7a, no crash, no
  fault, ping answered.

**Does the TA really run? (runs 6-11).** StartTA is 0x6c bytes: its +0x68
word is unused (a WaitIRQ placed there is skipped: run 8, an impossible
event mask, still completes), so in runs 4-5 FinalizeTA came straight
after StartTA and completed through its fast path. A real WaitIRQ after
StartTA (+0x6c) never ends -- not on `TA_FINISHED` (bit 5), nor on bits
0-5 / 8, nor on 6 / 7 (runs 9-11) -- **yet the VDM did read the stream**:
afterwards RGX+0x408 = 0x203c00 (the base), **+0x410 = 0x203c04** (the
current pointer, one word on), +0xc30 = 0xc0000000 (the last word read),
+0x430 = 0x11, +0xd20 = 1; no fault. So an empty stream ends without a
`TA_FINISHED` here; bit 6 (`TA_TERMINATE`) is taken by the dispatcher (fw
0x2920). To find with a stream that has geometry. Also read off the
registers: StartTA's B slots land as decoded (0x418, 0x428, 0x438-0x460,
0x470, 0xc38, 0xc40, 0xc80, 0xc90, 0xcc0 hold the probe slices).

**3D layouts (static).** Start3D (op 7): +0x4 -> 32-bit block ([0] 0xf18
`ISP_BGOBJDEPTH`, [1] 0xf80 `ISP_BGOBJVALS`, [2] 0xf88 `ISP_AA`, [4] 0x6d0,
[5..7] 0x618 / 0x620 / 0x628, [8] 0xf38 `ISP_CTL`, [9] 0x3e00, [10] 0xfd8,
[11] 0x4070, [12..17] 0x11d0..0x11f8); +0x8 -> 64-bit block ([0] 0xfa8,
[2] 0xfb0, [4] 0xfb8, [10] 0xf48, [12] 0xf50, [14] 0xf58, [16] 0xf60, [18]
0xf68, [20] 0xf28, [22..28] 0x6a0..0x6b8, [30] 0xf30, [0x20..0x2b] six
slots via 0x11020, [0x2c..0x37] six via 0x11080, [0x38] 0xfd0); +0x14 /
+0x18 pointers to words that must be 0, +0x1c cancel flag, +0x24 an object
(+0x24 -> +0xc8 counter), +0x28 byte -> 0xf078, +0x34 context, +0x48 ->
resume flag, +0x6c..+0x74 the threshold; kick **RGX+0xf00 = 1**.
Finalize3D (op 8, 0x50): +0xc / +0x10 stamp, +0x14 an object (+0xb8 /
+0xbc / +0xc8), +0x18 a counter, +0x20 the unit table (0x28-byte entries),
+0x24 the "already finished" flag, +0x30 an object (+0x864), +0x34 the
context for RGX+0xe230; its completion waits for event bit 8
(`PM_3D_MEM_FREE`). The 3D command's completion record (fw 0x1468c,
"AGF3DDataMasterPostProcessingCallback", type 1): **+0x89c address / +0x8a4
value** (+0x8b4 / +0x8b8 the stamp pair); the command is at least 0x918
bytes.

**G6 clear DONE (2026-11-23, `build/gpu/g6-3d-run*.log`): a TA + 3D pass
clears a 64x64 target through the background object** (the triangle,
G6's second half, followed on 2026-11-24: "G6 step 4"). Command line (base arguments
as for G4 / G5, `g4base3`):

    gfx-init.py 3 <base> --cold --channels --job --kick-ta --pm --clear --kick-3d --va0 \
        P+0x10=0x400000 P+0x18=0x400000          # CLEAR=r,g,b,a picks the colour

Result: STAMP+0x20 = 1 / +0x30 = 0x7a (TA), **STAMP+0x40 = 1 (Finalize3D),
+0x50 = 0x3d (the 3D command's completion record), all 4096 pixels
0xff4080bf** = (0.25, 0.5, 0.75, 1) as BGRA8, the colour of the iPadOS
capture. Controls: CLEAR=1,0,0,1 -> 0xffff0000 x4096, CLEAR=0,1,0,0.5 ->
0x8000ff00 x4096 (runs 30-32); without the background / end-of-tile
programs the target stays at its fill pattern (run 27).

### G6 step 2: the 3D job (runs 1-17)

- **The op sizes** (interpreter fw 0xb49c, the size it advances by):
  Start3D (op 7) **0x78** (words up to +0x74 used), Finalize3D (op 8)
  **0x50**. Start3D: +0x4 / +0x8 the register blocks, +0xc the PM slot,
  +0x10 unit table (0x28-byte entries; it writes +0x1c = 1, +0x24 = its
  +0x20), +0x14 / +0x18 pointers to words that must be 0 (else the
  partial-render path), +0x1c cancel flag, +0x24 the command, +0x28 byte ->
  RGX+0xf079, +0x34 free-list id (fw 0x1866c), +0x48 pointer to 0 (else
  resume, fw 0xe816), +0x6c..+0x74 threshold (cancel when the 64-bit
  {+0x6c, +0x70} >= *(+0x74)); kick RGX+0xf00 = 1. It loads the PM slot
  into the 3D side with fw 0x17606(slot, 1): +0xa8 -> 0xf20, +0xa0 -> 0x2e0,
  +0x0 / +0x8 -> 0x278 / 0x2c8, the PM catalog words +0x10.. +0x98 -> RGX
  0xe050 / 0xe070 / 0xe090 / 0xe0e0 / 0xe100 / 0xe120 (the TA side, called
  with 0: 0xe008 / 0xe028 / 0xe048 / 0xe098 / 0xe0b8 / 0xe0d8).
  Finalize3D: +0xc / +0x10 stamp address / value, +0x14 PM slot, +0x18 a
  counter block, +0x20 unit table, +0x24 pointer to the "already finished"
  flag, +0x30 the command, +0x38 a pointer it writes on the store path,
  +0x48 bytes to skip on that path (0x50). With the flag set it waits for
  event bit 8 (`PM_3D_MEM_FREE`) and then writes the stamp.
- **The 3D command** (type 1, CMD3, fw 0x13eb0 / 0x1468c): +0x14 / +0x18
  microsequence, +0x1c context state (0x90), +0x20 the free-list descriptor
  (the TA command's: retirement compares *(+0x50) with +0x0), +0x24 the PM
  slot (0xcc bytes; +0xc0 -> a header the retirement writes), completion
  record +0x89c / +0x8a4. Queue on channel 1: ring W+0x1400, host write
  index G+0x14, doorbell (0x21 << 56) | (3 << 48) | 1.
- run 1: a 3D job alone crashes the firmware in fw 0x17a18 (from 0x1866c):
  **free-list table fw 0xd6ca0 has no entry -- the TA's dispatch registers
  the TA command's +0x20 descriptor as id 0**, so the 3D runs after a TA.
- run 2: a crash on a null microsequence -- the firmware scans all channels
  whenever it wakes (the TA's completion woke it) and read the 3D command
  half-written: **the host write index is set last, right before the
  doorbell**.
- run 3: Start3D runs (unit table, RGX+0xf20 = the slot's +0xa8); IPF MMU
  fault in context 4 at VA 0: the PM had nothing (zero free list).

**The TA with a parameter manager (`--pm`, runs 4-15).** Region G3 (1 MiB
at GPU VA 0x400000): code heap at +0 (PDS base = USC base = 0x400000),
render target +0x50000, region headers +0x54000 (-> 0xc38), TPC +0x55000
(-> 0xc40), macrotile array +0x56000 (slot +0xa8 -> 0x240), MLIST +0x57000
(slot +0xa0 -> 0x2d8), VHEAP table +0x59000 (slot +0xb0 -> 0x248), free list
+0x5b000 (u32 physical page numbers; descriptor +0x1c base -> RGX 0x210,
+0x28 -> 0x220, +0x40 top = n - 1 -> 0x224, +0x48 / +0x4c -> 0x390 / 0x394,
fw 0x17a10), PM pages +0x80000. StartTA's 32-bit block: 0xc78 = 0x88, 0xc88
= 2, 0xc98 = 0x3f003f (PPP_SCREEN), 0xc18 = 0x1001 (TE_SCREEN, 32x32 tiles),
0xc08 / 0xc10 = 0x10100c (TE_MTILE1 / 2, macrotiles as upstream
`pvr_hwrt.c`), 0xc20 = 16, 0xc28 = 0x20000; 64-bit: 0xc90 = 0x1e3ce508, 0xc80
= 0x88, 0x428 / 0xcc0 = buffers, 0x470 = a PDS HALT. VDM stream: PPP_STATE
(3 words) -> the PPP terminate block `02000000 00040001 00000000` (Mesa
`ppp.xml`: clip to tile 1 / 1), then STREAM_TERMINATE. These values work;
which of them matter is to check.

- run 4: PDS base 0 -> an MCU read fault at VA 0 (a PDS task of the TA);
  with a code heap of PDS HALTs: **the TA completes, the VDM reads the whole
  stream (RGX+0x410 = base + 0xc), PM takes 45 pages (0x224 0x7f -> 0x52),
  MLIST gets 5 entries, region headers are written for every tile -- the
  same words as in the iPadOS capture (`00000020 00002000 00600000
  60000000 0` ...) -- and FinalizeTA saves the PM state into the slot**
  (+0x10 = 0xe008 = a VCE page catalog, physical | 1; FinalizeTA: wait for
  RGX+0x430 bit 0, save 0x270 / 0x2c0 / 0xe0xx, 0xc50 = 0x40000000, stop /
  flush). Start3D loads it (RGX+0xe050 = the catalog).
- **No WaitIRQ for the TA**: with the WaitIRQ (TA_FINISHED, now really
  executed at +0x6c) the job never ends (runs 6-8, RGX JONES_IDLE 0x8328 =
  0x3f7f: everything idle). FinalizeTA itself waits for the VDM and closes
  the scene; the TA's stamps come only that way.
- runs 9-14: the 3D faults in the IPF (requester 29) in PM context 4 at VA
  0; a hand-made catalog in slot +0x30 (-> 0xe070) moves it to context 5
  at VA 0x2080000: the 3D reads PM contexts the TA never created. **The
  difference is the macrotile array: with 128 free-list pages the PM
  start-up writes none; with 1 page (runs 15-16) it cannot allocate and
  writes it** -- `{n << 24 | offset, 0x4040 + n / 4}` x 16, the offsets of
  the iPadOS capture's array. Then the 3D runs without a fault (event
  `ZLS_FINISHED`, RGX+0x1090 = 3) but never ends: no end-of-tile program,
  no `PIXELBE_END_RENDER`. `PM_PAGES=1` is the default for now; why the
  normal start-up skips the array is to find.
- Without a page at GPU VA 0 the end-of-tile writes to VA 0 (write fault,
  requester 9) after the pixels are out (run 33): `--va0` maps one.

### G6 step 3: the background object and the end-of-tile program (runs 18-34)

**The iPadOS clear, decoded from the capture.** The process's 64 KiB heap
at CPU 0x6060f4000 is the PDS / USC heap (the record's 0x470 = 0x100 holds
a PDS program there); the record's descriptors point into it:

| what | registers (Start3D blocks) | value | content |
|---|---|---|---|
| background PDS | 0x6a0 `PDS_BGRND0` (64-bit B[11]) | 0x2180 << 32 \| 0x2200 | 0x2200: data `{0x20c0, 0x10}` + `f4000002` (DOUTU END -> USC 0x20c0, the colour-copy shader); 0x2180: `f4000800 d3000000` (DOUTD END: the uniforms) |
| uniform data | 0x6b0 `PDS_BGRND2` (B[13]) | 0x3c030 | `{u64 -> colour, u64 0x90000004}` (4 words into the common store) |
| sizes | 0x6b8 `PDS_BGRND3` (B[14]) | 0x0080000000800000 | |
| end-of-tile code / data / info | 0x618 / 0x620 / 0x628 (A[5..7]) | 0x2500 / 0x3c040 / 0x402 | `f0000800 f4000012 d3000000`: DOUTD of the image state, DOUTU END -> USC 0x2440 |
| end-of-tile data | | | `{u64 -> image state, u64 0x90000007, DOUTU {0x2440, 0x10}}` |
| image state | | 7 words | `fb050502 00007e01 <VA >> 1> 80018000 0 c0000000 0` (BGRA8, linear, 256-byte rows) |

The PDS data at heap+0x3c030 sits in the capture's region 0x60615c000
(its +0x30 / +0x40 hold exactly these words); the colour and the image
state are the 0x60614c000 region's +0x48 / +0x58 (written at commit). The
DOUT operand fields that fit every program seen (iPadOS and our G5 DMA
task): bits 3-7 = the 64-bit constant with the address (DOUTD) or the
DOUTU word, bits 11-14 = the 64-bit constant with the DOUTD control word.

`--clear` copies the heap window 0x2000-0x2580 of the capture (these four
programs and the two USC shaders; Apple's code, read from
`build/gpu/ioscap/rclear` at run time, not in git) to our heap at the same
offsets, writes the two PDS data blocks, the colour and the image state
(target address >> 1) itself, and sets the registers above. What it took:

- runs 18-26: a USC fault in the TA once `--clear` placed data at
  G3+0xe000: **the TA reads far beyond the 0x428 / 0xcc0 buffers**; with
  that data elsewhere it is clean (now 0x8000 bytes are kept for them);
- runs 28-29: an EOT / background code address of 0x200000 faults at VA
  0x600000 -- **both programs run, and the PDS base is used 2 MiB-aligned**:
  0x410000 acted as 0x400000, so the programs had been fetched from the
  render target. With the heap at G3+0 and P+0x10 = P+0x18 = 0x400000:
  **the clear (run 30)**.

### G6 step 4: the triangle (`build/gpu/g6-tri-run*.log`)

**G6 DONE (2026-11-24, runs 9-12): the iPadOS triangle, replayed from our
own VDM stream, renders on the A10 -- byte for byte the target iPadOS
drew** (`build/gpu/g6-tri-run10.rt` vs the capture's 0x1_00000000: 0
differing pixels; 1024 red pixels in the bottom-left quarter, 3072 of the
background colour). Control (run 11): CLEAR=0,0,1,1 and the corners moved
to (0,-1) (1,-1) (1,1) -> a blue background and the triangle in the
bottom-right quarter. Command:

    gfx-init.py 3 <base> --cold --channels --job --kick-ta --pm --tri --clear --kick-3d --va0 \
        P+0x10=0x400000 P+0x18=0x400000         # TRI_XY=x0,y0,x1,y1,x2,y2 / CLEAR=r,g,b,a

The two things that were missing (runs 1-9):

1. **RGX+0x470 must hold a real PDS program**: the capture's record has
   0x600 there -- data `{0x540, 0x10}` + `f4000002 d3000000`: it launches
   a USC task (heap 0x540). With a PDS HALT instead (all earlier runs) the
   vertex shader runs but no primitive reaches the tiling engine and the
   PM writes no macrotile array; with Apple's program (run 7) the TE writes
   tile control streams (`2c070000 00000009 c0000000`), MLIST grows to 11
   entries and the PM writes the macrotile array with the full 128-page
   free list. What the USC task does is to find. (This is also why the
   empty-scene clear needed the 1-page free list: to recheck with a 0x470
   program.)
2. **With that program the TA raises `TA_FINISHED`**, so a WaitIRQ on it
   before FinalizeTA is now right (`TA_REALWAIT`, the default with
   `--tri`): without it FinalizeTA saved the PM registers before the tiling
   engine had registered its page catalog, slot +0x30 (-> RGX 0xe028 / the
   3D's 0xe070) stayed 0, and the 3D's IPF faulted in PM context 4 on the
   tiles with geometry (run 7: only the right half was drawn). With the
   wait the slot gets the TE catalog (+0x30), +0x70 and +0x90 (run 9).

Open: after the job the firmware no longer answers a ping (runs 9-12; the
stamps and pixels are all there) -- to find. Next: our own programs
instead of Apple's ("legalization").

### G6 legalization (2026-11-24, `build/gpu/g6-own-*.log`, `g6-eot-*.log`): our own programs

`--own` (with `--pm --tri --clear`) draws the same picture from our own
pieces; **the target is again byte for byte the one iPadOS drew** (0
differing pixels), and CLEAR / TRI_XY move it (green background, an
upside-down triangle: 2048 pixels). What is ours now:

- **USC code**: built by Xcode's offline compiler (`metal-tt -arch
  applegpu_g9p`) from our Metal source in `gfx-init.py` (OWN_SRC): `tri_vs`
  and `tri_fs` come out byte for byte as the code in the iPadOS heap (the
  capture ran the same source); the background shader is our `bg_fs`
  (`return c;` for a `constant float4 &c`): four moves from the common
  store (registers 2..5) and the pixel emit `1c21 0008`, the shape of
  Apple's clear shader. Compiler output stays in `build/gpu/isa/`.
- **PDS programs and data**, written from the decoded DOUT fields (bits 3-7
  the 64-bit constant with the DMA address / DOUTU word, bits 11-14 the one
  with the DOUTD control word): vertex `DOUTW, DOUTU (cc), HALT` + `{DOUTU
  -> tri_vs, 0x80000000}`, shared data `DOUTD END` + `{corners, 0x90000006}`
  (the compiler keeps `tri_vs`'s constant array in common store 0..5),
  fragment `{DOUTU -> tri_fs}` + `DOUTU END`, background `{DOUTU -> bg_fs}`
  + `DOUTU END` and texunicode `DOUTD END` + `{colour, 0x90004004}` (4 words
  to common store 2), end of tile `DOUTD, DOUTU END, HALT` + `{image state,
  0x90000007, DOUTU}`.
- **RGX+0x470's USC task is one instruction: `5e0100a0`**. Our compiler's
  empty kernel (`0001 4001 0000 ... 0080`) never lets the TA raise
  `TA_FINISHED`; the 0x5e instruction the compiler ends vertex shaders with
  (`5e11 00c1 03`, `5e15 04c1 03`) does, in this short form without
  outputs; with the empty kernel's first instruction before it, it does
  not (runs own-470).
- **PPP state and the VDM stream**: every word from Mesa's `ppp.xml` /
  `vdm.xml` fields: varying words 0-2 (none), PDS pointers, OUTPUT_SEL
  (vertex size 4, rhw), ISPCTL (upass 0xf, back face present), ISPA
  (depth compare ALWAYS, depth write off, point / line width 0xf), ISPB
  (stencil ALWAYS), region clip (tiles 0..1) + viewport (32, 32 / 32, -32 /
  0, 1), WCLAMP + PPP_CTRL (clip mode, flat shading vertex, wclamp), ISP
  DBSC, the terminate block; VDM_STATE (vertex PDS code / sizes, data),
  PDS_STATE (shared data), INDEX_LIST (3 auto indices, TRI_LIST). The
  capture's stream-out block is not needed.

**The end-of-tile USC code is ours too (runs eot-*)**: three instructions,
in 16-bit little-endian words (an instruction runs to the first word with
bit 8 clear):

| words | instruction |
|---|---|
| `0198 0100 0101 0120 0154 0038` | o0 = 0: the move-immediate form the compiler emits for constants (`0198 01rr W2 W3`, W2 / W3 = the value, `0154 0038` after it) |
| `011c 4100 0800` | the pixel emit, opcode 0x1c: in our compiler's fragment shaders `111c 0800` / `211c 0800` / `411c 0800` for 1 / 2 / 4 data registers (W0 bits 12-14); here 0 registers and the extra word `4100` -- its bit 14 makes it the PBE store with the image state the PDS put in the common store (`1100` instead: an MMU fault) |
| `8000` | end |

How it was found: the capture's end-of-tile code split by the length rule
into o0 = (a value) with a 16-bit pack, an emit `051c 5100 0800`, a jump to
the end, the end word; dropping the jump changes nothing, dropping the
first instruction leaves register garbage in the target, and o0 = 0 from
the compiler's form works; of the emit's bits, W0 bit 10 and W1 bit 12 are
not needed, W1 bit 14 is. With it, **`--own` reads nothing from the
captures** (run with `build/gpu/ioscap` moved away: still 0 differing
pixels; control CLEAR=0.5,0,0.5,1 with the corners (-1,-1) (1,-1) (0,1): a
purple background and the upright triangle, 2048 pixels).

**On the screen (2026-11-24, run screen1).** `--screen` maps the display's
framebuffer (simpledrm's, from the device tree's `/chosen/framebuffer`:
PA 0x8_bc2dc000, 1620x2160 x8r8g8b8, stride 6528 = 102 x 64 bytes) at GPU
VA 0x1000000 (seven page tables) and points the end-of-tile image state at
it (word 2 = address >> 1, word 3 bits 15.. = 6528 / 64 - 1): **the GPU
draws the 64x64 picture straight into the display**, seen on the iPad (on
the wallpaper's Tux), and read back from the framebuffer's PA: the
background and the red triangle at (784, 1048), the wallpaper around it.
(`/dev/fb0` is simpledrm's fbdev shadow, stride 6480: it does not show
what the GPU wrote.)

**Bigger targets: the whole screen (2026-11-24, runs screen-*, off-*, dvfs-*,
full-dvfs).** `RT_SIZE=WxH` sizes everything from the target as upstream
`pvr_hwrt.c` does for 32x32 tiles: PPP_SCREEN, TE_SCREEN, TE_MTILE1 / 2 (4 x
4 macrotiles of mx x my tiles, mx / my rounded up to 4), 0xc20 = mx * my,
ISP_MTILE_SIZE (0xf18) = mx << 16 | my (other values change which tiles
the ISP visits: 16 -> only a 16 x 16-tile corner), ISP_RGN (0xf28) = mx * my
* 5 / 4 words, the PPP terminate and region clips, the viewport, and the
image state's size: **width - 1 in word 0 bits 27-31 + word 1 bits 0-8,
height - 1 in word 1 bits 9-22** (swapped, a 768 x 640 target came out 640
wide). Region headers and TPC live in region PMB at GPU VA 0x300000;
`--offscreen` renders into a RAM buffer of ours instead of the display.

What stopped big renders: **the firmware's DVFS sample tick powers the GPU
off in the middle of a 3D render.** Symptoms: the 3D drew a random 550-630
tiles and then waited forever (Finalize3D on `PM_3D_MEM_FREE`), every unit
idle (JONES_IDLE 0x3f7f), no fault, the power ring full of 0x11 / 0x12
(up / down) pairs; smaller renders (up to 480 tiles) usually finished in
time. With the sample period H+0x4 = 240000000 (10 s instead of 10 ms)
there is one power-up and no power-down, and **1024x1024 off screen (1024 of
1024 tiles) and the full 1620x2160 screen render completely** (the
background everywhere, the triangle in the left half -- seen on the iPad). Why (runs busy-*, 2026-11-24):

- **The unit tables are the firmware's per-data-master blocks.** P2 (X+0x144),
  P3 (X+0x148), P4 (X+0x14c) hold "this data master runs a job" flags:
  P2+8 (TA), P3+0x24 (3D, 1..3), P4+0xc (CL); fw 0x3b74 matches them with
  the CONTEXT_MAPPING bytes 0xf078 / 0xf079 / 0xf07a, the main loop's idle
  check (fw 0x1dcc6 -> 0x61a0) powers off only while all three are 0. The
  Start / Finalize ops set them through their unit-table argument: StartTA's
  +4 = 1 in 0x14-byte entries is P2+8 with the table at P2+4, Start3D's
  +0x1c = 1 / 3 in 0x28-byte entries is P3+0x24 with the table at P3+8. Our
  own tables (UT2 / UT3) bypassed that; `gfx-init.py` now passes P2+4 /
  P3+8 (`UNIT_TABLES=own` for the old way), and P3+0x24 = 1 during the 3D.
- **That is not enough**: the power ring still shows the same 64 up / 64
  down pairs and big renders still stop. The power-down comes from the
  power task's DVFS / power-gating decision (fw 0xab50, the sub-state
  machine fw 0x7c4c -> 0x1c0ce, event 0x12), which works on the power
  struct H (X+0x311): a mode at +0xc, perf level / utilisation at +0x50 /
  +0x54, busy-time counters at +0x4bf4.. -- all 0 in ours except the period
  H+0x4. **The ADT's `sgx` node has the inputs the kext puts there**:
  `gpu-power-sample-period = 16`, `gpu-perf-tgt-utilization = 95`,
  `gpu-num-perf-states = 6`, `gpu-pwr-filter-time-constant = 313`,
  `gpu-perf-filter-time-constant(2) = 5 / 50`, `gpu-perf-integral-gain2`,
  `gpu-perf-proportional-gain2`, `gpu-device-max-power = 3348`, ... Which
  goes to which H offset: to read from the kext's power init (AGXG9P).
  Until then render runs use H+0x4 = 240000000 (no sample during a job).
- **How the kext fills H** (AGXG9P, kernelcache 22H355): the init-data
  builder at 0xfffffff005f96424 puts the firmware VAs of its buffers into
  X (obj+0x258 / +0x260 / +0x268 -> X+0x144 / 0x148 / 0x14c, **obj+0x270 ->
  X+0x311**, the buffer table at 0xfffffff005f7a890 pairs obj+0x230 (CPU) with
  it); fw 0xfffffff005f7c2c8 fills that buffer: zero 0x4d10 bytes, copy
  0x160 bytes (obj+0x878) to H+0x64, H+0x4 = H+0x8 = the sample period (a
  device method; the ADT's `gpu-power-sample-period` is 16 ms and the kext
  keeps it x 24000), H+0x4bf4 = a timestamp, H+0xc = 0 / H+0x10 = 4 (the
  power sub-state fw 0x7c4c switches on), H+0x14 = 1.0f, H+0x2c = H+0x30 =
  1, H+0x54 = 100, the perf controller at H+0x8e8..0xb34 from the device's
  ADT-derived fields (0xf958.., `gpu-perf-*`), and P+0xe58.. the perf-state
  table (MHz). Setting H+0x8 / 0x10 / 0x14 / 0x2c / 0x30 / 0x54 that way
  did not stop the power cycling (run hk-1k); the 0x160-byte table and the
  controller block are still missing.
- **The fix (runs nodvfs-*, 2026-11-24): let the host own the power state
  and keep the firmware's power / DVFS path off.** With **P+0xd34 = P+0xd3c
  = 0** the firmware only records a software "powered" flag and never runs
  its Fender perf-state requests nor its DVFS / power-gating step; the host
  powers the core itself before the init message (`--power`, Fender
  +0x1000 = 0x10 | state, `POWER_STATE=6` the fastest; G1.5: states 1..6
  are the perf states). Then: **no power event at all during a run, the
  firmware answers a ping after the job (it did not before), 1024x1024 off
  screen (1024 of 1024 tiles) and the whole 1620x2160 screen render at the
  normal H+0x4, 64x64 is still byte for byte iPadOS's.** (Run 43's lesson
  "--power skips the firmware's power-up" was with P+0xd34 / 0xd3c = 1.)
  The arguments for render runs: `--cold --power` + `POWER_STATE=6` +
  `P+0xd34=0 P+0xd3c=0`; the H+0x4 work-around is no longer needed. The
  firmware's own DVFS (H, the kext's power-struct init below) is for later,
  if we want it at all.
- **The firmware's trace** (`TRACE_DUMP=1`, flags in A+0: 1 = job events;
  the trace ring is X+0xec / +0xf0): a 1024x1024 run shows StartTA (code 0),
  FinalizeTA (1), the TA's completion (0xa, 0x7a), then 850 ms later
  Start3D (4), code 0x13, and **Finalize3D (5) only 0.3 ms after Start3D**:
  the WaitIRQ on `PIXELBE_END_RENDER` returns long before 1024 tiles can be
  drawn, and Finalize3D then waits for `PM_3D_MEM_FREE`. Why the event comes
  that early, and where the power-downs fall relative to it (the power
  ring overflowed before the 3D in that run) -- next. Ruled out on the way: the free-list descriptor's +0x28,
1024 PM pages, Apple's own end-of-tile code, the per-pipe 0xfc0 buffers,
the Start3D values 0x6d0 / 0x3e00 / 0x4070 / 0xfd8 / 0x11080, the
end-of-tile PDS info, ISP_CTL's pipe count (one run with pipe_enable 2
finished by chance).

**The 1-page free list is no longer needed**: with the 0x470 program the
clear alone also runs on the 128-page free list with the WaitIRQ (runs
own-clear / own-clear128: 4096 yellow pixels either way) -- the missing
macrotile array of runs 9-14 was the missing 0x470 task. `--own` now
defaults to 128 pages and the wait.

Still to name rather than reuse: the StartTA / Start3D register values our
runs settled on (0xc78 = 0x88, 0xc90, 0x3e00, 0xfd8, 0x4070, 0x11080 = 0x1d3,
0x6d0 = 0x1f, ...), the image state words, the PDS DOUTW of the vertex
program.

The way there:

**A complete `tri` capture** (`gpucap rtri2 tri`, `build/gpu/ioscap/rtri2`):
gpucap now also saves every readable region within 256 MiB of the tagged
ones (`GPUCAP_ALL=1`: every region up to 4 MiB anywhere). The regions the
first captures seemed to lack were there all along, at other GPU addresses
than assumed; the layout, read off pointers and the 0x4000 guard gaps:

| GPU VA | CPU (rtri2) | content |
|---|---|---|
| 0x0 | 0xcda0dc000 (64 KiB) | PDS / USC heap = PDS base = USC base |
| 0x30000 / 0x3c000 | 0xcda158000 / 0xcda160000 | PDS data (vertex: 0x302c0 / 0x302d0; background / end-of-tile: 0x3c030 / 0x3c040) |
| 0x1_00000000 | 0xcda0d0000 | the render target (the image state's address) |
| 0x1_00008000 | 0xcda0ec000 (64 KiB) | PPP state blocks, the vertex shader's constants (+0x620) |
| 0x1_00040000 | 0xcda138000 | the VDM stream |
| 0x1_00070000 | 0xcda168000 | {64, 64}: a scissor table (to check) |
| 0x1_000f4000 ... 0x1_00148000 | 0xcda1e8000 ... 0xcda220000, 0xc000 apart | PPP state (0x1_0013c000 = 0xcda218000), viewport block (0x1_0014e3e8) |

The triangle is the stream's fifth batch: VDM_STATE (vertex PDS code
0x440, output size 4), PPP {varying header}, PDS_STATE (shared-data DMA
0x3c0, data 0x302c0: the six corner floats from 0x1_00008620 into the
common store), VDM_STATE (vertex PDS data 0x302d0: DOUTU -> USC 0x2c0),
PPP {fragment PDS: shader 0x1c0 (DOUTU -> USC 0x40), texunicode 0xc0,
sizes, varying 0x140}, PPP {output select}, PPP {ISP control / A / B},
PPP {region clip + viewport 32 32 32 -32 0 1}, PPP {WCLAMP 1e-20, PPP_CTRL
0x4a0}, PPP {ISP DBSC}, PPP {stream-out}, INDEX_LIST (3 indices,
TRI_LIST), PPP terminate, STREAM_TERMINATE. Every PPP block starts with
Mesa's `STATE_HEADER`; the words decode with `ppp.xml`.

`--tri` (with `--pm --clear`) rebuilds the low window at our PDS base,
copies the blocks into G3+0x6c000 and runs this batch from its own stream.
Results so far: the VDM reads the whole stream (RGX+0x410 = base + 0x84),
**the vertex PDS program and the vertex shader run** (pointing their code
at an unmapped offset faults PDS / USCS: runs 4-5), no fault -- but the PM
gets no geometry (only its start-up pages), so nothing reaches the tiling
engine, and the 3D then faults in the IPF as for an empty scene. Suspects:
StartTA's register values (Apple's are filled by the kernel, not in any
capture; 0xc90 = 0 makes the vertex USC fault, run v3), the PPP / TE side
after the vertex shader. To find.

Open: our own background / end-of-tile programs and shaders instead of
Apple's (metal-tt fragment shader, PDS from the formats above); the PM
start-up with a real free list (macrotile array); which StartTA / Start3D
register values are needed; a target that is not 64x64.

### G6: render captures from iPadOS (2026-11-23, `build/gpu/ioscap/rclear`, `rtri`)

`gpucap OUTDIR clear` / `tri` on iPadOS 18.7.9: a render pass into a linear
64x64 BGRA8 target backed by a shared buffer. Results: `clear` -> every
pixel 0xff4080bf (= (0.25, 0.5, 0.75, 1) as BGRA8), `tri` -> red only where
the triangle is (px(0,63) = 0xffff0000). Regions (CPU address-size; the
address differs per run, the order and sizes do not):

| region | clear / tri nonzero bytes | first reading |
|---|---|---|
| 0x4000 at +0xd0000 | 0x4000 | the render target (the clear colour) |
| 8 KiB, `80000001 000400dc ...` | 0x19c / 0x20c | **the VDM control stream**: four repeats of {`80000001 ...` (block type 4), `20000001` (PDS state), `48002f80`, `50002fa0`, `60000000`, index-list-like `00000301 0013c0xx`}, ending `c0000000` (type 6 = Rogue STREAM_TERMINATE) -- the clear is drawn: four batches, no draw of ours |
| 8 KiB, `00000001 00148020 ...` | 0x1e4 / 0x1fc | PDS programs and data: Rogue load / store / WDF sequences (`d5000280 d1800002 d1840000 b3800004 c8000001 d5800003 d1000000 db000000 d6000201 d1800000 d0800004 f000282b d3000000`), data blocks `{0x14e3b0 0x100001 ...}` |
| 8 KiB, `0000001f 00240000 103e0f00 ...` | 0xd8 / 0x110 | four identical 13-word state blocks, each ending with a pointer (0x300a0 / 0x30130 / 0x301c0 / 0x30250): per-batch PPP / ISP state |
| 8 KiB, `fb050502 00007e01 ...` | 0x54 / 0x54 | descriptors + **the clear colour as constants** (+0x48: 0.25 0.5 0.75 1.0) and 1.0 / 0.0 pairs |
| 8 KiB, `00064000 00000001 ...` | 0x44 / 0x44 | DMA data (`{ptr, ptr, ptr, 0x10000008, 0x10010002, 0x90014006, ptr, 0x90000004, ptr, 0x90000007}`), the shape of compute's binding DMA |
| 0x244000 (appears at submit) | 0x90 / 0xa8 | parameter buffer (PM writes it) |
| 0x2c000 (at submit) | 0x400 / 0x400 | a page list (`0089ce38 0089ce39 ...`): the PM free list |
| 0x10000 (at submit) | 0x400 / 0x3fc | `00002000 00600000 60000000 ...`: PM / region-header state |

**The render record.** The 0x4000 region that starts `0000000f 000000ac`
(`rclear`: 0x60612c000) is written at commit and is one render pass (the
gpucap "scheduled" snapshot is taken after the GPU ran: the target is
already drawn there, so it equals "done"):

- +0xe4.. four GPU pointers `0x1_00148020 / 24 / 28 / 2c` (into the PDS
  programs' region), +0x104.. two PDS task pairs `{24001002 00030005
  <data> 0 00004820 <code>}` / `{21001002 00030021 ...}`: the VDM context
  store / resume tasks (StartTA's RGX 0x438-0x460 slots);
- +0x34c and +0x5ec: two **background-object descriptors** `{PDS data,
  PDS code, ..., 0x3c030 / 0x3c000, ..., 0x2500, 0x402}` (code / data
  0x2180 / 0x2200 and 0x2340 / 0x23c0 in the code heap);
- +0x5c4: `3f800000 00000300 0000000f` -- Rogue's `ISP_BGOBJDEPTH` = 1.0f,
  `ISP_BGOBJVALS` = 0x300 (mask + bg tag enable), `ISP_AA` = 0xf, filled
  at commit; `ffffffff` triples at +0x2b8 / +0x5e4;
- +0x7ac: 0x40 0x40 1 8 8 (the 64x64 target; 8 x 8 = its tiles at 8 px?
  to check).

Its TA part (+0) and 3D part (further on) feed StartTA / Start3D, but the
PM side -- parameter buffer, free list, region headers, macrotile array --
is built by the kext and only its results show up in user memory (the
three regions that appear at submit). The Linux side has to allocate and
initialise those itself; upstream's `pvr_hwrt.c` / `pvr_free_list.c`
compute the same structures for Rogue.

Next: map these onto StartTA's / Start3D's arguments (which register gets
the stream base, the free list, the region headers, the target), decode the
VDM / PPP words against Mesa's `csbgen/rogue/{vdm,ppp,ipf}.xml`, then build
the clear-only TA + 3D job under Linux.

## G5 progress (2026-11-23): a kernel record, the PDS and USC bases

`gfx-init.py --job --kernel [--rec8] [--pad] [--usc] --rgx` (logs
`build/gpu/g5-run*.log`; runs whose extra arguments a zsh loop never passed
were moved to `build/gpu/bad/` and are not used here). With `--rgx` the
script stops the coprocessor first and reads RGX only if the core was left
on, so the fault status can be read without racing the firmware's power
management.

What the hardware showed:

- **CDM kernel record = 8 words on A10** (Mesa's `csbgen/rogue/cdm.xml`
  K0 K1 K2 K3 K4 K5 K8 plus one more word): with 7 words the stream does
  not finish (RGX+0x4a0 stays 0, run 1); with 8 it does (run 4).
- **RGX+0x610 is the PDS execution base, programmed at power-up from
  P+0x10** (fw 0x5660, 1 MiB aligned); K1 (data) and K2 (code) are offsets
  from it: K1 at an unmapped VA with base 0 faults there (MCU requester 8,
  read, run 9); base 0x100000 with K1 / K2 lowered by 0x100000 behaves like
  base 0 (run 20).
- **P+0x18 is the USC base** (RGX+0x4008 / 0x4010 / 0x4028, 64-byte
  aligned, fw 0x5660): a PDS program `DOUTU (END=1) ; HALT` (Mesa
  encodings, 0xf4000002 / 0xd3000000) starts a USC task that reads at
  **USC base + 0** (requester 16, USCS; run 21 with P+0x18 = 0x35000000).
  With the base mapped the job completes with no fault (runs 22-25).
- Processing a kernel record also reads **PDS base + 0x211080** (MCU,
  run 15 with P+0x10 = 0x34000000) -- not K1 / K2 (runs 9 and 20); what
  sits there is to find. `--pad` maps GPU VA 0x204000..0x3fffff onto one
  zero page, which absorbs it.
- Not yet working: **the USC does not appear to run our code.** The DOUTU
  word's EXE_OFF (our PDS constant) does not move the USC's read (runs 8,
  10: always base + 0; run 23: EXE_OFF unmapped, no fault), and
  `store_const` placed at the USC base writes nothing anywhere in our
  memory and faults nowhere (run 26). Either the constants do not reach
  DOUTU's src0, or base + 0 holds something other than code (a header the
  USC reads first). To find: what iOS puts at the USC base and in the PDS
  data segment -- the kext's compute path, or our own capture on the
  jailbroken iPad.

### G5 DONE (2026-11-23, runs 97-100): store_const writes the buffer

**The cause of everything odd in G5 was BIF tiling over the whole GPU VA
space.** At every power-up the firmware (fw 0x5660) builds four tiling
ranges from P+0x0..0xc ({u64 base, u64 size}) into RGX+0x12e0 / 0x12f0 /
0x1300 / 0x1310 (upstream `ROGUE_CR_BIF_TILING_CFG1/3/5/7`, written with
the 0x90000000 enable bits); with base and size 0 the range end is -1, so
every GPU access was swizzled. That is the stream at 0x200800 being read at
0x208000, the stream at 0x210000 read at 0x201100, the "phantom" PDS read
at 0x201100, the PDS-base + 0x211080 read, and the kernel's DOUTU never
starting a USC task. `gfx-init.py` now puts the range at GPU VA
0x6_0000_0000 + 0xffc00000, far above our VAs (where iOS's kext puts it:
to check in our kernelcache).

With that, `gfx-init.py --job --kernel --rec8 --store` (P+0x10 = P+0x18 =
GPU VA 0x200000; the DMA record binds the buffer, the kernel record runs
`store_const`, laid out as iPadOS does it, "G5 ground truth" below):

- run 97 / 99: **the output buffer reads 0xcafe0000** (`out[0] = 0xcafe0000
  + tid`, one thread), the rest stays 0xabadbeef; both stamps, no fault;
- run 98 (control): the argument entry moved to GV+0x2810 -> **0xcafe0000
  appears at +0x10**, +0x0 keeps the sentinel: the store goes through the
  DMA-loaded binding;
- run 100: the same without `--pad` -- no stray reads any more.

Runs 27-96 below were taken with the whole VA space tiled: their
addresses are swizzled, and their conclusions (the 0x201100 reader, the
PDS-code boundary at 0x211000, "EXE_OFF ignored", the 7- vs 8-word record)
are superseded until re-checked without tiling.

### G5 runs 27-93 (Linux, after the iPadOS capture): the reader at 0x201100

`--store` / `--store-ios` (the iPadOS layout at GPU VA 0x200000 / 0x300000)
complete the job but leave the output buffer untouched (0xabadbeef), and
point controls (buffer, EXE_OFF, USC base unmapped) show **no USC task at
all** (runs 27-33, 58-63). Bisecting against the configuration that did
fault at the USC base (runs 21 / 41) found why:

- **A PDS task we never set up reads GPU VA 0x201100** (requester PDS 10 /
  22, or CDM 4 when the stream sits at 0x210000), after the kernel
  record's own PDS program: with page 0x201000 unmapped the fault is
  0x201100 whatever K0 (0x2 / 0x6 / 0x2103), K1, K2, the PDS base (0x100000
  / 0x200000), the stream base (0x200000 / 0x210000), the StartCompute
  addresses (RGX+0x498, the six 0x11040 slots), the terminate / context-
  store PDS code and data, or P+0x50 / 0x58 / 0x60 / 0x350 / 0x358 /
  0x1a98 (runs 82-93). The value 0x201100 is in none of our memory.
- **Every earlier "USC start" came from a DOUTU we happened to have at
  0x201100** (`--usc` writes one there): with the kernel record's K2 on a
  HALT and the DOUTU left at 0x201100 the USC still starts (run 79); with
  the DOUTU only at K2 (0x1200 / 0x1300 / 0x1100 elsewhere) it never does
  (runs 73-77), also on fresh physical memory (run 78). That task's PDS
  data is not ours, so its EXE_OFF is 0 -- hence "the USC reads its base +
  0, EXE_OFF ignored" (G5 progress above). The "PDS code above VA 0x211000
  starts nothing" finding (runs 48-57 and the PTE sweep) is the same
  effect and does not stand on its own.
- So **the kernel record's own DOUTU has not been seen to start a USC task
  yet** -- the open G5 question, together with: who issues the PDS task at
  0x201100 (a fixed hardware / firmware program address, or state that
  survives `--cold`; to check from a fresh boot and from the RGX registers
  the firmware's cold power-up writes, captured on the RAM stand-in).

## G5 ground truth from iPadOS (2026-11-23): how Apple's stack dispatches

`tools/gpu/ios/gpucap` (built by `tools/gpu/ios/build.sh`, ad-hoc signed by
ldid with `tools/gpu/ios/entitlements.plist`: the AGX / IOGPU / IOSurface
user clients, without which `MTLCreateSystemDefaultDevice()` is nil) runs
our `store_const` through Metal on the iPad's own iPadOS 18.7.9 (22H355,
palera1n rootless, as `mobile` over `iproxy 2222:22`) into two buffers, then
saves the process's IOAccelerator memory (VM tag 100) after encoding, in the
command buffer's scheduled handler and after completion. Captures in
`build/gpu/ioscap/` (Apple data: not in git). Notes on A10's Metal: no
non-uniform `dispatchThreads` (APINotSupported); `dispatchThreadgroups`
works. Results: A[0] = B[0] = 0xcafe0000 (1 thread), 0xcafe0000..3 (4).

What the capture shows (all GPU VAs in the 0x1_0000_0000 range):

- **One 64 KiB heap at GPU VA 0x1_0000_0000 holds the USC code and the PDS
  programs and data; it is both the PDS and the USC base**: our
  `store_const` USC code at +0x40, byte for byte metal-tt's output; the
  kernel's PDS program at +0x200, its data at +0x280; a DMA PDS program at
  +0x180. Every K1 / K2 and the DOUTU's EXE_OFF are offsets into it.
- **The CDM stream**: 8-word records (K0 K1 K2 K3 K4 K5 + two words), seven
  per command buffer, then the terminate 0x80000000:

  | record | K0 | K1 | K2 | K3-K5 | w6 | w7 |
  |---|---|---|---|---|---|---|
  | 0: DMA | 0x4 | 0x30042 | 0x180 | 0 | 0x40000001 | 0 |
  | 1: kernel | 0x2103 | 0x280 | 0x200 | 0 | 0x40000001 (1 thread) / 0x1 (4) | 0 / 0x300000 (4) |
  | 2-6 | Metal's own (K0 0x4 / 0x2 / 0xc0000003 / 0x3, heap +0x14xxx) | | | | | |

  K0 bit 0 fence, bit 1 usc_target ANY, bits 8-11 PDS temps, bits 12-17
  USC unified size; **pds_data_size (bits 2-7) = 16-byte units - 1** (the
  DMA's 32 bytes -> 1, the kernel's 16 -> 0). K1 bit 1 =
  usc_common_shared. Words 6 / 7 are not Mesa's K8: the workgroup size - 1
  sits in w7 from bit 20 (4 threads -> 0x300000), w6 bit 30 set for a
  single thread.
- **The kernel's PDS program** (+0x200): `c2c00003 f0000a01 d3000000
  f0000e01 fc000002 d3000000`, data (+0x280): `00000040 00000010 90000000
  80000000` -- the DOUTU word (EXE_OFF 0x40 = the USC code, TEMPS 2) and
  two DOUTW words.
- **The buffer binding**: record 0 is a DMA-only PDS task (`f0001000
  f4001808 d3000000`) whose 32-byte data is `{u64 -> argument entry,
  u64 -> entry + 0x10, 0x10000002, 0x90004003}`; the argument entry is
  `{u64 buffer GPU VA, u64 CPU pointer, u32 0x00010001}`. The two
  dispatches differ only in record 0's K1 (0x30042 / 0x3c042): one
  argument table per dispatch. The DMA puts the buffer VA into the USC
  common store, where `store_const` reads it.

So G5's open points were: PDS / USC addresses are offsets from one heap
base (ours: P+0x10 = P+0x18), the kernel record needs Apple's K0 sizes and
w6 / w7, and the buffer reaches the shader through the DMA record.
`gfx-init.py --store` builds exactly this at GPU VA 0x200000 (to run).

## G3 results (2026-11-23): init data, the firmware idle and stable

`tools/gpu/gfx-init.py` (boots the firmware through gfx-boot.py, builds the
init data, sends it; KEY=VAL sets single words; re-runnable in one proxy
session). `tools/gpu/gfx-softcrash.py` asks a running firmware for a crash
dump (crash endpoint 1, message 0x40) -- its task list and stacks.

Fixes found on the way (G2 / G3):

- **Mailbox interrupts**: after IOP +0x4000 / +0xc00 = 0x1111 the kext's
  setter unmasks through +0x4004 / +0xc04 (inbox 0x1, outbox 0x1000).
  Without it the firmware answers the polled boot handshake and then never
  sees another message (no pong).
- **The coprocessor's D-cache**: the firmware's writes to its own memory are
  not visible in DRAM until it cleans them; reading its globals from the
  AP proves nothing. Its crash dumps (which it cleans) are the reliable view.
- **Its VA windows** (RTKit, from its BSS): pages 0..0x2000 (VA below 32 MiB)
  and 0x80000..+0x800 (VA 0x80000000 + 8 MiB). The firmware maps what it
  registers itself (init page, descriptors, crash buffer) at 0x80000000..;
  **a host descriptor mapped below 32 MiB hangs the firmware on its first
  access** (bisected with deliberate panics); at 0x80400000 it works.
- Bisecting by deliberate panics: a descriptor VA outside both windows
  ("Unable to map VA_..."), an unaligned scratch PA ("Unable to register
  scratch RAM region PA_123"), no descriptors (data abort at the first read
  of X, pc 0x16598).

The init data that brings the firmware up (fw 0x19370 and the main task
0x1cbd0; every other word zero):

| where | value | read by |
|---|---|---|
| message | (0x20 << 56) \| (0x81 << 48) \| PA of the init page | fw 0x19370 (type byte & 0xbf == 0x81) |
| init +0x08 / +0x0c | PA of an empty init sequence | fw 0x5660 |
| init +0x18 / +0x1c | X / T (fw VAs) | 0x19370, 0x2740 (T: ~0xe20 bytes of settings) |
| init +0x8c / +0x90 / +0x94 | PA / count of the memory descriptors | 0x19370 |
| init +0x98 / +0x9c / +0xa0 / +0xa4 | A / B / C / D (fw VAs; B >= 0x6a2c bytes) | 0x19370, 0x2740 |
| descriptor | {VA, PA >> 12, 0, pages \| flags << 16}: one, VA 0x80400000, 1 MiB | 0x19a58 |
| X+0x0 | P | |
| X+0x4 | a page: two words copied to fw 0xd6c90 | main 0x1cc92 |
| X+0x8 | a page: a 0x80-byte struct, first word 0 = "initialize it" | 0x1d9f4 |
| X+0xc / +0x14 ... +0xcc / +0xd4 | 13 channels x two pointers to ring indices | 0x4f80 / 0x5062 |
| **X+0x311** (unaligned) | the power-state struct (+0x38 state, +0x1e8 ...) | 0x18300, power task 0x23e5c |
| P+0x8c0 / +0x8c4 / +0x8cc | RGX PA / size (0x20000) | mapped by 0x19370 |
| P+0xac0 / +0xac4 | scratch RAM PA (a page): fw page 0x32000 is copied there and remapped | 0x164be |

**Run 10 (`g3-run10.log`): no crash for 5 s, ping answered.** The main
task wrote A+0x4 = 1, A+0x10 = 1, A+0x14 = 1, and all AGX tasks exist:
agx_background (main loop), agx_recovery, agx_interrupt, agx_sampler,
agx_power. Run 11: the same with the real RGX at P+0x8c0: stable too.

**The core ID, again**: with that firmware initialized and running,
Fender +0x1000 = 0x11 from the AP, then RGX +0x18 -- **still hung m1n1**
(`g3-run11-coreid.log`). Next check: m1n1 maps the MMIO identity range as
Device-nGnRnE and keeps aliases (0xc0.. GRE, 0xd0.. nGRE, 0xe0.. nGnRnE,
0xf0.. nGnRE); Linux's ioremap is nGnRE. `RGX_ALIAS=0xf000000000
gpu-id.py --req --arm-reset` reads the core through the nGnRE alias.

## G1 results

**Run 1** (2026-11-23, `build/gpu/g1-run1-two-clients.log`; by mistake a second
`gpu-id.py` opened the same proxy port at the same time, so the end of the run
is not trustworthy):

- PMGR GFX 0x20e080328: `0x0f000b00` (off) -> `0x0f0003ff` (target and actual
  0xf) after m1n1's `pmgr_adt_power_enable` of `sgx` and `gfx-kf`.
- Fender with GFX on, before any request: +0x1000 0, +0x1004 1, +0x10 0, +0x14
  0, +0x1018 0, +0x2000 7, +0x2048 0 (gfx-kf not running), +0x2050 0xf, +0x2054
  0x2f, +0x2058 0. Fender reads are safe with GFX on.
- **Fender +0x1000 = 0x11 -> reads 1 after 0.2 ms**, +0x14 becomes 2 (on the
  A13 +0x14 is Fender's interrupt status, bit 1 = powered up). The host alone
  asks for the power state: no firmware involved.
- The next read, **RGX +0x00, never answered** (UartTimeout) and the proxy was
  gone afterwards (no exception on the UART). Either a bus hang on RGX +0x00
  or the two clients corrupting the protocol. Run 2 reads +0x18 / +0x20 first.

**Run 2** (2026-11-23, `build/gpu/g1-004559.log`, one client): the same PMGR and
Fender values, Fender state 1 after 0.2 ms again -- and **the 64-bit read of RGX
+0x18 hung m1n1** (no reply, no exception printed; the exception guard was on,
so this is a bus stall or an SError, not a synchronous abort). So Fender's
acknowledgement alone does not make the Rogue registers reachable from a cold
m1n1. Fender +0x1018 (sequencer step on the A13) stays 0 and +0x2058 stays 0
throughout; the ack comes too fast (0.2 ms) for a real rail ramp.

Hypotheses for run 3, most likely first (one per run, each costs a DFU boot):

1. **No GPU clock.** PMGR `GFX` has `perf = True`, `perf_idx = 23`, and state 0
   of `perf-states` is 0 MHz; nothing under m1n1 selects a GPU performance
   state (on iOS the firmware / kext does). Read-only first: PMGR `perf-regs`
   (reg 0 + 0xb4000, 0x55 entries) and the GFX DVFM state, then select a
   nonzero state. The A13 had the same kind of gap (the GPU rail in VOLMAN
   manual mode).
2. **Fender's own request bits.** Fender +0x10 = 0 under m1n1; find in the
   AGXG9P kext ("FenderCfg", "Dynamic Power Control Regs") what the host
   writes there before a power request.
3. **Access width.** Read RGX +0x18 as 32 bits.

`clock-ids = [265]` of sgx / gfx-kf is PMGR clock 9 = `LPO`, not a GPU clock.

**Run 3** (2026-11-23, `build/gpu/g1-run3-*.log`, `tools/gpu/gpu-survey.py`,
read-only survey, no fault anywhere):

- PMGR `perf-regs` block 0 (0x20e0b4000, 0x400 bytes): **all zero** with GFX
  off, on, and after Fender's state 1. Not where the GPU's clock is chosen
  (the ADT's `perf-domains` list only SOC, CPU, MEM and AES; there is no GPU
  domain).
- **Fender 0x0..0x4000 is almost empty under a cold m1n1**: +0x1004 1, +0x2000
  7, +0x2040 6, +0x2050 0xf, +0x2054 0x2f, nothing else. The state request
  changes only +0x1000 (0 -> 1) and +0x14 (0 -> 2). Nothing else moves: no
  sequencer step, no clock or DVFM word. The "acknowledgement" is the
  register echoing the request; nobody has configured Fender.
- RGX +0x18 read as **32 bits hangs m1n1 too** (hypothesis 3 out).

**Conclusion.** iBoot leaves Fender unconfigured; on iOS it is set up by the
AGXG9P kext ("Probe-time Fender mapping", `FenderCfg(Legacy)`, `GFenderCfg
Regs`, "Dynamic Power Control Regs", `AGXFender::resetGPCSleepStateBits`) and
by the firmware itself, which runs `AGFHALExecuteResetSequence` and
`AGFHALWaitForPerfState` before its first core access, through I/O regions the
host hands it (`kAGFIPIORegionTypeFenderRegs*`, `GFXCLKGENMGPU`,
`DVFMMgpuRegisters`; ApplePMGR names the matching groups `AGX_DVFM_CLKGEN`,
`AGX_DVFM_MGPU`, `AGX_DFNDR_DVFM_CLKGEN`, `AGX_DFNDR_DVFM_MGPU`, filled at run
time, not in a static table). Reading the core's ID from a cold m1n1 needs
that sequence first, so **the plan is reordered**: the firmware (G2) does not
need the core and comes next; the core ID moves to G3, read once the
firmware (or our copy of its sequence) has configured Fender. Static work
before the next DFU run: the AGXG9P kext's Fender setup and the firmware's
reset / perf-state code (Ghidra, image at load base 0).

**Run 4** (2026-11-23, `build/gpu/g1-run4-*.log`, `gpu-id.py` with the
firmware's reset step): state 1 acknowledged in 0.2 ms, **Fender +0x2058 = 1
read back 0 at once** (0.2 ms, nothing else in Fender moved), and the 32-bit
read of RGX +0x18 **still hung m1n1**. Perf state + reset is not enough; next
suspect: what the AGXG9P kext does to Fender before the firmware exists
("Probe-time Fender mapping", `FenderCfg(Legacy)`). Also seen: +0x1004 read 0
this boot (1 in runs 1-3).

**Run 5** (2026-11-23, `build/gpu/g1-run5-*.log`, `tools/gpu/pmgr-gpu.py`,
read-only plus GFX on and Fender's request; m1n1 survived):

- **The iBSS did program the GPU's DVFM**: table PMGR 0x20e070000 states 1-6
  = 378 / 440 / 630 / 756 / 852 / 1008 MHz, voltage codes 0x39 / 0x43 / 0x59 /
  0x6c / 0x7e / 0xa0, SRAM codes 0x20 / 0x29 / 0x43 / 0x56 / 0x6b / 0x8f (word
  2 per state: 0x08070100, 0x07070100, 0x05050000, 0x03030000, 0, 0x707);
  states 0 and 7-15 zero. 0x20e070200 = 0x80000000 (enable), 0x20e070300 =
  0x40000000. PLL5 0x20e014000 = 0x80000000, +4 0x81480348.
- DWI (PS 0xff = on): +0x0 1, +0x4 0x3c (the PMU's i2c address), +0x10
  0x800002ff, +0x20 0x80004600, +0x84 0xa00005f3, +0x10c..+0x1fc all one
  value (a mirrored status/timer word).
- **Fender's state 1 reaches the DVFM**: 0x20e070200 0x80000000 ->
  0x80110100, and DWI's mirrored word changes (0x00bcb203 -> 0x000759ae).
  The SoC table (0x20e068000 = 0xddd) does not move.

So the clock and voltage path answers Fender. Next suspect: **PMGR's fabric
bridges.** The ADT's pmgr node has `#bridges` 14, `bridge-reg-index` 10 and
`bridge-settings-0..12` (offset / value pairs; `optional-bridge-mask` 0x2000 =
bridge 13 has none); ApplePMGR maps bridge i from pmgr reg 10 + i (its log
"[Die %u] Bridge %u, virtual address ... physical address"; the bridges are
AXI2AF, "axi2afConfigDataCount == _bridgeCount") and writes the settings --
neither the iBSS nor m1n1 does. **Bridge 8 = pmgr reg 18 = 0x201f00000** and
**bridge 9 = reg 19 = 0x201f80000** sit inside the GPU's block (0x201xxxxxx):
settings-8 `{0: 0xf, 0xc: 0xc0010005, 0x14: 0xff, 0x18: 0xff, 0x4000:
0xc0000000, 0x4004: 0}`, settings-9 offsets 0..0x7c all 0x40101. Run 6
(`tools/gpu/gpu-bridge.py`): read them, apply, Fender state 1, 10 ms, RGX.

**Run 6** (2026-11-23, `build/gpu/g1-run6-*.log`, `tools/gpu/gpu-bridge.py`):
bridges 8 and 9 **already held exactly the ADT's values** before we wrote
them (set by the iBSS, or reset defaults); writing them changed nothing, and
RGX +0x18 hung m1n1 again after Fender's state 1 and 10 ms. Bridges are out.

Excluded so far: PMGR GFX power (on), Fender's state request (acked, reaches
the DVFM and DWI), the firmware's reset step, the GPU's DVFM table / PLL5 /
enable (programmed by the iBSS), the GPU's fabric bridges, access width.
Left, from the kext: the G9P Fender subclass **`AGXArmFenderKingfisher`**
(only its kalloc site string is named). Its base class `AGXFender` declares,
as unimplemented stubs, `enableDynamicFenderGating`, `resetGPCSleepStateBits`,
`enableGPCPerfStateOverride`, `writeTimerSleepControl`,
`isPoweredOffWaitingOnHost` / `...ForKicks`, and a **ConfigRAM** --
`initConfigRAMOffset`, `writeConfigRAMCommand32(uint16_t reg, uint32_t val,
uint8_t)`, `writeConfigRAMCommand64`, `terminateConfigRAMSequence`: a
register-write program stored in Fender, most likely replayed by Fender
itself when it powers the core. Kingfisher's overrides (through its vtable)
are the next static target; GPC = the core's clusters, which may stay
asleep until their sleep bits are reset.

**Kingfisher decoded** (2026-11-23). The factory at kext 0xfffffff005fa005c
allocates through the `site.AGXArmFenderKingfisher` kalloc view and stores
the vtable 0xfffffff006e19d10 -- the one whose slots +0x88..+0x110 are the
"Unimplemented" `AGXFender` stubs: **on G9P the ConfigRAM, GPC sleep bits,
dynamic gating and timer-sleep methods are not used.** Its real methods:

| vtable | fn | does |
|---|---|---|
| +0x48 | 0x...fa0430 | perf state = Fender +0x1000 & 0xf |
| **+0x50** | 0x...fa04b0 | **Fender +0x10 \|= 1 << arg, arg 0 / 1 / 2** -- the AP's request bits |
| +0x58 | 0x...fa05b4 | Fender +0x14: write back its value & 7 (W1C interrupt status) |
| +0x60 / +0x68 | | flags in the object only |
| +0x70 | 0x...fa06bc | touch-reads +0x10, +0x14, +0x1000, +0x200c..+0x2020 |
| +0x80 | 0x...fa0948 | Fender +0x10 & 1 |

Fender registers are reached either through the kext's mapping or through a
helper (fn 0x...fb3808 read / 0x...fb4b34 write) when a flag at
accelerator +0xe7dc is not 1. Runs 1-6 never set +0x10 (it reads 0 under
m1n1); +0x1000 is the perf state (it reaches the DVFM), so **+0x10 may be
what actually asks Fender to power the core**. Run 7: `gpu-id.py --req`
sets +0x10 bits 0, 1, 2 one at a time (Fender logged after each), then state
1, the reset step, 10 ms, RGX.

**Run 7** (2026-11-23, `build/gpu/g1-run7-*.log`): +0x10 took 1, 3, 7 and
nothing else in Fender moved with it; state 1 (+0x14 = 2), the reset step,
10 ms -- **RGX +0x18 hung m1n1 again**. With this, everything the kext and
the firmware write to Fender on the way to the core has been replayed from a
cold m1n1 (+0x10 = 7, +0x1000 = 0x11, +0x2058 = 1), on top of PMGR GFX, the
iBSS's DVFM / PLL5 setup and the ADT's bridge settings -- and the core still
does not answer. What has not been reproduced is the GFX coprocessor
itself: under iOS the core is only ever touched while gfx-kf runs its
firmware (Fender +0x2048 bit 0 = 1; it is 0 under m1n1), and the firmware's
power-up continues with the host's init sequence (RGX and Fender writes)
right after the reset step. **Proposed next: G2** (boot the firmware,
which does not need the core), and read the core's ID once the firmware
has powered it (G3).

**Static pass 2 (2026-11-23).**

- **The coprocessor's window and run bit, from our kernelcache**
  (`AppleA7IOP-AKFWrap-v2`, kext text at kernelcache file offset 0x25d5000 =
  VA 0xfffffff006c15000): `AppleA7IOPV2GFX` maps the provider's reg 1
  (Fender) at init; its `_mapFirmware(iop_addr, md)` (0x...c165e0) writes
  **Fender +0x2014 / +0x2018 = IOP address lo / hi, +0x201c = length &
  ~0xfff, +0x2020 = 0, +0x200c / +0x2010 = physical address lo / hi**, then
  calls the V2 base's own copy (on the IOP block, through accessors); the
  run setter (0x...c16680) is **Fender +0x2048 = (+0x2048 & ~1) | run**.
  Mailbox: receive = IOP +0x4038, send = IOP +0x4010 (64-bit). This is the
  G2 recipe's hardware side, from our own binary.
- **The init sequence is not what the core needs**: a firmware given an
  empty init sequence (the program's first entry type 0) is enough to power
  and drive the core. So the replay list for G1 has nothing more to take
  from it.
- **What is left between run 7 and a working core is the coprocessor
  running.** Run 8 (`tools/gpu/gpu-kfrun.py`) separates "the coprocessor
  runs" from "the GFX firmware runs": gfx-kf executes a 16 KiB image of ARM
  `b .` through the window above, run bit set, then +0x10 = 7, state 1,
  reset step, 10 ms, RGX.
- To check if run 8 still hangs: **Fender +0x0**, 0 in every run so far and
  written by nothing we have decoded (kext, firmware, AKFWrap); a value with
  bit 30 set there is the next candidate.

**Run 8** (2026-11-23, `build/gpu/g1-run8-*.log`, `tools/gpu/gpu-kfrun.py`):
the window took the loop image (Fender +0x200c 0x110dc000, +0x2010 8, +0x201c
0x4000), +0x2048 = 1, and **gfx-kf's IOP status +0x48 went 0x79 -> 0x78** (bit
0 cleared when it started; IOP +0x0 1, +0x38 1, +0x3c 1, +0x40 0x30000
unchanged). Then +0x10 = 7, state 1, reset step, 10 ms -- **RGX +0x18 hung
m1n1 again.** A running coprocessor alone does not open the core either.

Left between run 8 and a core that answers: (a) Fender +0x0 (never written
so far); (b) what the GFX firmware itself does once it runs -- RTKit boot,
then, after the host's init data, its power path (state request, +0x2058
reset, init sequence even when empty) and whatever its startup code touches
through the I/O regions the host hands it. (a) is one more run; (b) is G2.

**Run 9** (2026-11-23, `build/gpu/g1-run9-*.log`, `gpu-kfrun.py --f0`): run
8 plus **Fender +0x0 = 0x40000000** before the state request; it reads back
0x40000000 and nothing else in Fender moves; state 1, reset step, 10 ms --
**RGX +0x18 hung m1n1 again.**

**G1 conclusion (runs 1-9).** From a cold m1n1, with every hardware write
we could find in the kext, the GFX firmware, AKFWrap and the iBSS replayed
(PMGR GFX, Fender +0x0 / +0x10 / +0x1000 / +0x2058, the coprocessor's
window and run bit with a running image, the GPU's DVFM, PLL5, the fabric
bridges), **the Rogue core's registers do not answer.** The one thing not
reproduced is the GFX firmware running under RTKit with the host's init
data. The core's ID is therefore read in G3, after G2; G1 stays "power path
known, core ID not read cold".

## G1.5 results: how the firmware powers the core (2026-11-23)

Ghidra 12.1.2 headless on `gfx-t8010-22H355.bin` (raw ARMv7 LE, base 0; the
project and scripts stay in the scratchpad). Fender is the firmware's I/O
region 0x1c, sgx-relative base 0xd00000; its mapped base is the global at fw
0x324cc. All firmware accesses to it, from every decompiled function:

| Fender | fw function | what |
|---|---|---|
| +0x1000 | 0x18300 (`AGFHALWaitForPerfState`), poll 0x184fc | **the GPU perf state**: write `state \| 0x10`, wait `& 0x1f == state` (timeout arg x 24 ticks = us); 0 = off, 1..6 = `perf-states` 1..6. Before writing, modes 0 / 2 wait for bit 4 (request pending) to clear ("AGFHALWaitForMGPUReqCleared") |
| +0x2058 | 0x6ce4 (`AGFHALExecuteResetSequence`) | **= 1, then poll bit 0 back to 0, 250 us** |
| +0x2044 bit 0 | 0x17404 (clear), 0x1d824 (set / clear) | to read |
| +0x203c | 0x1cffe (`AGFAFenderPowerCallback`) | = 1 on the RTKit power transition to SLEEP |
| < +0x4000 | 0x5660, 0x1f1ea | through the host's init sequence (below) |

**Power-up order** (fw 0x6ce4): if the init-data words at fw 0x32838 /
0x3283c are both nonzero, request the perf state (fn 0x18300(target, 2,
1000)); otherwise only a software "powered" flag is set and no hardware
request is made. Then **Fender +0x2058 = 1 and wait for it to clear**, then
the **init sequence** (fn 0x5660, called at 0x6dc4 and 0x7066).

**Init sequence** (fn 0x5660): a host buffer whose PA / size the init data
gives (fw 0xd74d8 / 0xd74dc; "Unable to register init sequence memory"),
20-byte entries `{u32 val_lo, u32 val_hi, u32 reg, u32 shift, u32 type}`, a
zero type ends it. type 1 = 32-bit write, 2 = 64-bit write, 3 = the 64-bit
value shifted right by `shift`, written as two words. `reg` picks the region:
< 0x20000 = RGX (sgx reg 0), 0xd00000..0xd04000 = Fender, page 0xe10000 = a
third block (sgx + 0xe10000), anything else region 0x38. Its contents come
from the AGXG9P kext (to find: G2/G3 need them; most likely built in code --
its `__TEXT,__const` holds no register / value tables).

**The kext's power-up** (AGXG9P, the register-mapping function around kext
VA 0xfffffff005f77a00..0xfffffff005f77fbc; `__text` is kernelcache file
offset 0x1912000 -- the kext header's section offsets are not file offsets):
it reads the provider's `has-kf` / `no-gpc` properties, maps sgx reg 0 (RGX)
and reg 1 (Fender; flag `startFender`), then

1. provider vtable +0x568(1, 0) and +0x570(1, ...) -- IOService power and
   clock on, i.e. ApplePMGR (power-gates / clock-gates 148, 150);
2. **Fender +0x1000 = 0x11, wait for bit 4 to clear**;
3. **IOSleep(10)** (10 ms);
4. map RGX, then the rest of start.

Power-down is the mirror: +0x1000 = 0x10, wait bit 4, provider +0x570(0) /
+0x568(0). **The kext writes nothing else to Fender** -- what we did in runs
1-4 is the kext's own sequence (minus the 10 ms sleep), so the missing piece
is not in Fender. It is behind step 1: what ApplePMGR does when the sgx
device powers up, beyond the PS register m1n1 writes. Most likely the
**GPU voltage rail** (`voltage-states9` / `-sram`): on the A13 the blocker was
exactly that (iBoot leaves the PMGR voltage manager's GPU channel in manual
mode, Fender's request never reaches the PMU), and Fender acknowledged
quickly there too. Next (run 5, read-only first): find the T8010 PMGR voltage
control for the GPU rail (ApplePMGR's `voltage-states9` handling, the PMGR
reg list) and its state under m1n1.

**The GPU's DVFM table is iBoot's** (our boot's iBSS, iBoot-8422.100.650,
`build/inputs/j171/iBSS.raw` in Ghidra at base 0):

- fn 0x28af8 (fills the ADT's `perf-states` / `voltage-states9`) reads the
  GPU table from the hardware: **PMGR 0x20e070000 + 0x10 * state** (voltage
  domain 0x17 -- GFX's `perf_idx` 23), word 0: post-div bits 3:0, pre-div
  bits 8:4, multiplier bits 20:12 (f = 24 MHz * mult / pre / (post + 1)),
  bit 21 = multiplier nonzero, voltage code bits 31:24; word 1 bits 7:0 =
  the SRAM rail's code (domain 0x1f); word 2 = four 6-bit fields.
- fn 0x28744 writes those words; the PMGR init fn 0x28f68 writes 17 states
  (0..16), then **PLL5** (0x20e014000: +4 |= 1 << 24, +0 &= ~(1 << 27), wait
  for bit 25 to clear) and **0x20e070200 |= 1 << 31**, then 0x210200000 =
  0x80000000. (PLLs are 0x20e000000 + 0x4000 * n in the same code.)
- The SoC's table is 0x20e068018 + 0x10 * i with its state in 0x20e068000.
- The PMU is a Dialog D2365 on i2c0 (0x3c); voltage changes reach it through
  PMGR's **DWI** block (`dwi`, 0x20e200000, PMGR device 51).

So under our boot the GPU table, its PLL and the DVFM enable should already
be set by the iBSS -- unless that init is skipped on this path. Run 5
(`tools/gpu/pmgr-gpu.py`) reads all of it before and after Fender's request.

**The kext's I/O-region table** (AGXG9P `__TEXT,__const` +0x2300.., two copies,
entries ending in the marker 0xdadadada): type 0x14 = sgx + 0x0, 0x20000 (RGX);
**type 0x1c = sgx + 0xd00000, 0x4000 (Fender)**; type 0x24 = sgx + 0xe10000,
0x1000 (0x201e10000: not in the ADT's sgx reg list -- to identify).

## Tools

| tool | what for |
|---|---|
| `tools/gpu/gfx-fw.sh` | extracts the GFX firmware from the 22H355 kernelcache into `build/firmware/apple/gfx-t8010-22H355.bin` (SHA-256 pinned; never in git) |
| `tools/gpu/gpu-survey.py` | G1 run 3: read-only dump of PMGR perf-regs and Fender (0x0..0x4000) before and after Fender's state request, line by line |
| `tools/gpu/gpu-id.py` | G1: PMGR GFX on, Fender words, Fender state 1, Rogue identity registers (`M1N1DEVICE=/dev/cu.usbmodem…1 build/proxyvenv/bin/python tools/gpu/gpu-id.py [--no-core] [--keep]`, from `devices/a10`) |
| `kernel/overlay/.../pvr_apple*.c`, debugfs `pvr-apple/{selftest,fender,rgx-standin}` | G7: the kernel driver's Apple backend and its self-tests ("G7 progress") |
| `tools/gpu/g9p-selftest.py` | G7: our USC code for the kernel's self-tests -> `build/firmware/apple/g9p-selftest.bin` |
| `devices/a13/tools/gpu/isa/metal-isa.sh` | offline Metal compiler; for G9P run its steps with `-arch applegpu_g9p` |

**Do not** (until shown safe): read RGX registers from a cold m1n1 -- even with
Fender +0x1000 at state 1 the read hangs m1n1 (G1 run 2); read Fender with PMGR
GFX off (hung m1n1 on the A13).
