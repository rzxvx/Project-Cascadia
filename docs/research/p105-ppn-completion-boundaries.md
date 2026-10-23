# PPN address/cache/completion boundaries

Research date: 2026-10-12. Static follow-up to
[the 12H321 field evidence](p105-ppn-error-list-evidence.md), using the same
hash-pinned local kernelcache and IOP image. No firmware execution/emulation,
device access, MMIO or ioctl took place. The private fact verifier passed
117 earlier plus 45 new instruction/import assertions. Firmware bytes and
complete instruction inventories remain outside the repository.

## Completion errors call panic, not an ordinary logger

AppleIOPFMI's imported Thumb stub at AP VA `0x80c580ec` constructs a pointer
slot at `0x80c5b0bc`. That slot contains `0x8001ea31`; the actual kernel's
LC_SYMTAB/nlist resolves the target to `_panic` at `0x8001ea30` (Thumb bit
removed). This is an import resolution, not a guess based on diagnostic text.

After the endpoint call at `0x80c55d90`, a nonzero result follows the branch
to a panic call at `0x80c55d98`. The later normal return-zero instructions
do **not** prove that a failed submission is merely logged and reported as
success. Similarly, the event handler's not-pending, null-command and
null-context cases call the same panic import before the syntactic fallthrough
(`0x80c51184`, `0x80c5124a`, `0x80c51266`). This corrects the provisional
ordinary-logging interpretation in the private ownership analysis.

Apple's related public
[XNU 2782.40.9 panic implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/osfmk/kern/debug.c)
has a debugger-dependent `return_on_panic` exception. That is a different
revision from this kernel's 2784.40.6 and is context only: neither an
unconditional no-return property nor a normal recovery contract is inferred
for the inspected binary. Buffer release, pending-count reconciliation,
stale-event defense and ownership on every error path remain unproven.

## Translation validates a start address, not a complete span

IOP helpers `0xb54c` and `0xb5b8` call mapping lookup `0xb568`, which compares
the supplied address against either of two bases and a length in 16-byte
mapping entries. They return translated base plus relative offset, or `-1`.
The observed lookup receives an address, not the complete descriptor length.
It does not establish that the end of `R`, data-list or meta-list stays
inside the selected mapping. Alias domains and mapping lifetime remain open.

The write wrapper separately rounds descriptor/list lengths up to 32 bytes,
using add-31 then mask, before calling `0xaf6c` with selector 3 and later
selector 1. That helper branches to `0x1f18`, which has a distinct zero-length
path. Thus zero length must not be classified as a proven harmless no-op.
No clean/invalidate meaning is assigned to individual selector values here.

A future bounded host view would have to check the full supplied span and
rounding arithmetic independently: exact u32 inputs, no wrap at `length+31`
or `base+length-1`, no ambiguous overlapping mappings, and no pointer
dereference. Such checks would validate a synthetic model only. The command
union/opcode ABI, complete descriptor layout, coherence and hardware lifetime
are not sufficiently recovered for a generic raw C/R parser or DMA driver.

## Next normalized analysis step

The existing [synthetic decoder](p105-ppn-synthetic-decoder.md) can retain
several independently copied error-list observations for one explicit caller
request snapshot. Each observation needs its own opaque CE, raw context
status, and caller-provided generation annotation. That annotation is not
a recovered firmware token and cannot prove actual AP/IOP ownership.

Unknown pages and foreign CE remain errors. Reused scratch must be copied
before the next query. A page appearing in several lists must keep several
raw observations; their labels do not prove contradiction, OR semantics,
retry, retirement or successful programming. A requested page absent from
one or all lists remains **unknown**: partial/empty lists do not establish
success or durability. No acknowledgement, recovery write or NAND integration
follows from this analysis.
