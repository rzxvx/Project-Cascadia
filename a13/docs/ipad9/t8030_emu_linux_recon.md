# Can qemu-t8030 boot Linux (via m1n1)? — recon + empirical result

Date: 2026-10-17. Goal: turn the qemu-t8030 bench into a hardware-free
Linux-on-T8030 test bench by loading m1n1 as a payload, which then boots Linux.

## Verdict: NO — the machine is hard-wired to boot an XNU kernelcache.
The emulator **cannot** load a non-XNU payload (m1n1, pongoOS) as-is. Proceeding to
a Linux bring-up is **blocked** until the qemu machine is patched (scope separately).
Empirically demonstrated below (qemu aborts with `unsupported xnu`).

## How the loader actually works  [qemu-t8030 @ fd4b0f79]
- Only entry point is `-kernel <macho>`. There is **no DFU / pongoOS / SecureROM
  staging path** for `-M t8030`: `hw/usb/apple_otg.c` models no bootrom DFU, and
  only `-M s8000` (iPhone 6s) has a `-bios` SecureROM. So the DWC2 USB gadget is an
  OS-facing device, not a payload loader.
- `t8030_machine_init()` [hw/arm/t8030.c:1673] does:
  `macho_load_file()` → `macho_build_version()` → **`t8030_patch_kernel(hdr)`**
  → later dispatch on `hdr->filetype` (MH_EXECUTE→classic_kc, MH_FILESET→fileset_kc,
  else abort "Unsupported kernelcache type").
- **The fatal gate is `t8030_patch_kernel()` [hw/arm/t8030.c:~165], which just calls
  `kpf()`** — the XNU kernel patchfinder [hw/arm/xnu_kpf.c]. It is run
  **unconditionally on every loaded macho**, and immediately looks up XNU kexts:
  ```
  xnu_pf_get_kext_header(hdr, "com.apple.filesystems.apfs")   // xnu_kpf.c:437
  xnu_pf_get_kext_header(hdr, "...AppleMobileFileIntegrity")  // :445
  xnu_pf_get_kext_header(hdr, "...AppleSEPKeyStore")          // :462
  ```
  Those require XNU-only Mach-O sections `__PRELINK_INFO` / `__PRELINK_TEXT`
  [hw/arm/xnu_pf.c:49-62]. A non-XNU macho has none → `error_report("unsupported
  xnu")` and the machine aborts (then NULL-derefs the missing range).

So although the `filetype` switch *would* accept any `MH_EXECUTE` macho and jump to
its entry, the unconditional XNU patchfinder upstream of that switch rejects anything
that isn't an iOS kernelcache. **The machine only ever boots XNU.**

## Empirical proof (what I actually ran)
Built m1n1 and tried to load it:
1. **m1n1** = `HoolockLinux/m1n1` branch `idevice` @ `d5a10ac5`, built generic with
   `CHAINLOADING=1` (toolchain: brew `llvm`+`lld`+`dtc`, rust 1.99.1 +
   `aarch64-unknown-none-softfloat`). Outputs `build/m1n1-idevice.macho` (983 KB) etc.
   Build **succeeded**.
2. `-kernel m1n1-idevice.macho` → qemu error `macho_parse: Invalid Mach-O:
   segCmd->fileoff >= len`. Cause: m1n1's `__LAST` segment declares `fileoff 983040,
   filesize 67108864 (64 MB)` for the appended-payload area; with no payload the file
   is shorter than the segment, and qemu's `macho_parse` does an unclamped `memmove`
   of `filesize` bytes → **qemu SIGSEGV** (crash report: `macho_parse` →
   `_platform_memmove`). (A real concatenated payload, or zero-padding the file to
   `983040 + 64 MiB = 68091904` bytes, avoids this.)
3. Zero-padded the macho to 68091904 bytes and retried → qemu now parses and prints:
   ```
   Loading Unknown 0.0...                     (macho_build_version = 0; not XNU)
   kernel_low:  0xfffffff007000000             (m1n1-idevice link base, parsed OK)
   kernel_high: 0xfffffff00b12c000
   qemu-system-aarch64: unsupported xnu        (kpf patchfinder, then abort)
   ```
   No UART output — it dies in `kpf()` during machine init, long before m1n1 code
   runs. This is the wall.

## Side-finding: m1n1 `idevice` has no A13 support anyway
Even if the loader were patched, this m1n1 branch would not bring up T8030:
- `src/soc.h` has **no T8030 define** (jumps T8015 → T8103); no `EARLY_UART_BASE`.
- `src/chickens.c` CPU part table lists A7…A11 then **M1** — **no A12 (Vortex) and no
  A13 (Lightning 0x12 / Thunder 0x13)** entry. At runtime an unknown MIDR falls back
  to `midr_part_info_unknown` (no chicken bits) [chickens.c:174-210] — non-fatal, but
  no real CPU init. (UART *is* ADT-driven via `adt_get_reg` [src/uart.c:34], so the
  console itself would work once m1n1 ran.)
So an m1n1 T8030 port must be written first (the Lightning/Thunder chickens entry +
SoC wiring) — same gap the init notes scoped. See `t8030_init_notes.md`.

## What a qemu machine patch would need (if we pursue it)
Minimal first step: make the XNU patchfinder conditional — skip
`t8030_patch_kernel()`/`kpf()` when the macho is not an XNU kernelcache (e.g.
`macho_build_version(hdr) == 0`, or no `__PRELINK_INFO`/`__PRELINK_TEXT`). Expect
further XNU assumptions downstream to surface after that (`macho_populate_dtb`,
trustcache/APTicket consumption, bootargs layout, entry EL, rtbuddy protocol), so
this is a real mini-project, not a one-liner — worth a separate scoped task.
Alternative routes: add a DFU/pongoOS staging path to the machine (bigger), or use a
different emulator that boots raw Apple-silicon payloads.

## Artifacts (all under ~/Desktop/t8030-emu-build, nothing committed)
- `m1n1-idevice/` (built), `qemu-t8030/` (built), toolchain installed.
- qemu crash report: `~/Library/Logs/DiagnosticReports/qemu-system-aarch64-*.ips`.
