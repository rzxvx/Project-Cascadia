# Shared by the rootfs builder and NFS exporter. XR remains the default.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/profile.sh"
PROFILE="$A12_DEVICE"
case "$PROFILE" in
    xr) DESKTOP_USER=xr; DISPLAY_NAME='iPhone XR User'; HOST_NAME=xr-debian
        TOUCH_FILES='n841-mtfw.bin n841-mtcal.bin' ;;
    ipad8) DESKTOP_USER=ipad; DISPLAY_NAME='iPad 8 User'; HOST_NAME=ipad8-debian
        TOUCH_FILES='j171a-mtfw.bin' ;;
    *) echo 'A12_PROFILE must be xr or ipad8' >&2; exit 2 ;;
esac
A12_AUTOLOGIN="${A12_AUTOLOGIN:-0}"
