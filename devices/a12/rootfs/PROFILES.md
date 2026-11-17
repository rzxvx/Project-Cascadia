# Debian accounts, login and RAM watchdog

Start with [../QUICKSTART.md](../QUICKSTART.md); see
[../DEBIAN_XFCE_NFS.md](../DEBIAN_XFCE_NFS.md) for root reuse and networking.
The central selector is `A12_DEVICE=xr|ipad8`; `A12_PROFILE` remains a compatible
alias. Conflicting selections are rejected.

| Fresh profile | Account | Display name | Hostname |
|---|---|---|---|
| xr | xr | iPhone XR User | xr-debian |
| ipad8 | ipad | iPad 8 User | ipad8-debian |

A rootfs build creates **only** the selected profile and replaces its staging
root. It does not run when merely selecting another board. Reuse an existing NFS
root without rebuilding; its existing account/hostname/password stay in place.
Do not rename an account or root directory while a device is running from it.

## Login policy

- Fresh builds prompt interactively for a desktop password. No common password
  or user's chosen password is stored in source or documentation.
- Password login is the default. Explicit `A12_AUTOLOGIN=1` selects passwordless
  bench autologin; it is not the normal login policy.
- Fresh desktop users are unprivileged and not automatically in sudoers. If you
  deliberately enable sudo, set a password and validate the sudoers configuration.
- Root SSH uses the build host's packed public keys. Desktop login and SSH
  authentication are separate.

For an existing inactive root, a password can be set on the host using
`chroot /path/to/root passwd ACCOUNT`; disabling autologin alone does not set a
password. Do not publish the shadow database or credentials.

## Login keyboard

XFCE uses LightDM's GTK greeter with a standalone Onboard launcher,
`/usr/local/bin/cascadia-greeter-keyboard`. It sizes the keyboard from the display
geometry and sets the **greeter user's** input to direct XInput, single touch and
XTest keystrokes. It does not change desktop touch calibration.

J171 laboratory tests confirmed visibility and typing. Embedded `--xid` was not
visible; a standalone GTK-input keyboard was visible but did not receive touches.
Do not present those failed alternatives as working configurations. Accessibility
controls are exposed at login and keyboard startup is requested automatically.
The new separated clean-build workflow still needs its own hardware login test.

## USB reconnect watchdog (default for XR and iPad)

The default initramfs builder uses `A12_USBWATCH_MODE=a13`: pinned ARM64 static
BusyBox and the upstream A13 watchdog loop (cached upstream `091d441`). No extra
option is needed. Rebuild the selected initramfs and repack to update older images.

Lifecycle follows the upstream design:
1. Start DHCP alone while discovering/mounting NFS.
2. Stop DHCP before moving `/proc` (process discovery still works there).
3. Preserve tmpfs `/run`, copy static BusyBox, config and worker to
   `/run/cascadia`, then start one worker chrooted into the new OS root.
4. `switch_root` to Debian. The `/run/cascadia/a13-usbwatch` marker causes
   `cascadia-usbwatch.service` to skip replacement. Inactive is expected here.
5. Start an initramfs watchdog only if root handoff fails.

Check while connected:
```sh
ssh root@172.16.42.1 'test -e /run/cascadia/a13-usbwatch && echo A13_HANDOFF; ps -eo pid,ppid,args | grep -E "[u]sbwatch|[u]dhcpd"; cat /sys/class/udc/*/state'
```
Expect one `/run/cascadia/busybox sh /run/cascadia/usbwatch` and its one DHCP
child, with USB `configured`. The upstream worker is quiet: it does not create
`/run/cascadia/usbwatch.log`. Serial logs, process state and restored SSH/NFS
are the recovery evidence. The separate RAM-root diagnostic service is not
installed or started by this default.

`A12_USBWATCH_MODE=debian` retains the old replacement path for comparison only;
it is not the recommended or default flow. Its service and preparation helper
remain for older initramfs images lacking the handoff marker.

### Hardware evidence (2026-11-06/07)

Corrected A13-lifecycle images booted on J171 and N841 with one worker/DHCP pair
and no manual cleanup. Automatic reconnect restored USB, SSH and NFS on both;
J171 also passed repeated reconnects and a further fresh-boot test. One earlier
clean A13-style J171 reconnect initially timed out, so intermittent recovery
remains a limitation, not a claim of exhaustive stability. Radio presence is
not a substitute for association/pairing testing.

The root is still a hard USB NFS mount: applications may block while the cable
is absent. This is reconnect recovery, not cable-free Debian. A13/A5 files and
the shared kernel are not modified by this userspace default.
