# P105 RAM-session diagnostics

These host commands prepare and inspect a separately authorized Linux RAM boot.
They never pair, jailbreak, boot, change host routing/firewall/NFS, probe raw
PMU/THS/NAND registers, or execute an uploaded artifact automatically.
They require an explicit `--device-access` for each connection. Interactive
`ssh` opens a root shell: commands subsequently typed there are the operator's
responsibility and may change the running system.

Use the existing USB peripheral/WLAN image for the initial baseline. The new
charging, writable NAND and external-host components are source-only and
inactive; successful source tests do not enable them. With no Pico, the current
Quickstart requires an existing jailbreak plus iOS OpenSSH for kDFU. A planned
RAM test does not imply permission to install either. The official12H321 boot
files are used with installed13G36; no downgrade/restore is part of the test.

## Key-pinned host commands

Use an identity whose public half is already in the chosen initramfs and a
private mode600 `known_hosts` containing the build's exact public host key as:

```
cascadia-p105 ssh-ed25519 BASE64_PUBLIC_HOST_KEY
```

Derive this public key from the locally built Dropbear key, verify its build
fingerprint, and keep all private keys/artifacts outside Git. Do not use
ssh-keyscan, accept-new, password fallback, or a global known_hosts edit.
An IPv4 address is required; default10.55.0.2. The command ignores user/system
SSH configuration and pins HostKeyAlias `cascadia-p105` so a different RAM image
with a different host identity is rejected. Check current10.55.0.0/24 routes
before separately authorized gadget-interface address setup. No helper sets it.

```
python3 tools/host/ipad-session.py --identity PRIVATE_KEY --known-hosts PRIVATE_PINS --device-access status
python3 tools/host/ipad-session.py --identity PRIVATE_KEY --known-hosts PRIVATE_PINS --device-access logs --out NEW_PRIVATE_LOG
python3 tools/host/ipad-session.py --identity PRIVATE_KEY --known-hosts PRIVATE_PINS --device-access ssh
```

`status` reads kernel/CPU/memory/network/battery/root metadata. `logs` and `test`
add framebuffer/backlight/input/Bluetooth and MTD sysfs metadata plus the last
500 kernel-log lines. `test` is a bounded metadata snapshot, not a claim that
touch, radio, charging or flash transfers work. Missing fields/tools remain
visible as missing data or command errors; this best-effort collector produces
no overall hardware PASS. Preserve SSH failures/timeouts and avoid blind retry.
A new log is created mode600 with O_EXCL; existing paths are never overwritten.
Logs may contain addresses and device identifiers; keep them private.
On the 45-second SSH timeout, captured partial stdout/stderr is saved to the
new log (or emitted to the terminal) before failure is reported. A timeout
never confirms remote completion or an upload receipt and triggers no retry.

MTD size/geometry here is what the driver reports, currently from its hardcoded
configuration, not a measured identification of this physical unit. No MTD
content reads, full scans, `nandctl ppninfo`, /dev/mem or raw controller takeover
occur in the default snapshot. Even raw read commands can change controller
state and conflict with bound kernel H2FMI. Its separate rejection guard is not
a positive exclusive DMA/IOP ownership lease.

## Explicit RAM-only staging

```
python3 tools/host/ipad-session.py --identity PRIVATE_KEY --known-hosts PRIVATE_PINS --device-access deploy --allow-ram-deploy --file REGULAR_FILE --out NEW_RECEIPT
```

At most4MiB are read/uploaded. The remote fixed shell script requires `/run`
itself to be tmpfs, creates a fresh mode700 directory with mktemp, writes only
its new mode600 `artifact`, and returns a checked SHA256 plus exact RAM path.
No user-selected remote path, chmod, install or execution is available. An
unconfirmed result retains possible partial RAM data and fails; no automatic
cleanup/retry can delete a previously existing file. Staging is lost on reboot.

## Initial hands-on sequence

1. Verify the selected kernel/bundle/loader/iBEC hashes and experiment-off
   configuration locally. Verify the matching host fingerprint/key authorization.
2. After the human device session begins, establish actual jailbreak/OpenSSH,
   pairing, battery and cable state. Stop on missing boot prerequisites.
3. Use only the reviewed temporary kDFU chain and guarded local Legacy iOS Kit;
   no auto restore, NAND writes, manual speculative `go`, or default host-link/
   UART changes. Host address setup remains separately scoped to the new gadget.
4. Collect one status/log snapshot. Check battery/temperature and root in RAM.
   Unknown battery data prevents assuming a long-test budget; end tests if the
   battery is critical, temperature rises or USB/kernel errors occur.
5. Test display/touch interactively, then separately qualify WLAN/Bluetooth
   using the current project helpers. Do not activate the new charger, flash
   writer or VBUS provider in a baseline test. Return to iOS with Power+Home;
   never run a restore as automatic error recovery.

Offline tests replace SSH with process-local fakes and cover the connection
gate, key options, unsafe addresses, permissions, no-overwrite, timeout/failure
retention, file bounds and upload receipt/hash/path checks. They exercise no
device connection. Remote scripts also pass shell syntax checking.


## Selecting a frozen package for a later authorized boot

The current tools/flash.sh takes bundle and loader from its own ROOT/output.
The --ibec option selects only iBEC; it does not select either output image.
Before a separately authorized upload, compare the actual files it will read
with the selected package's hashes. A research HEAD or a matching iBEC alone
does not establish the chosen kernel/rootfs/SSH identity.

For an isolated frozen-package session, prepare a private runner tree containing
an unchanged, reviewed copy of tools/flash.sh and the exact selected bundle and
loader at output/staging-bundle.bin and output/staging-loader.bin. Keep the
selected plain iBEC explicit. Pin/check script and all three artifact hashes;
preserve normal output. Record the future --kdfu --no-link --no-uart invocation
as data only. Resolve the reviewed guarded Legacy-Kit/irecovery paths and live
prerequisites in the human session; do not execute or accept the default kit
path automatically. Preparing the tree authorizes no jailbreak, exploit,
transfer, host address change or boot. These options preserve the documented
upload chain; there is no extra go or restore command.
