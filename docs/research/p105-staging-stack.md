# Reserve the staging loader stack below the kernel

The actual bundled loader executes at `0x80000000`; its kernel starts at
`0x80008000`. Before this correction, `_start` loaded SP=`0x8000f000`, placing
normal descending C/assembly stack writes inside the bundled zImage at offsets
below `0x7000`. A successful existing boot is not proof this overlap is harmless
for every compressed-kernel configuration. No observed hardware boot failure is
claimed here.

The linker now owns a dedicated8KiB NOLOAD stack at `0x80006000..0x80008000`.
Assembly takes SP from the linker symbol `__stack_top`; descending writes stay
below kernel entry. The bundle's existing zero-padding already fills this RAM,
so no load or kernel address changes. NOLOAD reserves ELF memory without adding
stack bytes to `linux-boot.bin`. A linker ASSERT refuses loader growth past
`__stack_bottom`; this checks both loaded code/data and the existing embedded
BSS layout before a bundle can be produced. No alternate bootchain is introduced.

`tests/test-staging-stack.py` compiles the actual C/assembly/linker sources into
ARM ELF with a synthetic DTB address, inspects the real NOBITS stack section and
symbol ranges, and runs original compiled entry/cache-function prologues in
Unicorn fake RAM. Current prologues preserve a full kernel canary and write only
inside the reserved stack. A separately compiled historical SP literal overwrites
the same canary. A deliberately enlarged loader fails the actual linker ASSERT.
Four test groups PASS. Watchdog addresses are ordinary mapped fake RAM; no device,
MMIO, boot, flash or firmware image is used.

This validates placement and selected real prologues; it is not a full CPU/cache,
physical boot or runtime stack-exhaustion proof. A new loader/bundle build and
content verification are required after this change; the kernel and DTB inputs
remain unchanged. Diagnostic loaders with different purposes/layouts are outside
this fix.

The CLI bundle stage also now sets `pipefail` inside its container shell. A
failed linker/builder previously yielded the successful `tail -5` status, letting
later checks see stale artifacts. `tests/test-bundle-build-pipeline.py` runs the
actual pipeline string with a failing local builder: exit73 survives in the
fixed command; the historical command returns0. No container, compiler or
hardware action is performed by that regression test.
