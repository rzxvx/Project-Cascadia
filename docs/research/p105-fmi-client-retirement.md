# NAND software retirement before the iOS 9.3.5 IOP handoff

The pinned official p105ap 13G36 kernelcache closes another prerequisite for a
future Linux NAND backend: AppleIOPFMI blocks new clients during inactivation,
waits for the final client release, and then acknowledges its power transition.
For asynchronous requests, its final IOP completion returns the command to the
pool and releases that client lifetime. Partial completions retain the client.
The client, pending-command, power-reference and BCH-reference counters are
separate; a Linux adapter must preserve that distinction.

80 bounded original-code runs in process-local Fake RAM passed. No iPad or NAND
was accessed. Failure controls show that software retirement may proceed despite
provider power/clock or timeout-context callback failure. The original acquire
leaf has no native timeout, and invalid zero-count release wraps its counter.
The IOPEndpoint-to-sleep threshold uses an atomic operation returning the old
counter: its checked transition is 3 to 2, not a zero-endpoint proof.

This evidence establishes software ordering, not physical DMA/IRQ quiescence or
NAND durability. A future backend still requires a transfer-specific receipt for
exclusive IOP/channel ownership, DMA no-future-access, stale/current IRQ fencing,
and verified halt/disable completion. The current isolated experiment remains
unbound and refuses to convert callback success or a power acknowledgment into
such a receipt. No MTD writer is enabled.

Input SHA256: `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`.
Exact numeric callsite evidence and reproducible private assertions are recorded
separately; no Apple firmware bytes are part of this note.

Independent root repeat exactly matches the frozen80-case JSON. Actual vtable/symbol bindings and the old-value atomic threshold were independently rerun. No source candidate can convert this accounting into a physical DMA fence.
