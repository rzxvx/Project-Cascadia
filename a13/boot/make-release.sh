#!/bin/sh
# make-release.sh TAG - assemble the release archive from this directory and
# ../build/payload: the boot scripts + the prebuilt payload (no Apple files, no
# keys; package.sh adds the user's own when they pack it).
# Output: a13/build/release/cascadia-<TAG>.tar.gz (+ its SHA256SUMS)
set -eu
TAG=${1:?usage: make-release.sh TAG   e.g. a13-n104-exp1}
cd "$(dirname "$0")"
P=../build/payload
NAME=cascadia-$TAG
R=../build/release/$NAME
rm -rf "$R" && mkdir -p "$R/payload" "$R/inputs"
cp package.sh boot-from-recovery.sh bootargs.txt repair_lzss_img4.py overlay_cpio.py "$R/"
cp inputs/README.md "$R/inputs/"
cp "$P/m1n1-idevice.macho" "$P/t8030-n104.dtb" "$P/initramfs.cpio.gz" "$P/Image.gz" "$R/payload/"
cp ../build/kernel.config "$R/payload/kernel.config"
cat > "$R/README.md" <<EOT
# Cascadia $TAG — Linux on the iPhone 11

Prebuilt payload for iPhone12,1 / N104AP. Full instructions, sources and
status: https://github.com/rzxvx/Project-Cascadia/tree/main/a13

1. Make your inputs (inputs/README.md) and put them in inputs/.
2. sh package.sh            -> out/m1n1-linux.rkrn.img4 (adds your ~/.ssh/*.pub)
3. usbliter8ctl boot inputs/iBSS.n104.20E247.sigpatched.raw
4. sh boot-from-recovery.sh
5. ssh root@172.16.42.1     (or: screen /dev/cu.usbmodemN104AP_linux1)

payload/: m1n1 $(cd ../build/m1n1 && git describe --always --dirty 2>/dev/null) (HoolockLinux/m1n1 d5a10ac + a13/m1n1 patch),
Linux $(gzip -dc "$P/Image.gz" | strings | grep -m1 -o 'Linux version [^ ]*' | cut -d' ' -f3) (yhavry/Hoolock-linux t8030 2f2662c + a13/kernel),
t8030-n104.dtb, initramfs (busybox + dropbear from Debian bookworm), kernel.config.
Never mount or write nvme0n1 from Linux: it is the phone's iOS storage.
EOT
(cd "$R" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS)
(cd ../build/release && tar czf "$NAME.tar.gz" "$NAME" && shasum -a 256 "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
ls -la ../build/release
