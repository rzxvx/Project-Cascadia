# Device-specific boot preparation and transfer

Follow [../QUICKSTART.md](../QUICKSTART.md). Select `A12_DEVICE=xr` or `ipad8`.
This describes the new source workflow, not an already validated replacement for
the preserved laboratory payloads.

`inputs [--serial|--no-serial]` prepares pinned Apple components in
`build/devices/<device>/boot/`:

- `iBSS.linux.raw`: selected-board validation patches plus explicit Linux arguments.
- `DeviceTree.raw` / `DeviceTree.img4`: pristine selected Apple ADT, wrapped `rdtr`.
- `RestoreTrustCache.img4`: minimal empty `rtsc` staging payload.
- `SmartIO.img4`: **J171 only**, matching ASCv2 payload wrapped `siof`.
- `ticket.im4m` and `bootset.json`: local ticket and identity/hash metadata.

`pack [--serial|--no-serial]` also creates `linux.macho`, `linux.img4`, and
`linux.json`. The final payload combines shared m1n1/kernel with the selected
Linux DTB, touch/calibration and initramfs/NFS setting. The iOS
`launchd_unsecure_cache=1` preset is not used. Serial capture is requested only
for a serial-enabled bootset.

Before touching USB:

```sh
python3 devices/a12/boot/boot-device.py --verify-only
```

`./cascadia a12 boot` then checks artifact manifests and exactly one matching,
pwned Apple DFU device (PRODUCT, CPID and BDID). Transfer order:

1. Selected raw iBSS through A12's frozen `usbliter8ctl`.
2. Wait for matching iBSS Recovery identity.
3. `DeviceTree.img4`, then `devicetree`.
4. **iPad only:** `SmartIO.img4`, then `firmware`.
5. `RestoreTrustCache.img4`, then `firmware`.
6. `linux.img4`, then `bootx`.

A transfer returning successfully is not proof of a completed Linux boot.
Inspect serial capture and SSH. Logs are written under the selected boot
folder's `logs/<timestamp>/`; its capture PID is profile-specific.

`./cascadia a12 boot radios` additionally waits for the selected board's Debian
SSH and runs only its external firmware loaders. The host NFS server/USB address
must be ready. iPad vendor/file checks happen before boot; they do not guess a
Murata/USI module. See [../FIRMWARE.md](../FIRMWARE.md).

The compatibility names `boot-xr-linux.sh` and `boot-and-load-xr.sh` accept XR/XS Max phone profiles.
They no longer accept arbitrary legacy `XR_LINUX_IMG4` overrides; use the selected
boot directory plus matching manifests. `prepare-xr-linux-bootset.py` likewise
rejects an iPad profile. New callers should use the unified commands.

Apple inputs, tickets, private calibration, payloads and logs are ignored by Git.

## XS Max (D331p)

Use `A12_BOARD=d331p` or `--device xsmax`. The same identity/hash-checked
boot orchestrator uploads the ticketed **16.6.1** `gfx.img4` after the restore
trust cache and before Linux/`bootx`. Its iBSS additionally uses `--any-im4m`.
The selected m1n1 comes from `m1n1/d331p/`, not the XR/iPad series.
Touch and PCIe radio firmware are embedded only for this board.
Serial capture is skipped when no DCSD tty is present.
