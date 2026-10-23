# Runs ON THE iPad, NFS root and ./cascadia net on:
#   ssh root@10.55.0.2 sh -s < tools/audio/audio-setup.sh
#
# Only for an NFS root seeded before the image carried the speakers'
# userland.  The image has alsa-utils among its base packages
# (scripts/add-base-packages.sh), so a RAM boot and a root seeded from it
# have them; /etc/asound.conf (dmix as "default", initramfs/etc/asound.conf)
# is copied onto the NFS root by /init at every boot.  An older root lacks
# only the package.  Running it again changes nothing.
set -e

apk add --no-progress alsa-utils
echo "audio: ok -- try: speaker-test -c 2 -t sine -l 1"
