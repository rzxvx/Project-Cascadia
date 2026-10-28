# Strict-build declaration diagnostics

An isolated complete-link qualification of the proposed MTD lifetime extension
first applied `KCFLAGS=-Werror` to the whole existing kernel. This exposed
pre-existing Apple bring-up diagnostics outside the MTD change. The affected
MTD driver/core had already passed actual Linux6.12 ARM object builds with
`-Werror` and the proposed MTD header.

The minimal normal-source changes are:

- Remove one unused local `extern bool apple_aic1_early_irq_escape` from the
  patched `kernel_init`. The actual global definition, assembly loads and IRQ
  users remain unchanged. The updated main source was reconstructed exactly
  from pinned Linux plus the patch and compiled under `-Werror`.
- Declare the existing `void(void)` helpers `apple_a9_periph_quiesce` and
  `apple_a9_gic_drain` in shared `asm/apple_boot.h`, included by `apple.c`.
- Mark the preserved, static noinline alternate-stack experiment
  `apple_spawn_on_alt_stack` as `__maybe_unused`. No function body, linkage,
  interrupt operation, stack instruction or MMIO operation is removed.

The original `apple.c` reproduces exactly the two missing-prototype errors and
the unused-function error with global `-Werror`. It compiles under the saved
normal flags. The updated source compiles without diagnostics with `-Werror`;
the complete original-normal and updated-strict ELF objects are byte-identical
for the tested ARM compiler/configuration. Root independently reproduced the
object comparison. This is stronger than a source-only claim about unchanged
function bodies, but does not establish physical boot or interrupt behavior.

Further unmodified bring-up code still has missing-prototype warnings, including
PMCCNTR, framebuffer-debug and clockevent helpers. Full-kernel qualification uses
the project's normal compiler flags and preserves those diagnostics rather
than pretending the entire existing kernel passes global `-Werror`. Changed
MTD and the selected boot objects have independent strict compiler checks.
Review the complete build log before claiming a full link. A declaration cleanup
does not establish NAND ownership, writable storage, charging or external VBUS.

The qualification uses a separate caller-owned source tree and no embedded
initramfs. Its ELF/zImage is not a RAM-test bundle. The previously verified
normal RAM image remains preserved and still requires a separately authorized
device session; no device access occurred during these checks.
