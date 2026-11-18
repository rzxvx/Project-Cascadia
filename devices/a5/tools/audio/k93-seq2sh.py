#!/usr/bin/env python3
"""k93-seq2sh.py -- the CS42L63 sequences (k93-cs42l63-sequences.py) as shell
commands for the iPad 2, for bring-up by hand before the kernel driver:

    python3 tools/audio/k93-seq2sh.py init_cold rate_48000 speaker_start > x.sh

codec writes go through i2cset on I2C0 0x4a; codec_reset is PMU GPIO 13
(D1946 register 0x6e: 0x08 low, 0x0a high, the values seen on the two boot
paths); spkr_mute is SoC GPIO 39 (pad 0x3fa0009c: 0x202 low, 0x203 high).
"""
import importlib.util, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("seq", os.path.join(HERE, "k93-cs42l63-sequences.py"))
seq = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seq)
SEQ = getattr(seq, "SEQUENCES", None) or getattr(seq, "sequences", None)
if SEQ is None:
    SEQ = {k: v for k, v in vars(seq).items() if isinstance(v, dict) and "steps" in v}

def emit(name):
    print("# ---- %s: %s" % (name, SEQ[name].get("title", "")))
    for s in SEQ[name]["steps"]:
        op = s["op"]
        if op == "write":
            print("i2cset -y -f 0 0x4a 0x%02x 0x%02x" % (s["reg"], s["val"]))
        elif op == "update":
            print("v=$(i2cget -y -f 0 0x4a 0x%02x); i2cset -y -f 0 0x4a 0x%02x $(( (v & ~0x%02x) | 0x%02x ))"
                  % (s["reg"], s["reg"], s["mask"], s["val"] & s["mask"]))
        elif op == "read":
            print('echo "  codec 0x%02x = $(i2cget -y -f 0 0x4a 0x%02x)"' % (s["reg"], s["reg"]))
        elif op == "sleep_ms":
            print("usleep %d" % (s["ms"] * 1000))
        elif op == "gpio":
            if s["name"] == "codec_reset":
                print("i2cset -y -f 0 0x3c 0x6e %s" % ("0x0a" if s["level"] else "0x08"))
            elif s["name"] == "spkr_mute":
                print("/tmp/audmmio 'w 3fa0009c %s' >/dev/null" % ("203" if s["level"] else "202"))
            else:
                raise SystemExit("unknown gpio %s" % s["name"])
        elif op == "mclk":
            print("# MCLK %s (%s Hz): set up before" % ("on" if s.get("on") else "off", s.get("hz")))
        else:
            raise SystemExit("unknown op %s" % op)

for n in sys.argv[1:]:
    emit(n)
