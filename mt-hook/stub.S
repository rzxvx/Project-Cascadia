    .syntax unified
    .thumb
    .section .text.stub,"ax",%progbits
    .global mt_stub
    .thumb_func
mt_stub:
    push {r0,r1,r2,r3,r12,lr}
    adr  r3, mt_stub
    movw r0, #0x3d34            @ ml_phys_read  = mt_stub - 0x3D34
    sub  r0, r3, r0
    orr  r0, r0, #1
    movw r1, #0x397c            @ ml_phys_write = mt_stub - 0x397C
    sub  r1, r3, r1
    orr  r1, r1, #1
    bl   mt_peek_dump
    pop  {r0,r1,r2,r3,r12,lr}
    push {r4,r5,r6,r7,lr}
    add  r7, sp, #0xc
    b.w  0x8026ee68
    .ltorg
