# Actual Cascadia 12H321 iBEC USB PHY clock producer

The actual existing unmodified `build/firmware/iBEC.dec` is iBoot2261.30.37,
282624 bytes, SHA256 `30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec`.
The filename is iBEC, while the software version string identifies its iBoot
code generation. This is the source used by our 12H321 boot preparation, not
an inference from the separately researched iOS9.3.5 iBoot.

The basis is independently resolved: vector +0x20 points to 0x9ff00040;
ARM entry computes PC−0x48 and uses expected-base literal **+0x310** (not
13G36's +0x330), with original value0x9ff00000.

## Original instruction chain

* Startup callsite0x9ff1e0a8 calls init0x9ff1ec88.
* Init loads original literal0x016e3600 (24,000,000) at image+0x1ecd0,
  writes clock structure0x9ff48110 **+8**, then initializes six other PLL rates.
* Clock getter0x9ff1f104 uses its actual TBB entry14 to branch0x9ff1f12e,
  reading exactly the clock structure+8. This path has no PMGR/PLL read.
* Boot preparation callsite0x9ff1b668 calls ADT producer0x9ff1e3f8.
  Its `arm-io/usbphy-frequency` block0x9ff1e4ea..0x9ff1e508 invokes the
  ID14 getter and writes its returned rate to the property at0x9ff1e506.

Init block, complete getter, and complete ADT preparation code and literal pool
are byteidentical in all three existing artifacts: unmodified iBEC.dec,
iBEC.patched (SHA256 `5ee16be93b87b0c41577083c7a693b8543c9d40a573ed6f8c80a18d9a0df2154`),
and iBEC.autogo (SHA256 `e987b101800bedf03c086aa55854ff594fcce1b0f55ea1424f3ccf180a40abce`).
Ranges checked: [0x1ec88,0x1ecdc), [0x1f104,0x1f180), [0x1e3f8,0x1e5e4).
Thus our local bootloader patches preserve this original named-rate producer.

## Offline regression and bounds

`emulate-frequency12.py` executes original instructions, with a fullhash check
and independently correct basis literal check before each run. Four stale
prior source values become24MHz after original init. Four independent source
values (0/12/24/48MHz) pass unchanged through the ID14 getter and property
block; absent-property controls preserve sentinel storage.
Eight groups /sixteen executions PASS, instruction cap1000.

Fake inputs are six unrelated PLL getter results, other-clock-table setup,
ADT property lookup with synthetic parent/storage, and a single zero PMGR
read at0x3f100038. No physical register or device is accessed. Other clock
computation, full boot/handoff, and physical crystal measurement are outside
the fixture. `calls.py` records real producer callsites and compares original
ranges; it does not execute firmware or a hardware action.

## Integration meaning

The 24MHz metadata source for the actual 12H321 boot code is now established;
using an unrelated UART rate is unnecessary. A source-only Linux provider
could publish an explicit inherited24MHz PHY reference-clock contract for
this pinned boot chain, and the staged-power backend can consume it through
real `clk_get_rate()` (code1 in the original PHY frequency discriminator).

However, this is software metadata, not a measurement or a guarantee that
all clock parents/gates remain initialized at Linux handoff. The bootbundle
currently uses a separate Linux DT, not this AppleADT property as its live
clock provider. Native parent-clock membership/release ownership, stable
physical clock/PLL state, controller quiesce, and connector/VBUS readiness
remain unverified. No fixed-clock DT binding, active clock registration,
PHY role write, VBUS source or completed USB host mode is enabled here.

## Separate iOS9.3.5 cross-check

Official13G36 iBoot SHA2567dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd has independently resolved9ff00000 base (literal+330). Actual init9ff1b0f4 writes24MHz to9ff47604+0; ID14 getter9ff1b574 selects that field; ADT property write9ff1a8e8 consumes it. Eight groups/sixteen original runs pass with fake helpers/MMIO/ADT. Its alternate init+34 field is not that getter's source. This cross-check is not a replacement boot image or proof of physical handoff.
