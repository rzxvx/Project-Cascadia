# Project Cascadia — cheatsheet (Linux on iPad mini 1 / Apple A5)

## Hardware
- Device: iPad mini 1, iPad2,5 / A1432, board `p105ap`, SoC Apple A5 (**S5L8942X**)
- 512MB RAM, dual Cortex-A9
- Exploit: checkm8 via a Raspberry Pi Pico (checkm8-a5, LukeZGD)
- DCSD cable: `/dev/cu.usbserial-A506VZ9Q`, 115200 baud, needs `sudo`. The cable carries UART and USB D+/D- at the same time.
- The firmware everything is pinned to: **12H321** (iOS 8.4.1, "Donner").
  It is not in the repository — `./cascadia firmware` derives iBSS/iBEC from your IPSW.
  The pin is not arbitrary: the autogo hook patches an address inside exactly this iBEC build.

## Everything goes through ./cascadia

The repository is self-contained since 2026-09-17. `~/iBSSloader` is NOT needed for
anything any more: dts, config, patches, the rootfs overlay and the boot chain live here.

```bash
./cascadia doctor      # what this machine is missing
./cascadia kernel      # clone Linux pinned at v6.12 into build/linux
./cascadia rootfs      # build Alpine armhf for the initramfs
./cascadia build       # dtb + kernel + output/staging-bundle.bin
./cascadia firmware    # iBSS/iBEC from your own IPSW (needs Legacy iOS Kit + ipad25.ipsw)
./cascadia flash       # flash:  iBSS → iBEC → bundle → loader
```

Already have a kernel tree and don't want a second 2.5 GB one:
```bash
KERNEL_SRC=~/Desktop/linux-kernel ./cascadia kernel   # symlink; build mounts
                                                      # the target into the container
```

### Where things live
```
dts/p105ap.dts              SOURCE OF TRUTH for the DTS (not the kernel tree!)
config/p105ap.config        Kconfig fragment, merged with multi_v7_defconfig
patches/files/**            whole NEW kernel files, copied as they are
patches/tree/*.patch        edits to EXISTING kernel files, git apply
rootfs/alpine/**            Alpine-side overlay: apk repositories, inittab, motd
initramfs/init              stage 1: mounts, clock, USB, network, choice of root
initramfs/sbin/p105-stage2  stage 2: getty, dropbear — shared by both roots
pongo/                      bare-metal linux-boot trampoline
build/                      everything generated: kernel, rootfs, firmware
output/                     staging-bundle.bin + staging-loader.bin
```

### How the initramfs is assembled

`./cascadia rootfs` unpacks alpine-minirootfs, lays two overlays on top —
`rootfs/alpine/` (the Alpine side) and `initramfs/` (our stage 1 and stage 2) —
installs what cannot be installed after the first boot (dropbear with your
public key, and `mount.nfs`), and copies whatever is in `build/keep/` over it.
The last one is for files the repository cannot reproduce yet: at the moment
that is `hx-touchd`, the touch daemon from Sandcastle.

**Nothing inside `build/initramfs-root` is edited by hand.** Until 2026-09-18 it
was edited exactly there, while the repository held a copy of `/init` from before
USB networking, and a clean clone built a kernel that came up on the glass —
with no console over the cable, no 10.55.0.2 and no ssh. Now `./cascadia build`
checks the built archive for `P105: stage1 start`, `p105-stage2`, `ttyGS0` and
`10.55.0.2` and fails if they are missing.

If docker is not running, the package step is skipped with a warning: the tree
still boots, but without ssh and without the NFS root.

### Two patch mechanisms, don't mix them up
**New files** — `patches/files/` are copied by `apply-kernel-patches.py --files-only`.
**Edits to existing files** — one `patches/tree/0001-cascadia.patch`, applied with
`git apply` through `apply-kernel-edits.sh`, with a reverse check for idempotency.

The second used to be done by matching anchor strings, and an anchor that did not
match was **skipped silently** — the first thing to disappear was the call to
`apple_aic1_rearm()`, after which the kernel built green, booted, and took not a
single interrupt. The patch either applies or says why. That is only possible
because the kernel version is pinned.

### Flashing
```bash
./cascadia flash                 # freshly built iBEC, checkm8 via primepwn
./cascadia flash --kdfu          # the same, but WITHOUT the Pico — via the jailbreak
./cascadia flash --skip-pwn      # the device is already in pwned DFU
./cascadia flash --known-good    # the August image — tells a bad build
                                 # from a bad bench
./cascadia flash --no-uart       # no serial capture
./cascadia flash --no-link       # leave the host's network side alone
./cascadia link                  # only the network step, no flashing
```

At the end, `flash` itself puts `10.55.0.1/24` on the gadget's interface on the
host and waits for `10.55.0.2` to answer. The interface is found by the MAC from
the bootargs (`g_cdc.host_addr`), not by an `enN` name. It cannot be set once by
hand: the interface is created on enumeration and destroyed when the device goes
away, and in one boot it goes away twice — when iBEC hands over and when `/init`
forces a re-enumeration. Without the address, packets for 10.55.0.2 follow the
default route out to the internet, and ssh just **hangs** without a word. Timing
matters too: stage 1 mounts the NFS root from 10.55.0.1 right after bringing up
usb0, and `mount.nfs` gives up after about two minutes — after that the root
stays in RAM.

### Two ways into pwned DFU
**primepwn** — checkm8, needs a Pi Pico or an Arduino with a USB host shield: on
A5 the exploit needs USB timing an ordinary host does not deliver.

**kDFU** — from a jailbroken iOS, `kloader` loads a patched iBSS directly, no
hardware needed at all. EverPwnage jailbreaks A5 on 7–9.3.6 **untethered**, so the
device comes up jailbroken every time and the step stays a single command.

Both are implemented in Legacy iOS Kit; we call them. One nuance: kDFU sends
**its own** pwned iBSS, not ours. They differ only in the boot-args iBoot32Patcher
wrote; what matters is the signature patch, which both have, and the boot-args
are set by iBEC anyway.

A second nuance, which cost one flash cycle: **after kloader the image has to be
decrypted**. By the time iOS has booted, the AES engine's GID key is already
disabled, so there is nothing to decrypt the KBAG with — iBSS gets garbage and
jumps into it. From outside it is deceptive: `irecovery` reports 100%, then the
device drops off the bus and looks switched off. That is why `./cascadia firmware`
builds iBEC in two forms:

| file | for |
|---|---|
| `iBEC.patched.autogo.dfu` | checkm8: cold DFU, GID alive, image packed like the stock one |
| `iBEC.patched.autogo.plain.dfu` | kDFU: img3 without a KBAG, payload in plaintext |

`flash --kdfu` picks the second by itself, and `--ibec` and `--known-good` do not
override it. LIK's pwnediBSS is built exactly the same way: `xpwntool ... -iv -k`
decrypts, `iBoot32Patcher --rsa` patches, `xpwntool ... -t iBSS` packs it back
**without** encryption, zeroing sigCheckArea. `scripts/img3pack.py` repeats that
layout rather than inventing its own.
The upload order and the pauses are not decoration: `primepwn` runs checkm8 and
leaves a pwned iBSS running; that accepts the unsigned iBEC; the autogo hook in
iBEC fires at the end of the bundle upload and starts the loader — which is why
the loader is sent last and there is no `irecovery -c go` anywhere.

The UART port is found automatically (`/dev/cu.usbserial-*`, `/dev/ttyUSB*`), and
the capture is optional. The early log is duplicated to the framebuffer, and the
UART has a habit of cutting off mid-boot — so a missing adapter is an
inconvenience, not a blocker.

### Reading the verdicts
The UART cuts off almost every time, so it is more reliable to pick them up from
the device:
```bash
ssh root@10.55.0.2 'dmesg | grep -E "AIC1-REARM|LATE-SMOKE|SOF-TIMER|AIC-TIMER|P105:"'
```

## Kernel status (Linux 6.12.0 on A5) — updated 2026-09-14
✅ Boot to an interactive shell (framebuffer console, tty0), Alpine 3.24 initramfs
✅ Custom AIC1 interrupt controller driver (**with two fixes, 2026-09-13**), PMCCNTR clocksource, simplefb
✅ **USB PHY driver** — registers fully decoded, iBoot already leaves a VALID state,
   the driver only releases reset (see the "USB PHY" section below)
✅ **DWC2 + g_ether** — the Mac physically sees the iPad and tries SET_ADDRESS (visible in `log stream`)
❌ **USB enumeration does not work at all** — the Mac gets a STALL (result code 25) because
   IRQ 50 (mapped to AIC1 IRQ 11 Edge) is never raised. The root cause is not USB
   but the **general IRQ delivery blocker** — see "AIC1 IRQ delivery" below
✅ **Touch stack — kernel side up** (September 2026 session):
   ✅ Port of 7 hx-* drivers from Corellium Sandcastle (5.4 → 6.12 API)
   ✅ Cmwp @ `0x33500300` — single-register layout (32768 Hz target) from Sandcastle
   ✅ PMU d2333 @ i2c0 0x3c, regulators `touch_ana@0x20c` + `touch_ldo@0x213`
   ✅ `input: S5L8940X Capacitive TouchScreen` → `/dev/input/event0`
❌ Touch **events do not come** — SPI1 registers return 0x22 (bus abort), the Cmwp XNU handler
   is not reversed (2026-09-12/13 session). Even if it worked, IRQ 51 would still
   not reach the CPU (the same blocker)
❌ Interactive UART input — none; the tty0 framebuffer is the only working input
❌ CPU1/SMP, WiFi — not started

# ═══════════════════════════════════════════════════════════════
# MAIN BLOCKER 2026-09-13/14: IRQ delivery AIC1 → CPU
# ═══════════════════════════════════════════════════════════════

## AIC1 IRQ delivery — what was found, what was fixed, where it stopped

### Found (bugs in `patches/files/drivers/irqchip/irq-apple-aic1.c`)

**BUG-1: CONFIG.ENABLE was never set.**
`aic1_of_init()` calls `aic1_clear_sticky_nirq()`, which among other things clears
`AIC1_CONFIG.ENABLE (BIT(0))`. There was no `aic1_enable_hw()` anywhere after it —
the AIC1 hardware stayed **switched off forever**. IPI setup worked (`ipi_mux_create`,
`set_smp_ipi_range`), but a hardware IRQ would never have been raised.

**BUG-2: `apple_aic1_early_irq_escape` was never cleared.**
The flag starts as `true` (see `arch/arm/mach-apple/apple.c:41`). While it is true,
`aic1_handle_irq` does `regs->ARM_cpsr |= PSR_I_BIT` at its very start — so even
if the handler had been called, it would force the IRQ mask on return, guaranteeing
that a second IRQ never comes. The function `apple_aic1_release_escape()` existed,
but nobody called it (the comment said "call from irq_thaw" — and irq_thaw itself
does not exist).

### Fixes (2026-09-13)
In `iBSSloader/patches/files/drivers/irqchip/irq-apple-aic1.c`, at the end of `aic1_of_init()`:
```c
/* FIX-1: switch AIC1 back on after clear_sticky_nirq */
aic1_enable_hw(aic);
/* FIX-2: clear the escape flag right away -- nobody to wait for */
apple_aic1_early_irq_escape = false;
```
Plus, right after it — bulk unmask of every IRQ (write-only registers, the readback lies):
```c
for (i = 0; i < nr_words; i++)
    aic1_write(aic, AIC1_MASK_CLR + i*4, ~0U);
for (i = 0; i < aic->nr_irq; i++)
    aic1_write(aic, AIC1_TARGET_CPU + i*4, BIT(0));
```

### Result of the fixes
- `CONFIG=0x10773` (bit 0 = ENABLE set; the `0xE0000000` IMPL bits are written but the
  readback does not show them — either write-1-clear or read-only status, not critical)
- Software test: `AIC1_SW_SET` on hwirq 0 → `EVENT` = `0x00010000` (type=HW num=0) —
  **the AIC1 hardware queues events correctly**
- **BUT** `/proc/interrupts` still shows 0 for ALL IRQs (49 SPI, 50 USB, 51 touch,
  IPI0-6, Err). The ARM CPU does not take the IRQ exception despite queued events.

### Where it stopped
There is a gap between the AIC1 output line and the CPU's nIRQ pin. Three possible causes:
1. `CPSR.I` stays `1` even after `local_irq_enable()` in `start_kernel()`
2. The AIC1 hardware IRQ output line is not physically wired to the CPU nIRQ pin,
   and some extra enable gate is needed (unknown so far)
3. `handle_arch_irq = aic1_handle_irq` is not installed / gets overwritten by someone

**Not confirmed yet: apple_a9_gic_drain()** — on an A9 MPCore the IRQ usually goes
through the GIC cpu-interface in the PERIPHBASE window (CBAR + 0x100). On the A5,
CBAR = `0x3E100000`, but any ioremap/MT_DEVICE of that window **hangs Linux**
(`aic1q33 stuck`) — Apple either removed the GIC, or the key registers do not answer
MMIO. `apple_a9_gic_drain()` is currently a **no-op**. If Apple kept the GIC's
cpu-interface component but removed the distributor, the CPU interface may need to
be initialised directly, without a distributor.

### Diagnostic patch 2026-09-13/14 (**built, not tested**)
Added to `patches/files/drivers/irqchip/irq-apple-aic1.c`:

**(a) Counter of handler entries:**
```c
static unsigned int aic1_handler_entries;
/* at the very start of aic1_handle_irq: */
aic1_handler_entries++;
```

**(b) `late_initcall(aic1_late_smoke)`** — runs AFTER `local_irq_enable()` in
start_kernel:
```c
static int __init aic1_late_smoke(void) {
    /* 1. Dump CPSR (if I=1 -- local_irq_enable() is a no-op) */
    asm volatile("mrs %0, cpsr" : "=r"(cpsr_before));
    /* 2. Baseline handler_entries + EVENT */
    /* 3. Fire AIC1_SW_SET on hwirq 0, mdelay(50) */
    aic1_write(aic, AIC1_SW_SET + 0*4, BIT(0));
    mdelay(50);
    /* 4. Re-read, classify the verdict */
    if (handler_after > handler_before)
        pr_err("LATE-SMOKE: PASS -- CPU takes AIC1 IRQ. Root cause elsewhere.");
    else if (cpsr_after & 0x80)
        pr_err("LATE-SMOKE: FAIL -- CPSR.I=1 late; local_irq_enable() didn't stick.");
    else if (ev_after == 0x10000)
        pr_err("LATE-SMOKE: FAIL -- CPSR.I=0 but AIC1 line never reaches CPU nIRQ pin.");
    else pr_err("LATE-SMOKE: WEIRD -- EVENT=%#x", ev_after);
}
late_initcall(aic1_late_smoke);
```

**(c) Bootargs cleaned up** — `initcall_debug` removed from `dts/p105ap.dts`
so the LATE-SMOKE verdict fits into the UART before it truncates.

**(d) `/init` now dumps:**
- `dmesg | grep "LATE-SMOKE|AIC1-DIAG|aic,1:"` at the very start (guaranteed to reach the fb)
- Periodically (every 10 iterations) all IRQ 49/50/51/Err counters

### What to do depending on LATE-SMOKE
| Verdict | Diagnosis | Next step |
|---------|---------|----------------|
| PASS (handler_entries>0) | The pipeline works; the real IRQ mask/target readback lies | Dig into per-line enable, check what irq_data mask/unmask actually writes |
| FAIL CPSR.I=1 | `local_irq_enable()` is a no-op somewhere | grep `raw_local_irq_enable` in arch/arm/, check for a patch in mach-apple |
| FAIL EVENT=0x10000 | AIC1→CPU wire dead, a gate is needed | RE the Apple SoC — look for some ENABLE in the PMGR/AIC region, read pongoOS interrupt_init more carefully |
| WEIRD | Something went sideways | Re-read until it makes sense |

### How to move forward next session
1. Build → flash → UART capture + photo of the fb → grep LATE-SMOKE
2. Depending on the verdict — one of the three paths from the table
3. Until IRQ delivery is solved — **USB, touch, any IRQ-driven peripheral is PARKED**.
   Everything working in the system runs on **polling** (I2C QUIRK_POLL, PMCCNTR polling clocksource,
   passive framebuffer)

# ═══════════════════════════════════════════════════════════════
# USB PHY — working state 2026-09-13
# ═══════════════════════════════════════════════════════════════

## Full register map (empirical, live dump from a running iBoot USB DFU)
Base: `0x36000000` (otgphyctrl). **All values are what iBoot LEAVES behind on leaving DFU:**

| Offset | Name | Value | Comment |
|--------|-----|-------|-------------|
| 0x00 | OPHYPWR | 0x00000006 | PLL power / XO power bits |
| 0x04 | OPHYCLK | 0x00000001 | CLKSEL_24MHZ (bit0) |
| 0x08 | ORSTCON | 0x00000000 | reset released |
| 0x1C | OPHYUNK1 (uotgtune0?) | 0x00000006 | |
| 0x30 | (unused) | 0x00000000 | |
| 0x34 | (unused) | 0x00000000 | |
| 0x40 | **UOTGTUNE1** | **0x00000549** | ADT: uotgtune1-device |
| 0x44 | **UOTGTUNE2** | **0x00002FF3** | ADT: uotgtune2-device — **not 0xF8!** |
| 0x60 | OPHYUNK4 | 0x00000200 | |

⚠️ **CRITICAL**: the old master copy of the driver tried to OVERWRITE `OPHYUNK2 (0x44) = 0xF8` —
that CRASHED the kernel at probe. The correct value is already set by iBoot (0x2FF3 == ADT
`uotgtune2-device`). **The driver no longer rewrites any tune registers**, it only:
1. Enables 7 PMGR clock gates: 87, 88, 89, 90, 91 (usb-complex), 5 (PHY), 292 (main OTG)
2. Toggles `ORSTCON.PHYSWRESET (bit0)` = 1, wait, back to 0, wait 1ms
3. Logs the final register state

Master file: `~/Desktop/linux-kernel/drivers/phy/phy-apple-s5l-usb.c` (not in NEW_FILES,
edited directly in the kernel tree).

## ADT tuning constants (for reference, to cross-check)
```
uotgtune1-device = 0x549   uotgtune1-host = 0x54F
uotgtune2-device = 0x2FF3  uotgtune2-host = 0x6DF3
ref-clock-sel = 3
usbhostset-en-incrx = 0xE0
usb-complex clock-gates = [87, 88, 89, 90, 91], main clock-id = 292
otgphyctrl clock-ids = [5]
usb-device @ 0x36100000, IRQ 11, num-of-eps = 14, fifo-depth = 0x820
```

## DTS changes (2026-09-13, in `dts/p105ap.dts`)
```dts
otgphy: phy@36000000 {
    compatible = "apple,s5l8940x-otgphy";
    reg = <0x36000000 0x1000>;
    #phy-cells = <0>;
    status = "okay";        /* ← was "disabled" */
};

dwc2: usb@36100000 {
    compatible = "apple,s5l8940x-dwc2", "snps,dwc2";
    reg = <0x36100000 0x40000>;
    interrupts = <11>;
    interrupt-parent = <&aic>;
    phys = <&otgphy>;
    phy-names = "usb2-phy";
    dr_mode = "peripheral";
    status = "okay";
};
```

## Kconfig fragment (in `config/p105ap.config`)
```
CONFIG_GENERIC_PHY=y
CONFIG_PHY_APPLE_S5L_USB=y
CONFIG_USB=y
CONFIG_USB_DWC2=y
CONFIG_USB_DWC2_PERIPHERAL=y
# CONFIG_USB_DWC2_DUAL_ROLE is not set   ← the OTG state machine hangs waiting for session-valid
CONFIG_USB_DWC2_DEBUG=y
# CONFIG_USB_DWC2_VERBOSE is not set     ← VERBOSE drowns the UART
CONFIG_DEBUG_FS=y
CONFIG_USB_GADGET=y
CONFIG_USB_ETH=y
CONFIG_USB_ETH_RNDIS=y
```

## What actually happens on the Mac
Even with a fully working DWC2 (Core Release: 2.72a, `snpsid=4f54272a` — the real chip answers),
`log stream --predicate 'process == "usbd"'` shows enumeration attempts:
```
USB device attached: iPad ... at 480 Mbps
Sending USB SETUP: SET_ADDRESS ...
Received USB response: STALL (25)
Sending USB SETUP: SET_ADDRESS ...
Received USB response: STALL (25)
...
```
The Mac sees the device, brings up HS, sends SET_ADDRESS. The DWC2 hardware takes the bus reset,
but `dwc2_handle_common_intr` is **never called** — the IRQ 50 counter in `/proc/interrupts`
stays 0. → the root cause is the IRQ delivery blocker (see above), NOT a USB problem.

## Live check that the PHY registers stayed valid
After `dwc2 36100000.usb: Core Release: 2.72a` in the UART log, look for:
```
PHY dump post-init: PWR=0x00000006 CLK=0x00000001 RST=0x00000000 U1C=0x00000006 R30=0x00000000 R34=0x00000000 R40=0x00000549 U44=0x00002ff3 U60=0x00000200
```
If U44 ≠ `0x2ff3` — the driver patch was rolled back, something rewrote the register. Alarm.

# ═══════════════════════════════════════════════════════════════
# Touch stack paths and files (September 2026)
# ═══════════════════════════════════════════════════════════════

**Drivers (master copies — edit HERE, not in linux-kernel!):**
```
~/iBSSloader/patches/files-sandcastle-port/drivers/
  clk/clk-s5l8940x-pmgr.c           — Cmwp/grape clock gate
  pinctrl/pinctrl-s5l8940x-gpio.c   — GPIO+pinctrl (arch_initcall)
  spi/spi-s5l8940x.c                — SPI controller
  i2c/busses/i2c-s5l8940x.c         — I2C controller
  mfd/apple-pmu-i2c.c               — PMU d2333 parent (regmap)
  regulator/apple-pmu-pwrsw.c       — power switch regulators
  input/touchscreen/apple-z2.c      — Z2 multitouch (miscdev "apple-z2")
```

**DTS nodes (in `~/iBSSloader/dts/p105ap.dts`):**
- `refclk24mhz` (fixed-clock 24MHz, at ROOT level, not in soc!)
- `touchclk: touchclk@3500300` — Cmwp, `apple,s5l8940x-pmgr-clk-touch`, 32768 Hz
- `i2c0: i2c@3200000` — `apple,s5l8940x-i2c`
- `pmu: pmu@3c` — `apple,pmu-d2333`, 16-bit reg
- `touch_ana@20c` + `touch_ldo@213` — `apple,pmu-pwrsw`
- `touch@0` (in spi1) — `apple,z2-multitouch`, generation=1, hv-supply/core-supply/gpios

## Cmwp / SPI1 — blocker, September 2026
The SPI1 registers at `0x32100000` return `0x22` (bus abort default). Did not help:
- Full pmgr_bootstrap-first ordering
- CLK 4/304/307 enabled
- Gate pokes replicating pongo touch_cursor exactly

**Empirical evidence**: the PMGR gate table's last non-zero entry is at `0x1140` (ibot_id 78) — gate 83 (PWM)
at `0x1154` reads 0 and writes are silently dropped. Confirms that the Cmwp handler in XNU
(kernelcache `0x804ba3e8`) does more than any known MMIO sequence. Reversing Cmwp is the only
unblocker for touch.

> **The conclusion above is wrong, measured on the hardware 2026-09-18.** Gate 83's
> register is not at `0x1154` but at `0x1124`: the array starts 10 ids lower than
> the disassembly was read (`0x3f100fd8 + id*4`). `0x1154` is a hole in the array,
> and that is what "swallowed the write". SPI1 and PWM are switched on from a
> running Linux with one write each, no `Cmwp` at all. See the "TOUCH POWER"
> section at the end of this file and `docs/research/p105-pmgr-gates.md`.

**Checked 2026-09-18: the kDFU route gives nothing.** Booting via `kloader` from a
live jailbroken iOS, where touch works, does not inherit the power state:
`peek r 32100000 8` and `peek r 33500300 1` return `0xd00c3ccc` in every word —
as meaningful as `peek r 38000000 2` (nobody's address, the same answer), while
PMGR, GPIO and UART0 read normally in the same boot. IRQ 50
(`32100000.spi`) and IRQ 52 (`apple-z2`) stayed at zero. iBSS and iBEC
re-initialise the clocks regardless of what was up in iOS.

**Even if touch had worked** — touch IRQ 51 would not reach the CPU either (the same blocker).

# ═══════════════════════════════════════════════════════════════
# XNU reversing (historical reference, not active)
# ═══════════════════════════════════════════════════════════════

USB PHY class: `AppleS5L8930XUSBPhy` (kernelcache.release.p105, iOS 8.4.1)
- `start()` = `FUN_80cab844` @ `0x80cab844`
- The real vtable: **`0x80cad090`**
- state machine = `FUN_80cabea8` @ vtable slot `0x35c`

## Decrypting the kernelcache (if another build is ever needed)
```bash
git clone --depth 1 https://github.com/tihmstar/fwkeydb /tmp/fwkeydb
# keys: /tmp/fwkeydb/keys/firmware/iPad2,5/0x8942/<BUILD>
# IV/Key for 12H321 (Donner) confirmed working
```
Format: IMG3 → AES-CBC(IV,Key) → `comp`+`lzss` header → Apple LZSS decompress → Mach-O.

## Ghidra — practical lessons
- Headless without `-analysis` does NOT resolve C++ vtables/RTTI
- The GUI CodeBrowser with full Auto-Analyze resolves strings fully, but not third-party kexts' vtables
- Finding a vtable: scan non-`__TEXT` memory for long (~90+) runs of consecutive 4-byte
  direct addresses into `.text`, find the table containing an already known function

## LZSS repack (for XNU hooks — not USB)
The original `create_lzss_payload.py` is **broken** (output not decodable).
The correct replacement: `ipad-mini-linux/lzss_ok.py` + `repack2.py` (copies in /home/claude/).

## Active XNU hook infrastructure (WORKING)
iBEC patch: `iBEC.serial` — replaced `pio-error=0` with `serial=3` — the XNU console goes to
uart0/DCSD, full TEXT logs. Kernel hook in `IOFindBSDRoot` (trampoline @ code cave
`0x80083108`), reads via `ml_phys_read` (`0x8007f3d4`), output via `kprintf` (`0x8027a7f0`).
PMGR/GPIO read without trouble; unpowered blocks (0x321/0x335/0x332) fault on the first read.
Details: `iBSSloader/docs/p105-mt-peek.md`.

# ═══════════════════════════════════════════════════════════════
# Priority order of work (after IRQ delivery)
# ═══════════════════════════════════════════════════════════════

1. **AIC1 IRQ delivery** ← blocker, solve first (LATE-SMOKE test → next steps)
2. **USB enumeration** — unblocks at once if IRQ is solved, DWC2 is fully ready
3. **Touch events** — will also need the Cmwp reverse (but UART/framebuffer are enough as input)
4. **WiFi (BCM4334 HSIC)** — an alternative path to SSH, independent of USB and touch
5. **CPU1/SMP** — there is IPI code in AIC1, waiting for IRQ to be unblocked

## WiFi — a note for the future
BCM4334 combo (WiFi+BT). SDIO/UART, HSIC for WiFi. Fully independent of the AIC1 IRQ decision
(if the WiFi driver can work by polling — TBD). Hardcoded SSID/pass in init for a test:
```
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
udhcpc -i wlan0
```

# ═══════════════════════════════════════════════════════════════
# The long history of USB reversing (Unicorn + QEMU) — summary
# ═══════════════════════════════════════════════════════════════

Covered: kernelcache decrypt, Ghidra GUI reverse (vtable `0x80cad090`, state machine
`FUN_80cabea8`), Unicorn emulation (ran to the end without crashes, but 0 MMIO writes —
the child refcount stubs returned the wrong values), QEMU danzatt/QEMU-s5l89xx-port
native build + offset trick (reached the right function, stalled on a child-object stub).

**Conclusion**: technically close, but with the AIC1 fixes and the empirical PHY dump found,
this road is now **lower priority**. The driver works on real hardware; once IRQ delivery
is unblocked it can be revisited if fine tuning constants are ever needed.

Links to the artifacts of this path:
- `/tmp/emulate_phy6.py` — Unicorn skeleton (recreate from these notes if needed)
- danzatt/QEMU-s5l89xx-port: `-M ipad1g` emulates A4, not A5 (there is no ready A5 port)
- Offset trick: `restore /tmp/seg_PRELINK_TEXT.bin binary 0x40460000` (kernelcache_vaddr - 0x40000000)

## USB diagnostics at the physical level (if ever needed)
A logic check of "does the PHY physically put out a signal" — not protocol decoding
(honest HS 480 Mbps needs GHz analyzers). 24 MHz is enough for activity/chirp.
Options: Raspberry Pi Pico (already have one) > a cheap CY7C68013A Saleae clone (~$10-15,
sigrok/PulseView) > an original Saleae.

**The user has a Saleae Logic 24 MHz** — could compare USB signalling during an iBEC boot
(where USB works — DFU/Recovery) vs a Linux boot to catch the difference. Not done —
the IRQ delivery blocker made it unnecessary at this stage.

# ═══════════════════════════════════════════════════════════════
# THE IRQ BLOCKER SOLVED (2026-09-14) — AIC1 was silenced in kernel_init()
# ═══════════════════════════════════════════════════════════════

## Root cause

`init/main.c` → `kernel_init()` contains TWO Apple blocks. The second one runs
**right before `kernel_init_freeable()` → `do_initcalls()`** and does:

```c
apple_aic1_quiesce();          /* aic1_clear_sticky_nirq(): MASK_SET for all,
                                  TARGET_CPU=0 for all, IPI_MASK_SET, drain,
                                  CFG &= ~ENABLE                              */
apple_aic1_hw_quiesce_quiet(); /* the same again, through the static mapping */
early_boot_irqs_disabled = false;
local_irq_enable();
```

Nothing after that switches the AIC on. `apple_aic1_enable()` / `aic1_enable_hw()`
are called only from `aic1_of_init()` (very early) and from the FIRST block, which
the second then overwrites.

**AIC state at the moment dwc2 does `request_irq(50)`:**
- `AIC1_CONFIG.ENABLE = 0` — the controller is off
- `TARGET_CPU[0..191] = 0` — not a single line is targeted at any CPU
- everything masked, IPIs too

A second, independent killer: `aic1_irq_unmask()` wrote **only** `MASK_CLR` and never
touched `TARGET_CPU`, so even a line unmasked by a driver stayed without a route.

This fully explains the earlier observations:
- the SW test in `aic1_of_init` showed events queued correctly (`EVENT=0x10000`) —
  it runs BEFORE the second block;
- `/proc/interrupts` = 0 on ALL lines, IPI0-6 included — quiesce silences IPIs too;
- the conclusion "there is a hardware gap between the AIC1 output and the CPU's nIRQ pin" was
  **premature** — the hardware had never been tested in the enabled state.

Also: `apple_aic1_arm_cpu0()`, despite its name, does `MASK_SET(~0)` twice and
never `MASK_CLR` — so "arm" = "mask everything, target CPU0".
The behaviour is right, the name is misleading.

## Fix (2026-09-14)

1. `iBSSloader/patches/files/drivers/irqchip/irq-apple-aic1.c`:
   - `aic1_irq_unmask()` now writes `AIC1_TARGET_CPU + hw*4 = BIT(0)` before `MASK_CLR`
   - new public `apple_aic1_rearm()`: masks every line, sets TARGET=CPU0 for
     all, drains EVENT, unmasks IPIs, `aic1_enable_hw()`, prints
     `AIC1-REARM: CONFIG=... (ENABLE=1)`
   - LATE-SMOKE prints CONFIG/ENABLE (otherwise the verdict is a false negative)
2. `~/Desktop/linux-kernel/init/main.c`: a call to `apple_aic1_rearm()` +
   the `irq_rearm` stamp between `apple_aic1_hw_quiesce_quiet()` and `early_boot_irqs_disabled = false`
3. `iBSSloader/scripts/apply-p105-boot-hacks.py`: a REPLACEMENT for the same was added
   (a clean tree gets the fix automatically)
4. `iBSSloader/build/initramfs-root/init`: `AIC1-REARM` added to the grep
5. `iBSSloader/dtb/p105ap.dtb` rebuilt — `initcall_debug` removed (frees ~70 KB of UART)

Backups: `*.bak-prerearm` next to every changed file.

**No storm risk**: all hardware lines stay MASKED, only the controller and routing are
switched on. A line is unmasked only by a specific driver's `request_irq()`. The sticky
IRQ 110 (seen as `EVENT pre=0x1006e`) stays under its mask.

## Build and flash

```bash
cd ~/Desktop/ipad-mini-linux
./rebuild-rearm.sh     # kernel + splice + bundle + verify the markers in vmlinux
./flash-rearm.sh       # UART capture in the background + checkm8 boot chain, log in uart_rearm.txt
```

## What to read in the log

| Line | Meaning |
|--------|----------|
| `AIC1-REARM: CONFIG=0x10773 (ENABLE=1)` | the re-arm ran, the controller is alive on entry to initcalls |
| `LATE-SMOKE: ... CONFIG=... (ENABLE=1) ... handler_entries=N` | N>0 → the CPU TAKES the IRQ exception, the pipeline is alive |
| `/proc/interrupts` non-zero for 50 | USB enumeration should just happen |
| `LATE-SMOKE: FAIL ... ENABLE=0` | the re-arm did not land — look for whoever else clears CFG |
| `LATE-SMOKE: FAIL ... ENABLE=1, handler_entries=0` | ONLY THEN does the hardware-gate hypothesis become relevant |

## A mine spotted for the future

The tree `~/Desktop/linux-kernel/init/main.c` has **diverged** from
`apply-p105-boot-hacks.py`: the script's anchors no longer match what is in the
tree (the tree has more forward declarations). `apply_replacements()` silently
skips anchors that don't match, so the build does not break — but **a clean tree
gets an older main.c**. The tree is currently the de facto source of truth for main.c.

# ═══════════════════════════════════════════════════════════════
# AFTER THE IRQ FIX (2026-09-14/15) — what started working and where next
# ═══════════════════════════════════════════════════════════════

## Result of the re-arm fix (log uart_rearm.txt)
```
AIC1-REARM: CONFIG=0x10773 (ENABLE=1) all lines masked, TARGET=CPU0, IPI live
LATE-SMOKE: CPSR=0xa0000053 (I=0) CONFIG=0x10773 (ENABLE=1) handler_entries=27
LATE-SMOKE: after SW_SET+50ms: handler_entries=28 (delta=1) -> PASS
IRQ: 50:  116  APPLE-AIC1  11 Edge  36100000.usb     Err: 0
```
`handler_entries=27` BEFORE the smoke test = 27 real hardware IRQs already taken by the CPU.
The hypothesis "the AIC1 output does not reach the nIRQ pin" — closed.

**USB enumerated all the way**: USBRst → EnumDone (high-speed) → SET_ADDRESS(5)
accepted (no STALL 25) → GET_DESCRIPTOR device/config/strings →
**SET_CONFIGURATION(1)** → SET_INTERFACE(1,alt1) → CDC ECM SET_ETHERNET_PACKET_FILTER.
macOS bound the gadget. `DCFG=0x00840050` (address 5), ep1–ep4 active.

Touch did not change: `irq/51-apple-z2` registers, but 0 hits;
SPI1 (IRQ 49) = 0. Cmwp remains the touch blocker.

## A trap in the logs
PMCCNTR is 32-bit and at ~1 GHz wraps every **~4.3 s**, so the
printk timestamps cycle WITHIN a single boot (`4.32 → 0.03`). These are not
reboots. Don't panic.

## Build N+1: CDC ACM instead of g_ether
```
config/p105ap.config:  USB_ETH off, USB_G_SERIAL=y, USB_U_SERIAL=y,
                       USB_DWC2_VERBOSE off (it was ~90% of the log)
/init:                 a respawn loop of setsid /bin/sh on /dev/ttyGS0
On the Mac:            ls /dev/cu.usbmodem* ; screen /dev/cu.usbmodem<...> 115200
```
Only one legacy gadget can bind the UDC — which is why g_ether is off.

## /bin/peek — an MMIO tool in the initramfs
The busybox in this rootfs **has no devmem applet**, so a static ARM binary
was built, `iBSSloader/tools/p105-peek.c` → `build/initramfs-root/bin/peek`
(baked into the zImage through CONFIG_INITRAMFS_SOURCE; `CONFIG_DEVMEM=y`,
`STRICT_DEVMEM` off — checked).
```
peek r    ADDR [WORDS]         dump 32-bit words
peek w    ADDR VAL             write a word
peek scan ADDR WORDS [SPIN]    two samples of a window, prints only the words that changed
```
Two things ordinary devmem does not have:
- SIGBUS/SIGSEGV are caught → reading an unpowered block (SPI1 0x321*,
  grape 0x335*) prints `--------` instead of killing the process. Exactly what
  is needed to hunt for powered blocks.
- the delay in `scan` is a busy loop, NOT a sleep. jiffies are frozen (see below), and any
  `nanosleep` would hang the shell for good.

## THE BIG ADT DISCOVERY: where to look for a timer
There is NO clockevent in the system at all — `pmccntr.c` registers only a
clocksource + sched_clock + delay_timer. No tick, jiffies frozen,
`sleep`/`msleep`/`schedule_timeout` never return (which is why /init
has `busywait`). Confirmation: `IPI1: Timer broadcast interrupts: 0`.

From `dts/apple-p105ap-raw.json`:
```
/device-tree/arm-io/pmgr   device_type = "timer"     ← !!!
                           reg[0] = 0x0F100000 +0x7000 → phys 0x3F100000
/device-tree/arm-io/wdt    reg = 0x0F103020 +0x10     → phys 0x3F103020, IRQ 4
                           (so wdt is a sub-window inside pmgr)

/device-tree/cpus/cpu0  interrupts = <192, 135, 193>
/device-tree/cpus/cpu1  interrupts = <194, 139, 195>
/device-tree/arm-io/aic target-destinations = <135 cpu0, 139 cpu1>
        function-ipi_dispatch       = <aic 'IPID' 192>   (cpu0)
        function-ipi_dispatch_other = <aic 'IPID' 193>   (cpu0)
```
Decoding the triple: **interrupts[0]=IPI self, interrupts[1]=TIMER,
interrupts[2]=IPI other**. So **IRQ 135 is CPU0's timer interrupt**
(and 139 is CPU1's), hard-wired to its core through target-destinations.

So the clockevent recipe: the timebase registers are in the PMGR window
`0x3F100000..0x3F107000` (the region is readable — confirmed by the XNU hook),
the interrupt = AIC hwirq 135. What is left is to find the exact offsets of the counter/comparator.

**How to find them without a single extra flash** — from the shell over ACM:
```sh
peek scan 3f100000 512 4000000      # free-running words = the counter
peek r    3f103000 16               # the whole wdt sub-block
peek scan 3f103000 16  4000000
```
The word whose delta grows linearly with SPIN is the timebase.
Next: look for a compare register and an enable bit nearby, hang a handler on IRQ 135.

## Peripheral map from the ADT (bus-local +0x30000000)
```
aic        0x0F200000  —        gpio  0x0FA00000 irq 119    pmgr 0x0F100000 —
wdt        0x0F103020 irq 4     pl310 0x0E000000 —          perfcounter 0x0F104000
spi1       0x02100000 irq 29    spi2  0x02200000 irq 30
uart0      0x02500000 irq 21    i2c0  0x03200000 irq 18     pwm  0x03500000 irq 16
otgphyctrl 0x06000000 —         usb-complex 0x0F104400 —
clcd       0x0A100000 irq 43,42 mipi-dsim 0x09500000 irq 41
usb-ehci   0x06400000 irq 12  ← taken by HSIC WiFi (child node "wlan"!)
usb-ohci0  0x06500000 irq 13    usb-ohci1 0x06600000 irq 14
```
Important for the USB keyboard plan: **there is no free host port**. EHCI is
WiFi over HSIC. A keyboard = dwc2 in host mode, i.e. mutually exclusive with ACM
on the same port, and almost certainly needs a working timer first
(hub_port_init is all msleep).

# ═══════════════════════════════════════════════════════════════
# THE TICK WORKS (2026-09-15) — USB SOF   (since 2026-09-18 only a fallback, see "THE TIMER INSIDE THE AIC")
# ═══════════════════════════════════════════════════════════════

```
SOF-TIMER: PASS -- clockevent registered at 8000 Hz (80 SOFs per jiffy). jiffies are live.
sleep 1 && echo ok   -> ok
```

The first sign was visual: **the fbcon cursor started blinking**. It blinks on a timer,
so with frozen jiffies it could not blink at all.

## What was tried and rejected (all checked on the hardware)
| source | verdict |
|----------|---------|
| Apple watchdog 0x3F103020 | the 24 MHz counter and the comparator WORK (RESET_EN reboots on schedule), the interrupt is not wired in any CTRL combination |
| the rest of PMGR (28 KB) | exactly one free-running register in the whole window — the same counter |
| A9 private timer PERIPHBASE+0x600 | zeros, writes don't stick — PERIPHCLK is not supplied |
| A9 global timer PERIPHBASE+0x200 | the same |
| PMU overflow | `PMCR N=6`, but `PMCEID0=0x0` — not a single architected event, CPU_CYCLES included |

Meanwhile the SCU (+0x000) and the GIC CPU interface (+0x100) in the same window answer
meaningfully (`SCU CTRL=0x2d` enable, `CFG=0x511` = two cores, CPU0 in SMP),
so the window is alive — it is specifically the timers that are dead.

## How it is done
```
arch/arm/mach-apple/apple_sof_clkevt.c   clockevent 8 kHz, rating 250
drivers/usb/dwc2/gadget.c                +GINTSTS_SOF in intmsk (under ifdef)
                                         hook BEFORE spin_lock(&hsotg->lock)
```
The hook is deliberately outside dwc2's lock: the tick handler goes into timer and
scheduler code, and holding the USB driver's lock across that is not allowed.
Registration happens only after SOFs have actually been seen, at late_initcall.
Both dwc2 edits live in apply-p105-boot-hacks.py.

## Limitations (honest)
- no tick before USB enumeration — but that was already so, no regression
- a bus suspend by the host stops the tick
- 8000 interrupts/s — a few percent of CPU, line 51 in /proc/interrupts runs into the thousands
- **host mode (USB keyboard) will kill this tick**: SOF in device mode disappears.
  In host mode dwc2 generates SOF itself, so the hook would have to be duplicated
  in hcd.c — solvable, but not free

## On the side: the CPU frequency is now measured
```
CALIB: 50000549 CPU cycles per 1200013 ticks of 24 MHz => CPU = 1000000146 Hz
```
PMCCNTR calibrated against the watchdog's 24 MHz counter at startup. The old
hardcoded guess "fixed 1 GHz" turned out right to within 0.6 ppm,
but now it is a measured fact and it goes into the clocksource, sched_clock and udelay.

# ═══════════════════════════════════════════════════════════════
# NETWORKING OVER USB WORKS (2026-09-16) — g_cdc
# ═══════════════════════════════════════════════════════════════

```
UDC: 36100000.usb state=configured function=g_cdc speed=high-speed
usb0: addr=10.55.0.2/24 carrier=1 oper=up mac=6e:f1:1c:27:f4:65
ACM nodes: /dev/ttyGS0
SOF-TIMER: PASS -- 835 SOFs counted, clockevent registered at 8000 Hz
LATE-SMOKE: PASS -- handler_entries=1015     (on September 14 it was 27)
```
On the Mac: `networksetup -listallhardwareports` → `Hardware Port: CDC Composite
Gadget`, `Device: en10`. Ping 0.58/0.76/0.99 ms, no loss.

## Why g_cdc and not configfs (THE MAIN THING, don't break it)
The tick = dwc2's SOF interrupt. SOFs only come once the host has enumerated the device,
and that happens only once the gadget has BOUND and pulled up D+. A legacy gadget
binds from its own initcall, i.e. BEFORE the `late_initcall` where
`apple_sof_clkevt` registers. A configfs gadget is bound by userspace writing to `$GADGET/UDC` —
that is `/init`, much later: the timer would not see SOFs, would refuse to register,
and the system would come up with frozen jiffies.

The same inside `/init`: between soft-disconnect and reconnect there are no SOFs, so no
jiffies, so `sleep` will NOT RETURN. That window can only be crossed with a CPU spin.

## Setting up the Mac
```bash
networksetup -listallhardwareports | grep -B1 -A2 'CDC Composite'
sudo ifconfig enN 10.55.0.1 netmask 255.255.255.0 up
ping 10.55.0.2
```

## Cost of one shell-loop iteration: ~60 µs
Measured from the log timestamps: disconnect at 2.88 s, "configured after 2 polls"
at 26.77 s → 200000 iterations of `i=$((i+1))` in ash = ~12 SECONDS. The inherited
comment "~200 ms" was a lie and on its own made up the whole long USB init.
Now `settle()` = 5000 iterations (~300 ms), `blip()` = 1700 (~100 ms).

## The IRQ numbering shifted
It was `50: ... 36100000.usb`. Now:
```
50:      0  APPLE-AIC1  29 Edge  32100000.spi
51: 107847  APPLE-AIC1  11 Edge  36100000.usb, 36100000.usb
```
The hwirqs did not change (USB = 11, SPI = 29) — only the Linux numbers moved.
Scripts that grep by number have to take both lines.

## Shell over the network — a crutch for now
The busybox in this rootfs has NO `telnetd`, `httpd`, `udhcpd` or `sshd`
(checked by grepping the binary — they are separate Alpine packages). For now there is
a FIFO trick on `nc`, port 2323. `/bin/sh` with stdin from a pipe considers itself
non-interactive and prints no prompt — hence "it hangs". Fixed with `sh -i`.
The proper solution is to put dropbear in the initramfs.

## Three defects the network shell exposed (2026-09-16, evening)
1. **The gadget's MAC is random on every boot.** The log had `6e:f1:1c:27:f4:65`,
   the next boot `da:5b:e0:9b:ef:1d`. macOS names interfaces by MAC,
   so every time a new `enN` and another `ifconfig`. Pinned in the bootargs:
   `g_cdc.host_addr=02:10:5a:05:00:01 g_cdc.dev_addr=02:10:5a:05:00:02`.
2. **`lo` comes up DOWN** — in a bare initramfs nobody brings it up. Everything
   that goes to 127.0.0.1 silently fails. `ip link set lo up` in /init.
   (It got lost again later; back in stage 1 since 2026-09-25, when the iPad
   could not ping its own 10.55.0.2.)
3. **hostname `(none)`** — simply not set. `hostname p105`.

## The DTB is now built inside rebuild-rearm.sh
It used to be a manual step, and an edit to `dts/p105ap.dts` could quietly fail to land:
the build splices whatever .dtb is lying there, and nobody complains. Now step 1/6
does `rm` + `dtc` and checks that the MACs are in the bootargs.

## SSH: why apk cannot simply be installed
The rootfs is Alpine **armhf**, the host is Apple Silicon, and the M series has **no AArch32
EL0 at all**: 32-bit ARM code runs neither natively nor in Docker. So
`apk add` against this tree can never run. `scripts/add-dropbear.sh`
downloads the .apk (it is just concatenated gzip streams of tar) and unpacks it by hand —
nothing of the target architecture is executed.

The host key (since 2026-09-19) is made **once, at build time**:
`./cascadia build` → `scripts/dropbear-hostkey.py` → `build/keep/etc/dropbear/`
→ into the image, and stage 1 carries it onto the NFS root too. The fingerprint is printed in
step 2/6. It used to be generated by `dropbear -R` on the device, and on the RAM root it
was new on every boot (`REMOTE HOST IDENTIFICATION HAS CHANGED`). After the
switch, once: `ssh-keygen -R 10.55.0.2`, then simply
```bash
ssh root@10.55.0.2
```

**The client key** no longer goes stale either: `./cascadia build` appends, every time,
the key of the machine doing the build to the image's `/root/.ssh/authorized_keys`
(`~/.ssh/id_ed25519.pub`, or `PUBKEY=...`), and every `~/.ssh/cascadia*.pub`. It used
to be put there only by `add-dropbear.sh` during `./cascadia rootfs`, and after a new `ssh-keygen` the image
met a login with a prompt for a password that does not exist (2026-09-19: the image had
`SHA256:yicA...`, the client offered `SHA256:j+r6...`; the NFS root already had the
new key, so everything worked there). "User 'root' has blank password,
rejected" in `dmesg` is only the password login being refused and has nothing to do
with keys.

# ═══════════════════════════════════════════════════════════════
# apk AND THE NFS ROOT (2026-09-17)
# ═══════════════════════════════════════════════════════════════

```
10.55.0.1:/Users/k/cascadia-root on / type nfs (rw,vers=3,nolock,proto=tcp)
df -h /   ->   926.3G, 58% used
apk update -> OK: 25140 distinct packages available
```

## The key misconception that cost a lap
"apk against this tree can never run" — true ONLY of the Mac.
The M series has no AArch32 EL0, 32-bit ARM does not run there at all. But
the device is an armv7 machine, and `/sbin/apk` has been in the Alpine minirootfs
from the start, together with the signing keys and the CA bundle. As soon as there was
a network, `apk add` just worked.

Through `apk-unpack.py` it only makes sense to install what is needed BEFORE apk:
dropbear (without it there is no shell over the network) and `mount.nfs` (a root cannot be
mounted by a binary that lives on that root). Everything else — `apk add` over ssh.

## Bring-up order
```bash
# once
./cascadia rootfs          # dropbear with your key and mount.nfs are installed here
./cascadia nfs on          # export the root from the host + /etc/nfsroot in the initramfs
                           # Mac: ~/cascadia-root, Linux: /srv/cascadia-root
./cascadia build && ./cascadia flash

# every time after flashing -- flash sets 10.55.0.1 on the host itself
# (on its own: ./cascadia link)
./cascadia net on
ssh root@10.55.0.2
```

## Pitfalls collected along the way
| what | why |
|---|---|
| `flash` without `rebuild` | uploads the old bundle; the uptime is fresh, the contents old. The freshness check now walks the WHOLE initramfs tree, not only `/init` |
| `# CONFIG_NFS_FS is not set` in the trim section | merge_config takes the last word, and the trim twenty lines further down undid the enable. Caught by `scripts/check-config-fragment.awk` |
| a `# cascadia` marker in `/etc/exports` | on macOS a comment is only a whole line; trailing text is parsed as host names |
| `PTY allocation request failed` | devpts not mounted. `/dev/ptmx` comes from devtmpfs, the slave side lives in devpts |
| the host key changes every boot | `dropbear -R` writes it to the root. On RAM — new every time, on NFS — persistent. Doubles as an indicator of which root booted |
| running `nfs on` again | the export is now a LIVE root; `rsync --delete` would wipe everything installed. Both scripts (Mac and Linux) refuse to overwrite a non-empty directory without `FORCE_SYNC=1` |
| a Linux host with docker | docker sets the FORWARD policy to DROP, and an accept in a table of our own does not save the packet: every base chain on the hook has to let it through. `linux-share-internet.sh` inserts its rules into FORWARD itself, commented `cascadia`; `off` removes exactly those |
| a Linux host with NetworkManager | NM takes the gadget's new Ethernet interface, runs DHCP for 45 s, gives up and drops the address — on Ubuntu in the middle of the boot: NFS "server not responding", ssh hangs, then "No route to host". Marking it unmanaged lost the race. `host-link.sh` creates an NM profile `cascadia-usb` once (the gadget's MAC, 10.55.0.1/24, no default route), and from then on NM itself sets the address on every enumeration. To remove: `sudo nmcli connection delete cascadia-usb` |
| ufw "running" although it is off | `ufw.service` sits in `active (exited)` even with ufw off; whether it is on is `ENABLED=yes` in `/etc/ufw/ufw.conf`, and that is what the scripts check |
| the iPad cannot ping itself (10.55.0.2) | nobody brought `lo` up, and Linux reaches its own addresses through lo. Stage 1 brings lo up now |

# ═══════════════════════════════════════════════════════════════
# THE BOOT CHAIN REPRODUCES FROM THE IPSW (2026-09-17)
# ═══════════════════════════════════════════════════════════════

```bash
./cascadia firmware    # iBSS.patched + iBEC.patched.autogo.dfu from your own IPSW
./cascadia flash       # the freshly built one
./cascadia flash --known-good   # the August image, when a bad build has to be told
                                # from a bad bench
```
The freshly built one **booted on the hardware**. The decrypted Apple firmware is no longer
needed either in the repository or next to it.

## What turned out about autogo
It is NOT `iBoot32Patcher -c "go"`: that option runs the command as soon as it is parsed,
and a 13-megabyte bundle does not have time to arrive — the loader starts after the first
32 KB chunk. The real autogo is a hand-written hook in the USB transfer completion callback,
firing only on a short packet (EOF). The `.lk` suffix = linux-boot.

Because of the hook's hard-wired address, everything is pinned to **iPad2,5 / 12H321**.

## Firmware build pitfalls
| what | why |
|---|---|
| `hook target wrong: #0x9ff43dea`, off by 0x24 | Ubuntu's gcc enables `--build-id`, the linker puts `.note.gnu.build-id` at the address from the script — exactly 36 bytes — and `.text` shifts. Fixed with `-Wl,--build-id=none`, as already done in `build-staging-bundle.sh` |
| the image built, but `iBoot32Patcher` is missing | `RUN` goes through `/bin/sh` without `{a,b,c}` expansion, the sources are at the repo root, `-I` has to be the root, and a trailing `|| true` swallowed all of it |
| the `.dfu` does not match the reference although the plaintext does | the reference is a binary saved in August, older than the current `patch-ibec-autogo.py`. Compare the plaintext, not the ciphertext: one changed block in AES-CBC spoils everything after it |
| `duser[@]: unbound variable` only on the Mac | bash 3.2 treats expanding an empty array under `set -u` as an error; 4.4 allowed it |
| over kDFU the iBEC uploads to 100%, then the device "switches off" | the image was encrypted: after iOS has booted there is no GID key, nothing to decrypt the KBAG with. `iBEC.patched.autogo.plain.dfu` is needed |

# ═══════════════════════════════════════════════════════════════
# THE TIMER INSIDE THE AIC (2026-09-18) — found in iBEC, counter confirmed
# ═══════════════════════════════════════════════════════════════

The AIC window had never been scanned — the search was in PMGR. But iBoot, unlike XNU,
reaches registers through literals, and its AIC driver reads directly
(`build/firmware/iBEC.dec`, base 0x9FF00000, all in 0x9FF01500..0x9FF01B40):

```
0x9ff01b00  timer_get_ticks:  hi=[AIC+0x28]; lo=[AIC+0x20]; hi2=[AIC+0x28]; repeat while hi!=hi2
0x9ff01b2c  tick_rate:        return 24000000
0x9ff01a68  deadline_enter:   [+0x2014]=~0; [+0x2010]|=1; [+0x2018]=1; [+0x2014]=deadline-now
0x9ff01a24  timer ISR:        [+0x2010]&=~1; [+0x2018]|=1; callback()
0x9ff018ec  IRQ dispatch:     EVENT 0x00070001 (type 7, no. 1) -> timer; then [+0x2020]=2
0x9ff019e4  init:             [+0x2010]=0xe; handler on "irq 0xC1"; [+0x2020]=2
```

| register | what it is |
|---|---|
| AIC+0x20 / +0x28 | TIME_LO / TIME_HI, 64 bits, 24 MHz |
| +0x2010 | local event config, bit0 = timer enabled |
| +0x2014 | countdown in timebase ticks |
| +0x2018 | status, writing 1 = clear |
| +0x201c / +0x2020 | local event mask set / clear, timer = bit 1 |

**Confirmed on the hardware (peek, read):** `3f200020` runs, over `sleep 1` it
adds ~24.35M — exactly like the watchdog counter `3f103020`, and their low 32 bits
match to within the delay between the reads. It is one timebase.

**Do not read `3f202004` and `3f205004` with peek**: that is EVENT; reading it acknowledges
the interrupt, and the line stays masked — it can kill USB, the tick and the session.

**Clockevent** — in `irq-apple-aic1.c`, `late_initcall(aic1_timer_init)`:
arms 10 ms first through the alias window 0x2000, then through the per-CPU 0x5000,
waits up to 200 ms on the timebase itself. Registers (rating 400, above SOF's 250)
only if the event arrived; otherwise everything stays as it was.

| line in dmesg | means |
|---|---|
| `AIC-TIMER: ... a 10 ms shot fired after ~10000 us` + `PASS` | the timer works, the units are 24 MHz ticks, the tick no longer depends on the host |
| `fired after` far from 10000 | works, but the units are different — look at the number |
| `nothing in 200 ms ... CNT a -> b` | did not fire; if CNT changed — the countdown runs but the event does not arrive |
| `unexpected local event` | an event of type 7 but with a different number — also a lead |

The acceptance test — as with SOF: after booting **pull the cable** and watch the
fbcon cursor. With the SOF tick it freezes; with the AIC timer it should keep blinking.

## Result (2026-09-18/19): it works

```
AIC-TIMER: alias window 0x2000: nothing in 200 ms. CFG was 0x0, CNT 0x0 -> 0x0, STAT 0x0
AIC-TIMER: cpu0 window 0x5000: a 10 ms shot fired after 10003 us (CFG was 0x0)
AIC-TIMER: PASS -- clockevent registered at 24 MHz, rating 400.
/sys/devices/system/clockevents/clockevent0/current_device -> apple-aic1-timer
```

- The `0x2000` alias is dead from Linux (all zeros, no event) — just like
  "the classic 0x2004 is always 0". The live copy is the per-CPU `0x5000 + (cpu << 7)`,
  so every core has its own timer.
- 10003 µs for a 10 ms shot: the units are 24 MHz ticks, iBoot's recipe is exact.
- The timer registers at **arch_initcall** — before dwc2 first
  builds its interrupt mask. dwc2 asks `apple_s5l_usb_sof_wanted()` and,
  with the AIC timer alive, **does not enable SOF**: the 8000 interrupts/s are gone, and
  `SOF-TIMER` says `not needed`. If the timer's self-test fails, everything
  falls back to SOF as before.
- LATE-SMOKE no longer "drains" EVENT at the end: reading EVENT acknowledges
  and masks a pending event, and a swallowed timer event stays masked
  forever — the tick would simply stop.

**Editing `patches/tree/0001-cascadia.patch` no longer requires re-downloading the
kernel.** `apply-kernel-edits.sh` remembers the applied version
(`<tree>/.cascadia-applied-0001-cascadia.patch`), and for a tree patched
before that record existed, it looks for the applied version in the repository's own git
history, reverts it and applies the new one. Checked in three scenarios:
an old version without a record, a repeat run, a new edit over a record.

# ═══════════════════════════════════════════════════════════════
# TOUCH POWER (2026-09-18/19): the gates were 10 ids lower
# ═══════════════════════════════════════════════════════════════

Everything that spent three weeks trying to switch on SPI1 and PWM wrote to the wrong register.
Not the wrong bit and not too early — the wrong register, by exactly 10 ids.

```
power-state register = 0x3f100fd8 + <id from ADT clock-gates> * 4
```

Found by brute force from a live Linux (`tools/pmgr-map.sh`): dump the whole PMGR window,
then switch on, one at a time, every register that looks like a switched-off device, and
after each one check whether the block started answering.

```
SPI1 (0x32100000) came alive on a write to 0x3f1010e8  ->  ADT gate 68
PWM  (0x33500000) came alive on a write to 0x3f101124  ->  ADT gate 83
```

| Where the formula came from | Formula | Where it landed for spi1 (68) |
|---|---|---|
| measured | `0x3f100fd8 + id*4` | `0x3f1010e8` — spi1 |
| XNU disassembly | `pmgr + 0x1000 + id*4` | `0x3f101110` — a hole, no register |
| iBEC helper | `pmgr + 0x1008 + id*4` | `0x3f101118` — **i2c0**, already on |

Hence the old "the gate reads as on (`g68=2FF`) but the block is dead": what was being read was
the gate of the I2C controller the PMU hangs off, switched on long before us.

**The array:** ids 12..90, 71 registers, holes at 29, 30, 63–66, 71, 78. Above id 90
there are no registers at all — the display (clcd 103/127), sgx, isp are switched by something
else (candidates: the bitmasks `0x3f101200/0x1204`, and `0x3f101180`, which
XNU sets bit 31 in). Bit 9 is set in every implemented register; a solid
zero does not mean "off" but "there is no register here".

**How to switch on** (XNU's `clock_gate_switch` recipe, it was always right):

```
write (v & ~0x10f) | 0xf, then wait until bits 7:4 equal bits 3:0
```

**How a dead block reads.** Not a constant: the bus returns whatever last travelled
over it — seen `0xd00c3ccc`, `0x0015006b`, `0x006b18f4`. So the
"is the block alive" check is a comparison with a read of `0x38000000`, where there is nothing:
the same twice in a row = dead. Once switched on, both blocks read as zeros.

**The array's state on a fresh Linux boot** (what iBoot left):
on: uart0–uart3, uart5, i2c0–i2c2, the flash controller, usb-complex (87–89);
off: spi1, spi2, pwm, pke, sha2, iop.

**Scripts** (all run over ssh: `ssh root@10.55.0.2 sh -s < tools/<script>`):

```
tools/touch-power-probe.sh   the state as booted + the old recipe
tools/pmgr-map.sh            the brute force that found the formula
tools/touch-power-on.sh      switch on spi1 and pwm, read the blocks' registers
```

**What this does not solve.** `Cmwp` is still needed, no longer for power but for
programming: PWM channel 2 has to put out 32768 Hz (ADT
`/arm-io/pwm/grape-clk`: `reg = 2`, `default-hz = 0x8000`). And the clock-id →
register map is still a guess (`0x3f100010/20/2c` for ids 4/304/307): the gates turned out
to be 10 lower, and the clocks get the same distrust.

# ═══════════════════════════════════════════════════════════════
# TOUCH WORKS (2026-09-25) — /dev/input/event0 from boot
# ═══════════════════════════════════════════════════════════════

Everything written above about touch ("events do not come", Cmwp, SPI1 abort) is out of date.
Details — `docs/research/p105-z2-boot.md`; here only what needs remembering.

**How it works.** stage2 starts `/sbin/z2-boot -c /lib/firmware/mtcal.bin`:
it loads the chip the same way iOS does and hands it to the `apple-z2` driver (`READY`),
from there the kernel reads the frames. Touches go to `/dev/input/event0` as standard
multitouch (`ABS_MT_*`, `BTN_TOUCH`), 60 frames a second, several fingers.

**The chip's firmware** (`P105.mtprops`) is Apple's and is not in the repo: `./cascadia firmware`
takes it out of the IPSW (`scripts/rootfs-extract.py`: encrcdsa → UDIF → HFS+,
pure Python, the 12H321 RootFS key is public) into `build/firmware/`, and the build puts it
into the image. Byte-identical to the copy from iOS (md5 in `build-firmware.sh`).

**Every iPad has its own calibration** (syscfg `MtCl`, 1024 bytes), and Linux cannot
read it. The image carries by default the calibration of the iPad everything was brought up on
(`initramfs/lib/firmware/mtcal.bin`); not tried on other iPads. Your own —
optional, from the jailbroken iOS of that same device, from a Mac or Linux:
```bash
./cascadia mtcal          # iPad in iOS, on the cable; iOS root password once
./cascadia build          # build/keep overrides the default
```
It lands in `build/keep/lib/firmware/`. What runs on the iPad is
`tools/mtdump/prebuilt/mtcal` (source `mtcal.c`, no SDK headers): the binary
is kept in the tree because only Xcode can build armv7 for iOS — ld64.lld has
only BR24/BR22 for armv7, the other relocations are FIXMEs, and ld64
on Linux needs libtapi. Rebuild: `tools/mtdump/build.sh` on a Mac.
No firmware (`./cascadia firmware` was not run) — touch does not start, and stage2 says so in dmesg.

**What it took** (one item each; without any one of them the chip does not scan):
- the calibration at `0x10009000` before `performCalibSeq`;
- the N1 bootloader defaults: `0x1000305c <- 0x20`, `0x10003000 <- 3` (never "0 at address 0":
  that is the firmware's reset vector, after EXECUTE you get `4f81`);
- after EXECUTE: `ee`, `e2`×2, GET `d1 d3 d0 a1 d9`, then **SET `bf = 9b 0b 0b 02`, `af = 00`** ×2 —
  in iOS userspace sets them, and they are what starts the scan;
- **PMU LDO idx 15** (`0x22` bit `0x02`, the 5-volt group) — in no ADT function at all,
  iOS keeps it on; in the DTS it goes together with the analog rail (`touch_ana`, mask `0x06`);
- reading a frame the way iOS does, with a retry: the first frame of a touch is often "not ready".

**Ground truth from iOS, without patches:** `build/mtlog` — the touch driver's own trace
(every step and every SPI transfer) through its user client; locking and unlocking the
iPad reloads the chip into that same log. The PMU registers of a live iOS — `mtdump`,
the `AppleRegisterDump` property of the `AppleARMPMUCharger` class.

**On the side:** `irq-apple-aic1.c` no longer switches off the whole AIC over one empty
interrupt — that is exactly what used to bring down USB/network the moment scanning started.

**Debugging:** `echo 300 > /sys/module/apple_z2/parameters/debug` turns on tracing of
frames and touches in dmesg; `z2-boot -p N -N` reads the frames itself, without the kernel.

# ═══════════════════════════════════════════════════════════════
# LINUX HOSTS, A DESKTOP (2026-09-25)
# ═══════════════════════════════════════════════════════════════

- `./cascadia net` / `nfs` have Linux twins (`tools/linux-*.sh`); walked on Arch.
  The pitfalls from Ubuntu (docker's FORWARD DROP, NetworkManager, ufw) are in the
  "Pitfalls collected along the way" table above.
- The NFS root stalling ("server not responding"): caught live on the Mac on
  2026-09-26, and it is the iPad's RPC client, not the server. pf's state for the
  connection read `ESTABLISHED:FIN_WAIT_2` — the Mac had closed its end and the
  iPad had acknowledged the FIN — and a minute of tcpdump on en10 showed nothing
  from the iPad but TCP keepalives on that same socket: no FIN of its own, no
  SYN, no request. The client saw the server hang up and never closed or
  reconnected. Why is still open; stage 2 now traces the RPC transport's
  connection events from boot (`cat /sys/kernel/tracing/trace`, from the ACM
  console). Not a pf problem: six dead connections from earlier boots sat in
  pf's table as `ESTABLISHED:ESTABLISHED`, harmless. Never seen on the Arch host.
- The ACM console hung along with NFS — keystrokes echoed, nothing ran: an
  interactive ash reads `$HOME/.ash_history` at start, and HOME was on the NFS
  root. It now runs with HOME, history and cwd in RAM, and `/run` and `/tmp`
  are tmpfs (they were plain directories on the server, carrying the last
  boot's sockets and pid files into the next).
- XFCE runs with touch as the pointer (first brought up by teutekeune):
  `ssh root@10.55.0.2 sh -s < tools/desktop/xfce-setup.sh`, then `desktop` on the
  iPad. What the boot adds for it: stage 2 starts eudev when installed — Xorg finds
  input devices only through udev.

| what | why |
|---|---|
| XFCE ignores the glass | no udev running: Xorg's input hotplug is udev only |
| evdev's `EmulateThirdButton` never fires | for a device with multitouch axes X emulates the pointer from touches itself; evdev's button logic is not in the path. Right click is touchegg's two-finger tap, over libinput |
| a black screen for over a minute at login | xfdesktop rasterising XFCE 4.20's SVG wallpaper on one core. `desktop` writes a solid-colour `xfce4-desktop.xml` before a first session; the monitor is `monitordefault` (fbdev: no RandR outputs, X names its made-up one "default") |
| `Failed to execute command "kbd-toggle"` | the session's PATH is whatever started X; ssh's has no `/usr/local/bin`. Launchers use absolute paths |
| `pkill -x svkbd-mobile-intl` matches nothing | the kernel keeps 15 characters of a process name; the name is 17. `pkill -f` |
| hundreds of zombies, and `exit` on the glass panics the kernel | stage 2 used to `exec` the glass shell as PID 1: it never reaped orphans, and PID 1 exiting is a panic. Now PID 1 respawns the shell and waits on it, and ash's wait (waitpid(-1)) reaps the orphans |
