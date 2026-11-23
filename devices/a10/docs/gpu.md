# GPU (Apple A10 / T8010, "G9P") -- plan and what is known

Status 2026-11-23: **G0-G3 done; the GPU work runs under Linux** (`GPU_BACKEND=linux`,
see "GPU work under Linux"): core ID 0x011b0602 / revision 0x000d0a00, the GFX
firmware boots and takes init data with the real RGX, stable. Under m1n1 the
core's registers hang the bus (not explained). Next: G4. Nothing runs on
the GPU yet. This page is what the iPad 7's own ADT, kernelcache and firmware
say, what the reference projects say, and the plan in checkable steps, in the
style of the iPad mini 1's SGX work and the A13's AGX work
(`devices/a13/docs/research/gpu.md`).

**Decision (the user's, 2026-11-22):** Cascadia writes its own A10 GPU driver.
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

**G4 -- the core under the firmware, an empty compute job (45 %).** Rogue
page tables for one GPU context, the firmware's queue for compute, a
terminate-only CDM stream. Check: the completion stamp moves, no MMU fault
(RGX_CR_MMU fault registers), no firmware crash.

**G5 -- a compute store (40 %).** The `store_const` kernel compiled offline
for `applegpu_g9p`, its PDS program (Rogue PDS encodings), the buffer bound.
Check: the value lands in memory; moving the buffer moves the write.

**G6 -- a clear, then the first triangle (30 %).** TA + 3D with no draws
(background object writes a colour into the render target), then a VDM stream
with vertex and fragment shaders. Check: the pixels in memory, then on the
screen.

**G7 -- kernel driver (35 %).** In our kernel, in C: `apple-rtkit` with a
T8010 A7-IOP mailbox backend, GEM shmem, `drm_gpuvm`, `drm_sched`. Decision
for later: our own small UAPI or upstream powervr's (Mesa's pvr winsys
unchanged, the kernel translating to Apple's firmware commands). Check: a
user program allocates, submits, waits.

**G8 -- Mesa and the desktop (30 %).** Our G9P shader backend (decoded from
`applegpu_g9p` output), Rogue state packing from upstream Mesa. Gallium (GL ES
directly, as with the SGX) or Vulkan + Zink -- decided after G6. Yardstick:
labwc and SuperTux on the GPU instead of pixman / llvmpipe.

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
| `devices/a13/tools/gpu/isa/metal-isa.sh` | offline Metal compiler; for G9P run its steps with `-arch applegpu_g9p` |

**Do not** (until shown safe): read RGX registers from a cold m1n1 -- even with
Fender +0x1000 at state 1 the read hangs m1n1 (G1 run 2); read Fender with PMGR
GFX off (hung m1n1 on the A13).
