# P105 GPIO input direction: request, pinmux and IRQ lifecycle

## Reproduced defect and narrow correction

The actual S5L GPIO input callback previously cleared only cached `OUTEN`.
An inherited or explicitly selected `PERIPH` flag therefore still selected the
peripheral branch of `refresh_reg()`. `gpiod_direction_input()` returned success,
and gpiolib classified the descriptor as input, while the fake register remained
in peripheral function `0x20`. This is a source-level contract defect; the
initial register state of the corresponding physical P105 pad has not been
measured in this investigation.

The correction clears `OUTEN | PERIPH` in that one direction callback. An
explicit input request now selects the driver's existing GPIO input/IRQ branch,
just as the existing output callback already leaves peripheral mode. No
register constant, pinmux function, initialization decoder, consumer, GPIO
number, DATA cache, IRQ cache, config-write ordering or board-power value is
added or changed. The resulting write is intentionally different: for a plain
input it applies the existing IRQ-off value `0x0e`; for a cached enabled IRQ it
applies the existing trigger. It must not be described as preserving the old
peripheral register contents.

This does not enable accessory power, USB host mode, battery charging or NAND.
It does not add an unsolicited pad operation at probe, request or free. It runs
only when a consumer already invokes input direction, including IRQ startup.

## Actual Linux call chain

The comparison compiles the following original Linux 6.12 functions and the
current S5L callbacks against synthetic structures, locks, descriptors and
ordinary C-array MMIO:

```text
gpiod_request_commit
  gpiochip_generic_request
    pinctrl_gpio_request
      pinmux_request_gpio
        pin_request                         ownership bookkeeping

gpiod_configure_flags(GPIOD_IN)
  gpiod_direction_input
    S5L direction_input                     cached flags + existing refresh

gpiod_free_commit
  gpiochip_generic_free
    pinctrl_gpio_free
      pinmux_free_gpio
        pin_free                            release ownership
```

The seven GPIO-chip callback assignments and the actual pinmux-ops initializer
are extracted from the driver too. The latter supplies neither
`gpio_request_enable`, `gpio_set_direction`, nor `gpio_disable_free`.
The core does not invent a `set_mux("gpio")` operation when these hooks are
absent. Its absent optional request/free hooks are no-ops. A `GPIOD_ASIS`
request/free therefore preserves the register contents, even when initialization
cannot represent a `0x40`-only inherited function. `get_direction()` uses cached
`OUTEN`; that software classification is not proof of the physical mode of an
untouched alternate function.

Primary sources are pinned to Linux commit
`adc218676eef25575469234709c2d87185ca223a`:
[gpiolib](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/gpio/gpiolib.c),
[pinctrl core](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/pinctrl/core.c),
[pinmux core](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/pinctrl/pinmux.c).
The later-SoC upstream Apple driver also explicitly clears its peripheral field
in direction-input, but its register layout is not imported as A5 documentation:
[Apple GPIO source at the same revision](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/pinctrl/pinctrl-apple-gpio.c).

## IRQ-startup path and consumer boundaries

The S5L driver's actual IRQ startup first assigns interrupt group zero, then
calls its direction-input callback and unmasks the IRQ. Its set-type and unmask
callbacks are compiled unchanged in the fixture. Starting from fake inherited
`0xa50302b0`, an ASIS lease followed by falling-edge set-type and startup leaves
old Linux at `0xa50002b0`: the IRQ-enable word is written, but the pad still has
peripheral selector `0x20` and no trigger bits. Corrected Linux reaches
`0xa500029a`, the existing falling-edge GPIO trigger, with the same block enable
write. Upper unrelated fields survive; group zero is the pre-existing startup
policy. No physical IRQ delivery is emulated or claimed.

| P105 consumer | Actual acquisition/use in current source | Scope of this correction |
| --- | --- | --- |
| GPIO keys, pins 0–4 | `gpio_keys` uses `devm_fwnode_gpiod_get(..., GPIOD_IN, ...)` | Explicit input path is reachable; inherited peripheral state on these physical pads is unknown. Normal GPIO-input fixture is unchanged. |
| Touch IRQ, pin 22 | Apple Z2 acquires with flags `0` (`GPIOD_ASIS`), calls `gpiod_to_irq()`, then requests a falling-edge threaded IRQ | Acquisition alone does not invoke input direction. The bound S5L IRQ-startup callback does, so the same defect is reachable if PERIPH was inherited. |
| Touch reset 21 and chip-select 103 | ASIS acquisition followed by explicit output direction | Existing output callback already clears PERIPH; unchanged. |
| HSIC WLAN HOST_READY, pin 50 | Cascadia brcmfmac helper acquires ASIS, optionally drives 0, then drives 1, and puts the descriptor | Existing output path; unchanged. A fake neighbor at HDQ index 196 remains untouched. |
| HSIC DEVICE_READY, pin 51 | DT property exists, but the current brcmfmac helper does not acquire/read it | No exposed input consumer in this implementation. |
| HDQ/UART5, inherited pin 196 | HDQ battery driver maps and operates UART registers directly; its DT node has no GPIO/pinctrl lease | The GPIO input callback is not used. Probe's initial GPIO-cache reads do not refresh pads. Unknown inherited-function policy remains unresolved. |
| Accessory power, firmware GPIO 132 | Firmware evidence and offline fixtures only; no active Cascadia DT/runtime consumer introduced | Not exposed by current Linux consumers; no rail, polarity, voltage or VBUS claim. |
| Bluetooth device-wakeup, pin 9 | Existing output GPIO; shutdown pin 2 belongs to the distinct PMU GPIO chip | No S5L input correction benefit established. |
| UART1 pins 40–43 | Existing default pinctrl state selects `periph` | No new lease or ownership policy. GPIO/mux strictness and shared ownership are unchanged. |

The relevant local sources are [P105 DT](../../dts/p105ap.dts),
[Apple Z2](../../patches/files-sandcastle-port/drivers/input/touchscreen/apple-z2.c),
[HSIC helper patch](../../patches/tree/0001-cascadia.patch),
[HDQ](../../patches/files/drivers/power/supply/apple-hdq-battery.c), and
[GPIO driver](../../patches/files-sandcastle-port/drivers/pinctrl/pinctrl-s5l8940x-gpio.c).
The upstream input acquisition is visible in
[gpio_keys at the pinned Linux revision](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/input/keyboard/gpio_keys.c).

## Historical control, provenance and reproduction

The separate [lifecycle fixture](../../tests/test-soc-gpio-lifecycle.py) reads
the exact original S5L source from upstream Git blob
`b38607939ad5cc668d0551772945b1000a189837`, SHA256
`e71fc77818ad2a6358b55ee8233da79c30b2f88985decd9e34bcadb01456c53f`.
This object is the GPIO file in both earlier upstream commit
`1118174a1d19ecc10ce1bc43b33a452dc7bb48e6` and project base
`a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf`. The test reads the blob directly
with `git cat-file blob`, requests disabled lazy fetching with
`GIT_NO_LAZY_FETCH=1`, and forbids transports using `protocol.allow=never`.
No network transport is allowed; a missing pinned local blob fails explicitly.
The earlier commit need not exist in a shallow base clone. An actual local
depth-one clone of the base lacks that commit, but returns the same verified
blob bytes. A shallow clone of a later changed tree may lack the original blob;
that remains an explicit local-source prerequisite; no network retrieval is
permitted. The transport restriction also applies on older Git implementations
without the lazy-fetch environment option.

It applies the separate hash-pinned selector-mask-only fixture
[patch](../../tests/fixtures/soc-gpio-mux-only.patch) inside a temporary directory,
then verifies the exact historical post-mask source SHA256
`d0486b6e605be4770fcc6a6d24d48e147e7e1fa4409a9cdc2272bd00e453664d`.
That fixture is SHA256 `f8563b2727be5035f4a1995bd8663d205b164a793487f0e47dfd58b3ea06e78e`; it contains no input-callback edit.
Local Codex commit objects are not required for the reconstructed control.
The corrected runtime source SHA256 is
`a26ca2e2c3ed16fc7ccaf3bffdea4c593c28d43e1845a830927c2d1d4bdcd88b`.
Its control is the exact verified historical source, reconstructed from
primary upstream bytes and the earlier selector-mask patch. No baseline callback
is handwritten and the candidate input edit is never inverted to create it. Each Linux gpiolib/pinctrl/header
input is independently hash-pinned and obtained from a local pristine source
or the original Linux Git object. No download or checkout mutation occurs.

Twelve unittest methods cover the explicit-input defect and exact historical
control, the corresponding ASIS/IRQ-startup defect and historical control,
ASIS/free preservation including unknown `0x40`, unchanged HSIC-shaped output,
explicit peripheral-to-input transition, IRQ-cache/upper-field/active-low
preservation, unchanged normal input and duplicate-request rejection, and
pre-callback failure with owned cleanup, commit-free blob retrieval and
explicit missing-blob rejection. C compiles with warnings as errors
(except an existing unused callback parameter) and undefined-behavior checking.

```sh
python3 tests/test-soc-gpio-lifecycle.py -v
python3 tests/test_soc_gpio_masks.py -v
```

Recorded verification: twelve lifecycle methods pass with no skips on the local
host and in the existing Ubuntu 22 image with networking disabled. The nine
existing mask/firmware methods also pass with no skips in that image, using the
verified 12H321 kernelcache. The new lifecycle suite itself needs no firmware
or Unicorn; it requires the existing C compiler and local pinned sources/Git
objects. The mask suite's firmware oracle and pinning remain documented in
[p105-soc-gpio-masks](p105-soc-gpio-masks.md).

Lookup, lock and ownership primitives, IRQ flow-handler assignment, module refs
and MMIO are simulated. The HSIC-shaped plain output helper uses the actual raw
output commit for a synthetic active-high descriptor; this does not test every
open-drain, active-low or real USB device acquisition path. The IRQ fixture runs
actual provider set-type/startup/unmask, not the full threaded-IRQ core or a
physical interrupt source. No concurrency, physical pad routing, voltage,
input thresholds, real IRQ delivery or inherited bootloader state is validated.
Initialization's incomplete alternate-function representation, DATA/IRQ policy
relative to Apple firmware and config-write timing remain separate questions.
