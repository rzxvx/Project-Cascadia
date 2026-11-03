#!/usr/bin/env python3
"""Makes a 20E247 iBSS (n104, iBoot-8422.100.650) that boots its kernel with
/chosen/debug-enabled = 1 and our boot-args, so the kernel honours
ApplePMGR's pmgr-debug from the start (gpu.md, G4: iOS's own log of a GPU
power-up).

    debug-ibss.py IN OUT [BOOT-ARGS]

IN is either
  - surrealra1n's tethered iOS boot image (iBSS.boot: iBoot64Patcher2
    -b "-v" + iBootpatch2, boots iOS from the NVMe, sent with
    usbliter8ctl), or
  - Cascadia's inputs/iBSS.n104.20E247.sigpatched.raw (byte for byte
    surrealra1n's iBSS.patch: stays in Recovery, for boot-from-recovery.sh).
    The boot-args pointer is redirected to a cave as iBoot64Patcher2 -b does.

Patches, each checked against the expected bytes first:
  0x29bf4  cbz w0 after security_allow_modes(0x20) -> nop: iBoot sets
           debug-enabled whatever the fuses say (the store at 0x29c00
           takes the property pointer from the stack, not w0)
  0x2cc54  adr x2, <default boot-args at 0xf4306> -> adr x2, 0xaf020
           (already so in iBSS.boot)
  0xaf020  the boot-args string, in a 2 KB run of zeros (iBoot64Patcher2's
           cave; iBSS.boot has "-v" there)
"""
import struct
import sys

DEBUG_CBZ = 0x29BF4
ARGS_ADR = 0x2CC54
BOOT_ARGS = 0xAF020
NOP = 0xD503201F
ADR_STOCK = 0x5063B582     # adr x2, #0xf4306
ADR_CAVE = 0x10411E62      # adr x2, #0xaf020
DEFAULT_ARGS = "-v serial=3 pmgr-debug=0x880e"

if len(sys.argv) not in (3, 4):
    sys.exit(__doc__)
d = bytearray(open(sys.argv[1], "rb").read())
args = (sys.argv[3] if len(sys.argv) == 4 else DEFAULT_ARGS).encode()

if b"iBoot-8422.100.650" not in d[:0x400] or b"for n104" not in d[:0x400]:
    sys.exit("not the n104 iBoot-8422.100.650 image")
w = struct.unpack_from("<I", d, DEBUG_CBZ)[0]
# cbz w0, 0x29c04 (+0x10)
if w != 0x34000000 | (0x10 >> 2) << 5:
    sys.exit(f"0x{DEBUG_CBZ:x}: 0x{w:08x}, not the expected cbz w0")
adr = struct.unpack_from("<I", d, ARGS_ADR)[0]
old = bytes(d[BOOT_ARGS:BOOT_ARGS + 3])
if adr == ADR_CAVE and old == b"-v\0":
    kind = "iBSS.boot"
elif adr == ADR_STOCK and not any(d[BOOT_ARGS:BOOT_ARGS + 3]):
    kind = "Recovery iBSS"
else:
    sys.exit(f"0x{ARGS_ADR:x}: 0x{adr:08x} / 0x{BOOT_ARGS:x}: {old!r}: unknown boot-args layout")
# iBoot takes boot-args shorter than 0x260 (cmp w0, #0x260 at 0x2cc60)
if len(args) >= 0x100 or any(d[BOOT_ARGS + 3:BOOT_ARGS + len(args) + 1]):
    sys.exit("boot-args too long")

struct.pack_into("<I", d, DEBUG_CBZ, NOP)
struct.pack_into("<I", d, ARGS_ADR, ADR_CAVE)
d[BOOT_ARGS:BOOT_ARGS + len(args) + 1] = args + b"\0"
open(sys.argv[2], "wb").write(d)
print(f"{sys.argv[2]} ({kind}): debug-enabled forced, boot-args \"{args.decode()}\"")
