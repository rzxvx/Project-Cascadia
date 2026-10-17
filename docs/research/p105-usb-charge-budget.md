# p105 USB gadget budget: missing Linux notification path

Research date: 2026-10-11. This is an offline integration design, not an
enabled charging driver or measured device result. No iPad access occurred.

## Verified Linux behavior

Local `build/linux` base is Linux v6.12
`adc218676eef25575469234709c2d87185ca223a`, with Cascadia patches applied.
The generated configuration enables `CONFIG_USB_CDC_COMPOSITE=y`,
`CONFIG_USB_DWC2_PERIPHERAL=y` and `CONFIG_USB_GADGET_VBUS_DRAW=2`.

1. The [CDC composite configuration](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/gadget/legacy/cdc2.c#L129)
   sets `USB_CONFIG_ATT_SELFPOWER` and leaves `MaxPower` at zero.
2. [Composite descriptor encoding](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/gadget/composite.c#L529)
   uses that zero value when SELFPOWER is set. The descriptor advertises zero
   USB bus current. Configuration activation similarly reports zero to
   `usb_gadget_vbus_draw()`; suspend reports 2 mA, while resume falls back
   to `CONFIG_USB_GADGET_VBUS_DRAW`, also 2 mA here. These two values serve
   different lifecycle paths and are not a useful charger-current grant.
3. [DWC2's vbus_draw callback](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/gadget.c#L4816)
   only forwards through a legacy `struct usb_phy` in `hsotg->uphy`.
   With no legacy PHY it returns `-ENOTSUPP`.
4. [DWC2 platform acquisition](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/platform.c#L237)
   requests the generic PHY first; it attempts the legacy PHY only when no
   generic PHY was found. Cascadia's
   `patches/files/drivers/phy/phy-apple-s5l-usb.c` registers generic PHYs
   with `devm_phy_create()` and `devm_of_phy_provider_register()`.

Consequently, the supported S5L generic-PHY path has no charger-budget
notification through this DWC2 callback. Increasing a gadget descriptor alone
would neither create the callback nor implement safe D1946 charging. It also
would not prove a Firebat USB port can provide the advertised amount.

## Required integration boundaries

The UDC must report a *requested/allowed USB input budget* to a board power
manager. This is different from battery charge current: supply voltage,
conversion and system load affect their relationship. The board manager also
needs independent attach/source classification, accessory power permission,
PMU fault availability, battery presence/temperature and device/host role.

The recovered Apple chain is documented in `p105-charging.md`: USBd flags
`0x80000` and `0x100000` permit/refuse the source, and source classification
changes discard cached current/type/adapter information. They are evidence
for the required lifecycle, not registers for a Linux driver to write.
The current HDQ driver's PRESENT property indicates that gauge refresh
succeeded. It does not yet provide a verified physical battery-presence bit.

A future implementation should have these interfaces:

- DWC2/UDC event input: configuration budget, disconnect, reset, suspend,
  resume and role change. Record the current attachment generation so a
  delayed enumeration grant cannot resurrect a detached source.
- Source/protection input: confirmed present source and power permission,
  fault availability/health, battery-presence evidence and thermal policy.
  Unknown status must not be converted into permission.
- Board manager output: desired input budget and separate battery charging
  ceiling. No active backend output until the D1946 initialization, voltage,
  fault shutdown and watchdog requirements are established.

The current DWC2 setup path releases its spinlock around `driver->setup()`
but remains part of interrupt/control-request handling. Charger I2C work must
be deferred to a worker rather than performed directly in that callback.
Use a lock-protected event snapshot and generation counter; process revocation
before enabling work, discard stale work after reset/detach/role change, and
cancel pending work when removing drivers. A normal deferred I2C shutdown alone
cannot guarantee immediate electrical fault protection: PMU hardware safety
and shutdown semantics still require independent evidence.

The callback design choice remains open: a board-specific notifier/supplier
relationship, a supported generic-framework extension, or an explicit legacy
USB-PHY bridge. Do not make DWC2 grab both PHY implementations or introduce
a global fallback that changes other boards merely to populate `uphy`.
Descriptor/current policy changes should be reviewed separately after that
interface exists and can be tested.

## Offline lifecycle prototype

`tools/power/usb-budget-plan.py` is a standalone design prototype. It only
reads a local JSON event array and emits plans; it contains no device calls,
USB writes, PMU addresses, SSH operations or charger commands. Its policies
are deliberately conservative, not a reconstruction of every Apple rule:

- Zero descriptor/configuration budget stays zero.
- A nonzero USB2 configuration budget needs fresh source permission and
  protection-health inputs and is limited to 0..500 mA in this prototype.
  Apple adapter classes and higher-current USB classifications are excluded.
- Disconnect, bus reset, role change, source refusal or protection failure
  invalidate the generation and erase the grant. Old asynchronous events
  cannot restore it. Recovery requires fresh inputs.
- Suspend grants no charging budget. Resume only restores a still-valid
  configuration in the same generation. Host/unknown role always denies the
  sink budget, including behind a hub or externally powered adapter.

Example local fixture:

```json
[
  {"event": "attach", "role": "device"},
  {"event": "permission", "valid": true, "epoch": 1},
  {"event": "health", "valid": true, "epoch": 1},
  {"event": "configured", "mA": 0, "epoch": 1},
  {"event": "suspend", "epoch": 1},
  {"event": "disconnect"}
]
```

```sh
python3 tools/power/usb-budget-plan.py /path/to/local-events.json
python3 tests/test-usb-budget-plan.py
```

All eight tests passed using the actual planner class. They cover the current
zero descriptor, independent permission/health requirements, suspend/resume,
reset and delayed grants, disconnect/reattach, role reports without an attached
source, host role, revocation and
malformed current/boolean inputs. Passing them does not validate a kernel
notifier, physical USB compliance, protection response timing or charging.
