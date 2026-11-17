# The NAND install on other iOS versions (a task for later)

Status: **not started.** The NAND install and the Cascadia app are checked on
iOS 8.4.1 (12H321) only, and both refuse any other build on purpose:
`cascadia-helper` (`BUILD`, `app/helper.c`) will neither boot nor update, and
`nand-volume` stops at `ios_up()`. Other versions were never tried and most
likely do not work as things are. The NFS route does not depend on the iOS
version (`--kdfu` from a jailbroken 7–9.3.6).

Until this is done, the answer for an iPad on iOS 9 is to go back to 8.4.1:
Legacy iOS Kit does that from a jailbroken iOS 9 through kDFU, with no
microcontroller (it wipes the iPad).

## Why it is tied to 8.4.1

1. **The boot without a computer.** kloader starts an iBEC from the 8.4.1
   IPSW, patched at fixed addresses (`patch-ibec-upgrade.py`). That iBEC
   reads `/kernelcache` from the System partition of whatever iOS is
   installed, through its own FTL, LwVM and HFS+ code.
2. **The partition table.** A new LwVM header is checked first by the
   validator out of the 12H321 kernelcache, run in Unicorn
   (`tools/nand-volume/lwvm_validate.py`).
3. **Writing through iOS's FTL.** Linux writes through the FTL iOS uses,
   and iOS booting on a context Linux wrote has been checked many times, on
   8.4.1 only. The FTL's context is global: a difference in another
   version's format would break the map of the whole disk, iOS included.
4. **Data's shrink.** `cascadia-hfs` resizes iOS's Data as 8.4.1 left it;
   on another version spec 6.4's checks would have to be redone.

## What it would take (9.3.5 / 9.3.6 first)

- [ ] An iPad mini 1 on 9.3.x that may be lost (a restore to 8.4.1 is the
      way back), and a full NAND dump of it before anything is written.
- [ ] Compare iOS 9's FTL with 8.4.1's: the context and block formats
      (`ndrG`, the sequence/LBA metadata, the VFL), from 9.3.x's kernelcache
      and a dump of a 9.3.x NAND. The Linux driver mounts read-only first.
- [ ] LwVM: the validator out of 9.3.x's kernelcache (a per-build
      `CASCADIA_KC`), and the header layout compared.
- [ ] The boot: does 8.4.1's patched iBEC read 9.3.x's System partition
      (FTL, LwVM, HFS+)? Else 9.3.x's own iBEC, patched the same way.
- [ ] `cascadia-hfs` on 9.3.x: Data's HFS+ and its resize, with the checks
      of spec 6.4.
- [ ] The whole round trip on that iPad: volume made, Linux filled and
      booted from the app, iOS booted on Linux's context, the app's update
      and roll back, the volume removed, iOS's fsck clean at each step.
- [ ] Only then: the build checks in `app/helper.c` and `nand-volume.sh`
      take a list of checked builds instead of one.
