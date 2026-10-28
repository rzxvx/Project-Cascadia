/* Linux EABI, K93-only. No arguments: read-only mapping/counter audit.
 * --reset: arm verified watchdog reset in five seconds without resetting CUR.
 * No storage access. Never run on another board. */
.syntax unified
.arch armv7-a
.arm
.global _start
.section .text
_start:
    ldr r8, [sp]
    cmp r8, #1
    beq identity
    cmp r8, #2
    bne bad_args
    ldr r1, [sp, #8]
    ldr r2, [r1]
    ldr r3, =0x65722d2d
    cmp r2, r3
    bne bad_args
    ldr r2, [r1, #4]
    ldr r3, =0x00746573
    cmp r2, r3
    bne bad_args
identity:
    ldr r0, =compatible
    mov r1, #0
    mov r7, #5
    svc #0
    cmp r0, #0
    blt bad_board
    mov r10, r0
    sub sp, sp, #16
    mov r1, sp
    mov r2, #12
    mov r7, #3
    svc #0
    cmp r0, #12
    bne bad_board
    ldm sp, {r0-r2}
    ldr r3, =0x6c707061
    cmp r0, r3
    bne bad_board
    ldr r3, =0x396b2c65
    cmp r1, r3
    bne bad_board
    ldr r3, =0x00706133
    cmp r2, r3
    bne bad_board
    mov r0, r10
    mov r7, #6
    svc #0
    ldr r0, =devmem
    mov r1, #2
    mov r7, #5
    svc #0
    cmp r0, #0
    blt bad_mapping
    mov r4, r0
    mov r0, #0
    mov r1, #4096
    mov r2, #3
    mov r3, #1
    ldr r5, =0x3f103
    mov r7, #192
    svc #0
    cmn r0, #4096
    bcs bad_mapping
    mov r6, r0
    ldr r0, [r6, #0x2c]
    cmp r0, #0
    bne bad_control
    ldr r0, [r6, #0x20]
    mov r2, #4096
1:  subs r2, r2, #1
    bne 1b
    ldr r1, [r6, #0x20]
    cmp r1, r0
    beq bad_counter
    cmp r8, #1
    beq audited
    ldr r2, =120000000
    adds r2, r1, r2
    bcs bad_wrap
    /* These are the ONLY MMIO stores. CUR_TIME is never written. */
    str r2, [r6, #0x24]
    dsb sy
    mov r2, #4
    str r2, [r6, #0x2c]
    dsb sy
    ldr r0, [r6, #0x2c]
    tst r0, #4
    beq bad_control
    ldr r1, =armed_message
    mov r2, #(armed_end-armed_message)
    b print_ok
audited:
    ldr r1, =audit_message
    mov r2, #(audit_end-audit_message)
print_ok:
    mov r0, #1
    mov r7, #4
    svc #0
    mov r0, #0
    b exit
bad_args:    mov r0, #2
    b exit
bad_board:   mov r0, #3
    b exit
bad_mapping: mov r0, #4
    b exit
bad_control: mov r0, #5
    b exit
bad_counter: mov r0, #6
    b exit
bad_wrap:    mov r0, #7
exit:
    mov r7, #1
    svc #0
.ltorg
.section .rodata
compatible: .asciz "/sys/firmware/devicetree/base/compatible"
devmem: .asciz "/dev/mem"
audit_message: .ascii "K93_WDT_AUDIT_OK\n"
audit_end:
armed_message: .ascii "K93_RESET_ARMED_5S\n"
armed_end:
