# Earlier boot initialization and selective reset leaves (offline original evidence)

This follows the frozen boot-CDMA entry group into concrete earlier callee bodies.
No real MMIO, device, boot, IRQ, DMA or NAND operation was performed. All native
code executes in strict Unicorn Fake RAM with pinned original 12H321 iBEC/iBSS.
SHA256s are the same original inputs as `ppn-boot-cdma/EVIDENCE.md` and checked in
both emulators. Private disassembly remains private; no firmware bytes or native
microcode/programming table is added to a Linux driver.

`emulate-earlier-leaves.py`: **10 exact instruction assertions /21 original leaf
controls PASS**. `emulate-reset-matrix.py`: **8 exact instruction assertions /546
original reset-leaf controls PASS**, all input IDs0..90 and initial register
words0/12345678/FFFFFFFF for both images. Unexpected instruction ranges, MMIO,
RAM writes or exceptions fail; each leaf has finite stepping/instruction bounds.
Clock-delay/critical-exit APIs are explicit unexecuted boundaries. Independent
address/value/state oracles compare actual original register accesses.

## Closed earlier bodies

| Leaf | iBEC12 offset | iBSS12 offset | Concrete qualified footprint |
| --- | --- | --- | --- |
| AIC initialization | `1714` | `a60` | AIC CONFIG0010 read-modify-write; then six MASK_SET words4100..4114 allFFFFFFFF; explicit unexecuted critical-exit |
| Native mask-all subleaf | `174c` | `a98` | Exactly six global AIC MASK_SET writes; no pending-event read |
| Configuration initializer | `1ea34` | `8528` | Original clock/configuration writes in PMGR1240..12ac, then **tail-calls selective reset with ID1** |
| Bus/cache-filter configuration | `1e978` | separate larger scope | Native accesses3fb00000 and3e000c00/c04; no DMA/IOP accesses in this iBEC leaf |
| Earlier clock stub | separate calculator | `8780` | Actual two-instruction return0, not adjacent full profile routine8784 |

AIC init's exact CONFIG sequence sets10000, clears770, sets770, then sets1,
preserving other raw bits. This is a source register sequence; unexplained fields
are not labeled a hardware reset or barrier. Mask-all does not drain the classic
or AP per-CPU EVENT queues, poll CDMA, halt IOP, retarget IRQs or wait callback
lifetimes. Its critical-exit body is unexecuted, so this fixture does not qualify
real CPU interrupt-enable or concurrency behavior. Fake critical-exit errors are
ignored by the original caller; that is not a real error ABI guarantee.

The configuration initializer is **not** an independent return1 success leaf:
its final movR0=1 selects the separate reset helper. That original tail-call now
executes in the same fixture. Reset register100c is included in the oracle, and
the returnedR0 is the raw final reset word. This closes an actual source edge
rather than pretending a callback marker proves physical completion.

## Native selective reset semantics

| Helper detail | iBEC12 | iBSS12 |
| --- | --- | --- |
| Selective reset helper | `1eb9c` | `8690` |
| Delay-call boundary | `20354` | `99a0` |
| Register base | `3f101008 +4*WMR-ID` | same |

The complete input-ID matrix confirms accepted IDs are exactly
**1,6,29,44,47,49,51,53** in0..90; other inputs produce no register accesses.
For a normal accepted ID, the source reads its word, sets bit31, calls the delay
boundary with raw64-bit argument1, reads again and **clears bit31**, preserving
other observed bits. The real timer/delay hardware is not executed; no specific
physical wait duration or reset completion is qualified by this fixture.

ID6 additionally pulses registers1024/1028/102c/1044 around its own1020 register.
Those extra words are asserted, delayed once, then cleared in original order.
It is not a single-controller reset and cannot be invoked under only a local
FMI reservation. All tests preserve other word bits. Initial bit31=1 has no
proved new rising pulse; it is nevertheless cleared by this original helper.
This differs from the native AP ARST leaf restoring its **original** word.

CDMA WMR gateID28/address1078 is **not accepted** by this selective reset helper.
Accepted WMR29 pulses bit31 at107c, which is the IOP low-nibble gate address;
that co-location alone does not establish IOP-core reset or DMA fencing.
AP's actual IOP function-device_reset uses ARST argument22 and its distinct
native leaf at PMGR+1000+4*22/address1058. Reset bit31 and clock/power low-nibble
fields must not be assigned identical resource roles merely from address overlap.
Raw ADT gatepacked w3 includes RGBOUT's second gatebyte22; it is not an IOP clock
ID or proof against the separate ARST namespace. No raw gatebyte3 names100c.
These ADT caveats are from the peer's independent original-board metadata check,
not a live controller observation.

## Actual caller qualification and remaining edges

The prior boot group qualifies main→early-init→configuration-init/CDMA-init
relationships, with unrelated earlier bodies explicitly unexecuted. This group
executes the named earlier leaves, not the entire platform graph.
`inspect-reset-callers.py` also inventories raw direct-branch candidates; raw
halfword searches are not themselves execution proofs.

Source-qualified initializer tail-calls use ID1. The iBEC wrapper at219a0 takes
bus0→47 or bus1→49 and branches to the selective reset helper; these register
addresses10c4/10cc agree with the respective FMI0/FMI1 gate addresses, not CDMA.
Its whole caller/boot execution graph remains unqualified. iBSS has a candidate
caller atf7e using ID6 in another routine. Supporting ID29 in the helper does
**not** prove that an IOP reset is invoked on the Cascadia path; no direct reset
caller in this inventory selects29. Indirect/earlier/cold-boot/SecureROM behavior
and real inherited warm/cold state remain unknown.

The iBSS adjacent full profile body8784 and bulk clock helper8e20 exist but are
not the actual early-init callee8780. Likewise, iBEC's clock calculator1ec88 is a
separate body from bulk profile reprogramming1eed0. An adjacent reset/profile
routine must not be silently treated as an executed initialization path.

No reset leaf has a reset-ack/queue-idle/IRQ-drained/no-future-memory-access
receipt. Neither global interrupt masks, generic AP-handler synchronization,
clock-configuration writes nor unqualified reset-bit pulses close physical DMA
ownership. The existing C cancel/completion helpers remain unchanged, default-
off, and unable to release buffers without their external fence prerequisite.

Independent root repeat matches the frozen18 assertions/21 earlier leaves/546 selective reset runs exactly. Logs `logs/night/boot-earlier-root-repeat-v1.log` and `logs/night/boot-reset-matrix-root-repeat-v1.log`. No physical reset was invoked.
