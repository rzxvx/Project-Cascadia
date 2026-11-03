#!/usr/bin/env python3
"""usse.py -- an assembler for the SGX543's USSE, enough for sgx2d's programs.

The encodings are the bit layouts of tools/iosgpu/usse-dis.py (Vita3K's
decoder table, GPLv2 like this repository): enc() sets an instruction's
fixed bits and its operand fields by the table's letters; the helpers below
give the fields the names Vita3K uses.  What is not in that table -- the
special instructions that emit to the rest of the GPU -- is named here from
what the programs do (docs/research/p105-gpu.md, M8).

Instructions are 64-bit.  Two things hold for every one sgx2d uses:
  - bit 50 ends the program (the "end" field where the table has one; NOP
    and the emits have it there too), except on PHAS, where it is "imm";
  - a program starts with PHAS; a PHAS with an address chains to a second
    phase at code base + address * 8.

Registers are (bank, n): 'r' temporaries, 'o' outputs, 'pa' primary
attributes (what the PDS loaded), '#' an immediate.
"""
import importlib.util, os, struct

_here = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    'usse_dis', os.path.join(_here, '..', 'iosgpu', 'usse-dis.py'))
_ud = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_ud)
LAYOUT = dict(_ud.OPS)

END = 1 << 50


def enc(name, **fields):
    """the instruction NAME with the table's letter fields set"""
    bits = LAYOUT[name]
    w = 0
    for i, ch in enumerate(bits):
        if ch == '1':
            w |= 1 << (63 - i)
    for letter, value in fields.items():
        pos = [63 - i for i, ch in enumerate(bits) if ch == letter]
        if not pos:
            raise ValueError('%s has no field %r' % (name, letter))
        if value >> len(pos):
            raise ValueError('%s.%s = %#x does not fit %d bits' % (name, letter, value, len(pos)))
        for k, p in enumerate(pos):              # most significant first
            if value >> (len(pos) - 1 - k) & 1:
                w |= 1 << p
    return w


# --- flow -----------------------------------------------------------------

def phas(next_phase=None):
    """Start of a program (phase).  With next_phase, the address (in
    instructions from the code base) of the phase that follows this one;
    without, wait_cond 7 -- what every single-phase program carries."""
    if next_phase is None:
        return enc('PHAS', i=1, w=7)
    return enc('PHAS', i=1, x=next_phase >> 14, o=next_phase >> 7 & 0x7f, d=next_phase & 0x7f)


def nop(end=False):
    return enc('NOP') | (END if end else 0)


def wdf(slot=0):
    """wait for the data fetch (load) on SLOT to land"""
    return 0xf920000000000000 | slot << 32


def dummy_load():
    """tst p0 = (0 & 0) != 0, i.e. false; p0 ld32 pa0, [pa0]; wdf 0.  A load
    that never happens, at the start of every program that reads its inputs
    -- iOS's GL driver puts it there, presumably a hardware workaround; it
    costs three instructions and is kept."""
    return [enc('VTST', s=1, d=1, c=1, e=1, z=2, m=1, b=1, k=2, f=2, l=3),
            enc('VLDST', o=1, p=1, s=1, m=1, b=1, a=1, t=1, h=1, q=2, u=2),
            wdf(0)]


# --- moves ----------------------------------------------------------------

BANK = {'r': 0, 'o': 1, 'pa': 2, 'sa': 3}


def limm(dst, value, end=False):
    """dst = a 32-bit immediate"""
    bank, n = dst
    return enc('LIMM', s=1, e=int(end), t=BANK[bank], u=n,
               i=value >> 26 & 0x3f, m=value >> 21 & 0x1f, v=value & 0x1fffff)


def mov_words(dst, src, count=1, end=False):
    """dst..dst+count-1 = src..: VBW OR with an immediate 0, repeated"""
    (db, dn), (sb, sn) = dst, src
    return enc('VBW', o=2, b=1, s=1, x=1, e=int(end), a=count - 1,
               k=BANK[db], f=BANK[sb], g=2, j=dn, m=sn)


def smlsi(dest_inc, src0_inc, src1_inc, src2_inc, src1_inc_mode=0):
    """the register increments of the repeated instructions that follow"""
    return enc('SMLSI', e=dest_inc, a=src0_inc, b=src1_inc, f=src2_inc, c=src1_inc_mode)


def vmov_f32(dst, src, repeat=1, mask=0b0011):
    """VMOV.f32 from a primary attribute to an output, repeated"""
    (db, dn), (sb, sn) = dst, src
    assert db == 'o' and sb == 'pa'
    return enc('VMOV', s=1, a=repeat - 1, o=5, w=4, l=1, f=2, h=mask, j=dn, u=sn)


# --- the pixel arithmetic: sum of products --------------------------------

def sop2m_modulate():
    """r? = shaded colour * vertex colour, all four channels (the texture
    sample times the per-vertex colour, before blending)"""
    return enc('SOP2M', s=1, w=0xf, l=6, t=2, k=2, g=2, h=2, i=0x40)


# SOP2's factor selects, for the blend: colour = src * csel1 + dst * csel2,
# alpha = src * asel1 + dst * asel2; a cmod/amod bit takes 1 - the factor.
SEL_ZERO, SEL_SRC_ALPHA, SEL_DST, SEL_SRC_ALPHA_C = 0, 1, 2, 3


def sop2_blend(csel1, csel2, cmod2, asel1, asel2, amod2, end=True):
    """o0 = pa0 (the shaded colour) blended with o0 (the tile's colour)"""
    return enc('SOP2', s=1, e=int(end), t=1, k=2, j=1,
               g=csel1, h=csel2, m=cmod2, a=asel1, l=asel2, i=amod2)


# --- emits: hand the outputs to the rest of the GPU -----------------------
# Special instructions; not in Vita3K's table.  Named by what the programs
# around them do.  All end their program (bit 50).

EMIT_STATE = 0xfb274000a0200000    # o0.. -> the tiler's state (VDM state programs)
EMIT_VERTEX = 0xfb275000a0200000   # o0.. -> the vertex, to the tiler
EMIT_PIXEL = 0xfb24000003200082    # r0..r5 = PBE state: the tile, to memory
EMIT_EVENT = 0xfb26000081200000    # r0, r1: the 3D pass's end-of-render event
EMIT_DONE = 0xf834800000000000     # the 3D pass's other event: nothing to emit


def words(program):
    return struct.pack('<%dQ' % len(program), *program)
