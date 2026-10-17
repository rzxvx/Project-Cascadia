# Inactive P105 IRQ and provider wiring

`p105-current-disabled.dtsi` is an explicit offline candidate; the normal build
does not include it. It adds one **disabled** direct child under the existing
D1946 PMU. It supplies only the current-driver compatible, SoC GPIO interrupt
specifier, battery and DWC2 provider references. It supplies no probe permission,
calibration/retention assertion, boot argument or current request. Do not remove
those gates as part of building this candidate.

The prior [P105 ADT/native GPIO evidence](EVIDENCE.md#central-ownership--initialization)
identifies PMU vector 19/Apple flags 1 as level-low. The Linux GPIO domain uses its
two-cell translation directly: hwirq 19 and **Linux flag 8**, not Apple flag 1 and
not vector 23. The existing GPIO driver's level-low case selects mode 3. Numeric 8
is deliberate: Cascadia's current DTS build invokes dtc without a C preprocessor.
The referenced labels resolve in the actual tree as follows:

| Input | Existing node |
| --- | --- |
| Parent PMU | `/soc@30000000/i2c@3200000/pmu@3c` |
| IRQ controller | `/soc@30000000/gpio@fa00000` |
| Battery supplier | `/soc@30000000/hdq@2a00000` |
| USB controller | `/soc@30000000/usb@6100000` |

With `status="disabled"`, Linux OF platform population rejects this child before
creating its device or mapping an interrupt. No parent I2C interrupt property or
GPIO/pinmux configuration is added. If separately activated in a future qualified
experiment, IRQ mapping/type setup can touch GPIO state: `IRQF_NO_AUTOEN` alone
must not be presented as a no-I/O guarantee for an enabled node.

Run `python3 tests/test-experimental-current-dt.py --dtc /path/to/existing/dtc`
for offline validation. It compiles the real base DTS and base plus candidate,
checks the resolved phandles and exact disabled property set, and compares all 58
pre-existing nodes and their non-phandle properties. Previously unreferenced
labels may acquire phandle numbers. Both compilations produce zero diagnostics.
Five validator negative controls reject enabling the child, wrong GPIO 19→23,
wrong trigger 8→1, wrong USB supplier, or a new probe permission assertion. The
first local validation used an incorrect shortened `/soc` path; it failed and
was corrected to the actual `/soc@30000000` path before qualification.

This does not qualify physical IRQ delivery, event-register ownership or analog
charging. The private linked kernel, provider code, shared PMU transport and
source-budget candidate are existing prerequisites; this fragment does not
install them or change their default-off parameters. Full voltage/termination,
protection/retention and physical source-loss behavior remain unresolved. No
new kernel build, module load, boot or firmware transfer follows from this test.
