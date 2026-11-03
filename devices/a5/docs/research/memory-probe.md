# Memory probe over USB — P105AP

## Two execution windows

You are correct: after **patched iBSS / payload** you can run arbitrary ARM
code (VRAM sweep crashed the iPad — that proves MMIO access from our code).

But **getting memory back over USB** depends on *which* USB stack is active:

| Window | When | USB mem read? | How |
|--------|------|---------------|-----|
| **A: Pwned DFU** | Pico pwn, **before** primepwn | **Yes** (if shellcode active) | `memdump8942.py` — checkm8 USB protocol |
| **B: iBSS running** | After primepwn, before iBEC | No standard tool | Custom iBSS dumper (not built yet) |
| **C: Recovery iBEC** | After iBEC upload | **No** | No `md`/`mem` command; `go` no-ops |

**Important:** Run `primepwn` **after** memdump. Sending iBSS replaces the bootrom
USB handler — memory read over USB usually stops working.

## Window A result (P105 Pico checkm8) — CLOSED

`p105-memdump/SUMMARY.txt` (2026-08-27): **every** region is
`bad0bad0` (all 256/64 words). Device enumerates PWND, but the
**ipwndfu-style USB `memc` read protocol is not installed** by this Pico
pwn. `memdump8942.py` cannot dump SecureROM/DRAM over USB here.

Do **not** retry `--map` expecting data. Next options:

1. **FB dumper** under Recovery (`aic1-lab`) — read candidate PAs, show hex
2. **iBSS stash** — early iBSS copies ROM window → DRAM; lab displays it
3. **Path D** — `checkm8_bootkit` (macOS) if that build exposes USB exec

---

```bash
# 1. Pico pwn → DFU, verify:
sudo irecovery -q    # PWND: checkm8, MODE: DFU

# 2. BEFORE primepwn:
pip install pyusb    # once
sudo python3 scripts/memdump8942.py --map -o /tmp/p105-memdump

# 3. Single word:
sudo python3 scripts/memdump8942.py --read 0x9f6fc000

# 4. Then continue normal boot:
sudo ./primepwn ...
```

Or: `sudo bash flashdrive/probe-memory.sh`

If reads return `DEADBEEF` / all fail → Pico pwn may not install ipwndfu-style
USB exec (only primepwn path). Then we need **custom iBSS memdumper** (below).

## What Window A gives you

- First 4 KiB of each candidate region as hex + `.bin` files
- Confirms **vram** base, **loadaddr** staging, **DRAM** contents
- Lets you pick `go` target addresses from real data instead of guessing

Default regions in `memdump8942.py`:

- `0x80000000` DRAM  
- `0x90000000` iBEC upload staging  
- `0x9f6fc000` / `0x9fc00000` framebuffer candidates  
- `0x32500000` UART  
- `0x34000000` bootrom buffer (checkm8 loadaddr)  
- `0x3fd00000` PERIPHBASE guess  

## Window B/C — USB transfer without crashing

### Why USB disappears today

The autogo trampoline calls **jump_helper** @ `0x9ff1f504` before your payload runs.
That helper **always** calls:

1. `0x9ff1f8dc` — USB quiesce / teardown  
2. `0x9ff0c16c` — display/backlight off  
3. then `blx` to staging @ `0x90000000`

So by the time staging-loader runs, the USB stack is dead — host sees disconnect.

### Safe execution model

```
Host bulk-OUT (memprobe.bin)
        │
        ▼
iBEC USB completion handler  ← USB stack STILL LIVE here
        │
        ▼
autogo hook @ file 0x22588
        │
        ▼
trampoline: direct bx r4      ← skip jump_helper (patch_autogo_direct_jump.py)
        │
        ▼
memprobe @ 0x90000000        ← read-only, WDT off, WFI halt at end
        │
        ├── UART hex dump (v1, always safe)
        ├── optional 4-pixel FB marker (read-test first)
        └── v2: bl iBEC "send over usb" (getenv path) — no raw OTG
```

**Do not:**

- Call jump_helper (r0=3 path) — kills USB before payload  
- Bitbang USB OTG registers from payload — different clock/domain, instant panic  
- Sweep VRAM — proved crash on P105  
- Tear down MMU/cache — iBEC still owns the machine  

**Do:**

- Run **inside** the bulk-complete callback via **direct `bx r4`**  
- Keep payload **small and synchronous** (< few ms)  
- **Read-only** DRAM/MMIO probes (`ldr` only, one word per region)  
- **WFI halt** at end — no return to iBEC boot path, no Linux jump yet  
- Disable WDT only (same as staging-loader)  

### USB egress options (ranked)

| Method | Crash risk | Host tool | Status |
|--------|------------|-----------|--------|
| **UART @ 0x32500000** | Low | serial 115200 / DCSD | **v1 memprobe** |
| **iBEC USB send API** | Low | `memrecv8942.py` on PID 1281 | planned — RE `"get environment variable over usb"` |
| **USB serial side-channel** | Low | `labrecv8942.py` | **v68 aic1-lab** — patch `CPID:`/`SRTG:` then return to USB |
| **Raw USB OTG** | **High** | — | **never** |
| **jump_helper path** | USB dead | — | **never** |
| **iBSS cpu1 park patch** | Reboot to iOS | — | **never** (sig-only iBSS) |

v68 aic1-lab: autogo uses **blx** (call-return). Lab publishes `LAB:flag,tramp,plant,tral,cbar`
into iBEC USB serial RAM and returns so Recovery stays PID 1281. Host:

```bash
sudo python3 scripts/labrecv8942.py
# or after boot-aic1-lab.sh
```

DRAM mailbox at `0x800E0000` (`C0DE05B1` + words) for a future memrecv path.

v1 memprobe ships UART-only so you get data even if USB IN path is silent on P105 (getenv
is often silent over USB on this board).

Build and run:

```bash
./scripts/build-memprobe.sh
# autogo iBEC with direct-jump patch, then:
sudo irecovery -f build/out/memprobe.bin
# read serial @ 115200 — expect [P05A-memprobe] lines
```

### What we know from hardware

- **Autogo + jump_helper** → backlight off, USB gone → code ran but no host I/O  
- **Autogo + direct bx r4** → payload runs, USB may stay up briefly  
- **VRAM sweep** → crash → use read-only probes only  
- **`irecovery -c go`** on LIK iBEC → stays PID 1281 → ignored on P105  

## iBEC “memory map” string

iBEC contains `Final physical memory region layout:` — that prints during
**bootx** / iOS kernel load, not via a standalone `mem` command. LIK verbose
SSH ramdisk may show it on screen during boot; it is not available through
`irecovery -c` on P105.

## macOS VM role

Only needed for:

- `checkm8_bootkit` Mac build (boot raw iBSS, KBAG decrypt)
- Optional Xcode Mach-O tooling

**Not** required for `memdump8942.py` on your Linux laptop.

## Recommended order

1. Window A memdump (one pwn session, no primepwn yet)  
2. Analyze `/tmp/p105-memdump/*.bin` for vram magic, loadaddr content  
3. Adjust DTB / jump addresses from measured values  
4. If Window A fails → build iBSS-embedded USB dumper (Path B)  

## Relation to Linux boot

Memory probe is **orthogonal** to iBoot/XNU/Linux userspace:

- Find correct **FB address**, **loadaddr**, **PERIPHBASE**  
- Prove **`go` target** before jumping zImage  
- Avoid blind VRAM sweeps that crash instantly  

Once addresses are confirmed, retry minimal payload (read-only FB marker or
staging-loader) with autogo iBEC — the execution path that actually ran.
