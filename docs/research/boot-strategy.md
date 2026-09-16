# Boot strategy — P105AP (iPad mini 1)

Last updated after hardware tests: LIK SSH ramdisk **works**; `irecovery -c go`
**does not** leave Recovery on LIK-patched iBEC.

## Sanity check summary

| Gate | What | Your result |
|------|------|-------------|
| 0 | Pico pwn → `PWND: checkm8` | OK |
| 1 | primepwn iBSS | OK |
| 2 | LIK iBEC → `MODE: Recovery` | OK |
| 3 | Ramdisk upload + `ramdisk` | OK |
| 4 | DeviceTree + `devicetree` | OK |
| 5a | zImage + `go 0x90008000` | **FAIL** — still PID 1281 Recovery |
| 5b | staging-bundle + `go 0x90000000` | **FAIL** — still Recovery |
| LIK | kernelcache + **`bootx`** | **OK** — verbose boot, Apple spinner, SSH ramdisk |

**Conclusion:** iBEC accepts uploads and `-c ramdisk` / `-c devicetree` / **`bootx`**.
`-c go` / `-c memboot` appear to **no-op** on this LIK iBEC (or do not jump to
our payload). This is not a host/USB bug — the full LIK chain proves `-c` works
for Apple boot commands only.

## What iBoot actually boots (implemented, proven)

LIK order on P105 / 12H321:

```
primepwn iBSS
irecovery -f iBEC
irecovery -f Ramdisk.dmg  →  ramdisk
irecovery -f DeviceTree.dec  →  devicetree
irecovery -f Kernelcache.dec  →  bootx        ← only this starts execution
```

| Staged file | Format | `-c` command | Role |
|-------------|--------|--------------|------|
| iBEC | Mach-O (IMG3) | (upload only) | Recovery USB stack |
| Ramdisk.dmg | HFS in IMG3 | `ramdisk` | iOS root filesystem in RAM |
| DeviceTree.dec | Apple DT | `devicetree` | Hardware description for iBoot/XNU |
| Kernelcache.dec | **Mach-O XNU kernel** | **`bootx`** | iOS kernel — **only bootable image** |

Linux `zImage` is **not** Mach-O. **`bootx` will not load it.** Our `go` path is
unimplemented on this iBEC build.

Tools that already produce bootable artifacts (Linux host, no macOS):

- **Legacy-iOS-Kit** — iBoot32Patcher, xpwntool, hfsplus, primepwn
- `./restore.sh --device=iPad2,5 --sshrd --build-id=12H321 --no-device` — builds
  `saved/iPad2,5/ramdisk_12H321/saved/{iBEC,Ramdisk.dmg,DeviceTree.dec,Kernelcache.dec}`

macOS VM is **not** required for LIK/iBoot patching. It **is** useful for
**checkm8_bootkit** (boots raw iBoot from pwned DFU, bypasses Recovery iBEC).

## Paths ranked (clearest first)

### Path A — LIK SSH ramdisk (baseline, **use now**)

**Goal:** Confirm tool chain; mount NAND; develop next stage from a shell.

```bash
cd ~/Legacy-iOS-Kit
./restore.sh --device=iPad2,5 --sshrd --build-id=12H321
# SSH: root@127.0.0.1 -p 6414  password alpine
```

Or: `sudo bash ~/flashdrive/boot-lik-sshrd.sh`

Does **not** run Linux. It **is** the only end-to-end boot path that works today.

---

### Path B — Boot Linux from SSH ramdisk (recommended next)

**Idea:** Use Path A (proven `bootx`), then jump to Linux from **inside** the
ramdisk with a small **armv7 iOS binary** — same role as historical **kloader**.

1. LIK `bootx` → XNU + SSH ramdisk (proven)
2. `scp zImage-dtb` to device via iproxy
3. Run `linux-jump` (to build): disable MMU from userland or use `kloader`-style
   trap to physical `0x80008000`

**Pros:** Uses only implemented iBoot boot (`bootx`). No custom kernelcache yet.  
**Cons:** Need armv7 iOS Mach-O stub + physical memory map; kloader source may
help (iOS 8 era, armv7).

This is the most realistic Linux path without forging XNU kernelcache.

---

### Path C — Patched kernelcache + `bootx` (hard)

**Idea:** bspatch `Kernelcache.dec` so XNU early boot chainloads Linux from a
known RAM address, then use normal LIK upload + **`bootx`**.

LIK already bspatches kernelcache for jailbreak (e.g. daibutsu). Same tool chain,
but the patch must run **before** XNU owns the machine — expert XNU boot work.

**Pros:** Stays 100% on iBoot `bootx` rail.  
**Cons:** Large reverse-engineering effort; one bad patch = instant panic.

---

### Path D — checkm8_bootkit + custom iBSS (macOS VM)

**Idea:** From pwned DFU, boot a **custom raw iBSS** (not LIK Recovery iBEC).
May expose different USB commands or direct jump to staged zImage.

```text
# On macOS VM (Xcode CLT):
cd tools/checkm8_bootkit && make WITH_ARMV7=1
./build/checkm8_bootkit boot /path/to/patched-iBSS
```

**Pros:** Bypasses Recovery iBEC that ignores `go`.  
**Cons:** Requires macOS build; still boots **iBoot images**, not zImage — need
custom iBSS that hands off to Linux (same staging-loader problem, different entry).

---

### Path E — OpeniBoot / replacement bootloader

Historical **openiBoot** + **iDroid** ran Linux on **iPad 1** (A4), not P105.
No maintained P105 port found. Would be a from-scratch port.

---

### Path F — Raw `go` / staging-loader via LIK iBEC

**Status: ruled out for this iBEC.** Device stays PID 1281 after `go`. Do not
spend more cycles here unless you switch to a different iBEC (autogo, bootkit).

## Recommended plan

```
Now     → Path A: SSH ramdisk baseline (boot-lik-sshrd.sh)
Done    → PMCCNTR clocksource + restore time/IRQ init (mach-apple/pmccntr.c)
Next    → Path B: armv7 linux-jump from ramdisk SSH
Parallel→ Path D: macOS VM + checkm8_bootkit (optional bypass)
Later   → Path C only if B fails and you accept XNU patching
Stop    → Path F (go/memboot on LIK iBEC)
```

Prerequisite for Path B: Linux must not hang in `start_kernel` waiting on a
dead A9 global timer. That is addressed by the PMCCNTR clocksource (see
`DECISIONS.md` §5 / `docs/p105-cpu-freq.md`). This does **not** implement Path B.
## macOS VM — what to install (if doing Path D)

| Tool | Purpose |
|------|---------|
| Xcode Command Line Tools | `clang`, Mach-O linker |
| Homebrew | libusb, pkg-config |
| checkm8_bootkit submodule build | `make` in `tools/checkm8_bootkit` |
| LIK (optional on VM) | Same as Linux; patching works cross-platform |

You do **not** need macOS to run LIK or iBoot32Patcher — your Linux laptop already
builds all SSH ramdisk files.

## Quick verification commands

```bash
# After any "boot" attempt:
sudo irecovery -q          # MODE: Recovery + PID 1281 = jump did NOT happen
lsusb | grep 05ac          # 1281 = Recovery, gone = payload ran

# Working LIK baseline:
cd ~/Legacy-iOS-Kit && ./restore.sh --device=iPad2,5 --sshrd --build-id=12H321
ssh -p 6414 root@127.0.0.1   # alpine
```

## Honest bottom line

**"Compile something iBoot can boot"** = compile or patch **kernelcache** (Mach-O
XNU) or boot **iBSS/iBEC** chain — not Linux zImage directly.

The clearest implemented route to **Linux** is not `bootx` with zImage; it is
**`bootx` with iOS ramdisk**, then a **second-stage jump** to zImage from ramdisk
(Path B), or a **macOS-built custom iBSS** (Path D).
