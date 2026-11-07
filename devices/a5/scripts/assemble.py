#!/usr/bin/env python3
"""assemble.py -- the boot bundle from a kernel and this machine's own layer.

    assemble.py LOADER ZIMAGE_DTB OVERLAY_DIR OUT

LOADER is linux-boot.bin and ZIMAGE_DTB the kernel with its device tree
appended: the two halves ./cascadia a5 build makes from source, or a
prebuilt release ships -- a pair, since the loader is built knowing where
that DTB sits.  OVERLAY_DIR is what only this machine has: the firmware from
its IPSW, its ssh keys, the device's host key, the NFS root's address.

The bundle is the loader padded to 0x8000, the kernel (at 0x80008000 once
iBEC has loaded the bundle at 0x80000000), padding to 4K, and the overlay as
a newc cpio.  The loader finds the overlay's header at 0x7F00 and copies it
to OVERLAY_PHYS before the jump; linux,initrd-start/-end in the appended DTB
say where it is, and the kernel unpacks it over its built-in initramfs.
Python only: no docker, no kernel tree, seconds.
"""
import os
import stat
import struct
import sys

LOAD = 0x80000000
KERNEL_OFF = 0x8000
HDR_OFF = 0x7F00
MAGIC = 0x314C564F                 # "OVL1"
OVERLAY_PHYS = 0x88000000          # past the 46 MB kernel and the decompressor
OVERLAY_MAX = 0x08000000           # up to 0x90000000

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def fail(msg):
    sys.exit('assemble.py: ' + msg)


def cpio(root):
    """newc, every entry root's, parents before children, sorted"""
    out = bytearray()
    ino = [300000]

    def entry(name, mode, data=b'', mtime=0, nlink=1):
        ino[0] += 1
        hdr = '070701' + ''.join('%08X' % v for v in (
            ino[0], mode, 0, 0, nlink, mtime, len(data), 0, 0, 0, 0, len(name) + 1, 0))
        out.extend(hdr.encode())
        out.extend(name.encode() + b'\0')
        out.extend(b'\0' * (-len(out) % 4))
        out.extend(data)
        out.extend(b'\0' * (-len(out) % 4))

    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        rel = os.path.relpath(dirpath, root)
        if rel != '.':
            st = os.lstat(dirpath)
            entry(rel, stat.S_IFDIR | (st.st_mode & 0o7777), mtime=int(st.st_mtime), nlink=2)
        for f in sorted(files):
            p = os.path.join(dirpath, f)
            name = os.path.normpath(os.path.join(rel, f))
            st = os.lstat(p)
            if stat.S_ISLNK(st.st_mode):
                entry(name, stat.S_IFLNK | 0o777, os.readlink(p).encode(), int(st.st_mtime))
            elif stat.S_ISREG(st.st_mode):
                with open(p, 'rb') as fh:
                    entry(name, stat.S_IFREG | (st.st_mode & 0o7777), fh.read(), int(st.st_mtime))
    entry('TRAILER!!!', 0)
    out.extend(b'\0' * (-len(out) % 512))
    return bytes(out)


def patch_initrd(dtb, start, end):
    """linux,initrd-start/-end in /chosen, in place (both are there as one cell)"""
    magic, total, off_struct, off_strings = struct.unpack_from('>IIII', dtb, 0)
    if magic != 0xd00dfeed:
        fail('no DTB at the end of the kernel')
    strings = off_strings
    p, depth, path = off_struct, 0, []
    found = {}
    while True:
        tok, = struct.unpack_from('>I', dtb, p); p += 4
        if tok == FDT_BEGIN_NODE:
            e = dtb.index(b'\0', p)
            path.append(dtb[p:e].decode()); p = (e + 4) & ~3
        elif tok == FDT_END_NODE:
            path.pop()
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack_from('>II', dtb, p); p += 8
            e = dtb.index(b'\0', strings + nameoff)
            name = dtb[strings + nameoff:e].decode()
            if path == ['', 'chosen'] and name in ('linux,initrd-start', 'linux,initrd-end'):
                if ln != 4:
                    fail('%s is %d bytes, not one cell' % (name, ln))
                found[name] = p
            p = (p + ln + 3) & ~3
        elif tok == FDT_NOP:
            pass
        elif tok == FDT_END:
            break
        else:
            fail('a DTB token %d at %#x' % (tok, p - 4))
    if len(found) != 2:
        fail('the DTB has no /chosen linux,initrd-start and -end to patch')
    struct.pack_into('>I', dtb, found['linux,initrd-start'], start)
    struct.pack_into('>I', dtb, found['linux,initrd-end'], end)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    loader, zimage, overlay_dir, out = argv
    ld = open(loader, 'rb').read()
    if len(ld) > HDR_OFF:
        fail('the loader is %d bytes and would reach its own header at %#x' % (len(ld), HDR_OFF))
    kern = bytearray(open(zimage, 'rb').read())
    dtb_off = kern.rfind(b'\xd0\x0d\xfe\xed')
    if dtb_off < 0:
        fail('%s has no appended DTB' % zimage)
    dtb_len, = struct.unpack_from('>I', kern, dtb_off + 4)
    dtb = bytearray(kern[dtb_off:dtb_off + dtb_len])

    ovl = cpio(overlay_dir) if overlay_dir and os.path.isdir(overlay_dir) else b''
    if len(ovl) > OVERLAY_MAX:
        fail('the overlay is %d MB, more than the %d MB set aside' % (len(ovl) >> 20, OVERLAY_MAX >> 20))
    patch_initrd(dtb, OVERLAY_PHYS, OVERLAY_PHYS + len(ovl))
    kern[dtb_off:dtb_off + dtb_len] = dtb

    b = bytearray(ld) + bytearray(KERNEL_OFF - len(ld)) + kern
    b += bytearray(-len(b) % 4096)
    ovl_off = len(b)
    b += ovl
    if ovl:
        struct.pack_into('<IIII', b, HDR_OFF, MAGIC, ovl_off, len(ovl), OVERLAY_PHYS)
    if LOAD + len(b) > OVERLAY_PHYS:
        fail('the bundle (%d MB) would run into the overlay\'s place' % (len(b) >> 20))
    with open(out + '.part', 'wb') as f:
        f.write(b)
    os.replace(out + '.part', out)
    print('bundle: %s  kernel %.1f MB, overlay %d KB -> %#x' % (
        os.path.basename(out), len(kern) / 1048576, len(ovl) >> 10, OVERLAY_PHYS))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
