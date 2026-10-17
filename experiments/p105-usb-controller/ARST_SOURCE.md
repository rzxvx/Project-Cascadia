# P105 original USB20 ARST controller reset action

Only offline original-code execution with synthetic providers/MMIO; no device
access. Kernelcache12H321 SHA256 db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c;
P105ADT SHA2565c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72.

## Typed producer and actual instance dispatch

Actual `function-usb20_reset` property bytes are
`00000010 54535241 22000000`. Its native little-endian numeric argument is
34decimal = **0x22**, not0x34. FourCC is ARST. No argument is a register offset.

Original typed factory0x804b01ac recognizes ARST and constructs the instance
vtable via PIC0x804b0222/224+8 →0x804d46e8. Actual `init` slot+38 points to
0x804b59e1. After inherited init/metacast, it obtains original OSData bytes
and loads argument at bytes+8 unchanged into instance+18; platform in+14.

Original generic call0x804ab5f4 normalizes any nonzero value to1 and dispatches
instance+40→0x804b59c9. Normalized1 invokes retained platform+410 using the
unchanged typed argument0x22. Zero skips provider. This interface is one reset
pulse request, **not** distinct assert/deassert values. It discards provider
return and reports0; the candidate Linux component must not copy that loss.

Actual S5L Performance instance constructor PIC0x80b8bc80/82(r1)+8 stores
vptr0x80b90938. Its+410→0x80b8cf31 forwards the index through CommandGate+70,
with action cached in instance+55c. Startup stores actual+46c reset action
0x80b8d155 into+55c. A bare unrelated vtable is not used as class proof.

Original reset action0x80b8d154 checks index<=0x57, computes `0x1000+4*index`,
reads retained mapped PMGR+550 through actual+490 accessor0x80b8df5d, writes
`saved|BIT(31)` through+494 accessor0x80b8df65, calls IODelay(1), then restores
`saved`. IODelay importstub0x80b8f1fc resolves pointer0x80b90000 to actual
symbol `_IODelay` (0x802e46cc), checked in the oracle.

Thus P105 USB20 ARST index0x22 addresses **PMGR+0x1088**, the EHCI clock gate,
with a one-microsecond controller-reset pulse preserving all saved bits.
This is neither PMGR+0x10d0 (the wrong0x34 decode), nor PHY+8 reset, nor any
PMU/Tristar/VBUS rail or electrical supply operation.

## Regression

`emulate-controller-reset12.py` verifies full firmware and ADT hashes,
constructor PIC/vptrs, exact ADT bytes and linked IODelay symbol. It executes
unmodified init, normalized call, actual Performance forwarding/action and
real read/write accessor instructions. Mocked providers: inherited init,
metacast, OSData byte pointer, CommandGate trampoline, delay. The trampoline
passes original callback/index to the actual action with fake Perf owner;
it is not a hand-reimplementation of reset arithmetic.

Eight groups PASS: argument0; argument1; noncanonical2 andUINT_MAX; maximum
index87; out-of-range88; originally asserted BIT31; inherited-init failure.
Indices/arguments are independently asserted. Valid pulses preserve all32
saved bits, including existing resetbit; index88 causes no register/delay,
zero argument skips provider, failed base init skips later callbacks.
Per experiment instruction cap3000; fake RAM is the only register model.

## Source-only integration limits

A narrowly scoped regmap controller reset primitive can use this exact
PMGR+1088 saved-bit pulse. It must check full EHCI-allports quiesce (including
WLANHSIC), DWC2/UDC quiesce before mux, exclusive external-role lease, owned
and acknowledged gate89, matching P105 binding, and connector epoch. It must
remain denied by default, reject already asserted reset rather than implying
known controller state, and latch read/write/delay/epoch errors without
claiming rollback or full clientstate5. Quiescing Wi-Fi is a real remaining
integration task; active WLAN cannot be treated as harmless during a shared
EHCI controller reset. Native VBUS and adapter/authentication remain separate.

## Retained provider loss regression

A missing epoch or required membership callback after an owned transition now
uses the existing retained-fault path. Restoring the callback cannot silently
resume the old physical disposition. Cold, untouched contexts remain retryable.
`python3 tests/test-experimental-usb-provider-loss.py` compiles the actual three
backends under UBSan and executes five groups, including earlier admission
blocks as negative controls and adopted active PHY state without own writes.
No physical hardware or new default binding is used.
