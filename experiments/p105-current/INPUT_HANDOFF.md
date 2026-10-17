# Input handoff validation and transaction order

The experimental worker previously reported an applied input limit after checking
its ISET register without checking whether the inherited hardware selected that
limit. It now requires programmed-input selection (register 0x40 bit 0x40 set)
and bus power not suspended (0x1b bit 0x40 clear) for every positive input plan,
including the unchanged-plan fast path. These are distinct from battery suspend
at 0x1b/0x04 and 0x1c/0x80. A missing mode or failed read uses the existing fault,
inhibit and zero-input path; it does not silently repair inherited control bits.
Zero-input requests never depend on a positive handoff check.

The positive input transaction now writes and checks the requested ISET code
before releasing either battery suspend control. Previously the release happened
while the previous code was still programmed. Zero retains suspend-before-zero
ordering. Final register states still match all 16 existing original-code oracle
cases; transaction order intentionally differs. This is a software ordering fix,
not a measurement of a transient electrical overdraw on this device.

The existing source lease, epoch checks, analog-retention requirement, thermal
and voltage bounds, IRQ ownership, default-disabled build and opt-ins are
unchanged. Read helpers use the existing checked bank-zero transport, which may
write a bank selector; they are not a direct raw I2C interface. Transport failure
can still make physical inhibit unavailable. This change neither initializes
analog protection nor proves charging or safe shutdown.

## Evidence and verification

The alternate-limit operation is corroborated by the original 12H321 iBSS RMW
helper at 0x340033d8 and caller at 0x3400c8e6. The separate bus-suspend mask occurs
in the original 13G36 input setter at 0x9ff0cff4. These bounded instruction
observations and the associated register semantics establish which controls
must be distinguished; they do not establish their live state after Linux boot.
No Apple code, firmware bytes, device identifiers or private images are included.

The dedicated C fixture checks every mode-byte combination, each transport
failure, and positive/zero transaction ordering. Its retained previous-function
counterexample demonstrates release before the new code is programmed. Actual
worker fixtures cover bad initial modes, changed modes on the fast path, zero
requests, read failures and source revocation during handoff reads. Fake register
and supplier results do not emulate physical charging. ARM object compilation
checks the kernel-facing implementation separately; it is not a full boot test.
