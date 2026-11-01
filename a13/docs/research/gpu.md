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

**G3 — the firmware is there under Linux (70 %).** Does our boot (sig-patched iBSS
in Recovery → `bootx`) load `gfxf` like iOS's iBoot does? Check on Linux: the live
ADT (`/dev/mtd1`) `iop-gfx-nub` `pre-loaded` and `segment-ranges`, and the bytes at
0x8002c0000. If not loaded: send `gfxf` from Recovery (`irecovery` firmware
command), or have m1n1 place the Mach-O segments itself.

**G4 — m1n1 proxy on A13 (65 %).** m1n1 in proxy mode over our USB (dwc3) instead
of chainloading Linux, so the Python side on the Mac can drive the hardware. Then
`agx_boot.py`: power `gfx-asc`/`sgx`, boot the RTKit firmware, hand it initdata
from G2. Check: the firmware logs, no "interface mismatched", the power/stats
rings move.

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

## Not possible / ruled out

- m1n1 hypervisor tracing (Asahi's way on macOS): A13 has no EL2.
- iOS 16.4 is the only firmware version to target: the Linux boot uses the
  20E247 iBSS, so its iBoot (8422.100.650, `V13_3`) is the one that would load the
  firmware.
