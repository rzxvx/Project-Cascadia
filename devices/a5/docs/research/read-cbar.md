# Reading PERIPHBASE (CBAR) on the A5

The single unresolved value in this port is the Cortex-A9 private memory region
base. Everything else came out of the ADT; this one is not in it. See
`DECISIONS.md` §5 for why it matters — without it there is no clocksource and
the kernel cannot reach userspace.

The good news is that the CPU will simply tell you. Cortex-A9 implements the
Configuration Base Address Register, and reading it takes one instruction:

```asm
mrc p15, 4, r0, c15, c0, 0   @ r0 = PERIPHBASE
```

The low 13 bits are reserved; mask with `0xFFFFE000`. The result is the base of
a 0x2000-byte region laid out as:

| offset  | block                        |
|---------|------------------------------|
| 0x0000  | SCU                          |
| 0x0100  | GIC CPU interface            |
| 0x0200  | global timer                 |
| 0x0600  | private timers and watchdogs |
| 0x1000  | GIC distributor              |

The DTS currently guesses `0x3fd00000`, taken from the second `reg` entry of the
ADT `pl310` node, which is `0x2000` long — the right size, which is suggestive
but not proof.

## Three ways to read it

### 1. From a running iOS kernel with a debugger

If you already have kernel code execution, execute the `mrc` above and print
`r0`. This is the most direct route and needs no new tooling.

### 2. From your 32-bit checkm8 loader stub

Whatever payload ends up loading the kernel already runs in a privileged mode on
the target CPU, which is exactly where CBAR is readable. Add this before the
jump and print it over the UART you are already using for the console:

```asm
    mrc     p15, 4, r0, c15, c0, 0
    lsr     r0, r0, #13         @ not "bic #0x1fff" -- that constant is not
    lsl     r0, r0, #13         @ encodable as an ARM immediate
    bl      uart_print_hex
```

`pongo/loader.S` in this tree has a `print_cbar` block wired up for exactly this,
because it is a five-line addition to code you need anyway.

### 3. Infer it from the first boot attempt

Boot `dtb/p105ap.dtb` (A9 timer nodes disabled). The kernel will come up far
enough to print through earlycon and then panic with a message along the lines
of

```
Kernel panic - not syncing: Unable to find a suitable clocksource
```

That panic is a *success* for this milestone: it means the DTB parsed, the AIC
bound, and the console works. Then read CBAR by whichever method above and boot
`dtb/p105ap-a9timer.dtb`.

## Once you know it

If it is `0x3fd00000`, nothing to do — `make dtb` already generates
`dtb/p105ap-a9timer.dtb` at that address.

If it is something else, change `pl310`'s second region handling in
`scripts/gen_p105ap_dts.py` (the `periph` variable in `render()`) to the measured
value, then `make dtb`. Please also record the real number in `DECISIONS.md` §5
so the next person does not have to repeat this.
