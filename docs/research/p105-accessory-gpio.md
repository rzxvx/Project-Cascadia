# P105 accessory switch GPIO descriptor: software line 132

Pure offline analysis of the official P105 iOS 8.4.1 **12H321** kernelcache,
SHA-256 `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No runtime driver, device access or GPIO/MMIO backend is added. This narrows
one blocker from [accessory rail callbacks](p105-accessory-power.md).

## Descriptor, function and decoder

The actual P105 dock `function-acc_sw_en` ADT reference names the SoC GPIO
provider at `/device-tree/arm-io/gpio`, FourCC `GPIO`, arguments **`0x1004`**
and **`0x101`**. It is separate from the PMU GPIO controller and LDO5.

The verified cache's AppleS5L8930X kext is at VA `0x80dec000`; addresses from
other cache layouts must not be reused. The AppleS5L8930XGPIOFunction actual
instance vtable is `0x80df1d28`, from constructor PIC pair
`0x80ded9d0/0x80ded9d2` plus 8, stored at `0x80ded9d6`. Its invocation slot
`+0x3c` resolves to **`0x80dedaa8`**.

Initializer **`0x80dedb60`** copies the 16-byte ARMFunction descriptor from
its retained OSData to `object+0x18` using VLD1/VST1. The pinport argument is
then `object+0x20`; flags are bytes `+0x24..+0x27`. It validates the port
(`pinport >> 8`) against the provider's port count (`+0x70`, P105 ADT: 28),
and requires `(pinport & 0xf8) <= 7`. For accepted descriptors, instructions
`0x80dedbb0..0x80dedbba` replace the pinport by:

```text
software line = (pinport & 7) | (pinport >> 5)
0x1004 -> (16 * 8) + 4 -> 132
```

Thus **`0x1004` does not designate software line 4**. This is a software
controller index, not a connector pin, physical PCB net name or measured
pad voltage. The P105 descriptor has mode byte `+0x24 = 1`, polarity byte
`+0x25 = 1` and default-value byte `+0x26 = 0`.

Invocation `0x80dedaa8` with its ordinary value-pointer argument passes
`value XOR polarity XOR 1` to the retained GPIO provider's slot `+0x384`.
For the actual `0x101` flags, input 0/1 requests output 0/1 on software line
132. Initialization itself invokes the same callback with no value pointer;
mode 1 uses the descriptor's default byte, giving an initial output request
of 0. These are requests to a fake provider, not measured output states.

As an independent primary-code comparison, OpeniBoot's
[A4 GPIO implementation at revision 866562f](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-a4/gpio.c)
separates the port and low-three-bit pin and indexes ordinary GPIO registers
by `8 * port + pin`. That supports the index convention; it is an A4 source
and does not establish P105 accessory wiring or an A5 VBUS circuit.

## Reproduce the actual initialization and callback

[`scripts/emulate-accessory-gpio.py`](../../scripts/emulate-accessory-gpio.py)
validates the firmware hash, constructor-derived instance vtable, invocation
slot and base-initializer import. Unicorn executes the original initialization,
descriptor copy, validation, index conversion and output callback. NEON is
enabled only in the virtual CPU for that real descriptor copy.

The base validation result, OSData/cast and GPIO output provider are mocked.
By default only the provider requests are captured; no physical register
address, device handle, USB interface or GPIO backend is exposed. Only the two audited
output-mode flag shapes `0x101` and `0x1` are accepted. The latter is a
synthetic comparison fixture with opposite polarity, not another observed
P105 dock descriptor.

### Optional actual GPIOIC provider in synthetic RAM

The opt-in `--model-provider` runs the real GPIOIC provider instructions as
well. The provider's actual instance vtable `0x80df1938` is established by
constructor PIC pair `0x80ded0d8/0x80ded0da` plus 8. Verified dispatch:

| Slot | Function | Observed software operation |
|---|---|---|
| `+0x384` | `0x80ded938` | Level 0/1 selects mode 14/15 |
| `+0x380` | `0x80ded868` | Update cached 16-bit pin configuration |
| `+0x378` | `0x80ded988` | Read through retained synthetic base pointer `+0x64` |
| `+0x37c` | `0x80ded990` | Write through that base pointer |

For mode 14/15, the actual switch table and instructions form respectively
`(shadow & ~0x6f) | 2` and `(shadow & ~0x6f) | 3`. The shadow array is retained
at provider field `+0x80`; the resulting configuration is written at an offset
of `4 * software_line` from the provider's retained base pointer. Line132
therefore writes the offset `0x210` in **ordinary fake RAM**. Starting from a
synthetic shadow `0x290`, initialization requests `0x292`, and the subsequent
asserted output requests `0x293`.

IRQ-state and lock/unlock import boundaries `0x80df0874`, `0x80df05a4` and
`0x80df05b4` are intercepted; all shadow and register storage is synthetic.
The harness models neither real concurrency nor provider faults or hardware
register semantics. The software offset does not establish a physical pin,
actual pad state, voltage, boost converter or VBUS function.

[`tests/test_accessory_gpio.py`](../../tests/test_accessory_gpio.py) asserts
actual descriptor → line132, initialization output, both subsequent values,
synthetic opposite-polarity flags, invalid pin `0x1008`, out-of-range port
`0x1c00`, and refusal of unverified firmware or unaudited modes. Separate opt-in
cases assert the real provider's writes, IRQ/lock boundary sequence, unrelated
shadow fields, opposite polarity and absence of writes for invalid descriptors. Seven tests
passed in the existing resource-limited, network-disabled build image.

```bash
python3 tests/test_accessory_gpio.py
python3 scripts/emulate-accessory-gpio.py build/firmware/kernelcache.12H321.macho
python3 scripts/emulate-accessory-gpio.py build/firmware/kernelcache.12H321.macho --model-provider
```

The verified firmware is supplied locally; no Apple binary bytes are included.
An ignored report is `build/usb-host-research/accessory-gpio-emulation-v1.json`.
The optional provider report is
`build/usb-host-research/accessory-gpio-provider-emulation-v1.json`.

## Still required before a live implementation

Neither the GPIOFunction name nor `acc_sw_en` establishes a 5 V boost switch.
The physical hardware's output/register semantics, physical connection, protection
circuit, full accessory power policy and adapter-side power conversion still
need proof. This fixture is not an instruction to toggle line132. Gadget mode
and all runtime PHY/VBUS behavior remain unchanged.
