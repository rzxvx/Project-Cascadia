# Pinned ARM clock conversion: reproducible offline comparison

The p105 ADC deadline arguments **80 / 1000** with scale **1000000** denote
80 ms / 1000 ms at the clock API boundary. This is established by executing
the pinned ARM clock conversion code, not by assuming the scale's unit.
It does not measure actual ADC settling, IRQ latency or the device clock.
See [ADC lifecycle](p105-d1946-adc-lifecycle.md) for those separate limitations.

Source: official p105 iOS **12H321** kernelcache, SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The tool accepts only that local image and distributes no Apple code bytes.
See [firmware provenance](p105-charging.md).

## Executed functions and explicit fixtures

| Actual image function | Address | Role |
| --- | --- | --- |
| `clock_interval_to_absolutetime_interval` | 0x800b5adc | Convert interval × scale to absolute ticks |
| `nanoseconds_to_absolutetime` | 0x800b5a6c | Convert a 64-bit nanosecond input to absolute ticks |
| `clock_interval_to_deadline` | 0x8001dc88 | Inline interval conversion, then add current absolute time |

The three original instruction sequences and their integer-division helpers
execute in Unicorn. Their shared frequency import **0x803920d4** points to
variable **0x803ab1b8**, replaced with an explicitly synthetic positive
32-bit frequency. The current-time helper **0x800b2b28** is mocked to return
an explicitly synthetic 64-bit value. No clock register or live device is
read. All mapped addresses belong to emulator RAM.

For `n = interval * scale`, the conversions agree with:

```text
q, r = divmod(n, 1000000000)
ticks = q * fake_frequency + floor(r * fake_frequency / 1000000000)
deadline = fake_now + ticks
```

The converter named `nanoseconds_to_absolutetime` supplies the input-unit
evidence, reinforced by the common divisor and identical arithmetic. The
actual deadline function performs that conversion before adding current
time. Apple's historical [clock API](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/osfmk/kern/clock.h)
and [deadline wrapper](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/osfmk/kern/clock.c#L835)
provide supporting interface context; the pinned image determines the tested
ARM implementation. No public x86 clock implementation is used as an ARM
register or timing oracle.

## Bounds and reproduction

```sh
python3 tools/power/emulate-clock-interval.py build/firmware/kernelcache.12H321.macho
python3 tools/power/emulate-clock-interval.py build/firmware/kernelcache.12H321.macho --interval 1000 --scale 1000000 --frequency 24000000 --now 0
python3 tests/test-clock-interval-emulation.py
```

Defaults are fake frequency 24000000 and fake current time 123456789.
At that synthetic frequency, the ADC arguments convert to 1920000 and
24000000 ticks respectively. These numbers are fixtures, not the measured
frequency or time of our iPad.

The frequency must be an integer in 1..0xffffffff; interval and scale each
in 0..0xffffffff; current time and direct nanosecond input in
0..0xffffffffffffffff. Booleans and nonintegers are refused. Zero interval
or zero scale is accepted; division never uses a zero denominator. Products
and mathematical reference results use unbounded Python integers, and the
tool refuses fixtures whose tick conversion or deadline would exceed the
64-bit result. It does not silently treat firmware integer wrap as a valid
deadline. Each actual function has a 20000-instruction budget and must return
to the synthetic sentinel. The deadline helper must execute once, and the
other functions must not invoke it.

Ten tests pass without skips in the existing offline image. They include
the original 24 comparisons (four fake frequencies × six interval/scale
pairs, each executing all three functions), ADC arguments, nanosecond/second
and 64-bit boundaries, sub-tick truncation, 32-bit frequency extremes,
high-word/carry preservation in current time, zero/maximal products,
overflow rejection, boolean/range/hash guards and instruction-budget failure.
The reference in tests also checks direct `n * frequency // 1000000000`,
independently of the tool's quotient/remainder form.

This is unit and arithmetic evidence only. It does not supply an ADC cancel
protocol, protection against late completion IRQs, analog calibration or
safe charger settings. No hardware callback, active charging or Linux IIO
driver is enabled.
