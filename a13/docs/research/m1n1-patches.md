# m1n1 (idevice) patches for A13 / t8030

Base: `HoolockLinux/m1n1` branch `idevice`, commit `d5a10ac`. The fixes are
`a13/m1n1/0001-*.patch`; `a13/m1n1/build-m1n1.sh` fetches, patches and builds
(`CHAINLOADING=1 build/m1n1-idevice.macho`). The build tag reads
`d5a10ac-dirty` once patched.

Stock m1n1 runs on A13 up to `Running proxy...` out of the box (framebuffer,
MMU, pmgr, AIC, display all work). Two patches were needed to get a *payload*
to actually execute after the handoff. Both are confirmed on-device.

## The handoff is a plain EL1 call

There is no EL2→EL1 drop on iPhone: m1n1 is entered by iBSS at **EL1** (Macs get
EL2). `main.c` just calls the entry as a C function with the Linux boot-protocol
registers, after turning the MMU off:

```c
// src/main.c (~209)
printf("Vectoring to next stage...\n");
next_stage.entry(next_stage.args[0], ... );   // x0 = dt (phys), x1..x4 = 0
```

m1n1 does **not** shut the dockchannel UART down before this (it prints
"Vectoring" through it after `mmu_shutdown`). It *does* run `nvme_shutdown`,
`exception_shutdown`, `usb_iodev_shutdown`, `display_shutdown`, `fb_shutdown`,
`mmu_shutdown`. On internal display `display_shutdown(DCP_SLEEP_IF_EXTERNAL)`
does not sleep the panel, so the display keeps scanning `0x8fa298000` — which is
why a payload can still paint the screen after the handoff.

## Patch 1 — CTRR M4 guard (crash fix)

Symptom (stock m1n1): right after `Vectoring`-prep the kernel path took a
`SYNC` exception, `ESR 0x2000000` (EC=0, "unknown"), PC in `kboot_prepare_dt`.

Cause: `kboot_prepare_dt()` reads an **M4-only** register:

```c
// src/kboot.c:2942, inside  if (!cpu_features->apple_sysregs_unlocked) {
u64 ro_start = mrs(CTRR_M4_LWR_EL2);   // undefined at EL1 on A13 -> SYNC
```

A13 (`CPU part 0x13`) is **absent from `midr_parts[]` in `chickens.c`**, so it
falls back to `features_unknown`, where `apple_sysregs_unlocked == false`, so
the guard `!apple_sysregs_unlocked` wrongly enters the M4 branch. Constants for
A12/A13 already exist in `midr.h` (`T8030_LIGHTNING=0x12`, `T8030_THUNDER=0x13`).

Band-aid applied (kboot.c:2937), matches the upstream `a13-ctrr.patch` idea:

```c
if (chip_id != 0x8030 && !cpu_features->apple_sysregs_unlocked) {
    ... M4 CTRR reservation ...
} else if (chip_id == 0x8030) {
    printf("FDT: skipping M4-only CTRR reservation on A13\n");
}
```

Proper fix (not done, preferred long-term, per the "fix the driver" rule): add
A12/A13 to `midr_parts[]` with a `features_a13` based on `features_a11`
(`apple_sysregs_unlocked = true`, `disable_dc_mva`, `acc_cfg`, `nex_powergating`,
`fast_ipi`, `uncore_version=UNCORE_V1`, `sleep_mode=SLEEP_CLUSTER`). That also
removes the `CPU: Unknown` line and applies the Apple HID/chicken register setup
the core normally needs — which may matter for other subtle issues.

On-device result: crash gone, log reaches `Vectoring to next stage`. The marker
line `FDT: skipping M4-only CTRR reservation on A13` confirms the patch is live.

## Patch 2 — clean the payload to PoC before the caches-off jump (silence fix)

Symptom: after the crash was fixed, the payload was *silent* — it did not even
execute. A bare-metal stub proved this: it only started running once this patch
was in.

Cause: `mmu_shutdown()` (src/memory.c:636) turns caches off and then flushes by
**set/way**:

```c
write_sctlr(read_sctlr() & ~(SCTLR_I | SCTLR_C | SCTLR_M));
dcsw_op_all(DCSW_OP_DCCISW);   // set/way clean+invalidate
```

Set/way cache maintenance does **not** reach the Point of Coherency on Apple
cores (system-level cache + non-architected hierarchy; ARM only defines set/way
for power management). So the kernel/stub m1n1 decompressed through cached
mappings was not written back to RAM, and after caches were turned off the CPU
fetched stale bytes and faulted silently (m1n1's exception vectors were already
torn down). `chainload.c` already cleans its stub by VA (`dc_cvau_range` +
`ic_ivau_range`) — `kboot.c` did not do the equivalent for the kernel.

Fix added in `kboot_boot()` (src/kboot.c, before `next_stage.entry = kernel;`):

```c
struct kernel_header *hdr = kernel;
dc_civac_range(kernel, hdr->image_size);      /* clean kernel to PoC by VA */
if (dt && dt_bufsize)      dc_civac_range(dt, dt_bufsize);
if (initrd_start && initrd_size) dc_civac_range(initrd_start, initrd_size);
sysop("dsb sy");
ic_ivau_range(kernel, hdr->image_size);
sysop("dsb sy");
sysop("isb");
```

`dc civac` (clean+invalidate to PoC by VA) is the architected op that *does*
reach PoC on Apple, unlike set/way. On-device result: a framebuffer-painting
stub now executes with the real (MMU-off) handoff — the screen turns the colour
the stub writes. This is also what lets Linux execute (it just hangs later for
the config reason in [bringup-status.md](./bringup-status.md)).

## Diagnostic toggles used (currently reverted)

While debugging the silent payload, `main.c` was temporarily edited to (a) keep
the MMU on and keep `exception_shutdown` out so a faulting payload would be
caught and printed, and (b) skip `usb_iodev_shutdown`/`display_shutdown` to test
whether they killed the dockchannel transport. Both are **reverted** to the
normal shutdown sequence now. Keep them in mind if the screen behaves oddly.
