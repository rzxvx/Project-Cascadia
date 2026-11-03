# SSHRD live timer peek — P105 / A5

**Goal:** Under running XNU (LIK SSH ramdisk), see whether **A9 GT** ticks, or
only **AIC** timestamps (what `mach_absolute_time` uses).

This is **observation only** — not the Linux boot path.

## Static RE (already done)

See `docs/p105-xnu-pmgr-extract.md` and `scripts/xnu_pmgr_extract.py`.

| Fact | Detail |
|------|--------|
| A9 GT/PT literals in kernelcache | **0 hits** |
| `_initPMGRState` | **reads** PMGR only; no GT stores |
| Timebase | **AIC** phys `0x3F200000`, shared `+0x6008/+0x600c` |
| Clock gates | `0x3F101000 + id*4` (XNU), not iBoot `+0x1008` |

So iOS does **not** “enable GT in another power mode” in the kernelcache we have —
it never programs A9 GT. Live peek still matters: PMGR bring-up might **side-effect**
PERIPHCLK so GT ticks anyway.

## Boot SSHRD

On the **same machine that has the USB cable** (usually the laptop), pwned DFU first.

**Preferred (LIK itself):**

```bash
cd ~/Legacy-iOS-Kit
./restore.sh --device=iPad2,5 --sshrd --build-id=12H321
# LIK starts iproxy; password: alpine
ssh -p 6414 root@127.0.0.1
```

**Alt:** `~/flashdrive/boot-lik-sshrd.sh` then wait for Apple logo / USB re-enum.

### SSH “Connection refused” on `:6414`

That means **nothing is listening on that port on that host** — not “wrong password”.

Checklist (run on the machine with USB):

```bash
# 1) Is the device alive as an iOS USB device?
idevice_id -l
# or: system_profiler SPUSBDataType | grep -i apple   # macOS
# or: lsusb | grep -i apple                           # Linux

# 2) Is iproxy running?
pgrep -a iproxy
# if empty:
iproxy 6414 22 &
sleep 1
ss -ltnp | grep 6414 || netstat -ltnp 2>/dev/null | grep 6414

# 3) SSH only to the host where iproxy runs
#    WRONG: ssh from WSL to 127.0.0.1 while iproxy is on the laptop
#    RIGHT: ssh on the laptop (iproxy binds 127.0.0.1 only)
#
# P105 SSHRD offers only ssh-rsa — modern OpenSSH needs:
sshpass -p alpine ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa \
  -p 6414 root@127.0.0.1
# Transfer to /mnt1 (ramdisk root is often read-only):
#   scp ... -P 6414 file root@127.0.0.1:/mnt1/

# 4) If still refused: unplug/replug USB, kill old iproxy, restart
pkill -f 'iproxy 6414' || true
iproxy 6414 22 &
```

`boot-lik-sshrd.sh` often prints “SSH not up yet” even when `bootx` worked — kernel
needs ~30–90s, then USB must re-enumerate. Unplug/replug after the Apple logo is
normal.

## Peek (only after SSH works)

**Must run on the device**, not on the laptop. Running the script on Linux host
always prints `NOACCESS` (no phys `/dev/mem` for those addresses).

```bash
# on laptop, with SSH up:
scp -P 6414 ~/flashdrive/xnu_timer_peek.sh root@127.0.0.1:
ssh -p 6414 root@127.0.0.1 'sh xnu_timer_peek.sh'
```

Stock LIK SSHRD often has **no `/dev/mem` and no `devmem`**. Then every sample is
`NOACCESS` even though SSH works — that is a tooling gap, not a timer result.

If diagnostics show `/dev/mem: MISSING`, build the C tool on a Mac (or any Darwin
SDK) and scp the Mach-O:

```bash
xcrun -sdk iphoneos clang -arch armv7 -miphoneos-version-min=8.0 \
  -o xnu_timer_peek tools/xnu_timer_peek.c
ldid -S xnu_timer_peek
scp -P 6414 xnu_timer_peek root@127.0.0.1:
ssh -p 6414 root@127.0.0.1 ./xnu_timer_peek
```

## Addresses

| Name | Phys |
|------|------|
| CBAR | `0x3E100000` |
| GT count / ctrl | `+0x200` / `+0x208` |
| PT count / ctrl | `+0x604` / `+0x608` |
| AIC shared lo/hi | `0x3F206008` / `0x3F20600C` |
| AIC legacy lo/hi | `0x3F202048` / `0x3F20204C` |

## How to read the result

| Observation | Meaning for Linux |
|-------------|-------------------|
| **GT_DELTA ≠ 0** | PERIPHCLK live under XNU → extract PMGR side-effects and replay in Recovery |
| **GT dead, AIC advances** | Confirms static RE → use **AIC** or **PMCCNTR** clocksource; stop fighting A9 GT |
| **Both dead / NOACCESS** | Tooling gap (no phys access) or deeper map; fix access first |

## Recovery backdrop (v13)

`0x180` enable bits at `0x3F100040` **do not stick** in Recovery. GT CTRL does not
stick. PMCCNTR works. That is consistent with “never enter the domain XNU uses”
**or** “XNU never turns on A9 GT either.”
