# Project Cascadia — Native Linux on Apple A5

Mainline Linux 6.12 on an iPad mini 1 (iPad2,5 / S5L8942X), booting to an
interactive shell — on the glass and over USB.

> **Status: Phase 1 & 2 complete.** Linux boots, takes interrupts, keeps time,
> enumerates as a USB gadget, and gives you a shell over the Lightning cable.

![Boot](imgs/boot.png)

## What is this?

Cascadia is an attempt to run native, mainline Linux on Apple A5-based devices —
starting with the iPad mini 1 (iPad2,5).

As far as I can tell, no prior Linux port exists for the A5 (S5L8942X).
postmarketOS covers A8+, Project Sandcastle targeted A10; the A5 has been a
blind spot. Cascadia aims to change that.

The goal is not just to boot Linux — it's to build a foundation for future
ports to A6, A10 and eventually A12/A13, documenting everything along the way,
including the parts that didn't work.

## Why A5?

- checkm8 BootROM exploit covers A5 (permanent, hardware-level)
- No prior Linux work — genuinely uncharted territory
- A5 uses Samsung-derived IP blocks (UART, cache) with partial open
  documentation — more approachable than newer Apple-custom silicon
- PowerVR SGX543 GPU has some open documentation (future goal)

## Supported Devices

| Device | Model | Chip | Board ID |
|--------|-------|------|----------|
| iPad mini 1 (Wi-Fi) | iPad2,5 / A1432 | Apple A5 (S5L8942X) | p105ap |

## Current Status

- [x] checkm8 via Raspberry Pi Pico (checkm8-a5)
- [x] Full boot chain: primepwn → autogo iBEC → staging-bundle → Linux
- [x] Linux 6.12 boots on hardware
- [x] Apple DeviceTree fully parsed, all hardware addresses extracted
- [x] Custom Linux DTS (memory, UART0, AIC1, simplefb, PL310 L2, dwc2, SPI, I2C)
- [x] Custom AIC1 interrupt controller driver
- [x] **Interrupts actually reach the CPU** — see *The AIC problem* below
- [x] **A working system tick** — see *The timer problem* below
- [x] **Correct wall-clock time** — calibrated 24 MHz clocksource + sched_clock
- [x] Serial output via DCSD cable — kernel logs confirmed
- [x] simplefb framebuffer — `/dev/fb0`, Tux on screen, blinking cursor
- [x] Alpine Linux 3.24 embedded initramfs
- [x] Interactive shell on tty0 (framebuffer console)
- [x] **USB gadget enumerates** — dwc2 peripheral mode, full SET_CONFIGURATION
- [x] **Interactive shell over USB** — CDC ACM, `screen /dev/cu.usbmodem* 115200`
- [x] **USB networking** — CDC ECM on the same port as the console (`g_cdc`),
      10.55.0.2, ~0.7 ms RTT to the host
- [x] **SSH** — dropbear, key-only, over the ECM link
- [x] **`apk` works on the device** — the whole Alpine repository, over the cable
- [x] **NFS root** — the root filesystem lives on the host's disk, so installed
      packages survive a reboot and the disk is no longer 512 MB of RAM
- [x] `/bin/peek` — MMIO poke tool in the initramfs for live hardware probing
- [x] **The boot chain regenerates from a stock IPSW** — `./cascadia firmware`
      decrypts and patches iBSS/iBEC from the user's own firmware, verified
      byte-for-byte and booted on hardware. No Apple binaries in this repository
- [ ] Touch input — blocked on the Cmwp touch clock
- [ ] Wi-Fi (BCM4334 — HSIC, behind EHCI, not SDIO as initially assumed)
- [ ] CPU1 / SMP bringup — **parked**, see *Negative results*
- [ ] USB host mode / keyboard — no free host port, and it would kill the tick

## Three problems worth reading about

Most of the interesting work in this port was not writing drivers. It was
finding out why perfectly correct drivers did nothing.

### 1. The AIC problem — no interrupt ever reached the CPU

For weeks, `/proc/interrupts` was zero on every line, IPIs included. USB never
enumerated, touch never fired. The working theory was a hardware gap between
the AIC output and the CPU's nIRQ pin.

It was not hardware. `kernel_init()` carries two Apple-specific blocks, and the
second one runs immediately before `do_initcalls()`:

```c
apple_aic1_quiesce();           /* MASK_SET all, TARGET_CPU=0 all, drain, CFG &= ~ENABLE */
apple_aic1_hw_quiesce_quiet();  /* ...and again through a static mapping */
early_boot_irqs_disabled = false;
local_irq_enable();
```

Nothing turns the controller back on. By the time any driver called
`request_irq()`, `AIC1_CONFIG.ENABLE` was 0 and every line's `TARGET_CPU` was 0 —
no core selected. A second, independent bug: `aic1_irq_unmask()` wrote only
`MASK_CLR` and never touched `TARGET_CPU`, so even an unmasked line had no route.

The fix is `apple_aic1_rearm()` — mask every line, point every `TARGET_CPU` at
CPU0, drain `EVENT`, unmask IPIs, re-enable the controller — called right before
`early_boot_irqs_disabled = false`, plus a `TARGET_CPU` write in the unmask path.

```
AIC1-REARM: CONFIG=0x10773 (ENABLE=1) all lines masked, TARGET=CPU0, IPI live
LATE-SMOKE: CPSR=0xa0000053 (I=0) handler_entries=27
IRQ: 50:  116  APPLE-AIC1  11 Edge  36100000.usb
```

27 real hardware interrupts had already been taken before the smoke test even
ran. The hardware had been fine the whole time.

### 2. The timer problem — the SoC has no usable timer interrupt

With interrupts working, the system still had no clockevent: `jiffies` frozen,
`msleep()` never returning, `sleep` hanging forever. Every plausible timer source
on this chip was tried on real hardware and every one of them failed (table
below).

The tick that actually works comes from an unlikely place: **the USB
controller's Start-of-Frame interrupt**. At high speed, dwc2 sees one SOF per
125 µs microframe. That is a free, hardware-accurate 8 kHz interrupt, and it is
enough to build a clockevent on.

```
arch/arm/mach-apple/apple_sof_clkevt.c   clockevent, 8 kHz, rating 250
drivers/usb/dwc2/gadget.c                +GINTSTS_SOF in intmsk, hook before
                                         spin_lock(&hsotg->lock)
```

The hook deliberately sits outside the dwc2 lock — the tick handler descends
into timer and scheduler code, and holding a USB driver lock across that is not
acceptable. Registration happens at `late_initcall`, and only after real SOFs
have been observed, so a failed USB bringup can't leave a dead clockevent
registered.

The first sign it worked wasn't a log line — it was the framebuffer cursor
starting to blink.

Honest limitations: there is no tick before USB enumerates; a host bus suspend
stops it; 8000 interrupts/second costs a few percent of CPU; and switching dwc2
to host mode would kill it (the hook would have to be duplicated in `hcd.c`).

### 3. The clock problem — `sleep 1` took 14 seconds

With a tick running, wall-clock time was still wrong by an order of magnitude.
The clocksource was PMCCNTR, the Cortex-A9 cycle counter — and PMCCNTR **stops
in WFI**. Every idle period simply vanished from the kernel's notion of time.

The replacement came out of the failed watchdog investigation. The Apple
watchdog block at `0x3F103020` turned out to contain the only free-running
register in the entire 28 KB PMGR window: a 24 MHz counter. A scan of all 7168
words confirmed there was nothing else.

```
CALIB: 50000549 CPU cycles per 1200013 ticks of 24 MHz => CPU = 1000000146 Hz
```

So PMCCNTR is now calibrated against that counter at boot (the old hardcoded
"1 GHz guess" turned out to be right to 0.6 ppm — but it's a measured fact now,
and it feeds `udelay`), and the 24 MHz counter itself became the clocksource
(`apple-wdt-24m`, rating 350) and `sched_clock`. It keeps counting in WFI.

Verified by stopwatch.

## Negative results

Kept deliberately. Knowing what doesn't work on this silicon is most of the value.

| Candidate timer source | Verdict |
|---|---|
| Apple watchdog @ `0x3F103020` | 24 MHz counter and comparator both **work** — `RESET_EN` reboots the device on schedule. The interrupt half is simply not wired on wdt v1: every combination of `{+0x04, +0x08} × CTRL {0x1, 0x8, 0x9, 0xc}` was tried, `IRQ_STATUS` never latched, AIC hwirq 4 never fired. PMGR gate for clock-id 4 reads `0x2ff` (on), so it isn't gating. |
| Rest of the PMGR window (28 KB) | Exactly one free-running register in the whole window — the same counter. |
| A9 private timer `PERIPHBASE+0x600` | Reads zero, writes don't stick. PERIPHCLK is not supplied. |
| A9 global timer `PERIPHBASE+0x200` | Same. |
| PMU overflow (nPMUIRQ, hwirq 135) | `PMCR N=6` but `PMCEID0=0x0` — **no architected events implemented at all**, CPU_CYCLES included. The counter never counts, so it can never overflow. |

The A9 private memory region is alive — SCU at `+0x000` reports `CTRL=0x2d`,
`CFG=0x511` (two cores, CPU0 in SMP), and the GIC CPU interface at `+0x100`
answers sensibly. It's specifically the timers that are dead.

**SMP is parked.** CPU1 is held in reset and the release mechanism lives in
SecureROM; ~20 lab iterations against PMGR `function-enable_core` and AIC
`IPI_SEND` produced nothing. Details in `docs/research/p105-smp-bringup.md`.

**No free USB host port.** The ADT puts the Wi-Fi part (`wlan`) as a child node
of `usb-ehci` — BCM4334 is HSIC-attached, not SDIO. So a USB keyboard would have
to come from dwc2 in host mode, which is mutually exclusive with the ACM console
on the same port and would take the SOF tick with it.

## Technical notes

Every address below was read out of Apple's DeviceTree, not transcribed from a
datasheet.

| Peripheral | Physical | Notes |
|---|---|---|
| UART0 | `0x32500000` | boot-console, `earlycon=s3c6400`, IRQ 21 |
| AIC | `0x3F200000` | Custom `aic,1` driver (not the arm64 mainline one) |
| dwc2 USB | `0x36100000` | IRQ 11 → hwirq 50. Peripheral mode. Source of the tick |
| OTG PHY | `0x36000000` | Register map recovered empirically from live iBoot DFU |
| Watchdog / 24 MHz counter | `0x3F103020` | IRQ 4 (dead). Counter is the clocksource |
| PMGR | `0x3F100000` | ADT `device_type = "timer"`. Not modelled — iBoot leaves our clocks ungated |
| PL310 L2 | `0x3E000000` | Left enabled by iBoot; registering `outer_cache` with L1 D-cache off hangs |
| Framebuffer | `0x9F6FC000` | 768×1024, stride 3072, a8r8g8b8 |
| SPI1 (touch) | `0x32100000` | IRQ 29 |
| GPIO | `0x3FA00000` | IRQ 119 |
| RAM base | `0x80000000` | 512 MB |

Boot chain: `primepwn` → patched `iBSS` → `iBEC.patched.autogo.lk.dfu` →
`staging-bundle.bin` → `staging-loader.bin` → `zImage-dtb` at `0x80008000`.
Built on teutekeune/iBSSloader.

### apk runs on the device, not on the build host

Worth stating because it caused a wrong turn here: an Alpine armhf `apk` cannot
run on an Apple Silicon build host — there is no AArch32 EL0 on M-series, so no
32-bit ARM code executes there at all, natively or under Docker. That is a fact
about the host and only about the host. On the device `apk` is a native binary,
and the Alpine minirootfs already ships it along with the signing keys and a CA
bundle. Once there was a network, `apk add` simply worked.

What genuinely cannot come from apk is anything needed *before* apk can run:
dropbear (no network shell without it, no convenient apk without a shell) and
`mount.nfs` (the root filesystem cannot be mounted by a binary that lives on the
root filesystem). Those two are unpacked into the initramfs by
`scripts/apk-unpack.py`, which resolves shared-library dependencies out of
APKINDEX and the ELF headers without executing anything. Everything else is
`apk add` over ssh.

### The tick and the USB gadget are coupled

This is the sharpest edge in the port, and it is not obvious from any one file.

Because the tick is the SOF interrupt, and SOFs only arrive once the host has
enumerated the device, and the host only enumerates once a gadget driver binds
and pulls up D+ — **the gadget must bind before `apple_sof_clkevt` registers at
`late_initcall`.** A legacy gadget (`g_cdc` here) binds from its own initcall
and satisfies that. A configfs gadget is bound by userspace writing to
`$GADGET/UDC`, which happens in `/init`, long after `late_initcall`: the tick
would look for SOFs, find none, decline to register, and the machine would come
up with frozen jiffies and no way to `sleep`. Do not port this to configfs
without first making the clockevent registration deferrable.

The same trap exists inside `/init`. Between a soft disconnect and the
reconnect there are no SOFs, so there are no jiffies, so `sleep` never returns.
That window has to be crossed with a CPU spin. Both facts are commented at the
places where someone would otherwise "clean up" the code.

Two more gotchas worth recording:

- PMCCNTR is 32-bit at ~1 GHz, so printk timestamps wrap every ~4.3 s. A log
  that goes `4.32 → 0.03` is not a reboot.
- A shell arithmetic loop costs about 60 µs per iteration on this CPU. The spin
  delays in `/init` are sized from that measurement, not from a guess — an
  inherited "~200 ms" comment turned out to be 12 seconds and was, by itself,
  the entire slow USB bring-up.

## Building

Needs git, docker (with a running daemon), python3 and rsync.  Everything else
happens inside the build image.  Verified on macOS/arm64 and Arch Linux/x86_64.

```bash
git clone https://github.com/rzxvx/Project-Cascadia.git
cd Project-Cascadia
./cascadia doctor     # says what this machine is missing
./cascadia kernel     # clone Linux, pinned to v6.12
./cascadia rootfs     # build the Alpine armhf rootfs for the initramfs
./cascadia build      # dtb + kernel + output/staging-bundle.bin
```

The kernel pin matters. Every edit to an existing kernel file is one patch
(`patches/tree/0001-cascadia.patch`) applied with `git apply`, so it either
applies or says why. Whole new files are copied separately. This replaced
anchor-matched insertion, which skipped a stale anchor silently — and the first
thing it skipped was the `apple_aic1_rearm()` call, so a clean tree built green
and took no interrupts at all. `./cascadia build` re-checks both the config
symbols and the boot stamps in `vmlinux` for that reason.

Flashing needs the device, a Lightning cable, a way into pwned DFU, and
[Legacy iOS Kit](https://github.com/LukeZGD/Legacy-iOS-Kit) for `primepwn` and
`irecovery`:

```bash
./cascadia firmware   # patch iBSS/iBEC out of your own IPSW
./cascadia flash      # iBSS -> iBEC -> bundle -> loader
ssh root@10.55.0.2
```

`flash` ends by giving this machine `10.55.0.1` on the gadget's network
interface and waiting for the device to answer; `./cascadia link` does only
that step. It has to happen on every boot, because the interface is recreated
each time the device enumerates, and without it `ssh` does not fail, it hangs.

Reaching pwned DFU has two routes. checkm8 on A5 needs hardware that drives USB
with tighter timing than a general-purpose host manages — a Raspberry Pi Pico,
or an Arduino with a USB host shield. If you have neither, `./cascadia flash
--kdfu` goes the other way: from a jailbroken iOS, `kloader` boots a patched
iBSS directly, with no extra hardware.
[EverPwnage](https://github.com/LukeZGD/EverPwnage) jailbreaks A5 on iOS
7–9.3.6 untethered, so the device comes up jailbroken every time and this stays
a one-command step. Both routes are Legacy iOS Kit's; Cascadia calls it rather
than reimplementing either.

The two routes do not take the same image. Anything sent after kDFU has to be
unencrypted: once iOS has booted, the AES GID key is gone, so a stock-layout
KBAG decrypts to nothing and the iBSS jumps into garbage — while `irecovery`
reports a clean 100% upload and the device simply looks switched off. So
`./cascadia firmware` packs the patched iBEC twice, once like the stock image
and once as a plain img3 with no KBAG, and `--kdfu` picks the second. Legacy
iOS Kit's own pwned iBSS is built the same way, which is the reason kDFU works
there at all.

No Apple firmware ships with this repository. `./cascadia firmware` derives the
boot chain from an `iPad2,5_8.4.1_12H321_Restore.ipsw` you supply, and checks
the result against reference hashes. The pin to that build is not arbitrary: the
auto-go hook patches an address inside that exact iBEC.

The host-side network and NFS helpers (`./cascadia net`, `./cascadia nfs`) are
still macOS-only and say so.

## Repo layout

```
patches/files/  new source files, copied verbatim into the kernel tree
                (irqchip/, mach-apple/, phy/) — the core of the port
patches/*.patch generated diffs of the glue edits, for reference
config/         p105ap.config — the kernel config fragment
scripts/        patch application + build + bundle scripts
dts/            p105ap.dts — hand-written, generated from the Apple ADT
initramfs/      /init for the embedded Alpine rootfs
tools/          p105-peek.c (MMIO tool), build/flash wrappers, LZSS helpers
docs/           CASCADIA-CHEATSHEET.md — the real reference. Start there
docs/research/  one file per investigation; several are dead ends, on purpose
pongo/          pongoOS module (ADT read, DT fixup)
mt-hook/        XNU multitouch hook (RE infrastructure)
attic/          superseded experiments, kept for provenance — see attic/README.md
```

Build products (`output/`) and stock firmware are not tracked; everything in
`output/` is reproducible from `patches/ + config/ + scripts/`.

## Roadmap

- **Phase 1 ✓** — Linux boots to an interactive shell. Serial logs. Framebuffer console.
- **Phase 2 ✓** — USB gadget, CDC ACM shell, CDC ECM networking, SSH, working
  tick, correct wall clock, `apk`, and an NFS root on the host's disk.
- **Phase 3** — Touch (unblock the Cmwp clock) → a tick that does not depend on
  USB device mode → Wi-Fi via HSIC/EHCI.
- **Phase 4** — A6 port (iPhone 5 / iPad mini 2), on this foundation.
- **Phase 5** — A12/A13, longer term.

SMP is not on the roadmap until the SecureROM core-release path is understood.

## Background

In May 2026, while looking for an existing Linux port for the iPad mini 1, I
found nothing. postmarketOS stops at A8, Project Sandcastle targeted A10, and
the A5 had been completely ignored — likely because it's 32-bit, old, and "not
worth it".

That's exactly why it's interesting.

This project started as an experiment to see how far you can get. Turns out —
pretty far.

## Credits

- **teutekeune** — iBSSloader: kernel patches, boot chain, the original AIC1 driver
- **axi0mX** — checkm8 BootROM exploit
- **LukeZGD** — Legacy iOS Kit, EverPwnage, checkm8-a5 Pico firmware
- **NyanSatan** — checkm8_bootkit, iBoot research
- **iH8sn0w** — iBoot32Patcher
- **Project Sandcastle** — Z2 multitouch protocol reference

## License

GPL-2.0
