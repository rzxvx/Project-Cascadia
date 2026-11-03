#!/usr/bin/env python3
"""Idempotent, opt-in instrumentation of two pinned ARM startup sources."""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / 'build/linux'
parser = argparse.ArgumentParser()
parser.add_argument('--init-stages', action='store_true')
parser.add_argument('--late-stages', action='store_true')
args = parser.parse_args()
# r0/r3/ip/lr are scratch at each selected site; r1/r2 and r4-r11 survive.
# No stack, function calls, literal pools, or relocation-sensitive addresses.
# Visibility while external caches are enabled is best-effort, not proof of
# absence. Optional stage timing only updates the proven watchdog comparator.
MACRO = r'''
/* K93_EARLY_TRACE: physical display checkpoints for this RAM-only test. */
        .macro k93_band, hi
#ifdef K93_EARLY_TRACE
        movw r3, #0xc000
        movt r3, #\hi
        mvn ip, #0
        mov lr, #0x2000
.Lk93_loop\@:
        str ip, [r3]
        mrc p15, 0, r0, c1, c0, 0
        tst r0, #(1 << 2)
        mcrne p15, 0, r3, c7, c10, 1
        add r3, r3, #4
        subs lr, lr, #1
        bne .Lk93_loop\@
        dsb sy
#endif
        .endm
'''

OLD_MACRO = MACRO
MACRO = MACRO.replace('.macro k93_band, hi', '.macro k93_band, hi, seconds').replace(
    '#ifdef K93_EARLY_TRACE\n', r'''#ifdef K93_EARLY_TRACE
#ifdef K93_WDT_STAGE_TRACE
        movw r3, #0x3020
        movt r3, #0x3f10
        movw ip, #((\seconds * 24000000) & 0xffff)
        movt ip, #(((\seconds * 24000000) >> 16) & 0xffff)
        str ip, [r3, #4]
        dsb sy
#endif
''')
STAGES = {'9f86': 30, '9f88': 45, '9f8a': 60, '9f8c': 75, '9f8e': 90, '9f90': 105}
STAGE_MACRO = MACRO
MACRO = MACRO.replace('#ifdef K93_WDT_STAGE_TRACE', '#if defined(K93_WDT_STAGE_TRACE) && !defined(K93_MMU_STAGE_TRACE)') + r'''
/* Does not alter flags, r0 (SCTLR), boot args, SP, LR or callee-saved regs. */
        .macro k93_mmu_mark, seconds
#ifdef K93_MMU_STAGE_TRACE
        movw r3, #0x3020
        movt r3, #0x3f10
        movw ip, #((\seconds * 24000000) & 0xffff)
        movt ip, #(((\seconds * 24000000) >> 16) & 0xffff)
        str ip, [r3, #4]
        dsb sy
#endif
        .endm
'''
MMU_MACRO = MACRO
MACRO = MACRO.replace('#ifdef K93_MMU_STAGE_TRACE\n', '#if defined(K93_MMU_STAGE_TRACE) && !defined(K93_INIT_STAGE_TRACE)\n')

def upgrade(text):
    text = text.replace(MMU_MACRO, MACRO).replace(STAGE_MACRO, MACRO).replace(OLD_MACRO, MACRO)
    for address, seconds in STAGES.items():
        text = text.replace(f'k93_band 0x{address}\t', f'k93_band 0x{address}, {seconds}\t')
    return text

specs = {
    'arch/arm/boot/compressed/head.S': [
        ('#include "efi-header.S"', '#include "efi-header.S"\n' + MACRO),
        ('\t\tmov\tr8, r2\t\t\t@ save atags pointer', '\t\tmov\tr8, r2\t\t\t@ save atags pointer\n\t\tk93_band 0x9f86\t@ y368: decompressor startup after hyp setup'),
        ('\t\tmov\tr0, r4\n\t\tmov\tr1, sp', '\t\tk93_band 0x9f88\t@ y400: before decompress_kernel\n\t\tmov\tr0, r4\n\t\tmov\tr1, sp'),
        ('\t\tbl\tdecompress_kernel\n', '\t\tbl\tdecompress_kernel\n\t\tk93_band 0x9f8a\t@ y432: decompress_kernel returned\n'),
        ('\t\tbl\tcache_off\n', '\t\tbl\tcache_off\n\t\tk93_band 0x9f8c\t@ y464: final cache cleanup returned\n'),
    ],
    'arch/arm/kernel/head.S': [
        ('ENTRY(stext)\n', MACRO + '\nENTRY(stext)\n\tk93_band 0x9f8e\t@ y496: uncompressed kernel entry\n\tmov r0, #0\n'),
        ('\tmovs\tr10, r5\t\t\t\t@ invalid processor (r5=0)?', '\tk93_band 0x9f90\t@ y528: processor lookup returned\n\tmovs\tr10, r5\t\t\t\t@ invalid processor (r5=0)?'),
        ("\tbeq\t__error_p\t\t\t@ yes, error 'p'", "\tbeq\t__error_p\t\t\t@ yes, error 'p'\n\tk93_mmu_mark 30\t@ CPU type accepted"),
        ('\tbl\t__vet_atags\n', '\tbl\t__vet_atags\n\tk93_mmu_mark 45\t@ boot-data validation returned\n'),
        ('\tbl\t__create_page_tables\n', '\tk93_mmu_mark 60\t@ SMP and phys/virt fixups returned\n\tbl\t__create_page_tables\n\tk93_mmu_mark 75\t@ initial page tables ready\n'),
        ('1:\tb\t__enable_mmu\n', '1:\tk93_mmu_mark 90\t@ processor setup returned; preserve SCTLR r0\n\tb\t__enable_mmu\n'),
        ('#ifdef CONFIG_DEBUG_LL\n#if !defined(CONFIG_DEBUG_ICEDCC)', r'''#ifdef K93_MMU_STAGE_TRACE
#ifdef CONFIG_ARM_LPAE
#error K93 diagnostic identity map requires short-descriptor page tables
#endif
    /* Temporary device mappings for the first MMU-on watchdog checkpoint.
     * r0/r3/r5-r7 are documented scratch here; r4 is the page table base.
     * Map only watchdog's 1MiB section and four framebuffer sections.
     */
    ldr r7, [r10, #PROCINFO_IO_MMUFLAGS]
    ldr r3, =0x3f100000
    orr r3, r3, r7
    ldr r0, =0xfc4
    str r3, [r4, r0]
    ldr r0, =0x27d8
    add r0, r0, r4
    ldr r3, =0x9f600000
    orr r3, r3, r7
    mov r6, #4
.Lk93_fb_map:
    str r3, [r0], #4
    add r3, r3, #0x100000
    subs r6, r6, #1
    bne .Lk93_fb_map
#endif
#ifdef CONFIG_DEBUG_LL
#if !defined(CONFIG_DEBUG_ICEDCC)'''),
    ],
    'arch/arm/kernel/head-common.S': [
        ('\tmov\tr10, r0\n', '\tmov\tr10, r0\n\tk93_mmu_mark 105\t@ MMU-on entry, original SCTLR saved in r10\n'),
    ],
    'init/main.c': [
        ('#include <linux/types.h>\n', '#include <linux/types.h>\n#include <linux/k93_trace.h>\n'),
        ('\tset_task_stack_end_magic(&init_task);', '\tk93_boot_stage(30);\n\tset_task_stack_end_magic(&init_task);'),
        ('\tsetup_arch(&command_line);', '\tsetup_arch(&command_line);\n\tk93_boot_stage(45);'),
        ('\tsched_init();', '\tsched_init();\n\tk93_boot_stage(60);'),
        ('\tinit_IRQ();', '\tinit_IRQ();\n\tk93_boot_stage(75);'),
        ('\ttime_init();', '\ttime_init();\n\tk93_boot_stage(90);'),
        ('\tconsole_init();', '\tconsole_init();\n\tk93_boot_stage(105);'),
        ('\tlockdep_init();', '\tk93_late_stage(30);\n\tlockdep_init();'),
        ('\tcalibrate_delay();', '\tk93_late_stage(45);\n\tcalibrate_delay();\n\tk93_late_stage(60);'),
        ('\trest_init();', '\tk93_late_stage(75);\n\trest_init();'),
        ('\tschedule_preempt_disabled();', '\tk93_late_stage(90);\n\tschedule_preempt_disabled();'),
        ('\tkernel_init_freeable();', '\tk93_late_stage(105);\n\tkernel_init_freeable();'),
    ],
    'arch/arm/mm/mmu.c': [
        ('#include <linux/init.h>\n', '#include <linux/init.h>\n#include <linux/k93_trace.h>\n'),
        ('\tfor (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE)\n\t\tpmd_clear(pmd_off_k(addr));', '''\tfor (addr = 0; addr < MODULES_VADDR; addr += PMD_SIZE) {
#ifdef K93_INIT_STAGE_TRACE
        /* Retain only the diagnostic watchdog PMD (one mapped section). */
        if (addr == (0x3f100000ul & PMD_MASK))
            continue;
#endif
\t\tpmd_clear(pmd_off_k(addr));
    }'''),
    ],
}
pending = []
for rel, replacements in specs.items():
    p = TREE / rel
    original = p.read_text()
    updated = upgrade(original)
    for before, after in replacements:
        after = upgrade(after)
        if after in updated:
            continue
        if updated.count(before) != 1:
            raise SystemExit(f'{rel}: anchor absent/ambiguous; no files written')
        updated = updated.replace(before, after)
    pending.append((p, original, updated))
header_path = TREE / 'include/linux/k93_trace.h'
header = (ROOT / 'pongo/k93_kernel_trace.h').read_text()
if args.init_stages or args.late_stages:
    header = '#define K93_INIT_STAGE_TRACE 1\n' + header
if args.late_stages:
    header = '#define K93_LATE_STAGE_TRACE 1\n' + header
old_header = header_path.read_text() if header_path.exists() else ''
pending.append((header_path, old_header, header))
for p, original, updated in pending:
    if original != updated:
        p.write_text(updated)
    print('Trace anchors verified:', p.relative_to(TREE))
