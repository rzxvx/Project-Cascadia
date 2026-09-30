# P105 GPU (SGX543MP2) — powered, clocked, microkernel running, first transfer done (2026-09-30)

The GPU is off when Linux starts. As of 2026-09-30 it is switched on from Linux
and **every register bank of the SGX543MP2 answers**: the master and both cores
report CORE_ID `0x01194201` and revision 1.2.2. The same GPU family sits in the
iPad 2 (SGX543MP2, same A5), the iPad 3 (SGX543MP4) and the iPad 4 (SGX554MP4),
so much of this may carry over.

Probe: `tools/sgx-probe.sh` (`SGX_INIT=1 SGX_READ=2` runs the whole sequence
below and reads every bank; without `SGX_READ` it touches no SGX register).

## STATUS 2026-09-30: the SGX answers — and the "clock dead end" never existed

Everything in the 2026-09-27..30 sections below that says the GPU "has no
clock" is **wrong**. The clock was running all along. Every freeze came from
reading a register bank that cannot be read, or cannot be read yet.

### The bring-up recipe (verified on the device)

1. **Power**: switch the GFX_SYS and GFX power domains on — `0x3f101024`, then
   `0x3f101028`, each `(v & ~0x10f) | 0xf`, wait for bits 7:4 = `0xf`
   (`0x300 -> 0x3ff`). Nothing else: no clock register is written, and
   HPERF-NRT (`0x3f10102c`) is **not** needed (tested with it off).
2. **Master bank** (`0x35104000`) now answers: CORE_ID `0x01194201` (ID
   `0x0119`, cores field 2, multi-core), CORE_REVISION `0x00010202` = 1.2.2 —
   exactly the revision the ADT's `brn_31195` predicted.
3. **iOS's own init** (IMGSGX543.kext, `SGXDriver543::initSGX` at kernelcache
   8.4.1 `0x80bf3918`, helpers `0x80bf3700` / `0x80bf3754`), clock mode 1 =
   forced on (iOS uses 2 = auto when auto clock gating is enabled):

   | reg | value | what |
   |---|---|---|
   | `0x4000` MASTER_CORE | `1` (cores - 1) | reads 3 after power-on |
   | `0x4020` MASTER_CLKGATECTL2 | `0x155` | master modules' clocks |
   | `0x4004` MASTER_CLKGATECTL | `0x5` | **each slave core's clock, 2 bits per core — without it the core banks hang** |
   | `0x4080` MASTER_SOFT_RESET | `0x5f3` | BIF, IPF, DPM, VDM, SLC, PTLA, cores 0-1 |
   | `0x4d00` MASTER_SLC_CTRL | `0x44c000` | |
   | `0x4d04` MASTER_SLC_CTRL_BYPASS | `0x4001e40` | `0x1e40` = the brn_31195 bypass bits |
   | `0x4080` | `0` | release |
   | bank 0 `+0x0` CLKGATECTL | `0x10155555` | broadcast to every core |
   | bank 0 `+0x4` CLKGATECTL2 | `0x05454555` | broadcast |
   | bank 0 `+0x310` | `1` | broadcast |

   After it: MASTER_CLKGATESTATUS `0x4008` `0xc -> 0xf`, MASTER_CLKGATESTATUS2
   `0x4024` `0 -> 0x1f`.
4. **Core banks** (`0x35108000`, `0x3510c000`) answer: CLKGATECTL `0x10055555`
   (the BIF_CORE field, bits 21:20, reads back 0), CLKGATESTATUS `0x00fbbfff`,
   CORE_ID `0x01194201`, CORE_REVISION `0x00010202`, SOFT_RESET `0`.

### The register banks (DDK `sgx_mkif_km.h`, confirmed by iOS)

16 KB banks, from the MIT/GPLv2 DDK (TI omap5-sgx-ddk-linux,
`eurasia_km/services4/include/sgx_mkif_km.h`): `SGX_REG_BANK_MASTER_INDEX` 1,
`SGX_REG_BANK_BASE_INDEX` 2. iOS addresses core n at `0x8c00 + (n << 14)`
(`waitForMemoryRequests`, `0x80bf4854`), the same layout.

| bank | offset | contents | read |
|---|---|---|---|
| 0 | `0x0000` | **broadcast**: the DDK and iOS write CLKGATECTL/BIF registers here at bare offsets to reach every core | **hangs the bus — never read** |
| 1 | `0x4000` | master | works once GFX is powered |
| 2 | `0x8000` | core 0 | hangs until MASTER_CLKGATECTL gives the core a clock |
| 3 | `0xc000` | core 1 | same |
| 4-5 | `0x10000+` | nothing on an MP2 (the ADT window is sized for an MP4) | not tried — do not |

Master registers besides the DDK's (`sgxmpdefs.h`), named by iOS's register
dump (`0x80bf9230`): `0x4004` CLKGATECTL, `0x4008` CLKGATESTATUS, `0x4020`
CLKGATECTL2, `0x4024` CLKGATESTATUS2. MASTER_CORE's "+ 1" in the DDK is right
(1 -> 2 cores); the 3 found after power-on is the reset default for four.

### What was wrong in the earlier analysis

- **Every freeze was a bank-0 read** (`0x35100000` on 09-27, `0x35100020` on
  09-28, `0x35100000` again on 09-30), plus one core-bank read before step 3.
  None of them was a clock problem.
- **The GFX clock tree was decoded off by one.** In iBEC's frequency walker the
  array is indexed with OSC = 0 and the PLLs at 1..6 (`0x00, 0x08, 0x10, 0x18,
  0x20, 0x28`), so parents `[0,3,4,5]` are OSC, PLL@0x10, **PLL@0x18 (513 MHz,
  running)**, PLL@0x20. The GFX parents `0x48/0x4c/0x50` select PLL@0x18: GFX-CLK
  ≈ 513/5 = 102.6 MHz, GFX_SYS ≈ 513/2/2 = 128 MHz, in perf state 2. The
  section "The GFX clock tree, fully decoded" below says PLL@0x20 (off) — that
  is the error.
- **`0x70`/`0x74` are perf-state outputs, not locked registers.** The walker
  replaces the low bits of the clocks at walker indices `0xc..0x19` (regs
  `0x3c, 0x40, 0x64, 0x68, 0x6c, 0x70, 0x74`) with the PMGR perf-table row
  `0x220` (state 2, the value in PMGR `+0x300`), and the live registers match it.
  That is why a write to `0x70` "did nothing". The perf table at `0x200` is not
  CPU-only: GFX-CLK's divider is byte 3 of each row's first word.
- **`0x2c` is not MANAGED0**: it is PLL5's second register (every PLL has
  `0x00010960` at +4). `scripts/adt-pmgr-map.py` labels MANAGED*/PREDIV* with
  wrong addresses, and the "top nibble is locked by hardware" test poked it.
- So the tfp0 / kdebug / Route B / iBEC-payload work below was aimed at a
  problem that did not exist. The tools and facts stay useful (tfp0, kdebug,
  the kernelcache and iBEC tooling, domain 0x50), but none of it is needed to
  run the GPU.

### In the kernel (2026-09-30)

`drivers/misc/apple-sgx.c` (`CONFIG_APPLE_SGX`, DT node `sgx: gpu@5100000`
with the two power states as clocks) does all of the above at boot, and the
first boot with it came up clean:

    apple-sgx 35100000.gpu: SGX543MP2 rev 1.2.2, 2 core(s) up, clocks on

`/sys/kernel/debug/apple-sgx/regs` shows the master and both cores with the
values in the tables above; `clk_summary` shows `gfx-sys-ps` and `gfx-ps`
enabled once, by `gpu@5100000`. Every access goes through one function that
refuses bank-0 reads and core banks the SGX does not have. `MASTER_BIF_CTRL`
is still `0x000e0000` (MMU bypass for VDM/IPF/DPM): nothing sets up the BIF
yet. `apple_sgx.clock_mode=2` selects iOS's automatic clock gating. The
interrupt (49) is in the DT but not requested.

### The microkernel runs (2026-09-30)

`echo 1 > /sys/kernel/debug/apple-sgx/boot` loads iOS's microkernel the way
`initSGX` does -- page tables, 22 buffers, 16 LIMM patches, the PDS
programs, the registers -- and kicks core 0. First try:

    apple-sgx 35100000.gpu: starting the microkernel: code at GPU 0x80000000, page directory 0x9b047000
    apple-sgx 35100000.gpu: microkernel is up: host[0] 0x00000001

15 ms from kick to answer. Only the GPU can have set that bit, through our
page tables, at an address it can only have found by following the patched
pointers, so the MMU format, the patches and the boot path are all right.
Afterwards (sgx544defs.h names):

| register | value | meaning |
|---|---|---|
| core 0 `EVENT_STATUS` | `0x20002a00` | TIMER, TA_FINISHED, TPC_CLEAR, DPM_CONTROL_CLEAR |
| core 1 `EVENT_STATUS` | `0x00002a00` | TA_FINISHED, TPC_CLEAR, DPM_CONTROL_CLEAR -- core 1 ran its boot too |
| both `EVENT_STATUS2` | `0x20` | TE_RGNHDR_INIT_COMPLETE |
| master `EVENT_STATUS` | `0x20000000` | (timer) |
| all `BIF_FAULT` | `0` | no MMU fault |
| all `BIF_CTRL` | `0` | MMU on for every requestor |
| all `BIF_INT_STAT` | `0x00080000` | FLUSH_COMPLETE -- set before the start as well; not a fault |

The microkernel initialises the tiling and parameter-management hardware on
both cores and arms its timer. The GPU buffers are at `0x80000000`
(`0x80000000-0x8013ffff`), from `dmam_alloc_coherent`; the parameters iOS
takes from the ADT (DVFS, timing) are still zero and did not stop it. The
microkernel comes from the user's IPSW: `scripts/extract-sgx-firmware.py`,
run by `./cascadia firmware`, into `/lib/firmware/apple/sgx543.fw`.

### Commands: the DDK's kernel CCB, unchanged (2026-09-30, static)

Apple's host interface to the microkernel is the DDK's SGXMKIF
(`sgx_mkif_km.h`, `sgxutils.c`, `sgxpower.c`) as it stands, buffer for
buffer:

| iOS (`SGXDriver543`) | DDK |
|---|---|
| `this+0x750`, `0x2000` bytes | kernel CCB: 256 × `SGXMKIF_COMMAND` {`ServiceAddress`, `CacheControl`, `Data[6]`}, 32 bytes each |
| `this+0x758`, 8 bytes | `PVRSRV_SGX_CCB_CTL` {`WriteOffset`, `ReadOffset`} |
| `this+0x760` (-> `EVENT_KICKER`) | the kernel CCB event kicker (a count, `& 0xff`) |
| `this+0x748`, `0xec` bytes | `SGXMKIF_HOST_CTL`, built with `SUPPORT_HW_RECOVERY`, without `FIX_HW_BRN_28889` |
| `this+0x72c..0x73c` | `aui32HostKickAddr[]`: the handler for each command type |

**`SGXScheduleCCBCommand` is `0x80bfb104`** `(this, service, cache_control,
data0, data1)`: wait while `(WriteOffset + 1) & 0xff == ReadOffset`
("SGX Hang - Poll Timeout"), bump `WriteOffset`, write the 32-byte command
into the old slot (`Data[2..5]` = 0), barrier, and if powered
(`this+0x878 == 3`) bump the kicker and write 1 to core 0 `EVENT_KICK`
(`0x8ac8`). `0x80bfafe8` re-kicks once per pending command after a power-up.

**The handlers** are microkernel offsets, `ServiceAddress = (UK_START + off)
>> 3` in the code buffer's 8-byte units (`this+0x6d0` is the offset `0x1000`,
not an address; computed at `0x80bfa448`). Which is which follows from the
callers:

| slot | ukernel off | command | sent by |
|---|---|---|---|
| `0x72c` | `0x1fa8` | TA (render) | slot 38 of vtable `0x80c02798`, `Data[1]` = a context buffer's GPU address |
| `0x730` | `0x1bf0` | **POWER** | `deinitSGX` (`0x80bf4048`): `Data[1] = 1` (`PVRSRV_POWERCMD_POWEROFF`), then waits for `PowerStatus` bit 3 -- exactly the DDK's `SGXPrePowerState` |
| `0x734` | `0x1e80` | TRANSFER | slot 38 of vtable `0x80c02898` (strings: `validateRenderCommand` / `validateTransferCommand`) |
| `0x738` | `0x21d0` | ? (one `BR`) | `initSGX` after a hardware recovery (type 2), once per context at `this+0x628/0x634/0x640`, cache control 6 |
| `0x73c` | `0x21d8` | SETHWPERFSTATUS | `0x80bfb1f0`: copies the counter selectors to host `+0x3c`/`+0x5c`, `Data[0]` = status (0 or 3) |

TA and TRANSFER open with the same instructions (both walk a queue).

**Host control words** confirmed by iOS's accesses: `+0x00` InitStatus,
`+0x04` PowerStatus (`POWMAN_*`: 4 idle, 8 power-off complete, 0x20 no work),
`+0x0c` uKernelDetectedLockups (iOS adds it to a counter and zeroes it),
`+0x14` HWRecoverySampleRate (`this+0xccc`), `+0x18` uKernelTimerClock
(`this+0xcc4`), `+0x1c` ActivePowManSampleRate (`this+0x87c`), `+0x20`
InterruptFlags, `+0x24` InterruptClearFlags ("sgx ukernel didn't clear HWR
state"), `+0x34` HostClock (ms), `+0x38` AssertFail, `+0x3c`/`+0x5c` the
perf-counter group/bit selectors (8 each). The driver leaves the rates and
the timer at 0.

**The interrupt**: `EVENT_HOST_ENABLE = 0x4000` is SW_EVENT, the
microkernel's signal to the host. iOS's handler (`0x80bf2168`) reads
`EVENT_STATUS` (`0x12c`) and `EVENT_STATUS2` (`0x118`) **in bank 0**, masks
them with its enables, writes the result `| 0x80000000` (MASTER_INTERRUPT) to
`EVENT_HOST_CLEAR` / `CLEAR2`, handles SW_EVENT in `0x80bf4400`, then looks
at InterruptFlags: bit 0 = "Microkernel detected lockup", bit 1 = the
microkernel asking to be powered down (active power management, then
`deinitSGX`). So bank 0 **is** readable once the SGX is initialised -- every
bank-0 hang happened before the cores had clocks. The driver still reads
the core banks.

In the driver: `apple-sgx/cmd` takes `hwperf N` and `power N` (1 off, 2
idle, 3 resume); `regs` shows the CCB offsets, the kicker and the host
words; `apple_sgx.irq=1` (also writable in `/sys/module`) switches the
interrupt on at the next microkernel boot, with a guard that turns the line
off after 16 interrupts in a row that it cannot clear.

### The microkernel takes commands (2026-09-30, on the device)

**First try: not taken.** The command landed in the CCB and `WriteOffset`
moved, but `ReadOffset` stayed 0 for every command. Snapshots of every
buffer (with `peek` on the physical addresses `regs` prints) before and
after a kick showed that the microkernel wrote **nothing** in response, while
at init it had filled `0x794`, `0x798` and `0x7a0` and set host `+0x30`
(TimeWraps) to 1. So it was alive and was kicked, but saw no work.

**Cause: the page flags.** iOS's allocator (`0x80bf9658`, args `this, &obj,
init, size, opts, align, heap, flag`) sets the memory object's flags from
`flag`, or else by heap: **heap 1 -> 6** (cache-consistent + EDM-protected),
heap 0 -> 1 (read-only). The PTE writer turns them into `CACHECONSISTENT
(0x8) | EDMPROTECT (0x10)` and `READONLY (0x4)`. Every buffer the host and
the microkernel share (the `0x740` block, host control, CCB, CCB control,
kicker, `0x768`, the PDS programs, `0x704`) is heap 1; the code, the second
code buffer and the blob tables are read-only; `0x794..0x7a0` (heap 2/6,
flag 0) get nothing. Our PTEs had only VALID, so the microkernel read the CCB
control once at init, kept `WriteOffset = 0` in its data cache and never
looked again.

Setting the flags by hand in the live page table (`peek w` on the PTEs,
then a new `boot`) fixed it at once:

    command HWPERF (cc 0x0, data 0x0 0x0): taken after 40 us, CCB write 1 read 1
    command POWER (cc 0x0, data 0x0 0x1): taken after 44 us, CCB write 2 read 2
    power status 0x0000002c: powered off

A run of seven (`hwperf` x3, `power 2` -> status `0x4` idle, `power 3`
resume, `hwperf`, `power 1` -> `0x2c` = IDLE | POWEROFF | NO_WORK) went
through at 33-44 us each, then again with iOS's full flag set including the
read-only code. The driver now carries the flags per buffer
(`sgx_buf_descs[].pte`). No interrupt was raised for any of these commands
(0 handled, 0 unhandled): the microkernel does not signal SW_EVENT for them.

### The first job: a transfer through a transfer queue (2026-09-30, on the device)

**The queue, from iOS.** `IMGSGXTQChannel` allocates three heap-1 buffers
(`0x80bfcf06..0x80bfcfa0`): a hardware context of `0x24` bytes, its own CCB
of 64 KiB and that CCB's control words (8 bytes). The context (filled at
`0x80bfd074`) is `{1, channel number (this+0x34), page directory physical
address, CCB GPU address, CCB control GPU address, 0, 0, 0, 0}`. A submit
(`0x80bfd150` -> builder `0x80bfd19c`) puts a **0x140-byte** command at the
CCB's write offset -- in bytes, 16 bits wide; a command that would not fit
before the end pads to it (`0x80bfd364`) -- and sends TRANSFER through the
kernel CCB with `Data[1]` = the hardware context, as the DDK's
`SGXSubmitTransferKM` does.

**The command.** GL's part is a vendor payload of type 2, exactly `0x7c`
bytes (`copyAndValidateVendorPayload`, `0x80bf64d4`), copied to the
descriptor at `+0x16c`; `validateTransferCommand` fails unless payload word 1
is **0**. The builder places payload words at `+0x00..+0x43` and (when
payload word 2 is set) `+0x78..+0x9f` -- the transfer's register values --
and adds its own:

| offset | what |
|---|---|
| `+0xa0` | the command size, `0x140` |
| `+0xa4` | payload word 1, always 0 from GL: the flags (DDK `SGXMKIF_TQFLAGS_*`) |
| `+0xa8` | from the descriptor (`+0x168`) |
| `+0xac`, `+0xb0` | {address, value}: **written by the microkernel when the command is done** |
| `+0xb4` | number of dependencies |
| `+0xb8 + 8i` | {address, value}: **wait until `(s32)(*address - value) >= 0`** |
| `+0x104` | payload word 2 |
| `+0x108` | `0x1800000` |

(The addresses iOS puts there point into its sync buffer, `this+0x6c8`.)

**On the device**, before the driver had it, by hand with `peek` in the
spare part of the kicker page (context `+0x100`, CCB control `+0x200`,
scratch `+0x300`, CCB `+0x800`): one command with flags `0x20` (the DDK's
DUMMYTRANSFER), no register words, completion {scratch, `0xcafe0001`}, one
dependency {scratch + 4, `0xcafe0002`}:

    kernel CCB: write 1 read 1        tq CCB: write 0x140 read 0x140
    scratch:    cafe0001 00000000     context word 0: 1 -> 0
    core0 EVENT_STATUS 0x20002a00 -> 0x24002a00 (TCU_INVALCOMPLETE)

**The microkernel ran its first job**: took the transfer from the queue and
wrote our value to our address. Two more with the dependency changed to
{scratch + 4, 1} were taken from the kernel CCB but stalled the queue at
read `0x140` -- `0 - 1 < 0`, the wait was not met (with `0xcafe0002` it had
been, signed). Writing 1 there did nothing until the next kick; after one,
both ran (read `0x3c0`, scratch `0x12345678`, context word 0 back to 0).
So: dependencies are wrap-around compares, and the microkernel re-checks a
waiting queue only when kicked.

In the driver: `echo "tq 0x20 VALUE" > .../apple-sgx/cmd` (a context, its
CCB and a scratch page are allocated with the other buffers; no
dependencies; waits for VALUE in scratch).

### A real transfer, captured: mipmap generation (2026-09-30, iOS)

`gltrace tq` (commit `6a74b91` and after) draws the triangle, then does
`glCopyTexSubImage2D` (-> a **render**, no transfer) and `glGenerateMipmap`
on a 64x64 RGBA texture (-> **six transfers, one per level**). Raw output
and dumps: `logs/ios/tq/` (not in git). The GPU addresses are the same run
after run (`0x90012000`, `0x980ac000`, `0x980f3000` ...).

**The hardware command**: header `{0, 5, 0x94, 0x7c, 0x18, ...}` (word 4 =
payload offset, word 2 = total size), payload of 31 words:

| word | value (level 1 / levels 2-6) | goes to |
|---|---|---|
| 0 | `2` (type) | -- |
| 1 | `0` (flags) | `+0xa4` |
| 2 | `0x90012000` / 0 -- a GPU address; non-zero also copies words 21-30 | `+0x104` |
| 3 | `0x02000600` / `0x02000628`, `..648` ... | `+0x00` |
| 7 | `0x6200`, `0x6400`, `0x6600` ... | `+0x10` |
| 12 | `0x980ac000` / 0 -- not copied by the kernel | -- |
| 13 | `0x980f3000 + 0x1c0 * level` -- the level's state block | `+0x24` |
| 14, 15 | `5`, `0x4000` | `+0x28`, `+0x2c` |
| 18 | `(w12 + 0x100 - 0x80000000) >> 4` / 0 -- a PDS program pointer | `+0x38` |
| 23, 24 | `0x200`, `3` / 0 | `+0x84`, `+0x88` |

**The per-level state blocks** (GPU `0x980f3000`, 0x1c0 each, six; found
in the process's IOKit mappings -- VM tag 21 -- by content, CPU `0x8a3000`):

| offset | what |
|---|---|
| `+0x000`, `+0x008`, `+0x010` | `0x388a`, `0x3a8a`, `0x380a`, each `+0x1b80` per level (code addresses?) |
| `+0x01c..+0x040` | constants (`ffff0000`, `ff`, `ff00`, `0x100`, `0x20000`, `fffeffff`, `0x30000`) |
| `+0x050..+0x0a4` | a PDS program, the same for every level (ends `af000000`) |
| `+0x120..+0x138` | **destination**: `{code, 0xa, 0, 0xf800, 0x001e0090, 0x0c<log2 w><log2 h>, GPU address}` -- level n+1 |
| `+0x140..+0x14c` | a PDS program |
| `+0x180..+0x198` | **source**: the same shape with `0x001e1490` -- level n |
| `+0x1a0..+0x1ac` | a PDS program |

Source and destination are levels of the same texture: GPU `0x98104000`
(CPU `0x8b3000`, 6 pages), levels at `+0x0, +0x4000, +0x5000, +0x5400,
+0x5500, +0x5540, +0x5550` (16K/4K/1K/256/64/16/4 bytes); `0x0c06_0006` =
64x64 (`0x0c` the format). After the six transfers the texture holds the
whole chain (exactly 5461 non-zero words).

GPU buffers the driver allocates are laid out size + one guard page apart
(`0x9809b000` 64K, `0x980ac000` 64K, `0x980bd000` 8K, `0x980c0000` 8K), which
lines up with the IOKit mappings in the process in several places, but the
CPU copy of `w12`'s block is not pinned yet, nor what `0x90012000` and
`0x8c009000` (also in the command's resource list) hold, nor where the
USSE/PDS code the `0x388a`-style words point at lives.

### Replaying the captured transfers under Linux (2026-09-30, on the device)

Tools in the driver: `echo "map VA SIZE" > cmd` (zeroed, cache-consistent
memory at a chosen GPU address; map first, then `boot`, which invalidates
the MMU caches), `apple-sgx/mem` (the file offset is the GPU address --
**write with `dd ... conv=notrunc`**, without it dd tries to truncate the
debugfs file, gets "File too large" and writes nothing), and `echo tqkick >
cmd` (queue the 0x140-byte command put at the transfer CCB's write offset
through `mem`, send TRANSFER, report the completion word and the MMU status
of every bank).

Replayed from the capture, at iOS's GPU addresses: the level blocks
(`0x980f3000`), the texture (`0x98104000`, one level zeroed), `w12`'s block
(`0x980ac000`, only its `+0x100` = `af000000` is used) and `0x90012000`
mapped empty; the kernel command built from the payload as the kernel does,
completion pointed at scratch. Level 2 alone and level 1 (with `w2`/`w12`)
behave the same:

- TRANSFER taken, the context's word 0 cleared, and **`TRIG_3D` set in
  EVENT_STATUS2** (core 0 and 1 `0xa8` = TRIG_3D | TE_RGNHDR_INIT_COMPLETE |
  DCU_INVALCOMPLETE, master `0x8`): the microkernel started the 3D pipe.
- The render never ends: no `PIXELBE_END_RENDER`, transfer read offset stays
  0, no completion write, the texture is unchanged, `0x90012000` untouched.
- No MMU fault anywhere; the microkernel still takes other commands.

So everything the command points at is in place, and what is missing is
state set outside the command -- registers iOS programs when it creates a
transfer context or in `initSGX`, or ADT-derived parameters still zero.

### Next

Done since this list was first written: the page tables, the firmware
extraction step, the microkernel start, the command path, a first
(dummy) transfer and a capture of real ones (above).  Open:

- **A transfer that moves pixels** under Linux: replay one mip-level
  transfer -- everything it references mapped at the same GPU addresses,
  source and destination repointed (the destination can be the
  framebuffer). Still missing: `w2`/`w12` contents and the code the blocks
  point at. Flags 0 with zeroed registers was not tried (it would run an
  unconfigured transfer).
- TA/3D (a triangle): the render queue (`0x80bfc9a4`, a `0x40`-byte context,
  64 KiB CCB) and a whole render command -- the bigger step.
- The interrupt: requested and switched on with `irq=1`, never fired yet.
- The ADT-derived parameters (DVFS, timing) are still zero.
- SGX543 has PTLA (2D hardware, `SGX_FEATURE_2D_HARDWARE`).

State at the end of 2026-09-30: power, clocks, register access, the MMU,
the microkernel start and context-free commands all work from Linux
(`drivers/misc/apple-sgx.c`, `echo 1 > .../apple-sgx/boot`, `echo "power 1"
> .../apple-sgx/cmd`).

## The microkernel, and how iOS boots it (2026-09-30, static)

Everything here is from the decrypted 8.4.1 kernelcache (`kc841.macho`),
kext `com.apple.driver.IMGSGX543` (Mach-O at `0x80bf1000`, `__text`
`0x80bf1500`, class `SGXDriver543`; register base pointer at `this+0x5bc`).
The kext's code loads strings as `ldr rX, [pc, #n]` + `add rX, pc`.

**The microkernel is inside the kext**, in `__DATA,__data` (VA `0x80c02a20`,
`0x16094` bytes). `tools/iosgpu/usse-dis.py scan` finds a run of 3627 valid
USSE instructions from `+0x60`. It is Apple's/IMG's firmware: never commit
it; a build step has to take it out of the user's own IPSW, as with
`P105.mtprops`.

| blob offset | size | what | where iOS puts it |
|---|---|---|---|
| `+0x00000` | `0x60` | header (`1, 0.., 1, 2, 2, 2, 0..`) | — |
| `+0x00060` | `0x11f88` | the microkernel code | code buffer `+0x1000` |
| `+0x11fe8` | `0x3b58` | data | its own buffer (`this+0x700`) |
| `+0x15b40` | `0x248` | boot program | code buffer `+0x0` |
| `+0x15d90` | tables | `+0x28` (0xf0 bytes), `+0x118` (0x140 bytes) | buffers `this+0x784` (heap 6), `this+0x78c` (heap 5) |

**Loader** `0x80bf9754`: allocates the code buffer (`0x12f88` = boot program
at 0, microkernel at `0x1000`) and copies both in, then allocates and fills
the data buffers, some from templates in `__TEXT,__const` (sizes `0x18`,
`0x38`, `0x18`, `0x350`, `0x9c`, `0x258`). **Allocator** `0x80bf9658`
`(this, &obj, init, size, opts=0x100, align, heap, flag)`: size rounded to
4 KiB, GPU mapping by the IOAccel memory object (heap index at `obj+0x20`),
**GPU address at `obj+0x18`**, CPU mapping, then memcpy of `init` or bzero.
**Setup** `0x80bfa080` calls the loader and allocates the rest: the host
control block (`this+0x74c`, `0xec` bytes -- the microkernel acknowledges
through its word 0), and buffers of `0x1a4`, `0x2000`, `8`, `0x1000`, ...
Heap names in the kext: Default Vertex, Default Fragment, VDM Control
Stream, USE Spill (vertex/fragment), USE Code, Transfer, 3D Aperture -- the
GPU address map behind them is not decoded yet.

**The rest of `initSGX`** (after the clock prologue in the STATUS above;
register names from the DDK where it has them, bank 0 = broadcast):

- BIF: `BIF_CTRL` `0xc00 = 0`, `BIF_BANK0` `0xc78 = 0x77077`, `BIF_BANK1`
  `0xc7c = 0`, `BIF_BANK_SET` `0xc74 = 0`, `DIR_LIST_BASE0` `0xc84` and
  `DIR_LIST_BASE1..7` `0xc38..` = page directories, `BIF_TILE1/2`
  `0xc10 = 0xbeffe00`, `0xc14 = 0xcffff00`, `BIF_CTRL_INVAL` `0xc34 = 8`,
  `MASTER_BIF_MMU_CTRL` `0x4cd0 = 2` and each core's `0x8cd0 = 2`,
  `MASTER_BIF_CTRL` `0x4c00 = 0`.
- Misc (names unknown): `0xa58 = 0`, `0xacc` (`EVENT_TIMER`) `= 0`, `POWER`
  `0x1c = 0`, `0xa7c = 0xa80 = 0`, `0xa00 = 0x7c000`, `0xabc = 0x4c`,
  `0xaa0`, `0x818 = 0`, `0x804 = 0x5e0`, `0x814 = 0xffff`, `0xa74`, `0xb30 =
  0x100`, master `0x4808 = 4`, `0x4144 = 1000`, `0x414c = 0`, `0x630 = 2`.
- **Boot entry per core**: `core n + 0xba0` = entry / 8 into the boot
  program -- `0x40` for core 0, `0x240` for the others (`0x80bfa068` and
  `0x80bfa06c` just return those constants); `+0xbb4 = 0`. iOS writes it
  for cores 1..3 regardless of the core count, so writes to the empty banks
  4-5 are evidently harmless.
- **Code bases**: `USE_CODE_BASE_0` `0xa0c = 0x0c000000 | codeVA >> 6`,
  `USE_CODE_BASE_1` `0xa10` = GPU address of the `+0x118` table buffer
  `>> 6`, `USE_CODE_BASE_2..14` `0xa14..0xa44` from `this+0x7f0..`.
- `EVENT_KICKER` `0xac4` = a buffer's GPU address; `0xa68..0xa70` (bank 0)
  and core 0's `0x8a68..0x8a70`, `0x8a58`, master `0x4a58`: GPU addresses.
- Events: `EVENT_HOST_CLEAR` `0x134` and `CLEAR2` `0x114` = `~0`,
  `0x140 = ~0`, `EVENT_HOST_ENABLE` `0x130 = 0x4000`, `ENABLE2` `0x110`,
  `0x13c = 0`.
- **Start**: host control word 0 = 0, **core 0 `EVENT_KICK` (`0x8ac8`) = 1**,
  then poll host control word 0 until the microkernel sets bit 0 -- the
  "SGX Hang - Poll Failure" if it never does.

### The GPU's MMU and address space (2026-09-30, static)

**Page tables: the DDK's format, confirmed by iOS.** Two levels, 4 KiB
pages, 32-bit GPU addresses: page directory index `va >> 22` (1024 entries),
page table index `(va >> 12) & 0x3ff`. From `sgxmmu.h` (GPL DDK, no 36-bit
MMU on SGX543): PDE = page table physical address `& 0xfffff000 | VALID (1)`,
bits 3:1 page size (0 = 4 KiB); PTE = page physical address `| VALID (1) |
WRITEONLY (2) | READONLY (4) | CACHECONSISTENT (8) | EDMPROTECT (0x10)`.
iOS's PTE writer (`0x80bf4dc4`) builds exactly that from the memory
object's flags (`obj+0x94`: bit 0 -> READONLY, bit 1 -> EDMPROTECT, bit 2
-> CACHECONSISTENT) and the fault dump (`0x80bf8e50`) walks it the same way.
`DIR_LIST_BASE0` takes the page directory.

**Fixed regions in Apple's GPU address map** (constants in the kext):

| GPU address | what |
|---|---|
| `0x80000000` | the "GART" base: the fault dump prints `(va >> 20) - 0x800` as the GART offset in MB |
| `0x84000000 - 0x8407ffff` | must be mapped writable (`0x80bf4dc4` panics "bad pte bits on page" otherwise) |
| `0x87800000` | written into TA/transfer commands |
| `0x8c000000` | written into render commands |
| `0x94000000 - 0x977fffff` | the parameter buffer (`0x80bf9620`), must be writable |

USE code addresses in commands are relative to the code buffer
(`0x80bf963c`: `((va - this[0x6cc]) << 1) & 0xfffff0`).

**The microkernel hard-codes no data address.** Decoding every LIMM in the
code (immediate = `(bits 49:44 << 26) | (bits 40:36 << 21) | bits 20:0`, the
destination register in bits 27:21 -- `usse-dis.py`'s LIMM pattern labels
these wrongly) gives no `0x84.../0x94...` constants: the `0x...dbeef` words
are `LIMM rN, #0xdeadbeef` (a fill pattern), the rest are masks and
`0xad00xxxx` values. So it gets its addresses from registers and the host
control block, and Linux can choose its own layout for the boot buffers.
The "data" part of the blob (`+0x11fe8`) is more USSE code (a 1942-instruction
run at `+0x120a8`).

### The host patches the microkernel (2026-09-30, static)

"No hard-coded data address" above is true of the shipped code, but only
because the host fills the addresses in. After the buffers are allocated
(`0x80bfa080`, from `0x80bfa3f2` on) it rewrites the immediates of seven
LIMM instructions at the very start of the microkernel (offsets relative to
the microkernel, i.e. code buffer `+0x1000`, blob `+0x60`). A LIMM
immediate is set by: low word `[20:0] = imm[20:0]`; high word `&= 0xfffc0e0f`,
`|= (imm >> 14) & 0x3f000` (imm[31:26] -> bits 17:12) `| (imm >> 17) & 0x1f0`
(imm[25:21] -> bits 8:4).

| ukernel offset | immediate |
|---|---|
| `+0x00` | `0x01001000` |
| `+0x10` | GPU address of buffer `this+0x6e0` `>> 4` |
| `+0x18` | GPU address of buffer `this+0x740` |
| `+0x58` | `0x00800900` |
| `+0x60` | GPU address of buffer `this+0x6e8` `>> 4` |
| `+0xe0` | GPU address of buffer `this+0x6f0` (16-byte aligned) |
| `+0xf0` | `7` |

Also computed there: `this+0x72c..0x738` = (microkernel start + `0x1fa8`,
`0x1bf0`, `0x1e80`, `0x21d0`) `>> 3` and `this+0x73c` = (start + `0x21d8`)
`>> 3` -- entry points in 8-byte instruction units; a `0x60`-byte buffer
(heap 6) filled with the words 0..23; and further buffers of `0x14000`
(heap 2, 1 MiB aligned), `0x1000`, `0x8000`, `0x28000` (heap 6). Buffers
named by `this+` offset: `0x6d4` code, `0x6d8`/`0x6e0`/`0x6e8`/`0x6f0`/`0x6f8`
from `__TEXT,__const` templates (`0x18`, `0x38`, `0x18`, `0x350`, `0x9c`
bytes), `0x700` blob `+0x11fe8`, `0x704` template (`0x258`), `0x784`/`0x78c`
blob tables, `0x740` (`0x1a4`), `0x748` host control (`0xec`), `0x750`
(`0x2000`), `0x758` (`8`), `0x760` (`0x1000`, its address goes to
EVENT_KICKER), `0x768`, `0x774` (`0x28`, heap 4, from blob `+0x15d90`).

### Where the register values come from (2026-09-30, static)

`0x80bfa884` (called by `initSGX` on a normal start) fills them:

- `this+0x6cc` = GPU address of the code buffer (-> `USE_CODE_BASE_0`).
- The template buffers (`0x6d8`, `0x6e0`, `0x6e8`, `0x6f0`, `0x6f8`; CPU
  pointers one word after each) are small **PDS programs**: the host writes
  USE code addresses into them, each `((codeVA + ukernel offset - this[0x6cc])
  << 1) & 0xfffff0`, for ukernel offsets `0x90`, `0xc0`, `0x1470`, `0xd0d0`,
  `0x4340`, `0xac50`, `0x10d20`, `0xd3f0`, `0x4510`, `0xad20`, `0x11190`.
- Event PDS registers: core 0 `0x8a68` = `this[0x70c]` = GPU address of the
  `0x6d8` PDS program `& ~0xf`, `0x8a6c` = `this[0x710]` = `1`, `0x8a70` =
  `this[0x714]` = `0x12001`; `0x8a58` = `this[0x718]` = `0x2200c000`; master
  `0x4a58` = `this[0x71c]` = `0x804300f`. Bank 0 `0xa68..0xa70` =
  `this[0x720..0x728]` (set at `0x80bfad68`, not decoded yet).
- Four more LIMM patches inside the microkernel: `+0x86c8` and `+0x7f98`
  get `0x900` (high word `|= 0x140`), `+0x86e0` and `+0x7fb0` get the `0x6f8`
  buffer's GPU address `>> 4`.
- `this+0x744` (the `0x1a4`-byte block at `0x740`): GPU addresses of the
  `0x740`, `0x748`, `0x758`, `0x750`, `0x768` buffers, `+0x14 = 2`, **`+0xf8` =
  the page directory's physical address**, plus timing/DVFS parameters.
- Host control (`0x74c`, `0xec` bytes): zeroed, word 0 = 0 (the handshake),
  a few parameters and two 0x20-byte copies from `this+0x5cc` / `this+0x5ec`.

## STATUS 2026-09-29: the dead end is REOPENED — kernel observability solved

> Superseded by the 2026-09-30 status above: there was no clock dead end.

The "we can neither observe the kernel" premise the 2026-09-28 status rests on is
no longer true. Two ways to watch iOS from the inside now work on the device
(jailbroken iOS 8.4.1, iPad2,5):

- **tfp0 (the kernel task port) — CONFIRMED.** The jailbreak is **daibutsu**
  (what Legacy iOS Kit installs for 8.4.1 on a 32-bit device — *not* TaiG; every
  earlier "TaiG 8.4.1" note in this repo is mislabelled). daibutsu NOPs the
  `pid == 0` guard in `task_for_pid` but keeps the posix/entitlement check, so a
  binary signed with the `task_for_pid-allow` entitlement (daibutsu's own ent.xml
  set: platform-application + get-task-allow + task_for_pid-allow) gets the kernel
  task port: `task_for_pid(mach_task_self(),0,&kt)` succeeds and `pid_for_task(kt)`
  returns pid 0, no panic. The old "tfp0 CLOSED" finding was wrong on two counts:
  a *plain* (unentitled) binary correctly returns KERN_FAILURE, and the old
  kmemprobe "panic" was its own blind kernel scan, not a tfp0 denial. Tool:
  `tools/mtdump/tfp0probe.c` (+ `tfp0.entitlements`).
  **CAVEAT:** a blind `vm_read` of an *unmapped* kernel page PANICS here (it faults
  the bus, it does not return a clean error) — read only addresses known to be
  mapped; finding the kernel base needs a non-scanning slide-finder, not a page walk.
- **kdebug perf trace — WORKS.** `tools/mtdump/kdtrace.c` captures the kernel's
  kdebug stream from userspace (sysctl `KERN_KDEBUG`, no tfp0, no reboot risk) and
  `tools/mtdump/kddec.py` decodes it offline. The AppleS5L8940XPerformanceController
  emits its clock/voltage/perf state machine as debugid **class 0x26** (not 0x27,
  which was a guess from static RE and is empty at runtime).

**What this bought for the GPU clock** (full write-up at the end, "GPU clock via
kernel observability, 2026-09-29"): the **GPU clock domain is identified** — it is
perf-controller domain **0x50** (class-0x26 `sub=56` gate events with a2 high byte
0x50: `0x0050a300/0x00508300/0x00504300`, seen 378× while the GPU is active and 0×
in idle). Still open — the **last mile** — is turning that abstract domain/state
into the raw PMGR `source+divider` the Linux clock driver must write.

**Update 2026-09-30 — Route A is ruled out; Route B is the only path to the value.**
Route A (recover it from Linux-readable static registers) was tested and does NOT
work: on the Linux boot the GPU is off, so `GFX-CLK 0x3f100070 = 0x80000001`
("disabled") — the very register that would hold the enabled value shows nothing —
and the perf-state table at `PMGR 0x200` is the **CPU/SoC DVFS** table, not the GPU
(the GPU has a fixed single clock and is not in a multi-state table). So the enabled
`source+divider` exists only in the **live PMGR on iOS with the GPU on** = Route B.
Route B needs a safe kernel-read primitive: `kas_info` is `ENOTSUP` here and
`mach_port_kobject` returns a `VM_KERNEL_ADDRPERM`-permuted address, so it takes a
real-address info-leak (`sysctl KERN_PROC` `kinfo_proc.e_paddr`) + a `proc->task`
offset to derive the permutation constant, then un-permute the perf-controller object
and read its `_pcBaseAddress` (+0x550) = PMGR kernel VA. Details in the last two
sections. So clock bring-up is no longer an unbreakable wall, but the last mile is a
bounded (reboot-prone) kernel-primitive task, not a quick read.

**Update 2026-09-30 (later) — iBoot RE breakthrough (see "iBoot/iBEC clock RE" at
the end).** Reversing `iBEC.dec` (linear C, no OOP wall) gave the **PLL formula**
(`freq = 24MHz*M/P/2^S`) which shows **`0x3f100010` = 200 MHz = the GPU clock**, the
**clock write mechanism** (`write config to 0x3f100038+idx*4, poll bit30 clear`;
these regs are writable in iBEC), and iBEC's full boot value table (which *parks*
GFX off). So the frequency (200 MHz) and the write mechanism are now known; what
is left is the enabled GFX source+divider (iBEC parks it; computation says the
200 MHz chain, divider 1) and whether those registers are writable *from Linux* —
a reboot-prone on-device test to run with a human present.

The 2026-09-28 status below is kept as the record of why the *static-only* path
stalled — it is accurate for that path, and its register map/gate protocol are still
correct and reused.

---

## STATUS 2026-09-28: clock bring-up is a DEAD END (for the static-only path)

> Superseded: the hangs were bank-0 reads, not a missing clock (2026-09-30).

After a full arc of work (below), clocking the SGX from Linux is **blocked**, and
honestly so: it is the hardest wall in the project because the mechanism lives in
kernel-only, object-abstracted code we can neither observe nor fully reverse with
the access we have. The bottom line, so nobody re-treads it:

- The GFX clock chain is mapped: SGX ← gate/power **GFX** (idx 0x5c) ← **GFX_SYS-CLK**
  (id30, PS 0x3f101024, clk mirror 0x3f100074) + **GFX-CLK** (id31, PS 0x3f101028,
  clk 0x3f100070) ← one of **MANAGED0..4** (0x3f10002c..3c) ← PLL/PREDIV.
- **Enabling the GFX_SYS/GFX PS gates works** (0x3f101024/28: `(v&~0x10f)|0xf` →
  0x3ff) — first time the GPU clock *domain* is on under Linux — **but it is not
  enough**: the GFX clock's **source (a MANAGED clock) stays off**, so the first
  SGX register read (0x35100020) hangs the bus every time (hard freeze, reboot).
- **Every writable knob was tested and none clocks the GPU**: the gates (above),
  the CPU/SoC voltage rails (0x3f100110+idx*0x10, all 0x80010000 — DVFS voltage,
  not GPU), the perf-state setter (0x3f102100/04 — CPU DVFS, no effect on GFX),
  and a bit31 reset pulse. The clock **config registers (0x3f100000+idx*4, incl.
  GFX 0x70/0x74) are read-only mirrors** — direct writes are silently ignored.
- The MANAGED-source enable is only reachable through a **runtime-dispatched
  clock-controller** (the router `FUN_804c09a8`/`FUN_804c1640` in the base
  AppleARMPerformanceController kext). Ghidra (full analysis of the decrypted
  8.4.1 kernelcache) **cannot recover that class's vtable** (no RTTI, no data
  xref), so the concrete "enable MANAGED N / route GFX" register write is not
  pinned. The register *primitives* are known (clock_gate_switch, the managed-rail
  ramp `FUN_80b8d280`, the domain-apply `FUN_80b8cdbc`) — but they are the CPU/DVFS
  paths; the GPU-clock path is behind the unrecovered controller.
- **We cannot observe the kernel** to shortcut this: TaiG 8.4.1 has no working
  tfp0 (plain fake-signed → KERN_FAILURE; task_for_pid-allow entitlement → kernel
  PANIC), so there is no live PMGR read and no kernel-write trace on iOS. kdebug
  (sysctl, userspace) would give state *indices*, not register values.

**What would unblock it:** (a) a kernel-observation primitive — a working tfp0 on
a different jailbreak/firmware, or a kernel hook — to trace the exact PMGR writes
iOS makes when the GPU powers on; or (b) recovering the clock-controller class and
its commit path (deeper RE, or a decompiler that reconstructs the C++ vtables); or
(c) reimplementing the clock framework from the ADT topology. All are large.

**Reusable and solid (not lost):** the full clock topology and register map; the
working gate protocol; the decrypted 8.4.1 kernelcache + cached Ghidra project
(scratchpad); the USSE disassembler validated on real shaders; the gltrace
command-stream capture; the reliable Arch capture channel. Note also (below) that
software rendering is measured too slow (SuperTux ~0.22 fps on llvmpipe), so the
GPU really is the only path to smooth graphics — and it is the one gate we cannot
currently open. **Paused here on purpose.**

## Why it matters

Software OpenGL (Mesa 26.1.6 llvmpipe, LLVM 22, both cores, 768×1024, measured
2026-09-27 under XFCE):

| Test | fps |
|---|---|
| glxgears, 300×300 offscreen (pbuffer) | 206–287 |
| glxgears, 300×300 window | 54 |
| glxgears, full screen | 12.5 |
| SuperTux 0.6.3 title screen, GL renderer | 0.22 |
| same, SDL's own software renderer | 1.2 |

Along the way llvmpipe turned out to have a 32-bit ARM bug: gallivm treats
NEON as "has vector rounding", which only AArch64 has, so LLVM turned every
vector round into four calls to musl's `rintf`/`nearbyintf` (19% of the time
in those two functions alone). Fixing that brought SuperTux to 0.27 fps; the
rest is plain fragment work (~750 NEON instructions per pixel for SuperTux's
shader, half of it bilinear filtering). Software rendering will not make this
screen smooth; only the GPU can.

## What the ADT says

`arm-io/sgx`:

| property | value |
|---|---|
| compatible | `sgx,s5l8940x`, device_type `sgx` |
| reg | `0x35100000` + 96 KB (the SGX), `0x3f10b000` + 4 KB (reads all zero; a glue block?) |
| interrupts | 49 |
| clock-gates, power-gates | 92 |
| clock-ids | 0x127 (295) — not decoded |
| brn_31195 | an IMG erratum number: the DDK's `sgxerrata.h` has it for SGX543 revisions 122 and 1221 only |
| gfx-qos | present, not decoded |

Gate 92 ("GFX") has no register of its own in the pmgr node's
`device-clocks` table (`scripts/adt-pmgr-map.py`). Its parents are:

| id | name | power state | clock register | parents (in mux order) |
|---|---|---|---|---|
| 30 | GFX_SYS-CLK | `0x3f101024` | `0x3f100074` | MANAGED0, MANAGED1, MANAGED2, MANAGED4 |
| 31 | GFX-CLK | `0x3f101028` | `0x3f100070` | MANAGED0, MANAGED1, MANAGED2, MANAGED4 |

and the MANAGED clocks in turn choose among the PREDIV clocks:

| id | name | clock register | parents |
|---|---|---|---|
| 24 | MANAGED0-CLK | `0x3f10002c` | PREDIV0, PREDIV1, PREDIV4, PREDIV5 |
| 25 | MANAGED1-CLK | `0x3f100030` | PREDIV0, PREDIV1, PREDIV2, PREDIV3 |
| 26 | MANAGED2-CLK | `0x3f100034` | PREDIV0, PREDIV1, PREDIV2, PREDIV3 |
| 27 | MANAGED3-CLK | `0x3f100038` | PREDIV0, PREDIV1, PREDIV2, PREDIV3 |
| 28 | MANAGED4-CLK | `0x3f10003c` | PREDIV0, PREDIV1, PREDIV4, PREDIV5 |

## The clock registers as iBoot leaves them

From `tools/pmgr-map.sh` on a fresh boot (Linux after iBSS/iBEC):

| register | name | value |
|---|---|---|
| `3f100010` | PREDIV0-CLK | `a001a642` |
| `3f100014` | PREDIV1-CLK | `00010960` |
| `3f100018` | PREDIV2-CLK | `a0012559` |
| `3f10001c` | PREDIV3-CLK | `00010960` |
| `3f100020` | PREDIV4-CLK | `40000000` |
| `3f100024` | PREDIV5-CLK | `00010960` |
| `3f100028` | PREDIV6-CLK | `a0012502` |
| `3f10002c` | MANAGED0-CLK | `00010960` |
| `3f100030` | MANAGED1-CLK | `00000008` |
| `3f100034` | MANAGED2-CLK | `40000000` |
| `3f100038` | MANAGED3-CLK | `90011041` |
| `3f10003c` | MANAGED4-CLK | `90000001` |
| `3f10006c` | HPERF-RT-CLK | `90000002` |
| `3f100070` | GFX-CLK | `80000001` |
| `3f100074` | GFX_SYS-CLK | `90000002` |
| `3f100078` | HPERF-NRT-CLK | `a0000013` |
| `3f101024` | GFX_SYS power state | `00000300` — off |
| `3f101028` | GFX power state | `00000300` — off |

Right after the clocks, a block that looks like a table of sixteen 16-byte
rows, the first seven different, the rest identical, and one word after it:

    3f100200: 01010101 03010101 00000001 00000000
    3f100210: 00000004 03010100 00000000 00000000
    3f100220: 01220001 00210122 00000021 00000000
    3f100230: 02430002 01210224 00000022 00000000
    3f100240: 04430002 02210424 00000022 00000000
    3f100250: 1f430002 03210824 00000022 00000000
    3f100260: 04430002 00210124 00000022 00000000
    3f100270: 01010101 00010101 00000001 00000000   (and the same up to 3f1002f0)
    3f100300: 00000002

Not decoded here.

## What was done

1. **As booted**, every SGX offset reads `0x0015006b` — the same value as
   `0x38000000`, where there is nothing. An unpowered block does not fault on
   this fabric; it returns whatever the bus last carried
   (`p105-pmgr-gates.md`).
2. **GFX_SYS and GFX power domains on**, the way the platform driver switches
   gates (`(v & ~0x10f) | 0xf`, then wait for bits 7:4): both go
   `0x300 → 0x3ff`, target and actual agree.
3. **The first SGX read after that** (`0x35100000 + 0x00`) hung the whole
   machine. Recovered with a hard reset and `./cascadia flash --kdfu`; no lasting
   effect.

Nothing else has been tried on the hardware. For the record, iBEC's reset of
"device 6" also pulses `0x3f101020` (unnamed), HPERF-NRT (`0x3f10102c`) and
`0x3f101044` (unnamed) together with the two GFX domains, and the pmgr node
carries three bus bridges (`0x38c00000`, `0x38d00000`, `0x38e00000`) with 512
bytes of `bridge-settings`.

## Open questions

- **Is the GPU clocked from sources that are off?** Every clock the running
  system plainly uses has bit 31 set (PREDIV0/2/6, MANAGED3/4, the HPERF
  clocks). MANAGED1 (`00000008`) and MANAGED2 (`40000000`) do not. If the low
  bits of GFX-CLK (`80000001`) and GFX_SYS-CLK (`90000002`) pick the parent,
  the GPU hangs off MANAGED1 and MANAGED2, which would explain the hang.
- **But HPERF-RT-CLK reads `90000002` too** — the same value, and the same
  third parent (MANAGED2) in its own list (MANAGED0, 1, 2, 3) — and the
  real-time fabric it names is plainly alive. So either the parent select is
  not the low bits, or bit 31 is not "running", or HPERF-RT is not what it
  seems. This is unresolved, and it decides whether "the GPU has no clock" is
  even the right diagnosis.
- `clock-ids` 295 and `gfx-qos` in the sgx node: meaning unknown.
- What the `0x3f10b000` block is (reads zero before and after power-on).

## Once the SGX answers

Register offsets come from the GPL kernel side of TI's DDK
(omap5-sgx-ddk-linux: `hwdefs/sgx543defs.h`, `sgxmpdefs.h`, `sgx_mkif_km.h`).
An MP core has 16 KB banks: the master at `+0x4000`, core n at
`(n + 2) * 0x4000`.

    +0x00 CLKGATECTL  +0x08 CLKGATESTATUS  +0x20 CORE_ID  +0x24 CORE_REVISION
    +0x28/+0x2c DESIGNER_REV_FIELD1/2      +0x80 SOFT_RESET
    master: +0x4000 MASTER_CORE (cores enabled)  +0x4010 CORE_ID
            +0x4014 CORE_REVISION  +0x4080 SOFT_RESET  +0x4c00 BIF_CTRL

`sgx-probe.sh` decodes CORE_ID and CORE_REVISION; brn_31195 predicts revision
1.2.2. Reading the ID is only the first checkpoint: after it come the
microkernel the SGX runs, a kernel driver, and a userspace GL driver, none of
which exist for this chip in the open.

## Reversing the command stream by tracing iOS (2026-09-28)

The other end of the problem: rather than bring the GPU up blind, watch iOS
drive it and copy what it does. The idea, and the ABI, so far:

**The stack** (from the 6.1 kernelcache, `com.apple.driver.IMGSGX543`):
- kext `IMGSGX543`, `IOClass SGXDriver543`, matches `sgx,s5l8940x`, category
  `IOAcceleratorES`, built on `IOAcceleratorFamily` (20.0.9). Userspace GL
  driver is `IMGSGX543GLDriver` (`IOGLESBundleName`), in the dyld shared cache.
- Userspace → kernel goes through IOAccelerator's user clients:
  `IOAccelSharedUserClient` / an `IOAccelContext` (`IMGSGXGLContext` on the
  kernel side), carrying `IOAccelCommandBuffer`s. The SGX-specific payload is
  validated by `IMGSGXGLContext::copyAndValidateVendorPayload(IOGLStreamHardwareCommand*,
  size_t, IMGSGXResource*, IMGSGXCommandDescriptor*)` and
  `validateRenderCommand` — that vendor payload is the USSE/PDS/command stream
  we want.

**How to eavesdrop without tfp0** (which this jailbreak denies): be the
rendering process. A small armv7 GLES tool draws a triangle to an offscreen
FBO; in the same process, `fishhook` rebinds `IOConnectCallMethod`,
`IOConnectCallStructMethod`, `IOConnectCallAsyncStructMethod` and
`IOConnectMapMemory` to log the selector, the input scalars/structs, and the
mapped shared buffers before calling through. The submit selector and the
command-buffer layout fall out of the log; the mapped buffers hold the vendor
payload. Cross-referenced with the SGX register set (TI DDK) and the USSE
decoder (Vita3K's), that is the raw material for a driver. Tool: `tools/iosgpu/`
(planned).

## First capture (2026-09-28): it works

`tools/iosgpu/gltrace` ran on the iPad's iOS 8.4.1 and drew a triangle
offscreen with GLES2. The headless EAGL context came up -- renderer string
**"PowerVR SGX 543"**, which confirms the core without the register read that
hangs the bus -- and the triangle rendered (centre pixel `ff 80 00 ff`, our
fragment colour). No tfp0, no kernel access, no reboot.

The IOAccelerator submission ABI, captured live (connections are user clients;
selectors are the external methods):

- `IOConnectMapMemory` maps three shared buffers per context on the render
  connection: **type 0** 0x8000 (the command / parameter buffer -- structured
  GPU data), **type 2** 0x1000 (an event/sync ring, each slot tagged
  `"EVTINIT"`), **type 1** 0x1000 (a status page, zero before the draw).
- Per frame the driver calls `IOConnectCallStructMethod` sel 2 (136-byte
  descriptors: they carry addresses and a length at +0x18, e.g. `c0 01`.. len
  0x3c) and sel 3 (8 bytes), then `IOConnectCallMethod` sel 0 with a 0- or
  96-byte struct -- the submit/kick. Context setup uses sel 0/256/258 and a
  96-byte struct that holds GPU virtual addresses (framebuffer, USSE/PDS).
- The 32 KB command buffer's head has descriptor records (counts, offsets,
  and 0x98..-based GPU addresses like `00 c0 0a 98`), i.e. the parameter/
  command stream the SGX consumes.

So the harness for reversing the command stream exists and is safe to iterate:
change the GL calls, watch the buffers and submits change. Next: draw variants
(clear-only, one triangle, a texture) and diff the type-0 buffer and the sel-2
descriptors to pin down the command and USSE/PDS layout, cross-referencing the
USSE encoding (Vita3K's SGX543 work) and the register/opcode names in the DDK.

## The command buffer's structure, by diffing (2026-09-28)

`gltrace` now renders three frames into one context, changing only one thing at
a time, and writes the full contents of each mapped buffer after each:
frame **a** clears only, **b** draws a triangle (uColor orange), **c** the same
triangle in teal (only the uniform differs). `tools/iosgpu/diffmaps` compares
them. (Buffers come off the device as `openssl base64` over ssh -- scp to this
jailbreak's sshd hangs, and the device has no od/xxd/base64, only openssl.)

Findings, all in the 32 KB command buffer (`map type 0`); the event ring
(`map2`) and status page (`map1`) never change with the draw or the colour:

- It is not a linear command FIFO but a **fixed-layout control page**: a whole
  frame (clear vs full draw) changes only ~40 of 32768 bytes, patched in place.
- It holds **GPU virtual addresses** (`0x0098xxxx`, `0x0190xxxx`) that point at
  separately-allocated shader programs and constants -- those allocations are
  *not* among the three IOConnectMapMemory buffers, so the USSE/PDS bytes are
  not captured yet.
- **Colour-only (b vs c)** moves the constant allocation: the pointer at
  offsets 0x69 and 0x81 goes `0x0098db80 -> 0x0098dfd0` (+0x450), and two
  descriptor args at 0x118 change. So a fragment constant lives in its own GPU
  allocation, reached by a pointer in the control page -- not inline.
- **Clear vs draw (a vs b)** changes a VA at 0x41 (`0x019000f0 -> 0x01900120`)
  and a run of 8-byte descriptor records at **0x118..0x148**, each
  `[u32 arg][0x02][index][u16 size]` -- a table of the state/allocation blocks
  the draw added (the args are handles/counts that shift as allocations are made).

Next: capture the allocations those VAs point at (the USSE fragment/vertex
programs and the PDS constants). They are made through the IOAccelerator
resource path, not IOConnectMapMemory; the submit struct (IOConnectCallMethod
sel 0) carries CPU-space pointers into them (values like `0x007446f4`, next to
map0's own `0x0074f000`). Following those is the way into the USSE bytes, which
Vita3K's SGX543 work then decodes.

## Reaching the shader/command bytes (2026-09-28)

The programs and constants the control page points at are not in the three
IOConnectMapMemory buffers -- but the driver keeps their CPU copies in our own
process, so `gltrace` now also snapshots its address space: it probes
0x00400000..0x00c00000 page by page with `vm_read_overwrite` on
`mach_task_self()` (safe -- unmapped pages just return an error) into a sparse
image `gt_<a|b|c>_arena.bin`. The device has `cmp`, so the diff is done there
(`cmp -l a b`), and only the small result is pulled -- the 8 MB image gzips to
~56 KB, it is almost all zero.

Diffing the arena (base 0x00400000) shows where the real work lands:

- **~0x740000**: the CPU-side command / PDS / USSE buffer the control page
  references. Clear→draw fills it with structured words; **colour-only
  (b→c) rewrites program words here** (e.g. 0x7402dd..0x740408), so for a
  constant uniform the driver *folds the colour into the USSE/PDS program*
  rather than storing it as separate data -- the first real USSE we have.
- **~0x750000**: a command/kick ring whose slots carry GPU addresses and flip
  between ASCII tags `"INIT"` (`49 4e 49 54`) and `"LIVE"` (`4c 49 56 45`);
  each submit's descriptor addresses appear here.
- **~0x6b4000..0x6bb000**: the mapped parameter/state buffers (map1/map2's
  arena); small descriptor fields and pointers change, matching the map0 diffs.
- The descriptor args at map0+0x118 (`0c/02`) also appear at ~0x744900 in the
  arena -- the control page is shadowed here.

So the pipeline is complete: render a variant, `cmp` the arena on device, pull
the diff, and the changed bytes are the command/USSE/PDS for that change. Next
is the slow part -- decode the ~0x740000 program words as USSE (Vita3K's
encoding) and the ~0x750000 ring as the kick/DMA format, one controlled change
at a time (one attribute, one instruction, one constant).

## Correction: 0x740000 is a patch/descriptor table, not USSE (2026-09-28)

Pulling the actual bytes at ~0x740000 (frame b) corrected the earlier reading.
It is not shader code but a **relocation / descriptor table**: 40-byte records,
each roughly `{srcA, srcA2, srcA3, dstA, patchA (a GPU VA ~0x1461xxxx), u32
index, flags}`, the addresses pointing into the mapped buffers (0x006bxxxx,
0x0075xxxx) and the arena. The `index` field (0f, 0d, 04, 05, 0e, 07, ...) is
the same handle that appears in map0's 8-byte records at 0x118 and that the
colour-only diff moved. So the b->c changes here are **addresses and handles in
this table shifting because the fragment constant is a separate allocation**
that got a new address -- consistent with the map0 pointer move, and *not* the
constant being folded into USSE. (The earlier "folded into USSE" note was
wrong; this supersedes it.)

The real USSE fragment program is small (a constant-colour shader is a couple
of instructions) and lives in one of the referenced allocations, not in this
table. Finding and decoding it needs a USSE disassembler rather than more blind
diffing -- that is the next, and genuinely long, step. Structure mapped so far:
map0 control page -> this patch table at ~0x740000 -> the mapped param/state
buffers (0x6b3000-0x6bc000) and the INIT/LIVE command ring (~0x750000), with
GPU VAs (0x0098xxxx / 0x0190xxxx / 0x1461xxxx) threaded through all of them.

## A USSE disassembler, and finding the programs (2026-09-28)

`tools/iosgpu/usse-dis.py` disassembles the SGX543's USSE ISA. USSE
instructions are 64 bits, top 5 bits = major opcode; the full encoding is taken
as 64-char bitstrings from the Vita3K project's decoder (GPLv2, like this repo),
turned into (mask, value) matchers. `dis FILE OFF N` lists instructions;
`scan FILE` finds runs of valid ones.

Run over the captured arena (`gt_b`), it works at the opcode level: it finds
PHAS-anchored programs, e.g. a small one at cpu 0x6a45c0 that reads

    PHAS / VBW / NOP / VTST / VLDST / SPEC / VPCK / SMP / VMAD / VMOV

before turning to data -- a plausible complete program. Caveats, honestly: the
low-fixed-bit opcodes (VMAD2 has only 5 fixed bits) match ~1/32 of random
words, so `scan` has false positives; the pred/end fields are multi-bit and not
yet mapped to meaning; and program boundaries are fuzzy. That 0x6a45c0 program
has an SMP (texture sample), which a constant-colour fragment shader would not
-- so it is likely a vertex or an always-present driver program, not ours. A
big PHAS-dense region at ~0x6bf000 (and copies at 0x7d7000, 0xaa9000, 0xbc1000)
looks like the driver's built-in program library.

So the disassembler exists and identifies instructions; what remains is the
long part: pin our fragment program exactly (follow the control-page GPU VA, or
diff two shaders that differ by one instruction), then decode operands
(banks/swizzles/dest) field by field. That is the ongoing USSE RE.

## The fragment program, located (2026-09-28)

`gltrace` now compiles two fragment programs in one process -- `gl_FragColor =
uColor` and `= uColor*uColor` -- draws both, and dumps the arena. Both render
correctly (prog2's centre pixel `ff 40 00 ff` = (1,0.5,0) squared), and both
USSE programs are then in the arena. Scanning for PHAS-anchored small programs
finds them, and they share a byte-identical **fragment preamble**:

    PHAS   fa44070000000000
    NOP    f800094000000000
    VTST   488b0281a00c0000
    VLDST  e9a30084a0000000
    SPEC   f920000000000000

This exact sequence appears at the head of every fragment program (0x3005c0,
0x300940, 0x300cc0, 0x3adec0, ...), so it is the driver's boilerplate (load the
iterated inputs / set up the pixel phase), and a reliable fingerprint for "this
is a fragment program". After it comes a short body that differs with the
shader's arithmetic -- VPCK (pack to the F16 output) plus a NMAD/MAD-class
multiply (`V16NMAD`/`VMAD`) for the `*` -- and ends folding into data.

So our fragment programs are pinned (~7-12 instructions) and the multiply maps
to a NMAD/MAD-class op, as expected. What is not yet done: the exact operand
fields (which bits pick the uniform, the register banks, the swizzles, the
F16 pack) -- single-instruction opcode IDs from the permissive matcher are also
not all trustworthy yet. Decoding those fields, using Vita3K's field
definitions, is the next step.

## Operands decode too (2026-09-28)

`usse-dis.py` now decodes operand fields (from Vita3K's field definitions) for
the output-writing ops. The fragment program at 0x3adec0 reads:

    PHAS / NOP / VTST / VLDST / SPEC          (preamble)
    V16NMAD                                    (the colour arithmetic)
    VPCK  sfmt=u16 dfmt=f32 dmask=15 dbank=o dn=6 s1bank=o s1n=3 ... end=1

So the shader ends by VPCK-ing the 4 components (dmask=15) into **output
register o6**, with the `end` bit set on the last instruction. That `end` bit
is exactly the one byte by which the two captured fragment programs differed
(0x40810a3e... vs 0x40850a3e...: end 0 vs 1) -- which confirms the field
decode. Register banks (temp/pa/o/sa), pack formats (f32/f16/...), dest mask
and reg number, and the end flag now come out correctly; VMOV and VPCK are
wired up, more opcodes to follow the same way.

## Reading the arithmetic: uColor*uColor = VMUL (2026-09-28)

With V16NMAD operands decoded (op2 selects the ALU op: VMUL/VADD/VFRC/VMIN/
VMAX/VDP...), the two captured fragment programs read cleanly and confirm the
whole method:

    prog1  gl_FragColor = uColor:
        PHAS / NOP / VTST / VLDST / SPEC
        VPCK  dmask=15 dbank=o dn=6 s1bank=o s1n=3      ; o6 <- o3, pack to output

    prog2  gl_FragColor = uColor*uColor:
        PHAS / NOP / VTST / VLDST / SPEC
        V16NMAD op2=VMUL dmask=15 dbank=o dn=3 s1n=3 s2n=3   ; o3 = o3 * o3
        VPCK    dmask=15 dbank=o dn=6 s1n=3                  ; o6 <- o3, end

So the `*` compiled to `VMUL o3, o3, o3` (square in place) followed by the pack
to output register o6 -- exactly the source, read back from the captured USSE.
We can now read fragment-program arithmetic and dataflow, not just opcodes.
(The preamble VTST/VLDST/SPEC set up the pixel phase / load the iterated
inputs; the separate program at 0x3005c0 with an SMP is a different,
texture-using program the driver keeps around, not one of ours.)

Stage 4 (decode USSE) is now real for the vector ALU + pack path. Still to do:
the swizzle fields, the preamble's exact meaning, the SMP/texture path, and how
PDS feeds uniforms/varyings into those `o`/`pa` registers.

## Swizzle: not solved yet (2026-09-28)

Tried to decode VPCK's component select by capturing `gl_FragColor = uColor`
vs `uColor.bgra` (both render correctly -- bgra gives centre pixel
`00 80 ff ff`). The identity VPCK's comp fields read as [0,1,2,3] as hoped, but
the identity-vs-bgra difference lands in the `jj` (comp_sel_2) and `w`
(src2_n) bits, not the comp0 bits a naive 4x2 layout predicted. So VPCK's
swizzle/component-select encoding is more subtle than a flat four 2-bit selects
(it interacts with src2_n and the scale/format), and is **not decoded yet** --
recorded so the wrong assumption is not repeated. The solid, verified part of
the operand decode stands: register banks, dest number/mask, pack formats, the
end flag, and the V*NMAD ALU op (VMUL et al.). Swizzles, the SMP/texture path,
and the PDS uniform feed remain for stage 4.

## Swizzle: solved (2026-09-28)

Reading Vita3K's `vpck()` gave the trick the naive layout missed: VPCK's
component-select is `SWIZZLE(comp0, comp_sel_1(ii), comp_sel_2(jj),
comp_sel_3(oo))`, where **comp0's high bit is not its own -- it comes from
`src2_n & 1` when the source is not F32** (from `comp0_sel_bit1` only for F32).
That is exactly why the uColor->uColor.bgra change had landed in the src2_n and
jj bits. With that, the two programs read correctly:

    uColor       VPCK ... dn=6 s1n=3 src1.rgba     (identity)
    uColor.bgra  VPCK ... dn=6 s1n=3 src1.bgra

`usse-dis.py` now prints the VPCK source swizzle. Stage 4 solid so far: opcodes,
operands (banks/reg/mask/fmt/end), the V*NMAD ALU op, and VPCK swizzles.
Remaining: the SMP/texture path and how PDS feeds uniforms/varyings into the
registers.

## Correcting the register banks; the uniform lands in a primary attribute (2026-09-29)

The bank names in the reads above were wrong: the 2-bit bank-select does not map
straight to the RegisterBank enum. From Vita3K's usse_decode_helpers.cpp the
no-ext mapping is dest {0 temp,1 output,2 primattr,3 indexed1}, src1/2
{0 temp,1 output,2 primattr,3 secattr}, and the ext bit picks
secattr/special/immediate/indexed. usse-dis.py now decodes banks per operand
role and ext bit. With that the fragment program reads correctly:

    V16NMAD VMUL dmask=15 dbank=pa dn=3 s1=pa3 s2=pa3      ; pa3 = pa3 * pa3
    VPCK    dmask=15 dbank=pa dn=6 s1=pa3 src1.rgba        ; pa6 = pack(pa3)

So it works in the **primary-attribute** bank (not "output"): the colour is in
pa3 and the pixel result in pa6. That answers the start of the uniform question
-- a uniform arrives in a *primary attribute* register, and on SGX primary
attributes are loaded by the **PDS** program before the USSE runs. So the PDS is
what DMAs uColor into pa3; decoding the PDS program (a separate small program the
control page points at) is the next step. (This supersedes the "o3/o6" bank
names in the notes above; the dataflow and swizzle there were right, the bank
labels were not.)

## The uniform's data, located (2026-09-29)

Searching the captured arena (gt_c, uColor = orange (1,0.5,0,1)) for the value,
found offline without the device:

- **F32 vec4 (1.0,0.5,0.0,1.0)** at 0x773185 and 0xb60185 -- the value the app
  set with glUniform4f, in the constant buffer the PDS reads (two copies, the
  two programs/draws).
- **F16 (1.0,0.5,0.0,1.0)** at 0x745230 etc. -- the driver's converted copy,
  matching the VPCK sfmt=f16 the fragment program uses.

So the uniform path is visible end to end: the app's F32 -> the driver converts
to F16 -> placed where it is loaded into the fragment program's pa3. Full PDS
decode (the DMA program that does the load) is still open -- PDS is its own
instruction set with no Vita3K reference -- but the constant and the F32->F16
conversion are pinned. (The F32 sits at a non-aligned 0x..185, i.e. inside a
larger constant/uniform record.)

## Texture/SMP fragment program is GPU-only, not CPU-readable (2026-09-28)

Reworked the capture path onto the user's x86 Arch box (see the tooling note
below): the Mac's usbmuxd was corrupting bulk transfers; the Linux
libimobiledevice path is clean (8 MB in 1.6 s), so we can now pull full arenas
and re-run at will. Re-ran gltrace with the texture-sampling frame (prog3 =
`texture2D(uTex,t)`, 2x2 RGBA, NEAREST; centre pixel = ffff00ff = the corner
texel, so the sample really happens).

Located the app's fragment programs in the CPU arena by their preamble
`PHAS(fa44070000000000) / VBW(50850009e0000300) / NOP / VTST / VLDST / SPEC`:

- **prog1** (`gl_FragColor = uColor`): preamble + `VPCK pa6 <- pa3 (src1.rgba)`.
- **prog2** (`uColor*uColor`): preamble + `V16NMAD VMUL pa3 = pa3*pa3` + `VPCK pa6 <- pa3`.

Both match the earlier reads exactly. Also found the vertex program as a
genuine *multi-phase* USSE program: its `PHAS` is `fa440000000003be` (non-zero
next-phase address field 0x3be, vs `...070000000000` = single phase), body =
`LIMM`(the 0.5 scale/bias) + `VMOV o0<-pa0 / o1<-pa1` across several phases.

**The texture fragment program (the one with the SMP) is not in CPU memory.**
Widened the arena dump to the whole 0x200000-0x2000000 window (vm_read on
mach_task_self) and even took a snapshot the instant after `glLinkProgram(prog3)`
/`glUseProgram(prog3)`, before any draw. In all 30 MB there is **not one run of
>=6 consecutive valid instructions containing an SMP**, and only two distinct
PHAS words exist (the two above) -- neither leads to an SMP. Every apparent
"SMP" (major 0x1c) is data (an `0xe...` word whose low 32 bits are a small
count); its decoded coordn/texn are noise. The GPU VA the pipeline binds for
the fragment program (map0 offset 0x69: prog2 0x00980f30 -> prog3 0x00980ac0)
is in the 0x98xxxx range, which `vm_read` returns as **zero** -- i.e. the
compiled fragment program is uploaded to GPU-only / write-combined memory and
the CPU staging copy is kept only for the trivial colour shaders.

map0 diff prog2(c) vs prog3(d), 41 bytes in 8 regions: the bound program VA at
0x69, resource counts at 0x118 (0x01->0x0d, +8 0x10->0x02), and a **new
non-zero record at 0x15c = `01 00 04 00 0a`** that is absent for the
non-textured draws -- the texture/sampler binding. map1/map2 unchanged.

Consequence: reading the real SMP encoding of our own shader needs either a
hook on the driver routine that writes the USSE into the GPU buffer (in
IMGSGX543GLDriver, in the dyld shared cache), or reading that GPU buffer through
the IOAccelerator surface API rather than raw vm_read. The SMP *encoding* is
already defined in usse-dis.py from Vita3K; what is missing is a captured
instance to validate it against. Operand decode itself is validated on
prog1/prog2 (banks/mask/fmt/end/ALU-op/VPCK-swizzle all correct).

## Capture tooling now runs through the Arch box (2026-09-28)

iOS-over-USB bulk transfers reset on the Mac (Apple's closed usbmuxd); they are
solid through the user's x86 Arch box (open libimobiledevice usbmuxd). On Arch:
`iproxy 2222 22` as a transient unit `cascadia-iproxy`, iPad root over a legacy
OpenSSH (`-o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa`),
key auth set up (helpers `~/ipad` / `~/ipush` / `~/ipull`). gltrace deploys to a
fresh `/var/root/gltraceN` each time (AMFI kills a re-used path on stale cdhash).

## GPU clock/power topology — full map from the ADT (2026-09-28)

Pivoted to the real end goal (running the GPU from Linux). The first gate is the
clock bring-up: on Linux the first SGX register read hangs the bus because the
SGX's feeding clocks are off. Pulled the complete clock tree from the ADT with
NO device risk -- it is all static data, live in the IORegistry
(`ioprops "IODeviceTree:/arm-io/pmgr"` and `.../sgx`) and offline in the
teammate's ADT dump (`~/iBSSloader/dts/apple-p105ap-raw.json`, pmgr node
`device-clocks` = 5104 bytes, 116 records of 44 bytes: [id|flags]/.../[u32
sources packed as bytes]/.../[16-char name]).

**SGX device node** (`/arm-io/sgx`, `compatible "gpu,s5l8940x"`):
`clock-gates = 0x5c`, `power-gates = 0x5c`, `clock-ids = 0x127`,
`interrupts = 49 @ AIC`, `gfx-qos = 1,1`, `brn_31195 = 1` (SGX543 rev 1.2.2),
reg = 0x35100000/0x18000 (SGX bank) + 0x3F101000/0x1000 (its PMGR power block).

**The clock chain (each clock's `id`, and its candidate source ids):**

- SGX  ->  gate/power **GFX** (id 0x5c, record 81); GFX sources = {0x1e, 0x1f};
  field5 = 0x02d5 (QoS/freq-ish). 
- **GFX_SYS-CLK** (id 0x1e, rec 29): sources {0x18,0x19,0x1a,0x1c}, sel 0x09.
- **GFX-CLK**     (id 0x1f, rec 30): sources {0x18,0x19,0x1a,0x1c}, sel 0x0a.
- **MANAGED0..4** (ids 0x18..0x1c, recs 23-27): fed by PREDIV/PLL; the 0x80 flag
  byte = "managed/enabled" marker. These are the ones that read DISABLED
  (bit31 clear) on Linux -> the missing link.
- PREDIV0-6 / VID / PLLs feed the MANAGED clocks; those PLLs are already up on
  Linux (the CPU runs off them).

So the bring-up order is: MANAGED0/1(/2/4) on (source+divider, the perf-state) ->
GFX_SYS-CLK + GFX-CLK on (0x3f101024 / 0x3f101028, seen going 0x300->0x3ff) ->
GFX gate/power (idx 0x5c) -> only THEN read SGX CORE_ID at 0x35100020.

**Known register spots** (from earlier Linux peeks): GFX_SYS-CLK 0x3f101024,
GFX-CLK 0x3f101028; MANAGED0/1 0x3f100070/0x3f100074 (bit31=enable, bit30=busy);
perf-state index -> PMGR+0x300 then wait bits 16-23; per-clock perf table at
PMGR 0x200 + n*16 (byte = source 6:5 / divider 4:0). PMGR base 0x3F100000 len
0x7000 (pmgr `reg`).

**The one remaining unknown for a safe attempt:** the actual values to write to
enable MANAGED0/1 (source select + divider = the GPU perf-state), and the exact
handshake. iOS's AppleS5L8940XPerformanceController writes these at runtime; we
cannot read PMGR live (no tfp0). Get them from static RE of the real 8.4.1
kernelcache (decrypts with docs/kernelcache-keys.txt; the in-tree Kernelcache.dec
is 6.1) -- the PerformanceController / ClockController there holds the perf-state
table and the enable sequence. That is the next step, zero device risk.

**Reachability, honest:** a GL "game" from Linux is out -- no open Mesa or kernel
driver exists for SGX543 (Series5XT; mainline drm/powervr is Rogue-only), and the
compiled texture program never even appears in CPU memory (see above). The
realistic triumph is hardware bring-up: clock the GPU, read CORE_ID/revision,
load its ukernel (extractable from IMGSGX543), and ideally drive a hand-built
command stream to a triangle -- using our USSE + command-stream RE, bypassing GL.

## GPU clock bring-up — register-level findings from Linux + kernelcache (2026-09-28)

Booted to Linux, drove PMGR by `peek r/w` over ssh, cross-checked against the
decrypted 8.4.1 kernelcache (AppleS5L8940X kext, PIC-aware capstone). Confirmed:

- **PS-gate registers ARE writable and work.** `clock_gate_switch` (kc @0x80b8de38):
  reg = `PMGR + 0x1000 + idx*4`; recipe `write (v & ~0x10f) | 0xf`, poll until
  bits[7:4]==bits[3:0]. Applied to GFX_SYS-CLK (idx 9, 0x3f101024) and GFX-CLK
  (idx 10, 0x3f101028): both went 0x300 -> **0x3ff (ON)**, no hang. First time the
  GPU clock domain is gated on under Linux. (Map registers with
  `scripts/adt-pmgr-map.py ibootfiles/DeviceTree.raw <peek dump>`.)

- **Clock-config registers (`PMGR + idx*4`, e.g. GFX-CLK 0x3f100070, GFX_SYS
  0x3f100074, MANAGED0/1/2 0x3f10002c/30/34) are READ-ONLY mirrors.** Every
  `peek w` to them is silently ignored (reads back unchanged). So a clock's
  source/divider/enable cannot be set by a direct register write — that was the
  whole reason the naive "re-point GFX to a running MANAGED" failed.

- **The CPU/SoC DVFS state machine is at PMGR+0x2100.** A second kernelcache
  function (@0x80b8deb4) writes 0x3f102100 (domain1) / 0x3f102104 (domain0) and
  polls busy bit16 (0x10000). Live: 0x2100=2 (current state), 0x2104=1; a 16-entry
  voltage/freq table at 0x2120..0x2190 (`00020904 00824024` per state), more at
  0x2010/0x2080/0x21c0. This is CPU/SoC voltage-frequency, **not** the GPU clock.

- **Reading any SGX register (0x35100020) with GFX sourced from a dead MANAGED
  still HANGS the bus** (froze the device, reboot). Confirmed: gating the domain
  on is not enough; a running MANAGED source must actually feed GFX first.

**Clock-reg format (from the touch-clk driver, clk-s5l8940x-pmgr.c):** bit31 =
DISABLE (1 = off), bit19 = enable, bit18 = busy, low bits = divider. So GFX-CLK
0x80000001 has bit31 set = **disabled**; MANAGED3/4 (0x38/0x3c, bit31 set) are
likewise "off" by this reading — i.e. the whole GFX chain is parked.

**Still open — the real next task:** the GPU managed-clock **enable + source
routing** is a subsystem in the base AppleARMPerformanceController kext (kext
__text file 0x461b48, vm 0x804a9b48; disasm scratchpad/gpu2/pc_disasm.txt),
reachable by its strings "routing failed: could not connect %s" (@0x804c18c2)
and "clock conflict (%s vs %s)" (@0x804c1a18) and AppleARMSlowAdaptiveClockingManager
(@0x804bff40). Its control register + write protocol for a managed clock is not
yet pinned (the routing solver is heavy C++ bit-manipulation over in-memory clock
nodes; a Ghidra decompile is the efficient way in). Once pinned: add GFX clocks
to clk-s5l8940x-pmgr.c with the routing sequence, enable MANAGED source, gate on
GFX_SYS/GFX, THEN read CORE_ID. This is genuine driver work, a multi-session arc.

## Ghidra pass on the clock-routing subsystem (2026-09-28)

Headless Ghidra 12.1.2 (analyzeHeadless, ARM:LE:32:v7) on kc841.macho, project
in scratchpad/gpu2/gproj. Decompiled the routing subsystem reached from the
"routing failed" strings:

- FUN_804c1640 (@0x804c1640) is the routing SOLVER: a recursive graph algorithm
  over in-memory clock-node structures (this[0x17]=node array of 0xc-byte
  records, this[0x18]=adjacency, this[0x14]=stride, this[0x15]=mask word count).
  It computes connectivity masks and calls FUN_804c1c6c / FUN_804c1ba0 to apply.
- FUN_804c1ba0 dispatches FUN_804c1c6c with mode 0/1/2 per direction flags.
- FUN_804c1c6c walks the graph via vtable method (*this+0x360) and merges masks.
- FUN_804c1490/14e8 only build OSString names ("M","<","-",">") for the logs.

So this layer is pure software routing logic; the actual MMIO write is one more
level down, behind vtable dispatch to per-clock objects: (*this+0x340) returns a
clock object, and a method on it does the register write. The S5L8940X register
accessors are tiny: readReg = `ldr base,[this+0x550]; ldr r0,[base+off]`,
writeReg = `str val,[base+off]` (base ivar at +0x550; for the base
PerformanceController the base is _pcBaseAddress = PMGR 0x3f100000).

NEXT Ghidra target: resolve (*this+0x340)/(*this+0x360) to the concrete
clock-object class and decompile its enable/setSource method to get the control
register offset + protocol for a managed clock. Then implement in
clk-s5l8940x-pmgr.c. (The routing solver decides *what* to connect; we need the
*apply* register write.) Reusable: the analyzed Ghidra project is cached, so
re-running DecompDump.java with new addresses is fast (-process -noanalysis).

## Ghidra passes 2-4: static RE hits the OOP/runtime-dispatch wall (2026-09-28)

Chased the clock commit through the vtable. The PerformanceController vtable is
at file 0xb48938 (found via ptrs to its methods: readReg=slot 0x490 @0x80b8df5c,
writeReg=0x494, clock_gate_switch=0x488 @0x80b8de38). Slot 0x364 (the commit the
router calls) -> FUN_804b3490, which calls slot 0x440 -> FUN_804b2900. But:

- FUN_804b2900 is NOT an MMIO write -- it records voltage/perf state into in-mem
  tables ([this+0xac]/[0xa8]/[0xb0]) under a spinlock and emits `_kernel_debug`
  events; the `0x27002000 | dom<<4` "selector" is a **kdebug trace code**, and the
  branches panic on "voltage state"/"SRAM EMA"/"performance state" -- this is the
  **CPU/SoC voltage-frequency DVFS path, not the GPU clock**.
- The real hardware apply in FUN_804b3490 is `(*(*(perdomain+0x10))+0x3c)(...)` --
  a **runtime-populated clock/regulator object**, whose class is not statically
  known.
- The clock ROUTER (FUN_804c09a8/1640) is a different class again; Ghidra found no
  RTTI, no data-xref, and no vtable pointer to its methods, so its vtable (and its
  own +0x364 commit) can't be pinned statically.

Conclusion: static decompilation of this framework does not cheaply yield the
concrete "enable managed clock N, route GFX" register write -- the MMIO sits
behind runtime-object vtable dispatch. Switching tactics (per plan).

**Better tactic — dynamic kdebug trace on iOS.** The perf/clock framework TRACES
its own operations via `_kernel_debug` (code group 0x27xxxxxx). kdebug is readable
from iOS userspace WITHOUT tfp0 (sysctl KERN_KDEBUG: KDSETUP/KDENABLE/KDREADTR).
So: on iOS, enable kdebug, run a GPU workload (gltrace), capture the 0x27-class
events -> ground truth of the clock/voltage state machine as the GPU powers on,
including the state indices/args passed to the tracer. This bypasses both the
static-RE wall and the no-tfp0 wall. Alternative (cheaper, in Linux now): sweep
CPU perf-states via the writable setter 0x3f102100/04 and watch whether the GFX
MANAGED sources turn on (long-shot; GPU clock is likely GPU-dedicated, not tied to
CPU perf-state).

## Ghidra pass D: the concrete register recipe (2026-09-28)

Decompiled ALL 44 functions of the AppleS5L8940X kext (text 0x80b8b328-0x80b8f1fc)
via headless Ghidra (scratchpad/gpu2/kext_c.txt). All register access goes through
readReg = vtable+0x490 (`ldr [this+0x550 + off]`) and writeReg = vtable+0x494;
_pcBase (this+0x550) = PMGR 0x3f100000. Found the real writable control (the
0x00-0xff clock regs we saw are read-only mirrors; the control is in the 0x100+
and 0x1000+ regions):

- **FUN_80b8d280 — managed-rail enable/ramp** (the missing piece). For rail idx
  (param_2, 0..6):
    writeReg(0x110 + idx*0x10, level ? 0x90000000 : 0);  poll (v & 0x40000000)==0  // bit30 busy
    // enabling (level != 0):
    writeReg(0x114 + idx*0x10, level*2);        poll +0x110 bit9 (0x200) clear
    writeReg(0x118 + idx*0x10, level*2 - k);    poll bit9        // k from this[0x6b] companion (voltage)
    writeReg(0x110 + idx*0x10, 0x90000000|0x400); poll bit9      // ramp step
    writeReg(0x110 + idx*0x10, 0x90000000|0xc00); poll bit9      // ramp step
  So a managed rail is controlled at **0x3f100110 + idx*0x10** (+0x114/+0x118),
  NOT the 0x2c mirror -- that is why raw peek writes to 0x3f10002c/0x70 were ignored.
- **FUN_80b8cdbc — per-domain clock apply**: domain1 writes reg 0x38 (poll bit30
  0x40000000); domain0 writes reg 0x300 (poll bits 16-23, 0xff0000). Values come
  from tables this[0x201]/this[0x202] indexed by state.
- **FUN_80b8cf58 = _enableDevicePowerGated**: device clock-gate id -> group via
  this[0x81][id], group struct at this[0x159]+group*0x2c holds up to 4 gate ids at
  +0xc; each enabled via clock_gate_switch (vtable+0x488). Panics
  "power gate enabled before clock" if a gate reads (v&0xf)!=0xf while enabling.
- clock_gate_switch (FUN_80b8de38, vtable+0x488): reg 0x1000+idx*4, (v&~0x10f)|0xf,
  poll [3:0]==[7:4]. FUN_80b8d154: bit31 reset pulse on 0x1000+idx*4.
  FUN_80b8d1a0: CPU1 start (0x1214/0x1220/0x1204/0x1210). FUN_80b8d0a0: device
  on/off touching this[0x1ff] + poll 0x2104.

**Still to pin for the GPU:** which managed-rail idx (0..6) feeds GFX and at what
level; whether 0x110-region is the clock or the companion voltage (this[0x6b]).
NEXT (on Linux): read 0x3f100100-0x1ff (the rail region, not read before) to see
current rail states; find the caller that drives the GFX rail (rail<->clock map);
then reproduce FUN_80b8d280's ramp for the GFX rail + clock_gate_switch for GFX,
and only then read CORE_ID. This is now a concrete register sequence, not a
mystery -- the driver work is bounded.

## Ghidra pass A2: the rail path is CPU DVFS; apply the recipe empirically (2026-09-28)

The managed-rail ramp FUN_80b8d280 (vtable+0x478, regs 0x3f100110+idx*0x10) is
called from exactly one site: FUN_804b0cb0 = AppleARMPerformanceController::
initVoltageAndPerformanceStates (reads DT performance-domain-features /
nominal-performance%ld / boost-performance%ld / voltage-states%ld and builds the
per-domain state tables). So the 0x110-region rails serve CPU/SoC DVFS
(voltage+perf domains), and the domain-apply FUN_80b8cdbc (regs 0x38/0x300) is
CPU too. The GPU clock is NOT in the PerformanceController; it lives in the clock
ROUTER/controller class (FUN_804c09a8/1640), whose vtable Ghidra can't recover
(no RTTI/xref) -- static RE of the GPU-clock-source enable stalls there.

BUT the rail CONTROL PROTOCOL is general (any rail idx 0..6), so the GFX MANAGED
source can be brought up EMPIRICALLY on Linux with the extracted recipe:
  1. read 0x3f100100-0x1ff (rail region -- never dumped) to see rail states;
  2. for each candidate rail feeding GFX (GFX sources = MANAGED0..4, mirror ids
     0x18-0x1c): ramp it via FUN_80b8d280's sequence
       w(0x110+idx*0x10, 0x90000000); poll bit30; w(+0x114, lvl*2); poll +0x110 bit9;
       w(+0x118, lvl*2-k); poll bit9; w(0x110.., 0x90000000|0x400); poll bit9;
       w(0x110.., 0x90000000|0xc00); poll bit9;
  3. gate GFX_SYS/GFX (0x3f101024/28, (v&~0x10f)|0xf) -- already works;
  4. read SGX CORE_ID 0x35100020.
The rail<->GFX map + level is the one empirical unknown; resolve on-device (read
region, try, watch, CORE_ID). Static RE delivered the register recipe; the rest
is a bounded on-device experiment.

## GPU clock via kernel observability (2026-09-29): tfp0 + kdebug

The static-only path above stalled because the GPU-clock enable sits behind a
runtime-dispatched clock controller we could neither reverse (no RTTI/vtable in
Ghidra) nor observe (tfp0 believed closed). Both of those turned out to be
solvable. This section is the record of reopening the dead end and how far it got.

### The jailbreak is daibutsu, and it gives tfp0

The device was jailbroken with **Legacy iOS Kit** ("install with jailbreak"),
which for iOS 8.4.1 on a 32-bit device installs the **daibutsu** untether
(kok3shidoll, open source) — *not* TaiG (TaiG never supported 8.4.1; Apple shipped
8.4.1 to kill it). This matters because the tfp0 mechanism is daibutsu's, and its
kernel patches (from `untether/patchfinder.c` / `untether32.c`):

- `find_tfp0_patch` NOPs the conditional branch at the start of `task_for_pid`,
  which removes the `pid == 0` guard — but the posix/entitlement check remains.
- daibutsu's own `ent.xml` carries `task_for_pid-allow` (+ `platform-application`,
  `get-task-allow`), which is the proof the entitlement is *required*, not optional.

So: a plain fake-signed binary gets `KERN_FAILURE` from `task_for_pid(0)`
(correct — no entitlement), and a binary signed with daibutsu's entitlement set
gets the real kernel task port. Verified on-device with `tools/mtdump/tfp0probe.c`:

    task_for_pid(0): OK, port 2563
    pid_for_task(port 2563) => kr=0 pid=0   [CONFIRMED kernel_task]

no panic, device stayed up. The earlier `kmemprobe` conclusion ("tfp0 closed;
entitlement panics") was doubly wrong: the plain binary *should* fail, and the
"panic" was `find_kernel()` blind-scanning kernel VAs, not a tfp0 denial.

**Hard rule learned:** `vm_read_overwrite(kernel_task, VA)` on an *unmapped* kernel
page does **not** return a clean error here — it faults the bus and panics
(reproduced: a bounded scan of 0x80000000–0x84000000 read 0x80000000–0x80080000
fine and then rebooted the device around 0x800Cxxxx). Twice. So **never blind-scan
kernel memory**; read only addresses known to be mapped. Finding the kernel base
therefore needs a non-scanning KASLR-slide source (a leaked kernel pointer via a
Mach/IOKit call) plus the decrypted 8.4.1 kernelcache segment map — not a page walk.
`vm_region_recurse` on the kernel task returns nothing from userspace here (a known
XNU quirk), so it cannot be used to pre-check a read either.

### kdebug: watching the clock/perf state machine without tfp0

`tools/mtdump/kdtrace.c` captures the kernel kdebug ring from userspace via
`sysctl KERN_KDEBUG` (=24): `KDREMOVE / KDSETBUF(nbufs) / KDSETUP / KDENABLE(1) /
wait / KDENABLE(0) / KDREADTR / KDREMOVE`. K32 `kd_buf` is 32 bytes
(`u64 timestamp; uintptr_t arg1..arg5; u32 debugid`); on `KDREADTR` the sysctl
oldlen is bytes in, entry-count out. Modern SDKs deleted the legacy `KERN_KD*` /
`kd_buf` defs from `<sys/kdebug.h>`, so they are inlined in the tool (stable
xnu-2784 ABI). It also has a `raw` mode that dumps the whole `kd_buf` array to a
file; `tools/mtdump/kddec.py` decodes those offline (`hist` / `diff` / `dump` /
`onset`). Device max buffer here is ~198656 entries.

`debugid = (class<<24) | (subclass<<16) | (code<<2) | func`. The
AppleS5L8940XPerformanceController's trace is **class 0x26** (its IORegistry
`TraceBufferNomenclature` names the events — PERF_CLOCK_GATE{ClockID},
PERF_PERF_CHG, PERF_VOLT_CHG, … — but that list is *not* indexed by the kdebug
subclass number, so the labels are not yet pinned to subclasses). The
graphics/IOAccelerator command trace is **class 0x31**.

### Finding the GPU clock domain by diffing idle vs Angry Birds

Method: capture a GPU-idle baseline, then capture with a GPU workload (Angry Birds
6.1.0), then diff `(class,subclass,code)` counts and the event args. Captures used:
`kd_base.bin` (idle), `kd_game.bin` (GPU already on), `kd_trans.bin` and
`kd_wake.bin` (GPU off → wake, i.e. an off→on transition). A "did nothing" run
produced no class-0x31 at all — confirming the GPU stays off in idle.

Class-0x26 event shapes observed:

- `sub=25 code=8` START/END — **perf-state apply**: a2 = a packed freq/voltage
  descriptor (changes START→END, e.g. `0x07001980 → 0x04802180`), a4 = state index.
- `sub=56 code=10` — **clock/power gate apply**: a2 = `[domain<<16 | gateval]`.
  Idle only ever shows domain 0 (a2 in {0x300,0x6300,0x8300,0xa300}).
- `sub=16` / `sub=17` — per-domain voltage/clock *change* events, but they fire on
  the **wake/display** sequence (present in the wake capture, absent during steady
  gameplay), so they are not GPU-specific.

**The result:** `sub=56` gate events whose a2 high byte = **0x50**
(`0x0050a300 / 0x00508300 / 0x00504300`) occur **378× while the GPU is active and 0×
in idle**. So perf-controller **domain 0x50 is the GPU clock domain**, and its gate
values are `0xa300/0x8300/0x4300`. (This domain-index space is the controller's own;
it does not map to the ADT clock-gate ids 0x1e/0x1f/0x5c.)

### The limitation, and the two routes for the last mile

Two things kdebug does **not** give us:

1. **No one-shot "enable" event.** In every capture the first class-0x31 graphics
   event and all domain-0x50 gates come *after* graphics has already started — the
   GPU is clocked at the instant of power-on, before the perf controller emits any
   0x26 event, and because the GPU clock is **fixed** (SGX `CurrentPowerState=1 /
   MaxPowerState=1`, no GPU DVFS) there is no distinct enable event to catch — only
   ongoing gate management of the already-running domain.
2. **No raw register values.** kdebug shows the controller's abstract view (domain
   0x50, gate value 0xa300, state index), not the PMGR `source+divider` the Linux
   driver must write.

So the last mile is translating `domain 0x50 / state index` → a real PMGR register
write, via one of:

- **Route A (offline, no device):** map the perf-state index (`sub=25 a4`, e.g.
  0x1f) through the static perf-state table at `PMGR 0x200 + n*16` already dumped on
  Linux. Caveat: n=0x1f exceeds the 16 rows we dumped, so first confirm the table's
  size/identity.
- **Route B (direct, uses tfp0 — the ground truth):** read the live GFX/MANAGED
  clock registers (`0x3f100070/74`, `0x3f10002c/30`) *while the GPU is on*; they hold
  the real enabled `source+divider` then, unlike on Linux (GPU off → "disabled"
  mirrors). Prerequisite: the PMGR mapping's kernel VA, found *safely* (no blind
  scan) — via a non-scanning slide-finder + the kernelcache segment map, or by
  reading the AppleS5L8940XPerformanceController driver object's `_pcBaseAddress`
  ivar.

### Tooling and artifacts

- On device (armv7, daibutsu iOS 8.4.1): `tools/mtdump/tfp0probe.c` (+ signed with
  `tools/mtdump/tfp0.entitlements`), `tools/mtdump/kdtrace.c`. Both build via
  `tools/mtdump/build.sh` (Xcode + ldid; the entitled tfp0probe is a separate
  `ldid -Stfp0.entitlements` step). `tools/mtdump/ioprops.c` located the perf
  controller (class `AppleS5L8940XPerformanceController`, provider `AppleS5L8940XIO`).
- Host-side offline decoder: `tools/mtdump/kddec.py`.
- The raw kdebug captures (~6 MB each) are **not** committed (like Apple firmware,
  they do not belong in the tree); they live in the working session's scratchpad.
- Deploy/run went through the Arch box's usbmuxd/iproxy (the Mac's usbmuxd resets
  bulk transfers); the device has no `head`/`wc`/`grep`, so filter output host-side.

## The last mile: Route A ruled out, Route B is the only path (2026-09-30)

The goal is the raw value the Linux clock driver must write to enable the GFX
clock — the `source+divider` for the GFX/MANAGED clock chain. This session settled
which of the two candidate routes can actually produce it.

### Route A (read Linux-side static registers) — DOES NOT contain the value

Peeked the PMGR on the Linux boot (`peek r`, read-only, safe):

- **Clock config `0x3f100000..0x7f`** matches the earlier dump exactly. Crucially
  `GFX-CLK 0x3f100070 = 0x80000001` and `GFX_SYS 0x3f100074 = 0x90000002` — the GPU
  is **off**, so the mux registers that would carry the enabled `source+divider`
  read as "disabled". The GFX power-state gates `0x3f101024/28 = 0x300` (off) too.
  MANAGED sources, for reference: MANAGED0(`0x2c`)=`0x00010960`,
  MANAGED1(`0x30`)=`0x00000008`, MANAGED2(`0x34`)=`0x40000000`,
  MANAGED3(`0x38`)=`0x90011041`, MANAGED4(`0x3c`)=`0x90000001`; the running clocks
  (PREDIV0/2/6, HPERF, …) have high nibble `0x9`/`0xa`.
- **The perf-state table at `0x3f100200` is CPU/SoC DVFS, not the GPU.** Rows
  `0x220–0x260` decode as multi-clock CPU states (`byte = source[6:5] / divider[4:0]`,
  e.g. row `0x230` word0 byte `0x43` → src 2 / div 3, `0x24` → src 1 / div 4);
  rows `0x270–0x2f0` are filler `0x01010101`. The GPU has a **fixed** single clock
  (SGX `CurrentPowerState=1 / MaxPowerState=1`), so it is not represented in a
  multi-state DVFS table. The earlier hope that the kdebug perf-state index `0x1f`
  (`sub=25 a4`) indexes a GPU row here was a false lead — `0x1f` at `0x250` is a CPU
  state.

Conclusion: no Linux-readable static register holds the GPU's enabled clock value.
It exists only in the live PMGR while the GPU is running — i.e. Route B, on iOS.

### Route B (read live PMGR on iOS via tfp0) — the plan, and why it needs a primitive

`tools/mtdump/pmgrread.c` (entitled) does: `task_for_pid(0)` → kernel task (works);
`IOServiceGetMatchingServices("AppleS5L8940XPerformanceController")` → the perf
controller service; `mach_port_kobject(mach_task_self(), io_object, &type, &addr)`
→ the object's kernel address. That returned type `0x1e` (`IKOT_IOKIT_OBJECT`) and
addr **`0xb9ef1771`** — but that address is odd/unaligned, i.e. it is
`VM_KERNEL_ADDRPERM`-permuted (xnu-2784 adds a per-boot constant to kobject
addresses before handing them out). `vm_read` of it hit an unmapped page and
rebooted the device (recovered fine).

The clean shortcut to a usable address is also gone: `tools/mtdump/kasinfo.c` shows
`kas_info(KAS_INFO_KERNEL_TEXT_SLIDE_SELECTOR)` returns **`ENOTSUP` (errno 45)** on
this kernel, via both the libc wrapper and raw syscall 439, despite daibutsu setting
`PE_i_can_has_debugger`.

So Route B needs a real **safe-kernel-read primitive** first:

1. Leak one real (unpermuted) kernel address from userspace — `sysctl KERN_PROC`
   → `kinfo_proc.kp_eproc.e_paddr` (this is the linchpin, and it is a safe,
   read-only probe; on some iOS 8 builds this field is zeroed/permuted, so it must
   be verified before relying on it).
2. Read `proc->task` (offset from the decrypted 8.4.1 kernelcache) to get the real
   task address, and `mach_port_kobject(mach_task_self(), mach_task_self())` for the
   *permuted* task address → their difference is `vm_kernel_addrperm`.
3. Un-permute the perf controller (`0xb9ef1771 − addrperm`), read `_pcBaseAddress`
   at `+0x550` (its register base, per the AppleS5L8940X Ghidra work) = the kernel VA
   that maps PMGR, then read `GFX-CLK`/`GFX_SYS`/`MANAGED*` through it **with the GPU
   powered on** — those are the enabled `source+divider` we want.

Bounded but reboot-prone: a wrong offset yields an unmapped VA, and unmapped kernel
reads fault the bus here (they do not return an error), so each mistake reboots.
Reads of the leaked-real proc struct and of the (correctly) un-permuted object are of
mapped kernel heap and are safe; the risk is concentrated in getting the two offsets
right. Next step is the safe step 1 probe (does the leak yield a real address?),
then build the rest only if it does.

### This session's tools

- `tools/mtdump/pmgrread.c` — the two-step live-PMGR reader (`obj` finds the
  controller object + base VA; `pmgr <VA>` reads the clock region / perf table /
  gates). Sign with `tools/mtdump/tfp0.entitlements` (needs tfp0).
- `tools/mtdump/kasinfo.c` — the `kas_info` slide probe (result: unavailable here).
- Both build via `tools/mtdump/build.sh`; new stub symbols added to
  `tools/mtdump/stubs/libSystem.tbd` (`_mach_port_kobject`, `_kas_info`, `_syscall`,
  `___error`).

## iBoot/iBEC clock RE: the PLL formula, 200 MHz, and the write mechanism (2026-09-30)

Attacking the clock from iBoot (linear C, no IOKit OOP wall) instead of the iOS
kernel paid off. `build/firmware/iBEC.dec` (iBoot-2261.30.37 for p105, base
`0x9ff00000`, Thumb-2, absolute literals) programs the SoC clocks at boot with
straight-line code. Tooling: capstone Thumb, VA = `0x9ff00000 + fileoffset`.

**The PLL frequency formula** (iBEC's clock-frequency getter, linear code):

    freq = 24 MHz * M / P / 2^S       M = bits[12:3], P = bits[19:14], S = bits[2:0]

reference 24 MHz (literal `0x016e3600`); early-out if `(reg & 0x40800000)` (off/
bypass). Root/PLL registers: `0x3f100008 / 0x10 / 0x18 / 0x20`. Applied to the
Linux-side register values this gives, decisively:

    0x3f100010 = 24e6 * 200 / 6 / 2^2 = 200.00 MHz   <-- exactly the SGX543 GPU clock
    0x3f100018 = 513 MHz,  0x3f100028 = 240 MHz

So there is a **200 MHz PLL at `0x3f100010`**, and the GPU (200 MHz) runs off it
through a MANAGED clock at divider 1. This is the clock *value* by computation —
no live read of iOS needed.

**The clock write mechanism** (iBEC `set_clocks(start,end)` @ `0x9ff1f4d0`):

    for idx in [start..end]:
        *(0x3f100038 + idx*4) = SOURCE_TABLE[idx]     # write config
        while (*(0x3f100038 + idx*4) & 0x40000000) {} # poll bit30 (busy) until clear

i.e. **write the config word, poll bit30 clear**. The register index maps
`reg = 0x3f100038 + idx*4`, so `idx 0x0e -> 0x70` (GFX-CLK), `idx 0x0f -> 0x74`
(GFX_SYS). The PLLs are enabled by a sibling routine that writes the PLL regs and
polls **bit30 for LOCK**. These `0x3f100038..0xcc` registers are writable in
iBEC's context.

**But iBEC parks the GPU off.** `SOURCE_TABLE` (`0x9ff44448`) holds iBEC's boot
value for every clock: GFX-CLK and GFX_SYS are both `0x80000000` = parked/disabled
(matching the Linux read `0x80000001`). Running clocks in the table are
`0x80000001`, so for these PMGR mux clocks **bit31 = 1 is the normal/active state,
not "disable"** — the on/off is the separate power-state gate at `0x3f101024/28`
(the touch-leaf format bit31=disable/bit19=enable does *not* apply to these).

**Net.** We now have the PLL formula (decode any clock), the 200 MHz GPU source,
the register write mechanism, and iBEC's full boot value table. What remains for
actually clocking the GPU from Linux: (a) the *enabled* GFX source+divider — iBEC
parks it, iOS sets the real value, so it is still only directly observable live
(Route B); computation says source = the 200 MHz chain, divider 1; (b) whether the
`0x38..0xcc` config registers are writable *from Linux* (an earlier peek-write to
`0x70` was ignored — likely locked after iBEC hand-off, or that test was flawed;
must be re-tested carefully on-device, and it is reboot-prone); (c) the mux
source-select bit position + the iBEC-index→clock map to confirm GFX→PLL@0x10.
The concrete resumption step (do it with a human present, it can reboot the pad):
on Linux, re-test writing a benign clock reg with the real mechanism (write + poll
bit30); if writable, enable a 200 MHz MANAGED source, write GFX-CLK (source = the
200 MHz chain, divider 1) + open the gate, then read CORE_ID at `0x35100020`.
Tooling: `ibectool.py` (scratchpad) for iBEC; `kc841.macho` + `kctool.py` for the
decrypted 8.4.1 kernelcache.

## The GFX clock tree, fully decoded from the live 1537 iBEC (2026-09-30)

> Correction (same day, top of this file): the root indices here are off by
> one. GFX hangs off the running PLL@0x18, not PLL@0x20.

The boot chain we actually run is **iBoot-1537.9.55** (6.1.3), not the 2261 (8.4.1)
iBEC the earlier RE used. Both are the same silicon (S5L8942X), and the 1537 iBEC
carries the identical clock machinery — verified: the 12-byte clock descriptor
table (`reg`, `f2`, `parents`), the PLL frequency getter, the `set_clocks` apply
primitive, and `SOURCE_TABLE` (boot values, here at `0x9ff4340c`). This session
decoded the *frequency-reporter* (`freq_of_root` at `0x9ff1f90c` + the tree walker
at `0x9ff1f984`) in full, which is the ground truth for the register bit layout,
and validated it against the live Linux PMGR dump (`logs/pmgr-map.txt`).

### The register formats (ground truth from the walker)

- **PLL/root freq** = `24 MHz × M / P / 2^S`, `M=bits[12:3]`, `P=bits[19:14]`,
  `S=bits[2:0]`; `bit31=0` → disabled, `bits & 0x40800000` → bypass (24 MHz).
- **A mux/derived clock's source-select** = `(reg >> shift) & 3` (a 2-bit index into
  that clock's 4-entry `parents` list). `shift` is per clock *type*: the default
  type uses `shift=28` (source = `bits[29:28]`); GFX-CLK's type uses `shift=29`
  (source = `bits[30:29]`).
- **Divider** = `(reg & mask) >> dshift`, from a per-type table at `0x9ff41754`
  (divcode 1 = `bits[4:0]`; divcode 7 = `bits[28:24]`). GFX-CLK is divcode 7.
- Parent indices in a `parents` byte are **clock indices** into the tree array
  (`entry = 0x9ff4143c + idx*12`); indices 0–6 are the roots, filled by the PLL
  getter: **idx0→reg0x00, 1→0x08, 2→0x10, 3→0x18, 4→0x20, 5→0x28**.

### The root PLLs, computed from the live dump (clean numbers = decode is correct)

| root | reg | live value | frequency |
|---|---|---|---|
| 0 | `0x3f100000` | `a001a7d1` | **500 MHz** |
| 1 | `0x3f100008` | `40000000` | off |
| 2 | `0x3f100010` | `a001a642` | **200 MHz** |
| 3 | `0x3f100018` | `a0012559` | **513 MHz** |
| 4 | `0x3f100020` | `40000000` | **off** |
| 5 | `0x3f100028` | `a0012502` | **240 MHz** |

### GFX-CLK's tree — and the correction

`GFX-CLK` is config reg `0x3f100070`, clock-index `0x18`, set-clocks index `0x0e`.
Its four selectable parents (`parents = 0x11100f0e` → clock indices `0e,0f,10,11`)
are the clocks at regs **`0x48, 0x4c, 0x50, 0x54`**. Each of those is a default-type
mux with `parents = [0,3,4,5]`, and in every live (parked) state each one's own
source-select points at **root 4 = PLL@`0x3f100020`** (with dividers ÷5, ÷2, ÷3
respectively). So the entire GFX source subtree hangs off **PLL@0x20, which is
powered down on the Linux boot** — that, concretely, is why the GPU has no clock.

**This corrects the earlier "200 MHz PLL@0x10 is the GPU clock" note.** That was a
coincidental value match: PLL@0x10 *is* 200 MHz, but it is **not reachable** from
GFX-CLK (root 2 is not in any GFX parent's `[0,3,4,5]` list). The GPU is fed by the
dedicated **PLL@0x20**, currently off. iBEC's own nominal template for GFX-CLK
(hard-coded in the reporter: `0x01220001`, decoded with its type = source 0 → the
`reg0x48` tap, GFX divider 1) means **GFX = PLL@0x20 ÷ 5 ÷ 1**; for the SGX543's
200 MHz operating point that implies **PLL@0x20 ≈ 1000 MHz**.

### What is and isn't recoverable statically

iBEC computes the PLL configs it programs with a solver (they are not stored as
literals) and it **never programs PLL@0x20** — it leaves the GPU PLL off, because
the GPU is only clocked once iOS's driver runs. So PLL@0x20's exact operating M/P/S
(the last scalar: is it exactly 1000 MHz? what enable value?) is **not present in
iBEC** and not in the Linux dump (root 4 off in both). It exists only in the live
iOS PMGR with the GPU on (Route B), or inside the iOS GPU/PMGR driver in the
kernelcache (behind the runtime-dispatch wall; a raw-constant scan of `kc841` did
not isolate it). What *is* now fully solved and reusable: the complete GFX clock
tree, every register's source-select/divider bit layout, the root map with
validated frequencies, and that **bringing up the GPU requires turning on and
programming PLL@0x20 (≈1000 MHz), then routing GFX-CLK (source 0 → reg0x48÷5,
GFX÷1) and opening the `0x3f101024/28` gate** — the PLL-enable being the same
HW-locked operation that must be done in iBEC context or via the clock router.
