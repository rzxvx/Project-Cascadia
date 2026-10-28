# P105 boot CDMA entry and initialization — bounded original evidence

No iPad, MMIO, NAND, firmware upload or host device operations were performed.
All native instructions execute in process-local Unicorn RAM. This group uses
original 12H321 iBSS/iBEC, the bootloader profile Cascadia currently builds.
It does not transplant those offsets into iOS9.3.5's 13G36 bootloaders.

Pinned original plaintext inputs:

| Image | SHA256 | Linked base / entry |
| --- | --- | --- |
| iBEC12 | `30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec` | `0x9ff00000 / +0x40` |
| iBSS12 | `67d8f8d6fa82e220f436233e89ba0d411b948d5d5aeee1289e2a0d268b03bfb5` | `0x34000000 / +0x40` |

## ARM entry is memory initialization, not a controller reset

`emulate-entry.py` verifies 14 exact instructions/literals and executes 12 actual
original ARM entries: each image in place or loaded at linked base+2MiB, with
initial BSS bytes0/A5/FF. Source relocation, data relocation checks, banked stack
setup and BSS clearing execute, ending before the first Thumb-main instruction.
Writes are restricted to destination image/BSS; executed code is restricted to
original vector/entry. Inherited CDMA registers are only Fake RAM, seeded with
global active bits5..8 and each CONTROL's raw busy condition0x10000. Any attempted
access to those registers is fatal. Every entry clears its BSS and leaves these
synthetic hardware observations unchanged. This is a software counterexample to
using cleared software state as a hardware handoff witness, not a measurement of
what real hardware retains across a processor/BootROM reset.

| Entry detail | iBEC12 | iBSS12 |
| --- | --- | --- |
| Initial linked-image copy end | `0x9ff44840` | `0x340123c0` |
| BSS begin / end | `0x9ff44840 /0x9ff535e8` | `0x340123c0 /0x34014144` |
| Thumb main | `+0xab0` | `+0x8b8` |

The native iBEC CDMA software table begins at `0x9ff45190`, stride0x58, inside
this cleared BSS. The preceding cancel group demonstrates that a software phase0
skips hardware cancellation. Therefore first-use initialization cannot qualify
an unknown inherited DMA lease. In-place versus relocated is a memory-loading
control; neither is labeled a physical warm/cold boot or reset scenario.

## Qualified original call path to CDMA clock enable

`emulate-init-graph.py` verifies 10 exact instruction/literal assertions and
executes 90 main-prefix/early-init/CDMA-init/WMR-gate paths. The main prefix and
both early-init bodies execute natively, as do CDMA-init and the exact WMR gate
body. Earlier unrelated callees are explicitly logged unexecuted boundaries,
with return0/negative-error controls. Their bodies, side effects and error
contracts remain outside this qualification; this is not evidence that the whole
platform graph lacks reset operations.

| Original path detail | iBEC12 | iBSS12 |
| --- | --- | --- |
| Main→early-init call | `+0xabe → +0x1e09c` | `+0x8d6 → +0x78a0` |
| Early-init→CDMA-init call | `+0x1e0d8 → +0x1c38` | `+0x78f6 → +0x224c` |
| CDMA-init→WMR gate tail-call | `+0x1c4a → +0x1f1ec` | `+0x225e → +0x8dd0` |

Both CDMA-init leaves only emit the diagnostic identifying `cdma_init`, then
request WMR gateID28/on1. This WMR namespace has base`0x3f101008`, so ID28 binds
exactly to `0x3f101078`, matching CDMA's separate native ADT power-gate index30.
ADT logical CDMAID85 and IOPID86 are not WMR arguments. IOP WMR gate29 maps to
`0x3f10107c`; the separately proven AP ARST reset-index22 maps to`0x3f101058`.
None of those namespaces are interchangeable.

The actual WMR helper preserves all other word bits, writes low nibbleF for on,
and polls until low nibble equals bits4..7 through its actual XOR/TST loop.
The Fake gate model supplies immediate, delayed or missing acknowledgement.
Immediate/delayed cases return; missing acknowledgement reaches our bounded
instruction stop, not a successful handoff. Native WMR code provides no local
clock-progress/timeout check. The CDMA-init closure accesses no CDMA CONTROL,
queue, IRQ, IOP core or reset register. Gate acknowledgement has no proven
outstanding-bus/DMA/IRQ drain semantics and yields no fence receipt.

## iBSS security checks are not a warm/cold classifier

Native iBSS early-init first calls +0x8388, reading`0x3f500004 >>31`, then tests
`0x3f10a000 &1`. Other combinations reach the original fatal diagnostic boundary,
whose text identifies unlocked fuses/SecureROM. The valid combination proceeds
to the CDMA-init call. Our fixtures execute these reads/branches with synthetic
values, covering all four combinations. They do not identify register semantics
beyond the actual instructions and diagnostic, modify fuses, bypass security or
classify physical warm/cold state. Main, record-byte0/1/FF and explicitly unexecuted
callee return values are additional controls, not real boot observations.

## Static investigation scope and open edges

`inspect-boot-cdma.py` and `inspect-caller-graph.py` list candidate literal/branch
references. Unqualified candidates and halfword scans must not be promoted into
executed call paths; the two emulator groups above qualify only named scopes.
Private sequential disassembly identifies the bulk WMR helper at iBEC+1f440 /
iBSS+8e20. Its off path complements the input bitset, so it cannot be assumed to
modify only set input bits. Its native callers are in other clock-profile paths,
not the qualified CDMA-init leaf; execution through full earlier callees remains
separate work. The private disassemblies are not intended for public export.

The actual inherited state after this unit's kDFU/BootROM/iBSS/iBEC path is unknown.
BootROM/full reset behavior, CPU1/IOP core state, live CDMA master/queues,
outstanding transactions, shared clock ownership and native IRQ/callback drain
are not established here. The Cascadia EOF hook calls cache flush then payload
BLX; that narrow trampoline and its AP cache/MMU teardown are not a CDMA/IOP
handoff. No absence of an active IOP, no DMA fence and no NAND durability may be
inferred from this group. Existing checked cancel/completion modules remain
unchanged and unbound.

## Reproduction

Use the existing bounded image `cascadia-firemarco:a9bf28f`, readonly `/code`,
readonly original plaintext inputs at `/input/ibec` and`/input/ibss`, and a new
private writable `/out`; no device mounts and networknone. Run:

```
python3 /code/emulate-entry.py --ibec /input/ibec --ibss /input/ibss --output /out/entry-traces.json
python3 /code/emulate-init-graph.py --ibec /input/ibec --ibss /input/ibss --output /out/init-graph-traces.json
```

Entry has per-run500,000 instructions/5second bounds (largest legitimate path
copies the original image); init-prefix calls have3,000 stepping calls and5,000
executed-instruction bounds. Missing gate ACK is expected bounded-stop. No
unexpected native exception or register access is converted into success.

The root independently repeated both frozen groups,24 exact assertions,12 actual entries and90 prefix/gate paths. Normalized outputs match exactly. Private logs: `logs/night/boot-entry-root-repeat-v1.log` and `logs/night/boot-init-root-repeat-v1.log`. No firmware bytes are distributed.
