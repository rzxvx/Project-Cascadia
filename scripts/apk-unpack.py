#!/usr/bin/env python3
"""
Unpack Alpine .apk packages, with shared-library dependencies resolved, WITHOUT
running apk.

Why this exists: the target rootfs is Alpine armhf and the build host is Apple
Silicon, which has no AArch32 EL0 at all -- 32-bit ARM code cannot execute
there, natively or under Docker.  So `apk` itself can never be run against this
tree.  Everything here is pure data handling: an .apk is three concatenated
gzip streams of tar archives, and a package's dependencies can be read out of
the repository's APKINDEX and out of each ELF's own dynamic section.  No
target-architecture code is ever executed.

What this deliberately does NOT do, because apk does it properly and this does
not need to: version constraint solving, conflicts and replaces, install
scripts and triggers, signature verification, or the installed-package
database.  It resolves exactly one thing -- an unsatisfied soname -- by asking
the index which package provides it.  That covers "drop a static-ish userspace
tool into an initramfs" and nothing more ambitious.

Runs inside the build container (needs network and readelf), not on the host.
"""

import argparse
import gzip
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def log(msg):
    print(f"    {msg}", flush=True)


def fetch(url):
    with urllib.request.urlopen(url, timeout=60) as r:
        return r.read()


def untar_apk(blob, dest):
    """
    Extract every member of an .apk into dest.

    An .apk is signature || control || data, each a gzipped tar.  Two separate
    things have to be handled to read all three, and getting either wrong yields
    an empty extraction rather than an error:

      gzip level -- the members are CONCATENATED, and tarfile's streaming mode
      ("r|gz") stops at the end of the first member, so it sees only the
      signature and reports an empty archive.  gzip.decompress() walks every
      member, so decompress first and hand tarfile plain bytes.  (GNU tar gets
      this right, which is why the shell version of this script worked.)

      tar level -- the first two segments omit their end-of-archive null records
      precisely so a reader runs on into the data segment.  ignore_zeros=True
      means we do not depend on abuild having done that: a segment that does
      carry the terminator would otherwise end extraction early.
    """
    names = []
    raw = gzip.decompress(blob)
    with tarfile.open(fileobj=io.BytesIO(raw), mode="r:", ignore_zeros=True) as tf:
        for m in tf:
            # Package metadata is not part of the filesystem.
            if m.name.startswith(".") or m.name.startswith("/"):
                continue
            if ".." in m.name.split("/"):
                continue          # never write outside dest
            tf.extract(m, dest, set_attrs=False)
            # set_attrs=False on purpose: it would also try to apply uid/gid and
            # mtime, and chown across a Docker bind mount from macOS is not
            # something to depend on.  But the mode has to come across by hand or
            # every extracted file lands at the umask default -- which silently
            # strips the executable bit off the very binaries being installed.
            if m.isfile():
                os.chmod(os.path.join(dest, m.name), m.mode)
            if m.isfile() or m.issym():
                names.append(m.name)
    return names


def parse_index(blob):
    """
    APKINDEX is itself in the apk container format.  Returns:
      by_name:    package name    -> "name-version.apk"
      by_soname:  "libfoo.so.1"   -> "name-version.apk"
    """
    index = None
    # Same two hazards as untar_apk: decompress every gzip member first, then
    # read through tar segments that may or may not carry their terminator.
    with tarfile.open(fileobj=io.BytesIO(gzip.decompress(blob)), mode="r:",
                      ignore_zeros=True) as tf:
        for m in tf:
            if m.name == "APKINDEX":
                index = tf.extractfile(m).read().decode("utf-8", "replace")
                break
    if index is None:
        sys.exit("error: APKINDEX not found inside APKINDEX.tar.gz")

    by_name, by_soname = {}, {}
    for record in index.split("\n\n"):
        f = {}
        for line in record.splitlines():
            if len(line) > 2 and line[1] == ":":
                f.setdefault(line[0], []).append(line[2:])
        name = f.get("P", [None])[0]
        ver = f.get("V", [None])[0]
        if not name or not ver:
            continue
        fn = f"{name}-{ver}.apk"
        by_name[name] = fn
        # "p:" is the provides list; shared libraries appear as so:NAME=VERSION.
        for prov in " ".join(f.get("p", [])).split():
            if prov.startswith("so:"):
                by_soname[prov[3:].split("=")[0]] = fn
    return by_name, by_soname


def elf_needs(path):
    """DT_NEEDED entries plus the program interpreter, read out of the ELF.

    readelf parses a foreign architecture without complaint, which is the whole
    point: this tells us what the binary demands without running it."""
    try:
        out = subprocess.run(["readelf", "-dl", path], capture_output=True,
                             text=True, timeout=30).stdout
    except Exception:
        return []
    needs = re.findall(r"Shared library: \[(.+?)\]", out)
    interp = re.findall(r"program interpreter: (.+?)\]", out)
    return needs + interp


def is_elf(path):
    try:
        with open(path, "rb") as fh:
            return fh.read(4) == b"\x7fELF"
    except OSError:
        return False


def satisfied(soname, roots):
    """A soname counts as present if any search root has it under lib/ or usr/lib/,
    or -- for the absolute path of the ELF interpreter -- at that exact path."""
    for root in roots:
        if soname.startswith("/"):
            if os.path.exists(os.path.join(root, soname.lstrip("/"))):
                return True
        else:
            for d in ("lib", "usr/lib"):
                if os.path.exists(os.path.join(root, d, soname)):
                    return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True, help="e.g. https://dl-cdn.../v3.24/main/armhf")
    ap.add_argument("--dest", required=True, help="directory to unpack into")
    ap.add_argument("--rootfs", required=True, help="existing rootfs, to know what is already there")
    ap.add_argument("--max-rounds", type=int, default=10)
    ap.add_argument("packages", nargs="+")
    args = ap.parse_args()

    if os.path.isdir(args.dest):
        shutil.rmtree(args.dest)
    os.makedirs(args.dest)

    print(f"==> index: {args.repo}/APKINDEX.tar.gz", flush=True)
    by_name, by_soname = parse_index(fetch(f"{args.repo}/APKINDEX.tar.gz"))
    log(f"{len(by_name)} packages, {len(by_soname)} sonames")

    wanted, done = list(args.packages), set()

    for rnd in range(args.max_rounds):
        for pkg in wanted:
            if pkg in done:
                continue
            fn = by_name.get(pkg)
            if not fn:
                sys.exit(f"error: no package named {pkg!r} in the index")
            print(f"==> {fn}", flush=True)
            for n in untar_apk(fetch(f"{args.repo}/{fn}"), args.dest):
                log(n)
            done.add(pkg)

        # Everything unpacked so far, plus the rootfs, is what a dependency can
        # be satisfied from.
        roots = [args.dest, args.rootfs]
        missing = set()
        for dirpath, _, files in os.walk(args.dest):
            for f in files:
                p = os.path.join(dirpath, f)
                if os.path.islink(p) or not is_elf(p):
                    continue
                for need in elf_needs(p):
                    if not satisfied(need, roots):
                        missing.add(need)

        if not missing:
            print("==> all shared-library dependencies satisfied", flush=True)
            return 0

        print(f"==> unsatisfied: {' '.join(sorted(missing))}", flush=True)
        wanted = []
        for so in sorted(missing):
            fn = by_soname.get(so)
            if not fn:
                sys.exit(f"error: nothing in the index provides {so!r}. "
                         f"Fetch it by hand or drop the package that wants it.")
            pkg = fn.rsplit("-", 2)[0]      # name-version-rrel.apk
            if pkg in done:
                sys.exit(f"error: {pkg} is already unpacked but {so} is still "
                         f"missing -- the index and the package disagree.")
            log(f"{so} -> {pkg}")
            wanted.append(pkg)

    sys.exit(f"error: dependencies still unresolved after {args.max_rounds} rounds")


if __name__ == "__main__":
    sys.exit(main())
