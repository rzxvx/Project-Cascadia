# P105 digitizer bring-up — what iOS does that hx-touchd does not (2026-09-20)

With the GPIO interrupt map fixed (p105-gpio.md), hx-touchd gets its boot IRQ,
uploads the firmware and runs its handshake — 12 ATTNs in the first second —
and then the chip is silent for good, finger on the glass or not. It answers;
it does not scan.

## Where P105's recipe lives

The mtprops for `C1F14,1` holds only `Constructed Firmware` (55 320 bytes),
its version, `PreconstructedBootloadPacketType = Z2` and `ResetInterval`. The
rest is in the kext personality, `AppleMultitouchSPIC1F14.kext`,
"AppleMultitouchSPI - C1,P105":

| key | value |
|---|---|
| IOClass | `AppleMultitouchN1SPI` (bootloader `MTSPIBootloader_N1`) |
| fll-mval-addr / fll-mval | `0x10003060` / `0x17d3` |
| clk32-clock-enable-addr / -val | `0x10003518` / `1` |
| fw-execute-addr | `0x10003400` |
| cal-dl-addr, prox-cal-addr | `0x10009000`, `0x10009600` |
| reset-deassert-delay | 15 ms |
| mt-merge-personality | `C1F14,1` |
| AutomaticallySetOperatingMode | true |
| parser-type / parser-options | 1 / 0x10 |

hx-touchd knows none of these keys; it was written for devices whose mtprops
carry a "Boot Sequence".

## The sequence (AppleMultitouchSPI kext, 12H321, header file `0x5bd000`, VA `0x805fe000`)

`MTSPIBootloader_Z2::bootloadDevice` (`0x806078d0`), with the N1 overrides:

1. firmware — the whole blob in one transfer, then ATN_ACK, `4bc1` = accepted
2. prox calibration, calibration — none here (no MtCl in our syscfg)
3. `MTSPIBootloader_N1::performCalibSeq` (`0x80608f44`):
   read `0x10008ffc` (version); write `0x10003060 <- 0x17d3` (FLL);
   write ref-clk-div (absent: 0 to address 0); write `0x10003058 <- 6`;
   write const-cal (absent: 0, or 2 for version `0x434d11a0`, to address 0);
   write `0x10003518 <- 1` (clk32 enable); read `0x10003800` (SPI_APU_EN);
   send `1f 01` (request calibration); sleep 65 ms; ATN_ACK
4. EXECUTE `1d 53 <0x10003400> <1> csum`, then sleep 40 ms

and afterwards `AppleMultitouchSPI` sets report `0xab` (rOPERATING_MODE) to
`0x00` (`0x806000c8`).

## HBPP packets (AppleMultitouchZ2SPI)

A 32-bit field is two big-endian 16-bit halves, low half first; csum is the
16-bit byte sum of the fields, stored big-endian.

| packet | bytes | then |
|---|---|---|
| ATN_ACK | `1a a1` | 2-byte status, big-endian |
| long ATN_ACK | `1a a1 18 e1 18 e1 18 e1` | value = `[4]<<24 \| [5]<<16 \| [2]<<8 \| [3]` |
| HBPP check | `1a a1` + `18 e1` x7 | first two words must be status words |
| register read | `1c 73 addr csum` (8) | long ATN_ACK |
| register write | `1e 33 addr mask value csum` (16) | ATN_ACK, `4ad1` = done |
| execute | `1d 53 addr 00 01 00 00 csum` (12) | — |

Status words the HBPP check accepts: `18e1 1aa1 1f01 4879 4969 4ad1 4bc1`.
hx-touchd's own check got `1f01 4879` back, so the bootloader was there.

`tools/z2-boot.c` does all of this and then hx-touchd's post-boot handshake,
with iOS's operating mode in place of hx-touchd's "mode 1" reports
(`9d`/`bf`/`af`: Sandcastle's, nothing P105 carries asks for them; `-m`
sends them anyway).
