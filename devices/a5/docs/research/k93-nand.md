# K93AP NAND: raw NAND behind H2FMI + BCH, read side

**Rule for everything below:** the NAND holds the iPad's iOS install.  Nothing
here programs or erases it.  `tools/nand/k93nand.c` can only put RESET (FF), READ
ID (90), READ STATUS (70) and READ (00 ... 30, and a bare 00) on the bus; program
(80/10), erase (60/D0) and set-feature (EF) are not in it, the FMI is never
pointed host-to-NAND (FMI_CTRL 5 is refused) and the FMI's command sequencer is
not even mapped.

The iPad mini 1 (P105) notebook is `p105-nand.md`; read it first.  The mini's
NAND is PPN (the package does ECC and hides the geometry).  The iPad 2's is raw
MLC: the SoC's BCH engine does the ECC, the page layout, the bad blocks and two
kinds of whitening are the host's business, and Apple's VSVFL + YAFTL sit on top.

## Status (2026-11-09)

Offline only: everything here comes from Apple binaries, nothing has run on the
iPad yet.  Sources, all 12H321 (iOS 8.4.1):

- **iBEC** `build/k93/firmware-12H321/iBEC.k93.RELEASE.dfu.dec`, iBoot-2261.30.37,
  base 0x9ff00000, Thumb-2.  It boots iOS from this NAND, so it carries the whole
  raw read stack: `drivers/apple/h2fmi/*` (H2fmi.c, H2fmi_read.c, fmiss_raw.c,
  H2fmi_timing.c), `drivers/flash_nand/id/nandid.c` (the chip tables),
  `drivers/flash_nand/raw/Whimory/Core/{VFL/VSVFLInterface.c,FTL/yaFTL.c}`, an
  LwVM parser and HFS+.  Every address below is an iBEC address unless it says IOP.
- **IOP firmware** `iop-firmware.bin` (base 0): iOS's own FMI driver.  Its raw
  page read (0x58b4) is instruction for instruction iBEC's fmiss read
  (0x9ff0702c), and its register dump (0x3144) **names the registers** (used
  throughout).
- **ADT** (`adt.txt`, template values: iBoot fills `ce-bitmap`, `device-readid`,
  `#ce-blocks` ... at boot, so they read 0 here).

"(C)" = read directly in the code at the address given.  "(I)" = inferred.

## 1. Hardware

| | FMI (transfer, FIFOs) | FMC (the NAND bus) | ECC (BCH) | IRQ |
|---|---|---|---|---|
| bus 0 | 0x31200000 | 0x31240000 | 0x31280000 | AIC 0x21 |
| bus 1 | 0x31300000 | 0x31340000 | 0x31380000 | AIC 0x22 |

(C) ADT `arm-io/flash-controller0` `reg` (six 4 KB windows, arm-io ranges base
0x30000000), `interrupts` 0x21 0x22; iBEC h2fmi_init_minimal 0x9ff03954
(bases), 0x9ff057ec (IRQ 0x21 for bus 0, 0x22 for bus 1).  Same windows as the
mini.

**Clocks and reset** (C).  ADT `clock-gates` = 59, 61, 60, 62 = FMI0, FMI1,
FMI0-BCH, FMI1-BCH; `scripts/adt-pmgr-map.py` on the K93 ADT puts them at PMGR
0x3f1010c4, 0x3f1010cc, 0x3f1010c8, 0x3f1010d0.  iBEC `WMR_CLOCK_GATE` 0x9ff21648
turns on iBoot clocks 0x2f+0x30 (bus 0) or 0x31+0x32 (bus 1); iBoot's
clock_gate 0x9ff1f128 = PMGR 0x3f101008 + id*4, low nibble := 0xf, then wait
until the status nibble (bits 7:4) equals it.  Block reset (`WMR_CLOCK_RESET_DEVICE`
0x9ff21690 -> clock_reset_device 0x9ff1eb88, ADT `function-device0_reset` 0x2f,
`function-device1_reset` 0x31): bit 31 of 0x3f1010c4 / 0x3f1010cc set, one timer
tick, cleared.  iBEC never pulses the BCH gates (0x3f1010c8 / 0x3f1010d0).

**iBEC always brings the NAND up**: early in its main task it calls nand_boot_init
(0x9ff00b8a -> 0x9ff1e2f4 -> 0x9ff11cf0 -> ... -> h2fmiInit 0x9ff02fcc),
tethered or not.  So when Linux starts after iBEC, both buses are
clocked, in SDR, with iBEC's tuned timing in FMC_IFCTRL -- unless something
resets them.

### Registers

Names from the IOP's dump ("CFG CTRL STAT INTPND INT_EN DBG0 DATSIZ", "ON
IFCTRL CECTRL RWCTRL CMD ADDR0 ADDR1 ANUM DNUM FSTATUS NSTATUS RBB_CFG", "DQST
NANDT1 NANDT2 NANDT3", "ecc{PND MASK}"), offsets from its code (IOP 0x3144-0x320e).

| reg | name | use (C unless marked) |
|---|---|---|
| FMI+0x00 | CFG | `(ecc_bits << 3) \| 5` for a page (0xf5); 5 for a raw 1 KB chunk; 0 for short transfers (ID, spare); 0xf0 for boot pages.  (I) low bits = sector size, 5 = 1 KB |
| FMI+0x04 | CTRL | 3 start NAND -> FIFO, 6 stop/reset, 0x83 next page of a multi-page read (legacy path), 0x1302 sequencer mode, 5 host -> NAND (writes) |
| FMI+0x08 | STAT | (I) bit 2 busy: the legacy read path waits on it (0x9ff066c2) |
| FMI+0x0c | INTPND | W1C.  bit 1 transfer done, bit 8 FMC event (status matched) |
| FMI+0x10 | INT_EN | 0x100 FMC event, 2 transfer done, 0x11c00000 sequencer |
| FMI+0x14 | (data FIFO) | page data; iBEC: CDMA ch 5 (bus 0) / 7 (bus 1), 32-bit, bursts of 8 words |
| FMI+0x18 | (meta FIFO) | page metadata; CDMA ch 6 / 8, **8-bit**, bursts of 1 (fmiss 0x9ff071ea) |
| FMI+0x1c | DBG0 | bits 3-4: data waiting (h2fmi_wait_dma_task_pending 0x9ff051dc) |
| FMI+0x34 | DATSIZ | `sectors \| bytes << 8 \| meta << 19 \| meta << 25` (0x9ff05cd0); ID: 0x801 |
| FMI+0xc0000.. | sequencer | command FIFO +0, level +4; operand FIFO +0x14, level +0x18; status +0x20, int enable +0x24, timer +0x2c.  Command words (0x09000000\|ce_mask, 0x1100000b, 0x11000b06) not decoded |
| FMC+0x00 | ON | 1 = SDR (raw NAND, h2fmi_reset 0x9ff050f6), 5 = DDR (PPN only) |
| FMC+0x08 | IFCTRL | `dc << 16 \| rs << 12 \| rh << 8 \| ws << 4 \| wh`, each clocks-1 (0x9ff03724); 0xffff = iBEC's untuned start value |
| FMC+0x0c | CECTRL | `1 << ce` |
| FMC+0x10 | RWCTRL | cycles: 1 cmd1, 9 cmd+addr, 0xb cmd1+addr+cmd2, 0x50 poll status |
| FMC+0x14 | CMD | `cmd1 \| cmd2 << 8` |
| FMC+0x18/0x1c | ADDR0/ADDR1 | page read: ADDR0 = row[15:0] << 16 (2 column bytes 0), ADDR1 = row[23:16] (0x9ff05270) |
| FMC+0x20 | ANUM | address bytes - 1 (4 = 2 column + 3 row) |
| FMC+0x24 | DNUM | 0 before a status poll |
| FMC+0x40 | (INTMASK) | 0x20 = route status match to FMI INTPND bit 8 |
| FMC+0x44 | FSTATUS | W1C cycle-done bits (1, 8, 2 -> 0xb), 0x20 status matched; clear all with 0x31ffff |
| FMC+0x48 | NSTATUS | the NAND status byte |
| FMC+0x4c | RBB_CFG | status poll `mask \| value << 8` (0x4040: wait for bit 6, ready) |
| FMC+0x70-7c | DQST, NANDT1-3 | DDR/Toggle timing; unused on raw NAND |
| ECC+0x08 | (config) | `1 << 31 \| t << 16 \| thr << 8` (0x9ff05d34); fmiss uses thr = t+1 = 31 -> 0x801e1f00, legacy read 15, boot pages 0x1e0000 |
| ECC+0x0c | (sector results) | read: pops one sector's result; write 1: clears the queue (0x9ff070d2) |
| ECC+0x10 | PND | bits 16-20: most bits corrected in the page.  iBEC writes 0x1a8 to it before boot-page I/O (0x9ff052ac) -- (I) W1C of pending bits; `p105-nand.md`'s "ECC_CONFIG 0x1a8 = 424 bits" reading is probably wrong |
| ECC+0x14 | MASK | 0 at init (0x9ff05816) |
| ECC+0x2c | (level) | bits 0-5: results waiting (fmiss_interrupt_handler 0x9ff07360) |

## 2. Bring-up and timing

h2fmi_init_minimal 0x9ff03914, per bus (C): clock gates on; timing := 0xffff;
FMC_IFCTRL := timing (0x9ff05bd4); **h2fmi_reset** 0x9ff050d8 = block reset,
FMI_CTRL 6, FMC_ON 1 (SDR), FMC_IFCTRL := timing; interrupts (0x9ff057a4):
FMC+0x40 0, INT_EN 0, FSTATUS 0x31ffff, INTPND 0xf, ECC MASK 0.  iBEC repeats
h2fmi_reset at the start of every page read (0x9ff0707e) and after every READ ID
(0x9ff053ca); a block reset wipes the FMC timing, which is why it restores it.

**Timing** (C for the structure, I for the values).  After the chip lookup,
NandRequirementToIFCTRL 0x9ff06b7c takes the FMI clock (`clock_get_frequency(16)`,
a runtime value iBoot computes from the PLLs: 0x9ff1f0a8 -> [0x9ff4a8f4]), the
constants 6, 6, 3, 3, 5, 5 and the 8 requirement bytes of the timing-table entry
(section 3), and produces rs, rh, dc, ws, wh in clocks; IF_CTRL = dc << 16 |
rs << 12 | rh << 8 | ws << 4 | wh (0x9ff03724-0x9ff0374a).  The clock is not in
the binaries, so the tuned value cannot be computed offline: `k93nand regs` after
a boot through iBEC shows it, and `k93nand` keeps whatever is there (0xffff, the
slow value iBEC itself reads the chip IDs with, if it finds none).  Raw NAND
never uses the DDR registers (h2fmiInit 0x9ff0374e skips them when fmi+0x5c = 0).

## 3. Chip ID, ce-bitmap, geometry

**Detection** (C).  h2fmi_reset_and_read_chipids 0x9ff05b20 per bus:
h2fmi_nand_reset_all 0x9ff05b64 sends FF to all 16 CE numbers (CE_CTRL = 0 for
the other bus's eight -- nothing selected) then waits 50 ms; h2fmi_nand_read_id_all
0x9ff05abc reads 8 ID bytes from every CE number: h2fmi_nand_read_id 0x9ff0531c =
CE_CTRL 1<<ce, CMD 0x90, ADDR0 0, ANUM 0, RWCTRL 9, wait FSTATUS 9; CFG 0, DATSIZ
0x801, CTRL 3, wait INTPND 2, wait DBG0 bits 3-4, two words from the data FIFO,
then h2fmi_reset.  0x9ff059f0 builds the **ce-bitmap**: a CE counts when its first
4 ID bytes equal bus 0 CE0's ("Found Chip ID ..."); a different non-empty ID is
"Ignoring mismatched"; empty = 8 x 00 or `ff ff ff ff ff 00 00 00` (0x9ff44084).
Bit numbering: bus*8 + CE (the ADT `ce-bitmap`; the mini's was 0x101).  The raw
bus is SDR, so an absent CE just reads pull-ups; the DQS wedge from the mini
(DDR) cannot happen.  iBEC probes every CE number on every boot.

**Virtual CEs** (C, h2fmiInitVirtToPhysMap 0x9ff039bc): CE numbers are walked
in step over both buses, bus 0 first: v = 0, 1, 2 ... go to (bus0 CE0), (bus1
CE0), (bus0 CE1), (bus1 CE1) ..., skipping absent ones.  The VFL's "CE" is this
virtual number.

**Tables** (C, nandid.c, findNandInfo 0x9ff217cc).  Chip table 0x9ff466d0 (12 x
28 bytes: 8-byte ID, u16 blocks/CE, u16 pages/block, u16 bytes/page, u16 spare,
u32 +0x10 (unused by the raw path; (I) the vendor's ECC requirement), u32 bbt-format,
u32 dies/CE); board table 0x9ff46864 and timing table 0x9ff46abc (25 entries each,
keyed by {2 buses, buses with chips, (ID, CEs per bus) x 2}: board -> the VSVFL
`vendor-type`; timing -> 8 requirement bytes).  The IDs must be identical and the
CE count equal on both populated buses ("Chip IDs not symmetrical!").

| ID (4 bytes) | blocks/CE | pg/blk | spare | dies | BCH t | boards (CEs/bus x buses, vendor-type) |
|---|---|---|---|---|---|---|
| EC D7 94 7A Samsung | 4152 | 128 | 640 | 1 | 30 | 4x1, 4x2 (0x100014) |
| EC DE D5 7A Samsung | 8304 | 128 | 640 | 2 | 30 | 4x2 (0x100014) |
| EC D7 94 72 Samsung | 4152 | 128 | 436 | 1 | 30 | 4x1, 4x2 (0x100014) |
| EC DE D5 72 Samsung | 8304 | 128 | 436 | 2 | 30 | 4x1, 4x2, 8x1 (0x100014); 8x2 (0x010001) |
| 98 E7 94 32 Toshiba | 4100 | 128 | 448 | 1 | 30 | 4x1, 4x2 (0x150011) |
| 98 EE 95 32 Toshiba | 8200 | 128 | 448 | 2 | 30 | 4x1, 4x2 (0x150011); 8x2 (0x010001) |
| 2C 88 04 4B Micron | 4096 | 256 | 448 | 1 | 30 | 2x1, 2x2, 4x2 (0x120014) |
| 45 48 94 32 SanDisk | 4096 | 128 | 448 | 1 | 30 | 4x1, 4x2 (0x150011) |
| 45 68 95 32 SanDisk | 8192 | 128 | 448 | 2 | 30 | 4x1, 4x2 (0x150011) |
| 45 2D 94 82 SanDisk | 4096 | 256 | 640 | 1 | 30 | 2x1, 2x2 (0x150011) |
| 45 4D 94 82 SanDisk | 4096 | 256 | 640 | 1 | 30 | 4x2 (0x150011) |
| 45 29 94 32 SanDisk | 4096 | 128 | 640 | 1 | 30 | 2x1 (0x150011) |

All pages are 8192 bytes.  The user's iPad is the 32 GB model; which row it is
only `k93nand id` will say (or iOS: `ioreg -l -p IODeviceTree -n disk` shows the
`device-readid`, `ce-bitmap`, `#ce`, `#ce-blocks`, `vendor-type` iBoot filled in).

**What iBEC computes from it** (C, h2fmiInit 0x9ff03162-0x9ff0323e,
h2fmiSetDeviceInfo 0x9ff04f08):

- sectors per page = bytes/page >> 10 = 8 (**1 KB BCH sectors**);
- metadata: 12 bytes per page as the FTL sees it, 10 stored (table 0x9ff46820:
  `meta-per-logical-page` 12, `valid-meta-per-logical-page` 10);
- ECC strength (h2fmi_calculate_ecc_bits 0x9ff05c38, table 0x9ff44094):
  `per = (spare - 10) / sectors`, then the first of {53->30, 51->29, 44->25,
  28->16, 27->15} with per >= key.  Every chip above gets **t = 30**: 14-bit BCH
  symbols, ceil(30*14/8) = **53 parity bytes per sector**;
- FMI_CFG 0xf5, FMI_DATSIZ 0x14540008 (8 sectors, 1024 bytes, 10 meta bytes),
  ECC config 0x801e1f00; refresh threshold t*8/10 = 24 bits (ADT `ecc-threshold`,
  `ecc-correctable` = 30);
- row address of block b, page p: `((b / bpd) * stride + b % bpd) * ppb2 + p`,
  bpd = blocks/die, stride = next power of two >= bpd when blocks/CE is not a
  power of two (0x9ff04f5a), ppb2 = pages/block rounded up to a power of two.
  Two-die Samsung/Toshiba parts therefore have a hole: die 1 starts at block 8192.

**On-flash layout** (I): `[1024 data][10 meta][53 parity]` then 7 x `[1024 data]
[53 parity]` = 8626 bytes, which fits the 436-byte Samsung spare with 2 bytes to
spare -- the ECC table's 53 is exactly (436 - 10) / 8.  Metadata after the first
sector is how the H2FMI laid out the mini's PPN pages too ("1024, 16 meta,
3072").  `k93nand raw` shows the real layout.

## 4. Reading a page

**What iBEC runs** (C).  h2fmiReadSinglePage 0x9ff046b0 (vCE, row, data, meta,
...) -> 0x9ff063bc -> fmiss raw read 0x9ff0702c (= IOP 0x58b4):
h2fmi_reset; CFG/DATSIZ/ECC config (0x9ff05d34, thr = t+1); sequencer timer
FMI+0xc002c = the FMI clock in Hz (fmi+8 * 1000); CTRL 0x1302; sequencer IRQs;
INT_EN 0x11c00000; INTPND := ~0; RWCTRL 0; ECC+0x0c := 1; ANUM := 4; then per page the sequencer
gets 0x09000000|cemask, the two address words and 0x1100000b, and in a second
pass 0x11000b06 ((I): issue the read; wait for ready and transfer -- the command
words are not decoded); CDMA moves 8 KB from FMI+0x14 and 10 bytes from
FMI+0x18; fmiss_interrupt_handler 0x9ff0732c pops the ECC results whenever
ECC+0x2c shows a page's worth.

**The same thing without the sequencer** (C): iBEC's boot-page reader
h2fmi_read_bootpage_pio 0x9ff05f9c and the legacy read state machine (H2fmi_read.c,
table 0x9ff440d0; present in the IOP too) do it with plain registers, and that is
what `k93nand` replays:

1. h2fmi_reset; FMI_CFG 0xf5, FMI_DATSIZ 0x14540008, ECC+0x08 0x801e1f00,
   INTPND := 0xffffffff, RWCTRL 0, ECC+0x0c := 1 (fmiss's values).
2. CECTRL 1<<ce; ADDR1 row>>16, ADDR0 (row&0xffff)<<16, ANUM 4; CMD 0x3000
   (00 ... 30); RWCTRL 0xb; wait FSTATUS 0xb (W1C) -- 0x9ff06080.
3. wait ready -- h2fmi_wait_status 0x9ff0592c: IFCTRL &= ~bit 20; RBB_CFG 0x4040;
   CMD 0x70, RWCTRL 1, wait FSTATUS 1; clear IRQs; DNUM 0; RWCTRL 0x50 (the FMC
   now polls the status byte); FMC+0x40 0x20, INT_EN 0x100; wait INTPND bit 8;
   NSTATUS = the status byte; clear IRQs; RWCTRL 0.
4. CMD 0x00, RWCTRL 0 then 1, wait FSTATUS 1 (back to data output) -- 0x9ff060d4.
5. CTRL 3; data from FMI+0x14 (2048 words), metadata from FMI+0x18 (10 bytes);
   wait INTPND bit 1.
6. ECC: PND, then one ECC+0x0c read per sector; CE off; clear IRQs.

**What comes out** (C): the data FIFO delivers the corrected 1 KB sectors back to
back (8192 bytes); the metadata FIFO the 10 metadata bytes; the 53-byte parity
blocks never leave the FMI.  With the AES context attached (section 5), the data
is decrypted on its way through the CDMA, not in the FMI.

**ECC verdict** (C, h2fmi_rx_check_page_ecc 0x9ff0612c, fmiss handler 0x9ff073a6).
Per sector result word: bit 1 = erased sector, with bits 8-13 a count that must
be <= 2 for the sector to count as clean ((I) zero bits seen); bit 0 =
uncorrectable; bit 2 = a flag whose meaning is unknown (a page fails only if every
sector has it -> 0x80000025); otherwise bits 16-20 = bits corrected.  Page: all
sectors clean -> 2 (**erased**); all sectors bit 2 -> 0x80000025; any uncorrectable
(or erased with too many zero bits) -> 0x80000024; else 1 (ok).  h2fmiReadSinglePage
maps 2 -> "clean", 1 -> ok, 0x80000023/24/25 -> UECC (0x80000002), anything else
-> hardware error; on ok it sets metadata bytes 10-11 to FF.  An erased page's
metadata is reported as all FF by the VSVFL (0x9ff33ee8).

**PIO** (I).  Nothing in iBEC reads a whole ECC page by PIO, but its PIO paths
show the FIFOs work that way: READ ID and boot pages wait for INTPND bit 1, then
DBG0 bits 3-4, then read the FIFO (boot pages are three 512-byte transfers, CFG
0xf0, DATSIZ 0x20001, ECC+0x08 0x1e0000, 0x9ff05c18); the mini's tools read PPN
pages in 1 KB PIO transfers at 14.6 MB/s.  `k93nand read` default ("page mode")
keeps iBEC's one 8-sector transfer and drains FMI+0x14 eight words per DBG0 check,
the way CDMA's 8-word bursts would, then the metadata FIFO with byte reads;
`-s` ("sector mode") does 8 one-sector transfers, each finished before the FIFO
is read, sector 0 with the metadata (DATSIZ 0x14540001, then 0x40001).
Whichever works on the hardware is the answer to the PIO question.

**Raw read** (C, h2fmi_rx_raw 0x9ff06204, FIL+0x08): ECC off (CFG 5), DATSIZ
0x40008 (8 x 1 KB, one transfer into DMA), then the spare with CFG 0, DATSIZ
`(spare_rounded << 8 & 0x7fc00) | 1`.  `k93nand raw` does the 8 KB as 1 KB
transfers.

## 5. Whitening

**Metadata** (C).  Table, h2fmiInit 0x9ff03866: `v = 0x50f4546a`; for each of 256
entries step `v = v*0x19660d + 0x3c6ef35f` 763 times and store v (T[0] = ac1f1d93,
T[255] = fb86536a; the same as iphone-dataprotection's).  h2fmiReadSinglePage
0x9ff04776: `meta32[i] ^= T[(row + i) & 0xff]` for i = 0, 1, 2, row = the
CE-relative row address.  Enabled when the NAND signature's word 1 has bit 16
(WMR_Init 0x9ff34df6 -> FIL+0x5c, flag at 0x9ff487c0).  The ADT template has
`metadata-whitening = 1`.

**Data** (C, h2fmi_setup_aes 0x9ff044c8): AES-128-CBC on the CDMA, decrypting as
the data leaves FMI+0x14, one IV per 8 KB page (chunk = fmi+0x94 = 8192,
0x9ff031b4).  IV(seed): `v = seed`; 4 times `v = v&1 ? (v>>1)^0x80000061 : v>>1`,
store v as a little-endian word (0x9ff04ea0, 0x9ff05eb8).

| page | key | IV seed |
|---|---|---|
| YAFTL user data (meta type 0x10) | `f65dae950e906c42b254cc58fc78eece` (0x9ff461e4) | the LPN |
| everything else: VFL/FTL context, index, BTOC, special blocks | `92a742ab08c969bf006c9412d3cc79a5` (0x9ff461f4, iphone-dataprotection's META_KEY) | the row address |

The user-data case is chosen when the destination buffer lies inside the "FTL
region" that YAFTL_Read 0x9ff30ecc declares (FIL+0x60, 0x9ff04cac: LPN, buffer,
count); then seed = region LPN + (buf - region buf) / 8192.  Metadata is not
AES'd (I: the AES context only goes to the data DMA).  On/off is not stored: the
signature is read with AES on, and if that fails, again with AES off ("[FIL]
Disable data whitening", VFL_ReadSignature 0x9ff2f260, also 0x9ff2f3b4).  So a
PIO reader gets ciphertext and must decrypt in software; `k93nand read -k` does.

## 6. Special blocks and the NAND signature (C)

Reader 0x9ff2ee4c: page 0 of blocks blocks/CE-1 down to `(0x60 << floor(log2
(blocks/CE))) / 100` (4151 .. 3932 for 4152 blocks) on virtual CE 0, data only.
Bytes 0-15 a tag: "NANDDRIVERSIGN", "DEVICEUNIQUEINFO", "DIAGCONTROLINFO",
"DEVICEINFOBBT"; header to 0x38 (+0x34 payload length), payload from 0x38.
DEVICEINFOBBT's header +0x18..0x2c lists the other special blocks; its payload is
the factory bad-block bitmap, blocks/CE bits, 1 = good.  Signature payload
(WMR_Init 0x9ff34cee): word 0 bytes {?, '1' = VSVFL, '1'/'0', 'C'} = 0x43313100;
word 1 bit 16 metadata whitening, low byte minor version (<= 6).  The FTL type is
in the VFL context (+8, must be 2 = YAFTL).  `k93nand sig` reads these.

## 7. VSVFL (C unless marked; VSVFLInterface.c)

**Banks per CE** from the board's vendor-type (VFL_Open 0x9ff333be): 2 for
0x100014, 0x120014, 0x140014, 0x150011, 0x150010, 0x160010, 0x160011,
0x110012, 0x130012, 0x010012; 4 for 0x140013, 0x120013, 0x010013; 1 for
0x010001, 0x170015.  NCE = CEs on both buses, TB = NCE * BPC banks, PPSB = pages
per block * TB pages per superblock.

**vpn -> (vCE, row)** (0x9ff32ed0, checked by hand):

```
vblock = vpn / PPSB;   vbank = vpn % TB;   vpage = (vpn % PPSB) / TB
ce     = vbank % NCE;  sub = vbank / NCE                   (ce = virtual CE)
pblk   = conv(sub, vblock):
         0x100014/0x120014/0x140014 (0x9ff345c8 -> 0x9ff3445c) and 0x150011
         (0x9ff346b0): 2*vblock + (sub & 1) -- even blocks bank 0, odd bank 1;
         0x010012/0x110012/0x130012/0x150010/0x160010 (0x9ff345f0): vblock, plus
         half the CE when sub is odd -- bank 1 is the upper half;
         0x010001 (0x9ff34454): vblock
pblk   = _RemapBlock(ce, pblk)                              0x9ff32e10
row    = ((pblk / bpd) * stride + pblk % bpd) * ppb2 + vpage
```

_RemapBlock: if the block's bit in the DEVICEINFOBBT bitmap is clear (bad), find
i with ctx.map[i] == pblk; nres = blocks/CE / BPC - pool_start; the replacement is
conv(NCE*(i/nres)+ce, pool_start + i%nres).  Map values: < blocks/CE = the bad
block replaced by reserved slot i; 0xFFF0 free; >= 0xFFF1 the reserved block
itself is dead.

**VFL context** (VFL_Open 0x9ff33120), per virtual CE: page 0 of blocks
start..start+198 (start = 1, or the start of the flash partition table's `fsys`)
with metadata byte 8 = 0x00 and byte 9 = 0x80; ctx+0x68e lists 4 context blocks;
the one whose page-0 metadata u32 0 is the smallest non-zero is current; inside
it, sets of 8 redundant pages at pages 0, 8, 16 ... -- the last readable set is
newest; the context is the first 2048 bytes of a page (META-key AES, IV = row).
Checksums: ctx[0x7f8] = sum(u32[0..509]) + 0xAABBCCDD, ctx[0x7fc] = xor ^
0xAABBCCDD; version ctx[0x7f4] < 3.  Fields: +0x008 FTL type (2 = YAFTL), +0x026
u16 reserved_block_pool_map[], +0x68e u16 context blocks[4], +0x696 usable blocks
per bank (= YAFTL superblocks), +0x698 reserved pool start, +0x69a u16 FTL
control blocks[3], +0x6da vendor type (0 -> the board's).

## 8. YAFTL (C unless marked; yaFTL.c, raw copy 0x9ff2f934-0x9ff32200)

**Page metadata** (after unwhitening; 10 stored + 2 x FF):

| bytes | meaning |
|---|---|
| 0-3 | user page: LPN; index page: index-page number; FFFFFFFF erased |
| 4-7 | usn ("weaveSeq") of the **superblock**: every page of a superblock carries it |
| 8 | (I) unknown; 0 on VFL context pages |
| 9 | type: 0x10 user data, 0x04 index, 0x20 FTL context (CX01), 0x08 BTOC (0x0c on an index superblock), 0x80 VSVFL context, 0xff erased.  _verifyMetaData 0x9ff2f934 requires LPN match and type & 0x40 == 0 |
| 10-11 | not stored, read as FF |

**Superblocks**: vpn = sb * PPSB + off.  User pages fill off 0 .. PPSB-n-1; the
last n pages are the BTOC, one u32 LPN per user page (n = smallest with 4*(PPSB-n)
<= n*(8192-4), i.e. 1).  An LPN is one 8 KB page.  Total LPNs = (SuBlks - 8) *
(PPSB - n) - PPSB * idx, idx = 3 * ceil((SuBlks-8)*(PPSB-n) / ((PPSB-n)*2048))
(index superblocks; 3 becomes 4, 0x9ff2ffce, 0x9ff30120); SuBlks = the VFL
context's usable blocks per bank.  The block device iBEC builds ("nand0") is
99 * ((LPNs-1)/100) pages of 8192 bytes (YAFTL_Open 0x9ff304fe).

**Rebuilding the map without the context** (YAFTL_Open's own "full NAND R/O
restore", 0x9ff30616): classify each superblock by its first valid page (type
0x10 user, 0x04 index, FF free), sort user superblocks by usn, newest first
(addBlockToList 0x9ff3181c, two equal usns = "found two blocks with the same
weaveSeq"), and in each read the BTOC (one page) or, if it is unreadable, the
pages from last to first (restoreBlock 0x9ff31c04); **the first sighting of an LPN
wins**.  So the live copy is max(usn), then max(offset in the superblock).  The
iosnand recipe from the mini carries over with "sequence" = (usn, off), where
off = vpage * TB + vbank.  Normal mount reads the newest of the 3 FTL control
superblocks (ctx +0x69a), whose context page (type 0x20, "CX01": +8 total LPNs,
+0x5c/0x5e/0x60 TOC geometry, +0x94 exported percent) leads to index pages of
2048 (LPN -> vpn) entries.

## 9. LwVM and the System partition (C, 0x9ff257bc)

LPN 0, offset 0: the LwVM header, GUID 6A9088CF-8AFD-630A-E351-E24887E0B98B (or
B189A519-4F59-4B1D-AD44-1E127AAF4539 with a CRC32 at +0x2c), count at 0x28,
0x80-byte records at 0x200, the u16 chunk map at 0x800 -- the mini's format.
Chunk size = the next power of two >= device size / 1024: **32 MB on the 32 GB
iPad** (16 MB on the 16 GB mini; iosnand's constant must become a variable).  Disk
byte X = LPN X / 8192.  iBEC names the partitions nand0a, nand0b ... and boots the
kernelcache from boot-partition 0 (System, HFS+).  To read LPN 0: find the page
whose metadata says LPN 0 with the newest usn, decrypt with the user-data key and
IV(0) (an all-zero IV); the GUID should appear.

## 10. The tool: `tools/nand/k93nand.c`

Build as in its header (`build/k93/tools/k93nand`, static, -Wall clean).  It
refuses to run unless `/proc/device-tree/compatible` starts with `apple,k93ap`,
prints every register it writes (`-q` silences it, `-v` adds reads), and holds a
flock per bus.

```
k93nand selftest                  # no hardware: LCG table, IV, AES vectors
k93nand regs                      # gates, FMI/FMC/ECC, IF_CTRL fields
k93nand init                      # gates on, block reset, SDR, timing, IRQs off
k93nand id                        # RESET + READ ID on CE0-7 x 2, ce-bitmap -> /tmp/k93nand.state
k93nand sig                       # special blocks at the top of vCE 0: data whitening on/off,
                                  #   signature word 1 (metadata whitening)
k93nand read 0 0 BLOCK:PAGE       # one page: ECC per sector, meta raw + unwhitened,
                                  #   data as read + after AES (-k auto|none|meta|user:LPN)
k93nand read ... -s               # same in sector mode
k93nand raw 0 0 BLOCK:PAGE f.bin  # no ECC: data + spare as on the flash
k93nand scan 0 0 1 16 [-1]        # one line per page; -1 = sector 0 only (8x less I/O)
```

Where it cannot do what iBEC does, it says so in the source: CPU instead of CDMA
on the two FIFOs, polling INTPND bit 8 (or FMC FSTATUS bit 5, what the mini's
tools poll) instead of the interrupt, no sequencer.  `read`/`scan`/`raw`/`sig`
only address CEs in the ce-bitmap `id` saved (`K93NAND_CEMAP` overrides; with
`id` it limits the probe, e.g. to iOS's `ce-bitmap`).

A first session should go: `selftest`, `regs` (note IF_CTRL), `id`, `sig`, then
`read` of a page `sig` found, `raw` of the same page, then `scan` of a few blocks
near block 1 (VSVFL context pages, type 0x80, should appear there).

`tools/nand/k93-ftl-map.py --cemap 0x.... --banks N scan-*.txt` turns `scan`
output into the LPN map with iBEC's rule (section 8) and prints where LPN 0 (the
LwVM header) is, with the `read ... -k user:0` line that checks it.

A host-side model of the controller (scratch, not in the repo) ran every command
through these code paths; that proves the control flow and the crypto, not the
hardware's semantics.

## 11. Linux plan

**Step 1 -- userspace, iosnand-style.**  Split k93nand into a small FIL library
(like `ppn.c`: init, read page with ECC, unwhiten, AES) and build `iosnand`'s
equivalent on it:

1. ce-bitmap and geometry from `id`; virtual CE order; the special blocks (bad
   block bitmap, signature: whitening flags).
2. VSVFL: the newest context of every virtual CE (META key), its replacement
   map; vpn -> (vCE, row) as in section 7.
3. YAFTL by restore, not by context: classify the superblocks (one page each;
   a superblock spans every bank, so there are only as many as blocks per bank,
   ~2000-4000), then read the BTOC of each user superblock newest-first (one page each:
   META key, IV = row) and fill LPN -> vpn first-sighting-wins; per-page metadata
   only for superblocks whose BTOC is missing (the one or two open ones).  A few
   thousand page reads: seconds, not the 20+ minutes a metadata scan of every
   page takes (`scan -1`, ~4.3 M pages on 32 GB).  Cache the map like iosnand.
4. LwVM (32 MB chunks), NBD export; System mounts with hfsplus read-only.  Data
   is per-file encrypted (content protection) and needs the class keys -- out of
   scope, like on the mini.

**Step 2 -- kernel.**  Not a raw-NAND MTD driver: Linux's nand core would want to
own ECC layout, bad blocks and would fight the whitening.  Same shape as
`mtd-apple-h2fmi.c`: an mtd per virtual CE (page = 8 KB, OOB = the 10 unwhitened
metadata bytes, `_read_oob` doing the BCH read, ECC result -> bitflips/-EBADMSG,
erased -> 0xff), then a read-only block driver that loads the VSVFL+YAFTL map at
probe (BTOC walk) and serves LPNs with the AES step (kernel crypto `cbc(aes)` or
the SoC's AES engine), and an LwVM partition parser (or a device-mapper table
built in userspace).  DMA through the CDMA (channels 5-8, 8-word bursts) is what
makes it fast; PIO first.

**A Linux root as a file on System**: readable this way (hfsplus + loop),
read-only.  Writes go through YAFTL's write path, which no binary here
implements in readable form (iBEC is "Apple NAND Driver (AND) RO"; the writer is
in the kernelcache's AppleNANDFTL, unreversed).  Root on a file needs an overlay
on tmpfs or a USB disk for writes.

*Later (2026-11-10):* the root on a file is what boots now (`tools/k93-root.sh`),
and its writes do not go through YAFTL at all: Linux owns a band of the VSVFL's
reserved pool that iOS never reaches, and `tools/nand/k93lv.c` keeps the
overlay's changes there -- [k93-nand-write.md](k93-nand-write.md).

## 12. Open questions, and the one experiment for each

| question | experiment |
|---|---|
| Which chips, how many CEs (the ce-bitmap) | `k93nand id` (or iOS `ioreg -l -p IODeviceTree -n disk`: `device-readid`, `ce-bitmap`) |
| The tuned IF_CTRL | `k93nand regs` after any boot through iBEC |
| Do page-mode PIO reads work (DBG0 per 8 words, meta by byte reads)? | `k93nand read` on a written page: ECC ok and sensible metadata; if it times out, `read -s` |
| Where the metadata and parity sit on the flash | `k93nand raw` of the same page: the 10 metadata bytes (as read) should follow the first 1024 data bytes |
| Is data whitening on? | `k93nand sig`: tag in plaintext = off, only after META decrypt = on |
| Is metadata whitening on, and is the XOR right? | `sig` prints signature word 1 bit 16; a VSVFL context page (`scan` near block 1) must unwhiten to byte 8 = 00, byte 9 = 80 |
| User-data key/IV, 8 KB vs 4 KB chain | the page whose metadata says LPN 0 (type 10): `read -k user:0` must show the LwVM GUID; if only the first 4 KB decrypts, the chain is 4 KB |
| Meaning of ECC result bit 2, metadata byte 8, the sequencer words | not needed for reading; bit 2 shows up in `read`'s per-sector lines if it ever happens |
| The PND write 0x1a8 before boot pages | irrelevant for user pages |
