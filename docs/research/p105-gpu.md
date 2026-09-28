# P105 GPU (SGX543MP2) — powered, never clocked (2026-09-27)

The GPU is off when Linux starts. Its two power domains switch on cleanly; the
first read of an SGX register after that hangs the bus, and the iPad freezes
hard (no ping, no ACM console; power + home to reset). Nothing past that point
is known. This note keeps everything there is, so that whoever picks it up next
starts here and not at zero. The same GPU family sits in the iPad 2 (SGX543MP2,
same A5), the iPad 3 (SGX543MP4) and the iPad 4 (SGX554MP4), so much of this
may carry over.

Probe: `tools/sgx-probe.sh` (reads, plus the two power-domain writes; it stops
before the SGX read unless `SGX_READ=1`).

## STATUS 2026-09-28: clock bring-up is a DEAD END (for now)

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
