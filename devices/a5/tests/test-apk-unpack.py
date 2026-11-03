#!/usr/bin/env python3
"""
Regression tests for scripts/apk-unpack.py.

The bug these exist for: tarfile's streaming mode ("r|gz") stops at the end of
the FIRST gzip member, and an .apk is three members concatenated.  The first is
the signature, so a streaming reader extracts nothing at all and reports an
empty archive rather than an error -- which looks exactly like a download
problem.  GNU tar walks every member, so a shell implementation works and the
obvious Python translation of it does not.

Fixtures are built here rather than downloaded: no network, and they are
deliberately STRICTER than a real package.  Every tar segment below carries its
end-of-archive null records, whereas abuild omits them from the signature and
control segments.  Passing here therefore implies passing on a real .apk, and
the test keeps holding if Alpine ever changes that detail.

    python3 tests/test-apk-unpack.py
"""

import gzip
import io
import importlib.util
import os
import sys
import tarfile
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(os.path.dirname(HERE), "scripts", "apk-unpack.py")


def load():
    spec = importlib.util.spec_from_file_location("apk_unpack", TARGET)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tar_bytes(files):
    """files maps name -> bytes, or name -> (bytes, mode)."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for name, entry in files.items():
            data, mode = entry if isinstance(entry, tuple) else (entry, 0o644)
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mode = mode
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def gz(b):
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb") as f:
        f.write(b)
    return out.getvalue()


def fake_apk():
    """signature || control || data, as three concatenated gzip members."""
    return (gz(tar_bytes({".SIGN.RSA.test.rsa.pub": b"sig"}))
            + gz(tar_bytes({".PKGINFO": b"pkgname = fake\n"}))
            + gz(tar_bytes({"usr/sbin/dropbear": (b"\x7fELFfake", 0o755),
                            "usr/lib/libx.so.1": (b"lib", 0o644)})))


def fake_index():
    idx = (
        "C:Q1aaa\nP:dropbear\nV:2026.91-r0\nA:armhf\n"
        "D:so:libc.musl-armhf.so.1 so:libutmps.so.0.1\n"
        "p:cmd:dropbear=2026.91-r0\n\n"
        "C:Q1bbb\nP:utmps-libs\nV:0.1.3.0-r0\nA:armhf\n"
        "p:so:libutmps.so.0.1=0.1 so:libutmpx.so.0.1=0.1\n\n"
        "C:Q1ccc\nP:skalibs-libs\nV:2.14.4.0-r0\nA:armhf\n"
        "p:so:libskarnet.so.2.14=2.14.4\n\n")
    return gz(tar_bytes({"APKINDEX": idx.encode(), "DESCRIPTION": b"x"}))


FAILED = []


def check(label, got, want):
    if got == want:
        print(f"  PASS  {label}")
    else:
        print(f"  FAIL  {label}\n        got  {got!r}\n        want {want!r}")
        FAILED.append(label)


def main():
    au = load()
    with tempfile.TemporaryDirectory() as tmp:
        os.chdir(tmp)

        names = sorted(au.untar_apk(fake_apk(), "out"))
        check("untar_apk reads all three concatenated gzip members",
              names, ["usr/lib/libx.so.1", "usr/sbin/dropbear"])
        check("data segment actually hits the disk",
              os.path.exists("out/usr/sbin/dropbear"), True)
        check("package metadata is not installed as a file",
              os.path.exists("out/.PKGINFO"), False)
        # tarfile's set_attrs=False drops the mode along with uid/gid, so an
        # installed binary arrives at the umask default and is not executable.
        # Everything downstream still "works" -- the file is there, the right
        # size, the right bytes -- until the device refuses to run it.
        check("executable bit survives extraction",
              oct(os.stat("out/usr/sbin/dropbear").st_mode & 0o777), oct(0o755))
        check("a non-executable file is not made executable",
              oct(os.stat("out/usr/lib/libx.so.1").st_mode & 0o777), oct(0o644))

        by_name, by_soname = au.parse_index(fake_index())
        check("index: name -> filename",
              by_name.get("dropbear"), "dropbear-2026.91-r0.apk")
        check("index: soname -> filename",
              by_soname.get("libutmps.so.0.1"), "utmps-libs-0.1.3.0-r0.apk")
        check("index: several so: tokens on one provides line",
              by_soname.get("libutmpx.so.0.1"), "utmps-libs-0.1.3.0-r0.apk")
        # D: is what a package NEEDS, p: is what it OFFERS.  Confusing them
        # makes every package look like the provider of its own dependencies.
        check("index: depends (D:) never lands in the provides map",
              by_soname.get("libc.musl-armhf.so.1"), None)

        check("package name recovered from filename",
              by_soname["libutmps.so.0.1"].rsplit("-", 2)[0], "utmps-libs")
        check("package name recovered when the version has dots in it",
              "skalibs-libs-2.14.4.0-r0.apk".rsplit("-", 2)[0], "skalibs-libs")

        os.makedirs("rootfs/lib")
        open("rootfs/lib/libc.musl-armhf.so.1", "w").close()
        open("rootfs/lib/ld-musl-armhf.so.1", "w").close()
        check("satisfied() finds a library present in the rootfs",
              au.satisfied("libc.musl-armhf.so.1", ["rootfs"]), True)
        check("satisfied() reports a genuinely absent library",
              au.satisfied("libutmps.so.0.1", ["rootfs"]), False)
        check("satisfied() handles the interpreter's absolute path",
              au.satisfied("/lib/ld-musl-armhf.so.1", ["rootfs"]), True)

    print()
    if FAILED:
        print(f"{len(FAILED)} failure(s)")
        return 1
    print("all green")
    return 0


if __name__ == "__main__":
    sys.exit(main())
