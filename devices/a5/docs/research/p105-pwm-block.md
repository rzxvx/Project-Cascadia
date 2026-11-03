# P105 PWM block — what is actually in it (2026-09-19)

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

## The recipe, from the kernelcache (2026-09-19)

Poking stopped being necessary once the right class turned up. `AppleARMPWM` in
the generic ARM platform kext leaves its hardware methods as stubs that return
`kIOReturnUnsupported`; the SoC subclass is **`AppleS5L8920XPWM`**, found by
looking for a vtable that inherits `AppleARMPWM`'s methods but overrides those
slots. Its base address is the whole block, mapped at index 0, and its accessors
are one instruction each: `ldr r0, [this, #0x58]; ldr/str rX, [r0, r1]`.

Enabling a channel (vtable slot `+0x340`, `0x80909f78` in 12H321) is three
writes:

    write(ch * 8 + 0, cycles_a)
    write(ch * 8 + 4, cycles_b)
    write(0x18 + ch * 4, 0x4003)          ; 0x4207 when its flag argument is set

Disabling (slot `+0x35c`, `0x8090a018`) writes `0` -- or `0x4000` -- to the same
control register, and when the last channel goes away it drops the block's clock
and gate through the provider.

The capture path (slot `+0x350`, `0x8090a08c`) writes `0x801` to that same
control register and reads a pair at `0x24 + ch*8` and `0x28 + ch*8`, which slot
`+0x358` subtracts from the current time: they are timestamps, and the class
`AppleARMPWMCaptureTimestampFunction` is what they are for.

That lands exactly on the map measured from this end, and explains its one
oddity: the "counter" that kept running at `+0x30` is channel 1's second capture
register, `0x28 + 1*8`, which is why it only ever moved when `+0x1c` -- channel
1's control register -- had been written.

So the block is three channels:

| channel | cycles | control | captures |
|---|---|---|---|
| 0 | `+0x00` `+0x04` | `+0x18` | `+0x24` `+0x28` |
| 1 | `+0x08` `+0x0c` | `+0x1c` | `+0x2c` `+0x30` |
| 2 | `+0x10` `+0x14` | `+0x20` | `+0x34` `+0x38` |

`0x4003` sets bit 1, which never shows up in a read-back: the implemented-bit
sweep saw `0x4c1d`, without it. A self-clearing latch, like the UPDATE bit Linux
knows at bit 5 on later chips.

## grape-clk runs, on GPIO 63 (2026-09-19)

Running the recipe on channel 2 -- grape-clk's channel, `reg = 2` in the ADT --
with 366 and 366 (732 ticks of 24 MHz, 32787 Hz) leaves its capture registers
dead. That turned out to say nothing: capture timestamps an **input** edge, and
the only thing on that wire is a digitizer with no power.

The pad says it directly. `tools/pwm-find-pin.sh` snapshots all 256 GPIO
registers six times with the channel off, six times on, six times off again,
and once more with channel 0 on instead:

    everything off       no pad changes
    channel 2 running    pin 63 (0x3fa000fc): 620 620 621 620 621 620
    channel 2 off        no pad changes
    channel 0 running    no pad changes

One pad in the whole SoC, reading as a coin toss exactly while channel 2 runs.
That is the digitizer's clock coming out of the SoC for the first time in this
project, and it needed nothing but the gate fixed and the three writes iOS
does: the pad was already in the right mode (`0x620`, set before Linux ran).

The control register reads back `0x4001` after `0x4003` is written -- bit 1
clears itself, as the implemented-bit map predicted.

A coin toss says "fast", not "32768 Hz". `tools/pwm-hz.sh` slows the channel
down and samples the pad every ~267 ms:

    12M + 12M   0011001100110010   one cycle per second, even halves
     6M + 18M   0001000100010000   high a quarter of the time
    18M +  6M   0111011101110110   high three quarters of the time
    366 + 366   0011000011000010   back to a coin toss

So the unit is the 24 MHz reference, the period is the sum of the two
registers, and **`ch*8 + 0` is the high time, `ch*8 + 4` the low time**. 366 +
366 is 732 ticks: 32787 Hz, 0.06 % above the 32768 the ADT asks for -- inside any
crystal's tolerance, and 732.42 is not an integer anyway.

grape-clk for the digitizer, in full:

    power:   0x3f101124 <- (v & ~0x10f) | 0xf, wait for bits 7:4 == 3:0
    cycles:  0x33500010 <- 366            high
             0x33500014 <- 366            low
    enable:  0x33500020 <- 0x4003         reads back 0x4001
    out on:  GPIO 63 (0x3fa000fc), already in the right mode at boot
