# P105 PMGR clock gates — the switches were ten registers off (2026-09-18)

Everything that ever tried to power the touch stack on this device wrote to the
wrong register. Not the wrong bit, not too early — the wrong register, by a
constant ten ids, for two years of other people's code and three weeks of ours.

## The mapping

    power-state register = 0x3f100fd8 + <ADT clock-gates id> * 4

Found by brute force from a running Linux (`tools/pmgr-map.sh`): dump the whole
28 KB PMGR window, switch on every register shaped like a powered-down device,
and after each one look at whether SPI1 or the PWM started answering on the bus.

    SPI1 (0x32100000) woke up on a write to 0x3f1010e8   -> ADT gate 68
    PWM  (0x33500000) woke up on a write to 0x3f101124   -> ADT gate 83

Both land on the same base, and the rest of the array then reads like the
machine actually behaves — see the table. Previous readings:

| Source | Formula | What it hits for spi1 (68) | for pwm (83) |
|---|---|---|---|
| this, measured | `0x3f100fd8 + id*4` | `0x3f1010e8` — spi1 | `0x3f101124` — pwm |
| kernelcache disassembly (`p105-xnu-pmgr-extract.md`) | `pmgr + 0x1000 + id*4` | `0x3f101110` — a hole, no register | `0x3f10114c` — no register |
| iBEC helper (`p105-touch-clock-re.md`) | `pmgr + 0x1008 + id*4` | `0x3f101118` — **i2c0**, already on | `0x3f101154` — no register |

That is the whole "the gate reads back enabled and the block stays dead"
mystery. The Recovery experiment that reported `g68=2FF` read `0x3f101118`
through iBEC's helper — the I2C controller the PMU hangs off, enabled long
before anyone got there. Every write aimed at the PWM, by either formula, went
to an address with no register behind it: it swallows writes and reads zero.

## The array as this machine boots (Linux, after iBSS/iBEC)

Implemented: ids **12..90**, 71 registers, holes at 29, 30, 63–66, 71, 78.
Nothing above id 90 — see *What is still open*.

| id | reg | state | ADT device |
|---|---|---|---|
| 58 | `3f1010c0` | off | sha2 |
| 59–62 | `3f1010c4`–`d0` | **on** | flash-controller0 (iBoot read NAND) |
| 68 | `3f1010e8` | off | **spi1** — the digitizer's bus |
| 69 | `3f1010ec` | off | spi2 |
| 72–75, 77 | `3f1010f8`–`10c` | **on** | uart0 (the console), uart1–3, uart5 |
| 79 | `3f101114` | off | pke |
| 80–82 | `3f101118`–`120` | **on** | i2c0 (the PMU), i2c1, i2c2 |
| 83 | `3f101124` | off | **pwm** — grape-clk for the digitizer |
| 86 | `3f101130` | off | iop |
| 87–89 | `3f101134`–`13c` | **on** | usb-complex (this ssh session rides on it) |
| 90 | `3f101140` | off | usb-complex (the fifth gate, 91, has no register) |

The clusters line up with the ADT's device groups — three I2C controllers in a
row, the UART block, the USB gates — which is the cross-check that matters more
than any single register.

## The write recipe (unchanged, it was never the problem)

From `AppleS5L8940XIO::clock_gate_switch`, kernelcache 12H321 @ `0x80b8de38`:

    write (v & ~0x10f) | 0xf, then poll until bits 7:4 == bits 3:0

On this machine both blocks settled before the first poll read. Bit 9 is set in
every implemented register; bit 8 appears on some. A register that reads all
zero is not a powered-down device, it is not a register — that is how the holes
and everything above id 90 read.

## Reading a dead block

A read of an address nobody decodes does not fault here and does not return a
constant: it hands back whatever the fabric last drove. Observed so far:
`0xd00c3ccc`, `0x0015006b`, `0x006b18f4`, and once a value that looked like a
counter. So "is this block alive" cannot be a comparison against a magic
number. The probes compare a read of the block against a read of `0x38000000`,
where there is nothing at all: same value twice means still dead. Once powered,
both blocks read plain `0x00000000`.

## What is still open

- **Everything above id 90 is somewhere else.** sgx (92), the display chain
  (clcd 103/127, mipi-dsim 104, rgbout 105/106), isp, i2s, dwi — all read zero
  in this array, yet the display is plainly running: iBoot's framebuffer is
  still being scanned out under Linux. There is a second mechanism, and
  `0x3f101200`/`0x1204` (bitmaps, `0x00005ffc` each here) and `0x3f101180`
  (`0x000f231e`, the one XNU sets bit 31 on) are the candidates.
- **The clock-id → register mapping is still a guess.** The three registers the
  old hook used (`0x3f100010`, `0x20`, `0x2c` for ids 4, 304, 307) are real and
  writable, but nothing has been proven about which clock they are. The gate
  turned out to be ten off; the clocks deserve the same suspicion.

## Tools

- `tools/touch-power-probe.sh` — first round: the state as booted, the old
  recipe, and what it does not do.
- `tools/pmgr-map.sh` — the brute force sweep that found the mapping.
- `tools/touch-power-on.sh` — switch spi1 and pwm on and read the blocks.

All three run on the device over ssh: `ssh root@10.55.0.2 sh -s < tools/<script>`.
