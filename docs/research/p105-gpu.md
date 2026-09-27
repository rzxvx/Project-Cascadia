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
