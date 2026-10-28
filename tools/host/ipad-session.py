#!/usr/bin/env python3
"""Explicit, key-pinned SSH diagnostics and bounded RAM-only file staging.

No boot, pairing, network configuration, raw controller access or remote
execution of uploaded files. Importing/help/testing never connects a device.
"""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys

ALIAS = "cascadia-p105"
MAX_DEPLOY = 4 * 1024 * 1024
STATUS = r'''set -u
section() { printf '\n== %s ==\n' "$1"; }
section kernel; uname -a
section cpu
awk '!/^[Ss]erial[[:space:]]*:/' /proc/cpuinfo
section memory; free -m
section network; ip addr
section battery
for supply in /sys/class/power_supply/*; do
    [ -d "$supply" ] || continue
    printf 'supply=%s\n' "${supply##*/}"
    for field in present status capacity voltage_now current_now temp charge_now charge_full; do
        if [ -r "$supply/$field" ]; then
            printf '%s=' "$field"; cat "$supply/$field"
        fi
    done
done
section root_mount
awk '$2 == "/" || $2 == "/run" { print }' /proc/mounts
'''
DIAGNOSTICS = STATUS + r'''
section display
for path in /sys/class/graphics/fb*/name /sys/class/graphics/fb*/virtual_size \
            /sys/class/backlight/*/actual_brightness /sys/class/backlight/*/max_brightness; do
    [ -r "$path" ] || continue
    printf '%s=' "$path"; cat "$path"
done
section input; cat /proc/bus/input/devices
section bluetooth_metadata; ls /sys/class/bluetooth 2>/dev/null || true
section mtd_metadata
for mtd in /sys/class/mtd/mtd[0-9]*; do
    [ -d "$mtd" ] || continue
    printf 'mtd=%s\n' "${mtd##*/}"
    for field in name type flags size erasesize writesize oobsize; do
        [ -r "$mtd/$field" ] || continue
        printf '%s=' "$field"; cat "$mtd/$field"
    done
done
section kernel_log
dmesg | tail -n 500
'''
DEPLOY = r'''set -eu
awk '$2 == "/run" && $3 == "tmpfs" { found=1 } END { exit !found }' /proc/mounts
umask 077
directory=$(mktemp -d /run/cascadia-dev.XXXXXXXX)
cat > "$directory/artifact"
sha256sum "$directory/artifact"
printf '%s\n' "$directory/artifact"
'''


def private_file(path):
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
        raise ValueError("SSH files must be regular files owned by the caller")
    if info.st_mode & 0o077:
        raise ValueError("SSH files must deny group/world access (use mode 600)")


def ssh_arguments(args):
    # A numeric address cannot be interpreted as an option or SSH configuration.
    host = str(ipaddress.IPv4Address(args.host))
    private_file(args.identity)
    private_file(args.known_hosts)
    lines = args.known_hosts.read_text().splitlines()
    if not any(re.fullmatch(ALIAS + r" ssh-ed25519 [A-Za-z0-9+/]+={0,2}", line)
               for line in lines):
        raise ValueError("known_hosts needs the build's explicit cascadia-p105 ed25519 pin")
    return ["ssh", "-F", "/dev/null", "-T", "-i", str(args.identity),
            "-o", "BatchMode=yes", "-o", "IdentitiesOnly=yes",
            "-o", "PasswordAuthentication=no", "-o", "KbdInteractiveAuthentication=no",
            "-o", "StrictHostKeyChecking=yes", "-o", "UpdateHostKeys=no",
            "-o", "UserKnownHostsFile=" + str(args.known_hosts),
            "-o", "GlobalKnownHostsFile=/dev/null", "-o", "HostKeyAlias=" + ALIAS,
            "-o", "ConnectionAttempts=1", "-o", "ConnectTimeout=5",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2",
            "root@" + host]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="10.55.0.2")
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--known-hosts", type=Path, required=True)
    parser.add_argument("--device-access", action="store_true",
                        help="explicit authorization to connect now; never supplied automatically")
    parser.add_argument("action", choices=["status", "logs", "test", "ssh", "deploy"])
    parser.add_argument("--out", type=Path)
    parser.add_argument("--file", type=Path)
    parser.add_argument("--allow-ram-deploy", action="store_true")
    args = parser.parse_args(argv)
    if not args.device_access:
        parser.error("device connection requires --device-access")
    output_fd = None
    try:
        command = ssh_arguments(args)
        if args.action == "deploy":
            if not args.allow_ram_deploy or args.file is None:
                raise ValueError("deploy needs --allow-ram-deploy and --file")
            # Bound before loading. Refuse devices, FIFOs and directories.
            info = args.file.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_DEPLOY:
                raise ValueError("deploy accepts one regular file of at most 4 MiB")
            fd = os.open(args.file, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
            with os.fdopen(fd, "rb") as source:
                if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                    raise ValueError("deploy source is no longer a regular file")
                data = source.read(MAX_DEPLOY + 1)
                if len(data) > MAX_DEPLOY:
                    raise ValueError("deploy source grew beyond 4 MiB")
            command += ["sh -c " + shlex.quote(DEPLOY)]
        elif args.action == "ssh":
            if args.out or args.file or args.allow_ram_deploy:
                raise ValueError("interactive SSH takes no diagnostic/deploy options")
            # This explicit interactive action may change the remote system.
            command[command.index("-T")] = "-tt"
            return subprocess.run(command).returncode
        else:
            if args.file or args.allow_ram_deploy:
                raise ValueError("diagnostics take no deploy options")
            data = (STATUS if args.action == "status" else DIAGNOSTICS).encode()
            command += ["sh -s"]
        if args.out:
            output_fd = os.open(args.out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        timed_out = False
        try:
            result = subprocess.run(command, input=data, capture_output=True, timeout=45)
        except subprocess.TimeoutExpired as error:
            # run() kills and reaps the child before raising. Retain the bytes
            # it captured: a stalled kernel/connection often leaves the most
            # useful diagnostics immediately before the timeout.
            timed_out = True
            result = subprocess.CompletedProcess(command, 1, error.output or b"",
                                                 error.stderr or b"")
        if output_fd is not None:
            with os.fdopen(output_fd, "wb") as output:
                output_fd = None
                output.write(result.stdout)
                output.write(result.stderr)
        else:
            if result.stdout:
                sys.stdout.buffer.write(result.stdout)
            if result.stderr:
                sys.stderr.buffer.write(result.stderr)
        if timed_out:
            print("ipad-session: SSH timed out after 45 seconds; partial output retained; "
                  "remote completion is unconfirmed; no automatic retry", file=sys.stderr)
            return 1
        if result.returncode:
            return result.returncode
        if args.action == "deploy":
            rows = result.stdout.decode().splitlines()
            expected = hashlib.sha256(data).hexdigest()
            if len(rows) != 2 or not rows[0].startswith(expected + "  ") or not re.fullmatch(
                    r"/run/cascadia-dev\.[A-Za-z0-9]{8}/artifact", rows[1]):
                raise ValueError("remote upload result/hash is unconfirmed; do not retry blindly")
            if rows[0] != expected + "  " + rows[1]:
                raise ValueError("remote checksum filename differs from returned RAM path")
        return 0
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print("ipad-session: " + str(error), file=sys.stderr)
        return 1
    finally:
        if output_fd is not None:
            os.close(output_fd)


if __name__ == "__main__":
    sys.exit(main())
