# Private staged PHY power closure and binding prerequisites

Original code: verified official P10512H321 kernelcache, SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No device access or runtime source/DT changes. Three bounded emulators execute
original instructions; no native provider or electrical state is simulated.
Nine independent regression groups PASS/no skips in the existing network-none
image. This is complementary to the frozen register primitives, not an
assertion that they now constitute a working host/charging/VBUS driver.

## Separate power-gate family: precise semantic correction

Arbitrator instance `+348`→`80864870` is not the inverse of clock membership
`+344`. The actual function manages sixteen identity sets beginning at `+74`
(the normal clock-gate sets begin at `+b4`), executes the same identity removal/
insertion helper `808647f0`, and on first/last transitions calls provider
`+358(state,0,index)`. This method does not manipulate the USB_CTL upper-bit
sets and does not clear the owner's normal clock membership.

The actual AppleARMIODevice instance vtable `804d2bb8+358`→`804ae0b9` uses
count`+64` and array`+68`, checks index before dispatch, then calls its retained
platform provider `+354(power-gate-ID,state,argument)`. Its property parser
`804ae382..3b6`, PIC`804ae384/38a`→`804ca551`, reads **power-gates**. The normal
clock-gate parser `804ae34c..380` reads clock-gates into count`+5c`/array`+60`.

`adt-power-certificate.py` full-hash verifies both actual P105 ADTs:
12H321 SHA256`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`
and13G36 SHA256`e8277fd827ba250de3f46567cb1c910d4e1c80633d666510a7780e00f4ad4b59`.
Neither otgphyctrl nor usb-complex has power-gates. With a count-zero provider
(the absent-property case in freshly zeroed object storage), the exact
`804ae0b8` instructions return **e00002c7** for both states and indices0..4,
without reaching downstream dispatch. This is IOKit's unsupported code; the
[primary IOReturn header](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/IOKit/IOReturn.h)
defines that value. Actual `80864870` continues after its provider callback and
returns0: that provider error is discarded.

This closes one specific suspected extra native PHY rail setter: the P105
power-gates property interface is absent. It says nothing about independent
AccessoryPowerSource/Tristar/PMU function dispatch or external VBUS sourcing.
The actual provider object layout/fresh allocation premise is explicit in the
fixture; it is not a live read of the original object's count field.

`powergate-family-scope.patch` changes only the old EHCI document's ambiguous
'complementary clock-client' label. No source changes are applied. The older
private client oracle's label 'clock disable' for its trapped+348 callback
should likewise be renamed if that private helper is reused publicly.

## PHY frequency code is not ref-clock-sel

Actual PHY startup`80cab892..80cab906` calls its retained AppleARMIODevice
`+34c` with string **usbphyclk**, resolved from PIC`80cab894/89a`. It compares
both returned frequency words. Exact supported rates and resulting field`+8c`:

| Rate with high32=0 | Frequency code | Initial PHY+4 write |
|---|---:|---:|
|12000000Hz|0|10h|
|24000000Hz|1|11h|
|48000000Hz|2|12h|

Unknown rates or nonzero high32 take the startup failure/cleanup branch. The
bounded frequency fixture intercepts only that frequency API, follows the
original comparisons/store and stops at the success/failure boundary. It
never executes a controller, gate or PHY register operation. Its rates are
synthetic values, not measured P105 clocks.

AppleARMIODevice actual`+34c`→`804adff4` forwards the name plus clock-ids array
and count to retained platform`+348`. Actual property parser`804ae30c..34a`
loads clock-ids. P105 PHY's clock-ids is5; parent USBcomplex's is292. Named-rate
resolution behind that retained platform object is still unclosed here.
Do not assign a platform class from a bare vtable-pointer search. The earlier
PerformanceController candidate assignment in the register DESIGN was
unproven; `register-design-scope.patch` removes it without changing code.

Separately, the ADT ref-clock-sel3 enters object`+88` and contributes
`9 | (3<<1)` to the initial PHY+0 write. It is **not** the frequency code.
Both full-hash ADTs have the same version4, refsel3, tuning-val0,
usbhostset-en-incrxe0, device549/2ff3 and host54f/6df3.

Current Linux P105 PHY node has no clocks/clock-names; current driver has no
clk_get_rate/validated named-frequency input and preserves iBoot PHY state.
A future registered backend needs an audited provider for that named clock,
not an unrelated UART/touch fixed-clock reference chosen because it says24MHz.
Linux-side lookup can require clk_get_rate exactly12/24/48MHz; zero/unsupported
must fail before register writes. No new binding has been activated.

## Actual staged PHY power transition

Instance PHY`+344`→`80cabea8` checks current object powerlevel`+54` and walks
levels0..4. The new power fixture executes the exact stage dispatch and
actual write/RMW accessors. Clock+power-gate providers are recorded rather
than executed. Arbitrator`+34c` is the actual original helper`80b3a78c`,
updating mapped USBcomplex controlbit80 in fake RAM. This is a shared PHY
control operation, **not proven external VBUS or a boost enable**.

With synthetic currentlevel0, frequency24MHz, original P105 ADT parameters,
and synthetic reset30:

| Stage | Exact observable operation order |
|---|---|
|0→1|Clock membership20010; power-gate membership20010; reset+8 OR3f; +60 write2e0; +0 writef; +4 write11; +40/+44 device549/2ff3;20µs; reset clearbit1;1000µs; clock membership0; publishlevel1|
|1→2|Clock membership20010; complexbit80 set(argument0); +0 clearbit0;20µs; reset clearbit0;1000µs; publishlevel2|
|2→3|+0 clearbit3; publishlevel3|
|3→4|Metadata level4 only|
|4→3|Metadata level3 only|
|3→2|+0 setbit3; publishlevel2|
|2→1|+0 setbit0; reset setbit0; complexbit80 clear(argument1); clock membership0; publishlevel1|
|1→0|Clock membership20010; +0 write19; reset OR3f; power-gate membership0; clock membership0; publishlevel0|

Same-level requests perform no I/O. A multi-level request runs the intervening
stages in the same call. The original helper ignores provider/error statuses;
software metadata advances even when a recorded callback would be ambiguous.
A Linux implementation must instead check providers and epoch, retain a fault
and uncommitted/pending state on partial error, and never pretend a failed
transition produced the target powerlevel.

The input initiallevel and register image are explicit synthetic fixtures,
not inferred from current iBoot or Linux generic-PHY counts. Code sequencing
and stage semantics are proved; a native initial powerlevel/adoption protocol,
full clock parents, existing-client migration, quiescence and physical
validation remain missing. Cold0 initialization cannot be applied to a live
PHY simply because this fake zero-metadata test succeeds.

## Concrete next backend boundary

The now evidenced shared-power primitive can be implemented privately using
checked Regmap reads/writes and a real named-rate input, with one external
shared-owner mutex covering all clock, PHY, controller and connector users.
P105 absent-power-gates is an explicit validated board capability, not a
mocked success from a required power provider. It still requires:

1. Complete gadget/EHCI-WLAN/port/PHY membership ownership and verified state
   adoption. Current independent generic-PHY core instances do not supply it.
2. Checked normal clock membership from the register primitive. Native last
   clock release/parent policy remains denied unless independently verified.
3. Quiescence of every affected controller and explicit connection epoch.
4. A retained named PHY clock whose verified actual rate selects0/1/2.
5. Checked reset-client add/remove semantics and connector/MUX/controller
   completion, separate from the staged power operation.
6. Accessory ID/authentication and externally supplied VBUS/protection.

No global runtime repair, native VBUS write, charge command or blind role mask
is part of this group. EHCI Port1/high-speed-device then a high-speed hub/TT
remains the intended first path, with normal USB gadget and HSIC WLAN defaults
preserved until an explicit hardware trial is authorized.

## Retained provider loss regression

A missing epoch or required membership callback after an owned transition now
uses the existing retained-fault path. Restoring the callback cannot silently
resume the old physical disposition. Cold, untouched contexts remain retryable.
`python3 tests/test-experimental-usb-provider-loss.py` compiles the actual three
backends under UBSan and executes five groups, including earlier admission
blocks as negative controls and adopted active PHY state without own writes.
No physical hardware or new default binding is used.
