# J171a / iPad 8 radio bring-up (initial hardware test completed)

> **Historical engineering log:** use [the A12 quickstart](../QUICKSTART.md) and [firmware reference](../FIRMWARE.md) for current commands/paths. Dated results below refer to laboratory images, not a clean build of the new separated workflow.


## Desktop package-manager/time fix — 2026-11-06

The desktop session retained its pre-sudo supplementary groups. At the
operator's request, password-authenticated sudo was enabled for the existing
desktop account using a live per-user sudoers rule; visudo and a test with
all supplementary groups cleared passed. No password is stored in source
or these notes. Corrected the local hostname mapping in /etc/hosts.

APT's later `sqv` “Not live until” signature errors were caused by the iPad
clock reading April 13, 2026 while repositories were signed in September /
November. Set the guest to the independently checked host/current UTC time.
APT update then fetched 487 kB successfully, exit 0, with signature and
validity checks **unchanged**. DNS and repository signing keys were not
the cause; no trusted=yes/allow-unauthenticated/check-valid-until bypass.

Installed only systemd-timesyncd (91.5 kB download / 261 kB installed),
enabled it, and verified `CanNTP=yes`, `NTP=yes`, `NTPSynchronized=yes`.
The rootfs builder now explicitly includes/enables time synchronization
for future boots/images. No device reboot or package upgrade required.
Logs: `apt-update-diagnostic.log`, `apt-update-fixed.log`, and
`time-sync-install.log` under ignored `build/ipad8-radio/bt-quirks/`.

## Desktop networking authorization — 2026-11-06

The GUI's “Not authorized to control networking” was a rootfs packaging
omission, not failed Wi-Fi firmware. The rootfs used --no-install-recommends
without explicitly installing polkitd; no authority or authentication agent
was running, and NetworkManager permissions were unknown. Installed polkitd
and lxpolkit live from six small Debian packages (414 kB), verified against
SHA-256 hashes in the existing Debian indexes. Added both to the rootfs
builder for future images.

`rootfs/debian-overlay/etc/polkit-1/rules.d/49-cascadia-network.rules` allows
only active, local members of netdev to control networking, Wi-Fi scan/
toggle, and their own/system connection profiles. It does not grant blanket
polkit approval or sudo. The actual nm-applet PID passes pkcheck for network
control and both connection-edit actions. A `runuser` command launched over
root SSH is not the local desktop session and correctly does not receive
the same unconditional local-session authorization. No NetworkManager
restart, desktop logout or device reboot was required.

The running deployment is named `ipad8-debian`, pretty hostname “Cascadia
on iPad 8”, MOTD identifies J171a, and the account display name is “iPad 8
User”. Shared overlay labels now say Cascadia/A12 rather than identifying
every A12 device as an XR. The active Unix login remains `xr`, and the
mounted NFS export path retains its historical name; these were deliberately
not renamed under an active session/root mount. Future per-board account/
root generation still needs a separate cleanup; do not globally replace XR
board identifiers or claim those technical names have all been changed.

## Bluetooth pairing confirmed — 2026-11-06

On the patch-0026 build, paired with the user's iPhone using the BlueZ
KeyboardDisplay agent and matching numeric comparison confirmed by the
user. BlueZ reported **Bonded: yes**, **ServicesResolved: yes**,
**Paired: yes**, and **Pairing successful**. The BR/EDR connection then
disconnected; this does not undo the recorded bond. No persistent profile
connection, audio, tethering or sustained traffic was tested. No automatic
trust setting was added. Do not publish addresses, passkeys or link keys.
The interactive agent was closed after verification; DCSD capture remains
separate. Earlier text saying pairing is untested is superseded by this
result, not by a claim that all Bluetooth profiles are supported.

## Bluetooth compatibility fixes — 2026-11-06 (latest test)

Patch `linux-0026-bcm-dt-firmware-quirks.patch` adds two opt-in properties to
`hci_bcm` setup: `brcm,broken-read-transmit-power` selects the existing HCI
quirk that skips unsupported 0x204b, and `brcm,use-bdaddr-property` selects
the HCI core's platform-address path. Only J171a DTS sets these properties.
XR retains its existing extended-scan quirk and has neither new property.
The core still rejects absent/all-zero platform addresses. Five host tests
pass, including execution of the actual helper under UBSan/bounds and DT
checks for both boards. Old patch 0024 and new patch 0026 reverse-check
successfully against the staged final driver, preserving incremental build
compatibility.

The final incremental kernel was rebuilt after the final source edit,
using one job, one linker thread and a 2 GiB process limit. Kernel config,
working-touch no-trace SIO source, initramfs, m1n1, arguments, calibration and
Apple boot inputs are unchanged. Exactly two boolean DT properties differ
from the preceding private radio DTB. Verified test IMG4 SHA-256:
`2995cfdeb193770361771ab423b0d7314a43b11c1a755009f95cda6034eb7b70`.

Captured hardware boot reached Debian systemd on NFS. After externally
loading MagnoliaA Murata HCD build 1334:

* Management index exists and `btmgmt power on` succeeds.
* Controller address privately verified to equal this iPad's SysCfg BMac.
* Discovery starts successfully; the 15-second management discovery found
  **four devices**, including both LE Random and BR/EDR reports.
* The old 0x204b failure still appears in early ROM/no-HCD boot logs because
  `bcm_setup()` returns early when firmware is missing. It does not recur
  after external HCD loading, when the quirk helper runs. Do not mistake
  that early log for failure of the patched controller.
* Some advertising reports are rejected with unknown packet types 0x23/24.
  This remains a compatibility issue to investigate; pairing/profile
  traffic and extended-report correctness are NOT yet validated.
* Touch driver initialized without the former SIO timeout; the preceding
  no-trace boot had user-confirmed touch, and this test changed no touch
  driver/DT properties. New interactive touch confirmation is still useful.

Wi-Fi firmware reload produced `wlp1s0`. An additional scan attempted after
Bluetooth discovery did not complete and the specifically identified `iw`
process was terminated; status 143, no results. Its cause is not isolated.
Do not replace the preceding successful 12-BSS scan with a claim that all
concurrent Wi-Fi/Bluetooth behavior is validated.

Logs, manifest, HCI summary and source backup are in ignored
`build/ipad8-radio/bt-quirks/`. Fresh DCSD capture remains active. Prior
working-touch and failed-radio bootsets are preserved. No commit/push.

## Bluetooth root-cause trace — 2026-11-06

A bounded 12-second btmon capture of one serial-child rebind is retained in
`build/ipad8-radio/no-sio-trace/bt-init.txt` (private, includes addresses).
It confirms two missing compatibility behaviors, not a UART baud/pin fault:

1. Patched MagnoliaA build 1334 advertises LE Read Transmit Power in
   Supported Commands octet 38 bit 7, but replies to `0x204b` with **Unknown
   HCI Command (0x01)**. The one-byte failure response is not random UART
   corruption. `hci_event.c` expects the normal three-byte return structure,
   reports length 1 < 3 and converts this to an initialization failure.
   `hci_sync.c:hci_le_read_tx_power_sync()` aborts LE init stage 3, explaining
   zero management indices. Linux already has
   `HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER`; btbcm applies it to affected Macs
   using a DMI table, which cannot match this device-tree iPad. The XR
   `brcm,broken-ext-scan` property handles another command (`0x2041`), not
   this prerequisite. J171 needs a separately selected transmit-power quirk;
   do not change global HCI error handling or pretend the command succeeded.

2. The live DT `local-bd-address` is six bytes and privately verified to
   match this iPad's SysCfg BMac in Linux byte order. Yet captured controller
   addresses do not match BMac. btbcm's default-address blacklist does not
   recognize this BCM4355C1 factory placeholder. `hci_bcm:bcm_setup()` sets
   `HCI_QUIRK_USE_BDADDR_PROPERTY` only when `INVALID_BDADDR` was set, so the
   correct injected property is ignored. A J171-only explicit opt-in to use
   the BDADDR property is required; keep its zero/multicast checks and do
   not log the actual address. XR behavior must remain unchanged.

These are traced fixes to implement and hardware-test, not yet claimed
working. UART power/mux/baud and HCD vendor selection are supported by
successful bidirectional command traffic and confirmed patch build 1334.

## No-SIO-trace test — 2026-11-06 (latest)

Removed only the five `sio_spi_trace()` call sites and their diagnostic
macro from `apple-sio-core.c`. Normalized functional C tokens, protocol,
100 ms timeout and kernel config are unchanged. Incremental build used one
job, one linker thread and a 2 GiB per-process address-space limit; no full
source rebuild or initramfs change. Only the kernel differs from the first
radio-test package. Image SHA-256:
`e594eb0675effc495f78513244a4827daa1c0aa5ad7dec27818bf65bdf9b6378`.

Captured boot reached Debian systemd on the existing NFS export after host
NFS services were restored. Touch initialized without the previous SIO
timeout and **the user confirmed working touch**. This supports the
diagnostic-timing explanation for the previous failure; it is one successful
comparison boot, not a general timing/stability certification.

Murata firmware was reloaded externally. Debian renamed Wi-Fi to `wlp1s0`;
an `iw` scan completed successfully (status 0), finding **12 BSS entries**.
Association, DHCP and traffic have not yet been tested. P2P interface setup
logged `-52`; infrastructure scanning nevertheless succeeded.

Bluetooth accepted MagnoliaA MUR build 1334 but still fails initialization
at opcode `0x204b`. `btmgmt info` reports **zero indices**; power-on reports
Invalid Index `0x11`. Thus Bluetooth is currently **not operational**, even
though `/sys/class/bluetooth/hci0` exists. The `btmgmt` CLI returned status 0
despite the operation error: inspect its response, not exit status alone.
Do not reuse XR's `0x2041` extended-scan workaround for this different error
without tracing the appropriate HCI command/capability/quirk path.

Latest logs: ignored `build/ipad8-radio/no-sio-trace/` (build, capture,
manifest, Debian mount status and radio functional tests). Previous failed
test and the original working-touch bootset remain preserved. New DCSD
capture remains active. No commit or push performed.

## Captured hardware result — 2026-11-06

The first radio DT test boot reached Linux and USB SSH. The unchanged kernel
and touch initramfs were verified against the known-good touch bootset;
only the calibrated radio DT changed. Logs and manifest are retained in
ignored `build/ipad8-radio/boot-test/`; DCSD capture was left running.

* APCIE port 2 linked and enumerated `14e4:43dc` at `0000:01:00.0`.
* OTP identified `s=C1`, module `YSBC`, vendor `m`, version `2.5`.
  This iPad therefore uses the **Murata** candidates, not USI.
* External Wi-Fi loading created `wlan0`, accepted the platform calibration
  and TXCAP, and reported firmware `9.30.503.0.3.50.98`, FWID `01-b45f7231`.
  `ip link set wlan0 up` succeeded. No scan or connection test yet: the
  small initramfs has no `iw` installed.
* Bluetooth accepted `BCM.apple,j171a.hcd` and changed from build 0000 to
  `BCM4355C1 MagnoliaA MUR MCC`, build **1334**. `hci0` exists, but opcode
  `0x204b` still fails with `-38`; power, scan and pairing are untested.
  `btmgmt` is not installed in this initramfs. Do not call Bluetooth fully
  working on firmware acceptance alone.
* Touch initialization failed this boot: SIO call `5001a` timeout, SPI
  `-62`, touch probe `-62`. The radio enablement/timing interaction is not
  isolated; no touch driver change or speculative recovery was applied.
* Missing Wi-Fi firmware during initial boot/probe triggered a kernel
  warning in `brcmf_bus_cancel_reset_work`; Linux continued and subsequent
  external firmware loading succeeded. Preserve this evidence for review.

The sections below describe preparation; their originally unverified
radio enumeration/vendor assumptions are superseded by this result.

### Touch regression comparison

Compared the working-touch DTB to the calibrated radio DTB, resolving
phandles rather than comparing their renumbered integer values. The touch,
SPI3, SIO and DART nodes plus their provider dependency chains (20 nodes)
have no semantic property changes. Kernel, initramfs, m1n1 and boot input
hashes also match the retained working-touch components.

The failing boot logs CONFIG at 2.883052, a successful configuration ACK
(`type=0x65 tag=5`) at 2.893374, then timeout at 3.006974. In
`apple-sio-core.c`, `sio_recv_msg()` performs the synchronous RX `dev_info`
trace **before** `complete()`, while `sio_call()` allows only 100 ms.
Radio startup/missing-firmware warnings and NVMe syslog substantially
increase concurrent console output. This identifies logging/scheduling
delay before completion as a concrete suspect, not missing SmartIO or a
changed touch DMA channel. The RX log timestamp is before the logging call
returns; it does not prove completion was signalled at that time.

Still unproven: exact time between receive/logging/completion and whether
the waiter/tag state was correct at dispatch. Do not claim a fully isolated
root cause yet or simply raise the timeout. Next correction/diagnostic
should process ACK/completion before synchronous logging and record bounded
receive/completion/wait timings without printk in the critical path.
The final attempted live read-only SSH check timed out; no rebind, rebuild
or further boot was performed during this comparison.

These paths are separate from the working XR loaders. No Apple firmware,
SysCfg, calibration, MAC addresses or tickets belong in Git. Do not embed the
radio payloads or BlueZ bundle into the boot-tested initramfs.

## Evidence and current limits

The iPad11,6 17.1.1 / 21B91 Apple DT describes BCM4355 Wi-Fi, module
`magnoliaa`, on APCIE port 2 (reset AP106, CLKREQ AP101). Both `/amfm` and
`uart2/wlan` use SMC `gP13`; this overrides XR's `gP0e` only in J171a DTS.
UART1 Bluetooth uses SMC `gP11`, AP39 BT_WAKE and 3 Mbit/s operation.
TX30 and RTS32 are explicit ADT properties; RX31/CTS33 and mux function 1
reuse XR's T8020 UART group and still require iPad hardware confirmation.

The existing kernel already includes BCM4355 PCIe/OTP support, Broadcom H4
serdev, SMC power functions and diagnostics. These additions need a new DTB,
not a kernel rebuild. Do not apply XR's broken extended-scan quirk unless
J171a HCI results justify it.

The retained A2270 SysCfg has WMac, BMac and WCAL, but **no WSKU**. Do not
fabricate an antenna SKU or reuse XR calibration. This also means brcmfmac's
automatic Apple module/vendor selection requires an explicit NVRAM fallback
after identifying the module vendor. The radio loader therefore has no
default vendor.

## Extracted inputs

Ignored workspace: `devices/a12/build/ipad8-radio/converted/`.
Five `brcm/brcmfmac4355c1-pcie.apple,magnoliaa*` files include both YSBC
Murata (`m`) and USI (`u`) NVRAM templates, firmware, CLM and TXCAP.
`bluetooth-candidates/` contains both MagnoliaA HCD variants from BlueTool.
Both HCD streams passed command-boundary/terminal-command validation;
neither has been selected or loaded. This is not a hardware compatibility test.
`radio-extraction.json` records hashes and source build.

The 5,677,161,172-byte OS DMG passed its ZIP CRC32 and BuildManifest SHA-384.
It is a UDIF DMG with LZFSE-compressed chunks (maximum decoded chunk 1 MiB),
**not one standalone LZFSE stream**. The standalone `lzfse` CLI buffers the
whole input and must not be used here. Selective extraction was performed
through a read-only APFS-FUSE mount with a 256 MiB address-space limit;
observed resident memory was about 5 MiB. The image is currently unmounted.
Use a comma-free symlink for APFS-FUSE: commas in parent names are parsed as
FUSE options. Recheck hard limits for any future extraction.

## Prepare a private DTB

Build the public DTB with `bash devices/a12/dts/build-dtb.sh j171a`, then:

```sh
python3 devices/a12/boot/calibrate-ipad8-radio.py \
  --syscfg /path/to/this-ipad/syscfg.bin \
  --dtb devices/a12/build/payload/t8020-j171a.dtb \
  --output /path/to/new-private-j171a.dtb
```

The tool verifies A2270 SysCfg and J171a DT identity, refuses overwrites,
injects WMac/WCAL and reversed BMac, and writes mode 0600. It does not log
private values. Package this DTB with the existing working kernel and exact
boot-tested touch initramfs; preserve the known-good bootset separately.

## First test, after Linux USB SSH is available

```sh
bash devices/a12/boot/load-ipad8-radio.sh diagnose
bash devices/a12/boot/load-ipad8-radio.sh probe
```

The probe requests the existing `brcmf_pcie_parse_otp_sys_vendor` diagnostic
when dynamic-debug control is available. It deliberately does not pick a
vendor or load an HCD. If that debug filesystem is not mounted, mount it
before probing. PCIe enumeration must succeed before OTP can identify the
module. If no trustworthy OTP vendor appears, stop rather than guess.

After confirming `vendor=m` or `vendor=u` from OTP:

```sh
bash devices/a12/boot/load-ipad8-radio.sh wifi m       # OR u, per actual OTP
bash devices/a12/boot/load-ipad8-radio.sh bluetooth m  # same verified module
```

`IPAD8_SSH_HOST` overrides `root@172.16.42.1`; `IPAD8_RADIO_DIR` overrides the
converted directory. Every operation checks J171a Linux before modifying
the target. PCI and serial devices are discovered, not hardcoded as XR's
bus/device names. HCD uses btbcm's `BCM.apple,j171a.hcd` board fallback.
This stage only binds drivers and collects results; it does not start a
second D-Bus, change network credentials, scan or assume successful radio
operation. Recheck touch, Wi-Fi, HCI identity, patch acceptance and traffic
on hardware. Retest XR separately after iPad work is complete.
