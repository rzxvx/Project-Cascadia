#!/bin/sh
# spk-tone.sh -- the speakers by hand: MCA0 as clock master on pin1, both
# CS35L19 amps set up the way iOS does it, a tone fed to MCA0's FIFO by PIO.
#
#   sh spk-tone.sh [HZ] [SECONDS] [VOLUME]
#
# Research tool for docs/research/p105-audio.md (needs /usr/local/bin/audmmio,
# tools/audio/audmmio.c); the kernel driver does the same in order.  VOLUME is
# the amps' reg 0x10, a digital volume in 0.5 dB steps: 0x00 is 0 dB, 0x18
# +12 dB, 0x34 -102 dB, 0x19 mute (iOS: volume 0..228 + 0x34); default -8 dB.
HZ=${1:-1000}
SECS=${2:-3}
VOL=${3:-0xf0}
M=/usr/local/bin/audmmio
set -e

nco() {	# nco N HZ -- PMGR NCO N to HZ, setNCOFrequency's sequence, Fref 342 MHz
	b=$((0x3f100110 + 16 * $1)); inc=$((2 * $2)); dec=$(((2 * $2 - 342000000) & 0xffffffff))
	$M "w $(printf %x $b) 90000000; p $(printf %x $b) 40000000 0
	    w $(printf %x $((b + 4))) $(printf %x $inc); p $(printf %x $b) 200 0
	    w $(printf %x $((b + 8))) $(printf %x $dec); p $(printf %x $b) 200 0
	    w $(printf %x $b) 90000400; p $(printf %x $b) 200 0
	    w $(printf %x $b) 90000c00; p $(printf %x $b) 200 0" >/dev/null
}

amp() { i2cset -y 0 $1 $2 $3; }
ampu() {	# ampu ADDR REG MASK VAL
	v=$(i2cget -y 0 $1 $2)
	i2cset -y 0 $1 $2 $(((v & ~$3) | $4))
}

echo "== clocks and power"
# AUDIO-CLK, the MCA and I2S1 power states
$M "w 3f100058 80000001; p 3f100058 40000000 0
    m 3f101140 10f f; p 3f101140 ff ff
    m 3f1010a4 10f f; p 3f1010a4 ff ff" >/dev/null
# MCLK for the amps: I2S1 as clock master from NCO1 at 12.000 MHz (iOS's
# function-mclk_control)
nco 1 12000000
# (TXCON as iOS's configure writes it: bit 7 = MCLK/BCLK ratio 1; other
# dividers leave the block without a bit clock)
$M "w 34191008 0; w 3419103c 1; w 34191004 03000081; w 34191030 1000; w 34191034 0
    w 34191000 1" >/dev/null
# the switch: pin1's data and frame clocks from MCA0 (reg1 = 0xf014), its
# MCK from I2S1 (reg 0x11 bit 0)
$M "w 3fa01004 f014; w 3fa01044 1; r 3fa01004; r 3fa01044"

echo "== keep-alive: GPIO 6 toggled every 20 ms, as iOS's timer does"
( while :; do $M "w 3fa00018 203"; usleep 20000; $M "w 3fa00018 202"; usleep 20000; done ) &
ALIVE=$!
trap 'kill $ALIVE 2>/dev/null' EXIT

echo "== amps out of reset (GPIO 5)"
$M "w 3fa00014 202"
usleep 2000 2>/dev/null || sleep 1
$M "w 3fa00014 203"
usleep 10000 2>/dev/null || sleep 1
for a in 0x40 0x41; do printf '%s: id %s %s %s rev %s\n' $a $(i2cget -y 0 $a 1) $(i2cget -y 0 $a 2) $(i2cget -y 0 $a 3) $(i2cget -y 0 $a 5); done

echo "== amp power-on (iOS 0x80af55d4)"
for a in 0x40 0x41; do
	amp $a 0x00 0x99; amp $a 0x45 0x3f; amp $a 0x46 0x05; amp $a 0x51 0xff
	amp $a 0x59 0x0a; amp $a 0x6b 0x20; amp $a 0x00 0x00
	amp $a 0x0b $((0x26 & 0x3f)); ampu $a 0x0c 0x03 0x02
done
for a in 0x40 0x41; do amp $a 0x06 0x80; done
for a in 0x40 0x41; do
	amp $a 0x07 0xf8
	ampu $a 0x08 0xc0 0x40; amp $a 0x09 0xff; amp $a 0x10 0x19; ampu $a 0x0f 0x10 0
	amp $a 0x11 0x1c; ampu $a 0x0e 0xcf 0xc4; ampu $a 0x12 0xf0 0x70
	amp $a 0x3a 0x8a; amp $a 0x3b 0x10; amp $a 0x3c 0x74; amp $a 0x3d 0x03
done
usleep 10000 2>/dev/null || sleep 1
for a in 0x40 0x41; do amp $a 0x06 0x00; done
usleep 8000 2>/dev/null || sleep 1
for a in 0x40 0x41; do
	echo "$a status: $(i2cget -y 0 $a 0x15) $(i2cget -y 0 $a 0x15) $(i2cget -y 0 $a 0x16) $(i2cget -y 0 $a 0x16)"
	amp $a 0x23 0x6a; amp $a 0x24 0x80; amp $a 0x25 0x30; amp $a 0x26 0x1e
	amp $a 0x27 0x02; amp $a 0x28 0x04; amp $a 0x29 0x0c; amp $a 0x2a 0x8a
	ampu $a 0x29 0x7f 0x14; amp $a 0x11 0x14
done

echo "== amp serial port: 2 x 32-bit slots, amp 0 left, amp 1 right, 48 kHz"
for a in 0x40 0x41; do
	for r in 0x2d 0x2e 0x2f 0x30 0x31 0x37 0x38; do amp $a $r 0x80; done
	for r in 0x32 0x33 0x34 0x35; do amp $a $r 0; done
	amp $a 0x0d 3; amp $a 0x08 0x49
done
amp 0x40 0x36 0x60
amp 0x41 0x36 0x64

echo "== MCA0: master, 2 x 32-bit slots, 32-bit samples, 48 kHz (NCO6 6.144 MHz)"
nco 6 6144000
$M "w 34196018 0; w 34196044 301; p 34196044 ffffffff 0
    w 34196010 0; w 3419601c d; w 34196030 d; w 34196008 0
    w 34196020 002f0004; w 34196034 002f0004; w 34196014 0; w 34196024 3; w 34196038 3
    w 34196018 208; w 3419602c 10a; w 34196010 1; w 34196040 b0003000; w 3419600c 0f401007
    w 34196018 209" | grep -v '^poll' || true

echo "== unmute ($VOL), tone $HZ Hz for $SECS s"
for a in 0x40 0x41; do amp $a 0x10 $VOL; done
$M "s 34196028 34196040 3f 10 $(printf %x $HZ) $(printf %x $((SECS * 1000000)))"
for a in 0x40 0x41; do amp $a 0x10 0x19; done
for a in 0x40 0x41; do
	echo "$a after: 06=$(i2cget -y 0 $a 6) 07=$(i2cget -y 0 $a 7) 15=$(i2cget -y 0 $a 0x15) 16=$(i2cget -y 0 $a 0x16) 17=$(i2cget -y 0 $a 0x17)"
done
$M "w 34196018 0; r 34196040"
