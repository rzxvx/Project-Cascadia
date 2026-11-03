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
2. prox calibration (none on P105), then the panel's calibration: the ADT's
   `multi-touch-calibration`, which iBoot fills from syscfg `MtCl` at boot
   (table at iBEC `0x1b70c`) and the driver republishes as
   "Calibration Data". 1024 bytes on this iPad, starting `4e 50 01 06` and the
   panel serial; `logs/ios/`. Sent to `cal-dl-addr` `0x10009000` as DATA
   packets of at most `0x3f0` bytes, each acked `4bc1` (`0x80607f04`). The
   IPSW's ADT only has a zeroed placeholder, and `build/keep`'s `syscfg.bin`
   is a 44-byte stub -- which is where "no MtCl" once came from.
3. `MTSPIBootloader_N1::performCalibSeq` (`0x80608f44`):
   read `0x10008ffc` (version); write `0x10003060 <- fll-mval`;
   write `0x1000305c <- 0x20` (ref-clk-div); write `0x10003058 <- 6`;
   write `0x10003000 <- 3` (const-cal; 2 for version `0x434d11a0`);
   write `0x10003518 <- 1` (clk32 enable); read `0x10003800` (SPI_APU_EN);
   send `1f 01` (request calibration); sleep 65 ms; ATN_ACK.
   Any write not acked `4ad1` fails the bootload.
4. EXECUTE `1d 53 <0x10003400> <1> csum`, then sleep 40 ms

ref-clk-div and const-cal are not in P105's personality, so they are the
bootloader's defaults: `init` (`0x80607848`) calls slot `+0x74` (defaults:
Z2 `0x8060877c`, then N1 `0x80609148`) before slot `+0x78` reads the
personality over them. Reading "absent" as "write 0 to address 0" was wrong
and fatal: address 0 is the firmware's reset vector, and after EXECUTE the chip
answered `4f81` forever with ATTN held low.

fll-mval: the personality says `0x17d3`, but the live service on this iPad
holds `0x17c9` (`logs/ios-mtdump.txt`). The kernel only reads the property;
what rewrites it is not in the kernelcache, the dyld cache, `/usr/libexec`,
`/usr/sbin` or the device's own `P105.mtprops` (identical to ours).
`z2-boot` writes `0x17c9`, `-F` overrides.

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

## iOS's own trace of all of this (2026-09-24)

AppleMultitouchSPI logs everything it does -- every step above by name, every
report it gets or sets, and a hex dump of every SPI transfer up to 1034 bytes
(commands, answers, frames; not the 55 KB firmware) -- to a user-client queue,
and to the kernel log as `mtlog: ...` when two boot-args are set:

| boot-arg | flag at `+0x808` | what |
|---|---|---|
| `mt-strings=1` | bit 0 | `IOLog("mtlog: %s")` of every driver message (`0x805ff6d4`) |
| `mt-bytes=1` | bit 1 | hex dumps of SPI transfers (`0x805ff984`) |

`start` (`0x805ff0a8`, at `0x805ff190`) reads them only if
`PE_i_can_has_debugger()` says yes, which is `/chosen/debug-enabled` from
iBoot (`0x8027a1c4`) -- 0 on a production iPad. `iBoot32Patcher --debug`
(Legacy iOS Kit's copy has it) makes iBEC set it; the alternative is one
instruction in the kernelcache: `bl PE_i_can_has_debugger` at `0x805ff194`
-> `movs r0, #1`.

So a tethered boot of the iPad's own iOS with
`serial=3 mt-strings=1 mt-bytes=1` and debug-enabled, captured on the DCSD
UART, records the whole bring-up iOS does on this very panel, byte for byte,
plus whatever userspace sets afterwards and real frames.  The personality has
`ResetWhenExitingUILock`: lock and unlock the iPad and the digitizer is reset
and bootloaded again, into the same log.

## What iOS does after EXECUTE, and what finally made it scan (2026-09-25)

Recorded with `tools/mtdump/mtlog` -- the driver's own trace through its user
client (selector 3 on, memory type 0x10 the queue), no boot-args or patches,
on the jailbroken iOS of this very iPad (`logs/ios-mtlog1.txt`):

| step | bytes | note |
|---|---|---|
| wake | `ee 00 .. ee 00`, 2 ms | MT_SPI_Z2_WAKE_CMD |
| device info | `e2 00` twice | answer `e2 14 01 98 07`: family 0x14, max packet 1944 |
| GET d1 d3 d0 a1 d9 | `e6 id 00 len` / `e6 id 01 len` short, `e7` long (answer `len + 5`) | csum = e6/e7 + id + len; the stage byte is left out |
| SET bf | `e4 bf 04 9b 0b 0b 02`, then `e1 00` status | from userspace (MultitouchSupport); later `99 09 09 00` |
| SET af | `e4 af 01 00`, then `e1 00` | 9d is tried as well; the chip has no such report and nothing is sent |
| bf, af again | | then frames |

Frames, on ATTN: `eb seq 00..` answered `e1 LL 00`, then `LL + 5` bytes
`eb seq 01 00.. csum` answered `ea seq LL 00 hcsum` + payload + csum; seq
1, 2, 1, ... Payload: `[16]` touch count, 30 bytes a touch from `[24]`
(`[0]` id, `[1]` state, 4 = down). No finger, no frames. `z2-boot` sends all
of it byte for byte (42 of 42 logged transfers, checked on the host).

It still did not scan until the PMU matched iOS. iOS's live PMU registers are
on IORegistry class `AppleARMPMUCharger`, property `AppleRegisterDump`
(0x00..0x7f, computed on request; `tools/mtdump`). Against ours, the power
difference is `0x22` = 0x67 vs 0x65: bit 0x02 is LDO idx 15, a 5 V-class LDO
(voltage register 0x3e, 5.1 V) that no ADT function names and iOS keeps on.
With `-M 22:02:02 -M 3d:20:20 -M 61:02:00` (the last two: 0x3d's bit 0x20,
and PMU GPIO0 back at 0x11 where iOS has it) the digitizer scans: 1131 frames
in 20 s, one to three fingers (`logs/z2-boot11.txt`). Split out: it is
0x22 bit 0x02 alone -- forced off, 0 frames; on, 457 frames in 8 s, with
0x3d untouched and PMU GPIO0 at our 0x13 (`logs/z2-boot12.txt`,
`z2-boot13.txt`). `dts/p105ap.dts` now switches it together with the analog
rail (`touch_ana`, mask 0x06), and drops `pmuclk-supply`.

Two things fell out on the way:
- `irq-apple-aic1.c` switched the whole AIC off (all lines masked,
  CONFIG.ENABLE cleared) on one IRQ entry with an empty EVENT, and on a
  storm. The start of the scan provoked one: USB, network and NFS stopped
  the moment bf/af went out, the Mac saw no detach, only the local timer
  kept ticking. Now an empty entry is counted and ignored, a storm masks
  its own line.
- The ADT's `DISPLAY_SYNC` pin to the flex (J1700.4) is unused on P105, and
  the 32 kHz on `AP_CLK_32K_CUMULUS` (J1700.14, GPIO 63) was never the
  problem -- measured clean with PMU GPIO0 at 0x11 and at 0x13.
