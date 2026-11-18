#!/usr/bin/env python3
"""How iOS programs and erases the iPad 2's raw NAND, computed by iOS's own FMI driver.

    docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia \\
        cascadia-build python3 tools/nand/k93-iop-nand-emu.py [--row 0xfa000] [--ce 0] [--status 0xe0]

(from devices/a5; K93IOP may point at another IOP image, the default is the 12H321
one, build/k93/firmware-12H321/iop-firmware.bin.)  Nothing touches the iPad.

On the A5 the NAND is driven by the I/O processor, not by the kernel: the
AppleIOPFMI kext sends commands (opcode 3 erase one block, 0xc erase several,
6 write one page, 9 write several, 10 write a boot page ...) and the IOP firmware
(EmbeddedIOP, iBoot-2261.30.37, Thumb-2, base 0) runs the H2FMI driver whose
read side iBEC also carries.  The program and erase side exists only here.
This script loads that firmware into unicorn and runs, unchanged:

    h2fmi_erase_blocks        0x36c4  (H2fmi_erase.c; IOP opcodes 3 and 0xc)
    h2fmi_write_multi         0x48f4  (H2fmi_write.c; opcodes 6 and 9, one page here,
                                       wVendorType 0 = no multi-plane: 80h ... 10h)
    h2fmi_tx_bootpage_pio     0x4ad8  (the only PIO program path: 3 x 512 bytes, ECC on)
    h2fmi_setup_aes           0x25b4  (+ the CDMA's IV callback 0x3e98: the key and IV a
                                       write gets with the kernel's "homogenize" option)

against a model of the FMI/FMC/ECC registers of bus 0 (0x31200000 +0, +0x40000,
+0x80000) and prints every register access in order, with the iBEC/IOP names
(docs/research/k93-nand.md, section 1).

The model is deliberately simple and is the only thing here that is not Apple's:
  * FMC RWCTRL v sets the FSTATUS "cycle done" bits v & 0xb at once (cmd1 = 1,
    cmd2 = 2, address = 8); RWCTRL 0x50 (the FMC polling READ STATUS) matches at
    once: FSTATUS bit 5, FMI INTPND bit 8, NSTATUS = --status (default 0xe0:
    ready, WP# high, pass);
  * FMI CTRL 5 (host -> NAND) completes (INTPND bit 1) when the transfer's bytes
    have been written to the data FIFO by the CPU, or at once if a CDMA channel
    was started for it (the DMA itself is not modelled, only logged);
  * DBG0 reads 0x18 (FIFO ready), the timer counts slowly, events and the CDMA
    driver are stubbed; the IOP's interrupt handler for the idle mode
    (0x3f8c: INTPND -> fmi+0x80, interrupts off, event) runs when a wait on the
    FMI event finds a pending, enabled interrupt.
So the order and values of the register writes are the firmware's; the timing
and whatever the hardware does inside are not.
"""
import os
import struct
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_READ, \
    UC_HOOK_MEM_WRITE, UC_PROT_ALL
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, \
    UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC

HERE = os.path.dirname(os.path.abspath(__file__))
IOP = os.environ.get('K93IOP', os.path.join(HERE, '..', '..', 'build', 'k93', 'firmware-12H321',
                                            'iop-firmware.bin'))


def arg(name, default):
    return int(sys.argv[sys.argv.index(name) + 1], 0) if name in sys.argv else default


ROW = arg('--row', 0xfa000)         # block 4000 page 0 (256 pages per block)
CE = arg('--ce', 0)                 # bus 0 CE 0 (the IOP's ce number = bus * 8 + ce)
NSTATUS = arg('--status', 0xe0)     # NAND status byte the model answers READ STATUS with
PPB = 256

# --- the IOP firmware's functions used (all Thumb) ---------------------------
ERASE_BLOCKS = 0x36c4       # h2fmi_erase_blocks(fmi, n, u16 ce[], u32 block[], u32 *fail)
WRITE_MULTI = 0x48f4        # h2fmi_write_multi(fmi, n, u16 ce[], u32 row[], data, meta, u32 *fail, vendor)
TX_BOOTPAGE = 0x4ad8        # h2fmi_tx_bootpage_pio(fmi, ce, row, buf 1536 bytes)
SETUP_AES = 0x25b4          # h2fmi_setup_aes(fmi, cmd, iv[], chunk, key type, key, page[])
AES_IV = 0x3e98             # the CDMA's IV callback (fmi, page index, iv out)
STUBS = {
    0xb410: 'enter_critical_section', 0xb474: 'exit_critical_section',
    0xbb88: 'event_init', 0xbb98: 'event_signal', 0xbc04: 'event_wait_timeout',
    0x257c: 'timer_get_ticks', 0x2580: 'timer_get_ticks',
    0x5578: 'h2fmi_dma_execute_async', 0x55dc: 'h2fmi_dma_wait', 0x566c: 'dma_cancel',
    0xd8cc: 'nand_log', 0xb2c8: 'panic', 0xd8f4: 'task_yield', 0xd8ec: 'spin',
    0xd8f8: 'WMR_CLOCK_RESET_DEVICE', 0xb3c8: '__stack_chk_fail',
}

FMI_BASE = 0x31200000
REG = {
    (0, 0x00): 'FMI CFG', (0, 0x04): 'FMI CTRL', (0, 0x08): 'FMI STAT', (0, 0x0c): 'FMI INTPND',
    (0, 0x10): 'FMI INT_EN', (0, 0x14): 'FMI DATA FIFO', (0, 0x18): 'FMI META FIFO',
    (0, 0x1c): 'FMI DBG0', (0, 0x34): 'FMI DATSIZ',
    (1, 0x00): 'FMC ON', (1, 0x08): 'FMC IFCTRL', (1, 0x0c): 'FMC CECTRL', (1, 0x10): 'FMC RWCTRL',
    (1, 0x14): 'FMC CMD', (1, 0x18): 'FMC ADDR0', (1, 0x1c): 'FMC ADDR1', (1, 0x20): 'FMC ANUM',
    (1, 0x24): 'FMC DNUM', (1, 0x40): 'FMC INTMASK', (1, 0x44): 'FMC FSTATUS', (1, 0x48): 'FMC NSTATUS',
    (1, 0x4c): 'FMC RBB_CFG',
    (2, 0x08): 'ECC CFG', (2, 0x0c): 'ECC RESULT', (2, 0x10): 'ECC PND', (2, 0x14): 'ECC MASK',
    (2, 0x2c): 'ECC LEVEL',
}

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
fw = open(IOP, 'rb').read()
uc.mem_map(0, 0x40000, UC_PROT_ALL)
uc.mem_write(0, fw)
HEAP, STACK = 0x100000, 0x200000
uc.mem_map(HEAP, 0x100000)
uc.mem_map(STACK, 0x100000)
uc.mem_map(FMI_BASE, 0x100000)
uc.mem_map(0x3f200000, 0x1000)          # the IOP's timer page (read by inlined helpers)
TRAMP = 0x3fff0
uc.mem_write(TRAMP, bytes.fromhex('00bf00bfe047fee7'))     # nop; nop; blx r12; b .
heap = [HEAP]


def alloc(n):
    p = heap[0]
    heap[0] = (p + n + 31) & ~31
    uc.mem_write(p, b'\0' * n)
    return p


def u32(a):
    return struct.unpack('<I', uc.mem_read(a, 4))[0]


def w32(a, v):
    uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


def cstr(a):
    b = bytes(uc.mem_read(a, 200))
    return b.split(b'\0')[0].decode('latin1')


log = []
st = {}                                 # register model: (blk, off) -> value
fifo = {'data': 0, 'meta': 0, 'dma': False}


def model_reset():
    st.clear()
    st[(1, 0x08)] = 0x0ffff             # IFCTRL as h2fmi_restore_timing leaves it (fmi+0x44)
    fifo.update(data=0, meta=0, dma=False)


def datsiz_bytes():
    d = st.get((0, 0x34), 0)
    return (d & 0xff) * ((d >> 8) & 0x7ff), (d >> 19) & 0x3f


def on_write(mu, access, addr, size, value, _):
    blk, off = (addr - FMI_BASE) >> 18, (addr - FMI_BASE) & 0x3ffff
    value &= (1 << (8 * size)) - 1
    name = REG.get((blk, off), 'blk%d+%#x' % (blk, off))
    if (blk, off) == (0, 0x14):
        fifo['data'] += size
        if fifo['data'] in (4, 512, 1024) or fifo['data'] % 1024 == 0:
            log.append('    W %-15s word %d (%d bytes so far)' % (name, fifo['data'] // 4, fifo['data']))
    elif (blk, off) == (0, 0x18):
        fifo['meta'] += size
        log.append('    W %-15s <- %02x (byte)' % (name, value))
    else:
        log.append('    W %-15s <- %08x' % (name, value))
    if (blk, off) == (1, 0x44):                         # FSTATUS: write one to clear
        st[(1, 0x44)] = st.get((1, 0x44), 0) & ~value
        return
    if (blk, off) == (0, 0x0c):                         # INTPND: write one to clear
        st[(0, 0x0c)] = st.get((0, 0x0c), 0) & ~value
        return
    if (blk, off) in ((0, 0x14), (0, 0x18)):
        ds, ms = datsiz_bytes()
        if st.get((0, 0x04)) == 5 and not fifo['dma'] and fifo['data'] >= ds:
            st[(0, 0x0c)] = st.get((0, 0x0c), 0) | 2    # transfer done
        return
    st[(blk, off)] = value
    if (blk, off) == (1, 0x10) and value:               # RWCTRL: run the cycles
        if value == 0x50:                               # FMC polls READ STATUS: ready at once
            st[(1, 0x44)] = st.get((1, 0x44), 0) | 0x20
            st[(1, 0x48)] = NSTATUS
            st[(0, 0x0c)] = st.get((0, 0x0c), 0) | 0x100     # the FMC event (pending
            # whether or not INTMASK routes it yet: the firmware sets INTMASK 0x20 and
            # INT_EN 0x100 right after RWCTRL 0x50, and the interrupt is taken then)
        else:
            st[(1, 0x44)] = st.get((1, 0x44), 0) | (value & 0xb)
    if (blk, off) == (0, 0x04):                         # CTRL
        if value in (3, 5):
            fifo['data'] = fifo['meta'] = 0
            if fifo['dma'] or value == 3:
                st[(0, 0x0c)] = st.get((0, 0x0c), 0) | 2
        if value == 6:
            fifo['dma'] = False


def on_read(mu, access, addr, size, value, _):
    blk, off = (addr - FMI_BASE) >> 18, (addr - FMI_BASE) & 0x3ffff
    v = st.get((blk, off), 0)
    if (blk, off) == (0, 0x1c):
        v = 0x18                                        # DBG0: FIFO ready
    w32(addr & ~3, v)
    name = REG.get((blk, off), 'blk%d+%#x' % (blk, off))
    if (blk, off) not in ((0, 0x1c),):
        log.append('    R %-15s -> %08x' % (name, v))


uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=FMI_BASE, end=FMI_BASE + 0xfffff)
uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=FMI_BASE, end=FMI_BASE + 0xfffff)

ticks = [1000]
events = {}
FMI = [0]
stop = [None]


def ret(r0, r1=None):
    uc.reg_write(UC_ARM_REG_R0, r0 & 0xffffffff)
    if r1 is not None:
        uc.reg_write(UC_ARM_REG_R1, r1 & 0xffffffff)
    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))


def isr_idle(fmi):
    """the IOP's FMI interrupt handler for mode 0 (0x3f8c), done here"""
    intpnd = st.get((0, 0x0c), 0)
    w32(fmi + 0x80, intpnd)
    log.append('  -- interrupt: INTPND %08x -> fmi+0x80, then (0x3f8c):' % intpnd)
    for (b, o, v) in ((1, 0x40, 0), (0, 0x10, 0), (1, 0x44, 0x31ffff), (0, 0x0c, 0xf)):
        uc.mem_write(FMI_BASE + (b << 18) + o, struct.pack('<I', v))


def on_stub(mu, addr, size, _):
    a = addr & ~1
    if a not in STUBS:
        return
    r = [mu.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    sp = mu.reg_read(UC_ARM_REG_SP)
    name = STUBS[a]
    if name in ('enter_critical_section', 'exit_critical_section', 'task_yield', 'spin'):
        return ret(0)
    if name == 'timer_get_ticks':
        ticks[0] += 24
        return ret(ticks[0], 0)
    if name == 'event_init':
        uc.mem_write(r[0], bytes([r[2] & 1, 0, 0, 0, r[1] & 1]))
        return ret(0)
    if name == 'event_signal':
        uc.mem_write(r[0], b'\x01')
        return ret(0)
    if name == 'event_wait_timeout':
        ev = r[0]
        if ev == FMI[0] + 0xa8 and not uc.mem_read(ev, 1)[0]:
            if st.get((0, 0x0c), 0) & st.get((0, 0x10), 0) and u32(FMI[0] + 0xb8) == 0:
                isr_idle(FMI[0])
                uc.mem_write(ev, b'\x01')
        if uc.mem_read(ev, 1)[0]:
            if uc.mem_read(ev + 4, 1)[0] & 1:
                uc.mem_write(ev, b'\x00')
            return ret(1)
        return ret(0)
    if name == 'h2fmi_dma_execute_async':
        st5 = [u32(sp + 4 * i) for i in range(4)]
        fifo['dma'] = True
        log.append('  -- CDMA channel %d, %s, %d bytes, %d-bit, burst %d, AES ctx %#x  (%s)'
                   % (r[1], 'memory -> FIFO' if r[0] == 1 else 'FIFO -> memory', st5[0], 8 * st5[1], st5[2],
                      st5[3], {0x31200014: 'FMI0 data FIFO', 0x31200018: 'FMI0 metadata FIFO'}.get(r[3], hex(r[3]))))
        return ret(1)
    if name == 'h2fmi_dma_wait':
        log.append('  -- wait for CDMA channel %d' % r[0])
        return ret(1)
    if name == 'dma_cancel':
        log.append('  -- cancel CDMA channel %d' % r[0])
        return ret(0)
    if name == 'WMR_CLOCK_RESET_DEVICE':
        log.append('  -- block reset of FMI%d (PMGR, iBoot clock %#x)' % (r[0], 0x2f + 2 * r[0]))
        return ret(0)
    if name == 'nand_log':
        log.append('  -- log: ' + cstr(r[0]).strip() % () if '%' not in cstr(r[0]) else
                   '  -- log: %s [%s:%d]' % (cstr(r[0]).strip(), cstr(r[1]) if r[1] else '', r[2]))
        return ret(0)
    stop[0] = '%s(%s) from %#x' % (name, cstr(r[0]) if r[0] < 0x40000 else hex(r[0]), mu.reg_read(UC_ARM_REG_LR))
    mu.emu_stop()


for a in STUBS:
    uc.hook_add(UC_HOOK_CODE, on_stub, begin=a, end=a + 1)


def new_fmi():
    """the fmi object as h2fmi_iop_set_config (0x26ec) leaves it for the K93's SanDisk part"""
    f = alloc(0x200)
    w32(f + 0x00, 0)                     # bus
    w32(f + 0x04, FMI_BASE)              # register window
    w32(f + 0x1c, PPB)                   # pages per block
    w32(f + 0x20, 8)                     # sectors per page (8192 >> 10)
    w32(f + 0x24, 640)                   # spare bytes per page
    w32(f + 0x2c, 1)
    w32(f + 0x34, 10)                    # valid metadata bytes per page
    w32(f + 0x38, 8192)                  # bytes per page
    w32(f + 0x3c, 8192)
    w32(f + 0x44, 0x0ffff)               # IF_CTRL timing (iBEC's untuned value; tuned at boot)
    w32(f + 0x48, 0xf5)                  # FMI CFG: 30-bit BCH, 1 KB sectors (0x3ca4)
    w32(f + 0x4c, 0x14540008)            # FMI DATSIZ: 8 x 1 KB + 10 metadata (0x3cb0)
    uc.mem_write(f + 0x54, bytes([30]))  # ECC bits (0x3c4c)
    w32(f + 0x5c, 0)                     # SDR (raw NAND)
    w32(f + 0x1d0, 0)                    # no limit on CEs programming at once
    w32(f + 0x1d8, 1)
    uc.mem_write(f + 0xa8, bytes([0, 0, 0, 0, 1]))    # the FMI event, auto-reset
    uc.mem_write(f + 0x1a8, bytes([0, 0, 0, 0, 1]))   # the write state machine's event
    FMI[0] = f
    return f


def call(fn, args):
    regs = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
    sp = STACK + 0xf0000
    for i, v in enumerate(args[4:]):
        w32(sp + 4 * i, v)
    uc.reg_write(UC_ARM_REG_SP, sp)
    for r, v in zip(regs, args[:4]):
        uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_R12, fn | 1)
    stop[0] = None
    try:
        uc.emu_start(TRAMP | 1, TRAMP + 6, count=5_000_000)
    except UcError as e:
        print('\n'.join(log))
        sys.exit('emulation stopped: %s at pc %#x lr %#x' % (e, uc.reg_read(UC_ARM_REG_PC),
                                                            uc.reg_read(UC_ARM_REG_LR)))
    if stop[0]:
        log.append('  !! stopped in ' + stop[0])
    return uc.reg_read(UC_ARM_REG_R0)


def section(title):
    print('== ' + title)
    for line in log:
        print(line)
    log.clear()
    print()


# ---- 1. erase one block --------------------------------------------------------
model_reset()
f = new_fmi()
ces = alloc(64)
blocks = alloc(64)
fail = alloc(4)
uc.mem_write(ces, struct.pack('<H', CE))
w32(blocks, ROW // PPB)
w32(fail, 0xdead)
rc = call(ERASE_BLOCKS, (f, 1, ces, blocks, fail))
log.append('  => h2fmi_erase_blocks returned %d, *fail = %d, fmi+0x1cc (failing CE) = %#x'
           % (rc, u32(fail), u32(f + 0x1cc)))
section('ERASE: block %d (row %#x) on CE %d -- h2fmi_erase_blocks 0x36c4, NAND status %#04x'
        % (ROW // PPB, ROW // PPB * PPB, CE, NSTATUS))

# ---- 2. program one page, as opcodes 6/9 do it ------------------------------------
model_reset()
f = new_fmi()
rows = alloc(64)
w32(rows, ROW)
data_sg, meta_sg = alloc(64), alloc(64)
w32(fail, 0xdead)
# what h2fmi_iop_write_multiple (0x2d64) does first: the AES context.  The kernel asks
# for "homogenize" (IOFlashStorage option 0x80 -> IOP flag 8) on every VFL/FTL
# metadata page; only YAFTL user data brings its own key and IVs (flag 4).
cmd = alloc(0x80)
w32(cmd + 0, 9)                          # opcode 9, write multiple
w32(cmd + 4, 8)                          # flag 8: default whitening
call(SETUP_AES, (f, cmd, 0, 8192, 0, 0, rows))
iv = alloc(16)
call(AES_IV, (f, 0, iv))
key = u32(f + 0x8c)
log.append('  AES context (fmi+0x84..0xa4): decrypt %d, key type %#x, key @%#x = %s, chunk %d, '
           'IV callback %#x, IV array %#x, seeds = row array %#x'
           % (u32(f + 0x84), u32(f + 0x88), key, bytes(uc.mem_read(key, 16)).hex(), u32(f + 0x90),
              u32(f + 0x94), u32(f + 0xa0), u32(f + 0xa4)))
log.append('  IV for row %#x (callback, page index 0): %s' % (ROW, bytes(uc.mem_read(iv, 16)).hex(' ')))
section('AES for a write (h2fmi_setup_aes 0x25b4 with IOP flag 8, IV callback 0x3e98)')
model_reset()
rc = call(WRITE_MULTI, (f, 1, ces, rows, data_sg, meta_sg, fail, 0))
log.append('  => h2fmi_write_multi returned %d (fSuccessful), *fail = %d, mode back to %d, '
           'pages started %d, failing CE %#x' % (rc, u32(fail), u32(f + 0xb8), u32(f + 0x1b8), u32(f + 0x1cc)))
section('PROGRAM: one page, row %#x on CE %d -- h2fmi_write_multi 0x48f4, wVendorType 0, NAND status %#04x'
        % (ROW, CE, NSTATUS))

# ---- 3. the PIO program path (boot page: 3 x 512 bytes) ----------------------------
model_reset()
f = new_fmi()
buf = alloc(1536)
uc.mem_write(buf, bytes(range(256)) * 6)
rc = call(TX_BOOTPAGE, (f, CE, ROW, buf))
log.append('  => h2fmi_tx_bootpage_pio returned %#x' % rc)
section('PIO PROGRAM: boot page at row %#x on CE %d -- h2fmi_tx_bootpage_pio 0x4ad8' % (ROW, CE))

if NSTATUS == 0xe0:
    print('(run again with --status 0xe1 to see a program/erase failure, 0x60 for WP# low)')
