# K93AP NAND: programming, erasing, and where Linux may write

**Rule for everything below:** the NAND holds the iPad's iOS 6.1.3 install.  A DFU
restore no longer ends the project (the owner moves between iOS versions with
Legacy iOS Kit), but it costs the jailbreak and every install, so nothing writes
outside the window of section 5.5.  The write path lives in *separate,
explicitly write-enabled* tools (`tools/nand/k93nandw.c`, `tools/nand/k93lv.c`);
`tools/nand/k93nand.c` and `k93iosnand` stay read-only.

## Results on the iPad (2026-11-10)

The tests of section 8 ran with `tools/nand/k93nandw.c` on a RAM-root Linux
(`/cascadia/root.sqfs` set aside, so nothing else used the buses); logs in
`logs/k93-nandw-T*.log`.  All passed:

| step | outcome |
|---|---|
| T0 | NAND status **0xE0 with IFCTRL bit 21 clear and set**, on all four CEs: bit 7 never follows bit 21, and IFCTRL reads back without it.  So 2.1's guess is wrong: bit 21 is not WP# (or WP# is not wired) and the chips were never write-protected.  There is no hardware second lock; the protection is the command allowlists, and in k93nandw the armed block (below).  The writes still set bit 21 as the IOP does. |
| T1 | every vCE matches section 5: pool at vblock 1952, 96 slots per bank, contexts in block 16 (17-19 erased; 40/40/40/104 valid copies, no bad checksum), replacements bottom-up to slot 18 (bank 0) / 26 (bank 1), **6 borrowed spares** at the top of bank 1 (vCE2 slot 94, vCE3 slots 90-94), special blocks 4089-4095 on vCE0 (DEVICEINFOBBT x2, DEVICEUNIQUEINFO x2, NANDDRIVERSIGN, DIAGCONTROLINFO x2) and 4094-4095 elsewhere -- the DEVICEINFOBBT header lists them as u32 from +0x14, its bitmap is LSB first -- all in 0xFFFF slots.  **All 56 window blocks of every vCE free in every copy and fully erased: 224 blocks, 448 MB.**  `k93-pool-plan.py` on the dumped copies agrees. |
| T2 | the metadata FIFO takes its 10 bytes before CTRL 5 (DBG0 0x10 once CTRL 4 is set); DBG0 & 0x18 means room in the TX direction; the transfer-done bit comes; DNUM ends at 1076 = 1024 + 53 - 1.  Nothing programmed (all 256 pages still erased). |
| T3 | erase: 4.2-5.4 ms in the status poll, status 0xE0, block erased. |
| T4 | program: 0.4 ms (lower page) to ~2 ms per page; 2.0 ms per page with the PIO and the AES (3.9 MB/s on one bus).  Read back by the unchanged `k93nand read -k meta`: ECC ok, metadata and data as written; `k93nand raw` shows the 10 metadata bytes right after sector 0's 1024 bytes -- the layout of `k93-nand.md` section 3 holds.  All 256 pages, then the same on vCE 0, 1 and 3 (both buses, both banks): 0-2 bits corrected per page. |
| T5 | two blocks left programmed across an iOS boot that wrote 64 MB: both intact; iOS's VSVFL contexts byte-identical before and after on all four CEs. |

What became of it: `tools/nand/k93lv.c`, the Linux volume (333 MB on the 224
blocks): a log-structured block device served with System by one process, the
overlay's upper layer at boot (initramfs/init), with every program and erase
behind k93nandw's checks: `arm()` takes only blocks a pool check in the same
process cleared, and `wrw()` refuses an 80h/60h whose ADDR/ANUM/CECTRL, read
back from the FMC, are not the armed block's.

Read `k93-nand.md` first: this note is its write side and reuses its register
names, the geometry (SanDisk `45 2d 94 82`, 4096 blocks/CE, 256 pages/block,
8 KB pages, 640-byte spare, BCH t = 30 per 1 KB, 10 metadata bytes), the virtual
CEs (vCE0 = FMI0:CE0, vCE1 = FMI1:CE0, vCE2 = FMI0:CE2, vCE3 = FMI1:CE2) and the
VSVFL layout (vendor-type 0x150011, 2 banks/CE, pblk = 2*vblock + bank).

## Sources (2026-11-09)

All 12H321 (iOS 8.4.1):

- **IOP firmware** `build/k93/firmware-12H321/iop-firmware.bin` (EmbeddedIOP,
  iBoot-2261.30.37, Thumb-2, base 0).  On the A5 the kernel does not touch the
  FMI: AppleIOPFMI sends commands to the I/O processor, whose firmware is the
  H2FMI driver of iBoot (`drivers/apple/h2fmi/H2fmi_write.c`, `H2fmi_erase.c`,
  `H2fmi_boot.c`, `H2fmi_iop.c`).  **The program and erase code exists only here**
  -- iBEC is "AND RO".  Every bare hex address below is an IOP address.
- **kernelcache** `kernelcache.12H321.k93.macho`, prelinked kexts:
  AppleIOPFMI-94 (0x80ab5000: the FIL -- metadata whitening, AES options, the
  IOP commands), AppleNANDLegacyFTL (0x80ef3000: IOFlashStorage-722.20.1
  Whimory -- `VSVFLInterface.c`, `FPart.c`, `yaFTL.c`), AppleNANDFTL
  (0x80932000), IOFlashStorage (0x80752000: AppleNANDFactoryBBT and the
  IOFlashPartitionScheme `nand_part_core`).
- Method: Ghidra 12.2 headless on the IOP image and on each kext's slice of the
  kernelcache (text block read-only so PC-relative literals resolve; decompiled
  C kept as scratch in `build/k93/research-write/ghidra/out/`), plus
  `tools/nand/k93-iop-nand-emu.py`, which runs the IOP's own erase and program
  functions in unicorn against a register model and prints the register trace.
  `tools/nand/k93-pool-plan.py` applies section 5.5's rules to dumped VSVFL
  context pages (host-side, offline).

Legend: **[E]** emulated (the trace of `k93-iop-nand-emu.py`), **[C]** read in the
code at the address given, **[A]** by analogy / general NAND knowledge, **[I]**
inferred from code that does not say it outright, **[?]** open -- section 9
lists the experiment for each.

**Version risk.** The iPad runs 6.1.3, these binaries are 8.4.1.  The FMI/FMC/ECC
programming model is the silicon's and the program/erase sequence below is the
only one the IOP has, so it is low risk.  The VSVFL map *format* (where it is,
what 0xFFF0/0xFFFF/0xFFFE mean) is on-flash state that 8.4.1 opens without
reformatting a 6.x device, so it is shared [A]; the *allocation policy* details
(which slot a new bad block gets, which bank a borrow picks) are code and may
differ in 6.1.3 [?].  The live maps agree with the 8.4.1 policy (replacements
fill each bank from slot 0 upward, section 5.3), and every safety rule in section
5.5 is checked against the device's own map at run time, so the policy only
affects how big the margins should be, not whether a block is safe.

## 1. Who drives the NAND on the A5

AppleIOPFMI builds an IOP command, the IOP's `fmi_message_process` (0x10f4) runs
it [C]:

| opcode | handler | does |
|---|---|---|
| 1 | 0x26ec | set config: fills the `fmi` object (geometry, timing, CFG/DATSIZ/ECC values) |
| 3 | 0x2998 | erase one block -> h2fmi_erase_blocks(n = 1) |
| 4 | 0x29ec | read one page (with ECC, AES) |
| 5 / 7 | 0x2f00 / 0x2f30 | read / **write raw** (ECC off; panics on PPN) |
| 6 | 0x2cb4 | write one page -> h2fmi_write_multi(n = 1) via 0x48bc |
| 8 / 9 | 0x2ad8 / 0x2d64 | read / **write multiple** pages |
| 10 / 0xb | 0x2ff4 / 0x2f68 | write / read a boot page (3 x 512 bytes) |
| 0xc | 0x294c | erase several blocks (in parallel over CEs) |

Before a command with flag bit 0 the IOP re-applies the timing and FMC_ON
(0x26b4 -> 0x3f00) [C].

## 2. Erase (60h ... D0h)

**h2fmi_erase_blocks** 0x36c4 (fmi, n, u16 ce[], u32 block[], u32 *fail) [C][E],
for one block on bus 0 CE 0:

```
save = FMC_IFCTRL;  FMC_IFCTRL = save | 0x200000          (0x3ab0; bit 21, see 2.1)
FMC_CECTRL = 1 << ce                                       (h2fmi_enable_chip 0x3b70)
FMC_ADDR0  = (block * pages_per_block) & 0xffffff          3 row bytes, no column
FMC_ANUM   = 2
FMC_CMD    = 0xd060                                        cmd1 60h, cmd2 D0h
FMC_RWCTRL = 0xb;  wait FSTATUS & 0xb == 0xb;  FSTATUS = 0xb (W1C)
-- other CEs' erases are issued here before any status is read (state[] per CE)
h2fmi_wait_erase_status 0x3918 -> h2fmi_wait_status 0x3ae4 (mask 0x40, value 0x40):
    0x39f4: FMC_IFCTRL &= ~0x100000; RBB_CFG = 0x4040; CMD = 0x70; RWCTRL = 1;
            wait FSTATUS bit 0 (W1C); INTMASK = 0; INT_EN = 0; FSTATUS = 0x31ffff;
            INTPND = 0xf; DNUM = 0; RWCTRL = 0x50
    INTMASK = 0x20; INT_EN = 0x100; wait the FMI interrupt (INTPND bit 8), 2 s
    status = FMC_NSTATUS; FMC_RWCTRL = 0
    status bit 7 must be 1, else WMR_ASSERT(bWPDisabled) -- the IOP panics
    status bit 0 = erase failed -> *fail = 1, fmi+0x1cc = failing CE
FMC_IFCTRL = save;  FMC_CECTRL = 0                         (0x3b98)
```

Unlike a page read, the erase does not start with h2fmi_reset [C]; a Linux tool
should anyway (k93nand resets before every operation).  The block number is in
the die-stride space the caller computes (`row_of(block, 0) / 256` here: one die,
4096 blocks, so the row is simply block * 256) [C, callers in VSVFLInterface use
the same stride arithmetic as `row_of`].

### 2.1 FMC_IFCTRL bit 21 is (very likely) WP#

**T0 says no** (results above): the status byte reads 0xE0 -- not protected --
with bit 21 clear as well as set.  What follows is the reasoning as it stood.

Every erase and program path sets IFCTRL bit 21 before the command and restores
the saved value after (0x36f8, 0x4946, 0x4af8, 0x4cba) [C], and right after the
command the status byte's bit 7 -- WP#, 1 = not protected, in every ONFI/legacy
part [A] -- is asserted (`bWPDisabled`, 0x3934 for erase, 0x543c for program;
the emulation with status 0x60 panics there [E]).  Reads never set bit 21 [C].
So bit 21 most likely drives the WP# pin [I]: **the read-only tools leave the
NAND write-protected**, a useful second lock.  Section 8, T0, checks it without
any risk (READ STATUS with bit 21 clear and set).

## 3. Program (80h ... 10h)

### 3.1 What iOS runs: h2fmi_write_multi

**h2fmi_write_multi** 0x48f4 (fmi, n, u16 ce[], u32 row[], data segments, meta
segments, u32 *fail, wVendorType) is a state machine (fmi+0xbc; table 0x12824:
0 idle 0x4ed4, 1 start 0x4f7c, 2 wait-for-CE 0x4fb0, 3 transfer 0x4fb8, 4 ending
0x5024, 5 ending-status 0x5084, 6 done 0x5164, 7 CE-limit 0x5180) driven by the
FMI interrupt and by the caller's loop [C].  For one page with wVendorType 0, the
register trace [E] (`k93-iop-nand-emu.py`, row 0xfa000 = block 4000 page 0):

```
                                        h2fmi_write_multi 0x48f4
  R FMC IFCTRL   -> saved
  block reset (clock 0x2f: PMGR 0x3f1010c4 bit 31), CTRL 6, ON 1, IFCTRL timing   h2fmi_reset 0x32a4
  W FMC IFCTRL   <- saved | 0x200000                                           WP# released
                                        state 0, idle 0x4ed4 -> per-op init 0x3d88
  W FMI CFG      <- 000000f5            (30 << 3) | 5: BCH t = 30, 1 KB sectors     0x3d14
  W FMI DATSIZ   <- 14540008            8 sectors x 1024 + 10 metadata bytes
  W ECC CFG      <- 801e1f00            1 << 31 | t << 16 | (t + 1) << 8
  W ECC PND      <- 000001a8            clear the ECC pending bits                   0x3464
  W FMI INT_EN   <- 00000002
  W FMI CTRL     <- 00000004            host -> FIFO enabled, no transfer yet
  CDMA ch 5: memory -> FMI+0x14, 8192 bytes, 32-bit, bursts of 8, AES (encrypt)     0x3df8
  CDMA ch 6: memory -> FMI+0x18,   10 bytes,  8-bit, bursts of 1, no AES
                                        state 1, start the page 0x4e78
  W FMC CECTRL   <- 00000001
  W FMC ADDR1    <- row >> 16           \
  W FMC ADDR0    <- (row & 0xffff) << 16 }  h2fmi_set_page_address 0x3428 (2 column + 3 row)
  W FMC ANUM     <- 00000004            /
  W FMC CMD      <- 00000080            0x4bc8
  W FMC RWCTRL   <- 00000009            cmd1 + address
  wait FSTATUS & 9 == 9, W1C 9
  W FMI INT_EN   <- 00000002
  W FMI CTRL     <- 00000005            transfer: FIFO -> NAND page register, BCH encoding
  W FMC CMD      <- 00001000            cmd2 = 10h preloaded (0x4c28), not yet sent
                                        state 3, 0x4fb8
  INTPND bit 1 (transfer done) -> clear: INTMASK 0, INT_EN 0, FSTATUS 0x31ffff, INTPND 0xf
                                        0x52c4 -> 0x4c68
  W FMC RWCTRL   <- 00000002            send 10h: the NAND programs (tPROG)
  wait FSTATUS & 2, W1C 2
                                        state 4 / 5: status of every CE still busy
  W FMC CECTRL   <- 1 << ce;  READ STATUS polled exactly as for the erase (0x3cf0)
  INTPND bit 8 -> clear IRQs; NSTATUS bit 7 must be 1 (else panic, 0x543c);
  NSTATUS bit 0 = program failed -> fSuccessful = 0, fmi+0x1cc = failing CE
  W FMC RWCTRL   <- 0;  W FMC CECTRL <- 0                          state 6
  wait CDMA ch 5 and 6 (2 s each; a timeout after a *successful* program panics)
  W FMC IFCTRL   <- saved
```

Several pages: after a page's 10h the next page starts at once on the next CE
whose previous program is finished (busy CEs are polled first); with a
non-zero limit at fmi+0x1d0 the 10h waits while that many CEs are still
programming (state 7) [C].  The ECC is
the same engine setting as for reads (CFG 0xf5, ECC CFG 0x801e1f00): the parity
is computed by the FMI while the data streams out; nothing in the IOP or the
kernel computes parity in software [C: no other ECC code on the path].  The
53-byte parity blocks therefore land where the read engine expects them; the
layout on the flash (`[1024 data][10 meta][53 parity]` then 7 x `[1024][53]`)
remains the [I] of `k93-nand.md` section 3, and `k93nand raw` of a page written
this way would show it.

**Status bits** [C][E]: bit 6 = ready (the FMC polls `RBB_CFG` 0x4040 for it),
bit 7 = WP# (must be 1), bit 0 = fail.  With `--status 0xe1` the emulation shows
the failure path: erase returns 0 / `*fail = 1`, write returns fSuccessful 0
(the kernel's FIL then reports a write failure and the VFL replaces the block,
section 5.3); the boot-page path returns 0x80200000 [E].

### 3.2 Multi-plane variants (not needed)

The IOP command's +0x68 becomes fmi+0x14c "wVendorType" (0x2d64 -> 0x48f4) and is
non-zero only when AppleIOPFMI runs the "VS" (vendor-specific multi-plane)
mode, whose table it builds from `_fmiInitVirtToPhysMap` (HYNIX_2P, MICRON_2P,
TOSHIBA_2P, SAMSUNG_2D, SAMSUNG_2P_2D(_EXT), TOSHIBA_2P_EXT) [C: 0x80abd5ec sets
+0x68 = fmi->vendor only for VS commands].  What it changes [C]:

| wVendorType | cmd1 (0x4bc8) | cmd2 (0x4c28) | status command (0x39f4) |
|---|---|---|---|
| 0 | 80h | 10h | 70h |
| 1, 5 | 80h / 81h by page parity | 11h then 10h | 70h (1), F1h/F2h by bit 1 (5) |
| 2, 3 | 80h | 11h then 10h | 71h, fail mask 0x1f (2); 70h (3) |
| 4 | 80h | 10h | F1h/F2h by page parity |

Single pages (opcode 6) never use VS, and the page format is the same either way.
Linux uses type 0: 80h-addr-data-10h, 70h.  Which mode iOS uses for bulk FTL
writes on this SanDisk part is irrelevant here [?].

### 3.3 What goes into the FIFOs: whitening

**Data** [C].  The CDMA's AES filter encrypts on the way in (the IOP's
h2fmi_setup_aes 0x25b4 sets fmi+0x84 = 0, "not decrypt", for opcodes 6/9/0x12):

- IOP flag 8 ("homogenize"): key = the IOP's static key at 0x13064 =
  `92a742ab08c969bf006c9412d3cc79a5` (the META key of `k93-nand.md`), chunk
  8192, IV from the **row** through the callback 0x3e98 (4 LFSR steps,
  0x80000061) -- e.g. row 0xfa000 -> `00 d0 07 00 00 e8 03 00 00 f4 01 00 00 fa
  00 00` [E];
- IOP flag 4: key and an IV per page from the kernel (key types 1-3 =
  AES-128/192/256 bytes in the command; 4-6 select fixed key ids 0x100, 0x200,
  0x201 -- the hardware UID/GID keys [I]).

AppleIOPFMI picks them from the IOFlashStorage options (0x80abd5ec) [C]: option
0x40 (kIOFlashStorageOptionUseAES) -> flag 4, option 0x80 (Homogenize) -> flag 8.
AppleNANDLegacyFTL's FIL shims choose: buffer inside the "FTL region" YAFTL
registered (user data) -> 0x40 with the user key `f65dae95...` (kext 0x80f15520)
and IVs from the LPN (vtable +0x390); otherwise, when the signature says data
whitening, 0x80 (single read 0x80f0d404, write multiple 0x80f0e064) [C].  So
every non-user page iOS writes -- VSVFL context, FTL context and index pages,
BTOCs, special blocks -- is META key + IV(row), exactly what `k93nand read -k
meta` undoes.

**Metadata** [C].  Option 0x200 (set when the signature has metadata whitening,
AppleNANDLegacyFTL +0x1a9): AppleIOPFMI's _fmiPerformIO 0x80abb7ac copies the
caller's 12-byte metadata per page into its bounce buffer and, for a write,
XORs it *before* the IOP sees it: `meta32[i] ^= T[(row + i) & 0xff]`, i = 0..2,
T = the LCG table built in start() (0x80ab6f78, global 0x80ac1e84); for a read
the same XOR runs after (0x80abbe9c) and bytes 10-11 are patched to FF
(_fmiPatchMetaFringe 0x80abb2e4).  The IOP then sends the first 10 bytes (DATSIZ
meta = 10).  It is the same XOR `meta_unwhiten` undoes.

**A page written by iOS** is therefore: data = AES-128-CBC(key, IV) of the
8192 plaintext bytes (META/IV(row), or USER/IV(LPN) for YAFTL user data),
metadata = the 12-byte FTL metadata XOR the table, first 10 bytes stored, both
BCH-protected by the engine.  A page Linux writes with the recipe of 3.4 and
the same two transforms reads back through `read_page` + `meta_unwhiten` +
`aes_cbc_decrypt(KEY_META, row)` exactly like an iOS VFL page.

### 3.4 The recipe for Linux, by PIO

The only PIO program path in the IOP is the boot page (h2fmi_tx_bootpage_pio
0x4ad8: CFG 0xf0, DATSIZ 0x20001, ECC CFG 0x1e0000, three times `INT_EN 2;
CTRL 5; wait DBG0 & 0x18; 128 words to FMI+0x14; wait INTPND bit 1`, then 10h)
[C][E] -- no metadata.  iOS feeds an ECC page with metadata only by CDMA.  The
recipe therefore keeps h2fmi_write_multi's register sequence and puts the CPU
where the two CDMA channels were, the way `k93nand read_page` already replaces
channels 5-8 on the read side (page mode, proven on the hardware):

```c
/* next to k93nand.c's read_page(); in a separate, write-enabled tool */
static int program_page(int bus, int ce, uint32_t row,
                        const uint32_t d32[2048],   /* AES-128-CBC(KEY_META, iv_for(row)) */
                        const uint8_t meta[10],     /* 12-byte meta XOR T[(row+i)&0xff], first 10 */
                        uint8_t *nstatus)
{
    uint32_t ifc;
    fmi_reset(bus);                                   /* h2fmi_reset, as 0x48f4 */
    ifc = rd(bus, B_FMC, FMC_IFCTRL);
    wr(bus, B_FMC, FMC_IFCTRL, ifc | 0x200000);       /* WP# released (2.1) */
    wr(bus, B_FMI, FMI_CFG, G.cfg);                   /* 0xf5        0x3d14 */
    wr(bus, B_FMI, FMI_DATSIZ, G.datsiz);             /* 0x14540008 */
    wr(bus, B_ECC, ECC_CFG, G.ecc_cfg);               /* 0x801e1f00 */
    wr(bus, B_ECC, ECC_PND, 0x1a8);                   /* 0x3464 */
    wr(bus, B_FMI, FMI_INTPND, 0xffffffff);
    wr(bus, B_FMI, FMI_CTRL, 4);                      /* TX enabled, as state 0 */
    for (int k = 0; k < 10; k++)                      /* [?] metadata first: CDMA ch 6 is   */
        wr8(bus, B_FMI, FMI_META, meta[k]);           /*     armed before the page starts   */
    ce_on(bus, ce);
    set_row(bus, row);                                /* ADDR1, ADDR0, ANUM 4  0x3428 */
    nand_cmd(bus, 0x0080);                            /* 0x4bc8 */
    wr(bus, B_FMC, FMC_RWCTRL, 9);
    if (wait_bits(bus, B_FMC, FMC_FSTATUS, 9, 9, 100000)) goto abort;
    wr(bus, B_FMI, FMI_CTRL, 5);                      /* 0x4e78 */
    nand_cmd(bus, 0x1000);                            /* 10h preloaded, as 0x4c28 */
    for (unsigned i = 0; i < 2048; i += 8) {          /* CDMA ch 5: 8-word bursts on request */
        if (wait_fifo(bus)) goto abort;               /* [?] DBG0 & 0x18 = room in TX       */
        for (int k = 0; k < 8; k++)
            wr(bus, B_FMI, FMI_DATA, d32[i + k]);
    }
    if (wait_bits(bus, B_FMI, FMI_INTPND, 2, 2, 100000)) goto abort;   /* state 3 */
    clear_irqs(bus);
    wr(bus, B_FMC, FMC_RWCTRL, 2);                    /* 10h: tPROG starts  0x4c68 */
    if (wait_bits(bus, B_FMC, FMC_FSTATUS, 2, 2, 100000)) goto abort;
    if (wait_ready(bus, nstatus)) goto abort;         /* start_page_read's 70h/0x50 poll, 2 s */
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    ce_off(bus, ce);
    wr(bus, B_FMC, FMC_IFCTRL, ifc);                  /* WP# back */
    if (!(*nstatus & 0x80)) return -EROFS;            /* still protected: bit 21 is not WP# */
    return *nstatus & 0x01 ? -EIO : 0;                /* EIO: grown bad block, retire it */
abort:                                                /* nothing was programmed without 10h */
    nand_cmd(bus, 0x00ff); wr(bus, B_FMC, FMC_RWCTRL, 1);    /* FFh drops the page register */
    recover(bus, ce);  wr(bus, B_FMC, FMC_IFCTRL, ifc);
    return -ETIMEDOUT;
}

static int erase_block(int bus, int ce, uint32_t block, uint8_t *nstatus)  /* 0x36c4 */
{
    fmi_reset(bus);
    uint32_t ifc = rd(bus, B_FMC, FMC_IFCTRL);
    wr(bus, B_FMC, FMC_IFCTRL, ifc | 0x200000);
    ce_on(bus, ce);
    wr(bus, B_FMC, FMC_ADDR0, row_of(&G, block, 0) & 0xffffff);
    wr(bus, B_FMC, FMC_ANUM, 2);
    nand_cmd(bus, 0xd060);
    wr(bus, B_FMC, FMC_RWCTRL, 0xb);
    int r = wait_bits(bus, B_FMC, FMC_FSTATUS, 0xb, 0xb, 100000) || wait_ready(bus, nstatus);
    wr(bus, B_FMC, FMC_RWCTRL, 0);
    ce_off(bus, ce);
    wr(bus, B_FMC, FMC_IFCTRL, ifc);
    if (r) return -ETIMEDOUT;
    if (!(*nstatus & 0x80)) return -EROFS;
    return *nstatus & 0x01 ? -EIO : 0;
}
```

(`wait_ready` is the status half of k93nand's `start_page_read`: IFCTRL &=
~bit 20, RBB_CFG 0x4040, 70h, RWCTRL 1, clear, DNUM 0, RWCTRL 0x50, INTMASK
0x20, INT_EN 0x100, poll INTPND bit 8 or FSTATUS bit 5, NSTATUS, clear.  Use the
IOP's 2 s limit for both tPROG and tBERS [C 0x3b08].)

Rules the NAND imposes [A]: erase before program; program the pages of a block
in order 0..255, each once (MLC, no partial-page programming); an interrupted
program may also damage the earlier page sharing its word line (MLC paired
pages).  Rules from iOS: never leave IFCTRL bit 21 set outside a program/erase.

Open points of the PIO recipe [?] (section 8, T2, settles them with no flash
change): whether the metadata FIFO accepts its 10 bytes before CTRL 5 (the CDMA
case suggests it: ch 6 is armed after CTRL 4 and before the page starts), and
whether DBG0 bits 3-4 mean "room for a burst" in the TX direction (the boot-page
path waits for them before writing, 0x367c).  Fallback if page mode fails: the
boot-page pattern per sector -- DATSIZ 0x14540001 for sector 0 (with the 10
metadata bytes), 0x40001 for sectors 1-7, `CTRL 5`, 256 words, `INTPND bit 1`
each -- the mirror of k93nand's sector-mode read.

Software AES encryption costs about what decryption does (table-driven, see
k93iosnand); a page in Linux = encrypt 8 KB + ~8 KB of FIFO writes + tPROG.

## 4. The emulation: `tools/nand/k93-iop-nand-emu.py`

```
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \
    cascadia-build python3 tools/nand/k93-iop-nand-emu.py [--row 0xfa000] [--ce 0] [--status 0xe0]
```

Loads the IOP image at 0, builds the `fmi` object as opcode 1 leaves it for this
part (fmi+0x1c 256 pages/block, +0x20 8 sectors, +0x24 640 spare, +0x34 10 meta,
+0x38 8192, +0x48 CFG 0xf5, +0x4c DATSIZ 0x14540008, +0x54 t = 30), and runs,
unchanged, h2fmi_erase_blocks, h2fmi_setup_aes + the IV callback, h2fmi_write_multi
(one page, wVendorType 0) and h2fmi_tx_bootpage_pio, printing every register
access.  Stubbed: the CDMA driver (logged), events, the timer, critical
sections, the block reset (logged), the log/panic functions; the idle-mode
interrupt handler (0x3f8c) is replayed when a wait finds a pending enabled
interrupt.  The register model is the only invented part: RWCTRL sets the
matching FSTATUS done bits at once, RWCTRL 0x50 matches the status at once
(FSTATUS bit 5, INTPND bit 8, NSTATUS = `--status`), CTRL 5 completes when the
transfer's bytes were written to the FIFO (PIO) or at once (CDMA), DBG0 reads
0x18.  So the order and values of the register writes are Apple's, timing and
the hardware's internals are not.  `--status 0xe1` shows the failure paths,
`--status 0x60` the WP# panics.

## 5. The VSVFL reserved pool

### 5.1 Layout (recap, [C] VFL_Format 0x80ef4bfc, _RemapBlock, k93-nand.md 7)

Per CE: 2 banks; usable vblocks per bank = pool start =
`(2^floor(log2(blocks/bank)) / 1024) * 976` = 1952 (VFL_Format) [C]; reserved
slots per bank nres = 2048 - 1952 = 96; the map `u16 map[192]` at ctx+0x26 has
bank 0's slots at 0..95 and bank 1's at 96..191.  Slot `bank*96 + i` is
physical block `2*(1952 + i) + bank`: bank 0 = even blocks 3904..4094, bank 1 =
odd blocks 3905..4095.  Per-bank allocation cursor: `u16 next_free[bank]` at
ctx+0x16 (+0x18 for bank 1), counted inside the bank (0..95) [C 0x80ef948c].

### 5.2 What the map values mean (complete)

| value | meaning | who writes it |
|---|---|---|
| < 4096 | the slot holds the data of that (original) block, which is bad or hidden | _ReplaceBadBlock 0x80ef948c |
| 0xFFF0 | **free** -- the only value an allocation takes | VFL_Format, for slots whose block is good in the BBT |
| 0xFFFF | the slot's own block is unusable: factory-bad, a special block (5.4), or a replacement that later failed | VFL_Format (BBT-bad), _ReplaceBadBlock (old slot of a re-failed block) |
| 0xFFFE | **borrowed** by the partition scheme (spare for boot/plog/nvrm/firm) | BorrowSpareBlock 0x80ef5f30 |
| 0xFFFD | a borrow candidate that failed its stress test | BorrowSpareBlock |
| 0x1000..0xFFEF | invalid: VFL_Open panics ("Bad Map Table entry ... invalid value") | -- |

VFL_Open (0x80ef660c) rebuilds the RAM bad-block bitmap from the map alone: for
a value < 0xFFF1 below 4096 the *original* block is marked bad; for any value >=
0xFFF1 the *slot's* block is marked bad; 0xFFF0 leaves it good [C].  Writes of
0xFFF0 exist only in VFL_Format [C: the only store of 0xfff0 into the map], so
between two formats every slot moves one way: free -> taken.

### 5.3 How iOS takes a slot

**Bad-block replacement**, _ReplaceBadBlock 0x80ef948c (cs, pbn) [C]: marks the
block bad in RAM; if pbn is already in the map (its replacement failed), that
slot becomes 0xFFFF; then it takes the **lowest free slot at or above
next_free[bank]** of the bad block's own bank, advancing next_free past every
slot it looks at; if that bank is exhausted, the other banks in order of
increasing next_free.  Callers: VFL_Erase 0x80ef5050 on an erase failure (then
the new slot is erased and the erase retried, up to 2 x banks replacements) and
for blocks on the scrub list, VFL_Format for factory-bad blocks and for blocks
0..(last context block) [C].  The context is stored (_StoreVFLCxt 0x80ef851c)
right after each replacement, before the block is used [C].  The live maps show
exactly this: every CE's replacements fill each bank from slot 0 upward
(vCE0: bank 0 slots 0-18 = 3904-3940 even, bank 1 slots 0-24 = 3905-3953 odd).

**Borrowing**, BorrowSpareBlock 0x80ef5f30 (option 0xb003) [C]: for every CE
and bank it counts free slots from `next_free[bank]` to the end of the bank's
section -- but indexes the map with the bank-relative cursor as an absolute
index, so bank 1's count also includes most of bank 0 and **bank 1 always
wins**; it takes the CE whose bank 1 has the most, and there the **highest free
slot** (searching down).  The candidate is stress-tested (`_stressBlock`: three
rounds of erase, program every page, read back and compare data, metadata and
the refresh threshold, then a final erase) and marked 0xFFFE if it passed,
0xFFFD (and the next candidate tried) if not; context stored [C].  So borrowed
spares are **odd blocks at the top of the pool**, spread over the CEs.

When iOS borrows [C]: AppleNANDLegacyFTL handles the platform functions
"BorrowBlocks"/"CheckBlocks" (0x80f0c67c), called by AppleNANDFactoryBBT
(0x80752e9c / 0x80752e6c) for IOFlashPartitionScheme.  The partition scheme
borrows only when it creates a partition table ("newborn", 0x8075b8e4: up to 64
spares, the count from 0x8075c498) -- i.e. after a restore -- or when, at boot,
the spares listed in its table are not all among the VFL's borrowed blocks
(CheckBlocks: AppleNANDFTL `_checkBorrowedBlocks` 0x80934e88 compares the list
with WMR_CtrlIO 0x2001d00, which enumerates every 0xFFFE slot, 0x80ef73a4).  Even
then `_borrowSpareBlocks` 0x80934c14 first re-uses every slot already 0xFFFE and
borrows only the shortfall.  iBoot never borrows ("nand_export.c: unexpected
callback requesting spare blocks", iBEC string) [C].

### 5.4 Who else touches blocks 3904..4095

- **Free slots are never read, verified, scrubbed or wear-levelled** [C]:
  VFL_Open reads only page 0 of blocks start..start+198 (the context search) and
  the four context blocks; VFL_Erase erases only mapped blocks; YAFTL reaches
  physical blocks only through the map; there is no pool scan anywhere in
  VSVFLInterface.  A free slot is touched only by the allocation that takes it
  (replacement: erased before use; borrow: stress test).  iOS never assumes a
  free slot is erased -- after a restore they are free with old contents.
- **Special blocks** (DEVICEINFOBBT on every CE; NANDDRIVERSIGN, DEVICEUNIQUEINFO,
  DIAGCONTROLINFO on CE 0): chosen at production by VFL_WriteInitialBBT
  0x80efa080 from the top of the CE downward to `(0x60 << log2(4096)) / 100` =
  3932, each candidate stress-tested: 2 on CEs 1-3 (the two DEVICEINFOBBT
  copies), on CE 0 5 (3 without DEVICEUNIQUEINFO support) plus 2 with diag
  support, i.e. up to 7 -- the first good, passing blocks from 4095 down [C].
  **Each chosen block is cleared in the bad-block bitmap that is stored as
  DEVICEINFOBBT** (0x80efaca2), so VFL_Format makes its slot 0xFFFF and nothing
  allocates it [C].  The DEVICEINFOBBT header (+0x18..+0x2c)
  lists them and fills the special-block cache (0x80ef97cc) [C].  Readers scan
  page 0 from block 4095 downward for a tag, stopping at the first match
  (0x80ef97cc; iBEC's 0x9ff2ee4c does the same on vCE0) [C]: they read nothing
  below the special blocks.  _WriteSpecialBlock 0x80efb9d0 rewrites a special
  block in place (erase + all pages + verify), never allocates [C].
- **Blocks 0..19** of each CE: 0..15 are the partition scheme's slices (format
  `slice:boot:1 slice:plog:1 slice:nvrm:6 slice:firm:8 bulk:fsys`, IOFlashStorage
  string at 0x8075f446 [C]; that this one applies here, not the "pool" format
  next to it, is [I] from fsys -- the VSVFL start -- being block 16 = 1+1+6+8),
  16..19 the four VSVFL context blocks; VFL_Format
  force-replaces blocks 0..ctx[0x694] (the last context block) so the FTL never
  sees them -- the 20 slots per CE "0..19 -> 3904..3923" [C].  The context ring
  never moves to new blocks: a context block that fails to erase is skipped
  (_StoreVFLCxt 0x80ef851c) [C].
- **NVRAM, effaceable storage, firmware**: IOFlashNVRAM ("nand-backed NVRAM"),
  AppleEffaceableNAND (plog) and AppleNANDFirmware all sit on
  IOFlashStoragePartition [C: strings], i.e. on the slices plus the partition
  table's spare list ("substituting %u:%u:%u with spare idx", 0x8075b8e4) -- and
  every spare is a borrowed, 0xFFFE slot [C].
- **Destructive paths outside normal boots**: VFL_Neuralize (boot-arg
  `nand-neuralize`), a reformat (`nand-enable-reformat`, a restore), and the
  FIL self-test FILInterfaceTest 0x80f06080 (erases and writes test blocks),
  for which no caller or pointer was found in 12H321 [I].
- **The scrub list** ctx+0x6a0 (count) / +0x6a2 (u16[20]): original block numbers
  with read trouble (_AddBlockToScrubList 0x80ef89f8), replaced at their next
  VFL_Erase [C].  It never names a pool block; it only feeds the bottom-up
  replacement.

### 5.5 Verdict

**Yes: Linux can own a band of free slots in the middle of each bank's pool,
not the top.**  The top of bank 1 is exactly where iOS borrows (and the odd
special blocks), the bottom of each bank is where bad-block replacements go;
both grow toward the middle.  Between them, a free slot is never read, erased or
reserved by iOS, and is untouched until an allocation reaches it.

Proposed window, the same on every CE (map slot -> physical block):

| bank | slots | physical blocks | count |
|---|---|---|---|
| 0 (even) | 48..87 | 4000, 4002, ..., 4078 | 40 |
| 1 (odd) | 48..63 | 4001, 4003, ..., 4031 | 16 |

= 56 blocks x 2 MB = 112 MB per CE, **448 MB raw over vCE0-3**, minus whatever
the device's map shows taken inside the window (those slots are skipped, never
substituted from outside).  Margins against the live maps of the brief:

| vCE | bus:CE | used from the bottom (bank 0 / bank 1) | free left below the window | above the window |
|---|---|---|---|---|
| 0 | FMI0:CE0 | slots 0-18 (to 3940) / 0-24 (to 3953) | 29 / 23 | b0: 8 slots (4080-4094: up to 3 special) - b1: 32 slots (4033-4095: up to 4 special + borrowed) |
| 1 | FMI1:CE0 | ~0-18 / 0-26 (to 3957) | ~29 / 21 | b0 8, b1 32 (1 special each bank + borrowed) |
| 2 | FMI0:CE2 | 0-16 (to 3936) / 0-14 (to 3933) | 31 / 33 | same |
| 3 | FMI1:CE2 | ~0-14 (to 3930; one of the 30 unplaced) / 0-14 (to 3933) | ~33 / 33 | same |

In about 13 years the worst bank (vCE1 bank 1) took 17 slots beyond the 10
forced ones, factory-bad included; 21 are left below the window.  None of the
window's blocks is in any replaced list of the brief (the highest replacement
is 3957).  If the device's map shows the borrowed set ending well above slot 64
of bank 1, bank 1's window can grow later; it must never reach the lowest
0xFFFE/0xFFFF slot of the top cluster minus a margin.

**What Linux must check before touching a block** (every Linux start, before
the first erase; the write tool refuses anything else):

1. Read all four VSVFL context blocks of the CE, every set, every copy with a
   good checksum (k93iosnand's reader).  Use iOS's choice of the current context
   -- the context block whose page-0 metadata usn is the smallest non-zero, in
   it the last set reachable from set 0 without an unreadable one (VFL_Open
   0x80ef660c) -- **and** the union over all valid copies: since slots only go
   free -> taken (5.2), a slot is free only if *every* valid copy says 0xFFF0.
2. The slot is in the window and reads 0xFFF0 as above; also refuse blocks
   < 20, the context blocks ctx+0x68e, and any block on the scrub list.
3. Not a special block: page 0 of every block from 4095 down to 3932, plain and
   META-decrypted, must not start with "NANDDRIVERSIGN", "DEVICEUNIQUEINFO",
   "DIAGCONTROLINFO" or "DEVICEINFOBBT", and the block must not be listed in the
   DEVICEINFOBBT header.  Also check the model: every special block found must
   have slot value 0xFFFF; if one reads 0xFFF0, stop -- the model is wrong.
4. First claim only: all 256 pages read "erased" (ECC status 2).  A non-erased
   free slot is possible (old data from before a restore) but unexplained on a
   first claim: skip it.  Afterwards the block's content is Linux's own (a
   Linux header in page 0 with a magic that is none of the tags above, and a
   metadata type byte none of Apple's 0x02/0x04/0x08/0x0c/0x10/0x20/0x40/0x80/0xff).
5. A Linux block whose slot no longer reads 0xFFF0 was taken by iOS: its data is
   gone (iOS erased it) and Linux must never write it again.

**Residual risks**

- iOS takes a Linux block (bottom-up replacements reaching slot 48, or a
  re-borrow larger than the top margin): Linux loses that block's data, iOS is
  unaffected; check 5 catches it at the next start.  At the observed rate this
  is decades away.  An MTD/UBI layer that reports such a block as bad keeps the
  rest of the volume.
- Linux writes into an iOS block because a check was wrong: a replacement slot
  -> silent corruption of iOS's FTL data; a borrowed slot -> NVRAM, effaceable
  storage or firmware damage, possibly an iOS that no longer boots (= DFU).
  This is the risk that matters; checks 1-4 and the test order of section 8 are
  there for it.
- 6.1.3's VSVFL is an older build [?]: the checks use the device's own map, so
  only the margins depend on the policy; the live maps match it.
- MLC: an interrupted program can corrupt the paired lower page in the same
  Linux block [A]; erase/program affect only their own block, so iOS's blocks
  are not at risk from them [A].
- Wear: all Linux writes land on 224 blocks; with wear levelling (UBI) and
  ~3000 P/E cycles for this generation of MLC [A] that is ~1.3 TB of writes --
  fine for a root's changes and /home, not for swap.
- A restore (VFL_Format) resets the map: Linux's blocks become free again for
  iOS (Linux data lost) -- but a restore ends this setup anyway.

**A stronger variant (later, not needed now):** record Linux's slots in iOS's own
map as 0xFFFD ("failed borrow candidate"): VFL_Open then marks them bad, no
allocation ever takes them, and CheckBlocks is unaffected (it only lists
0xFFFE).  It means writing iOS's VSVFL context exactly as _WriteVFLCxtToFlash
0x80ef86a4 does (ctx+4 usn_dec - 1, ctx+0 = max usn_inc + 1, ctx+0xe += 8,
checksums 0x80ef9058, 8 copies with metadata {usn_dec, 0xff x 4, 0x00, 0x80}
under META/IV(row), read back; erase the next ring block when the current one
is full) on all four CEs -- a write into the one structure iOS cannot live
without.  Do not use 0xFFFE: the next partition-table rebuild re-uses every
0xFFFE slot as a spare (_borrowSpareBlocks 0x80934c14) and would hand Linux's
blocks to NVRAM, effaceable storage or firmware.

## 6. YAFTL: writing through the FTL (lower priority)

What the code shows [C], YAFTL_Open 0x80efea7c and the block allocator
0x80efcc60 (yaFTL.c):

- Mount has two paths.  If the FTL context (CX01) in the three FTL control
  superblocks (ctx+0x69a) is valid, YAFTL "quick-mounts": the block table
  (status free/current/data/index/GC, validity counters, erase counts) and the
  index come from the context; no scan of the data superblocks on that path was
  found [I].  If it is not
  valid: "CXT is not valid. Performing full NAND R/O restore" (classify every
  superblock by its first page, BTOCs newest usn first -- the restore
  k93iosnand imitates), and if that fails a "R/W restore" that erases
  superblocks "that do not belong to anything" (0x80efecda, 0x80eff0a8) [C].
- How the context is marked stale once iOS starts writing after a mount, and
  re-validated at shutdown (YAFTL_ShutdownNotify / "flushquickmountupdate") was
  not traced [?].
- A free superblock is `BLOCK_FREE` in the context's table (quick mount) or one
  whose first page reads erased (restore); allocation takes the free block with
  the lowest erase count ("BLOCK_FREE == yaFTL.blockArray[minCountIndex].status",
  0x80efcc60) [C].

Verdict: **not safe.**  A superblock Linux filled behind a valid context is still
free to iOS, which will erase and reuse it; making iOS adopt Linux's writes would
mean invalidating iOS's context (forcing a restore on every iOS boot) and
writing complete YAFTL superblocks -- LPN/usn metadata, user-key AES with LPN
IVs, BTOCs, usn above all of iOS's, never a block iOS's GC is moving -- plus a
file system iOS accepts on top (HFS+ System; Data is per-file encrypted).  That
is reimplementing Apple's FTL write path with iOS's install as the test bed.
The pool window of 5.5 needs none of it.

## 7. What the read side learns from this

- Writes use the same CFG/DATSIZ/ECC CFG as reads (with threshold t + 1 = 31)
  and write 0x1a8 to ECC PND first (0x3464, every write and in h2fmi init)
  [C]; that it clears pending bits stays the (I) of `k93-nand.md`.
- iOS never reads a page without ECC on this path; raw (opcodes 5/7) is a debug
  path (h2fmi_tx_raw_page 0x4c90 writes 8 x [1024 data + up to 63 spare bytes]
  with CFG 5 and ECC MASK 0) [C].

## 8. Hardware test plan, in order of risk

Each step only after the previous one behaved; every step on one CE first
(vCE2 = FMI0:CE2 has the most room).  T0-T2 cannot change the flash.

| step | what | settles | risk |
|---|---|---|---|
| T0 | READ STATUS (70h, as `start_page_read`) on an idle CE with IFCTRL bit 21 clear, then set, then clear again | 2.1: bit 7 follows bit 21 -> bit 21 is WP# | none: status reads only |
| T1 | Pool dump (read-only): `k93nand read ... -k meta -O` of the context pages `k93iosnand vfl` names (and older sets/blocks), `k93nand sig` for the special blocks, then `tools/nand/k93-pool-plan.py --special ... CTX.bin...` on the host: next_free, every slot's value and class, the union over copies, the special slots = 0xFFFF check, the usable list | the real window, borrowed and special slots; 5.3's model | none |
| T2 | **Dry-run program**: on the row of page 0 of the first window block, run 3.4 up to `wait INTPND bit 1` with IFCTRL bit 21 *clear*, without the CMD 0x1000 preload and never writing RWCTRL 2; then FFh (RESET) and a block reset.  Log DBG0 before/after the 10 metadata bytes and every 64 data words, INTPND, FSTATUS | the PIO TX mechanics (metadata first? DBG0 meaning), transfer-done | none: without 10h nothing is programmed, FFh drops the page register [A]; WP# held as a second lock |
| T3 | Erase one window block that passed every check of T1 and reads fully erased; read all 256 pages back | erase recipe, status | the block was free and erased already |
| T4 | Program page 0 (pattern, Linux metadata, META AES + whitening); `k93nand read -k meta` and `raw` of it; then pages 1..255 in order; read all | program recipe, layout on flash, bit errors | only that block |
| T5 | Erase it again; boot iOS, use it, come back; re-run T1 | iOS untouched: same map, same newest context | none beyond T3 |

If T2 shows the metadata FIFO refusing early bytes, use the sector-mode fallback
of 3.4 and repeat T2.  Stop at any surprise: a status without bit 7 after bit 21
is set, a transfer that does not complete, any slot in the window that is not
0xFFF0 in some valid copy.

## 9. Open questions

| question | experiment |
|---|---|
| IFCTRL bit 21 = WP#? (2.1) | T0 |
| PIO TX: metadata bytes before CTRL 5; DBG0 bits 3-4 in TX; byte writes to FMI+0x18 | T2 |
| On-flash layout of data/meta/parity | T4 (`k93nand raw`) |
| The device's map: next_free, 0xFFFE/0xFFFD/0xFFFF slots, special blocks' slots | T1 |
| 6.1.3's VSVFL allocation policy vs 8.4.1's | T1 shows where its borrows and replacements went; T5 over time |
| How many spares the partition table borrowed | T1 (count of 0xFFFE) |
| IFCTRL bit 20 (cleared before every status poll) | not needed: copy it |
| Whether iOS uses VS multi-plane writes on this part | not needed |
| YAFTL context invalidation / re-validation | only if writing through YAFTL is ever reconsidered |
