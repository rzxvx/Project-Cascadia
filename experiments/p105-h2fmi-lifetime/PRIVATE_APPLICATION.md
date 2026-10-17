# Optional application to a private H2FMI qualification tree

prepare-lifetime.py is an optional source installer/checker. It has no build,
flash, boot, device, service, mount or normal-image publishing operation.
Default is read-only checking; --apply changes exactly the four proposal sources
inside an explicitly acknowledged private prepared tree.

## Preconditions

Use an existing, reviewed Linux6.12 Cascadia prepared-tree snapshot at kernel
commit adc218676eef25575469234709c2d87185ca223a. Copy it into a fresh caller-owned
0700 directory outside the actual Cascadia checkout, without hardlinks to normal
source/output. All access must be serialized. The caller still supplies reviewed
source provenance: checking a Git HEAD and component hashes is not a whole dirty
working-tree integrity proof or defense against a malicious same-UID process.

The proposal must be the exact published experiments/p105-h2fmi-lifetime directory
under the supplied existing Git checkout root. The checker verifies that actual
root with a local read-only Git discovery command, ignoring ambient Git routing
variables. A typo, unrelated checkout or a nested directory cannot weaken the
normal-tree exclusion. Source/provenance hashes are fixed; no patch fuzz is used.
Both .config and generated autoconf must retain ARM, ARCH_APPLE_S5L, MTD and
MTD_APPLE_H2FMI built-in. Module configurations are intentionally unsupported.

Create the following 0600 acknowledgement ONLY in that private tree, using its
absolute path and the current caller uid:

```json
{
  "schema": 1,
  "purpose": "offline-h2fmi-lifetime-complete-link",
  "kernel_pin": "adc218676eef25575469234709c2d87185ca223a",
  "tree": "/absolute/caller-private/linux",
  "owner_uid": 1000
}
```

Filename: .cascadia-lifetime-tree.json. Keep the same absolute paths when using a
container. Do not create this acknowledgement in the normal kernel/build tree.
Require enough space (preferably12GB) and a quiet snapshot window; never stop a
productive container to obtain it. Inspect source/build/.git symlinks in the copy;
source/output links back to normal trees are unsuitable for later qualification.

## Review and apply

Example, after explicitly preparing/acknowledging the private snapshot:

```bash
PROTECTED_REPO=/absolute/project-checkout
QUAL_TREE=/absolute/caller-private/linux
python3 "$PROTECTED_REPO/experiments/p105-h2fmi-lifetime/prepare-lifetime.py" \
  --tree "$QUAL_TREE" \
  --proposal "$PROTECTED_REPO/experiments/p105-h2fmi-lifetime" \
  --protected-repo "$PROTECTED_REPO"
python3 "$PROTECTED_REPO/experiments/p105-h2fmi-lifetime/prepare-lifetime.py" \
  --tree "$QUAL_TREE" \
  --proposal "$PROTECTED_REPO/experiments/p105-h2fmi-lifetime" \
  --protected-repo "$PROTECTED_REPO" --apply
```

The checker refuses the actual checkout or its ancestors, symlink paths, nonowned
or multiply linked source files, unknown/mixed inputs and unowned/incomplete
receipts. It reconstructs the core/header/partition patch in temporary private staging,
checks all final hashes before publication, then records its install.
An already installed tree with its complete owned receipt is idempotent and is
not rewritten. Replacements are atomic per file, NOT crash-atomic across all four
files. An interrupted/mixed state remains refused; there is no automatic recovery,
reset, deletion or attempt to repair a normal tree. Deliberately prepare a fresh
private snapshot instead. The component's original source-provenance.json is not
rewritten by this helper.

## Manual complete-link qualification only

No automatic build wrapper is exported. Ambient KBUILD_OUTPUT or MAKEFILES can
redirect even make clean despite a private working directory; a local source
installer cannot validate every future command environment. For a later reviewed
qualification, use the existing bounded compiler image, normal checkout mounted
read-only, caller-owned qualification root writable, no device mounts/privilege,
and network disabled. Preserve absolute paths so acknowledgements stay bound.

Use an environment-cleared manual make invocation and explicit private output.
With the same QUAL_TREE absolute path inside the container, examples are:

```bash
env -i /usr/bin/make -C "$QUAL_TREE" O="$QUAL_TREE" \
  ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabihf- clean
# Disable CONFIG_INITRAMFS_SOURCE only in private .config before olddefconfig.
env -i /usr/bin/make -C "$QUAL_TREE" O="$QUAL_TREE" \
  ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabihf- olddefconfig
# Re-run the read-only installer check and verify the private setting:
# CONFIG_INITRAMFS_SOURCE=""
env -i /usr/bin/make -C "$QUAL_TREE" O="$QUAL_TREE" \
  ARCH=arm CROSS_COMPILE=/usr/bin/arm-linux-gnueabihf- -j2 vmlinux zImage
```

These are instructions, not an invocation by the installer. env -i excludes
inherited KBUILD_OUTPUT/KBUILD_SRC, MAKEFLAGS/MAKEFILES and unrelated compiler or
Python/Git controls; /usr/bin paths and O bind the intended local tools/output.
The caller must inspect/log the actual commands and private source snapshot.
Missing tools are a stop/document condition, not permission to install packages.
Do not call normal build-kernel.sh/cascadia: they manage normal overlay/output.
Do not upload the copied tree: prior cached initramfs products may contain private
material even though the new linked image's embedded initramfs is disabled.

The first independent optional complete-link qualification on2026-10-12 passed
with NORMAL project compiler flags;18 existing prototype warnings were retained
and inventoried. Selected changed MTD and Apple boot objects separately passed
-Werror. That record used cached objects between retries; it is not a claim of a
fresh whole-kernel warning-free/-Werror build. Root's private verification record
logs/night/h2fmi-full-link-verification-v1.json remains separate and unchanged.
No linked image is a boot-test bundle, normal activation, DMA fence or NAND test.

## Focused containment regression

Private review controls execute the preserved historical installer and candidate
on only owned synthetic source/project trees. The historical helper accepts three
mistyped/unrelated protected paths and modifies a synthetic normal target; the
candidate refuses each before mutation. Correct private application/idempotence,
actual normal-root rejection, ambient Git override isolation and nested checkout
root rejection pass. An actual historical shell wrapper's ambient variables were
captured by process-local make/gcc stubs; an env-cleared manual call excludes them.
No GNU Make, compiler, kernel/device build or normal-tree mutation runs in these
containment fixtures. The kernel Makefile's actual KBUILD_OUTPUT routing supplies
the independent source explanation; fake routing is not a hardware/build result.

The corrected proposal also changes mtdpart.c. Older three-source receipts/snapshots are intentionally refused by this version; prepare a fresh reviewed private snapshot rather than silently upgrading it. The root-reviewed independent qualification uses its own private source-update record and preserves the earlier tree.
