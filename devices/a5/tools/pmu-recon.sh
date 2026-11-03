# Runs ON THE iPad, fed over ssh. Finds how we can reach the PMU (D2333 @ 0x3c
# on I2C0) registers -- to implement clock_enable-pmu (reg 0x0200<-0x01,
# 0x0201<-0x02) as a live test before touching the kernel.
echo "== regmap debugfs (the driver's own regmap, no bus conflict):"
ls -d /sys/kernel/debug/regmap/* 2>/dev/null || echo "   (none / debugfs not mounted)"
for d in /sys/kernel/debug/regmap/*; do
    [ -d "$d" ] || continue
    echo "   $d  name=$(cat "$d/name" 2>/dev/null)"
    echo "     has: $(ls "$d" 2>/dev/null | tr '\n' ' ')"
done
echo
echo "== i2c buses and devices:"
ls -d /sys/bus/i2c/devices/* 2>/dev/null | while read p; do
    echo "   $p  name=$(cat "$p/name" 2>/dev/null)"
done
echo "   /dev/i2c-*: $(ls /dev/i2c-* 2>/dev/null | tr '\n' ' ' || echo none)"
echo
echo "== i2c-tools present?"
for t in i2cget i2cset i2cdetect i2ctransfer; do
    command -v $t >/dev/null 2>&1 && echo "   $t: yes" || echo "   $t: no"
done
echo
echo "== debugfs mounted?"; mount 2>/dev/null | grep -i debug || echo "   not mounted (try: mount -t debugfs none /sys/kernel/debug)"
