# Isolated CDC USB2 declared-power candidate

Stock Linux6.12 cdc2 has SELFPOWER and MaxPower0. Actual libcomposite
encode_bMaxPower therefore encodes0, set_config requests0 and the actual owned
source observer reports0 even after successful configuration/status completion.
This candidate adds immutable unsigned module parameter usb2_maxpower with
choices0,100,500mA and default0. No normal overlay, Kconfig, DT, build script,
boot arguments, module loading or device operation is changed by this group.

A custom module_init validates before usb_composite_probe can register/bind.
cdc_bind validates again before function-instance acquisition. Positive choices
require UDC max_speed FULL or HIGH; non-USB2/unknown-capability controllers reject
with EOPNOTSUPP before acquiring functions or modifying MaxPower. Default0 retains
the generic original capability path. This avoids USB3's8mA unit rounding100→96
and500→496, and makes no SuperSpeed source-budget claim. VID/PID, ECM/ACM roles,
SELFPOWER configuration bit, function implementations and max-speed default are
otherwise unchanged. Bind assigns the selected declared mA to MaxPower.

USB-IF permits dual-powered descriptors to retain SELFPOWER while declaring
worst-case bus demand. GET_STATUS must report bus-powered above100mA. Switching
to high-power bus operation after losing self-power requires re-enumeration.
See the [primary USB-IF clarification](https://compliance.usb.org/index.asp?UpdateFile=Electrical),
'Peripherals may support both bus-power and self-power modes' (July2007),
referencing USB2 section9.6.3. This does not certify actual electrical compliance.

Actual Linux encode_bMaxPower returns declared0/2,100/2,500/2 at FULL/HIGH:
byte0,50,250. config_buf emits bmAttributes C0 and those byte values. Actual
set_config sets/clears the selfpowered indication at100mA. The actual DWC2
set_selfpowered function updates its software flag under controller.lock.
These software effects and primary power-mode rule are separate from actual
current/voltage/power measurement or completed charger safety validation.

The descriptor declares configuration demand; it does not grant power before
SET_CONFIGURATION and successful EP0 status completion. No unsupported hardware
vbus_draw callback is turned into a fake physical permit. The retained source
fixture explicitly returns EOPNOTSUPP from vbus_draw while the actually completed
configuration can still establish a USB2 protocol allowance. Observer still
rejects pending/delayed/failed completion, superspeed, suspend/disconnect/role
invalidation, stale generations and nonmatching supplier/driver lifetime.
The existing current/gauge/analog-retention assertions remain unchanged. No
analog CV/termination/fault programming or source-current calibration is supplied.

## Actual code checks

Four ASan/UBSan fixture groups compile candidate CDC init/bind/do_config/unbind,
actual Linux encode_bMaxPower/config_buf/set_config, actual DWC2 selfpowered
software callback, and actual observer/source-reader/gauge adapter code from the
existing source-provider test. Function instances, ECM/ACM descriptors/set_alt,
UDC devices and status transfers are explicit mocks; this is not complete USB
packet/hardware emulation or evidence of physical enumeration/power delivery.
The prior full DWC2 hook apply/TU checks establish status-cookie hook placement;
these tests exercise the actual observer completion function with synthetic
success/error status, not a live controller completion interrupt.

Cases cover0/100/500 at FULL/HIGH, encoded descriptor byte/C0 attribute, prior
stock defaultzero, invalid1/99/101/499/501/512/1000/UINT_MAX before composite probe
and function acquisition, non-USB2 opt-in rejection, unchanged SuperSpeed-default0,
instance failure unwind, delayed setup/no status, status failure, function setup
failure, ordinary successful completion, suspend invalidation, resume draw without
fresh configuration, superspeed denial. A second executable replaces only the
candidate CDC bind body with the exact original bind body; original config keeps
MaxPower0 and produces completed budget0. It does not pretend the original driver
had the new parameter. Existing source budget is derived from actual config logic,
not assigned directly by a fake permission producer.

Actual complete cdc2.c ARMv7 object compiles against real Linux6.12 W=1. No module
is linked/loaded. Source hashes and primary URLs are recorded in JSON; the normal
CDC/composite source is only read and remains unchanged. Strict git apply to a
disposable single-file source copy verifies patch equality/whitespace; no patch
is applied to the normal build. The positive parameter remains explicitly opt-in.

## Public reproduction

Run `python3 tests/test-experimental-gadget-maxpower.py` from the repository.
It imports the existing source-provider test by an explicit path; optionally
`CASCADIA_TEST_LINUX` selects a reviewed Linux6.12 tree. Missing prerequisites
explicitly skip; wrong existing CDC/composite hashes fail. The patch is verified
in a disposable single-file copy and is deliberately absent from normal patch
application/build wiring. No firmware image, device, privileges or network is
required.
