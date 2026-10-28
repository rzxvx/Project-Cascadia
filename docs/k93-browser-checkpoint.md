# K93 native browser checkpoint — 2026-10-19

Experimental, RAM-only iPad 2,1 (K93AP) bring-up; not a usable replacement OS yet.

Native Linux 6.12, Alpine ARMv7 and Firefox ESR 140.12 rendered a public
HTTPS page (`https://example.com/`) on the physical 1024×768 framebuffer.
The captured screen showed the document and HTTPS lock, and the bounded
desktop test completed with `DESKTOP_EXIT=0`. Normal certificate validation
was retained. Firefox ran as UID 65534 with its sandbox enabled, a private
RAM profile and authenticated local X connection.

This was a deliberately narrow USB-routing diagnostic: one static hostname
mapping and one allowed public IPv4 TCP/443 destination. It does not establish
working DNS, Wi-Fi or general browsing compatibility. Temporary host routing
rules, USB address and unsaved NetworkManager profile were removed afterward.
Host-specific orchestration and raw captures are not included in this source
checkpoint.

The guest diagnostics and root preparation changes are included here:

- `tools/k93-prepare-alpine.py --browser` adds Firefox and diagnostic assets.
- `tools/k93-prepare-gui-initramfs.py --network-root --desktop-test
  --browser-test --run-limit-mib 192` prepares bounded split-root browser assets
  when its prerequisite verified build artifacts exist.
- `rootfs/k93/browser-test.sh` tests the offline JS page and synthetic X input.
  Synthetic input is not evidence of physical touch support.
- `rootfs/k93/internet-browser-test.sh` is an explicit single-host guest-side
  diagnostic, not automatic network configuration or an Internet setup guide.
- Failure framebuffer decoding remains separate from desktop success proof.

The observed configuration used software rendering, Xorg `-dumbSched`,
`ShadowFB=false`, SquashFS direct file access and a separately tested L2-cache
configuration. This commit does not supply a standalone reproducible image or
all experimental kernel configurations. Optional startup diagnostics require
the pinned helper artifacts named in the preparation script.

Physical touch remains broken. DNS, Wi-Fi, persistent installation and an
untethered Linux boot are unfinished. No internal-storage installation is part
of this checkpoint. Touch-driver experiments and private firmware/calibration
data remain outside this commit.
