# GPU (AGX G12) — plan and what is known

Status 2026-11-01: started. Nothing runs on the GPU from Linux yet. This page is
the plan and the facts gathered so far; it grows as the work goes.

The phone for this work runs **iOS 16.4 (20E247) with Dopamine 3.0.12** (rootless,
OpenSSH). Linux boots the usual way (usbliter8 → iBSS → m1n1) and does not touch
the iOS disk: the `nvme-linux` marker in `boot/inputs/` is off, so the NVMe write
gate keeps namespace 1 (iOS again) read-only.

## Why this is doable

On the iPad mini 1 the GPU work started from nothing (SGX543, no open driver, the
iOS kernel not observable). Here:

- **The GPU is Apple's own AGX**, the line Asahi Linux reverse-engineered for M1:
  the GPU is driven through firmware on an RTKit coprocessor (`gfx-asc`), and
  Asahi has the host↔firmware structures, a Python driver in m1n1
  (`proxyclient/m1n1/agx/`, `experiments/agx_boot.py`, `agx_1tri.py`), the Rust
  kernel driver (`drm/asahi`) and Mesa (`asahi`).
- **The firmware interface version is one Asahi has described.** iOS 16.4 ships
  `iBoot-8422.100.650` — the same iBoot as macOS 13.3, and m1n1 maps exactly that
  string to `V13_3` (`proxyclient/m1n1/proxyutils.py`, `VERSION_MAP`). The
  firmware's structures for 13.3 are in `proxyclient/m1n1/fw/agx/initdata.py`
  (`Ver("V >= V13_3")` branches). The Rust driver dropped 13.3 (it keeps 12.3 and
  13.5), but 13.3→13.5 is a small diff.
- **The live iOS kernel is observable.** Dopamine's `libjailbreak.dylib` exports
  `jbclient_initialize_primitives`, kernel virtual read/write, physical
  read/write (`physrw`) and kernel calls (`kcall`). That replaces the m1n1
  hypervisor Asahi traced macOS with — which we do not have: A13 has no EL2.

The real gaps: the **chip** (A13 = G12 "H12"; Asahi has G13/G14 only — per-chip
power/clock tables, maybe registers) and the **shader ISA** (G12 vs G13, not known
how far apart).

## What is known (2026-11-01, from the IPSW and the live phone)

Firmware: `Firmware/agx/armfw_g12p.im4p` (IM4P type `gfxf`, manifest key `GFX`,
`IsLoadedByiBoot`). Payload = `rkosftab` table with one Mach-O, `RTKit-2062.100.81`,
"G12 Firmware initializing", "Host and FW Interface is mismatched" (the firmware
checks the host's interface version). Our IM4M has the `gfxf` tag.

Kernel side (kernelcache 20E247): `com.apple.AGXG12P`,
`AGXFirmwareKextG12PRTBuddy`, `AGXFirmwareKextRTBuddy64` (all 227.5.14),
`IOGPUFamily` 65.40.15. Live: `AGXAcceleratorG12P_B0`, `AGXArmFirmwareMapper`,
`AGXFirmwareKextG12RTBuddy`.

ADT (live, iOS) — the same property names m1n1/Asahi read on M1:

| | |
|---|---|
| `sgx@4000000` | `gpu,t8030`; MMIO 0x204000000 (1 MiB) + 0x204d00000 (128 KiB); 6 IRQs 0x18d-0x190, 0x19c, 0x19e; power/clock gates 0xd0, 0xd2 |
| `gfx-asc@6400000` | `iop,ascwrap-v2`, role GFX; MMIO 0x206400000 (128 KiB) + 0x206500000 (8 B); IRQs 0x199, 0x198, 0x19b, 0x19a; gate 0xd1; IOMMU parent phandle 0xb1 |
| `iop-gfx-nub` | `iop-nub,rtbuddy-v2`, `pre-loaded = 1` (iBoot loaded the firmware) |
| firmware segments | `__TEXT` phys 0x8002c0000 → fw VA 0xffffff8000000000, 0x4c000; `__DATA` phys 0x801684000 → 0xffffff800004c000, 0x78000 |
| regions | `gfx-handoff` 0x8fffb4000 +0x4000, `gfx-shared-region` 0x8fffb8000 +0x40000, `gpu-region` 0x8ffff8000 +0x4000; `rtkit-private-vm-region` 0xffffff8000000000 +0x20000000 |
| perf states (MHz/mV) | 0/400, 360/581, 519/640, 660/665, 804/721, 948/800, 1194/931 (SRAM: 753 up to 804 MHz, then 800, 931) |
| other | `soc-generation` H12, `has-kf`, `gptbat-ready`, `gpu-*` power/thermal controller gains (needed for initdata) |

## Plan

Each step ends in something checkable. Percentages: chance the step works as
planned.

**G1 — an iOS kernel memory tool. DONE 2026-11-01.** See "G1 results" below.

**G2 — dump what iOS gives the firmware. DONE 2026-11-01** (see "G2 results"):
all of RegionB/C, HwDataA/B, fw_status found and parsed, `CHIP_INFO[0x8030]`
for m1n1 built from them. `tiling_control` (render only) comes with G5.

**G3 — the firmware is there under Linux. DONE 2026-11-01:** handed to iBoot
with the `firmware` command, iBoot loads and sets it up exactly as for iOS
(see "G3 results").

**G4 — m1n1 proxy on A13, the firmware started. Mostly done 2026-11-01** (see
"G4 results"): the firmware takes initdata and DC_Init, runs, writes Stats.
Left: it does not read the DevCtrl channel yet (doorbell?), Stats records are
0x40 bytes, not m1n1's 0x60.

**G5 — first triangle (45 %).** `agx_1tri.py` with G12 shaders. Unknown: how far
the G12 ISA is from G13. Compile small Metal shaders on the phone, dump the
binaries the iOS compiler makes, compare with Mesa's (asahi) output for the same
NIR. Check: a triangle in memory, then on the screen.

**G6 — kernel driver (35 %).** `drm/asahi` (Rust) with a G12/13.3 entry, in our
Hoolock-based kernel (needs Rust support in the tree) — or a C port of the parts
we need. Check: Mesa's `asahi` driver allocates, submits, waits.

**G7 — Mesa + desktop (30 %).** Mesa `asahi` with G12 compiler changes; glmark2,
XFCE on GL. Native display (still simpledrm) is a separate item in the roadmap.

## G1 results (2026-11-01)

`a13/tools/ios/kmem.c`, built by `sh a13/tools/ios/build.sh` (Xcode iOS SDK +
`ldid`, entitlement `IOSurfaceRootUserClient` so libjailbreak's `kcall`/`kmap`
work), installed as `/var/jb/usr/local/bin/kmem`, run as root over
`ssh iphone11-ios`. Kernel virtual and physical reads/writes through Dopamine's
libjailbreak (`dlopen`, no headers).

- Dopamine 3.0.12's `gSystemInfo` matches the 2.x header only in its first two
  fields (slide, static kernel base); kmem uses only those. The kernel base reads
  `0xfeedfacf`.
- `physreadbuf` reaches only XNU's physmap (from 0x801938000 on this phone).
  iBoot's carve-outs below it are read through `kmap`, at most 64 KiB per call
  (0x20000 is refused); kmem reads them in 64 KiB windows, only with
  `KMEM_CARVEOUT=1`.
- **Reading the GPU firmware's `__DATA` (0x801684000 +0x78000) resets the phone
  instantly, no panic log** (`BAD MAGIC ... no panic log available`); twice, the
  second time with a single 64-byte read. The memory controller guards it. kmem
  refuses every `gfx-asc` segment except `__TEXT` (taken from the live ADT).
  MMIO is refused too (only DRAM ranges are allowed).
- **The firmware `__TEXT` in memory = the IPSW Mach-O** (file offset 0x4000,
  0x4c000 bytes) except what iBoot fills in before starting it:
  - the segment table: `__DATA` phys 0x801684000;
  - the RTKit boot-argument table at fw VA 0xffffff800004a02a (entries: 4-char
    tag stored byte-reversed, u32 size, value):

| tag | value on iOS | IPSW | what (from the ADT) |
|---|---|---|---|
| `STKG` | random | 0xaff | stack guard |
| `RTSZ` | 0xc4000 | same | firmware size |
| `PTPB` / `PTPS` | 0x8fffb8000 / 0x40000 | 0 | `gfx-shared-region` |
| `PTVB` | 0xffffff8000000000 | 0 | `rtkit-private-vm-region-base` |
| `T1Ps` | 0x2000000000 | same | |
| `GptB` / `GptS` | 0x8ffff8000 / 0x4000 | 0 / same | `gpu-region` |
| `PplB` | 0x8fffb4000 | 0 | `gfx-handoff` |
| `GxPB` / `GxPS` | 0x204000000 / 0x4000000 | same | GPU MMIO |
| `McRA` | 0x4000 | same | |
| `SOC_` / `SOCR` | 0x8030 / 0x11 | 0xffffffff | chip id / revision |
| `CpAd` | 0x206000000 | 0 | |
| `WrAd` | 0x206400000 | 0 | `gfx-asc` (ASC wrapper) |

  All other tags (`SLID`, `CSSS`, `v8Fq`, `DSRL`, `DSVS`, `DSCL`, `IGNT`, `SNeo`,
  `DSZS`, `DSZL`, `TUNS`, `TUNZ`, `RTTO`, `DEVf`, `IOBA`, `IOSZ`) stay 0. This is
  the recipe for G3 if our boot does not load the firmware: copy `__TEXT` and
  `__DATA` to the ADT's `segment-ranges`, fill this table, start the ASC.

## G2 results (2026-11-01)

**Three more resets, and the rule they teach.** Reading `gpu-region` (0x8ffff8000,
through a 64 KiB cacheable window) reset the phone like `__DATA` did. `kmem
ptscan` (XNU's own page tables, read from the physmap -- safe) shows how XNU maps
the GPU's carve-outs: `gfx-shared-region` cacheable read-only, `gfx-handoff` and
`gpu-region` write-combined read-only, each with a second read-write mapping
(PPL's: `PplB` in the boot-argument table), firmware `__DATA` cacheable
read-only, `__TEXT` as device memory. `__DATA` is reset-on-read although XNU maps
it, so the attributes do not explain it; the memory controller guards these.
kmem now refuses all of them. Kernel calls (`kcall`) are not available on this
phone (jailbreakd will not sign a thread: no PAC bypass), so reading through
XNU's own mappings is out too. Everything below came from the physmap.

**Finding the driver's memory** (all in `kmem`, see its header):

1. `kmem obj AGXAcceleratorG12P_B0` -- the IOService port turned into the kernel
   object (libjailbreak `task_get_ipc_port_kobject`).
2. `kmem ptrs VA LEN | kclass.py` -- every pointer in an object, named by the
   class of what it points at. The class map is `iometa -a` (Siguza) of the
   20E247 kernelcache (2617 classes with vtables and sizes); a vtable pointer
   minus the slide is iometa's `vtab`.
3. Accelerator (0x9bb0 bytes) → `AGXArmFirmwareChinookV4` at +0x440 (0x1a98
   bytes) → its allocations, each `AGXAllocation` (size at +0x30, an
   `IOBufferMemoryDescriptor` at +0x80) + `AGXSecureMemoryMap` (GPU VA at +0x28,
   size at +0x48) + kernel VA. The descriptor's buffer is at +0xb8; some buffers
   have no kernel mapping left, their pages are in the descriptor's ioGMDData
   (page numbers from +0x30).
4. The firmware object keeps the GPU VAs of what it built at +0x288: RegionB
   0xffffffa00c3d0000 and its parts (+0x6bc0 stats TA, +0x7240 stats 3D, +0x7980
   stats CP, +0x8080 HwDataA, +0xc280 fault info, +0xc300 HwDataB, ...), RegionC
   0xffffffa000020000 at +0x300. GPU kernel VAs are 0xffffffa0_..., as on M1.

`InitData` itself was not found: the buffers that could hold it are all zero
after boot (iOS seems to drop it once the firmware took it). Not needed: what it
points at is all there.

**Parsed with m1n1's own definitions** (`a13/tools/gpu/agxparse.py`, `V13_3`,
`G13` as the closest; constants that differ are reported, not fatal):

| structure | GPU VA | against `V13_3` |
|---|---|---|
| `InitData_RegionB` | 0xffffffa00c3d0000 | matches: 13 channels (TA/3D/CL x4, DevCtrl, Event, FWLog, KTrace, Stats), `gpu_idle` 1, marker 0xabcdabcd, `buffer_mgr_ctl` 0x1500000000 (Asahi's address) |
| `AGXHWDataA` | 0xffffffa00c3d8080 | matches; values agree with the ADT (`max_freq_mhz` 1194, fast-die sensor mask 0xc, power zones target 25000, 24 MHz `clocks_per_period` 0x5dc00), `max_power` 9220 mW |
| `AGXHWDataB` | 0xffffffa00c3dc300 | one difference: `pad_b14` (+0x16fc) holds 0xffffffff08957fc4 (m1n1: zero). `chip_id` 0x8030, `gpu_core` 9, `gpu_rev` 3, `num_cores` 4, `num_frags` 4, `base_clock_khz` 24000, `gpu_region_base` 0x8ffff8000, frequencies/voltages = the ADT `perf-states`, `rel_max_powers` 0,17,26,36,51,71,100 |
| `InitData_RegionC` | 0xffffffa000020000 | matches; `shared1` table -1, 0x340b, 0x2025, 0x1824, 0x11fc, 0xd76, 0x8e9; `shared2` 0x400, 0x800, -2; `unk_508` 0xc00001; `unk_89bc` 10000 |

The firmware's MMIO windows (`HwDataB.io_mappings`, GPU VA → phys, size):
0xffffffa010000000 → 0x204d00000 (0x20000 RW), 0xffffffa010028000 → 0x20e100000
(0x4000 RO), 0xffffffa010030000 → 0x23b104000 (0x4000 RW), 0xffffffa010038000 →
0x204000000 (0x20000 RW), 0xffffffa010060000 → 0x23b2ec000 (0x1000 RO),
0xffffffa010068000 → 0x23bc34000 (0x1000 RW), 0xffffffa010070000 → 0x204d80000
(0x5000 RW), 0xffffffa010081000 → 0x204d71000 (0x1000 RW), 0xffffffa010088000 →
0x200000000 (0xd6400 RW), 0xffffffa01016a000 → 0x23b102000 (0x1000 RO).

Also found: **fw_status** at 0xffffffa0000b8000 (`InitData_FWStatus`: the
fwctl channel, `unk_ctr` counting), **RegionA** = an empty 0x4000 buffer (m1n1
makes it empty too). The GPU VA space constants in HwDataB (`unk_8`
0x1400000000, `unk_18` 0xffc00000, `unk_20`/`unk_28` 0x1100000000, `unk_30`
0x6fffff8000) are M1's (t8103): the UAT layout to start G4 with is m1n1's
t8103 one (L0 at bit 39, 16 KiB pages, 3 levels, 40-bit physical mask).

So the host↔firmware interface of G12 on iOS 16.4 is Asahi's `V13_3` almost
field for field; the chip values come from these dumps, not guesses.

**`CHIP_INFO[0x8030]` for m1n1** (`a13/tools/gpu/chipinfo.py DUMPDIR HWDATA_A
HWDATA_B REGION_C`, every field read back from where m1n1 writes it):

```python
    0x8030: Container(
        chip_id = 0x8030,
        min_sram_volt = 753,          # lowest voltages_sram (field pre-13.0 only)
        max_power = 9220,
        max_freq_mhz = 1194,
        unk_87c = 0,
        unk_8cc = 10000,
        unk_924 = [[0.0] * 8] * 8,
        unk_e48 = [[0.0] * 8] * 8,
        unk_e24 = 120,
        gpu_fast_die0_sensor_mask64 = 0xc,
        gpu_fast_die1_sensor_mask64 = 0,
        gpu_fast_die0_sensor_mask64_alt = 0xc,
        gpu_fast_die1_sensor_mask64_alt = 0,
        gpu_fast_die0_sensor_present = 0x1,  # pre-13.0 only
        shared1_tab = [-1, 0x340b, 0x2025, 0x1824, 0x11fc, 0xd76, 0x8e9] + [-1] * 9,
        shared1_a4 = 0xffff,
        shared2_tab = [0x400, 0x800, -2, -1, -1, -1, -1, -1, 0, 0],
        shared2_unk_508 = 0xc00001,
        unk_3cf4 = [302.0, 0, 0, 0, 0, 0, 0, 0],
        unk_3d14 = [11.0, 0, 0, 0, 0, 0, 0, 0],
        unk_3d34_0 = [0] * 8,
        unk_118ec = None,
        hwdb_4e0 = 0,
        hwdb_534 = 0,
        num_cores = 4,
        gpu_core = 9,
        gpu_rev = 3,
        hwdb_ab8 = 0,                 # pre-13.0 only
        hwdb_abc = 0,                 # pre-13.0 only
        hwdb_b30 = 0,
        rel_max_powers = [0, 17, 26, 36, 51, 71, 100],
        shared2_t1_coef = None,
        shared3_tab = [0] * 16,
        shared3_unk = 0,
        sram_base = 0,
        sram_size = 0,
        rc_unk_54 = 0xffff,
        unk_hws2_0 = 0,
        unk_hws2_4 = [0.0] * 8,
        unk_hws2_24 = 0,
        tiling_control = None,        # render only: from an iOS render capture (G5)
    ),
```

For G4, m1n1 also needs to know G12: `Ver.set_version` turns ADT `soc-generation`
"H12" into "G12", which its version matrix (G13, G14, G14X) lacks; HwDataB's
`pad_b14` (+0x16fc) is the one field to take from iOS as is.

## G3 results (2026-11-01)

Linux booted the usual way (iBSS in Recovery → `devicetree`, trust cache via
`firmware`, `bootx`), checked with `a13/tools/gpu/g3-check.sh` (phone: live ADT
from `/dev/mtd1`, the firmware `__TEXT` via `/dev/mem`) and `g3-compare.py`
(Mac; a raw ADT walker, m1n1's ADT parser chokes on an iPhone audio property):

- iBoot's breadcrumbs list `rdtr`, `trst`, `rdsk`, `rkrn` -- no `gfxf`;
- `gfx-asc` has no `segment-ranges`, `iop-gfx-nub` no `pre-loaded`;
- 0x8002c0000 is plain Linux RAM (a kernel pointer there);
- the GPU regions are still assigned in `sgx` (handoff 0x8fffb4000, shared
  0x8fffb8000, gpu-region 0x8ffff8000), above Linux's RAM (which ends at
  0x8e7690000): nothing in Linux touches them;
- m1n1 already tries to describe the GPU to Linux (Asahi's `kboot_gpu.c`):
  "ADT: GPU: unsupported chip!".

So our boot does not load the GPU firmware. Our iBSS has the code for it (the
`firmware` command, which already takes the trust cache, and the strings
`rkosftab`, `segment-ranges`, `pre-loaded`), as a restore sends every
`IsLoadedByiBoot` component with `firmware` (idevicerestore
`recovery_send_loaded_by_iboot`). `boot-from-recovery.sh` now sends
`inputs/gfx.gfxf.img4` (the IPSW's `armfw_g12p.im4p` + our IM4M, which has the
`gfxf` tag) that way when it exists. If iBoot does it, it also fills the RTKit
boot-argument table and the ADT exactly as for iOS; if not, m1n1 does the same
from the G1 recipe.

**Second boot, with `gfx.gfxf.img4` sent: iBoot does it all.**

- breadcrumbs: `f0008(gfxf) ... f0100(gfxf) f0106(b000) ... f0009(gfxf)` --
  loaded, its `rkosftab` entry `b000` placed;
- ADT: `iop-gfx-nub` `pre-loaded` = 1, `gfx-asc` `segment-ranges` = `__TEXT`
  phys 0x800160000 (0x4c000, fw VA 0xffffff8000000000), `__DATA` phys
  0x8001ac000 (0x78000, fw VA 0xffffff800004c000);
- iBoot moves the start of usable RAM past it: m1n1 hands Linux 0x800224000
  up (= end of `__DATA`), so Linux never touches the firmware;
- `__TEXT` read under Linux (`a13/tools/peek/memdump`: /dev/mem `read()`
  fails outside RAM with EFAULT, `mmap` works, Device-nGnRnE) = the IPSW
  except iBoot's fill-ins, and = iOS's except the `__DATA` address in the
  segment table and the random `STKG`. The RTKit boot-argument table is
  iOS's: `PTPB` 0x8fffb8000, `PTPS` 0x40000, `PTVB` 0xffffff8000000000, `GptB`
  0x8ffff8000, `PplB` 0x8fffb4000, `SOC_` 0x8030, `SOCR` 0x11, `CpAd`
  0x206000000, `WrAd` 0x206400000.

So under Linux the GPU firmware sits loaded, configured and not yet started,
as iOS's kext finds it; G4 starts it. `__DATA` was not read (resets on iOS).

## G4 results (2026-11-01)

**Transport.** `a13/cascadia boot proxy` boots m1n1 alone (iBoot still loads the
GPU firmware). The DCSD cable carries nothing from the Mac to the phone (UART0
RX is on: UCON 0x5c85, m1n1's REQ_BOOT goes out, nothing comes in), so m1n1
got USB on the A13 (patches 0009, 0010):

- the ADT's `usb-drd`, `atc-phy`, `dart-usb` are unnumbered; the phy is an AUSB
  USB2 phy (`atc-phy,t8030`, reg 1 = 0x239000060) brought up with its ADT
  `tunable-device` as Linux's `phy-apple-ausb` does; the live ADT `usb-drd`
  has no reg 3 (the Type-C pipehandler);
- dwc3 sits behind both `dart-usb` instances (TRADDARTLLT, TRADDARTBLK, SID 3
  `mapper-usb-drd`); instance 0 is pointed at instance 1's tables
  (`dart_mirror`);
- the A13's dwc3 sets bit 26 in a completed TRB's size word: m1n1 took the
  whole word as BUFSIZ and pushed 16 KiB of filler into the proxy's ring
  (found with a temporary UART trace of the bulk OUT events).

The phone then shows up as `/dev/cu.usbmodem<serial>1` and `3`; the proxy
reads 1 MB in 0.14 s.

**proxyclient for T8030** (patch 0007, the Mac side only):

- `G12` in the version matrix, `CHIP_INFO[0x8030]` (G2), the firmware's MMIO
  windows (`build_iomappings`, from iOS's HwDataB), t8103's 40-bit physical
  mask;
- the ASC mailbox: the A13 has the A11's control registers (0x8108 / 0x810c;
  Linux: `apple,t8030-asc-mailbox`, `apple,t8015-asc-mailbox`). With the M1's
  the proxyclient popped the empty outbox and the firmware panicked
  `Mbox:underflow`;
- RTKit boot needs `SetAPPower(0x20)` from the AP after the endpoint map (Linux
  sends it; m1n1's ASC code did not): sent by hand, the firmware acked at once;
- the A13's `sgx` has no `gpu-avg-power-filter-tc-ms`: iOS's 1008 ms;
- `InitData.ver_info` must be `(0x6b88, 0xc128, 0x600, 0xb0)`, not G13's 13.3
  value: the firmware compares the first 64 bits (fw VA 0xffffff80000275f8)
  and panics "Host and FW Interface is mismatched" otherwise (found from the
  panic string's `adr` back to the branch);
- ADT properties typed for Macs that do not parse on an iPhone are kept as bytes.

**Where it stands** (`a13/tools/gpu/agx-start.py`, one fresh proxy boot per
try -- restarting the firmware in place panics it, "Invalid AP power"):

```
[mgmt] Supported versions 12 .. 12
[Handoff] Waiting for FW PPL init... Initialized!
[UAT] TTBR1[0] = 0x8fffb8000
[mgmt] Startup complete
Starting endpoint #0x20 (fw) / #0x21 (db)
Sending initdata / Sending DC_Init / Sending DC_UpdateIdleTS
```

No panic. Read back through the GPU page tables (TTBR1 in the shared region):
`InitData.ver_info` as set, the firmware wrote two Stats records (type 0x0c,
its timestamps; 0x40 bytes apart), `gpu_idle` 1. Not yet: it has not read the
DevCtrl channel (read pointer 0, ours 2), `fw_status.unk_ctr` stays 0 (iOS:
counting), RegionB's 0xabcdabcd marker not set. The GPU regions that reset
iOS when read are written here by m1n1 without trouble.

**Why DevCtrl is not read** (2026-11-01, same boot): a soft crash (`0x40` on
the crash endpoint) dumps every firmware task. The mailbox history shows both
DevCtrl doorbells (`0x83..11`) arrived. `agx_background` reads HwDataB +0x16fc
(the one field that differs from iOS: iOS 0xffffffff08957fc4, m1n1 0), sets a
request bit for the power task and sleeps; `agx_power` sits in
`agfPollFenderRegIgnoreRecovery` (fw 0xffffff8000023fcc): it polls
**Fender** (the GPU power controller, `io_mappings[0]`, 0x204d00000)
+0x1000, low 5 bits, for a target state. Fender+0x1000 reads 0x11 and stays
(+0x1018 = 0x10, +0x1014 = 2); the firmware has programmed Fender
(+0x14080.. = HwDataA's shared1 table, +0x14000 = 0x30001). Its timeout
message would go to FWLog, which stays empty. Doorbell decoding in the G12
firmware (fw 0xffffff800002bccc): bits 53:48 type (3 doorbell, 4 FWCtl, 5
halt), for type 3 bits 1:0 = TA/3D/CL, bits 4:2 = queue (4 = none counted).

**The Fender wait is the PMP's job.** The firmware's power function (fw
0xffffff8000023fcc) writes `state | 0x10` to Fender+0x1000, writes 1 to the
PMP doorbell (0x23bc34000, `io_mappings` "PMPDoorbell") and waits for the 0x10
to clear: the power-management coprocessor (PMP, ASC 0x23bc00000, firmware
`t8030pmp`, IPSW `Firmware/pmp/t8030pmp.im4p`, tag `pmpf`) carries GPU power
states out. Our boot never loaded it. HwDataB +0x16fc is a clock offset the
firmware adds to "now" for deadlines (iOS: about -173 s), not the cause.

With `pmp.pmpf.img4` also handed to iBoot (periphery makes it), the PMP is
`pre-loaded` but stopped (iBoot does not start it). Booted from the proxy
(`agx-start.py --boot-pmp`): RTKit v12 hello, endpoints 1-4 and 0x20, the
crash and kdebug endpoints ask for buffers -- then nothing. `dart-pmp`
(0x23b300000) tells why: ERROR 0x80000401 at 0x19dbc000 = the crash buffer we
gave as a physical address; SID 0 translates (TCR 0x80) with no tables. The
PMP sees memory and its registers only through `dart-pmp`: SID 0
(`mapper-pmp`), `pio-vm-base` 0xc0000000, `pio-granularity` 0x1000000, and the
26 register windows of `mapper-pmp/pio-range` (MCC 0x2000x0000, Fender's
0x204d14000, CPU cluster DVFS 0x210/0x211..., 0x23500c000, 0x23c100000,
0x23d100000.., 0x26b854000, 0x26b8f0000). The PMP image holds no such table
(its boot arguments: STKG, DBUG, DART=1, PWRG, CLKG, NRGD, PMON, ... the GFX
set): the iOS kernel's `dart-pmp` page tables decide where each window sits.

iOS's tables could not be read (no TTBR or table address in
`AppleT8020DART` / `IODARTMapper`; iOS 16 keeps them elsewhere). Not needed:
**m1n1 already has the PMP's host driver** (Asahi's v1 protocol,
`proxyclient/m1n1/fw/pmp.py`), and the A13 PMP speaks it (its endpoint-0x20
dispatch: subtype 1 = map a 0xf000 host buffer = `Configure`).
`agx-start.py --boot-pmp` (one session with the GPU):

- `dart-pmp` initialized (SID 15 bypass); the PMP's RTKit boots fully
  (crash, syslog, kdebug, iorep buffers through the DART; IOP and AP power
  0x20; "Startup complete");
- endpoint 0x20: `Startup` -> shared buffer (energy-model DRAM configs at
  +0x2000, the register-window table at +0xe000: `pmp` reg[3..] laid out from
  0xc0000000, Fender's page 0x204d14000 at 0xc0024000) -> `Configure` (ack
  0x200000023b53f) -> `Init1(1, 3)`, `Init2(1, 0)`, `Unk(0x3bc, 2)`, all acked;
  the PMP's syslog: "[main.cpp:3295] PMP started";
- its tasks (soft crash dump): rtk_ep_work, power, main, pm_work, pm_rt_work,
  pmu_telemetry_work.

**Still: Fender+0x1000 = 0x11 after the GPU firmware's request**, and the
PMP shows nothing from the GPU in its mailbox log. The PMP is given only
Fender's +0x14000 page, so the +0x1000 request bit is cleared by Fender's own
sequencer. PMGR: `GFX` (parent) and `PMP` are on; `GFX-SGX`, `GFX-ASC`,
`GFX-BUSY` are not PS-managed. m1n1's M1 "poke" (Fender+0x14000 = 0x70001)
reads back 0x30001 here.

**What the iOS side does around Fender** (static, kernelcache 20E247):
`com.apple.AGXG12P` has an `AGXFender` class (`agxk_fender.h`,
`agxk_fender_common.cpp`, `G12::AGXFenderCommon`): config-RAM commands
(`initConfigRAMOffset`, `writeConfigRAMCommand32/64`,
`terminateConfigRAMSequence`), `isPoweredOffWaitingOnHost`,
`isPoweredOffWaitingForKicks`, `resetGPCSleepStateBits`,
`enableGPCPerfStateOverride`; the base class's are panicking stubs in this
build (G12's own methods: below). The GPU firmware
runs its own register init sequence (a static table in its image, fw
0xffffff8000010cb4: entries {value, offset, type 0-3}) -- nothing to give it.

**PMGR's GFX domain has `notify_pmp`** (ADT pmgr devices: GFX id1 129, flags
perf + notify_pmp; GFX-SGX 208, GFX-ASC 209, GFX-BUSY 210 are `no_ps`
virtual devices). m1n1 sets the PS register only and never tells the PMP.
A first `DevPwr` (m1n1 fw/pmp.py type 0x20e, DEV 129, STATE 0xf) to the
configured PMP got no answer and did not move Fender: the device numbering
or the message for this PMP version is still to be found (ApplePMGR's
notify-PMP path in the kernelcache; the PMP firmware's type-2 handler).

**iOS PMGR -> PMP** (kernelcache `com.apple.driver.ApplePMGR`, also
`AppleT8030PMGR`, `ApplePMP` with class `ApplePMPv2`): no mailbox message --
a "dashboard": `_pmpWriteDashBoard(PMPCommand, ...)`,
`_pmpWriteDashBoardSetDeviceState(DeviceID, level, die)` reads a 64-bit PTD
register (`PTD-SOC-DEV-PS-REQ: Device=%x level=%d psEntry=%llx`), sets or
clears the device's bit (index from a per-device table, -1 = not PMP
managed), writes it back and waits for the PMP to mirror it ("PMP-DevHandle
... Expected/Actual"); `_ringPMPDoorbell` writes doorbell bits (PMGR offsets
0xb8000 + 4 * n); `PMP_EXT_SW_GEN`, `PMP_STATUS` registers; `_initPMP` and
`_initPMPv2` fill the PTD ranges and the PMP device list (`_pmpPtdRangeCount`,
`_pmpDeviceCount`). The A13's ADT has no `ptd-range` (Asahi's PMP v2 script
`experiments/pmp_init.py` writes `SOC-DEV-PS-REQ` from it); where iOS gets
the PTD layout on the A13 is the next question (`_initPMP`).

The configured PMP's shared buffer (Configure, Asahi v1) holds its
energy-model view: rails SOC/DCS/DISP/AVE with VMIN/VNOM/VMAX, accounting
groups CPU/ECPU/PCPU/GPU/ISP/VDD_AVE/DISP0/VENC/VDEC/JPEG/MSR/ANE/PCIE/SBR/SEP
(+0x40a8, 0x18 bytes each), telemetry channels (+0x6000: VDD_MAIN, PCPU/GPU/
SOC [V]/[T]/[I], CPU_SRAM, GPU_SRAM, DCS, ECPU...).

**The PMP's PM commands** (`ApplePMP::_sendPMCommand(command, domain,
device, arg1, arg2)`, kernelcache `com.apple.driver.ApplePMP`; class
`ApplePMP` is the v1 host driver, `ApplePMPv2` the PTD one):

    msg = 0x0020000000000000 | (command & 0xff) << 44 | (domain & 0xf) << 40
        | (device & 0xff) << 32 | (arg1 & 0xffff) << 16 | (arg2 & 0xffff)

commands below 0x11; those below 0xd wait for an ack with command + 1 and
the same domain and device (for 0xa and 0xc the ack's arg1 must be 0).
m1n1's `Init1`/`Init2` are commands 0 and 2; its `DevPwr` (0x20e) is command
0xe: no ack is expected, which is why none came. The kext also has
`_getNumDVFSStates`, `_setDVFSStates(domain, ...)` with a watchdog,
`cpu-power-limit-cluster-*`, the fast-die controller, telemetry: the GPU's
voltage rail ("GPU [V]") and DVFS are the PMP's, and the Fender transition
probably waits on them. `_setDVFSStates(minMask, maxMask, ...)` (fn 0xfffffff0091e1b44) sends, for
every DVFS domain index d (0 .. `_getNumDVFSDomains()`), **command 6**,
domain d, device 0, arg1/arg2 from d's 4-bit fields of the two masks (state
indices; arg2 = number of states - 1 - field); the PMP's message handler
(fn 0xfffffff0091df2f4) answers some PMP requests with **command 4** (domain
from the request). Tried (proxy, `agx-start.py --pmp-dvfs` + by hand): command 6 with range
(0, 0xf) is acked (command 7, same domain) for domains 0-6; domain 7 crashes
the PMP -- **the PMP has 7 DVFS domains**. Fender+0x1000 stays 0x11 with all
seven set: the DVFS ranges are not what the transition waits for.

**iOS's Fender driver, read** (2026-11-01, `com.apple.AGXG12P` and
`com.apple.AGXFirmwareKextG12PRTBuddy`, 20E247). The `AGXFender` methods
named above are panicking stubs in the vtable `G12::AGXFenderCommon` uses
(0xfffffff0079b9240; found by the stubs' addresses, the kext's pointers are
fileset fixups: target = 0xfffffff007004000 + low 30 bits): **G12 has no
config RAM, no "waiting on host", no timer power interrupt**. Its own
methods (the object: +0x10 Fender's mapping, +0xc "power management on",
+0xd a flag) are tiny:

| fn (0xfffffff00851....) | does |
|---|---|
| f8c4 | init: keep the mapping, PM off |
| f860 | state = +0x1000 & 0xf (only with PM on; "Fender State" in IOReport) |
| f824 | control: +0x1010 \|= 1, 2 or 4 for mode 0, 1, 2; any other mode writes 0; then reads it back |
| f7fc | interrupt ack (PM on): v = +0x1014; if v, write v & 7 back |
| f7e4 / f7d8 | set the +0xc / +0xd flags (software only) |
| f7c0 | dummy reads of +0x1010, +0x1014, +0x1000 |
| f7a4 | +0x1010 & 1 |

Who calls them: the GPU interrupt handler (fn 0xfffffff0084c78e0) passes the
interrupt index to the firmware object (fn 0xfffffff0084f4e48): **indices
1-3 go to the Fender ack** (+0x1014), 0 to the firmware path, 4 to another.
The sgx node has six interrupts (AIC 0x18d, 0x18e, 0x18f, 0x190, 0x19c,
0x19e), so 0x18e-0x190 are Fender's. The idle path (fn 0xfffffff0084c79e8,
after a firmware interrupt) writes +0x1010 = 0 (control mode 3) before the
GPU powers down and +0x1010 |= 1 (mode 0) otherwise (fn 0xfffffff0084f4b80:
`x ? 0 : 3`). RTBuddy's `powerStateChangeGated` panics with "trying to change
power state with disabled fender power management" if PM is off, and turns
it on (+0xc) on the way up. The firmware itself never names +0x1010, +0x1014
or +0x1018 (no such immediate offsets in `armfw_g12p`): they face the host.
"PMPDoorbell (unused)" is the G12 IO-mapping table's name for the slot
(fn 0xfffffff0084cbd24; "FenderRegs" size 0x5000 there); the firmware still
rings 0x23bc34000 when its flag at fw 0xffffff80000bf1a8 is 0. The PMP's
"GFX_FENDER" is an energy-meter node name, not a control.

So our stuck state reads as: the firmware asked (+0x1000 = 0x11), Fender
raised a host interrupt (+0x1014 = 2, sgx interrupt index 2) that nobody
acknowledged, and +0x1010 (host power management) may be 0.
`agx-start.py --fender` prints +0x1000/+0x1010/+0x1014/+0x1018 before the
GPU and after its request, then sets +0x1010 bit 0, acknowledges +0x1014 and
watches +0x1000 for 3 s (acking again as interrupts come).

Tried (proxy, `agx-start-12.log`): Fender reads all zero before the GPU
starts (+0x1000/+0x1010/+0x1014/+0x1018 = 0); after the firmware's request
+0x1000 = 0x11, +0x1014 = 2, +0x1018 = 0x10, +0x1010 = 0. Setting +0x1010
bit 0 and acknowledging +0x1014 (it clears, no new interrupt) leaves +0x1000
at 0x11 for 3 s more. `agx-start.py --fender` now sets +0x1010 before the GPU
starts and acks +0x1014 as it runs. (The `--pmp-dvfs` loop had been failing
since 3776b6f: m1n1's `send()` wants an `R_INBOX1`, fixed; the earlier DVFS
result was from sends by hand.) The PMP image's strings are all telemetry
(energy meters, DCS bandwidth, PMU ADC, "Perf Drivers"): nothing that would
carry out a GFX power state, so the 0x10 in +0x1000 is Fender's/PMGR's to
clear.

**PMP doorbells are registered by the host** (static, `t8030pmp`, 20E247).
Live probes of Fender (+0x1010 = 1, 2, 4, 3, 7) never moved +0x1000 (0x11);
Fender +0x14000 page: 0x30001, 1, then at +0x80 ffff 340b 2025 1824 11fc d76
8e9 ffff... The PMP's PM-command handler (fn 0x100c004, jump table 0x10272fa;
cmd = bits 51:44, domain 43:40, arg1 31:16, arg2 15:0): cmd 0xe = DevPwr
(arg1 device id, arg2 on/off), 0x8 (reply 9), 0xa (reply 0xb), **0xc =
register a doorbell for a device** (arg1 device id, arg2 doorbell n >= 0x20;
reply 0xd carrying arg1/arg2 on success, zeros on failure). Registration (fn
0x10112c4) keeps name, handler, page 0x23bc1c000 + (n >> 3) * 0x4000 and bit
n & 7. The GPU firmware rings 0x23bc34000 with 1 = **n 0x30**; nothing
registers it under m1n1, so the GPU's power request goes nowhere. Device ids
are looked up in a table at config+0x170 (count +0x16c, 0x88-byte entries, id
u16 at +0xe, name pointer at +0); config = the pointer set at fn 0x1003a38
(probably the host's Configure buffer, which m1n1 leaves zero). Next: find who
fills that table on iOS (ApplePMGR `initPMP` / `ApplePMPControlFunction`,
PMGR `function-pmp_control` "PMPC", GFX has `notify_pmp`), then send cmd 0xc
(device GFX, n 0x30) before the GPU starts.

**The PMP's device table is its own** (static, 20E247). The config pointer
(VA 0x1045000, loaded at fn 0x1003a38) is set by the firmware's static
initialiser (fn 0x102630c) to a struct at 0x1044de8 that it fills itself:
+0x16c = 15, +0x170 = 0x1045050. `a13/tools/gpu/pmp-devtable.py` runs that
initialiser in unicorn and prints the table:

| # | name | id | doorbell (+0x28) |
|---|---|---|---|
| 0-2 | CPU, ECPU, PCPU | 0 | 0, 0x10, 0x11 |
| 3 | **GPU** | **0x81** (= PMGR GFX, 129) | **0x30** |
| 4-14 | ISP, VDD_AVE, DISP0, VENC, VDEC, JPEG, MSR, ANE, PCIE, SBR, SEP | 0x82, 0, 0x6e, 0x83, 0x84, 0x78, 0x79, 0x85, 0x7d, 0x30, 0xf6 | -- |

So nothing from the host is missing in the table: the GPU entry already
knows doorbell 0x30, it just was never registered. PM command 0xc
(dispatcher: domain = bits 43:40, arg1 = 31:16 matched against the ids,
arg2 = 15:0; every command needs the PMP's state at 0x1046018 != 0, which
DVFS commands showed it is after Init) with arg1 0x81, arg2 0x30: per-device
slot (0x58 bytes at *0x1045e90, +0x1c = registered) must be free and n >=
0x20; it hooks interrupt 0x10000 | n (handler 0x1011598) and replies 0xd with
the message's arg1/arg2. `agx-start.py --pmp-doorbell` sends it after the PMP
is configured, before the GPU starts.

Tried (proxy, 2026-11-02): the PMP acks the registration
(`0x0020d00000810030`), yet after the firmware's request Fender+0x1000 stays
0x11 and the PMP sends nothing. Why, from the PMP image: the doorbell's work
function (0x10115c8) reads the device's request from a PMGR register and
compares it with three thresholds in the device's entry (+0x2a, +0x2c,
+0x2e); the GPU's are 0 (the static initialiser writes 0; ISP's are 300,
427, 533), so it returns at once. DevPwr (command 0xe) ignores "on" for
devices of type 4 (entry +8: GPU, VDEC, JPEG, ...). PM command 2 is the
PMP's state (arg1 0-3; the doorbell work runs only in states 1-2) and m1n1's
Init2 already sends `2, arg1 1`. **The PMP does not power the GPU on.** The
firmware's flag that skips the PMP doorbell (fw 0xffffff80000bf1a8) comes
from HwDataB +0x964 (copied at fw 0xffffff80000276e8) and is 0 on iOS too.

**PMGR trigger controls** (`com.apple.driver.ApplePMGR` /
`AppleT8030PMGR`, 20E247). At start ApplePMGR calls its `setTrigger(n, 1)`
(fn 0xfffffff0091b2eb4, from fn 0xfffffff0091b07bc) for each `*-trig`
property the pmgr node has: read the trigger's register group, OR in the
property's value, write it back (per die). The A13's node has `ecpu-trig` 7,
`pcpu-trig` 7, `ane-trig` 7 and **`gpu-trig` 0x20007**. AppleT8030PMGR maps
the groups (fn 0xfffffff00945e034, `setRegGroup(group, slot, offset)`; slot
0 = pmgr reg 0 = 0x23b000000): ECPU_TRIG_CTL +0xe8000, PCPU_TRIG_CTL
+0xe9000, ANE_TRIG_CTL +0xf9000, **GPU_TRIG_CTL +0xfd000 (0x23b0fd000)**.
m1n1 never sets it. If it is what lets Fender's power/perf request reach the
PMGR, it is the missing piece: `agx-start.py` prints the four trigger
controls before and after the GPU's request, and `--gpu-trig` sets
GPU_TRIG_CTL |= 0x20007 before the GPU starts.

Tried (proxy, 2026-11-02): under m1n1 the trigger controls read ECPU =
PCPU = ANE = 0x2100 (iBoot does not set the CPUs' 7 either, and the CPUs
run) and GPU = 0x2430; `--gpu-trig` makes it 0x22437, and Fender+0x1000
still stays 0x11 (+0x1018 = 0x10). Not it, or not alone.

**Next: registers on the live iOS.** Static reading keeps producing
candidates; a reference of Fender and the PMGR as iOS leaves them is
cheaper. `kmem mmio PA [LEN]` / `kmem mmiowatch SECS PA...` map registers
into kmem as device memory (Dopamine's `pmap_map_in_with_flags` with
AttrIndx 5, nGnRE) instead of the cacheable kmap that reset the phone on
carve-outs.

That first version (PTEs written into kmem's own page tables) read one
register and then took the phone down, at exit most likely (the kernel tore
down a mapping it did not know). kmap itself is `IOSurface_map` (an
IOSurface whose memory descriptor Dopamine points at the range);
`IOSurface_map_withCacheMode(pa, size, &uaddr, 0x100 kIOMapInhibitCache)` is
the same path uncached, and that is what `kmem mmio` uses now: no trouble.

**iOS reference** (2026-11-02, idle phone, then apps opened with `uiopen`):

| register | iOS |
|---|---|
| PMGR GFX ps 0x23b080390 | 0x1400034f / 0x1000030f / 0x1f0003ff: **AUTO_ENABLE (bit 28) set**, the auto state (27:24) and actual (7:4) flip between 0, 4 and 0xf every few tens of us by themselves, target 0xf |
| PMGR PMP ps 0x23b080368 | 0x1f0f03ff |
| GPU_TRIG_CTL 0x23b0fd000 | 0x22437 (ECPU/PCPU/ANE 0x2107) |
| Fender +0x1000 / +0x1010 / +0x1014 idle | 0 / 0 / 0 or 3 |
| Fender +0x1018 | flips 0 / 8 with the GFX ps |
| Fender +0x14000 / +0x14004 | 0 / 1; +0x14020 0x0694f470, +0x14028 0x003f6b87; +0x14080.. = shared1 table (ffff, 340b, 2025, 1824, 11fc, d76, 8e9, then ffff) |

`kmem mmiowatch` on Fender while apps open: **+0x1000 goes 0x11 -> 0x01 in
about 16 us** on its own, +0x1014 = 2 then 3, acknowledged by the host
(cleared) ~200 ms later; then 0x01 -> 0x00 (or 0x10 -> 0x00) on power-down.
**+0x1010 stays 0 throughout**: neither the host's control bit nor the PMP
is in the power-up path. What differs from ours is the PMGR: m1n1's
`pmgr_set_mode` clears AUTO_ENABLE when it powers a device on, so GFX runs
under m1n1 as plain "on, no auto" -- and Fender's request (presumably an
auto-PM handshake with the PMGR) gets no answer. `agx-start.py` prints the
GFX and PMP power-state registers; `--gfx-auto` sets GFX's AUTO_ENABLE
after powering it on.

Tried (proxy, 2026-11-02, `--boot-pmp --gfx-auto --fender`): GFX ps
0x0f0000ff -> 0x1f0000ff, and after the firmware's request 0x1f0003ff (the
was-power/clock-gated bits: auto-PM does act), PMP ps 0x0f0f00ff (iOS:
0x1f0f03ff). Fender+0x1000 still 0x11. Next difference: Fender +0x14000,
0 on iOS, 0x30001 here -- m1n1's `AGX.poke_sgx` (M1 "Early init, needed?")
writes 0x70001 there. `agx-start.py` now skips that poke (`--poke` keeps
it) and prints Fender's +0x1000 and +0x14000 pages before the GPU and after
its request, for a word-by-word comparison with iOS's.

Tried (proxy, no poke): Fender's +0x14000 page is now iOS's (+0x14004 = 1,
the shared1 table), +0x1000 still 0x11. So Fender is set up alike and what
it waits on is outside it.

**PMGR, iOS vs m1n1** (`a13/tools/gpu/pmgr-regions.txt`, `pmgr-dump.py`
through the proxy, `kmem mmio` on iOS, `pmgr-names.py ADT ios proxy`; dumps
in `a13/build/gpu/pmgr-diff/`, iOS idle, proxy after the GPU's request):

- **pwrgate block (pmgr `pwrgate-regs`, 0x23b09c000..)**: 16-byte entries;
  the first word of most is 0x80000000 on both, but **0x23b09c000 itself
  is 0x80000000 on iOS and 0 under m1n1**. The other differences (bit 30 of
  word 0 and 0x0c000000 in word 3 of some entries) follow domains that iOS
  runs in auto and flip with them.
- **AUTO_ENABLE**: iOS runs the fabric and memory domains in auto (MCC,
  DCS0-3, SMX, IMX, RMX, MMX, SBR, PMS*, PMP, DISP0_GP*, SIO*, SOC_SPMI*,
  PCIE_REF, APCIE*, ANS2, SEP); m1n1 clears it everywhere.
- trigger controls: iOS ECPU/PCPU/ANE 0x2107, GPU 0x22437.
- the rest are devices one side uses and the other does not (I2C, SPI,
  MCA, USB, ...).

ApplePMGR reads `pwrgate-regs` into its PwrGate register groups (fn
0xfffffff00919d894; fn 0xfffffff0091a4168 only logs them); who sets
0x23b09c000 bit 31 was not found statically. `agx-start.py --ios-pmgr`
tries it on the hardware in two steps: 0x23b09c000 |= 0x80000000 and GFX
auto before the GPU starts; if Fender still waits, AUTO_ENABLE on the
fabric/memory domains (MCC, DCS0-3, SMX, IMX, RMX, MMX, SBR, PMS_BUSIF,
PMS, PMS_SRAM, PMP) with the request pending.

Tried (proxy, `agx-start-14.log`): 0x23b09c000 takes 0x80000000; GFX auto;
Fender 0x11. Step 2: the fabric domains go into auto and flip by themselves
(DCS0 0x1400024f, MMX 0x14000340, RMX 0x1000030f, as on iOS); Fender still
0x11 after 5 s more, and m1n1's USB was gone after the session (step 2 is
now `--fabric-auto`, opt-in). **Neither pwrgate +0 nor auto-PM is what
Fender waits on.** Next block to compare: PMGR perf-regs (GFX device
perf_idx 30 and power domain GFX perf_idx 71 in perf block 0 =
0x23b0b4000; blocks 2/3 at 0x23b0fc000/0x23b0f8000): clocks and DVFM;
added to `pmgr-regions.txt`.

On iOS the perf blocks are pairs {1, a growing count} every 0x10 bytes
(residency counters, it seems), nothing that gates a power-up.

**Fender +0x1018 is the power sequencer's step** (iOS, `kmem mmiowatch`,
apps opened): power-up 0 -> 2 -> **0x10** (~12-95 us) -> 5 (with +0x1000
0x11 -> 0x01), power-down 6 -> 0; one wake went 2 -> 0xc instead. Ours
stops at **step 0x10**. The pwrgate entries 0x23b09c180/190/1a0 flip on
their own, not with Fender (not the GPU's gates). Whole of Fender on iOS
(idle; `a13/build/gpu/pmgr-diff/ios-fender-{lo,hi}.txt`): +0x50 1, +0x5c
0xc01, +0x64 1, +0x1100 0xe0000001, **+0x4000..+0x4510** (0x03001000,
0x00090000, 0x00030c00, 0x000c0404, 0x00020202, 0x01320100, 0x00001311,
+0x4048 0xf, +0x404c 0x2f, +0x4060 2, +0x4080.. 0x5a 8 2 1 2 0x00204080
0xe 0x00030101, +0x4200 0x10000, +0x4500 0xf, +0x4510 0x100), +0x8000 1,
+0x800c/+0x8010 0x003680d8, +0x8024 0x0023fffe, +0x14004 1, +0x18080..
0xffff x16, +0x18100 1, +0x18110/+0x1811c -1, +0x18140 1, +0x18150/
+0x1815c -1. `agx-start.py` writes the same full dump under m1n1 before
the GPU and after its request (`proxy-fender-{before,after}.txt`).

Whole-Fender diff (proxy `agx-start-15.log`, iOS idle): everything the
same (the +0x4000 block, +0x8000, +0x18080..) except +0x4200 (iOS
0x10000, ours 0), +0x409c (iOS 0x00030101, ours 0x00030001), +0x1100 bit 0
(iOS 0xe0000001), +0x54 (ours 6 before the GPU). +0x4200 |= 0x10000 is the
firmware's own write (fw 0xffffff80000234dc, on its power-up path, when
HwDataA +0x4158 is set): a consequence, not the cause. Nothing writes
+0x409c by immediate. Not tried yet: `--fender-ios` sets both before the
GPU starts. m1n1's USB was gone after this session too (no fabric auto this
time): the full Fender read is now opt-in (`--fender-dump`).

Next to compare: the other PMGR register groups AppleT8030PMGR maps
(slot 0 = 0x23b000000: ISP_REF0_CLK_CFG +0x40210, TOUCH +0x78000,
CLUSTER_CTL +0x98000, SOC_TVM +0xc8000, MISC_CORES +0xd4000, MISC_ACG
+0xdc000; GFX_TVM is not mapped on T8030); added to `pmgr-regions.txt`,
`agx-start.py --dump-regions` dumps the whole list before the GPU and
after its request.

**Do not bulk-read 0x23bc1c000** (PMGR reg 19, inside the PMP's ASC block):
reading it through the proxy killed m1n1 (the USB link dropped).

Crash logs: m1n1 gives the firmware a crash buffer ("[crash] buf <phys>");
`m1n1.fw.asc.crash.CrashLogParser(data).dump()` decodes it (message,
backtrace, the mailbox history both ways).

## Not possible / ruled out

- m1n1 hypervisor tracing (Asahi's way on macOS): A13 has no EL2.
- iOS 16.4 is the only firmware version to target: the Linux boot uses the
  20E247 iBSS, so its iBoot (8422.100.650, `V13_3`) is the one that would load the
  firmware.
