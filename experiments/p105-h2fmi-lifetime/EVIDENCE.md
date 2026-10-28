# H2FMI read-only MTD context lifetime

This group changes software lifetime management only. It has no program/erase
callbacks, no new NAND opcodes, no hardware binding, and no device tests.
The normal read-only page/status geometry and allowlist are preserved.

## Actual source counterexample

The preserved H2FMI baseline allocates the context, names and page buffers with
provider devres. Its remove calls MTD unregister, then platform driver teardown
releases devres. Linux 6.12 `del_mtd_device()` does not reject live clients.
`__get_mtd_device()` retains the master MTD kref only with
CONFIG_MTD_PARTITIONED_MASTER=y, and neither configuration retains the
provider's devm allocation. Built-in module refs do not retain those allocations.
An existing MTD handle can therefore refer to retired driver memory/MMIO after
remove or a later die's probe failure.

A second edge remains if only client refs are added: `get_device(&mtd->dev)` can
defer the actual `mtd_release()` beyond MTD unregister. Actual
`mtd_device_release()` calls `device_unregister()` and then clears a master's
embedded dev. An external device reference neither retains driver devres nor
necessarily pins a driver/module callback. The new opt-in is consequently limited
to built-in MTD core and built-in providers, and cannot re-register a retired
master. Ordinary MTD drivers keep their existing path, including no new master
resolution in their final device callback.

The historical controls execute exact preserved H2FMI and Linux MTD functions.
Their fake devres backend marks original allocations retired and quarantines
backing memory so the test can inspect the loss of ownership safely. This is an
actual-control-flow lifetime counterexample with a modeled device/devres backend,
not a kernel UAF observed on hardware or an ASan detection of an original UAF.
Candidate allocations really free through their actual destructor, under ASan
and UBSan. There are no physical hardware calls.

## Candidate contract

* H2FMI owns regular-memory context/names/page buffers with an initial probe ref.
  Its _get_device/_put_device callbacks retain/release that context for MTD
  clients, including partition clients; provider/MMIO availability is separate.
* Every published opt-in MTD device retains an additional context ref before
  device_add. Its actual final mtd_release caches master/callback before possible
  partition freeing and releases the context last. Thus an external device ref
  also retains all memory the delayed core callback needs.
* The core opt-in requires IS_BUILTIN(CONFIG_MTD), owner=NULL, and both callbacks
  before publication. H2FMI's MODULE probe explicitly refuses before allocation.
  External device refs cannot safely pin unloadable callbacks implicitly.
* For an opt-in master, mtd_device_release skips the dev memset. Actual final
  release remains possible if device_unregister was deferred. Re-registration
  of this retired master is unsupported. Opt-in failed publication leaves node
  and ID cleanup to final mtd_release; a delayed callback cannot remove a newly
  reused ID after duplicate early cleanup.
* H2FMI explicitly disables automatic regular NVMEM providers on the master and
  all descendants. Actual mtd_nvmem_reg_read bypasses MTD acquisition callbacks;
  retaining a NVMEM consumer alone is not a provider-context lifetime guarantee.
  H2FMI supplies no OTP callbacks. This is no general NVMEM lifetime fix.
* Remove/probe unwind atomically mark the context removed, then drain a whole
  synchronous request gate before core unregister/provider devres unmapping.
  Existing handles fail ENODEV with zero new progress and untouched output;
  new acquisition fails. A long read stops at its next page boundary, preserving
  exact completed progress. A blocked current fake page finishes before teardown.
  No wait for FD close is introduced. No scheduling/concurrent-client-count or
  inherited IOP/CDMA hardware fence is claimed by this gate.
* The probe owner remains alive through every unregister/unwind access. An
  unexpected unregister failure conservatively retains inactive context instead
  of freeing potentially referenced memory. That exceptional allocation may leak;
  it cannot re-enable provider access. No reset or device retry is added.

## Cases and verification

Native GCC and existing constrained Ubuntu22 image, ASan+UBSan+Werror:
original 28 controls (PARTITIONED_MASTER=n), 32 (y); candidate 31 (n), 35 (y).
Both configurations exercise actual add/parse/register/unregister/get/put/device
release functions and real H2FMI probe/remove/read wrappers. Cases include:

* each MMIO mapping and context/page/name allocation failure;
* partial earlier-die publication and FD retention during a later failure;
* current-die publication plus parser EPROBE_DEFER and an open current-die FD;
* each die's device_add failure, with and without an external retained device;
* delayed master device release with/without a client FD;
* partition get callback arguments, master regular-NVMEM opt-out, and delayed
  partition release after master remove;
* modeled try_module_get failure rollback (the real built-in owner=NULL cannot
  fail try_module_get; this exercises the actual generic core rollback branch);
* conservative unregister failure; blocked read/removal ordering and exact
  one-page progress without second-page stale output.

Three additional candidate controls reject modular MTD core, modular provider
owner and MODULE H2FMI probe. Existing 21 driver/MEMREAD read/ECC/progress cases
continue to pass with the actual new wrapper and its request gate.

Two actual ARM Linux 6.12 objects, H2FMI and mtdcore, compile under -Werror with
candidate include/linux/mtd/mtd.h and existing generated normal kernel headers.
The exact saved kernel flags plus private include override, source/output paths,
and working directory are in out/arm-commands.json. The kernel source tree and
normal bundle remain untouched. No complete kernel link or device validation is
part of this private group; Root performs integration/builds separately.

## Pinned primary sources

The baseline original source hashes are checked by the portable runner, not
recreated by inverting a candidate:

* H2FMI 820ce09ca39bef023ec4289f05bc2110d5e415b48e0a6f1d43b503dead83a58a
* mtdcore a14842ae73acebc5601e74bcbde1b2dfac594979fd2512a38ea30d406b57540e

Other exact input hashes, generated config and saved compiler commands are in
input-hashes.json. The public sources are GPL Linux code; no Apple firmware bytes
are present. Relevant upstream Linux v6.12 originals:

* https://github.com/torvalds/linux/blob/v6.12/drivers/mtd/mtdcore.c
* https://github.com/torvalds/linux/blob/v6.12/drivers/mtd/mtdpart.c
* https://github.com/torvalds/linux/blob/v6.12/drivers/base/core.c
* https://github.com/torvalds/linux/blob/v6.12/drivers/base/dd.c

## Isolated proposal and repeat

This package is not installed by apply-kernel-patches.py or selected by normal
Kconfig/DT or RAM bundle. Generic MTD interfaces and built-in-only support need
focused review before integration; the normal read-only driver is unchanged.

    python3 tests/test-h2fmi-lifetime.py
    python3 tests/test-experimental-h2fmi-read.py

The first runner reconstructs the actual proposed core/header with git apply
in a fresh temporary directory from SHA-pinned GPL baseline fixtures. It then
compiles exact extracted functions with complete driver structs and modeled
resource controls. No git history or normal build tree mutation is required.
The second runner exercises the actual new read wrapper with unchanged Linux
MEMREAD helpers and needs the existing pinned local Linux source tree.
No whole-kernel link or runtime activation is claimed by the object builds.

## Failed partition publication and final release

Initialized opt-in partitions now retain a constructor device reference through
attributes, subpartition parsing and the caller's last offset access. Failure
unlinks before dropping it; earlier failures and non-opt-in callers retain their
existing allocation path. Nested parent devices stay explicitly retained.
Final cleanup with a retained driver context runs from embedded work under the
MTD table mutex, preserving the ID reservation until cleanup and avoiding
recursive lock acquisition from synchronous device-release callbacks. A
pre-publication failed get has no driver reference and its constructor's final
put holds the table mutex directly. Unregistered opt-in devices reject new gets.

Actual core/partition C controls cover synchronous/delayed failed publication,
single and array callers, nested parents, unregistered-parent and early errors,
parser removal before final offset access, parse rollback and ordinary non-opt-in
get/put/unregister. Historical failed allocation uses logical retirement and
quarantined owned storage; no real invalid access or hardware/kernel execution
is performed. Workqueue/device/allocator/list providers remain explicit host
models, separate from actual ARM header/object and complete-link qualification.
No physical IOP/DMA fence, write operation or normal-image activation is supplied.


Root integration of this follow-up independently compiles three selected ARM
objects (H2FMI, MTD core and partition) with the updated header under `-Werror`.
A separate private full kernel compile/link also passes with normal flags,
reusing cached baseline objects. This incremental update emits no new warnings;
the preceding full qualification recorded 18 existing missing-prototype warnings.
It has no userspace initramfs and is not a boot-test image. The portable partition
runner covers 11 fixed cases plus two quarantined historical controls for each
PARTITIONED_MASTER configuration. These modeled workqueue cases do not prove
physical DMA retirement. Repeat with:

    python3 tests/test-h2fmi-partitions.py

The updated four-source private installer passes 24 filesystem regression
controls and nine portable actual-Git boundary controls. Older 11 historical/fixed
containment controls remain evidence for the preceding installer revision; they
are not counted as a fresh run of this revision.
