#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
brcmfw.py - turn Apple's Broadcom Wi-Fi firmware tree (iOS / macOS
/usr/share/firmware/wifi) into the file names Linux's brcmfmac asks for.

  brcmfw.py SRC OUTDIR [PLATFORM]     e.g. brcmfw.py wifi out moana

Adapted from asahi-installer's asahi_firmware/wifi.py (Copyright The Asahi
Linux contributors, MIT): Apple names files by dimensions -- C (chip),
s (stepping), P (platform), M (module), V (vendor), m (version), A (antenna)
-- in directory and file names; brcmfmac asks for
brcm/brcmfmac<chip><stepping>-pcie.apple,<platform>-<module>-<vendor>-<...>.
<ext>, and falls back along the dimensions, so a node whose leaves are all
the same file is collapsed into it. PLATFORM keeps only that platform
(the ADT's wlan module-instance; "moana" on the iPhone 11).
"""
import os
import sys

EXTMAP = {"trx": "bin", "txt": "txt", "clmb": "clm_blob", "txcb": "txcap_blob", "sig": "sig"}
DIMS = ["C", "s", "P", "M", "V", "m", "A"]


class Node:
    def __init__(self):
        self.this = None        # (relpath, data)
        self.leaves = {}

    def key(self):
        return (self.this[1] if self.this else None,
                tuple((k, v.key()) for k, v in sorted(self.leaves.items())))


def nvram(data):
    lines = []
    for line in data.decode("ascii").split("\n"):
        if line:
            k, v = line.split("=", 1)
            lines.append(f"{k.strip()}={v}\n")
    return "".join(lines).encode("ascii")


def load(src, platform):
    root = Node()
    for dirpath, dirnames, filenames in os.walk(src):
        for skip in ("perf", "assert"):
            if skip in dirnames:
                dirnames.remove(skip)
        sub = os.path.relpath(dirpath, src)
        for name in sorted(filenames):
            if not any(name.endswith("." + e) for e in EXTMAP) or "_gen" in name:
                continue
            path = os.path.join(dirpath, name)
            if not name.endswith(".txt"):
                name = "P-" + name
            idpath, ext = os.path.join(sub, name).rsplit(".", 1)
            props = {}
            for part in idpath.replace("/", "_").split("_"):
                if not part or "-" not in part:
                    continue
                k, v = part.split("-", 1)
                if k == "P" and "-" in v:
                    props["P"], props["A"] = v.split("-", 1)
                else:
                    props[k] = v
            if platform and props.get("P") != platform:
                continue
            ident = [ext] + [props.pop(d) for d in DIMS if d in props]
            if props:
                continue                # a name we do not understand
            node = root
            for k in ident:
                node = node.leaves.setdefault(k, Node())
            data = open(path, "rb").read()
            if ext == "txt":
                data = nvram(data)
            node.this = (os.path.join(sub, name), data)
    return root


def prune(node, depth=0):
    for leaf in node.leaves.values():
        prune(leaf, depth + 1)
    if node.this is None and node.leaves and depth > 3:
        first = next(iter(node.leaves.values()))
        if all(l.key() == first.key() for l in node.leaves.values()):
            node.this = first.this
    if node.this and all(l.this and l.this[1] == node.this[1] for l in node.leaves.values()):
        node.leaves = {}


def walk(node, ident):
    if node.this is not None:
        yield ident, node.this
    for k, sub in node.leaves.items():
        yield from walk(sub, ident + [k])


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    root = load(sys.argv[1], sys.argv[3] if len(sys.argv) == 4 else None)
    prune(root)
    n = 0
    for ident, (rel, data) in walk(root, []):
        if len(ident) < 3:
            continue
        (ext, chip, rev), rest = ident[:3], ident[3:]
        name = f"brcmfmac{chip}{rev.lower()}-pcie.apple" + ("," + "-".join(rest) if rest else "") + "." + EXTMAP[ext]
        out = os.path.join(sys.argv[2], "brcm", name)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        open(out, "wb").write(data)
        print(f"{name:72} {len(data):8}  <- {rel}")
        n += 1
    if not n:
        raise SystemExit("no firmware found")


if __name__ == "__main__":
    main()
