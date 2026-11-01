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

**G2 — dump what iOS gives the firmware (75 %).** Find the AGX driver's objects
(from `AGXAcceleratorG12P_B0` in the IORegistry) and the firmware's page tables
(UAT), then dump initdata and everything it points to (HwDataA/B, globals, the
rings) from a running iOS. Parse with m1n1's `fw/agx/initdata.py` at `V13_3`
(as `agx_dumpstructs.py` does). Result: every G12 field and where 13.3's layout
differs on G12; a `CHIP_INFO[0x8030]` for m1n1 filled from iOS, not guessed.

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

## Not possible / ruled out

- m1n1 hypervisor tracing (Asahi's way on macOS): A13 has no EL2.
- iOS 16.4 is the only firmware version to target: the Linux boot uses the
  20E247 iBSS, so its iBoot (8422.100.650, `V13_3`) is the one that would load the
  firmware.
