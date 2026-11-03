/* Diagnostic-only replacement for the 32-byte ARM zImage NOP patch area.
 * Preserve r0/r1/r2 and SP; fall through into the original branch at +0x20.
 * MMU/caches must be off (loader's fourth checkpoint). Paint white band at
 * y=320..335 in the now-observed 1024-pixel scanout, then continue normally.
 * This proves compressed-image entry, NOT decompression or kernel init.
 */
.syntax unified
.arch armv7-a
.arm
.text
    movw r3, #0xc000
    movt r3, #0x9f83
    mvn r12, #0
    mov lr, #0x4000
1:  str r12, [r3], #4
    subs lr, lr, #1
    bne 1b
    dsb sy
