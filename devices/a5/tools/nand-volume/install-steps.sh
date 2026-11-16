# Sourced by ./cascadia a5 install and uninstall: the NAND install's steps
# (disk=nand).  Uses the cascadia script's say/note/fail/ask/isub/find_ipsw.
#
# The Linux volume is made by tools/nand-volume/nand-volume.sh, a step at a time
# with a reboot between most of them; these functions look at where the iPad is
# (iOS or Linux) and at nand-volume's state, run the next step, and boot the
# iPad where the step after it needs it.  Every write still asks first.

NVS="$ROOT/build/nand-volume"
NV() { IMAGE="$IMAGE" bash "$ROOT/tools/nand-volume/nand-volume.sh" "$@"; }
nv_st() { sed -n "s/^$1=//p" "$NVS/state" 2>/dev/null | tail -1; }
nv_plan_sha() { shasum "$NVS/plan.bin" 2>/dev/null | cut -c1-40; }
nv_header_done() { [ -n "$(nv_plan_sha)" ] && [ "$(nv_st header_written)" = "$(nv_plan_sha)" ]; }
nv_filled() { [ "$(nv_st flow)" = done-create ] && [ -n "$(nv_st filled)" ] && [ "$(nv_st filled)" = "$(nv_plan_sha)" ]; }
lx_up() { ssh -o ConnectTimeout=4 -o BatchMode=yes root@10.55.0.2 true </dev/null >/dev/null 2>&1; }

# NAND or NFS, asked once.  Only "NAND1" / "NFS1" are taken: the 1 says the
# warning was read.
install_ask_disk() {
    local v
    v=$(sed -n 's/^disk=//p' "$INSTALL_CONF" 2>/dev/null | tail -1)
    if [ -z "$v" ] && [ -n "${DISK:-}" ]; then
        v=$DISK
        case "$v" in nand|nfs) mkdir -p "$ROOT/build"; echo "disk=$v" >> "$INSTALL_CONF" ;; esac
    fi
    case "$v" in nand|nfs) echo "$v"; return 0 ;; esac
    [ -t 0 ] || fail "where Linux lives is asked once, at a terminal: run ./cascadia a5 install in one"
    cat >&2 <<'EOF'

    Where should Linux live?

      NAND  On the iPad itself, next to iOS.  The installer shrinks iOS's Data
            partition (by 8 GiB unless you choose otherwise), makes a Linux
            volume in that space and puts the system on it.  Afterwards the
            iPad starts Linux with one tap in the Cascadia app: no computer.
            iOS and everything in it stay, but this rewrites the iPad's
            partition table and its flash translation layer.  Back the iPad
            up first (Finder / iTunes), and keep it charged and plugged in.
      NFS   On this computer, over the cable.  Nothing on the iPad changes;
            Linux runs only while the iPad is plugged in here.

    Recommended: NAND.
    If you really read the warning and the recommendation above, add the
    digit 1 right after your answer.

EOF
    while :; do
        read -r -p "    NAND or NFS: " v || fail "no answer"
        case "$v" in
            NAND1) v=nand; break ;;
            NFS1)  v=nfs; break ;;
            *) echo "    not accepted: read the text above again" >&2 ;;
        esac
    done
    mkdir -p "$ROOT/build"; echo "disk=$v" >> "$INSTALL_CONF"
    echo "$v"
}

# The volume's size in GiB, asked once, with iOS up: 8 suggested, 4 at least,
# Data's free space less 1 GiB at most.
install_nand_size() {
    local v max out
    v=$(sed -n 's/^nand_size=//p' "$INSTALL_CONF" 2>/dev/null | tail -1)
    [ -n "$v" ] && { echo "$v"; return 0; }
    out=$(NV space) || { printf '%s\n' "$out" >&2; fail "could not read iOS's free space (above)"; }
    max=$(printf '%s\n' "$out" | sed -n 's/^max_gib=//p' | tail -1)
    [ -n "$max" ] || fail "could not read iOS's free space: $out"
    [ "$max" -ge 4 ] || fail "iOS's Data has room for ${max} GiB; Linux needs 4 at least.  Free space in iOS (Settings > General > Usage) and run this again"
    local def=8; [ "$max" -lt 8 ] && def=$max
    [ -t 0 ] || { v=$def; echo "nand_size=$v" >> "$INSTALL_CONF"; echo "$v"; return 0; }
    echo "    The Linux volume: 4 to $max GiB (iOS keeps the rest, and 1 GiB of it free)." >&2
    while :; do
        read -r -p "    size in GiB [$def]: " v || fail "no answer"
        v=${v:-$def}
        case "$v" in *[!0-9]*|'') ;; *) [ "$v" -ge 4 ] && [ "$v" -le "$max" ] && break ;; esac
        echo "    a whole number from 4 to $max" >&2
    done
    echo "nand_size=$v" >> "$INSTALL_CONF"
    echo "$v"
}

# Linux up over the cable, from iOS: FLASH_OUT=dir boots another bundle (the RAM
# root one, nand-volume ram-image).
nand_boot_linux() {   # [BUNDLE_DIR]
    local i
    note "booting Linux over the cable (about a minute)"
    FLASH_OUT="${1:-}" isub flash --kdfu
    for i in $(seq 1 40); do lx_up && return 0; sleep 3; done
    note "no ssh to 10.55.0.2 yet: setting up this computer's side of the cable link"
    isub link
    for i in $(seq 1 20); do lx_up && return 0; sleep 3; done
    fail "Linux did not answer on 10.55.0.2.  Is its screen up?  Run ./cascadia a5 install again once it is (it carries on)"
}

# Linux -> iOS: with the cable in, a reboot comes back in iOS.
nand_back_to_ios() {
    note "restarting the iPad into iOS"
    ssh -o ConnectTimeout=5 -o BatchMode=yes root@10.55.0.2 'sync; reboot' </dev/null >/dev/null 2>&1 || true
    echo "    Wait for iOS, unlock it (keep the cable in)."
    ask "iOS is up and unlocked?" || fail "run ./cascadia a5 install again when it is"
}

ios_restart_cleanly() {
    echo "    Restart iOS cleanly now: hold Power, slide to power off, then turn it"
    echo "    on again.  Unlock it once it is up; keep the cable in."
    ask "iOS is up again and unlocked?" || fail "run ./cascadia a5 install again when it is"
}

# The Linux volume, made and filled with ROOT_TAR.  Re-entrant: each pass looks at
# the iPad and the state and does one step.
nand_install_volume() {   # SIZE_GIB ROOT_TAR
    local size="$1" tar="$2" n
    for n in $(seq 1 16); do
        nv_filled && return 0
        if lx_up; then
            if [ "$(nv_st flow)" = create ] && [ "$(nv_st shrunk_restarted)" = 1 ] && ! nv_header_done; then
                NV create "${size}G" || fail "the header write stopped (above).  Do not boot iOS; run ./cascadia a5 install again"
                nand_back_to_ios
            elif [ "$(nv_st flow)" = done-create ]; then
                NV fill "$tar" || fail "the fill stopped (above); run ./cascadia a5 install again to redo it"
            else
                nand_back_to_ios
            fi
            continue
        fi
        if [ "$(nv_st flow)" = done-create ]; then
            nand_boot_linux "$(NV ram-image)"; continue
        fi
        NV create "${size}G" || fail "stopped (above); run ./cascadia a5 install again to carry on"
        if [ "$(nv_st flow)" = done-create ] ||
           { [ "$(nv_st shrunk_restarted)" = 1 ] && ! nv_header_done; }; then
            nand_boot_linux "$(NV ram-image)"
        else
            ios_restart_cleanly
        fi
    done
    fail "the volume steps did not finish; ./cascadia a5 nand-volume status shows where the iPad is"
}

# iOS: the app, kloader, the PC-less iBEC and the boot files at System's root.
nand_install_ios() {
    local ipsw out="$ROOT/build/pcless/out"
    ipsw=$(find_ipsw) || fail "no IPSW (step 4)"
    python3 "$ROOT/scripts/make-pcless.py" --ipsw "$ipsw" --bundle "$ROOT/output/staging-bundle.bin" \
        --devicetree "$ROOT/build/firmware/DeviceTree.dec" --out "$out" | sed 's/^/    /' \
        || fail "the boot files did not build (above)"
    if [ ! -s "$ROOT/build/app/cascadia.deb" ]; then
        sh "$ROOT/app/build.sh" || fail "the app did not build (above): it needs macOS with Xcode, ldid, and BearSSL in build/cache"
    fi
    OUT="$out" bash "$ROOT/app/install-dev.sh" || fail "installing on iOS stopped (above)"
}

# ---------------------------------------------------------------- uninstall --
# The Linux volume's space back to iOS's Data; the app and the boot files off.
nand_uninstall() {
    local n
    for n in $(seq 1 16); do
        if lx_up; then
            if [ "$(nv_st flow)" = remove ] && ! nv_header_done; then
                NV remove || fail "the header write stopped (above).  Do not boot iOS; run ./cascadia a5 uninstall again"
            fi
            nand_back_to_ios
            continue
        fi
        case "$(nv_st flow)" in
            done-remove) break ;;
            remove)
                if nv_header_done; then
                    NV remove || fail "stopped (above); run ./cascadia a5 uninstall again"
                    [ "$(nv_st flow)" = done-remove ] && break
                    ios_restart_cleanly
                else
                    nand_boot_linux "$(NV ram-image)"
                fi ;;
            *)
                NV remove || fail "stopped (above): nothing changed"
                [ "$(nv_st flow)" = remove ] && nand_boot_linux "$(NV ram-image)" ;;
        esac
    done
    [ "$(nv_st flow)" = done-remove ] || fail "the removal did not finish; ./cascadia a5 nand-volume status shows where the iPad is"
    . "$ROOT/tools/ios-ssh.sh"
    ios_setup uninstall
    ios_check_model
    say "the app and the boot files"
    ios "dpkg -r org.cascadia.boot 2>&1 | tail -2; rm -f /kernelcache /ramdisk /devicetree /var/root/go; rm -rf /var/root/cascadia; su mobile -c uicache; sync; sleep 20; sync" </dev/null \
        | sed 's/^/    /' || fail "removing the app stopped (above)"
    grep -v -e '^disk=' -e '^nand_size=' "$INSTALL_CONF" > "$INSTALL_CONF.new" 2>/dev/null; mv "$INSTALL_CONF.new" "$INSTALL_CONF"
    say "done: the iPad is back to iOS alone"
}
