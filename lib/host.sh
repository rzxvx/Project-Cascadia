# lib/host.sh -- what a device's `setup` puts on this computer, sourced, not
# run.  One place that knows the package managers (brew, apt, pacman, dnf),
# so devices/<soc>/cascadia setup only says what it needs, by these names:
#
#   base      git, python3, rsync, curl
#   docker    the engine (Docker Desktop on macOS: asked, not forced)
#   libusb    for lib/appleusb.py (pyusb itself comes with pylib_pyusb)
#   nfs       an NFS server (Linux; macOS has nfsd)
#   firewall  iptables or nftables, for net on (Linux; macOS has pf)
#   usbmuxd   ssh to iOS over USB (Linux; part of macOS)
#
# Everything asks before it changes anything (YES=1: yes to all), and says
# what it runs.  The caller defines REPO (the repository's top), say, note
# and fail.

HOST_OS=$(uname -s)
SUDO=sudo
[ "$(id -u)" = 0 ] && SUDO=

host_pm() {
    if [ "$HOST_OS" = Darwin ]; then command -v brew >/dev/null 2>&1 && echo brew || echo none
    elif command -v apt-get >/dev/null 2>&1; then echo apt
    elif command -v pacman >/dev/null 2>&1; then echo pacman
    elif command -v dnf >/dev/null 2>&1; then echo dnf
    else echo none; fi
}

# our name -> the package manager's names (empty: nothing to install there)
pm_names() {
    case "$(host_pm)/$1" in
        brew/base)       echo "" ;;           # git, python3, rsync, curl: the Command Line Tools
        brew/libusb)     echo libusb ;;
        brew/*)          echo "" ;;
        apt/base)        echo git python3 rsync curl ;;
        apt/docker)      echo docker.io ;;
        apt/libusb)      echo libusb-1.0-0 ;;
        apt/nfs)         echo nfs-kernel-server rpcbind ;;
        apt/firewall)    echo iptables ;;
        apt/usbmuxd)     echo usbmuxd ;;
        pacman/base)     echo git python rsync curl ;;
        pacman/docker)   echo docker ;;
        pacman/libusb)   echo libusb ;;
        pacman/nfs)      echo nfs-utils ;;
        pacman/firewall) echo iptables-nft ;;
        pacman/usbmuxd)  echo usbmuxd ;;
        dnf/base)        echo git python3 rsync curl ;;
        dnf/docker)      echo moby-engine ;;
        dnf/libusb)      echo libusb1 ;;
        dnf/nfs)         echo nfs-utils ;;
        dnf/firewall)    echo iptables-nft ;;
        dnf/usbmuxd)     echo usbmuxd ;;
        apt/qemu)        echo qemu-user-static binfmt-support ;;
        pacman/qemu)     echo qemu-user-static qemu-user-static-binfmt ;;
        dnf/qemu)        echo qemu-user-static ;;
    esac
}

ask() {   # QUESTION -> 0 for yes (Enter is yes)
    [ "${YES:-0}" = 1 ] && return 0
    [ -t 0 ] || return 1
    local a
    read -r -p "    $1 [Y/n] " a || a=n
    case "${a:-y}" in y|Y|yes|д|Д) return 0 ;; *) return 1 ;; esac
}

run() {   # show it, then run it
    printf '    $ %s\n' "$*"
    "$@"
}

# install our names that are missing; a package manager knows what it has,
# so this just asks it for the lot
pm_install() {
    local pkgs="" n
    for n in "$@"; do pkgs="$pkgs $(pm_names "$n")"; done
    pkgs=$(echo $pkgs)
    [ -n "$pkgs" ] || return 0
    case "$(host_pm)" in
        brew)   local miss="" p
                for p in $pkgs; do brew list --versions "$p" >/dev/null 2>&1 || miss="$miss $p"; done
                [ -z "$miss" ] && return 0
                ask "install$miss with Homebrew?" || return 1
                run brew install $miss ;;
        apt)    ask "install $pkgs with apt (sudo)?" || return 1
                run $SUDO apt-get update -q && run $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y $pkgs ;;
        pacman) ask "install $pkgs with pacman (sudo)?" || return 1
                run $SUDO pacman -Sy --needed --noconfirm $pkgs ;;
        dnf)    ask "install $pkgs with dnf (sudo)?" || return 1
                run $SUDO dnf install -y $pkgs ;;
        *)      note "no package manager found: install $pkgs yourself"; return 1 ;;
    esac
}

# docker that this user can talk to
ensure_docker() {
    if docker info >/dev/null 2>&1; then note "ok: docker"; return 0; fi
    if [ "$HOST_OS" = Darwin ]; then
        if ! command -v docker >/dev/null 2>&1; then
            note "Docker Desktop is not installed."
            if [ "$(host_pm)" = brew ] && ask "install it (brew install --cask docker)?"; then
                run brew install --cask docker
            else
                fail "install Docker Desktop (https://www.docker.com/products/docker-desktop/), start it, and run this again"
            fi
        fi
        note "start Docker Desktop (it is in Applications) and wait until it says it is running"
        local i=0
        until docker info >/dev/null 2>&1; do
            i=$((i + 1)); [ $i -gt 120 ] && fail "docker is still not running"
            [ $i = 1 ] && open -a Docker 2>/dev/null || true
            sleep 2
        done
        note "ok: docker"; return 0
    fi
    command -v docker >/dev/null 2>&1 || pm_install docker || fail "docker is needed"
    if ! systemctl is-active -q docker 2>/dev/null; then
        ask "start docker now and at every boot (systemctl enable --now docker)?" \
            && run $SUDO systemctl enable --now docker
    fi
    docker info >/dev/null 2>&1 && { note "ok: docker"; return 0; }
    if ! id -nG "$USER" | tr ' ' '\n' | grep -qx docker; then
        ask "let $USER use docker without sudo (the docker group)?" \
            && run $SUDO usermod -aG docker "$USER"
    fi
    sg docker -c 'docker info' >/dev/null 2>&1 \
        && fail "docker works for $USER from the next login on: log out and in (or: newgrp docker), then run this again"
    fail "docker is installed but does not answer (sudo systemctl status docker)"
}

# the device's userland is ARM: on an x86_64 Linux docker runs it through
# qemu, which a fresh install does not have registered (Docker Desktop does).
# The distribution's qemu-user-static first: it registers through
# systemd-binfmt at every boot.  tonistiigi/binfmt only as a fallback -- its
# registration is gone after a reboot.  And a local image of the same tag for
# another platform is pulled again for this one: docker does not always do
# that by itself.
binfmt_try() { BINFMT_ERR=$(docker run --rm --platform "$1" alpine:3.24 true 2>&1); }
ensure_binfmt() {
    local plat=$1
    binfmt_try "$plat" && { note "ok: $plat containers"; return 0; }
    if echo "$BINFMT_ERR" | grep -q -i -e 'does not match the specified platform' -e 'no matching manifest'; then
        run docker pull -q --platform "$plat" alpine:3.24 >/dev/null || true
        binfmt_try "$plat" && { note "ok: $plat containers"; return 0; }
    fi
    if [ "$HOST_OS" = Linux ]; then
        note "docker cannot run $plat containers yet: $(echo "$BINFMT_ERR" | tail -1)"
        if pm_install qemu; then
            command -v systemctl >/dev/null 2>&1 && run $SUDO systemctl restart systemd-binfmt 2>/dev/null || true
            binfmt_try "$plat" && { note "ok: $plat containers (qemu-user-static)"; return 0; }
        fi
        if ask "register qemu with docker's tonistiigi/binfmt instead (until the next reboot)?"; then
            run docker run --privileged --rm tonistiigi/binfmt --install arm,arm64 >/dev/null
            binfmt_try "$plat" && { note "ok: $plat containers (until the next reboot)"; return 0; }
        fi
    fi
    printf '%s\n' "$BINFMT_ERR" | sed 's/^/    docker: /' >&2
    fail "docker still cannot run $plat containers (its words above)"
}

# Apple's USB devices for this user (Linux; macOS needs nothing)
ensure_udev() {
    [ "$HOST_OS" = Linux ] || return 0
    local f=/etc/udev/rules.d/39-cascadia-apple.rules
    [ -e "$f" ] && { note "ok: udev rule $f"; return 0; }
    ask "give your user Apple's USB devices (a udev rule in $f, sudo)?" || return 0
    run $SUDO mkdir -p "$(dirname "$f")"
    echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", MODE="0666"' | run $SUDO tee "$f" >/dev/null
    run $SUDO udevadm control --reload 2>/dev/null && run $SUDO udevadm trigger 2>/dev/null || true
    note "ok: udev rule (replug the device once)"
}

# pyusb for lib/appleusb.py: the system's if it has one, else the wheel from
# PyPI into build/pylib -- no pip, so no "externally managed environment"
PYUSB_URL=https://files.pythonhosted.org/packages/28/b8/27e6312e86408a44fe16bd28ee12dd98608b39f7e7e57884a24e8f29b573/pyusb-1.3.1-py3-none-any.whl
PYUSB_SHA256=bf9b754557af4717fe80c2b07cc2b923a9151f5c08d17bdb5345dac09d6a0430
pylib_pyusb() {
    if python3 -c 'import usb.core' >/dev/null 2>&1 || [ -f "$REPO/build/pylib/usb/core.py" ]; then
        note "ok: pyusb"; return 0
    fi
    local w="$REPO/build/pylib/pyusb.whl"
    mkdir -p "$REPO/build/pylib"
    run curl -fsSL -o "$w" "$PYUSB_URL" || fail "could not download pyusb"
    python3 - "$w" "$PYUSB_SHA256" "$REPO/build/pylib" <<'PY' || fail "pyusb's download does not match its hash"
import hashlib, sys, zipfile
w, want, out = sys.argv[1:4]
if hashlib.sha256(open(w, 'rb').read()).hexdigest() != want:
    sys.exit(1)
z = zipfile.ZipFile(w)
z.extractall(out, [n for n in z.namelist() if n.startswith('usb/')])
PY
    rm -f "$w"
    note "ok: pyusb 1.3.1 in build/pylib"
}

# a key the device lets in, without a passphrase: ssh then just works
ensure_sshkey() {
    [ -f "$HOME/.ssh/cascadia.pub" ] && { note "ok: ssh key ~/.ssh/cascadia"; return 0; }
    ask "make an ssh key for the device (~/.ssh/cascadia, no passphrase)?" || return 0
    mkdir -p "$HOME/.ssh" && chmod 700 "$HOME/.ssh"
    run ssh-keygen -q -t ed25519 -N '' -f "$HOME/.ssh/cascadia" -C cascadia
    note "ok: ssh key ~/.ssh/cascadia (the next build puts it into the image)"
}
