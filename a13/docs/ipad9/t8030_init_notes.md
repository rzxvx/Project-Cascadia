# T8030 (A13 / iPhone 11) low-level init notes for an m1n1 port

Date: 2026-10-17. Ground-truth register map and init sequences for the T8030 SoC,
to write the missing m1n1 port **without hardware**. Cross-referenced from four
independent sources (tagged on every value):

| tag | source |
|-----|--------|
| **[dts]** | `yhavry/Hoolock-linux` branch **`t8030`** @ `2f2662c87fff`, `arch/arm64/boot/dts/apple/t8030.dtsi` (+ `t8030-pmgr.dtsi`, `t8030-common.dtsi`, `t8030-n104.dts`). A **real, working Linux T8030 port** — the primary source for bases + exact `compatible` strings. |
| **[m1n1]** | `HoolockLinux/m1n1` branch **`idevice`** @ `d5a10ac5` — the generic `pmgr.c` / `dart.c` / `aic_regs.h` / `midr.h` drivers (authoritative register layout + enable algorithm). |
| **[qemu]** | `TrungNguyen1909/qemu-t8030` @ `fd4b0f79` device models (`hw/{char,intc,arm}`). |
| **[live]** | read back over the QEMU monitor (`xp`) from iOS 14.0 (18A5351d) running on the emulator bench — "iOS actually programmed this". |

> ⚠️ **Source-reality correction (important).** The m1n1 `idevice` branch does **NOT**
> contain a T8030 target. `src/soc.h` jumps from `T8015` (A11) straight to `T8103`
> (M1) — there is **no `T8030`/`0x8030` define, no `EARLY_UART_BASE` for it, and no
> Lightning/Thunder case in `chickens.c`** (grep: 0 hits for `8030`/`lightning`/
> `thunder` outside `midr.h`). So the "authoritative T8030 init" is not a T8030
> codepath in m1n1 — it is m1n1's **generic** pmgr/dart/aic drivers, which the
> *kernel* wires to T8030 via `t8103`-compatible fallbacks (see each block). The
> real, already-written T8030 bring-up is the **Hoolock-linux DTS**; the m1n1 port
> (a `chickens` entry for Lightning=0x12/Thunder=0x13 + SoC wiring) **still has to be
> written** — that is the actual porting task this file supports.

All four sources **agree** on every base and register offset below (differences are
only in naming); divergences are called out explicitly.

## SoC MMIO window (the master offset)
- Kernel DTS reg cells are `<0x2 <lo> 0x0 <size>>`, i.e. the high address cell is
  **`0x2`** [dts t8030.dtsi:220 etc.] ⇒ the SoC window is at PA **`0x2_0000_0000`**.
- qemu derives the same from the ADT `arm-io` node `ranges` →
  `soc_base_pa = ranges[1] = 0x200000000` [qemu hw/arm/t8030.c:1725-1730].
- **⇒ every block's absolute PA is given directly by its DTS `serial@…`/`iommu@…`
  node address** (Linux uses absolute). This is the Apple `0x2_0000_0000` family
  window shared with t8103/M1 — not a per-IP magic base.

---

## 1. UART — `apple,s5l-uart` (Samsung/S5L class)

### Location & binding
- **`uart0: serial@235200000`**, `compatible = "apple,s5l-uart"`,
  `reg = <0x2 0x35200000 0x0 0x4000>`, `reg-io-width = <4>`,
  `interrupts = <AIC_IRQ 435 IRQ_TYPE_LEVEL_HIGH>`, `clocks = <&clkref>` [dts
  t8030.dtsi:424-431]. ⇒ **base = `0x2_3520_0000`, AIC IRQ 435.**
- `clkref` = fixed **24 MHz** [dts t8030.dtsi:26-30] → the UART baud reference.
- [qemu] computes the same base `soc_base_pa + uart_offset[0] + uart_offset[1]*port`
  [hw/arm/t8030.c:161]; [m1n1] `soc.h` uses the identical value `0x235200000` for
  its T8103 neighbour, so **m1n1's existing `apple,s5l-uart` driver binds as-is**.

### Register map  [qemu hw/char/apple_uart.c:44-55] (S5L/S3C layout)
| off | name | notes |
|-----|------|-------|
| 0x00 | ULCON | line control — word length [1:0] (0x3 = 8-bit) |
| 0x04 | UCON | control — TX/RX mode, IRQ/DMA, thresh/err enables |
| 0x08 | UFCON | FIFO control — enable b0, Rx/Tx reset b1/b2, trigger levels |
| 0x0C | UMCON | modem control |
| 0x10 | UTRSTAT | status (RO) — Rx_rdy b0, Tx_buf_empty b1, Tx_empty b2; reset 0x6 |
| 0x14 | UERSTAT | error status (RO) |
| 0x18 | UFSTAT | FIFO status (RO) |
| 0x20 | UTXH | TX holding (WO) |
| 0x24 | URXH | RX holding (RO) |
| 0x28 | UBRDIV | baud divisor |
| 0x2C | UFRACVAL | fractional divisor |

### Minimal console TX for m1n1
1. `ULCON` = 0x3 (8N1). 2. `UBRDIV` = clk/(16·baud) − 1. 3. opt `UFCON` FIFO en, `UCON` TX mode.
4. per byte: poll `UTRSTAT & (1<<1)` (Tx buffer empty) → write `UTXH` (0x20).

### Live confirmation (iOS had it configured) — [live]
- `0x235200000` ULCON = **0x3** (8-bit) · `0x235200028` UBRDIV = **0x0c** (=12) ·
  `0x235200010` UTRSTAT = **0x6** (idle). With clkref 24 MHz: 24e6/(16·(12+1)) =
  **115 200 baud** ⇒ console is **115200 8N1**.

---

## 2. AIC — interrupt controller (`apple,t8030-aic`, "apple,aic" / **v1 layout**)

### Location & binding
- **`aic: interrupt-controller@23b100000`**,
  `compatible = "apple,t8030-aic", "apple,aic"`, `reg = <0x2 0x3b100000 0x0 0x8000>`,
  `#interrupt-cells = <3>` [dts t8030.dtsi:263-268]. ⇒ **base = `0x2_3B10_0000`,
  size 0x8000.** aic-timebase/"timer" window follows at `0x2_3B10_8000` [ADT].
- **Divergence / nuance:** Linux binds the **legacy `apple,aic` (v1)** driver, NOT
  `apple,aic2`. The register `rAIC_REV` reads **2** [live, below] but the MMIO
  *layout* is the v1 one — confirmed because [m1n1] `aic_regs.h` v1 offsets match
  [qemu] `apple_aic.c` exactly (table below). So an m1n1 T8030 port uses the
  **AIC_\*** (v1) codepath, not the AIC2_\* path used for M1.

### Register map — [m1n1] `aic_regs.h` ≡ [qemu] `apple_aic.c` (identical)
| off | m1n1 name | qemu name | notes |
|-----|-----------|-----------|-------|
| 0x0000 | — | rAIC_REV | revision (reads 2) |
| 0x0004 | AIC_INFO | rAIC_CAP0 | caps; NR_IRQ = bits[15:0] (0x240 = 576) |
| 0x2000 | AIC_WHOAMI | rAIC_WHOAMI | current CPU id |
| 0x2004 | AIC_EVENT | rAIC_IACK | ack/read pending event |
| 0x2008 | AIC_IPI_SEND | rAIC_IPI_SET | IPI send |
| 0x200C | AIC_IPI_ACK | rAIC_IPI_CLR | IPI ack/clear |
| 0x2024 / 0x2028 | AIC_IPI_MASK_SET / _CLR | rAIC_IPI_MASK_SET/_CLR | |
| 0x3000+4n | AIC_TARGET_CPU | rAIC_EIR_DEST(n) | IRQ routing |
| 0x4000+4n / 0x4080+4n | AIC_SW_SET / _CLR | rAIC_EIR_SW_SET/_CLR | SW trigger |
| **0x4100+4n / 0x4180+4n** | **AIC_MASK_SET / _CLR** | **rAIC_EIR_MASK_SET/_CLR** | **disable / enable IRQ group n** |
| 0x5008+(cpu<<7) … | AIC_CPU_IPI_* | rAIC_*_Pn | per-CPU bank, stride 0x80 |

- AIC_EVENT decode [m1n1 aic_regs.h:46-52]: DIE [31:24], TYPE [23:16] (HW=1, IPI=4),
  NUM [15:0]. Group/bit math [qemu apple_aic.c:93-95]: `group = src>>5`,
  `bit = 1<<(src&0x1F)`. Constants: 576 IRQs, 6 CPUs, VERSION 2 [qemu:34-36].

### Bring-up sketch
Read AIC_INFO for IRQ count → mask all via `AIC_MASK_SET(n)` for n in 0..(576>>5) →
per IRQ route `AIC_TARGET_CPU`, unmask `AIC_MASK_CLR` → in handler read `AIC_EVENT`
(per-CPU bank) and EOI. (m1n1's existing v1 AIC driver does exactly this.)

### Live confirmation — [live]
- `0x23b100000` REV = **0x2** · `0x23b100004` CAP0/INFO = **0x30240** (NR_IRQ =
  **0x240 = 576** ✓) · `0x23b100010` GLB_CFG = **0x00700000** (iOS timeouts/ACG).

---

## 3. PMGR — power/clock manager (`apple,t8030-pmgr`, t8103-compatible)

### Location & binding
- **`pmgr: power-management@23b000000`**,
  `compatible = "apple,t8030-pmgr", "apple,t8103-pmgr", "syscon", "simple-mfd"`,
  `reg = <0x2 0x3b000000 0x0 0x100000>` [dts t8030.dtsi:217-220]. ⇒ **base =
  `0x2_3B00_0000`, size 0x100000.** (t8103 fallback ⇒ reuse the M1 PMGR driver.)

### Power-state (PS) registers — the critical sequence (all 4 sources agree)
- Each power domain = **one 32-bit PS register** at `pmgr_base + offset`, offsets in
  the **0x80000+ bank**. Concrete offsets from the real Linux port [dts
  t8030-pmgr.dtsi / t8030.dtsi:224-231]:
  | domain | offset | absolute PA | parent |
  |--------|--------|-------------|--------|
  | ps_apcie | 0x80378 | 0x23b080378 | — |
  | ps_usb | 0x802e8 | 0x23b0802e8 | — |
  | ps_usbctl | 0x802f0 | 0x23b0802f0 | ps_usb |
  | ps_usb2dev | 0x802f8 | 0x23b0802f8 | — |
  (binding `apple,t8030-pmgr-pwrstate`,`apple,t8103-pmgr-pwrstate`, `reg = <off 4>`.)
- **Bit fields** [m1n1 pmgr.c:11-17] ≡ [qemu hw/arm/t8030.c:625]:
  `PMGR_PS_TARGET = bits[3:0]` (desired), `PMGR_PS_ACTUAL = bits[7:4]` (status, RO),
  `PMGR_PS_AUTO = bits[27:24]`. `PMGR_PS_ACTIVE` (on) = 0xF.
- **Enable algorithm** — m1n1 `pmgr_set_mode()` [m1n1 pmgr.c:84-92], the authority:
  ```c
  mask32(addr, PMGR_AUTO_ENABLE|PMGR_WAS_CLKGATED|PMGR_WAS_PWRGATED|PMGR_PS_TARGET,
         FIELD_PREP(PMGR_PS_TARGET, target_mode));      // write target into [3:0]
  poll32(addr, PMGR_PS_ACTUAL, FIELD_PREP(PMGR_PS_ACTUAL, target_mode), TIMEOUT);
                                                        // wait until [7:4] == target
  ```
  qemu models this literally: a write to 0x80000–0x8c000 does
  `actual[7:4] := target[3:0]` [qemu hw/arm/t8030.c:625]. **Parents must be enabled
  first** — m1n1 recurses over `power-domains` parents before the child
  [pmgr.c:151-193] (e.g. ps_usb before ps_usbctl).

### Other modelled PMGR registers  [qemu hw/arm/t8030.c:629-646]
- **`0xD4004` = CPU-start**: a write triggers `t8030_start_cpus()` — on T8030
  secondary CPUs are released through PMGR (relevant to m1n1 `smp.c`; not a PSCI call).
- **`0xF0010` = commonSramCheck**: reads constant `0x5000`.

### Live confirmation — [live]
- `0x23b0f0010` = **0x5000** ✓ (confirms PMGR base 0x23b000000). The PS-bank poll
  semantics are confirmed by m1n1+qemu source (above); the per-device offsets are
  the concrete DTS values in the table.

> Caveat: qemu models the PMGR *registers* faithfully (target→actual reflect,
> CPU-start, sram-check) but not real PLL lock / voltage ramp. The PS enable+poll
> loop, register offsets and parent ordering are ground truth; exact clock
> *frequencies*/PLL config still come from the ADT `voltage-states*` / `clkref`
> (24 MHz ref) or real HW.

---

## 4. DART — IOMMU (`apple,t8103-dart`, = t8020 family)

### Locations & binding
- **`usb_dart0: iommu@239024000`**, `compatible = "apple,t8103-dart"`,
  `reg = <0x2 0x39024000 0x0 0x4000>`, `interrupts = <AIC_IRQ 501 …>`,
  `power-domains = <&ps_usbctl>` [dts t8030.dtsi:441-448]. ⇒ **base `0x2_3902_4000`.**
  `usb_dart1 @239028000` [dts:450]. (sio/disp0 DARTs likewise; ADT also lists
  dart-sio `0x2_3500_4000`, dart-disp0 `0x2_3070_4000`.)
- **Binding takeaway:** Linux uses the **`apple,t8103-dart`** driver (the t8020
  family) for T8030 → the existing M1 DART driver covers it unchanged.

### Register map — [m1n1] `dart.c` `DART_T8020_*` ≡ [qemu] `apple_dart.c` (identical)
| off | m1n1 name | qemu name | notes |
|-----|-----------|-----------|-------|
| 0x00 | — | DART_PARAMS1 | PAGE_SHIFT = bits[27:24] (0xE ⇒ 16 KiB) |
| 0x04 | — | DART_PARAMS2 | BYPASS_SUPPORT b0 |
| 0x20 | DART_T8020_STREAM_COMMAND | DART_TLB_OP | INVALIDATE b20, BUSY b2 (poll→0) |
| 0x34 | DART_T8020_STREAM_SELECT | DART_SID_MASK_LOW | |
| 0x40 | DART_T8020_ERROR | DART_ERROR_STATUS | FLAG b31, stream[27:24], code |
| 0x50 / 0x54 | DART_T8020_ERROR_ADDR_LO/HI | DART_ERROR_ADDRESS_LO/HI | fault addr |
| 0x60 | DART_T8020_CONFIG | DART_CONFIG | LOCK b15 |
| 0x80+4·s4 | DART_T8020_STREAM_REMAP | DART_SID_REMAP | 4 sids/reg |
| 0xFC | DART_T8020_ENABLED_STREAMS | — | (m1n1-only) |
| **0x100+4·sid** | **DART_T8020_TCR_OFF** | **DART_TCR(sid)** | **TRANSLATE_ENABLE b7**, BYPASS_DART b8, BYPASS_DAPF b12 |
| 0x200+16·sid+4·idx | — | DART_TTBR(sid,idx) | VALID b31, PA = (val & 0xFFFFFFF)<<12 |

- Geometry [qemu apple_dart.c:26-29]: 16 streams, 4 TTBR each, 38 VA bits; page
  shift from PARAMS1 (16 KiB).

### Bring-up sketch (= Linux apple-dart t8020 path)
Read PARAMS1 (page shift) → per stream set `DART_TTBR(sid,idx) = (PA>>12)|VALID` →
`DART_TCR(sid) = TRANSLATE_ENABLE` (clear BYPASS) → invalidate: `STREAM_COMMAND`
INVALIDATE, poll BUSY→0.

### Live confirmation (dart-usb, iOS had enabled it) — [live]
- `0x239024000` PARAMS1 = **0x0e000000** ⇒ PAGE_SHIFT 0xE = **16 KiB pages** ✓ ·
  `0x239024004` PARAMS2 = 0 · `0x239024100` TCR(sid0) = **0x80** ⇒ **TXEN set,
  translation ON** ✓.

---

## CPU / chickens (the part m1n1 is missing)
- CPU core MIDR parts [m1n1 midr.h:21-22]: **Lightning (P) = 0x12, Thunder (E) =
  0x13** (T8301 Thunder = 0x26). ADT: 2×P `apple,lightning` + 4×E `apple,thunder`,
  `aic` `#main-cpus 6`.
- There is **no `chickens.c` case** for these in the m1n1 idevice branch — writing
  one (CPU errata/ACC init for Lightning/Thunder, between the existing A11 `twister`
  and M1 `firestorm` entries) is the core porting work. The DTS `cpus` node
  [t8030.dtsi:38-104] enumerates the 6 cores (`reg` 0x0..0x3, 0x10100, 0x10101).

## Appendix — gdb/watchpoint method (for live *ordering*)
Static map above (DTS + m1n1 + qemu) plus post-boot `xp` reads were enough; to
capture exact write *order* live: launch `-s -S` (frozen), `lldb --arch aarch64 -o
"gdb-remote 127.0.0.1:1234"`, `watchpoint set expression -s 4 -- 0x23b0802e8`
(ps_usb) or `0x235200020` (UART UTXH), `continue`; each hit shows the iOS PC + value.
Monitor alternative used here: `xp/Nwx <addr>` reads live MMIO through the models.

Bench build/run: `~/Desktop/t8030-emu-build/BUILD_NOTES.md` · UART boot log:
`~/Desktop/t8030-emu-build/t8030_boot_uart.log`. Sources cloned under
`~/Desktop/t8030-emu-build/{hoolock-linux,m1n1-idevice,qemu-t8030}` and DTS in the
scratch dir.

## Absolute base summary (PA)
| block | base | size | DTS compatible | m1n1/Linux driver |
|-------|------|------|----------------|-------------------|
| UART0 | 0x2_3520_0000 | 0x4000 | apple,s5l-uart | s5l-uart (exists) |
| AIC | 0x2_3B10_0000 | 0x8000 | apple,t8030-aic, apple,aic | AIC **v1** (exists) |
| PMGR | 0x2_3B00_0000 | 0x100000 | apple,t8030-pmgr, apple,t8103-pmgr | pmgr (t8103, exists) |
| └ PS bank | 0x2_3B08_0000+ | per-dev (ps_usb 0x802e8) | …-pwrstate | target[3:0]/actual[7:4] |
| └ CPU-start | 0x2_3B0D_4004 | | | PMGR CPU release |
| DART usb0/usb1 | 0x2_3902_4000 / …8000 | 0x4000 | apple,t8103-dart | dart t8020 (exists) |
| DART sio / disp0 | 0x2_3500_4000 / 0x2_3070_4000 | 0x4000 | apple,t8103-dart | dart t8020 |

**Net:** every peripheral block (UART/AIC/PMGR/DART) is register-compatible with
existing m1n1/Linux drivers (s5l-uart, AIC v1, t8103-pmgr, t8103/t8020-dart) at the
bases above. The only genuinely new code for an m1n1 T8030 port is the **CPU
`chickens` entry for Lightning/Thunder** plus SoC wiring (soc.h define +
EARLY_UART_BASE 0x235200000); the Hoolock-linux `t8030` branch already proves the
peripheral map end-to-end.
