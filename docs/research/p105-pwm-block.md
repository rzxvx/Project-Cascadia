# P105 PWM block — what is actually in it (2026-09-19, in progress)

The digitizer's clock comes from here: the ADT's `/arm-io/pwm` is at
`0x33500000` (size `0x1000`, gate 83, clock-id 4, hwirq 16) and its only child
is `grape-clk` — `reg = 2`, `default-hz = 0x8000`, pointed at by the
multi-touch node's `function-clock_enable` with the `Cmwp` token. 24 MHz /
32768 = 732.4, so somewhere in this block a counter wants 732 in it.

Powering the block is solved (`p105-pmgr-gates.md`). Its register map is not.

## Four files of 0x100

Writing all-ones into every word of the first kilobyte and reading back the
implemented bits (`tools/pwm-map.sh`) gives the same shape four times, at
`0x33500000`, `+0x100`, `+0x200` and `+0x300`:

| offset | implemented bits |
|---|---|
| `+0x00` `+0x04` `+0x0c` `+0x10` `+0x14` | all 32 |
| `+0x08` | not probed on purpose; it took bits 12 and 15 when written once |
| `+0x18` `+0x20` | `0x00004c1d` — bits 0, 2, 3, 4, 10, 11, 14 |
| `+0x1c` | `0x00004c3d` — the same plus bit 5, and bit 5 sets itself |
| `+0x30` | a counter, see below |
| `+0x24`..`+0x2c`, `+0x34`.. | nothing |

So the Samsung layout that `touch_cursor.c` assumed is not what this is: there
are no TCNTB/TCMPB/TCNTO triples every `0x0c`, and channel 2's counter is not
at `+0x24`. That code's base, `0x33500300`, is this block's fourth file.

The narrow registers have the bit shape of the control register Linux knows on
later Apple chips — `drivers/pwm/pwm-apple.c`: ENABLE bit 0, MODE bit 2, UPDATE
bit 5, INVERT bit 10, OUTPUT_ENABLE bit 14 — which is the first real sign that
this IP is the ancestor of the one in the M1.

## The counter at +0x30 only runs after the narrow registers are written

Two runs, one difference:

- `pwm-map.sh` wrote all-ones into every register (and put each back). While it
  did, `+0x30` in files 0 and 2 was live: `0x004e4c23`, then `0x0003f7d8` a
  moment later — a write to the file appears to reload it.
- `pwm-period.sh`, which only ever put 732 into one wide register at a time and
  never touched `+0x18`/`+0x1c`/`+0x20`, read `+0x30` as zero in all four files.

So one of those three narrow registers starts it. `tools/pwm-start.sh` takes
them one at a time and then looks for the wide register that is the period: the
counter should stop going above whatever is written there.

## Scale

The AIC's 24 MHz timebase read either side of a `peek` says one invocation of
it costs about `0x401000` ticks, ~175 ms, on the RAM root. Any rate measured by
reading the counter between two `peek` calls is in that unit, not in
instructions.

## Tools

    ssh root@10.55.0.2 sh -s < tools/pwm-map.sh      # implemented bits, whole block
    ssh root@10.55.0.2 sh -s < tools/pwm-period.sh   # 732 into each wide register
    ssh root@10.55.0.2 sh -s < tools/pwm-start.sh    # what starts it, what sets the period

`+0x08` is left alone by all of them: it took bits 12 and 15 the one time it was
written, and on a Samsung-ish TCON those start timers. A timer started with a
zero reload and an interrupt nobody handles is a hang, and the one time it
happened, it was one.
