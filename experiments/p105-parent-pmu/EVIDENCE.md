# P105 shared parent bank0 regmap — inactive optional integration draft

This group is a real Linux-C parent-regmap transport draft and a narrow patch of the existing Corellium-derived MFD. It is **not applied to the normal tree**, enabled in normal config/DTS, uploaded, booted or used on hardware. `CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT` is a new bool fragment with default n and a builtin-MFD dependency. Its second, readonly `p105_bank0` parameter is a zero-initialized static bool: false. The branch additionally requires the existing `apple,p105ap` machine and D1946 match. It does not change experimental child allowlists or grant charging/ADC permission.

## Source-defined bank0, rather than merely an 8-bit wire address

Pinned official p10512H321 KC SHA256 `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`. Actual D1946 vptr `80ed3208` slots +378/read=`80ec8741`, +37c/write=`80ec85e5`. Both complete wrappers call **`80ec86d8`**, whose requested bank is the full logical register argument shifted right eight; descriptor `80ed44f0` is `01 03 57 1e 01`. Selector is write `1e,[bank]`; no selector readback meaning is inferred. The independent13G36 descriptor/full-wrapper work matches this layout, with separately qualified offsets.

The **logical** addresses passed by actual matched firmware owners are below256, hence requested bank0 through that real helper:

| Existing Linux consumer/address | Actual12H owner/argument evidence |
| --- | --- |
| Parent GPIO61..72; Bluetooth63, Wi-Fi64/6c | `80ece9d4..80ece9e8`, `80ecea66..` construct61+n after n<=17 check, then pass the full result through PMU read/write slots. |
| Touch regulator22 (mask06),23 (mask40) | `_setLDO` `80ece79c` uses12-byte table `80ed2ec8`; entries12/15 have enable hword+8=22, entry19=23; `80ece8dc..80ece8e8` forwards that hword as logical R1. This is distinct from the older iBEC table mentioned in existing DTS comments. Public original LDO emulator also rejects a full R1>=256 without truncating it. |
| WLED d0/d1; enable d2 | Native getter `80ecebf8` passes d0/d1 at `80ecec06/1a`; setter `80ecec40` at `80ecec52/6e`; shutdown d2 at `80ecec96`. |
| Poweroff events01..09, masks12..1a | Actual PMU +388→`80ecc461` reads nine bytes starting1; shutdown `80ececc6..d4` passes logical12 and count9. |
| Poweroff alarm5b,a1,af,GPIO61..72,standby1b | `80ecd39e`→5b; `80ecece4`→a1; `80eced40/72`→af; `80eced88/b0`→61,count18; `80ecedda/ec`→1b. |

`check-original-parent.py` (Unicorn optional, external pinned image) executes the **full original12H read/write wrappers** with initial fake bank2 for84 normalized cases, including events9, masks9 and GPIO18 bursts. Every case emits selector `[1e,00]` before data and uses fake bank0. The84-case golden JSON has no firmware bytes. It is independently compared against actual C callback packets/outputs by the portable host tests. This proves software address routing; fake data does not prove physical register contents, retention or calibration.

Current `dts/p105ap.dts` has five PMU platform children: touch_ana22, touch_ldo23, wlan_reg_on64, wlan_lpo6c, wled d0/d2. Parent GPIO callbacks, regulator config and WLED context all point to **the same `pmu->regmap`**. There is no PMU RTC child/driver in this current overlay/build; alarm5b is used by parent poweroff. ARM Cortex-A9 performance-PMU timer code is unrelated to D1946.

## Transport and lock order

New `apple-pmu-d1946-bank0.c/.h` supplies real raw regmap read/write callbacks. Each hardware operation selects0 with a checked I2C write, then executes existing one-byte-prefix write or combined-prefix/read. There is **no bank cache**. Invalid/short transfers latch fault and first error; failed reads clear their bounded output; later normal operations deny. Selector1e cannot be addressed by a child, including a burst spanning1e (e.g.1b+4). No OTP/init table or CV/termination register is replayed.

The single parent config uses sleepable custom lock/unlock: **map mutex→I2C adapter segment lock→A5 driver's mutex/spinlock**. Both selector and data use `__i2c_transfer` under that adapter lock. A Linux `regmap_update_bits_base` call holds the lock over its whole native read/modify/write, so raw I2C traffic cannot enter between those callbacks. Distinct Regmap calls remain separate: raw traffic can run between WLED writes, between update_bits and a later readback, or between ADC-conversion steps. Reselecting0 on every next callback handles a preceding raw bank change; it does not provide a multistep ALL-ADC conversion reservation, atomic brightness update or current grant.

Actual A5 adapter initializes no retries field, so a fresh adapter has0. Init and each transfer reject nonzero retries without modifying global adapter settings. Linux6.12 `drivers/i2c/i2c-dev.c:484..488` assigns `adapter->retries` for `I2C_RETRIES` without the adapter lock, and `__i2c_transfer` reads it during EAGAIN retry decisions. Adapter locking/prechecks alone therefore did not close that race. The experiment now requires **`I2C_CHARDEV = n`** and its initializer also rejects either builtin or modular i2c-dev in forced builds before touching any state. This excludes the known in-tree ioctl entry point; normal Cascadia configuration is unchanged. The requirement that adapter algorithm/config/context remain stable still applies to other kernel users; this is not universal protection against arbitrary external modules or analog safety qualification.

## Poweroff bypass and lifetime

Existing late poweroff directly uses SMBus outside regmap. The opt-in branch replaces this with a dedicated non-sleeping sequence, after a complete checked version of existing poweroff preparation. Preparation preserves existing register values/ordering, checks every return, then seals normal bus admission and drains the adapter. Failure prevents arming. It does not add charger shutdown semantics or new wake masks.

Late helper requires IRQs disabled, system_state>SYSTEM_RUNNING, master_xfer_atomic present and retries0; missing prerequisites withdraw the one-shot authorization. It uses an **exclusive adapter trylock**, never the map mutex. Failure aborts without I/O/retry/SMBus fallback. Under the lock: select0→combinedread1b→select0→write1b|1. Exact transfer counts and SEALED/fault state are checked before and after every step. STOPPED blocks subsequent I/O. A stop observed after an acknowledged final write cannot undo it; there is no physical instant-cutoff claim. Acknowledged standby still is not evidence that the device powered off.

A reconstructed earlier guardless version of our own GPL source is retained privately with SHA `c7ebe7cc893b962f9433f4a96dddd5aedba05813cbc56bf66a00b00f057d8761`. Stop-at1/2/3/4 controls all returned0 and executed all4 transfers, fake1b=a5. Current guards returnESHUTDOWN and stop after1/2/3/4 respectively; fake1b staysa4 for1..3, remainsa5 at4 (write already executed). Exact eight counterexamples/results are in the exported `stop-negative-controls.json`, reproduced against entire old/new C with existing explicit fakes.

Opt-in probe uses managed OF population; partial creation failure explicitly depopulates. Existing default branch is preserved. Bus stop action is registered before sys-off/GPIO/child resources; managed unwind first removes children, GPIO and sys-off handlers, then stops/drains bus, then frees regmap, context and parent allocation. Blocking/atomic sys-off unregister follows real notifier drain APIs. Stop first forbids new traffic, drains adapter, then waits for a map unlock without nesting reverse locks. Tests exercise an actual paused in-flight selector and concurrent stop. Caller framework references/lifetime still matter. Managed child destruction must quiesce the child's actual workers/users before freeing their storage. `devm_stop` drains are not lifetime fences for escaped regmap handles or external workers that retain private pointers; post-STOP rejection cannot make an already freed context safe. There is no universal fence for foreign users.

## Validation and reproducible review

Portable `tests/test-experimental-parent-pmu.py`: **10 groups PASS0skip**, GCC Wall/Wextra/Werror + ASan/UBSan. Entire new bus C executes; actual unchanged GPIO, regulator and WLED callback bodies are extracted from normal source. Actual Linux6.12 `regmap_read`, `regmap_write`, `_regmap_update_bits` and `regmap_update_bits_base` execute from a GPL fixture; `_regmap_read/write` behind them are explicit fake dispatch to the real bus. Fixture SHA and normal child/DTS hashes are asserted through `source-provenance.json`; when normal `build/linux/drivers/base/regmap/regmap.c` exists, all four current function bodies are compared byte-for-byte against the fixture. This comparison passed on this host. Without a Linux source checkout, the portable C tests still use the pinned fixture, not an unverified regenerated model.

Coverage includes full84 native packet comparisons; normal GPIO/22/23/64/6c/WLED operations; private selector/burst boundaries; every selector/data negative/short/partial fault; retries rejection; actual checked prepare; every late atomic transfer failure/stop boundary; no hook, wrong IRQ/system phase and occupied adapter; no retry/fallback; exact default/opt-in parent poweroff routing; actual pthread raw-I2C exclusion over one native RMW; concurrent stop with adapter/map-unlock drain. Positive ownership/IRQ/PMU effects are explicitly fakes, not supplied hardware permissions.

Actual private Linux6.12 ARMv7 object commands (no normal build/config changed):

```
make -C /linux M=/stage ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1 KCFLAGS=-DCONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT=1 apple-pmu-d1946-bank0.o apple-pmu-i2c.o
make -C /linux M=/stage ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1 apple-pmu-i2c.o
```

All three objects PASS0warnings (`arm-enabled.log`, `arm-default-off.log`). Relocatable linking resolves every transport helper in the enabled parent. `nm -u` on the default-off parent contains no transport helper references. The standalone private header is forced before the old private-kernel header solely to represent the matching patched parent struct; normal headers/tree are untouched. These are object/link checks, not a runnable module, boot or hardware validation.

Review application, **isolated source only**: copy helperC/H to matching drivers/mfd, apply `parent-regmap.patch` to the Cascadia overlay MFD/header, append Kconfig/Makefile fragments in that isolated Linux source. The full candidate MFD and header are supplied for exact review/apply comparison. None of these steps is automatically run by a normal Cascadia build. Analogue CV/termination/protection, ADC cancellation/ownership, physical charging and poweroff behavior remain unvalidated.

## Current-controller attachment — 14 October 2026

The current frontend now requires an exported, probe-only accessor from this
actual parent implementation. Driver/bus/D1946/P105 identity checks precede any
parent `drvdata` interpretation. A managed `DL_FLAG_AUTOREMOVE_CONSUMER` link
protects the borrowed regmap through child teardown; it is not manually released
on probe failure, and the child must never free the parent's regmap. A probing
parent defers; no parent mutex is taken from the child probe. The accessor checks
matching bank-zero client identity, running/no-fault state and registered regmap
without any I2C operation. These checks are a snapshot; callback admission and
worker teardown remain required.

Actual Linux 6.12 `drivers/base/core.c` and `drivers/base/dd.c` were reviewed:
consumer-probe/active links hold off supplier teardown, including a link added
while unbind has started. The post-link driver/state checks reject that late
unbind path before the cast. Normal poweroff runs `device_shutdown()` before
power-off-prepare handlers, so the frontend's existing shutdown drain precedes
the parent's seal operation. This ordering is not proof of physical cutoff.

The targeted [current binding fixture](../../tests/test-experimental-parent-current-binding.py)
passes five positive groups and an expected historical negative control under
ASan/UBSan. It uses driver-core fakes and only the real probe prefix before the
first allocation; actual full-kernel lifetime behavior is source-reviewed, not
hardware-tested. The analog and event-ownership boundaries above remain. The later exclusion
below closes the known adapter-retry ioctl entry point. No normal build enables
this interface.


## Exclude unlocked adapter retry changes — 14 October 2026

The dependency uses `I2C_CHARDEV = n`, not `!I2C_CHARDEV`: negating `m` still
produces `m`, which Linux Kconfig can promote for this boolean option. The
initializer independently rejects both `CONFIG_I2C_CHARDEV` and
`CONFIG_I2C_CHARDEV_MODULE` before pointer access, lock initialization or state
changes. This also covers a manually forced compilation. Existing per-transfer
checks remain; no global adapter value is changed and no core validation is
bypassed by calling the hardware algorithm directly.

The new `test-experimental-parent-no-i2cdev.py` compiles the actual initializer
for n/y/m under ASan/UBSan and Werror. The n path initializes; y/m return
`-EOPNOTSUPP` without mutations. Two SHA-pinned historical-function controls fail
that rejection contract with expected exit23. With `--conf` pointing to an
existing Linux `scripts/kconfig/conf`, six additional checks run the actual
fragment: n accepted, y/m rejected; the former dependency accepts y/m and the
incorrect negated dependency accepts m. No Kconfig parser is modeled in Python.
Without that executable the script explicitly reports those checks skipped.
The ten existing parent transport regressions also pass.

Source review of the actual A5 adapter and generic I2C core found no additional
retry setter there; other adapter drivers' initialization does not modify this
A5 instance. This is a bounded in-tree finding, not a guarantee against unknown
kernel clients. Analog voltage/termination/protection, event ownership and real
source-loss cutoff remain unqualified. No device operation follows from these
checks.

The new private profile sets `I2C_CHARDEV=n`; full ARM `vmlinux` and both charging
modules pass their joint MODPOST/link in 31.568 seconds under 2 CPUs / 1 GiB,
with zero warnings in this incremental build. Configuration and linked symbols
confirm no i2c-dev implementation, while the parent export/current dependency
remain present. Compiled parent/current/gadget defaults are still zero. The
normal build and the live kernel retain their original configuration; no live
module removal or boot occurred. This private link stand has only the default
512-byte empty-source initramfs and is not a boot package.
