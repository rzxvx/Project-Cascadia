# Touch bring-up — consolidated state & decision (2026-09-06)

## Bottom line
The ONLY blocker is **powering the Samsung SPI1 controller** (the `Cmwp` /
grape-clk sequence). teutekeune's bare-metal `touch_cursor.c` already implements
the ENTIRE rest of the flow and it is sound:
PMGR gates/clocks, PMU rails over I2C bit-bang, GPIO reset/CS, Z2 protocol,
firmware upload. If the SPI1 controller powered, touch would work.

We proved (XNU active hook, 2026-09-06): enabling PMGR gate 83 + clocks does NOT
power the PWM/grape block (PIO decode error at 0x33500300). So gate/clock pokes
alone are insufficient — the `Cmwp` power sequencing is required.

## Complete power-up recipe (decoded from ADT apple-p105ap-raw.json)
`/arm-io/spi1` (compatible **spi-1,samsung**):
- clock-gates: **68** (SPI1); clock-ids: **304, 307** (enable 0x180)
- function-spi_cs0: GPIO **pin 7**

`/arm-io/spi1/multi-touch` (compatible **multi-touch,p105**):
- function-clock_enable  -> phandle + **"Cmwp"**  (SoC SPI/grape clock — THE WALL)
- function-clock_enable-pmu -> PMU(0x3c) "GPIO/OIPG": reg **0x0200<-0x01**, **0x0201<-0x02**
- function-power_ana -> PMU(0x3c) "Lump" reg **0x020c** (enable bit)  [analog rail]
- function-power_ldo -> PMU(0x3c) "Lump" reg **0x0213** (enable bit)  [digital LDO]
- function-reset -> GPIO **pin 5** (active-low)
- function-enable_cs -> GPIO **pin 7**
- interrupts (ATTN) -> GPIO **pin 22**
- reg rate cells: 5000 / 10000 kHz (use <=5 MHz)
PMU is on **I2C0** (0x33200000); grape gate **83**, PWM clk id **4**; I2C0 gate 80 clk 286.

## Driver classes in the kernelcache (Ghidra targets)
- **AppleSamsungSPIController** — SPI1 driver. `::start` logs
  "_spiBaseAddress = 0x%08x, _spiVersion". Its start() sets up the SPI1 pins
  (SCLK/MOSI/MISO/CS) and requests the clock (function-clock_enable -> Cmwp).
- **AppleARMPWM / AppleARMPWMDevice** — PWM/grape-clk provider; the "Cmwp"
  platform-function handler that actually powers grape-clk lives here.
- **AppleMultitouchSPI** — the Z2 multitouch driver (protocol reference).
Note: kernelcache accesses MMIO via dynamically-mapped pointers (no 0x321/0x335
literals), so grepping literals fails — must trace map()->base->offset in Ghidra.

## The real fork (both remaining unknowns need the SAME reverse)
1. **HW-SPI path**: crack `Cmwp` (grape-clk power seq). Reverse
   AppleARMPWM's Cmwp handler + AppleSamsungSPIController::start clock request.
   -> replay in Linux samsung-SPI driver. Unlocks teutekeune's whole flow.
2. **Bit-bang path**: needs SCLK/MOSI/MISO **GPIO pin numbers** — NOT in the ADT
   (only CS=7 is a GPIO function; data lines are dedicated Samsung-SPI pins).
   Get them from AppleSamsungSPIController::start pin-setup, OR a Pi-Pico logic
   analyzer on the Z2 connector, OR a passive 28-pin GPIO-mux dump (one boot).

=> **Reversing `AppleSamsungSPIController::start` (+ AppleARMPWM Cmwp) in Ghidra
is the key next step for EITHER path** — it yields both the SPI pin numbers
(bit-bang) and the clock-enable register sequence (HW-SPI). It is OFFLINE
(no boot cycles), which is what we wanted when leaving the XNU-hook loop.
The user has ghidra_kc / ghidra_project set up; the decrypted macho is
ibootfiles/kernelcache.macho with a full symbol table (AppleSamsungSPIController
symbol present at ~0x489f49 string; find its vtable/start via Classes tree).

## Complementary: Pi-Pico logic-analyzer snoop
Probing the Z2 connector's SPI lines during a real iOS boot simultaneously:
(a) identifies which pads are SCLK/MOSI/MISO (solves bit-bang pins physically),
(b) captures the firmware-upload byte stream (answers "where is the Z2 blob"),
(c) gives protocol ground truth. Cheap, hardware already in hand.

## Z2 protocol (port target, from touch_cursor.c z2_read_packet)
- 16-byte cmd: [0]=0xEB (READ_IRQ), [1]=parity+1, [14:15]=cksum(0xEB+[1]) LE, rest 0.
  XFER 16B @8bpw. Expect rx[0]==0xE1 (REPLY_IRQ) or 0xEB.
- pkt_len = (rx[1]|rx[2]<<8); pkt_len=(pkt_len+8)&~3; must be 32..sizeof(buf).
  XFER pkt_len bytes (tx=0) to read the packet.
- msg=rx+5; nfingers=msg[16]; finger=msg[24]: st=fp[1] (3=START,4=MOVED),
  ax=fp[4]|fp[5]<<8, ay=fp[6]|fp[7]<<8.
- Reset/boot: RESET(5)+CS(7) GPIO out; assert RESET=0, wait ATTN(22) low 100ms;
  else release RESET=1 edge, wait ATTN; upload FW held in reset; release; read.
