#!/usr/bin/env python3
"""
Port of Liter8's iBoot resolvers to Python for iPhone XR (n841) iOS 18.7.10 (22H374).

Resolvers (all anchor-based, no hardcoded offsets):

1. IBSSValidateResolver:
   Anchor: "Unknown ASN1 type %llu\\n"
   Patch: MOV X0,X20 → MOV X0,#0. The preceding B.NE is a stack-canary
   failure branch on this XR build and must remain unchanged.

2. Image4 manifest-auth gate:
   Shape: MOV X0,X20 / MOV X3,X21 / BL authenticator / CBZ W0,success /
   SUB W21,W27,#0x1e / B failure.
   Patch: make the CBZ an unconditional B to the existing success block.

3. IBSSBootArgsResolver:
   Call-shape anchor: ADRP X2 / ADD X2 / {ADD X0,SP|SUB X0,X29} / MOV W1,#0x400 / BL
   Finds largest page-tail zero run; redirects ADRP+ADD to a new slot; writes literal.

4. IBECRestoreResolver (iBEC-restore mode only):
   Shape anchor: inline nonce-cache MMIO materialization (D2900013/F2A76173/F2C00053/B9400268),
   or helper-wrapped variant on newer iOS. Changes TBNZ W8,#1 to unconditional B to the
   existing cached-nonce block.
   Patch: ibec.recovery-nonce.preserve

Modes (determines which boot-args literal + whether to add recovery-nonce patch):
  normal    "-v debug=0x2014e wdt=-1 launchd_unsecure_cache=1"
  restore   "-v wdt=-1 rd=md0 -restore"                              # iBSS-restore
  ramdisk   "rd=md0 -v wdt=-1 debug=0x2014e backlight-level=1024"    # SSHRD
  ibec      restore mode + ibec.recovery-nonce.preserve               # iBEC-restore

Reference: Liter8-iPad8/Sources/Liter8Core/Resolvers/iBoot/*.swift
"""

import os
import struct
import sys
from typing import List, Optional, Tuple

NOP = 0xD503201F
MOV_X0_0 = 0xD2800000  # MOV X0, #0 (movz x0, #0)


# ---- ARM64 helpers ----------------------------------------------------------

def decode_adrp_page(insn: int, pc: int) -> int:
    """ADRP: PC-relative, imm21<<12 added to PC & ~0xfff."""
    immlo = (insn >> 29) & 3
    immhi = (insn >> 5) & 0x7FFFF
    imm21 = (immhi << 2) | immlo
    if imm21 & (1 << 20):
        imm21 -= 1 << 21
    return (pc & ~0xFFF) + (imm21 << 12)


def decode_add_imm12(insn: int) -> int:
    sh = (insn >> 22) & 1
    imm12 = (insn >> 10) & 0xFFF
    return imm12 << (12 if sh else 0)


def decode_bcond_target(insn: int, pc: int) -> Optional[int]:
    """B.cond: 0x5400_0000 base, imm19 at bits 23:5, signed, scaled by 4."""
    if (insn & 0xFF000010) != 0x54000000:
        return None
    imm19 = (insn >> 5) & 0x7FFFF
    if imm19 & (1 << 18):
        imm19 -= 1 << 19
    return pc + imm19 * 4


def decode_bl_target(insn: int, pc: int) -> Optional[int]:
    if (insn & 0xFC000000) != 0x94000000:
        return None
    imm26 = insn & 0x03FFFFFF
    if imm26 & (1 << 25):
        imm26 -= 1 << 26
    return pc + imm26 * 4


def encode_adrp(reg: int, pc: int, target: int) -> int:
    """ADRP Xreg, target_page. Validates imm21 range."""
    page_offset = (target & ~0xFFF) - (pc & ~0xFFF)
    imm21 = page_offset >> 12
    if imm21 < -(1 << 20) or imm21 >= (1 << 20):
        raise ValueError(f"ADRP imm21 out of range: {imm21}")
    imm21 &= 0x1FFFFF  # 21 bits
    immlo = imm21 & 3
    immhi = (imm21 >> 2) & 0x7FFFF
    return 0x90000000 | (immlo << 29) | (immhi << 5) | (reg & 0x1F)


def encode_add_imm(dst: int, src: int, imm: int) -> int:
    """ADD (immediate), 64-bit, with optional LSL #12."""
    if imm < 0x1000:
        sh = 0
        imm12 = imm
    elif (imm & 0xFFF) == 0 and (imm >> 12) < 0x1000:
        sh = 1
        imm12 = imm >> 12
    else:
        raise ValueError(f"ADD imm not expressible: {imm:#x}")
    return 0x91000000 | (sh << 22) | ((imm12 & 0xFFF) << 10) | ((src & 0x1F) << 5) | (dst & 0x1F)


# ---- Image helpers ----------------------------------------------------------

def find_all_strings(data: bytes, needle: str) -> List[int]:
    """Find all NUL-terminated occurrences of needle in data."""
    b = needle.encode() + b"\x00"
    hits: List[int] = []
    pos = 0
    while True:
        i = data.find(b, pos)
        if i < 0:
            break
        hits.append(i)
        pos = i + 1
    return hits


def adrp_add_xrefs(data: bytes, text_base: int, target_offset: int) -> List[int]:
    """Return offsets of ADRP in ADRP+ADD pairs that compute target_offset+text_base."""
    target_va = text_base + target_offset
    results: List[int] = []
    for off in range(0, len(data) - 8, 4):
        a = struct.unpack_from("<I", data, off)[0]
        if (a & 0x9F000000) != 0x90000000:
            continue
        b = struct.unpack_from("<I", data, off + 4)[0]
        if (b & 0x7F800000) != 0x11000000:
            continue
        rd_a = a & 0x1F
        rn_b = (b >> 5) & 0x1F
        if rd_a != rn_b:
            continue
        page = decode_adrp_page(a, text_base + off)
        if page + decode_add_imm12(b) == target_va:
            results.append(off)
    return results


# ---- IBSSValidateResolver ---------------------------------------------------

def resolve_validate(data: bytes, text_base: int) -> List[Tuple[int, int, int, str]]:
    """
    Return the guarded validation-result patch. The candidate's B.NE remains
    intact: on this XR build it compares the stack cookie, not validation.
    Finds the ASN1 validation gate.
    """
    anchor = "Unknown ASN1 type %llu\n"
    anchors = find_all_strings(data, anchor)
    if len(anchors) != 1:
        raise RuntimeError(f'anchor "{anchor}" count: {len(anchors)}')
    anchor_offset = anchors[0]

    xrefs = adrp_add_xrefs(data, text_base, anchor_offset)
    if not xrefs:
        raise RuntimeError("no ADRP+ADD xref to anchor")

    # Scan each xref's local region for the validation shape.
    # Pattern at [branchOffset - 4 .. branchOffset + 8]:
    #   BL <any>
    #   B.NE <failure>   where failure == xref_offset - 8 (so failure+8 == xref)
    #   MOV X0, X20       encoded 0xAA1403E0
    # Plus somewhere in the next ~0x80 bytes:
    #   LDP X29, X30, [SP, #0xD0]
    LDP_X29_X30_SP_0xD0 = 0xA94D7BFD  # verified below
    MOV_X0_X20 = 0xAA1403E0

    # Verify LDP encoding for ldp x29, x30, [sp, #0xd0]
    #   LDP post/pre/signed-offset variants. Signed-offset LDP:
    #   10 101 0 010 1 imm7 Rt2 Rn Rt
    #   = 0xa9400000 | (imm7 << 15) | (Rt2 << 10) | (Rn << 5) | Rt
    #   imm7 = 0xd0 / 8 = 0x1a
    #   Rt=x29=29, Rt2=x30=30, Rn=sp=31
    #   = 0xa9400000 | (0x1a << 15) | (30 << 10) | (31 << 5) | 29
    #   = 0xa9400000 | 0xd0000 | 0x7800 | 0x3e0 | 0x1d
    #   = 0xa94d7bfd ✓

    # Note: Liter8's resolver requires "previous instruction is BL".
    # On XR 18.7.10 (n841/22H374) the preceding instruction is CMP (inlined
    # compare) rather than a function call. Dropping the BL check keeps the
    # resolver sound because the combination
    #   (B.NE target+8 == xref) ∧ (MOV X0,X20 at branch+4) ∧ (LDP x29,x30,[sp,#0xD0] nearby)
    # already uniquely identifies the ASN1-validate gate.
    candidates: List[int] = []
    for xref in xrefs:
        lower = max(0, xref - 0x800) & ~3
        for cursor in range(lower, xref, 4):
            if cursor < 4 or cursor + 8 > len(data):
                continue
            branch = struct.unpack_from("<I", data, cursor)[0]
            move = struct.unpack_from("<I", data, cursor + 4)[0]
            # branch must be B.NE
            if (branch & 0xFF00001F) != 0x54000001:
                continue
            tgt = decode_bcond_target(branch, text_base + cursor)
            if tgt is None or tgt != (text_base + xref) - 8:
                continue
            if move != MOV_X0_X20:
                continue
            scan_end = min(len(data), cursor + 0x80 + 4)
            found_ldp = False
            for k in range(cursor + 8, scan_end, 4):
                w = struct.unpack_from("<I", data, k)[0]
                if w == LDP_X29_X30_SP_0xD0:
                    found_ldp = True
                    break
            if not found_ldp:
                continue
            candidates.append(cursor)

    candidates = sorted(set(candidates))
    if len(candidates) != 1:
        raise RuntimeError(
            f"ibss-validate-asn1: {len(candidates)} candidates "
            f"({[hex(c) for c in candidates]})"
        )
    branch_offset = candidates[0]
    orig_branch = struct.unpack_from("<I", data, branch_offset)[0]
    orig_move = struct.unpack_from("<I", data, branch_offset + 4)[0]
    if (orig_branch & 0xFF00001F) != 0x54000001:
        raise RuntimeError("selected validation-result site lost its B.NE guard")
    patches = [
        (branch_offset + 4, orig_move, MOV_X0_0, "ibss.validate-asn1.result"),
    ]

    # The B.NE above is only this function's stack-cookie guard on 22H374.
    # The kernel loader's actual Image4 authentication decision is a separate
    # late gate in the manifest parser. Select it by its complete local flow:
    #
    #   MOV X0,X20; MOV X3,X21; BL authenticator; CBZ W0,success;
    #   SUB W21,W27,#0x1e; B failure
    #
    # The target begins with the two ADDs that initialize the successful
    # manifest-processing continuation. This avoids patching generic CBZs.
    auth_candidates: List[Tuple[int, int]] = []
    for call_off in range(8, len(data) - 24, 4):
        mov_x0_x20 = struct.unpack_from("<I", data, call_off - 8)[0]
        mov_x3_x21 = struct.unpack_from("<I", data, call_off - 4)[0]
        call = struct.unpack_from("<I", data, call_off)[0]
        branch_off = call_off + 4
        branch = struct.unpack_from("<I", data, branch_off)[0]
        sub_error = struct.unpack_from("<I", data, call_off + 8)[0]
        jump_error = struct.unpack_from("<I", data, call_off + 12)[0]
        if mov_x0_x20 != 0xAA1403E0 or mov_x3_x21 != 0xAA1503E3:
            continue
        if (call & 0xFC000000) != 0x94000000:
            continue
        if (branch & 0xFF00001F) != 0x34000000:  # CBZ W0
            continue
        if sub_error != 0x51007B75 or (jump_error & 0xFC000000) != 0x14000000:
            continue
        imm19 = (branch >> 5) & 0x7FFFF
        if imm19 & (1 << 18):
            imm19 -= 1 << 19
        target = branch_off + imm19 * 4
        if target <= branch_off or target + 8 > len(data):
            continue
        success0 = struct.unpack_from("<I", data, target)[0]
        success1 = struct.unpack_from("<I", data, target + 4)[0]
        if (success0 & 0xFFC003FF) != 0x910003E8:  # ADD X8,SP,#imm
            continue
        if (success1 & 0xFFC003FF) != 0x91000119:  # ADD X25,X8,#imm
            continue
        auth_candidates.append((branch_off, target))

    if len(auth_candidates) != 1:
        raise RuntimeError(
            f"image4-manifest-auth: {len(auth_candidates)} candidates "
            f"({[(hex(o), hex(t)) for o, t in auth_candidates]})"
        )
    auth_off, auth_success = auth_candidates[0]
    auth_orig = struct.unpack_from("<I", data, auth_off)[0]
    auth_repl = encode_direct_branch(text_base + auth_off, text_base + auth_success)
    patches.append(
        (auth_off, auth_orig, auth_repl, "iboot.image4-manifest-auth.branch")
    )
    return patches


# ---- IBSSBootArgsResolver ---------------------------------------------------

def find_boot_args_call_site(data: bytes, text_base: int) -> Tuple[int, int, int]:
    """
    Returns (adrp_offset, add_offset, format_offset)
    where format_offset is the file offset of the "%s" string the ADRP+ADD X2 pair points to.
    """
    candidates = []
    for off in range(0, len(data) - 20, 4):
        adrp = struct.unpack_from("<I", data, off)[0]
        add = struct.unpack_from("<I", data, off + 4)[0]
        dst = struct.unpack_from("<I", data, off + 8)[0]
        cap = struct.unpack_from("<I", data, off + 12)[0]
        call = struct.unpack_from("<I", data, off + 16)[0]
        # ADRP X2, <page>
        if (adrp & 0x9F00001F) != 0x90000002:
            continue
        # ADD X2, X2, #imm
        if (add & 0xFFC003FF) != 0x91000042:
            continue
        # ADD X0, SP, #imm  OR  SUB X0, X29, #imm
        if not ((dst & 0xFFC003FF) == 0x910003E0 or (dst & 0xFFC003FF) == 0xD10003A0):
            continue
        # MOV W1, #0x400
        if cap != 0x52808001:
            continue
        # BL
        if (call >> 26) != 0b100101:
            continue
        # resolve format offset
        page_va = decode_adrp_page(adrp, text_base + off)
        imm = decode_add_imm12(add)
        format_va = page_va + imm
        format_off = format_va - text_base
        # validate isolated "%s\0" at format_off (preceded by NUL)
        if format_off < 1 or format_off + 3 > len(data):
            continue
        if data[format_off - 1 : format_off + 3] == b"\x00%s\x00":
            candidates.append((off, off + 4, format_off))
    if len(candidates) != 1:
        raise RuntimeError(
            f"ibss-bootargs call site: {len(candidates)} candidates: "
            f"{[(hex(a), hex(b), hex(c)) for (a, b, c) in candidates]}"
        )
    return candidates[0]


def find_zero_runs(data: bytes) -> List[Tuple[int, int, int]]:
    """Return [(start, end, end_alignment)] — zero runs sorted by length descending."""
    runs: List[Tuple[int, int, int]] = []
    start = None
    for i in range(len(data) + 1):
        is_zero = i < len(data) and data[i] == 0
        if is_zero and start is None:
            start = i
        elif not is_zero and start is not None:
            end = i
            end_align = 0 if end == 0 else end & -end  # lowest set bit
            runs.append((start, end, end_align))
            start = None
    runs.sort(key=lambda r: r[1] - r[0], reverse=True)
    return runs


def find_page_tail_slot(data: bytes, required_length: int) -> Tuple[int, int, int]:
    """
    Find the largest zero run that ends on a 4KB boundary and can hold required_length.
    Returns (write_offset, run_end, capacity).
    Rejects if multiple page-tail runs could hold it (per Liter8's uniqueness rule).
    """
    runs = find_zero_runs(data)
    page_tail = [(s, e) for (s, e, _) in runs if e % 0x1000 == 0]

    def slot_in(run_start: int, run_end: int, req: int) -> Optional[Tuple[int, int, int]]:
        guarded = run_start + 8
        for align in (16, 8, 4, 2, 1):
            w = (guarded + align - 1) & ~(align - 1)
            if w + req <= run_end:
                return (w, run_end, run_end - w)
        return None

    # primary: page-tail runs
    fitting = [r for r in page_tail if slot_in(r[0], r[1], required_length) is not None]
    if fitting:
        best = page_tail[0]  # already sorted by length desc
        best_len = best[1] - best[0]
        # require unique maximum
        if len(page_tail) == 1 or (page_tail[1][1] - page_tail[1][0]) < best_len:
            s = slot_in(best[0], best[1], required_length)
            if s is None:
                raise RuntimeError("page-tail slot sizing failed")
            return s
        raise RuntimeError("ambiguous page-tail runs")

    # fallback: alignment-padding runs (end_alignment >= 256, length < end_alignment)
    padding = sorted(
        [(s, e, a) for (s, e, a) in runs if a >= 256 and (e - s) < a],
        key=lambda r: r[1] - r[0],
    )
    for (s, e, _) in padding:
        slot = slot_in(s, e, required_length)
        if slot is not None:
            return slot
    raise RuntimeError("no slot found for boot args literal")


def resolve_bootargs(
    data: bytes, text_base: int, args: str
) -> List[Tuple[int, int, int, str]]:
    assert args and "\x00" not in args and "%" not in args
    payload = args.encode("ascii") + b"\x00"

    adrp_off, add_off, format_off = find_boot_args_call_site(data, text_base)
    write_off, run_end, capacity = find_page_tail_slot(data, len(payload))

    new_adrp = encode_adrp(2, text_base + adrp_off, text_base + write_off)
    new_add = encode_add_imm(2, 2, (text_base + write_off) & 0xFFF)
    orig_adrp = struct.unpack_from("<I", data, adrp_off)[0]
    orig_add = struct.unpack_from("<I", data, add_off)[0]

    patches = [
        (adrp_off, orig_adrp, new_adrp, "ibss.boot-args.adrp"),
        (add_off, orig_add, new_add, "ibss.boot-args.add"),
    ]
    # byte-level string write — not a 4-byte word patch; handled specially by caller
    patches.append(("STR", write_off, payload, f"ibss.boot-args.string@{hex(write_off)}"))
    return patches


# ---- IBECRestoreResolver (recovery-nonce.preserve) --------------------------

MOV_W0_0 = 0x52800000  # MOV W0, #0


def decode_tbnz_target(insn: int, pc: int) -> Optional[int]:
    """TBNZ Wt/Xt, #bit, imm14. imm14 at bits [18:5], signed, scaled by 4."""
    # TBNZ: sf 011011 1 b40(5) imm14 Rt
    if (insn & 0x7F000000) != 0x37000000:  # TBNZ W variant, bit 5 == 0
        return None
    imm14 = (insn >> 5) & 0x3FFF
    if imm14 & (1 << 13):
        imm14 -= 1 << 14
    return pc + imm14 * 4


def encode_direct_branch(pc: int, target: int) -> int:
    """Unconditional B: 0x14000000 | (imm26 & 0x3ffffff), imm26 = (target-pc)/4, signed 26."""
    diff = (target - pc) >> 2
    if diff < -(1 << 25) or diff >= (1 << 25):
        raise ValueError(f"B offset out of range: {diff}")
    return 0x14000000 | (diff & 0x03FFFFFF)


def resolve_ibec_recovery_nonce(
    data: bytes, text_base: int
) -> List[Tuple[int, int, int, str]]:
    """
    Find the recovery-nonce cache check and replace TBNZ W8,#1 with
    unconditional B to the cached-nonce block (always reuse cached nonce).

    Two possible layouts:
      Pattern A (inline MMIO materialization):
        0xD2900013  mov  x19, #0x8000...     (base MMIO addr)
        0xF2A76173  movk x19, #0x3b0b, lsl #16
        0xF2C00053  movk x19, #..., lsl #32
        0xB9400268  ldr  w8, [x19]
        TBNZ W8, #1, cached_block            ← patch target
        0x52800000  mov  w0, #0
        BL   <generator>
        ...
        cached_block:
        0xB9402A60  ldr  w0, [x19, #0x28]
        0xB9402E68  ldr  w8, [x19, #0x2c]

      Pattern B (helper wraps the 3 movs; newer iOS):
        BL helper
          helper:
            0xD2900013
            0xF2A76173
            0xF2C00053
            RET
        0xB9400268  ldr w8, [x19]
        TBNZ W8, #1, cached_block            ← patch target
        ...
    """
    RET = 0xD65F03C0

    candidates: List[Tuple[int, int]] = []  # (branch_offset, target_offset)
    for cursor in range(0, len(data) - 0x3C, 4):
        first = struct.unpack_from("<I", data, cursor)[0]
        branch_off: Optional[int] = None

        # Pattern A: inline
        if (
            first == 0xD2900013
            and struct.unpack_from("<I", data, cursor + 4)[0] == 0xF2A76173
            and struct.unpack_from("<I", data, cursor + 8)[0] == 0xF2C00053
            and struct.unpack_from("<I", data, cursor + 12)[0] == 0xB9400268
        ):
            branch_off = cursor + 16

        # Pattern B: BL to a helper that materializes + RET
        elif (first & 0xFC000000) == 0x94000000:
            helper = decode_bl_target(first, text_base + cursor)
            if helper is not None:
                helper_file = helper - text_base
                if 0 <= helper_file <= len(data) - 16:
                    if (
                        struct.unpack_from("<I", data, helper_file)[0] == 0xD2900013
                        and struct.unpack_from("<I", data, helper_file + 4)[0] == 0xF2A76173
                        and struct.unpack_from("<I", data, helper_file + 8)[0] == 0xF2C00053
                        and struct.unpack_from("<I", data, helper_file + 12)[0] == RET
                        and struct.unpack_from("<I", data, cursor + 4)[0] == 0xB9400268
                    ):
                        branch_off = cursor + 8

        if branch_off is None:
            continue
        if branch_off + 12 > len(data):
            continue

        branch = struct.unpack_from("<I", data, branch_off)[0]
        # TBNZ W8, #1 — Liter8 mask: 0x7F08001F == 0x37080008
        if (branch & 0x7F08001F) != 0x37080008:
            continue
        if struct.unpack_from("<I", data, branch_off + 4)[0] != MOV_W0_0:
            continue
        # next must be a BL (to generator)
        if (struct.unpack_from("<I", data, branch_off + 8)[0] & 0xFC000000) != 0x94000000:
            continue

        tgt = decode_tbnz_target(branch, text_base + branch_off)
        if tgt is None:
            continue
        # Liter8 asserts target == branch_offset + 0x28
        if tgt != text_base + branch_off + 0x28:
            continue
        # verify cached block starts with two LDRs: 0xB9402A60 ; 0xB9402E68
        tgt_file = tgt - text_base
        if tgt_file + 8 > len(data):
            continue
        if struct.unpack_from("<I", data, tgt_file)[0] != 0xB9402A60:
            continue
        if struct.unpack_from("<I", data, tgt_file + 4)[0] != 0xB9402E68:
            continue

        candidates.append((branch_off, tgt_file))

    if len(candidates) != 1:
        raise RuntimeError(
            f"ibec-recovery-nonce: {len(candidates)} candidates "
            f"({[(hex(a), hex(b)) for a, b in candidates]})"
        )
    branch_off, target_off = candidates[0]
    orig = struct.unpack_from("<I", data, branch_off)[0]
    repl = encode_direct_branch(
        text_base + branch_off, text_base + target_off
    )
    return [
        (branch_off, orig, repl, "ibec.recovery-nonce.preserve"),
    ]


# ---- main ------------------------------------------------------------------

MODE_PRESETS = {
    "normal":  "-v debug=0x2014e wdt=-1 launchd_unsecure_cache=1",
    "restore": "-v wdt=-1 rd=md0 -restore",
    "ramdisk": "rd=md0 -v wdt=-1 debug=0x2014e backlight-level=1024",
    "ibec":    "-v wdt=-1 rd=md0 -restore",  # same literal as restore + nonce patch
}

# ---- SerialConsole --------------------------------------------------------
#
# Matches Liter8's SerialConsole.swift behaviour: append "serial=3" to the
# boot-args when requested. Routes the kernel console to the UART. Opt-in
# because without a serial cable attached the device will log nothing visible.
#
# Enabled by any of:
#   * environment variable XRA1N_SERIAL=1 (and LITER8_SERIAL=1 as an alias
#     so Liter8-style workflows carry over unchanged)
#   * CLI flag --serial anywhere in sys.argv
#   * a literal boot-args string that already contains "serial="  (then this
#     logic is a no-op; the caller took responsibility)
# The boot runners pass --serial or --no-serial explicitly. The latter takes
# precedence over inherited environment settings so non-serial builds are
# reproducible on a developer's serial-enabled shell.

SERIAL_ARG = "serial=3"


def serial_requested(argv: List[str]) -> bool:
    if "--serial" in argv and "--no-serial" in argv:
        raise ValueError("--serial and --no-serial are mutually exclusive")
    if "--no-serial" in argv:
        return False
    if "--serial" in argv:
        return True
    for key in ("XRA1N_SERIAL", "LITER8_SERIAL"):
        if os.environ.get(key) == "1":
            return True
    return False


def apply_serial(base: str, enabled: bool) -> str:
    if not enabled:
        return base
    if "serial=" in base:
        # caller already picked a serial= setting; don't double it
        return base
    return f"{base} {SERIAL_ARG}"


def main():
    argv = list(sys.argv)
    serial = serial_requested(argv)
    argv = [a for a in argv if a not in ("--serial", "--no-serial")]

    if len(argv) < 3:
        print(
            f"usage: {argv[0]} <raw-iboot> <mode-or-literal> [--serial]\n"
            f"       mode: one of {sorted(MODE_PRESETS)} (uses preset boot-args)\n"
            f"       or:   literal string to use as boot-args\n"
            f"       iBEC-mode additionally applies ibec.recovery-nonce.preserve\n"
            f"       --serial / XRA1N_SERIAL=1 / LITER8_SERIAL=1 appends '{SERIAL_ARG}'\n"
            "       --no-serial suppresses it even if the environment requests serial",
            file=sys.stderr,
        )
        sys.exit(1)
    path = argv[1]
    arg = argv[2]
    mode = arg if arg in MODE_PRESETS else None
    base_args = MODE_PRESETS[mode] if mode else arg
    boot_args = apply_serial(base_args, serial)
    with open(path, "rb") as f:
        data = bytearray(f.read())

    # iBoot binaries have their base virtual address at an entry known to iBoot
    # itself. For patch-finding by relative pattern + file-offset we can use any
    # consistent base; conventionally 0x180000000 or 0x100000000. ARM64 patterns
    # are PC-relative, so base cancels out in ADRP decoding anyway. Use 0.
    TEXT_BASE = 0

    print(f"[*] input: {path} ({len(data)} bytes)")
    print(f"[*] mode: {mode or '(literal)'}  serial: {serial}")
    print(f"[*] boot-args: {boot_args!r}")

    validate_patches = resolve_validate(bytes(data), TEXT_BASE)
    bootargs_patches = resolve_bootargs(bytes(data), TEXT_BASE, boot_args)
    nonce_patches = []
    if mode == "ibec":
        nonce_patches = resolve_ibec_recovery_nonce(bytes(data), TEXT_BASE)

    print("\n[*] ASN1 validate patches:")
    for off, orig, repl, desc in validate_patches:
        print(f"  {desc}: @0x{off:x} 0x{orig:08x} -> 0x{repl:08x}")

    print("\n[*] Boot-args patches:")
    for p in bootargs_patches:
        if p[0] == "STR":
            _, off, payload, desc = p
            print(f"  {desc}: @0x{off:x} write {len(payload)}B {payload!r}")
        else:
            off, orig, repl, desc = p
            print(f"  {desc}: @0x{off:x} 0x{orig:08x} -> 0x{repl:08x}")

    if nonce_patches:
        print("\n[*] iBEC recovery-nonce preserve:")
        for off, orig, repl, desc in nonce_patches:
            print(f"  {desc}: @0x{off:x} 0x{orig:08x} -> 0x{repl:08x}")

    # Apply all
    def apply_word(off: int, orig: int, repl: int, _desc: str) -> None:
        got = struct.unpack_from("<I", data, off)[0]
        if got != orig:
            raise RuntimeError(
                f"byte-mismatch @0x{off:x}: expected 0x{orig:08x}, got 0x{got:08x}"
            )
        struct.pack_into("<I", data, off, repl)

    for item in validate_patches:
        apply_word(*item)
    for p in bootargs_patches:
        if p[0] == "STR":
            _, off, payload, _ = p
            data[off : off + len(payload)] = payload
        else:
            apply_word(*p)
    for item in nonce_patches:
        apply_word(*item)

    out = path + ".patched"
    with open(out, "wb") as f:
        f.write(bytes(data))
    print(f"\n[+] wrote {out}")


if __name__ == "__main__":
    main()
