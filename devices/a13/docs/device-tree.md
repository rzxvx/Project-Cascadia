# Device tree (N104AP / t8030)

Our own work, derived from this device's Apple ADT dump

## First-boot DT (done 2026-10-29)

`dts/t8030-n104-firstboot.dts` -> `build/t8030-n104-firstboot.dtb` (compiles
clean with `dtc` 1.8.1, round-trips). Scope: the minimum for m1n1 -> Linux to
reach an **early console on UART0**. No PMGR, display, USB or other peripherals.

```bash
dtc -I dts -O dtb -o build/t8030-n104-firstboot.dtb dts/t8030-n104-firstboot.dts
```

Values taken straight from the ADT:

| Thing | ADT | DT |
|---|---|---|
| arm-io base | ranges child 0 -> parent 0x2_0000_0000 | children carry full phys addr |
| UART0 | `uart-1,samsung` reg 0x35200000/0x4000, irq 435 | `apple,s5l-uart` @ 0x235200000 |
| AIC | `aic,1` v2, reg 0x3b100000/0x8000 + 0x3b108000/0x1000 | `apple,aic2` @ 0x23b100000 |
| CPUs | cl0 "E" apple,thunder x4 (reg 0x0..0x3); cl1 "P" apple,lightning x2 (reg 0x100,0x101) | 6 cpu nodes, same MPIDR aff |
| DRAM | t8030 base, 4 GiB | memory@800000000 = 0x8_0000_0000 + 0x1_0000_0000 |

## Open items (verify against the kernel we build)

- **SoC / AIC compatibles + AIC `#interrupt-cells`**: used mainline-style
  `apple,t8030` / `apple,aic2` with 3 interrupt cells. Must match the t8030
  kernel's bindings; earlycon is polled so this does not block first console.
- **UART ref clock**: placeholder `fixed-clock` 24 MHz. Real clock is PMGR/NCO
  (`clock-ids: 0x400000150` in ADT) - fix when PMGR lands.
- **CPU bring-up**: `enable-method = "spin-table"`; m1n1 sets the release
  addresses. SMP is a Phase 3 item; first boot only needs cpu0.
- **No simple-framebuffer yet**: iBoot's framebuffer base/size/stride are only
  known at runtime; m1n1 can fill a `/chosen/framebuffer`. Add once m1n1 boots.

Next: once m1n1 hands off (user-driven, see `first-boot.md`), boot this DTB with
a tiny initramfs and confirm earlycon; then grow the tree (PMGR, USB gadget, fb).
