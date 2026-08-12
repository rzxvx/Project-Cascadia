# Project Cascadia
Native Linux on Apple A5 — the first step of the Cascadia project (A5 → A6 → A12/A13)

# Project Cascadia — Native Linux on Apple A5

> **Status: Work in Progress**

## What is this?

Cascadia is an attempt to run native, mainline Linux on Apple A5-based 
devices — starting with the iPad mini 1 (iPad2,5). 

As of writing, no prior Linux port exists for the A5 (S5L8942X). 
PostmarketOS covers A8+, Project Sandcastle targeted A10, but A5 has 
been a complete blind spot. Cascadia aims to change that.

The goal is not just to boot Linux — it's to build a foundation for 
future ports to A6, A10, and eventually A12/A13 (via usbliter8), 
documenting everything along the way so others can build on it.

## Why A5?

- checkm8 BootROM exploit covers A5 (permanent, hardware-level)
- No prior Linux work — genuinely uncharted territory  
- A5 uses Samsung-derived IP blocks (UART, cache) with partial 
  open documentation — more approachable than newer Apple-custom silicon
- PowerVR SGX543 GPU has some open documentation (future goal)

## Supported Devices

| Device | Model | Chip | Board ID |
|--------|-------|------|----------|
| iPad mini 1 (Wi-Fi) | iPad2,5 / A1432 | Apple A5 (S5L8942X) | p105ap |

More devices planned as the project matures.

## Current Status

- [x] checkm8 via Raspberry Pi Pico (checkm8-a5)
- [x] Full boot chain working: pwned iBSS → iBEC → Ramdisk → DeviceTree → Kernel
- [x] Linux kernel confirmed to start (USB stack dies = execution transferred)
- [x] Apple DeviceTree fully parsed, all hardware addresses extracted
- [x] Custom Linux DTS written (memory, UART0, framebuffer, PL310 L2)
- [x] complzss + img3 packaging pipeline working
- [ ] Serial output via DCSD cable ← current blocker
- [ ] Kernel panic diagnosis
- [ ] Framebuffer console
- [ ] Userspace / shell

## Roadmap

**Phase 1 (current):** Get kernel output. Diagnose and fix panics.  
**Phase 2:** Framebuffer console. Basic shell over SSH/serial.  
**Phase 3:** A6 port (iPhone 5 / iPad mini 2). Build on Phase 1 foundation.  
**Phase 4:** A12/A13 via usbliter8 (longer term).

## Technical Notes

Hardware addresses extracted directly from Apple's DeviceTree:

| Peripheral | Physical Address | Notes |
|-----------|-----------------|-------|
| UART0 | 0x32500000 | boot-console, debug-console |
| AIC | 0x3F200000 | Apple proprietary — no mainline driver |
| PL310 L2 | 0x3E000000 | ARM standard — mainline driver exists |
| Framebuffer | 0x9F6FC000 | confirmed via iBEC getenv |
| RAM base | 0x80000000 | confirmed via iBEC bootx logs |
| RAM size | 512MB | |

The AIC (Apple Interrupt Controller, "aic,1") is the main remaining 
challenge — no mainline driver exists for this revision. 
A12+ AIC is already in mainline (for Apple Silicon), but the older 
"aic,1" used in A5 is different and will require a custom driver.

## Background

In May 2026, while looking for existing Linux ports for the iPad mini 1, 
I found nothing. PostmarketOS stops at A8, Project Sandcastle targeted A10, 
and the A5 had been completely ignored — likely because it's 32-bit, old, 
and "not worth it".

That's exactly why it's interesting.

The A5 (S5L8942X) is covered by checkm8, uses Samsung-derived IP blocks 
with partial open documentation, and has PowerVR SGX543 GPU which has more 
public info than Apple's custom silicon. It's arguably one of the more 
approachable chips for a from-scratch Linux port.

This project started as an experiment to see how far you can get. 
Turns out — pretty far.

## Credits

- **axi0mX** — checkm8 BootROM exploit, without which none of this is possible
- **LukeZGD** — Legacy iOS Kit, EverPwnage, checkm8-a5 Pico firmware
- **NyanSatan** — checkm8_bootkit, extensive iBoot research
- **iH8sn0w** — iBoot32Patcher

## License

GPL-2.0
