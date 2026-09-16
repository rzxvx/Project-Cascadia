#!/usr/bin/env bash
# UART capture (background) + checkm8 boot chain.  Needs sudo and the iPad in DFU.
set -euo pipefail
cd ~/Desktop/ipad-mini-linux/ibootfiles

LOG=~/Desktop/ipad-mini-linux/uart_rearm.txt
echo "==> UART capture -> $LOG (240 s; the 19MB irecovery upload eats most of a 90s window)"
python3 -c "
import serial, time, sys
s = serial.Serial('/dev/cu.usbserial-A506VZ9Q', 115200, timeout=0.2)
end = time.time() + 240
buf = b''
while time.time() < end:
    c = s.read(65536)
    if c:
        buf += c
        try:
            sys.stdout.write(c.decode('utf-8','replace')); sys.stdout.flush()
        except Exception:
            pass
open('$LOG','wb').write(buf)
s.close()" &
CAP=$!
sleep 2

echo "==> boot chain"
sudo /Users/k/Legacy-iOS-Kit/bin/macos/arm64/primepwn iBSS.patched;  sleep 1
sudo irecovery -f iBEC.patched.autogo.lk.dfu;                         sleep 1
sudo irecovery -f ../output/staging-bundle.bin;                       sleep 1
sudo irecovery -f ../output/staging-loader.bin

wait $CAP
echo
echo "==> verdict lines"
grep -E "AIC1-REARM|LATE-SMOKE|AIC1-DIAG" "$LOG" || echo "  (none captured -- check the framebuffer)"
