#!/usr/bin/env python3
"""dropbear-hostkey.py -- the device's ssh host key, made once on the build host.

    python3 scripts/dropbear-hostkey.py build/keep/etc/dropbear/dropbear_ed25519_host_key

prints "made SHA256:..." or "kept SHA256:..." -- the fingerprint ssh will show
on the first connection, so the two can be compared.  An existing key is never
replaced.

Why: the device runs dropbear -R, which makes a host key at first use and keeps
it in /etc/dropbear.  On the NFS root that directory is on the host's disk and
the key never changed; on the RAM root it went with every reboot, and every
boot greeted ssh with REMOTE HOST IDENTIFICATION HAS CHANGED.  A key baked into
the image is the same on every boot and on both roots.

ssh-keygen does the cryptography -- it is on every host that can ssh into the
device at all -- and this only moves the 64 key bytes from OpenSSH's container
into dropbear's, which is what dropbear's buf_put_ed25519_priv_key() writes:

    uint32 11 "ssh-ed25519"  uint32 64  <32-byte seed><32-byte public key>

Checked against dropbear 2022.83: dropbearkey -y reads the result back with the
same fingerprint, and a full ssh handshake against dropbear -r verifies.
"""

import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

TYPE = b"ssh-ed25519"
SIZE = 4 + len(TYPE) + 4 + 64


def fail(msg):
    sys.exit("dropbear-hostkey: " + msg)


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def u32(self):
        if self.pos + 4 > len(self.data):
            fail("truncated OpenSSH key")
        (v,) = struct.unpack_from(">I", self.data, self.pos)
        self.pos += 4
        return v

    def string(self):
        n = self.u32()
        if self.pos + n > len(self.data):
            fail("truncated OpenSSH key")
        s = self.data[self.pos:self.pos + n]
        self.pos += n
        return s


def openssh_ed25519(path):
    """The 64 private bytes (seed, then public key) of an unencrypted OpenSSH key."""
    lines = open(path).read().strip().splitlines()
    if lines[0] != "-----BEGIN OPENSSH PRIVATE KEY-----":
        fail("%s is not an OpenSSH private key" % path)
    blob = base64.b64decode("".join(lines[1:-1]))
    magic = b"openssh-key-v1\0"
    if not blob.startswith(magic):
        fail("unexpected key container")
    r = Reader(blob[len(magic):])
    cipher, kdf = r.string(), r.string()
    r.string()                                  # kdf options
    if cipher != b"none" or kdf != b"none":
        fail("the key is encrypted")
    if r.u32() != 1:
        fail("expected exactly one key")
    r.string()                                  # public key blob
    p = Reader(r.string())                      # private section
    if p.u32() != p.u32():
        fail("check ints differ -- corrupt key")
    if p.string() != TYPE:
        fail("not an ed25519 key")
    pub, priv = p.string(), p.string()
    if len(pub) != 32 or len(priv) != 64 or priv[32:] != pub:
        fail("unexpected ed25519 key layout")
    return priv


def fingerprint(pub):
    blob = struct.pack(">I", len(TYPE)) + TYPE + struct.pack(">I", 32) + pub
    return "SHA256:" + base64.b64encode(hashlib.sha256(blob).digest()).decode().rstrip("=")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: dropbear-hostkey.py OUT")
    out = sys.argv[1]

    if os.path.exists(out):
        data = open(out, "rb").read()
        if len(data) != SIZE or data[4:4 + len(TYPE)] != TYPE:
            fail("%s exists but is not a dropbear ed25519 key -- move it away" % out)
        print("kept " + fingerprint(data[-32:]))
        return

    with tempfile.TemporaryDirectory() as tmp:
        key = os.path.join(tmp, "key")
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "",
                        "-C", "cascadia-p105ap", "-f", key], check=True)
        priv = openssh_ed25519(key)

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(struct.pack(">I", len(TYPE)) + TYPE + struct.pack(">I", 64) + priv)
    print("made " + fingerprint(priv[32:]))


if __name__ == "__main__":
    main()
