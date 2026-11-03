# GPU (AGX G12) — plan and what is known

Status 2026-11-03: **G4 done in one run** -- under the m1n1 proxy the GPU powers
up and its firmware runs (see "G4 summary: how the A13 GPU powers up" right
after the plan); a second clean run and a longer one are still to do. Nothing
runs on the GPU from Linux yet. This page is the plan and the facts gathered so
far, in the order they were found; the summary sections say what holds now.

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

**G4 — m1n1 proxy on A13, the firmware started. DONE 2026-11-03** (see
"G4 results", end): the GPU powers up and the firmware runs. The block was
the GPU's power rail: iBoot leaves the PMGR voltage manager's GPU channel
in manual mode (VOLMAN control +0x14 bit 0), XNU hands it to the hardware
at boot; with that bit cleared (`agx-start.py --volman-gpu`) Fender's
sequencer goes 0 -> 2 -> 0x10 -> 5, +0x1000 = 1, PMU buck 0x4100 comes up
at code 0x3a, and the firmware takes initdata, DC_Init, DC_UpdateIdleTS and
reads DevCtrl (read pointer 2) without crashing. Left for G5: Stats records
are 0x40 bytes, not m1n1's 0x60; the other VOLMAN channels (CPU, SoC, GPU
perf +0x3c) are still manual.

**G5 — first triangle (45 %).** `agx_1tri.py` with G12 shaders. Unknown: how far
the G12 ISA is from G13. Compile small Metal shaders on the phone, dump the
binaries the iOS compiler makes, compare with Mesa's (asahi) output for the same
NIR. Check: a triangle in memory, then on the screen.

Steps (2026-11-03), each one checkable on its own:

- **G5.0 the firmware stays up.** Second clean run, 30 s: Stats records
  decoded (0x40 bytes on G12; m1n1's driver read 0x60 -- fixed in patch
  0007), FWLog, the GPU's idle power-off and wake-up (Fender +0x1000 back
  to 0 and up again, VOLMAN +0x38 / buck 0x4100 following).
- **G5.1 an empty compute job. DONE 2026-11-03** (see "G5 results"). WorkCommandCP on CL_0 with a shader that
  is only `stop`: the smallest work that goes through the firmware's
  queue, the CDM and the USC. m1n1's Python driver submits render work
  only (render.py); compute after drm/asahi's compute.rs, control words
  from Mesa's genxml (CDM_LAUNCH, USC). Check: the completion stamp moves,
  no fault, no firmware panic.
- **G5.2 a compute store.** The shader writes a constant (then its thread
  id) to a buffer. First real G12 ISA test: compare `device_store`
  encodings with the iOS compiler's output for the same kernel.
- **G5.3 a clear-only render.** TA + 3D with no draws, the background
  program writing a colour: tile buffers, ZLS, the render target in
  memory.
- **G5.4 the triangle.** VDM stream, vertex and fragment shaders,
  `agx_1tri.py`'s path; then the framebuffer on the screen.

Reference G12 machine code: **Xcode's Metal toolchain compiles for the
A13 offline** (target `applegpu_g12p`, backend `libapplegpuG9G12-nt`;
`devices/a13/tools/gpu/isa/metal-isa.sh SRC.metal FUNC...`, see "G5 results").
The iOS compiler on the phone (MTLBinaryArchive) is the fallback.

## G5 results (2026-11-03)

**G5.0, first run** (`agx-start-38.log`, fresh proxy boot, `--volman-gpu
--fender-init --watch 30`). G4 repeats: Fender 1 / step 5, VOLMAN +0x38
0x013a3a3a, buck 0x4100 `09 3a`, initdata / DC_Init / DC_UpdateIdleTS,
DevCtrl read pointer 2. Stats records decode at 0x40 bytes: five "FW
active" records (False, True, False, True, False) by 0.76 s, then nothing
for 30 s. **The GPU never powered down**: GFX, Fender, VOLMAN and the buck
unchanged for the whole run and still 1.5 min later; the crash buffer
untouched (0xefefefef filler). initdata has iOS-like delays
(idle_to_off 2 ms, fender_idle_to_off 40 ms) and Fender +0x1010 (host
power management) was 0, +0x1014 = 2 unacknowledged -- the firmware went
idle while PM was off.

Afterwards on the same session, through a bare proxy connection (no
`m1n1.setup`): +0x1010 |= 1 and +0x1014 acknowledged -> no change in 10 s
(the firmware's idle decision had passed). **The host can cycle the GPU
itself**: +0x1000 = 0x10 -> state 0, step 0, VOLMAN +0x38 = 0x01000000
(code 0, still under hardware), +0x1014 = 1, in under 1 ms; +0x1000 =
0x11 -> state 1, step 5, +0x38 0x013a3a3a, +0x1014 = 2; repeatable. So
+0x1014 bit 1 = powered up, bit 0 = powered down, and the rail follows
Fender both ways. Next run: `--fender` as well (PM on before the firmware
starts, acks in the loop), to see the firmware power the GPU down itself.

**Run 2** (`agx-start-39.log`, fresh boot, `--volman-gpu --fender-init
--fender --watch 30`): +0x1010 = 1 before the firmware, interrupt 2 acked
at once, the same five Stats records -- and still **no power-down** in
30 s. GFX AUTO_ENABLE set afterwards on the spent session: GFX goes to
auto state 0xf (0x1f0000ff) and stays (iOS idle: auto state 4,
0x1400034f), i.e. GFX follows Fender, and nobody asks Fender for state 0.

Firmware side (static, `gfx-text.bin`): fn 0x23fcc(state, ?, timeout,
?) is the firmware's GPU power-state setter (returns at once if the
current state, [[0xbec80]+0x188]+0x38, already matches). State 0 is asked
in two places: 0xded4 (the big scheduler loop, under w27) and 0x126dc in
fn 0x12280, which looks like the idle power-down (stats/timestamps,
per-channel bits at +0x398c cleared, KTrace 0x200, then state 0); state 1
at 0x12004; the rest ask state 2 (timeout 0xfa). Under m1n1 the firmware
never gets there. Not a blocker for G5.1-G5.4 (the GPU just stays on;
the host can switch it off through Fender); left as an open item.
`agx-start.py` now reads Fender only while PMGR GFX is on.

**G5.1 code** (`devices/a13/tools/gpu/g5compute.py`, `agx-start.py --compute
[HEX]`; not run yet). m1n1's Python driver submits render work only, so
the compute job is built by hand: firmware side after render.py's TA path
and drm/asahi's compute.rs, command stream after Mesa's genxml
(`~/Desktop/ipad-mini-linux/build/mesa/src/src/asahi/genxml/cmdbuf.xml`,
gallium `agx_build_pipeline` / `agx_cdm_launch` / `agx_cdm_barrier`):

- shader (G12 code, default `08 01`) and USC words in the pipeline region
  (offsets from 0x11_00000000): Shared `4d 90 00 00` (layout 0x24, 64 KiB,
  none used), Shader `0d 0c` + code offset (unk_2 = 3), Registers `8d 01
  00 00` (8 GPRs), No preshader `88 00`;
- CDM stream: Launch word 0 `0x1012` (uniform / texture / preshader
  counts at one group each, no samplers, Direct), word 1 = USC offset
  (64-byte aligned), Global size 1,1,1, Local size 1,1,1, Barrier
  `0x600fffff` (Mesa's bits 0-19), Stream Terminate `0x40000000 0`;
  encoder_end points at the Terminate;
- WorkCommandCP (0x320 bytes) on queue type 2 (CL_0): ComputeInfo
  (preempt buffer 0x7fa0 with slots at +0x7f80.., pipeline_base, unk_38
  0x8c60, unk_58 1), ComputeInfo2 (encoder_end), EncoderParams (unk mask
  0xffffffff), JobMeta (stamp / fw stamp / value / event slot), the
  microsequence StartCompute -> Timestamp -> WaitForInterrupt(0, 0, 1) ->
  Timestamp -> FinalizeCompute -> End.

Dry run on a fake AGX (flat memory, fake UAT) builds everything; it found
that m1n1's WorkCommandCP had two fields named `unk_pad` and could not be
built at all (fixed in patch 0007: the V13 one is `unk_pad2`). Check on
the phone: stamp 1 reaches the value, the event fires, no firmware
crash; then the store kernel (G5.2).

**G5.1 run 1** (`agx-start-40.log`, `--volman-gpu --fender-init --compute
5`): **the firmware crashed 0.3 ms after the CL_0 doorbell** and did not
tell the host (no crash message on EP1; m1n1 waited 5 s, stamps 0,
timestamps 0, queue done pointer 0). Found afterwards: the crash buffer
held a log (`CLHE`), read through a bare proxy connection word by word
(`devices/a13/build/gpu/g5-crash-40.bin`; `readmem` gave checksum errors there).
Task `agx_background`, data abort ESR 0x96000006 (translation fault),
**FAR 0x20, PC fw 0x1e0b4**: `ldr w8, [x19, #0x20]` with x19 = NULL.

- fn 0x1e010(queue, idx) takes x19 = [0x70c38 + idx * 0x38 + 0x18]: a
  firmware __DATA table of 128 entries (7-bit index, also packed into
  records at bits 14-20, fn 0x1ddb4). Its caller is the RunCmdQueue
  handler (fn 0x8a14, the CL branch at 0x9934: `x0 = [x23]` = our
  CommandQueueInfo, `w1 = [x23 + 0xc] & 0xff`, x23 = ring element + 4) --
  i.e. idx = the message's **event_number** (0 here).
- The live table (fw __DATA, read from PA 0x8001fcc38: the crash log's
  Ccdp gives DATA VA 0xffffff800004c000 -> PA 0x8001d8000, not the
  segment-ranges address) is **all zero**: no event slot is set up, so the
  first RunCmdQueue that names one dereferences NULL. TA/3D take the
  same path (fn 0x1fc14 is called from all three branches), so render
  would crash the same way.
- Not the cause: fn 0x28570 also fills 0x38-byte entries, but its table is
  RegionC.unkptr_88b4 (perf sources; [0xbfae8] = RegionC, perf_source_count
  0 on iOS too).

Next: who fills the event table entries (+0x10, +0x18) -- a host message
m1n1 never sends, or a field in initdata / the work command that names
the event's stamp objects. `g5compute.py` now checks the crash buffer
itself after a timeout.

**Cause: the work command layout.** The firmware's queue loop (fn 0x1fefc)
walks the ring, dispatches on the command's type word through a jump
table (0x2079c) and flushes the command's size from the cache first:
type 0 TA 0x5e0, 1 3D 0x940, 2 0x650, 3 **CP 0x2e0**, 4 0x20, 5 0x18, 6
0x20, 7 0x50, 9 0x58. m1n1's V13_3 structures are TA 0x61c, 3D 0x9b0, CP
0x320 (V12_3: 0x5e0, 0x928, 0x2d8): **this firmware's interface is "V13_3"
for initdata but its work commands are of the 12.x line**, with V13's
counter at +4. Then it calls fn 0x1dcfc(cmd, slot byte, stamp_value,
queue, type) with the event slot and stamp value from the command's
JobMeta: TA +0x574 / +0x570, 3D +0x8d8 / +0x8d4, CP **+0x28c / +0x288**.
With our 0x320-byte V13_3 layout +0x28c was the high half of the stamp
pointer (0xffffffa0...): slot 0xa0, beyond the 128-entry event table --
the NULL.

G12 CP layout from the firmware's reads (`g5compute.G12WorkCommandCP`,
0x2e0, offsets checked on the fake AGX): V13_3's head -- counter +4,
context_id +0x10 (fw 0x20390), event_control +0x14, ComputeInfo +0x70,
microsequence pointer / size +0x1f0 / +0x1f8 (fw 0x101ec), ComputeInfo2
+0x1fc; **JobMeta at +0x274** (no_preemption byte +0x277 fw 0x10450,
stamp +0x278 stored as event_control's pstamp, stamp_value +0x288, slot
+0x28c, evctl_index +0x290 indexes event_control+0x74, flush +0x294 ORed
into event_control+0x20, uuid +0x298); ts1 +0x2a0, ts_pointers +0x2a8 /
+0x2b0, user_ts_pointers +0x2b8 / +0x2c0 (fw 0x1038c..0x103a0). The 0x10
bytes less between ComputeInfo2 and JobMeta are taken as EncoderParams
without its sampler fields (a guess: nothing read there seen yet).

Microsequence ops (interpreter fn 0x17970, dispatch `op & 0x3f` through
halfword table 0x495be, handler = 0x180e4 + 4 * entry): StartCompute 0x29
at 0x198b4 is V13_3's 0x16c-byte layout (computeinfo +0xc, cmdqueue +0x1c,
computeinfo2 +0x3c, unk_flag +0x154, counter +0x15c, event_ctrl_buf +0x164;
it advances 0x168 from op + 4); Timestamp 0x19 at 0x187f8 is V13_3's too
(ts0 +4, ts_pointers +0xc, cmdqueue +0x1c, user_ts +0x24, unk_ts +0x2c,
uuid +0x34). StartCompute copies **only ComputeInfo's first six qwords**
(+0x0..+0x28) into GPU state and tests bit 0 of the sixth (a helper /
enable flag?) -- the G12 ComputeInfo may differ from M1's too; still open.

**G5.1 run 2** (`agx-start-41.log`, G12 layout): the firmware took the
command (queue GPU_RPTR 1) and crashed -- this time with a crash message
-- on an instruction abort at address 0: `blr` at fw 0x1e19c through the
per-pipe handler table at 0x7c8d0 + 0x150 (5 pipes x 3 function
pointers, filled by fn 0x1fa80 from the static sets at DATA 0x4c000..
0x4c070), which was **all zero**: the scheduler had never been
initialised. fn 0x1fa80 (also clearing the event table 0x70c38, 0x1c00
bytes) runs from fn 0x2a61c, which the DeviceControl handler (fn 0x8a14,
0x30-byte messages, `switch (type - 4)` through table 0xb8c0) calls for
**message type 0x10** when its word +4 is 0. m1n1 sends DC_Init as 0x1a
(V13_3), which this firmware handles at 0xa9e4 by only setting the flag
at 0xbedb0 (also set before the firmware's own GPU power-down at 0xdec0).
DC_UpdateIdleTS 0x23 is the same in both (fw 0xab44 copies RegionC
+0xb368 to 0xbee78). Fixed in patch 0007: DC_Init is 0x10 on G12. This
may also be why the GPU never powered down in G5.0.

**G5.1 run 3** (`agx-start-42.log`, DC_Init 0x10): the firmware is busier
(255 Stats records "FW active" toggling instead of 5) but crashes the same
way, the handler table still empty. fn 0x2a61c (the 0x10 handler) runs
the scheduler init (0x1fa80 at 0x2ab24) only if **RegionC +0xb1dc** is
nonzero, and then clears it (`str wzr` at 0x2ab30; the same check and
clear in the scheduler at 0xdee0 / 0xec4c). That word lies in m1n1's
opaque `unk_9100` blob (RegionC 0x9254..0x10154), all zero; on iOS it
reads 0 too, but that dump was taken long after the firmware had cleared
it. Patch 0007 sets it to 1 for the A13.

**G5.1 run 4** (`agx-start-43.log`, RegionC +0xb1dc = 1): **no firmware
crash any more; the GPU ran our job and did not finish it.** The
firmware took the command (CL_0 channel read 1, queue GPU_RPTR 1), but
GPU_DONEPTR stayed 0, stamps and timestamps 0, the event never fired, not
even after kick_firmware (doorbell 0x10). Meanwhile it ran the GPU on
its own: Stats "Utilization 0% 0% 100% 100%" and "PowerState ACT ps=6"
(9081 Stats records in 10 s), Fender state 6, VOLMAN +0x38 0x01aaaaaa,
buck 0x4100 `09 aa` -- DVFS works now (G5.0's open item: the firmware
manages power once its scheduler is initialised). EP 0x20 messages
0x0042... every ~0.4 s are m1n1's "poll channels" events. A soft crash
afterwards: agx_background busy, no crash of its own.

GPU MMU (sgx + 0x17030, read once with the GPU on after the run):
FAULT_INFO 0x1600027fe2862210 -- **address 0x1600027fc0, context 3
(ours), unit 0x11 = UL1C1 (VDM/PDM/CDM L1), read, reason INVALID
(unmapped)**, FAULTED bit 0 (handled or latched). Our buffers: CDM stream
0x1600003f00, preempt 0x1600008060..0x1600010000, USC words at pipeline
offset 0x1bfc0, shader at 0x13f00. Nothing is mapped at 0x1600027fc0. One
reading: the CDM used a pipeline base of 0x160000c000 instead of
0x11_00000000 (0x160000c000 + 0x1bfc0 = 0x1600027fc0) -- the G12
firmware copies only ComputeInfo's first six qwords into GPU state and
pipeline_base (+0x30) is not among them. Unconfirmed; the G12 ComputeInfo
/ CDM words need a reference from iOS.

**iOS reference capture (planned).** `devices/a13/tools/ios/g12cap` (built by
`devices/a13/tools/ios/build.sh`): runs the same kernel through Metal with an
unusual grid (77 threadgroups x 13 = 1001 threads), checks the output,
then reads its own address space (regions listed first; its scratch
mapping, its image and the dyld shared cache skipped) for what
AGXMetalG12 wrote with the CPU: the CDM sizes (1001,1,1 / 77,1,1 /
13,1,1), the offline G12 code's first word (`f2051004`) and the out
buffer's gpuAddress. Each hit goes to `hits.txt` with 0x300 bytes around
it, each region with a hit to `region-<addr>.bin`, the region map to
`regions.txt` (default `/var/mobile/g12cap`). On the phone (iOS 16.4 +
Dopamine): `g12cap /var/mobile/g12cap 1`, then copy the directory back.
Kernel side next: the WorkCommandCP and ComputeInfo are the kernel's;
with the CDM stream's GPU address from the user-side dump, look for a
type-3 command (0x2e0 bytes, ComputeInfo +8 = that address) in the AGX
allocations (`kmem obj` -> fw object -> AGXAllocation kva / GPU VA, as
for `bufs.list` in G2) with `kmem kd`.

**iOS reference: captured** (2026-11-03, `devices/a13/build/gpu/g12cap-{1,2,3}`).
g12cap needs the GPU user-client entitlements (`g12cap.entitlements`:
AGXDeviceUserClient, AGXSharedUserClient, IOGPUDeviceUserClient, ...;
without them MTLCreateSystemDefaultDevice is nil); Metal's result is
right (1001 of 1001 words). The out buffer is CPU 0x1030f4000 but GPU
0x1500018000: user GPU VAs are their own space (0x15_....).

User side (AGXMetalG12's words, found in its CPU-mapped regions, VM tag
100):

- **CDM stream** (GPU 0x15000a0000): `00000002` (launch word 0), `00018004`
  (word 1: USC at pipeline offset 0x18000, low bits 4), global size
  **1001, 1, 1 -- in threads**, local size 32, 1, 1 (the driver re-blocked
  13 to 32: the kernel only uses thread_position_in_grid), barrier
  `60000160`, terminate `40000000 00000000`; 0x24 bytes to the
  terminate (= the command's encoder_end).
- **USC control program** (pipeline offset 0x18000): `03 00 40 f0 0a 08 00
  15` and `03 04 40 20 00 00 00 15` (tag 0x03 = uniform load, Mesa's
  Uniform field layout: halfs 0-3 from 0x1500080af0 = the argument table
  holding the out buffer's address, halfs 4-7 from 0x1500000020 =
  constants), `1f 80 00 90 40 00 00 00` (tag 0x1f = shader, flags 80 00
  90, code at pipeline offset 0x40), `00 00`, **`08 01` = G12's end of
  shader** (Mesa's G13 words end with `88 00` = G13's stop: the USC words
  are a small program in the shader ISA's encoding).
- **Code heap** at pipeline offset 0: entries with a 0x40-byte header
  (`80 00 ...`), the kernel's code at +0x40: `f2051004 e2000000 52288242
  10000e09 426a1800 00004511 200e00c0 1200520e 00000000 0801`.

Kernel side (`kmem` walk: accelerator +0x440 -> fw object -> 15
AGXAllocation / AGXSecureMemoryMap pairs, kva from the descriptor +0xb8;
dumps with g12cap alive; the CP commands sit in fw buffer
0xffffffa00c178000 at +0xc000, +0xc600, +0xc900):

- **WorkCommandCP = our G12 layout** (0x2e0): context_id 7 at +0x10,
  event_control +0x14, microsequence 0xffffffa00c3a4000 / 0x280 at
  +0x1f0, ComputeInfo2 +0x1fc (deflake_1 +0x224, encoder_end +0x22c),
  EncoderParams encoder_id 0x0c00ab92 at +0x268 and 0xffffffff at +0x270,
  JobMeta +0x274 (stamp +0x278, fw stamp +0x280, value 0x13d00, slot 5,
  uuid +0x298), ts_pointers +0x2a8, unk_ts +0x2d0, unk_flag +0x2d8.
- **ComputeInfo (G12): +0x0 0x1500080000 (a 0x8000 scratch buffer; the
  argument table is inside it), +0x8 the CDM stream, +0x10 0, +0x18
  pipeline base 0x11_0000_0000, +0x20 0, +0x28 0** -- everything else zero
  (M1's has the pipeline base at +0x30 and 0x8c60 / unk_58 1).
- Microsequence 0x258 bytes: StartCompute 0x16c (V13_3; unk_28 0,
  counter 0, unk_flag -> cmd +0x2d8, event_ctrl_buf -> event_control
  +0xa8), Timestamp 0x3c (unk_3 0x80; unk_addr = ts_pointers start, not
  0xdeadbeef), **WaitForInterrupt `01 00 80 00`** (we had 0, 0, 1),
  Timestamp (unk_addr = ts_pointers end), **FinalizeCompute 0x6c** (V13_3's
  up to restart_branch_offset +0x58, then one byte and the unk_flag
  pointer at +0x5d, 7 bytes pad), End `18 00 00 40`.

g5compute now builds all of this the same way (USC program, CDM words,
ComputeInfo, wait, the 0x6c Finalize); dry-run offsets equal Apple's.

**G5.1 DONE (run 5, `agx-start-44.log`, 2026-11-03): the first job on the
A13's GPU completed.** `--volman-gpu --fender-init --compute --watch 10`,
the empty kernel (`08 01`): the firmware took the command, the queue's
done pointer moved to 1, both stamps reached 0xc0000100, event 0 fired,
GPU timestamps 0x114d03d5d -> 0x114d03dca (109 ticks of 24 MHz, ~4.5 us),
no firmware crash, no fault. What it took, in order (runs 1-5): the G12
work-command layout (12.x line sizes, JobMeta +0x274), DC_Init = 0x10,
RegionC +0xb1dc = 1 (scheduler init), and Apple's ComputeInfo / USC
program / CDM words / wait op / 0x6c Finalize from the iOS reference.
Still open: the GPU stays on after the job (Fender 1, VOLMAN 0x3a; Stats
"Utilization 0% 0% 100% 100%" throughout) -- no idle power-down yet.
Next, G5.2: the store kernel with Apple's on-device code and its two
uniform loads (argument table, constants).

**Offline G12 compiler.** Xcode's Metal Toolchain (`xcodebuild
-downloadComponent MetalToolchain`, 27A5228f) has `metal-tt` and the
translator `applegpu-nt`; `applegpu-nt -available-archs ios 16.4` lists
`applegpu_g12p` (A13) next to g13p/g13g. `metal-tt -arch applegpu_g12p
-platform_version ios 16.4 16.4 LIB.metallib PIPELINES.mtlp-json -o
OUT.metallib` writes a Mach-O "GPU executable applegpu_g12p" whose
`__compute` section holds one Mach-O per pipeline with the shader in
`__TEXT,__text` (`devices/a13/tools/gpu/isa/gpumacho.py` prints it). AIR has to
be built for iOS 16.4 (`-std=metal3.0 -mios-version-min=16.4`; AIR 2.9
is refused, the G12 backend takes 2.5); `metal-objdump` has no
disassembler for these, dougallj's applegpu (G13) decodes by hand.

First comparison (`isa/g5-kernels.metal`: `out[tid] = 0xcafe0000 + tid`,
and an empty kernel):

| | G12 (applegpu_g12p) | G13 (applegpu_g13g) |
|---|---|---|
| store_const | `f2051004` `0e01424a18000000` `4501200e00c01200` `0801` | 0x40 zero bytes, then the same, ending `8800` |
| empty | `0801` | 0x40 zero bytes, `8800` |

`get_sr` (thread_position_in_grid), `iadd` and `device_store` are
**byte-identical**. Only the end differs: **G12 ends a shader with
0x0108**, G13 with `stop` 0x0088 (in G13's map 0x0008 is `trap`). G13's
code also starts after 0x40 zero bytes; G12's at once. So for G5.1 the
empty compute shader on the A13 is the two bytes `08 01`.

**G6 — kernel driver (35 %).** `drm/asahi` (Rust) with a G12/13.3 entry, in our
Hoolock-based kernel (needs Rust support in the tree) — or a C port of the parts
we need. Check: Mesa's `asahi` driver allocates, submits, waits.

**G7 — Mesa + desktop (30 %).** Mesa `asahi` with G12 compiler changes; glmark2,
XFCE on GL. Native display (still simpledrm) is a separate item in the roadmap.

## G4 summary: how the A13 GPU powers up (2026-11-03)

The long "G4 results" section below is the search log. This is what holds.

**The power chain.** The GPU firmware (or the host, for a test) asks
**Fender** (sgx + 0xd00000 = 0x204d00000) to power the GPU: +0x1000 = 0x11
(state 1 + request pending 0x10). Fender's sequencer steps through +0x1018
= 0 -> 2 -> 0x10 -> 5; at **step 0x10 it waits for the GPU's power rail**.
The rail is **PMU buck 0x4100** (spmi0 slave 0xf, `avus`). Fender's request
reaches the PMU through the PMGR's hardware voltage manager **VOLMAN**
(0x23b0a0000): Fender writes the rail's code (0x3a) into VOLMAN channel
+0x38, and VOLMAN, if that channel is under hardware control, sends it to
the PMU through spmi0's hardware queues. Then +0x1000 reads 1, the GPU
core answers, the firmware runs.

**What was missing.** iBoot leaves every VOLMAN channel in **manual mode**
(control bit 0 = 1); XNU's AppleT8030PMGR switches them to the hardware at
boot (fn 0xfffffff009463e6c). With the GPU's channel manual, the request
lands in VOLMAN (+0x38 = 0x3a3a, no bit 24) and stops there; Fender waits
at 0x10 for ever and the firmware's first GPU access is an SError (FAR
0x204010190). **The fix: clear bit 0 of VOLMAN +0x14** (control of channel
+0x38) before the GPU is used -- `agx-start.py --volman-gpu`.

**VOLMAN** (PMGR reg 0 + 0xa0000; read as 32-bit words):

| offset | meaning | iBoot (m1n1) | iOS |
|---|---|---|---|
| +0x0 | global | 0x01000000 | 0x01000000 |
| +0x4..+0x1c | 7 controls, bit 0 = manual; control +0x4 + 4i pairs with channel +0x28 + 4i | +0x4..+0x14 3, +0x18/+0x1c 1 | +0x4..+0x14 2, +0x18/+0x1c 0 |
| +0x20 | status (`PMGR_VOLMAN_STATUS`, `DVC_DONE_ERR` if & 0x7f) | 0 | 0 |
| +0x24 | config | 0x0851b092 | 0x0851b092 |
| +0x28..+0x40 | 7 channels: bit 31 busy, **bit 24 on (under hardware)**, bytes = voltage codes | codes, no bit 24 | 0x01xxxxxx |

Channels, the code XNU writes at boot (`devices/a13/tools/gpu/volman-codes.py`,
read-only; under m1n1 every one equals what iBoot already put there) and
the PMU buck it follows on iOS:

| channel | control | source of XNU's code (PMGR = 0x23b000000) | m1n1 | iOS | buck |
|---|---|---|---|---|---|
| +0x28 | +0x4 | ([PMGR+0x58018 + 20 v] >> 8) & 0xff, v = [PMGR+0x58000] & 0xf | 0x5a | 0x3e..0x70 | 0x4200 |
| +0x2c | +0x8 | [PMGR+0x5c008 + 8 v] >> 24, v = [PMGR+0x5c000] & 7 | 0x61 | 0x40..0x8f | ? (no `avus` buck matched; `dotara`, slave 0xe?) |
| +0x30 | +0xc | CPU cluster 1: [0x211e70000 + 0x20 s] >> 56, s = [0x211e20020] & 0xf | 0x39 | 0x30..0x81 | 0x4000 |
| +0x34 | +0x10 | CPU cluster 0: the same at 0x210e70000 / 0x210e20020 | 0x3e | 0x35..0xb0 | 0x4b00 |
| **+0x38** | **+0x14** | **not written by XNU: Fender's power request (GPU on 0x3a / 0x31, idle 0)** | 0 | 0 / 0x313a31 | **0x4100** |
| +0x3c | +0x18 | GPU perf: [PMGR+0x64008 + 8 v] >> 24, v = [PMGR+0x64000] & 7 | 0x43 | 0x3b/0x43 | 0x4500 |
| +0x40 | +0x1c | [PMGR+0x60008 + 8 v] >> 24, v = [PMGR+0x60000] & 7 | 0x34 | not read | ? |

(XNU's channel base is VOLMAN + 4: `mov w23, #4; movk w23, #0xa, lsl #16`.
The cluster codes come through vtable +0xa60, fn 0xfffffff009466b4c, which
maps cluster-relative addresses onto pmgr regs 3-11 / 12-18.)

**Proof** (`devices/a13/build/gpu/agx-start-37.log`, fresh proxy boot,
`--volman-gpu --fender-init`, only +0x14 changed): Fender +0x1000 = 1,
+0x1018 = 5; VOLMAN +0x38 = 0x013a3a3a; buck 0x4100 `09 3a` (iOS with the
GPU on: `09 31` / `09 3a`); the firmware takes initdata, DC_Init,
DC_UpdateIdleTS, reads DevCtrl (read pointer 2) and runs 5 s with no
crash. Every earlier run, whatever else was changed, stopped at 0x10.

**Why it took so long.** Every iOS reference was taken with the GPU idle,
and at idle iOS has the rail off too (+0x38 code 0, buck 0x4100 `00 00`), so
everything "matched" -- including the wrong conclusion that 0x4100 is a
spare phase. What found it: iOS's own PMGR log (debug-enabled iBoot,
pmgr-debug) showed no hidden kernel step; booting iOS's kernel by our own
boot path showed the hardware hand-off is fine (the firmware ran there);
sampling the rails with the GPU busy (`rails-gpu.sh`) showed 0x4100 / VOLMAN
+0x38 switching with the GPU; comparing VOLMAN under m1n1 showed the
manual bits.

**Open.**
- A second clean run, and a longer one (firmware Stats / FWLog over 30 s,
  idle power-off and wake-up of the GPU).
- Stats records are 0x40 bytes, m1n1 expects 0x60 (G5).
- The other VOLMAN channels stay manual under m1n1: the CPU and SoC rails
  stay at iBoot's codes (e.g. buck 0x4000 at 0x68 where XNU's table says
  0x39 for cluster 1's state 2), and the GPU-perf channel +0x3c will not
  follow GPU DVFS. Fine for bring-up; for Linux, decide whether to hand
  all seven to the hardware as XNU does.
- Linux: the same bit has to be cleared before the GPU is used -- in
  m1n1 (where the boot-time power setup lives) or in the kernel's PMGR /
  GPU driver.

**Do not.**
- Write the spmi0 hardware-queue words (+0x400.., +0x500.., +0x600..):
  not plain registers, a write zeroed them (`agx-start-36.log`).
- Read spmi0's +0x400..+0x6ff as a block: it disturbed the controller
  (new fault counts, +0xd00 changed); single config words are fine.
- Write PMU registers directly: VOLMAN and the queues are the way in.
- (Older, still true) read the GPU firmware's __DATA through kmem (resets
  the phone), bulk-read PMGR 0x23bc1c000, read Fender with GFX off.

## Tools and methods (state 2026-11-03)

**Proxy side** (`devices/a13/tools/gpu/`, run on the Mac with
`M1N1DEVICE=/dev/cu.usbmodem…1`, phone booted with `./cascadia a13 boot
proxy`; each run wants a fresh boot, a stalled Fender request does not
repeat):

| tool | what for |
|---|---|
| `agx-start.py [opts] [SECS]` | starts the GPU firmware with m1n1's AGX driver and prints Fender / VOLMAN (and with `--volman-gpu` buck 0x4100) before and after. **The working run: `--volman-gpu --fender-init SECS`.** Other options are the experiments of the search (each documented in the script's header); `--pre ADDR=VAL` writes before the request, `--try` while it is pending |
| `volman-codes.py` | read-only: the codes XNU would write into VOLMAN, the channels, the PMU bucks |
| `pmgr-dump.py`, `pmgr-names.py`, `pmgr-regions.txt` | dump / compare PMGR and related regions (`--dump-regions`); VOLMAN 0x23b0a0000 is in the list now |
| `pmp-devtable.py` | the PMP firmware's device table |

**iOS side** (`devices/a13/tools/ios/`, phone on iOS 16.4 + Dopamine, `ssh
iphone11-ios`; `kmem` built with `build.sh`):

| tool | what for |
|---|---|
| `kmem` | kernel / physical / MMIO reads and writes, `obj CLASS`, `spmir SLAVE ADDR LEN` (one SPMI read; loops of them with the screen on crash iOS) |
| `rails-gpu.sh [SECS]` | samples Fender +0x1000, VOLMAN +0x0..+0x3c and one PMU block every 0.3 s -- how the GPU's rail was found (screen off, then busy) |
| `pmgr-log.sh [SECS [MASK]]`, `pmgr-log.sh off` | turns ApplePMGR's debug log on in the running kernel (writes the `pmgr-debug` field, ApplePMGR +0xe0) and collects dmesg; bits: 1 PS config, 2 gating, 3 state changes, 11 PMP requests, 15 IOLog (default 0x880e) |
| `debug-ibss.py IN OUT [ARGS]` | makes an iBSS that sets `/chosen/debug-enabled` (nop at 0x29bf4) and carries our boot-args (cave 0xaf020); IN = surrealra1n's `iBSS.boot` (boots iOS from disk) or `inputs/iBSS…sigpatched.raw` (stays in Recovery) |
| `serial-log.sh` | records the DCSD serial port (or use SerialMonster; never both on one port) |
| `retag-img4.py IM4P TYPE IM4M OUT` | wraps Apple's IM4P byte for byte with another type and our IM4M (`ipsw img4 create` re-encodes LZFSE kernelcaches into something iBoot refuses) |

**iOS's own log of a GPU power-up at boot.** iOS ignores `pmgr-debug`
unless `debug-enabled`; boot surrealra1n's local-boot image patched by
`debug-ibss.py` (boot-args `-v serial=3 pmgr-debug=0x880e`) and record the
serial port. What it showed is in "G4 results" (the boot log).

**iOS's kernel by our boot path (the bisect).** `IBSS=$PWD/a13/build/gpu/
ibss-debug/iBSS.recovery.raw ./cascadia a13 boot ios` sends what our Linux
boot sends (stock DeviceTree, gfx and pmp firmware) but the root volume's
trust cache and iOS's kernel instead of m1n1. Needed, in
`devices/a13/build/gpu/ios-recovery/` (not in git): the phone's own
`apticket.der` (the root snapshot's name is derived from it; kernel, trust
cache and DeviceTree are wrapped in it), the phone's `firmware/`
(`/usr/standalone/firmware`, for `sep-firmware.img4`, sent with
`rsepfirmware`), and a kernel with APFS's root-hash requirement patched
off (`mov w20, #1` -> `#0` at raw offset 0x313909c, repacked LZSS +
`repair_lzss_img4.py`; the `arp0` boot-arg does not help). It gets to
launchd, which dies on the missing OS cryptex (`libSystem.B.dylib`) --
far enough: AGX starts long before.

## G1 results (2026-11-01)

`devices/a13/tools/ios/kmem.c`, built by `sh devices/a13/tools/ios/build.sh` (Xcode iOS SDK +
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

**Parsed with m1n1's own definitions** (`devices/a13/tools/gpu/agxparse.py`, `V13_3`,
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

**`CHIP_INFO[0x8030]` for m1n1** (`devices/a13/tools/gpu/chipinfo.py DUMPDIR HWDATA_A
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
`firmware`, `bootx`), checked with `devices/a13/tools/gpu/g3-check.sh` (phone: live ADT
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
- `__TEXT` read under Linux (`devices/a13/tools/peek/memdump`: /dev/mem `read()`
  fails outside RAM with EFAULT, `mmap` works, Device-nGnRnE) = the IPSW
  except iBoot's fill-ins, and = iOS's except the `__DATA` address in the
  segment table and the random `STKG`. The RTKit boot-argument table is
  iOS's: `PTPB` 0x8fffb8000, `PTPS` 0x40000, `PTVB` 0xffffff8000000000, `GptB`
  0x8ffff8000, `PplB` 0x8fffb4000, `SOC_` 0x8030, `SOCR` 0x11, `CpAd`
  0x206000000, `WrAd` 0x206400000.

So under Linux the GPU firmware sits loaded, configured and not yet started,
as iOS's kext finds it; G4 starts it. `__DATA` was not read (resets on iOS).

## G4 results (2026-11-01)

**Transport.** `./cascadia a13 boot proxy` boots m1n1 alone (iBoot still loads the
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

**Where it stands** (`devices/a13/tools/gpu/agx-start.py`, one fresh proxy boot per
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
+0x16c = 15, +0x170 = 0x1045050. `devices/a13/tools/gpu/pmp-devtable.py` runs that
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

**PMGR, iOS vs m1n1** (`devices/a13/tools/gpu/pmgr-regions.txt`, `pmgr-dump.py`
through the proxy, `kmem mmio` on iOS, `pmgr-names.py ADT ios proxy`; dumps
in `devices/a13/build/gpu/pmgr-diff/`, iOS idle, proxy after the GPU's request):

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
(idle; `devices/a13/build/gpu/pmgr-diff/ios-fender-{lo,hi}.txt`): +0x50 1, +0x5c
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

iOS, idle (`devices/a13/build/gpu/pmgr-diff/ios-groups.txt`): ISP_REF0 +0x40200..
0x80000101 0x85000001 0x80000101 0x80000101 0x83000101 0x03000101
0x03000101 0x8200011e; CLUSTER_CTL 0xf 0xf; SOC_TVM a table (+0xc8000
0x301, +0x60.. 0x0028001e 0x000501fb 0x000a01f6 0x000a01ec repeated);
MISC_CORES all 0; MISC_ACG +0 1, +0x20 0x7fff, +0x50 1, +0x68 0x1801,
+0x88 1.

**PMGR reg 0 starts with the PLLs**, 0x4000 apart: 0x23b000000 0x80c80300
0x80480168 0x804f012b..., +0x4000 0x80590100, +0x8000 0x80320181, +0xc000
0x803d0101 (bit 31 on, then the dividers; the rest of each the same).
**Reading on at 0x23b010000 took iOS down at once** (64 KiB window;
`kmem` now refuses 0x23b010000-0x23b03ffff): the next PLLs do not answer on
idle iOS -- off, most likely. Fender's step 0x10 (12-95 us) looks like a
PLL being started and locked: iOS keeps the GPU's PLL off while the GPU
idles and Fender brings it up. `agx-start.py --pll-probe` reads PLL0-6 one
word at a time before the GPU and, last of all, after its request.

Tried (proxy, `agx-start-16.log`): under m1n1 all seven read, all on, and
the GPU's request changes none of them: PLL0-3 as on iOS; PLL4 0x804b0100
**0x404800a8** (word 1 bit 31 clear, the others have 0x8048....) 0x804f0424;
PLL5 0x800e4001 0x810000f0 0xce59cb39; PLL6 0x80320100 0x804800c0
0x804f0224. Which PLL the GPU runs from, and which one did not answer on
iOS, is not known.

PMGR groups, iOS vs m1n1 (`--dump-regions`): **MISC_ACG +0 (0x23b0dc000)
iOS 1, ours 0** -- bit 0 is set and cleared by an ApplePMGR function (fn
0xfffffff0091b27d4: read, `& ~1 | on`, write): global automatic clock
gating, it seems; +0x84/+0x8c/+0x90 ours 1, iOS 0. MISC_CORES +0/+4 ours
0x3e, iOS 0 (an ApplePMGR core-mask writer, fn 0xfffffff0091b1530: CPU
cores, likely). SOC_TVM +0 iOS 0x301, ours 0x300. ISP_REF0 clock configs
differ (sensor clocks). m1n1's USB was gone after this session again; it
seems to happen after every agx-start session now. `--try ADDR|=MASK`
(repeatable) applies such candidates one by one with the request pending,
printing Fender after each, so one boot tests several.

Tried (proxy, `agx-start-17.log`, request pending): MISC_ACG 0 -> 1,
SOC_TVM 0x300 -> 0x301, PLL4 word 1 0x404800a8 -> 0xc04800a8: Fender stays
0x11 / step 0x10 after each. **Ruled out so far for step 0x10:** PMP
(doorbell registered), GPU_TRIG_CTL, GFX and fabric auto-PM, pwrgate +0,
Fender's own registers (whole 0x20000 alike but +0x4200/+0x409c), the
m1n1 poke, MISC_ACG, SOC_TVM, PLL4 word 1. Not compared yet: the AGX core
registers (sgx 0x204000000, 16 MiB; reads while the GPU is off may stop
iOS like the PLL window did -- keep the GPU busy on iOS while reading),
the second PMGR (reg 1, 0x23d200000: ps 0x23d280000, pwrgate 0x23d29c000,
perf 0x23d2b4000), and the GPU firmware's own steps before it writes
Fender (what iOS's firmware has done by then that ours has not).

**Outside reading (2026-11-02).** Nothing public on Fender or the A13/G12
GPU (Asahi docs, drm/asahi, the pyre t8030 emulator's GPU scoping study:
high-level emulation, no power-up). The M3 forks (aurora-silicon/linux,
omacom/linux-aurora) say the M3 Pro GPU "needs the PMP's AGX power vote
before its firmware can start": an untagged message on the PMP's endpoint
0x20, `0x0020f50100000000` (command 0xf), reply `0x0021000000000000`
(aurora-silicon/linux#358, `drivers/soc/apple/pmp-display.c`); the M3
Max's GPU runs on its PMGR domain alone. SGX+0xd14000 = 0x70001 is their
"sgx_setup" and makes no difference to the firmware there either.

**Our PMP has command 0xf too** (t8030pmp fn 0x100c228): domain = bits
43:40, below the count of the config's domain list (config +0x28: 4
domains **SOC, DCS, DISP, AVE**, 3 levels each), device = the table index
in bits 39:32 (GPU = 3), arg1 = level (taken if below the domain's count,
else 0); it stores the level in the device's vote table (entry +0x68,
per domain; **the GPU's has slots for SOC and DCS**, the CPUs' none),
recomputes the domain's level (fn 0x100f1e4, table 0x1045f50) and, if it
changed, applies it (fn 0x100e134). No reply. ApplePMGR has per-device
perf-state requirements on PMP-managed perf domains
(`devicePerfStateRequirements`, `kPerfDomainFlagPMPManaged`) and GFX has
`notify_pmp`: on iOS, GFX powering up most likely raises SOC/DCS through
this vote; m1n1 never sends it. Fender's step 0x10 waiting 12-95 us fits a
voltage or memory-clock change. `agx-start.py --pmp-vote 0:2,1:2` sends
the GPU's votes (SOC 2, DCS 2) after the PMP is configured, before the
GPU starts.

Tried (proxy, `agx-start-18.log`): `0x0020f00300020000` and
`0x0020f10300020000` sent (no reply, as expected), PMP alive; Fender still
0x11 / step 0x10. The votes do take: the screen (m1n1's framebuffer) turned
to coloured noise during that run -- the DCS (memory controller) level
changed under the display, which iOS would coordinate. So command 0xf
works, and raising SOC/DCS does not move Fender's step 0x10. **Do not vote
on DCS (domain 1) again under m1n1.** Next on iOS: `kmem mmiowatchr SECS PA LEN
[PA2 LEN2]` watches whole ranges, to catch which pwrgate entry (0x23b09c000
.. +0x300) or PS register changes in step with Fender's +0x1018 when iOS
powers the GPU up.

**On iOS the GPU wakes ~370 times in 6 s** (`kmem mmiowatchr`, apps
opening): GFX's PS register 0x1400034f -> 0x1f0003ff ~7 us *before* the
firmware writes Fender (auto-PM wakes the domain), then Fender 0x11 / step
2 -> (0x10) -> 5 and +0x1000 = 0x01 ~105 us later; on the way down GFX
returns to 0x1400034f (clock-gated). No pwrgate entry and no other PS
register moves with it beyond chance (devices/a13/build/gpu/pmgr-diff/ios-watch-
{pwrgate,ps}.txt). Ours (`--gfx-auto`) has GFX at 0x1f0003ff during the
request, as iOS: **the PMGR side matches; step 0x10 waits on something
inside the GPU block.**

**Our boot skips the second stage.** `boot-from-recovery.sh` sends
devicetree, firmware and `bootx` straight to the patched **iBSS**; every
iOS boot runs a second stage (iBoot from NAND, or iBEC for restores), and
surrealra1n's A12/A13 tethered boots send the same patched image again
tagged `ibec` before the devicetree (`surrealra1n.sh`: `img4 -i iBSS.patch
-o ... -A -T ibec`; boot: iBSS, iBEC, DeviceTree, trust cache, kernel,
bootx). The image is one unified iBoot (8422.100.650; it knows `/boot/
iBEC`, "load upgrade iBEC"); what it sets up only as a second stage is
unknown, GPU-side setup among the candidates. `IBEC=1 ./cascadia a13 boot
proxy` now sends `inputs/iBEC.ibec.img4` first (the patched iBSS tagged
ibec, made with `ipsw img4 create --type ibec`), waits for Recovery again,
then the usual sequence.

Tried (`IBEC=1 ./cascadia a13 boot proxy`, `agx-start-19.log`): Fender 0x11 /
step 0x10 as before, PLLs and PMGR as before. **The second stage is not it**
(as far as m1n1 can tell the boot is the same; nothing in the ADT says which
stage booted it).

**iOS's AGX driver powers the GPU up itself, once, at init** (AGXG12P
20E247, fn 0xfffffff0084f1b54 -- the one that reads `model-slow`, `no-gpc`,
`has-kf` from the sgx node; Fender's mapping through vtable +0x78):

    if (!(platform flags & 8)) {          // global at 0xfffffff00a4fa0ca
        Fender[+0x10000] = 0xffff0000;
        Fender[+0x0]     = 0x40000000;
    }
    Fender[+0x1000] = 0x11;               // state 1, pending
    while (Fender[+0x1000] & 0x10) ;      // busy-wait

(0xfffffff0084f2340..2360; before it, two calls through the power object,
vtable +0x568 (1, 0) and +0x570 (1, &out, 0).) A re-init path in the same
function writes +0x1000 = 0x10 and waits likewise (power down). We never
wrote +0x10000 or +0x0: both read 0 on idle iOS, so they look like
triggers/masks, not state, which is why the dumps did not show them.
`agx-start.py --fender-init` does the same before the firmware starts.

Tried (proxy, `agx-start-20.log`): the writes read back 0 (write-only or
self-clearing), and **the host's own request, before any GPU firmware runs,
stops at the same place**: +0x1000 = 0x11, +0x1014 = 2, +0x1018 = 0x10 for
the whole second. Then the firmware, finding Fender already mid-request,
took an **SError at FAR 0x204010190** (sgx +0x10190, an RGX register: the
GPU core is not powered). So the stall has nothing to do with the GPU
firmware: Fender cannot bring the core's power island up on this boot.
What iOS has and we do not is now most likely below the SoC's registers --
the GPU's supply (VDD_GPU on the PMU, `avus` on spmi0, 0x23d180700; its
register map is not in the ADT) or something the PMU/SPMI side does for
GPU power-up. The PMP's telemetry table names GPU [V]/[T]/[I] and
GPU_SRAM [I] (shmem +0x6070..; the values live elsewhere).

**The GPU's supply, what is known** (20E247): iOS's kernel does not switch
bucks for the GPU. `com.apple.driver.AppleSPMIPMU` / `AppleARMPMU` deal with
battery, RTC, LPEM, AAPF; the ADT's `spmi-pmu` (`pmu,avus`, `has-fw 1`) lists
fault names and PTMU regions, no bucks. The SoC's fast voltage path to the
PMU is **DWI** (`/arm-io/dwi`, `dwi,t8030`, 0x23b200000, PMGR device 50,
on in both boots, PS 0xff): `com.apple.driver.AppleS8000DWI` reads
`dwi-version`, `nclk-div`, `polarity-config`, `lockout-us`, `str-delay`
(and `soc-buck`, `slave-select`, `voltages-buck%x`, absent on the A13) and
writes three registers at start and on wake (+0x0, +0xd0, +0x80, from its
fields +0xb0/+0xb4/+0xbc); it exports `AppleS8000DWIVoltageFunction` and a
backlight function, but in the A13's ADT only `/backlight`'s
`function-backlight_update` points at DWI (phandle 40). The GPU's voltage
moves are then hardware (PMGR's GFX perf / Fender -> DWI or SPMI -> PMU),
set up by iBoot or the PMU's own firmware. DWI +0x0..+0x100 is added to
`pmgr-regions.txt`, for the next iOS/m1n1 pair of dumps.

DWI on iOS (`devices/a13/build/gpu/pmgr-diff/ios-dwi.txt`): +0x0 1, +0x4 0x78, +0x8 1,
+0xc 2, +0x10 0x800000ff, +0x20 0x200, +0x60..+0x7c 0xffffffff, +0x80
0x1070, +0x84 0xa00004e4, +0x8c..+0x9c 1, +0xa0 0x70, +0xc0 0x70,
+0xd0..+0xdc 0x2ee00, +0xe0..+0xfc 0x40. **Reading on from 0x23b200100
stalled iOS** (a hang, not a reset); kmem refuses DWI past +0x100.

**DWI, iOS vs m1n1** (`agx-start-21.log`, `--dump-regions`; same before
the GPU and after its request):

| DWI | iOS | m1n1 |
|---|---|---|
| +0x4 | 0x78 | 0x3c |
| **+0x10** | **0x800000ff** | **0** |
| +0x20 | 0x200 | 0 |
| +0x84 | 0xa00004e4 | 0xa00005ad (backlight level, it seems) |
| **+0xd0..+0xdc** | **0x2ee00 each** | **0** |

The rest (+0x0 1, +0x8 1, +0xc 2, +0x60.. -1, +0x80 0x1070, +0x8c.. 1,
+0xa0/+0xc0 0x70, +0xe0.. 0x40) is the same. iOS has a channel enabled
with a mask of 0xff at +0x10 (the driver logs a "buck mask") and per-channel
timings at +0xd0; m1n1's boot has them off -- **the SoC's voltage path to
the PMU may be closed**, which would leave Fender's step 0x10 (a voltage
wait, 12-95 us on iOS) waiting forever. Test: with the request pending,
`--try 0x23b2000d0=0x2ee00 ... --try 0x23b200010=0x800000ff --try
0x23b200020=0x200 --try 0x23b200004=0x78` (`--try` now takes ADDR=VAL).

Tried (proxy, `agx-start-22.log`, request pending): writing +0xd0 sets
+0xd0..+0xdc together (one register, mirrored) to 0x2ee00; +0x4 takes 0x78;
**+0x10 and +0x20 read back 0 after writing** -- status, not configuration:
on iOS they hold the trace of DWI transactions (bit 31 + 0xff), on ours
nothing has gone over DWI. Fender stays at step 0x10 after each. So DWI's
own settings are not it; whether the PMGR ever sends the GPU's voltage over
DWI (on iOS, at GPU power-up) is not known.

**Outside reading, second pass (2026-11-02):** nothing public on DWI's
role, Fender, or the GPU's supply; Corellium's M1 talk only names PMGR,
DWI, SPMI and PMP as blocks (A10+ PMP; their A10 PMP log shows the same
RTKit start). An M-series sleep study (JJRPF/Aurora-DeepIdle#5) lists
AppleS8000DWI among the wake actions and an `AGX_PMGR` block (+0x100 = 0,
polled with timeout 0x2ee00). In the A13's ADT only the backlight uses DWI,
so DWI's status words on iOS are most likely the backlight's: **DWI is
probably not the GPU's path**. The likelier one is the PMGR's own SPMI
masters (SOC_SPMI0-2) writing the PMU (`spmi0`, slave 0xf) directly.
`agx-start.py --pmu-dump` reads the PMU's whole register space (8-byte
extended-long reads) before the GPU starts and with its request pending and
prints the bytes that changed: a buck's voltage register moving would show
the GPU's rail; nothing moving would say the PMGR never reached the PMU.

Tried (proxy, `agx-start-23.log`; `devices/a13/build/gpu/pmgr-diff/proxy-pmu-
{before,after}.bin`): all 8192 reads answered (22 s each). 296 bytes
changed, all of them logs and counters: the PMU firmware's ASCII log at
+0x8840.. ("ibat ..., vbat ..., rtc ..."), RTC/ADC words (+0x80.., +0x1102,
+0x2820.., +0x81xx-+0x82xx). **No regulator register moved: nothing asked
the PMU for anything during the GPU's request.**

The PMU's regulator blocks (one per 0x100, same layout): **bucks** at
0x4000, 0x4100, 0x4200, 0x4300, 0x4400, 0x4500, 0x4b00 (+0x0 = on, +0x1 =
voltage code: 0x68, **0x00**, 0x5a, 0x4d, 0x94, 0x70, 0x68), LDO-like
blocks at 0x4600-0x4a00, 0x4c00. **0x4100 is off** (+0x0 = 0, code 0,
+0x4 = 0x0f where the others have 0). 0x4000 and 0x4100 share a richer
configuration (+0x12/+0x13, +0x42..+0x60, +0x73..): two phases of one
multi-phase buck, or two bucks of a kind. Whether 0x4100 is the GPU's rail
(off on our boot) or a second phase shed at light load needs iOS's values:
**no blind writes to the PMU** (a wrong rail or phase could damage
hardware). `kmem spmir SLAVE ADDR [LEN]` reads PMU registers on iOS
through the spmi0 controller (STATUS +0, CMD +4, REPLY +8, extended-long
reads of 8 bytes; gives up if the FIFOs are busy).

iOS (`kmem spmir 0xf 0x4N00 0x10`, `devices/a13/build/gpu/pmgr-diff/ios-pmu-
bucks.txt`): **0x4100 is off on iOS too** -- a spare or shed phase, not the
GPU's rail. *(Wrong, 2026-11-03: read with the GPU idle. 0x4100 is the
GPU's rail and is off at idle on iOS too; see the G4 summary.)* Every other buck is on in both boots; four run at other, moving
voltage codes on iOS (0x4000: 0x44 then 0x30, ours 0x68; 0x4200 0x3e vs
0x5a; 0x4500 0x3b vs 0x70; 0x4b00 0xb0 vs 0x68 -- DVFS at work), 0x4300
(0x4d) and 0x4400 (0x94) are the same. **The supply is on; the PMU is not
the cause.**

The second PMGR (pmgr reg 1, 0x23d200000; `devices/a13/build/gpu/pmgr-diff/ios-
mpmgr.txt`) reads fine on iOS: its PS block holds the AON "nub" devices
(NUB_SPMI0-2 0x1400024f, NUB_AON 0x1f0002ff, NUB_GPIO 0xff, NUB_FABRIC
0x1f00030f, NUB_SRAM 0x1000230f, DEBUG_USB 0x1000030f, DEBUG 0x2ff), its
pwrgate group 0x23d29c000.. a dozen entries. Added to `pmgr-regions.txt`
for the m1n1 side.

**GPU-block registers on iOS with the GPU on** (`kmem agxr PA LEN`: before
each word, wait for Fender +0x1000 == 0x1, read, check it still is; apps
opened in a loop to keep the GPU waking; files `devices/a13/build/gpu/pmgr-diff/
ios-{gmgifaf,rgx-10000,fender-on-*,metrology}.txt`):

- Fender while on vs idle: +0x1000 1 / 0, +0x1014 7 / 3, +0x1018 5 / 0,
  +0x1100 0xe0000000 / 0xe0000001, +0x4064 1 / 0, the +0x8000 block (1,
  0x3680d8, 0x3680d8, 0x23fffe) 0 while on, +0x1401c 1 / 0. Ours after the
  request: +0x1100 like "on", the rest like "off".
- **GMGIFAF** (0x204d71000, the GPU's fabric interface): +0x0 0x100, +0x20
  0x20, +0x24 0x0049494c, +0x28 0x303, +0x2c 0x003a3c3c, +0x30 2, +0x40
  0x00140014, +0x44 0x801d0005, +0x48 0x8404, +0x4c 4, +0x60 0xc0800080,
  +0x70 5, +0x78 0x180a151f, +0x80 0x00ea0155, +0x84 0x01000071, +0x88 1,
  +0x90 0x40b, +0x94/+0x98 0x180000, +0x200 0x01000080, +0x204 0x02000180,
  +0x20c 0x20000000, +0x800 0x100.
- RGX 0x204010000..+0x400: only +0x2e4 = 0xd5. **Reading RGX from
  0x204000000 (64 KiB window) stopped iOS** even gated.
- Metrology (0x204d80000): sensor tables (+0x0 0x7530, +0x4 0x00100010,
  +0x8 0xf0100063, +0x1400.. 0x03000009 ... per block).

Whether ours has GMGIFAF/Metrology set (they may answer with the GPU off,
or not): `agx-start.py --risky-dump` reads them last of all.

Ours (proxy, `agx-start-24.log`, after the request): **GMGIFAF answers with
the GPU off** and is mostly iOS's; it differs in what look like fabric
QoS/bandwidth settings and one status-like word:

| GMGIFAF | iOS (GPU on) | m1n1 |
|---|---|---|
| **+0x30** | **0x2** | **0x202** (bit 9) |
| +0x2c | 0x003a3c3c | 0x003a3a3c |
| +0x60 | 0xc0800080 | 0xc0600060 |
| +0x78 | 0x180a151f | 0x080a141d |
| +0x80 | 0x00ea0155 | 0x00d20134 |
| +0x84 | 0x01000071 | 0x00010068 |
| +0x90 | 0x40b | 0x207 |
| +0x94/+0x98 | 0x180000 | 0x80000 |

Metrology is half-filled on ours (the firmware's sensor setup stops early);
the second PMGR differs only in its nub domains' auto modes. Next try, with
the request pending: GMGIFAF +0x30 bit 9 (clear it, or write 0x200 in case
it is write-1-to-clear), then iOS's QoS words.

Tried (proxy, `agx-start-25.log`, request pending): +0x30 takes 0x200 and
0x2 (a setting, not status), +0x2c, +0x60, +0x78, +0x80, +0x84 take iOS's
values, +0x90..+0x98 are read-only; Fender stays at step 0x10 after each.
**GMGIFAF is not it.**

**Ruled out for Fender's step 0x10, all told:** the GPU firmware and its
interface (the host's own request stalls too), the PMP (doorbell, votes,
DevPwr), PMGR GFX/fabric auto-PM, GPU_TRIG_CTL, pwrgate +0, MISC_ACG,
SOC_TVM, the PLLs (all on), DWI settings, the PMU's supply (bucks on as on
iOS), the boot stage (iBEC), Fender's own registers (+0x10000/+0x0 init
writes, +0x4200, +0x409c, the m1n1 poke), GMGIFAF. Left: the AGX core
itself (RGX, 0x204000000: unreadable on iOS even with the GPU on, an SError
on ours), and what iOS's driver does just before its own Fender request --
two calls on its provider (vtable +0x568 (1, 0), +0x570 (1, &out, 0), AGXG12P
fn 0xfffffff0084f1b54) that were not followed yet.

Followed (static, 20E247): the provider is the sgx node's
**AppleARMIODevice**; its +0x568 (state, index) takes `index` into the
node's **`power-gates`** list (<0xd0, 0xd2>: GFX-SGX, GFX-BUSY) and its
+0x570 into **`clock-gates`**, then calls the same slot on its own provider
(`[this+0x88]`; kdebug codes 0x5070000/0x5070004). So **just before its own
Fender request, iOS's AGX driver enables power-gate 0 (GFX-SGX, 208) and a
clock gate through the platform's power-gate interface**; m1n1 skips both
(they are `no_ps` virtual PMGR devices). The provider is not AppleARMIO (its
slots return kIOReturnUnsupported) nor AppleARMPE; ApplePMGR's slot goes to
fn 0xfffffff0091c2240 -- what it writes for a no_ps device (the PMGR's
`_pwrGateRegToPowerDomainIDMap`, built at run time from `pwrgate-regs` and
`power-domains`, GFX = domain 14, perf index 71) is the open question.
The perf block 0x23b0b4000 is all {1, count} on iOS and all 0 on ours, for
every index (residency counters ApplePMGR switches on, it seems; GFX device
at +0x1e0, GFX domain at +0x470).

Followed further (static): the sgx nub's provider is **AppleT803xIO**
(`com.apple.driver.AppleT8030`, it creates the arm-io nubs, vtable slot
0x580); its clock-gate (+0x568) and power-gate (+0x570) call platform
function objects' +0x80: `ApplePMGRFunctionClockGate` -> fn
0xfffffff0091bdc2c -> (PMGR workloop) fn 0xfffffff0091bdd18, the PMGR's
generic set-device-state; `ApplePMGRFunctionPowerGate` -> fn
0xfffffff0091becb0, which only **queries** (no_ps devices -> "off"). The
set-device-state engine skips devices that are both `perf` and `no_ps`;
GFX-SGX is `no_ps` only, so it runs: it resolves parents (GFX), programs
PS registers (fn 0xfffffff0091be3d8), and **for devices with `notify_pmp`
(GFX) tells the PMP**: fn 0xfffffff0091adcd4, PM command **0xe, or 0xf when
the device's flag bit 7 is set -- GFX has it** -- through ApplePMP's
platform function (fn 0xfffffff0091e05a4 -> `_sendPMCommand(cmd, args[0],
args[1], args[2], args[3])`, packed `0x0020000000000000 | cmd<<44 |
args[0]<<40 (4 bits) | args[1]<<32 (8 bits) | args[2]<<16 | args[3]`).
Which values ApplePMGR passes for GFX goes through its vote list (fn
0xfffffff0091a8130) and was not pinned down. The PMP keeps the result in
its __DATA, in SRAM (0x23b500000 = VA 0x1000000): the GPU's vote slots
(SOC 0x102bb20, DCS 0x102bb4c), domain levels (0x1045f50). Next: read the
PMP's whole __DATA (0x23b52a000, 0x1d000) on iOS and on ours and compare
(added to `pmgr-regions.txt`).

**The PMP's __DATA on iOS** (`kmem mmio`, all of it readable;
`devices/a13/build/gpu/pmgr-diff/ios-pmp-data.txt`): state 0x1046018 = 1; the
device table (0x1045050..) is byte for byte the static image's (the GPU's
thresholds +0x2a..+0x2e are 0 on iOS too); **only ISP (slot 4) has a
doorbell registered (n 0x21) -- the GPU has none on iOS**; the GPU's vote
slots hold a per-perf-state table, SOC 0 0 1 2 2 2 2 and DCS the same
(words +4..+0x18 -- the `pm-gpu-constraint-p1..p6-soc/dcs` settings the
PMP lists in its shared memory), not what command 0xf writes (one level in
all ten words). So on iOS the PMP plays no part in powering the GPU up
either; the constraints only matter once the GPU changes performance
states.

The PMGR's apply step (fn 0xfffffff0091be3d8), for `no_ps` devices
(flag bit 4), touches no PS register: it signals a per-device "event"
indexed by a byte of its device record (+0x19; GFX-SGX's looks zero, and
zero is skipped). So iOS's clock-gate call for GFX-SGX comes down to GFX
(parent) on -- ours is on -- plus the PMP notice for GFX.

More (iOS): the PMGR perf block's entries are {1 = enabled, 64-bit count
at ~24 MHz} (GFX device +0x1e0, GFX domain +0x470): residency counters,
not clock settings. The ADT's `perf-domains` are SOC, ECPU, DCS, PCPU, ANE,
AVE, DISP -- **no GPU**: the PMGR's DVFM does not manage the GPU's voltage
(`voltage-states9`). Bucks on iOS over a few seconds: 0x4000 (with its
second phase 0x4100) follows CPU load (codes 0x30..0xa1), 0x4b00 and
0x4200 move too, also with the GPU off; 0x4500 steady. `kmem gpuvwatch
SECS ADDR...` samples Fender +0x1000 and PMU registers in turns to find
the GPU's rail; **with the screen on it took iOS down within seconds**
(SPMI reads racing the kernel's own PMU traffic, most likely) -- with the
screen off it ran (the GPU never woke).

PMP __DATA, iOS vs ours (`agx-start-26.log`, `--dump-regions`): ~5400
words differ, almost all heap (different allocation order) and the PMP's
window table (iOS maps the register windows at 0xc0e..., m1n1's driver at
0xc08...); nothing GPU-specific stands out. The sgx nub's `clock-ids`
(<0x13b>) methods (AppleARMIODevice slots 0x548-0x560) are not called by
AGXG12P or its RTBuddy (one PAC-diversifier hit was another object's
+0xa0). AGX's other gate calls (fns 0xfffffff0084cfea0, ...84d8904,
...850f210, ...850f358) are power-down / query paths.

**Status (2026-11-02, end of day; superseded 2026-11-03 -- the comparison
missed VOLMAN and was done with the GPU idle, see the G4 summary):**
everything outside the GPU block that
could be read and compared matches iOS or was set like iOS without effect:
PMGR (PS/auto, triggers, pwrgate, groups, perf counters, second PMGR),
PLLs, DWI, PMU bucks, PMP (state, votes, doorbells -- iOS has none for the
GPU), Fender's registers and its iOS driver's init writes, GMGIFAF, the
boot stage. Fender's power sequencer still stops at step 0x10 even for the
host's own request with no firmware running, and the RGX core then is
unpowered. What step 0x10 waits for is not visible from outside the GPU.

**What Asahi does on M1** (drm/asahi `regs.rs`, `driver.rs`, `gpu.rs`;
m1n1 `kboot_gpu.c`, `proxyclient/m1n1/agx`, `hv/trace_agx.py`; t8103
device tree): nothing special for the GPU's power. m1n1 calls
`pmgr_adt_power_enable("/arm-io/sgx")` and reads the core masks at
sgx+0xd01500/0xd01514 (Fender +0x1500); the driver reads the ID registers at
sgx+0xd04000.. (Fender +0x4000, the block that matches iOS here) before the
firmware runs, starts the ASC and sends initdata; the proxyclient's only
poke is sgx+0xd14000 = 0x70001 ("Early init, needed?"). The DT gives the
GPU one power domain, `ps_gfx`, whose parent is `ps_pmp`. The firmware does
all GPU power management after that. So on Macs the GPU powers up "by
itself" after iBoot (normal boot mode) -- ours is booted through DFU
(iBSS/iBEC).

**PMGR bridge settings.** The pmgr node has 18 fabric bridges (`#bridges`,
`bridge-reg-index` 26: pmgr reg 26.. = 0x262000000, 0x264000000, ...,
**0x205000000** (bridge 7, right after sgx), 0x60c000000, ...) and
`bridge-settings-N` tunables, (offset, value) pairs. **bridge-settings-7,
the GPU's bridge**: +0x0 0x410, +0x400 0x40010001, +0x600 0x01ffffff, +0x708
0x02000000, +0x70c..+0x718 0x00400080, +0x71c 0x00100010, +0x800 0x101.
iBoot applies bridge settings (`bridge-settings-%d`, "pmgr %s %x bridge %d
doesn't exist" in its strings); m1n1 does not, and with the GPU domain off
at our iBoot's time the GPU's bridge may never have been set (or lost it).
Next: read 0x205000000 on ours (added to `pmgr-regions.txt`) and apply the
pairs with `--try` while the request is pending.

Tried (proxy, `agx-start-27.log`): **every bridge-settings-7 value is
already in place on ours** (iBoot applied it in our boot too: +0x0 0x410,
+0x400 0x40010001, ..., +0x800 0x101 read back before writing). Not it.

**Outside reading, third pass:** surrealra1n's A12/A13 tethered downgrades
boot iOS 14/15 through iBSS -> iBEC -> DeviceTree -> trust cache ->
kernelcache -> `bootx` (no GPU firmware sent) and are usable phones: the
DFU path does not leave the GPU unusable for iOS -- iOS's kernel does what
is needed, in a way that leaves no trace in the registers compared so far.
A reset or a power cycle would be such a thing. AGXG12P calls on its nub
only clock/power-gate (slots 0x568/0x570) and slot 0x38 (bookkeeping), no
reset; but its re-init path gates GFX-SGX **off** (0x...84f1c94/1cd8, w1 =
0) before gating it on again, and switching off GFX-SGX can take its parent
GFX with it: a GFX power cycle. Our GFX was only ever switched on, from
the state iBoot's DFU path left (with the GPU firmware loaded).
`agx-start.py --gfx-cycle` (GFX target 0 until actual 0, then 0xf) and
`--gfx-reset` (RESET/DEV_DISABLE pulse) try that before the firmware
starts; with `--fender-init` the host's own power-up request follows at
once, so Fender's answer shows even if the cycle upsets gfx-asc.

First try (`agx-start-28.log`): GFX goes off (0x1f0000ff -> 0x0f000300,
actual 0, was-power/clock-gated set); reading Fender then hung m1n1 (the
GPU block does not answer with GFX off -- the cycle now does not read it).

Second try (`agx-start-29.log`): GFX off (0x0f000300) and on again
(0x0f0001ff), auto back on, then a RESET/DEV_DISABLE pulse (0x1f0000ff):
Fender reads all 0 at +0x1000 afterwards and +0x1401c = 0x64 survives (iOS
idle 0, on 1); the host's own request then stops at **step 0x10 again**,
and the firmware takes the same SError (FAR 0x204010190). **A GFX power
cycle or reset does not clear it.**

**iOS's own log of a GPU power-up.** The phone's iOS is booted tethered
(surrealra1n + USBLiter8: `~/surrealra1n/boot/iPhone12,1/16.4/iBSS.boot`,
iBoot64Patcher2 `-b "-v"` + iBootpatch2, kernel and the rest from the NVMe),
so boot-args are ours: the string sits in a cave at file offset 0xaf020 with
2 KB of zeros after it. But ApplePMGR parses its debug boot-args
(`pmgr-debug`, `pmgr-debug-devices`, `pmgr-trace`, ...; fn
0xfffffff009194540) only when `PE_i_can_has_debugger()` (kernel fn
0xfffffff00846444c, `debug_enabled` at 0xfffffff00798c928, from
`/chosen/debug-enabled`) is true, which it is not on a production phone. So
`devices/a13/tools/ios/pmgr-log.sh` writes the value straight into the field the
boot-arg fills, ApplePMGR +0xe0, through Dopamine, and reads dmesg. The
`pmgr-debug` bits, from the format strings they gate: 1 PWR_*_CFG0 / *_PS
(MANUAL_PS, AUTO_PM_EN), 2 autoClockGating / autoPowerGating / reset per
device, **3 "%s: %u -> %u" (every device state change)**, 4 perf-state
limits, 5 wake times, 6 ROSC guardbands, 7 DPE nodes, 9 interrupts, 10
_pmPerformanceBoost, **11 PTD-SOC-DEV-PS-REQ/-ACK/-DVFS (device requests to
the PMP)**, **15 IOLog instead of kprintf** (kernel fn 0xfffffff0084647c8,
serial only). Default mask 0x880e. AGXG12P has no such switch (only a
firmware trace ring, `drainFirmwareTraceRing`). `pmgr-debug-devices`
(+0x14c, per die) holds a few packed device ids for extra tracing, not a
filter. First runs: the log works (`[Die 0] SPI3: power-off`, `PMGR:
SPI3_PS: 0x80208: MANUAL_PS=0, AUTO_PM_EN=FALSE`, ...), but SPI3 switches
on and off many times a second and the kernel's message buffer holds ~230
lines, so the script polls dmesg (`/var/jb/sbin/dmesg`; the phone has no
awk) every 0.2 s and merges. With the phone idle, no GFX/SGX line.

Lock / unlock (`pmgr-log-222859.txt`, 6520 PMGR lines in 40 s): the only
GPU lines are `GFX-BUSY: power-off` 10 ms after the backlight goes off and
`GFX-BUSY: run-max` just before IOMFB reloads at unlock. **GFX, GFX-SGX and
GFX-ASC never change state**, and no PS / PTD line for the GPU: iOS powers
the GPU off and on at runtime entirely in hardware (Fender + auto-PM), as
seen from the registers before. GFX-BUSY (id 0xd2) is a virtual device:
`no_ps`, no parents, the second entry of sgx's `clock-gates` /
`power-gates` (<0xd0 0xd2> = GFX-SGX, GFX-BUSY), so AGX switches it with
GFX-SGX. PMGR writes no register for it; its state goes to the CLPC
device-set listeners (ApplePMGR fn 0xfffffff009196c54 registers up to 4,
`clpc` / `device-set-%d`; notified from set-device-state via fn
0xfffffff0091cdfa4) -- a "GPU busy" performance hint, not power.
m1n1's `pmgr_adt_power_enable` does nothing for it, and nothing needs it.
What the log cannot show at runtime is AGX's start at boot (GFX-SGX
run-max, PS config, any reset): the field must be set before ApplePMGR
starts, i.e. the boot-arg with `debug-enabled` -- iBoot sets
`/chosen/debug-enabled` (string present in the 20E247 iBSS), so that needs
an iBoot patch in surrealra1n's `iBSS.boot`.

The patch (`devices/a13/tools/ios/debug-ibss.py IN OUT [ARGS]`, output
`devices/a13/build/gpu/ibss-debug/iBSS.boot`): in iBSS.boot (base 0x19c050000) the
/chosen properties are filled one after another, each `if (prop found &&
security_allow_modes(mask)) *prop = 1` (fn 0x590e8, store fn 0x2c188);
debug-enabled uses mask 0x20 and the `cbz w0` at file offset **0x29bf4**
skips the store; a nop there forces debug-enabled = 1. Boot-args go into
iBoot64Patcher2's cave at 0xaf020: `-v serial=3 pmgr-debug=0x880e`. XNU
has no `msgbuf` boot-arg (only the `kern.msgbuf` sysctl), so the boot
log is read live: on the `-v` screen and the DCSD serial port
(`serial-log.sh`). After boot `pmgr-log.sh off` clears the field.

**The boot log** (SerialMonster over DCSD, `devices/a13/build/gpu/pmgr-log/
serial-boot-pmgr-debug.txt`, 17.7k lines, 16k from ApplePMGR; iBoot's own
lines are hashed `file-hash:line` and unreadable): `debug-enabled` and the
boot-args took (launchd: `boot-args = -v serial=3 pmgr-debug=0x880e`).
What iOS does with the GPU's power, in order:

1. ApplePMGR start writes every PS register; **GFX_PS (0x80390) gets
   MANUAL_PS=0, AUTO_PM_EN=FALSE**, and unlike ANE_SYS / AVD_SYS / VENC_SYS
   / ISP_SYS (`No change to PS`) it was not off yet: iOS switches off the
   GFX iBoot left on, and keeps it off for the seconds until AGX starts.
2. AGX start: `GFX-SGX: run-max` -> `GFX: run-max` -> `GFX-SGX:
   wasDisabled=false` -> `GFX-SGX: power-off` -> `GFX: power-off`, then
   `GFX-ASC: run-max` -> `GFX: run-max` -> `RTBuddy(GFX): Resuming` (the
   firmware boots). No reset, no PWR_GFX_CFG0, no PTD line. Afterwards
   GFX-ASC alone holds GFX on; GFX-SGX stays off.
3. No GFX_PS line for the run-max steps, while SPI3 / I2C log one per
   switch. The devices that switch silently are exactly the ones with
   `notify_pmp` and a PMP domain (ADT `unk3`: VENC 2, AVD 3, ANE 4, ISP 5,
   GFX 7; `unk2_3` = 1). For them set-device-state calls fn
   0xfffffff0091adcd4 with 0xe, or 0xf when ADT flag `b7` is set (GFX,
   ANE); ApplePMPControlFunction (+0x80 = fn 0xfffffff0091e20f8, action fn
   0xfffffff0091df2f4, jump table 0xfffffff0091df858) turns these into PMP
   messages 0xe and **0x8** (arg1 device id, arg2 on/off; for GFX on:
   `0x0020800000810001`). In the PMP (fn 0x100c004) 0x8 and 0xe share one
   handler (0x8 also replies 0x9): type-4 devices (GPU) ignore "on", and
   "off" acts only on a change of the PMP's own device state (fn 0x100e428,
   state byte in the 0x58-byte slot at *0x1045e90), which "on" never set.
   So the PMP is told but does nothing for the GPU, as before.

So iOS's kernel has no hidden GPU power step: a full GFX off from the
iBoot hand-off until AGX starts, then GFX on (the PS write for these
devices is not logged), firmware boot. The one difference from our
`--gfx-cycle` (50 ms off) is how long GFX stays off -- seconds here.
`agx-start.py --gfx-off SECS` holds it off that long (AUTO_ENABLE clear,
MANUAL 0, as iOS writes it; no GPU reads while off) before switching it on.

Tried (proxy, `agx-start-30.log`, `--gfx-off 5 --fender-init`): GFX
0xf0000ff -> 0xf000300 (actual 0) for 5 s -> 0xf0001ff; the host's request
then stops at **step 0x10 again** (+0x1000 0x11, +0x1014 2, +0x1018 0x10),
the firmware crashes as before. Fender +0x1401c = 0x64 survives the 5 s
off, so that page is outside GFX's power domain. After the firmware has
run, +0x1401c reads 0 (the firmware clears it and fills the +0x14080
table) and the request still sits at 0x10 (`agx-start-31.log`, same proxy
session, `--pre 0x204d1401c=0` added for writes before the request). **The
length of the GFX off time and +0x1401c are not it.**

(AGXG12P's `AGXFender::initConfigRAMOffset` / `incrementConfigRAMOffset` /
`terminateConfigRAMSequence` / `writeConfigRAMCommand32/64` looked like a
power-sequence program the kernel loads into Fender; in the G12 vtable
(0xfffffff0079b9240, slots 21-25) they are the base stubs that panic
(`agxk_fender.h`), so G12 does not use them.)

**Bisect: iOS's kernel by our path.** The same iBSS that boots iOS with a
working GPU (surrealra1n's iBSS.boot) is ours plus three patches (boot-args
adr at 0x2cc54, "-v" at 0xaf020, local boot at 0x30704); ours is byte for
byte surrealra1n's `iBSS.patch`. So booting iOS 16.4's own kernel through
our Recovery flow -- same iBSS, stock DeviceTree, gfx and pmp firmware,
`bootx` -- with only the trust cache (the root volume's,
`078-23869-318.dmg.trustcache`, as rtsc) and the payload (the stock
kernelcache, rkrn, LZFSE) changed, splits the question: GPU up under iOS
=> the difference is in what XNU does before AGX outside the PMGR; GPU
stuck => it is in our boot path (iBoot state, inputs), bisectable input
by input. AGX starts long before launchd (boot log: line 946 vs 2651), so
the answer is in the serial log even if userland does not come up (no SEP
firmware is sent; surrealra1n's old A12/A13 flow did not send one either).
Files: `devices/a13/build/gpu/ios-recovery/{kernelcache.rkrn.img4,rootfs.rtsc.img4}`,
`devices/a13/build/gpu/ibss-debug/iBSS.recovery.raw` (`debug-ibss.py` on
`inputs/iBSS...sigpatched.raw`: + debug-enabled, boot-args
`-v serial=3 pmgr-debug=0x880e`). Run:
`IBSS=$PWD/a13/build/gpu/ibss-debug/iBSS.recovery.raw ./cascadia a13 boot ios`.

First tries: `ipsw img4 create` re-encodes the LZFSE kernelcache and iBoot
refuses it ("Kernelcache image not valid"); `devices/a13/tools/ios/retag-img4.py`
keeps Apple's IM4P byte for byte and changes only the type (krnl -> rkrn,
trst -> rtsc). Loading the gfx firmware twice in one iBoot session drops
the USB link (start over from DFU). Then the kernel boots and panics at
root mount, just before AGX would start (`bisectlogios`, the BUCK0
energy-counter line, which in the normal boot comes right before
`GFX-SGX: run-max`): "Failed to find the root snapshot ... Rooting from
the live fs of a sealed volume is not allowed on a RELEASE build". The
root snapshot is `com.apple.os.update-<boot-manifest-hash>`, and iBoot
derives that hash from the IM4M; ours is a generic one. The phone's own
ticket is `/private/preboot/<hash>/System/Library/Caches/apticket.der`
(not kept by surrealra1n, which restored from a custom.ipsw). Fetched to
`devices/a13/build/gpu/ios-recovery/apticket.der`: SHA-384 = B1B3E492...2DD949, the
Preboot directory and the mounted snapshot's name. The ios mode now wraps
the kernel, the trust cache and the DeviceTree (same payload, sha256
62b7e44e...) in it; APFS falls back to validating the root hash on disk
(`validate_on_disk_root_hash`) when iBoot forwards none.

With the apticket the snapshot is found; the next panic (`panic` log) is
"could not authenticate personalized root hash! (0), (0, 0)": iBoot
forwards root_hash.img4 only in a local boot. APFS
`is_root_hash_authentication_required_ios` (fn 0xfffffff00a13cf80) skips
it ("will be skipped") when the boot-args have **`arp0`** (and an internal
flag bit 3 at 0xfffffff00a6d6b78 is clear); the debug Recovery iBSS now
carries `-v serial=3 pmgr-debug=0x880e arp0`.

Third try (`logg`): `arp0` does not skip it (same panic, `Boot args: -v
serial=3 pmgr-debug=0x880e arp0`; the volume is not role 4 or the flag is
set). But this time **AGX started before root mount**: `GFX-ASC: run-max`
-> `GFX: run-max` -> `RTBuddy(GFX): Resuming` -> `GFX-SGX: run-max` ->
`wasDisabled=false` -> `GFX-SGX: power-off`, then ~230 lines (IOMFB, ANE,
ISP, PCIe; >= 0.45 s by the Bluetooth timestamps) with no GPU error, no
RTBuddy(GFX) crash, until the root-hash panic. Under m1n1 the same
firmware takes its SError within about a second of initdata. Suggestive
(the GPU firmware ran on our path without crashing), not conclusive: the
kernel died too early to show the GPU doing work. Next, if pursued: patch
APFS's panic (fn 0xfffffff00a13cf80's caller, the `cmp w8, #1; b.ne` at
0xfffffff00a13d2a4) in the kernelcache and repack it the proven way (LZSS
+ `repair_lzss_img4.py`), so iOS reaches SpringBoard.

Done that way (2026-11-03): in `is_root_hash_authentication_required_ios`
the "required" answer `mov w20, #1` (VA 0xfffffff00a13d09c, raw file
offset 0x313909c) -> `mov w20, #0`, as on its "will be skipped" branch;
`kernelcache.patched.rkrn.img4` (LZSS + repair, 30 MB, apticket). SEP: the
ticket's `sepi` / `rsep` digests do not match the 16.4 IPSW's SEP
firmware (surrealra1n installed another), so the phone's own
`/usr/standalone/firmware/sep-firmware.img4` is sent unchanged with
`rsepfirmware` (SEP's ROM checks it, not iBoot); it goes into
`ios-recovery/firmware/` with the rest of that folder. Copied
(2026-11-03): its IM4M is apticket.der. The local boot's GPU and PMP
firmware (`FUD/GFX.img4`, `FUD/PMP.img4`) carry the same payloads as our
`inputs/gfx.gfxf.img4` / `pmp.pmpf.img4` (sha256 3961319..., d7d52c4...).

Fourth try (`panic2`, patched kernel + the phone's SEP): root mounts, the
kernel starts launchd, which dies: "initproc failed to start ... Library
not loaded: /usr/lib/libSystem.B.dylib" -- iOS 16's dyld cache is in the
OS cryptex, grafted in a local boot from what iBoot passes (also missing:
`system-volume-auth-blob` in the DeviceTree). On the GPU side: `GFX-ASC:
run-max`, `GFX-SGX: run-max`, `RTBuddy(GFX): Resuming`, `GFX-SGX:
power-off`, then ~290 lines to the launchd panic with no GPU error and
no firmware crash. Under m1n1 the same firmware (same payload as the
local boot's FUD/GFX.img4) crashes within 1 ms of initdata (`agx-start-30`:
Sending initdata 0.705 s, DC_Init 0.706 s, Crashed!). Neither boot prints
an AGX "firmware up" line, so this assumes iOS's AGX sends its initdata in
start() (it does its firmware init there), but on that assumption **the
GPU powers up on our boot path under XNU**: iBoot's hand-off, the inputs
and the firmware are fine, and the difference is between the hand-off and
the firmware start -- what XNU does before AGX vs what m1n1 + the
proxyclient do (or do not do).

**Audit of our side (2026-11-03).** What runs between iBoot's hand-off and
`--fender-init` (whose host-only request already stops at step 0x10):
m1n1 `m1n1_main`: `mcc_init` (no `mcc,t8030` match: nothing), `mmu_init`,
`aic_init`, `wdt_disable`, `pmgr_init` (only switches on parents of
already active devices), `display_init` / `fb_init` (iBoot framebuffer),
`cpufreq_fixup` (no T8030 case: nothing), `sep_init`; chickens are CPU
system registers. proxyclient `m1n1.setup`: `PMU(u).reset_panic_counter()`
writes the ADT's `info-leg__scrpad` + 2 (a scratch byte). `agx-start.py`
before the request: `pmgr_adt_power_enable` of gfx-asc and sgx (GFX on;
GFX-BUSY is virtual), reads; everything else opt-in. **Nothing on our side
touches the GPU's power path before the request.** So the difference is
something XNU (or a coprocessor it resumes: PMP, SMC, AOP -- `RTBuddy(...):
Resuming` lines) does before AGX, or state that only exists while the GPU
is on (our iOS references are mostly GPU-idle: e.g. the PMU bucks were
read with the GPU off, and which buck feeds the GPU is still unknown).
Also found: ApplePMGR `_pmPerformanceBoost` (fn 0xfffffff009198d20, log
bit 10) sets per-level gates (DCS/CPM/IMX/SMX/RMX/AMCC/AFI/AFNC) through
`_setPSAutoMinLevel` (fn 0xfffffff0091a59ec: PS register bits 19:16 =
auto minimum level); not GPU-specific.

**Rails: never looked at while the GPU is on, and two pieces never looked
at all.** (1) The SPMI bus has a second device next to the PMU (`avus`,
slave 0xf, `has-fw 1`): **`dotara`, slave 0xe**, with no kext naming it --
so if it carries SoC rails, the hardware drives it. (2) The PMGR has a
hardware voltage manager, **VOLMAN at 0x23b0a0000** (AppleT8030PMGR:
"DVC_DONE_ERR :PMGR_VOLMAN_STATUS" = +0xa0020, fn 0xfffffff00946d804; fn
0xfffffff009463e6c, sleep/wake, clears / sets bit 0 of +0x4..+0x1c, seven
channels, and writes +0x24..+0x3c with 0x80000000 / 0xe70000 / 0x20), not
in `pmgr-regions.txt` until now. If Fender's step 0x10 waits for the GPU
rail, the request goes through VOLMAN to a PMIC. `devices/a13/tools/ios/
rails-gpu.sh` samples Fender +0x1000, VOLMAN +0x0..+0x3c and one PMU block
per 0.3 s on iOS, screen off then on; `--dump-regions` now includes VOLMAN.

**Result (iOS, `pmgr-diff/ios-rails-gpu-005902.txt`, 300 samples): the
GPU's rail is PMU buck 0x4100, driven by VOLMAN channel +0x38.** VOLMAN on
iOS: +0x0 0x01000000, +0x4..+0x14 2, +0x18/+0x1c/+0x20 0, +0x24
0x0851b092, and six channel words +0x28..+0x3c (bit 24 on, bit 31 a
transition in flight, three voltage-code bytes). +0x30 follows buck 0x4000
(codes 0x44/0x30/0x5e/0x38/0x81), +0x34 buck 0x4b00 (0xb0/0x8f), +0x3c
buck 0x4500 (0x3b/0x43), +0x28 buck 0x4200 (0x3e/0x47): always on.
**+0x38 is code 0 in 91% of samples** and 0x31/0x3a (0x01313a31,
0x81313a3a) only while the screen is in use, and **buck 0x4100 then reads
`09 31` (on, code 0x31)** instead of `00 00`. A rail that is off at idle
and comes on in bursts with activity is the GPU's (iOS powers the GPU off
within ms of idling). **This corrects the earlier "0x4100 is off on iOS
too -- a spare phase"**: that was read with the GPU idle. So Fender's step
0x10 is most likely the wait for the GPU rail, which on iOS VOLMAN raises
(channel +0x38 -> buck 0x4100) and on our boot never comes up. *(Channel
words in this paragraph are right; the first VOLMAN offsets below, +0x24 as
a channel and control +0x18 for +0x38, are off by 4 -- see the correction
further down and the G4 summary.)*
`agx-start.py` now prints VOLMAN +0x0..+0x3c with every Fender readout.

**VOLMAN under m1n1 (`agx-start-32.log`): iBoot leaves it in manual
mode.** Control +0x4..+0x14 = 3 (iOS 2), +0x18/+0x1c = 1 (iOS 0); no
channel has bit 24 (iOS: all); +0x28 0x5a5a5a, +0x2c 0x616161, +0x30
0x3939, +0x34 0x3e3e, +0x38 0, +0x3c 0x4343. **After the host's power-up
request +0x38 = 0x3a3a**: the GPU's request reaches VOLMAN with code 0x3a
(the code iOS uses), but the channel is not in hardware control, so buck
0x4100 never comes up and Fender waits at step 0x10.

XNU's switch (AppleT8030PMGR fn 0xfffffff009463e6c, the w1 != 0 path):
clear bit 0 of the seven controls +0x4..+0x1c, then per channel write the
current voltage code (a byte) and poll bit 31 clear (fn 0xfffffff0091b2398,
0x2ee00 us): +0x30 and +0x2c from a vtable call (+0xa60, CPU clusters),
+0x24 from PMGR +0x58000.., +0x3c from PMGR +0x5c008/+0x60000.., **+0x38
(GPU) from the GPU's perf table at PMGR +0x64000** (entry +8 + 8 *
(PMGR+0x64000 & 7), code = bits 31:24), +0x28 from +0x5c008/+0x5c000.
Control +0x4 + 4i pairs with channel +0x24 + 4i (GPU: +0x18 / +0x38).

`--volman-gpu` does it for the GPU's pair only (+0x18 bit 0 cleared,
+0x38 = 0, the idle code) and prints PMU buck 0x4100. First try on an
already stalled session (`agx-start-33.log`): the writes take (+0x18 0,
+0x38 0, still no bit 24), but the sequencer had sent its code before and
does not repeat it -- needs a fresh boot, VOLMAN before the request.

Fresh boot (`agx-start-34.log`, `--volman-gpu --fender-init`): +0x18 0,
+0x38 0 before; the request writes +0x38 = 0x3a3a again, **no bit 24,
buck 0x4100 stays `00 00`, Fender stays at 0x10**. Writing the code
ourselves (`agx-start-35.log`, `--pre 0x23b0a0038=0x3a`): the channel
takes it (0x3a3a) and nothing reaches the PMU. On iOS every channel has
bit 24, under m1n1 none (not even the CPU channels), so VOLMAN as a whole
is not getting its writes to the PMU. VOLMAN reaches the PMU as a hardware
master on spmi0: AppleSPMI (20E247) names hardware queues HW_0 / HW_1 and
a BackLight queue next to SW_0..3, and a transmit filter with a "write
disabled" mode (faults "Any transmit filter write when write disabled",
"Any HW Q soft reset when write disabled", "Any SW Q request transmission
when blocked by filter", "TX Filter Violation Interrupt!"). The kext never
configures the filter (iBoot does), and its `queue-reset` (3 written to
0x23d180014, ADT reg 3) resets the SW queues only. spmi0 under m1n1
(`pmgr-diff/proxy-spmi0.txt`): +0x0 0x4444, +0x4 0x733, +0x28 0x203, +0x40
0xf0000, +0x88/+0x8c 0x18, +0x90 1; queue blocks +0x400 and +0x500 the
same (+0x0 0x1000100, +0x60 0x10000000, +0x64 0x190000, +0x68 0x24: HW_0,
HW_1?), +0x600 another (+0x20 0xffff0f01, +0x24 0x400103ff, +0x28 0x26,
+0x3c 0x80000000, +0x64 0x180000, +0x7c 0x2940000); +0xd00 0x40000
0xc0800000; fault counters +0xe50/+0xe54/+0xe58/+0xe60 = 1. Next: the
same windows on iOS.

iOS (`pmgr-diff/ios-spmi0.txt`): globals, +0xd00 (0x01000000) and the
fault counters (+0xe60 = 1) are the same as m1n1's first read -- the
+0xd00 and extra counters above came from that wide read of +0x400..+0x6ff
itself, so on iOS only single config words were read. **The hardware
queues differ**: +0x460 / +0x560 **0x11000000** (m1n1 0x10000000: bit 24),
+0x464 / +0x564 **0x00190400** (m1n1 0x00190000: bit 10), BackLight
+0x664 0x00180400 (m1n1 0x00180000), +0x67c 0x0bb70000 (m1n1 0x02940000, a
counter it seems). The spmi0 block is also **PMGR reg 21** (0x23d180000,
0x2000), outside the ADT windows AppleSPMI maps -- so it is the PMGR side
(or iBoot) that enables the HW queues VOLMAN writes the PMU through, and
under m1n1 nobody does. `agx-start.py --spmi-hwq` sets +0x64 bit 10 (and
+0x60 bit 24 if it does not follow) on HW_0 / HW_1 before `--volman-gpu`.

Tried (`agx-start-36.log`): **these are not plain registers** -- setting
+0x464 bit 10 made it read 0, setting +0x460 bit 24 zeroed +0x460 too
(same for HW_1); the GPU request still stops at 0x10, +0x38 0x3a3a, buck
0x4100 `00 00`. So iOS's bit 24 / bit 10 (and the +0x67c count) are most
likely traces of use -- queue pointers / counters after real VOLMAN
traffic -- not enables; the option is removed again (do not write the HW
queue words). What makes VOLMAN emit at all is still XNU's switch
(fn 0xfffffff009463e6c) for all seven channels, which also moves the CPU
and SoC rails to hardware control: the codes it writes come from the
current perf states (two of them, +0x30 / +0x2c, through a vtable call),
and a wrong one is an undervolt.

**Correction: the channel offsets above are 4 short.** XNU's base is
w23 = VOLMAN + 4 (`mov w23, #4; movk w23, #0xa, lsl #16`), so the channels
it writes are +0x28..+0x40 and **control +0x4 + 4i pairs with channel
+0x28 + 4i**. Sources (`devices/a13/tools/gpu/volman-codes.py`, read-only):
+0x34 cluster 0 (s = [0x210e20020] & 0xf, code = [0x210e70000 + 0x20 s] >>
56; vtable +0xa60 = fn 0xfffffff009466b4c maps cluster-relative addresses
onto pmgr regs 3-11 / 12-18), +0x30 cluster 1 (0x211e20020 / 0x211e70000),
+0x28 ([PMGR+0x58018 + 20 v] >> 8, v = [PMGR+0x58000] & 0xf), +0x40
([PMGR+0x60008 + 8 v] >> 24), +0x3c **GPU perf** ([PMGR+0x64008 + 8 v] >>
24, v = [PMGR+0x64000] & 7), +0x2c ([PMGR+0x5c008 + 8 v] >> 24). **+0x38 is
never written by XNU**: it is the channel the GPU's power request drives.
Under m1n1 (`pmgr-diff/proxy-volman-codes.txt`) every code XNU would write
**equals what the channel already holds** (+0x28 0x5a, +0x2c 0x61, +0x30
0x39, +0x34 0x3e, +0x3c 0x43, +0x40 0x34; states 3/5/2/2/2/2): iBoot
programs the targets, XNU's rewrite changes nothing, and the real step is
clearing the manual bits. So the GPU's pair is **control +0x14 / channel
+0x38** (iOS +0x14 = 2, ours 3) -- `--volman-gpu` had cleared +0x18 (the
GPU-perf channel +0x3c's), the wrong one, and is fixed to clear +0x14
only. On the spoiled session (HW queues zeroed, request already sent)
clearing +0x14 changed nothing (`volman14-1.log`); a fresh boot is next.

**SOLVED (fresh boot, `agx-start-37.log`, `--volman-gpu --fender-init`).**
VOLMAN +0x14 3 -> 2, nothing else touched. The host's own power-up request
then completes: **Fender +0x1000 = 0x1, +0x1018 = 0x5** (iOS's sequence),
**VOLMAN +0x38 = 0x013a3a3a** (bit 24 on, as on iOS), **PMU buck 0x4100 =
`09 3a`** (on, code 0x3a, as on iOS). The firmware then takes initdata,
DC_Init and DC_UpdateIdleTS, the DevCtrl read pointer moves to 2, and it
runs for the whole 5 s with no crash (before: SError at FAR 0x204010190
within 1 ms of initdata). Bits 24 / 10 in the spmi0 HW queues were indeed
just traces of VOLMAN traffic.

What it was, end to end: Fender's step 0x10 waits for the GPU's power
rail. The rail (PMU buck 0x4100) is driven by the PMGR voltage manager
VOLMAN, channel +0x38, through spmi0's hardware queues. iBoot leaves all of
VOLMAN's channels in manual mode (control bit 0); XNU's AppleT8030PMGR
(fn 0xfffffff009463e6c) clears those bits at boot. With the GPU channel
manual the request reaches VOLMAN (+0x38 = 0x3a3a) and goes no further.
Everything compared at GPU idle matched iOS because at idle the rail is
off on iOS too; the bisect (iOS's kernel on our boot path powering the
GPU) and sampling the rails while the GPU was busy found it.
For Linux: the same bit has to be cleared before the GPU is used (m1n1
or the kernel's PMGR / GPU driver).

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
