# Private P105 reset membership and WLAN factory closure

No hardware access, runtime/DT changes or peripheral/WLAN reset. Original
verified12H321 cache SHA256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The reset helper oracle executes actual client add`80cac59c`, remove`80cac448`
and actual register accessors in fake RAM. Six OSSet identities/count/insert/
remove providers, lock imports and temporary shared-power selector are fake.
These are additional separate reset-membership sets at object`+b0..+c4`, not
the five shared power/mode sets at`+64..+74`, and not clock-mask sets.

## Exact reset domains from original dispatch tables

The actual TBB bytes at`80cac64e` are3,3,32,40,48,3 (add), at`80cac4fc`
3,3,30,37,44,3 (remove). Their actual branch targets produce:

| Kind | Exact PHY+8 reset mask | Membership policy |
|---|---:|---|
|0|10h|Shared total of kinds0/1/5|
|1|10h|Shared total of kinds0/1/5|
|2|8h|This kind's first/last owner|
|3|4h|This kind's first/last owner|
|4|20h|This kind's first/last owner|
|5|10h|Shared total of kinds0/1/5|

First add deasserts the relevant reset bit; duplicate add/no-owned-remove
produce no reset write. Removing one of two owners keeps reset released.
Removing the last member of kind0 while kind1/5 remains performs20µs delay
but does not assert sharedbit10; only removal of the last total group member
asserts it. All audited operations preserve other32-bit fields. Every original
call, including no-ops, brackets work with actual shared-selector mask16 and0;
that provider is recorded, not executed, in this oracle. It is a temporary
Level4 request, not a generic-PHY unowned count increment/put.

Eight independent regression groups PASS, including all six kind masks,
duplicates, unowned release, aggregate0/1/5, two same-kind owners,32-bit
preservation and simultaneous reset domains. Example kinds2 then3 then0
produce reset3f→37→33→23; releasing0 restores33 while retaining both other
kinds, then releasing2/3 reaches3b/3f. This proves register-policy separation,
not measured WLAN continuity or full original nested power-provider behavior.

## Actual P105 WLAN producer selects kind3/Level1

Actual S5L EHCI HSIC factory slot480 at`80c96b54` remaps its arguments and
calls stub`80c96fa8`; linkage pointer`80c97010` resolves`80c8c605`.
That factory belongs to AppleS5L8930XUSBHSIC and constructs its port instance,
then the start path`80c8c660` reads properties. Exact source PIC strings:

-`80c8c688/68e`: phy-port; if absent,`80c8c6a6/6ac`: port.
-`80c8c6dc/6de`: phy-id; recursive property absent→default0.
-`80c8c6fe/704`: clock-mask.

At`80c8c76c..774` it passes the selected port number as kind, phy-id and raw
clock-mask into stub`80c8cd38`, whose linkage pointer`80c8d020` is the same
actual PHY client factory`808622ad` used by the external removable port.

The bounded original start fixture simulates only bootarg/property/OSData
providers and stops at that factory call; it executes no factory, controller
start, MMIO, regulator or network operation. Actual P105 WLAN ADT has port3,
clock-mask30012, no phy-port or phy-id. The actual producer returns
**factory(kind3, phy-id0, rawclock30012)** with queries phy-port/port/phy-id/
clock-mask. A separate actual factory/state oracle creates kind3, filtered
clock30010, shared power membership **2h (Level1)**, reset membershipkind3
(mask4h). It is not a Level4 WLAN membership.

The external EHCI removable Port1 producer/factory previously established
kind0, filteredclock30000, shared membership4h (Level2), reset mask10h. Thus
standard external host activity is represented separately from WLAN's HSIC
reset and Level1 membership. An external release must retain WLAN's kind3,
power2h request and clock30010; it cannot blindly power down the shared PHY.
This provides concrete integration semantics, while physical uninterrupted
HSIC operation remains untested.

Three additional HSIC regression groups PASS: actual P105 fallback arguments,
synthetic phy-port override/phy-id (not board defaults), and original factory
membership/state dispatch. These fixtures do not execute inherited HSIC
controller start, platform GPIO-ready callbacks, pad tuning or publication.

## Retained provider loss regression

A missing epoch or required membership callback after an owned transition now
uses the existing retained-fault path. Restoring the callback cannot silently
resume the old physical disposition. Cold, untouched contexts remain retryable.
`python3 tests/test-experimental-usb-provider-loss.py` compiles the actual three
backends under UBSan and executes five groups, including earlier admission
blocks as negative controls and adopted active PHY state without own writes.
No physical hardware or new default binding is used.
