# Lightning accessory-side conversion: capability and missing wiring evidence

Research date: 2026-10-11. This is a separate public-source architecture note.
It adds no device operation, register sequence, adapter recommendation, or
claim that a USB host has been tested on P105. The verified 12H321 firmware
evidence remains in [accessory power](p105-accessory-power.md) and
[VBUS presence](p105-vbus-presence.md).

## A concrete accessory-side 3.3 V to 5 V design exists

Third-party patent **CN223552803U**, filed 2024-11-27 and published
2025-11-14, describes a phone/Wi-Fi/power adapter. Its description and claim
1 explicitly place a 3.3 V to 5 V boost circuit in the adapter, with the
phone supplying the lower voltage. Claims 8/9 identify a `Vacc` path through
Q1 to U4/L1 and a separate incoming `Vin` path through D2. This is an
architecture described by Dongguan Tet Precision Electronic, **not** an
Apple A1440 circuit, measured product validation, or A1432 board evidence.
[Primary patent text, claims 1 and 8/9](https://patents.google.com/patent/CN223552803U/zh).

Consequently, the electrical requirement for downstream USB VBUS does not
by itself establish that the iPad must contain the converter. Conversion
could reside in an accessory. Applying that possibility to Cascadia
requires independent evidence of the actual adapter, negotiated role,
source rail, current limit, and isolation.

## Genuine Apple accessories provide a narrower corroboration

TechInsights/Chipworks' **2012-10-18** teardown identifies an **ISL9110** die
inside Apple's Lightning-to-30-pin adapter. It does not resolve that
converter's input/output net, programmed voltage, or camera-adapter wiring.
[Primary teardown](https://www.techinsights.com/blog/apple-lightning-30-pin-adapter-teardown).

Renesas' **ISL9110/ISL9112 FN7649 Rev. 3.00, 2018-07-26** datasheet gives
ISL9110 an input range of 1.8..5.5 V, fixed 3.3 V/5 V variants and an
adjustable output up to 5.2 V. This establishes that the identified part
family can perform the conversion; it does not establish the exact variant
or operating point in the Apple adapter. It also does not prove which
direction any board net carries power.
[Manufacturer datasheet](https://www.renesas.com/en/document/dst/isl9110-isl9112-datasheet).

An unrelated Apple Lightning Dock teardown lists **TPS630250**. Its TI
datasheet limits the output to **3.6 V**, so this lead cannot substantiate a
5 V downstream VBUS converter. Merely finding a buck-boost component in an
accessory is insufficient.
[ChargerLAB's primary teardown](https://www.chargerlab.com/teardown-of-apple-iphone-lightning-dock/),
[TI TPS630250 datasheet](https://www.ti.com/lit/ds/symlink/tps630250.pdf).

## What remains unproved for P105

| Question | Current evidence | Missing evidence |
|---|---|---|
| Can accessory-side conversion produce USB voltage? | An explicit third-party design and a suitable converter in another genuine Apple accessory | Genuine A1440 schematic or independently identified converter, nets and operating point |
| Does P105 expose an accessory rail? | 12H321 `acc_pwr` resolves to D1946 LDO5; firmware request/table maximum is 3300 units | Electrical calibration, connector net and current protection on A1432 |
| Is LDO5 USB VBUS? | No evidence of that identity | Actual source-to-connector mapping; a converter cannot be inferred from a firmware name |
| Does `acc_sw_en` control USB power? | Actual GPIO ARMFunction and software line are decoded | Physical pad/net, switch identity, polarity, overcurrent handling and isolation |
| Is sensed VBUS an outgoing supply? | Actual Tristar status/optional voltage consumer is decoded | Power direction and relationship to the downstream USB socket |

Do not turn these sources into a 5 V regulator definition, label Tristar
register 5 as a boost enable, or enable a board power GPIO. A supported
camera adapter may ultimately carry the voltage conversion, but its role
identification and electrical behavior still need proof. No powered-hub,
backfeed, adapter modification or hardware experiment follows from this
note.

Cross-model board files and generic connector pinout summaries are excluded
from this evidence table. No A1455 net is promoted to A1432 physical proof.
