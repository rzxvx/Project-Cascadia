# Can Linux boot on iPad mini 1 (P105) via checkm8?

Short answer: **checkm8 gets you code execution; Linux is still unconfirmed on your
hardware without a serial console.** The chain you have is the right one. Blank
backlight after `go` is not proof of failure.

## What checkm8 can and cannot do

| checkm8 alone | Does |
|---|---|
| Exploit bootrom (Pico 8942 UF2) | Yes — `PWND: checkm8` |
| Run pongoOS / interactive shell | **No** — AArch64 only, not on A5 |
| Boot Linux directly | **No** — still need iBSS → iBEC → kernel → jump |
| Leave NAND untouched | Yes |

checkm8 is step 0. Everything after is **irecovery uploads + iBEC `go`**.

## What your tests already proved

| Step | Your result | Verdict |
|---|---|---|
| Pico pwn | PWND: checkm8 | Bootrom exploited |
| primepwn iBSS | SRTG N/A (not rom) | iBSS ran |
| irecovery iBEC | Faster USB, Recovery | iBEC loaded |
| Kernel upload | 100% progress | Host sent ~11 MB |
| `go` | Backlight on, blank, no iOS | **Payload likely ran** (same as earlier 0x90008000 test) |
| probe printenv / bgcolor | No output, no red screen | **Normal** — P105 Recovery often ignores `-c` echo and does not drive LCD |
| verify md | No kernel magic | **Invalid test** — iBEC has no `md` command |

So you are **not** stuck at checkm8. You are stuck at **“did Linux actually start?”**
with no way to see output.

## Why iBEC seems “dead”

Recovery-mode iBEC on iOS 8 / P105 often:

- Accepts `irecovery -f` uploads (works for you)
- Does **not** print `getenv` / `printenv` replies over USB
- Does **not** change the panel for `bgcolor` (display owned by closed pipeline)
- After `go`, the host `irecovery` command **hangs or prints nothing** because iBEC is gone

That looks like “no response” but matches a **successful jump into RAM code**.

## Ways to actually boot Linux (realistic)

### A — Current path (checkm8 + irecovery) — **in progress**

```
Pico pwn → primepwn iBSS → irecovery iBEC → upload staging-bundle → go 0x90000000
```

Status: chain reaches iBEC; jump behavior consistent with execution. **Need UART**
to confirm kernel (expect `Uncompressing Linux...` or clocksource panic).

### B — Jailbreak + kloader (if iOS still bootable)

If the iPad still boots iOS with 3uTools/SSH: in-iOS kloader loads iBSS/iBEC and
hands off to host `irecovery`. That path was removed from this repo but **still works
on device** and avoids pwned-DFU iBSS upload issues. Requires jailbreak.

### C — checkra1n A5 beta

checkra1n betas included **32-bit A5** support — jailbreak payload, not Linux.
Proves post-checkm8 handoff works; does not load zImage.

### D — Custom 32-bit USB payload (not built yet)

`pongo/loader.S` + upload stub loaded straight from pwned bootrom — bypasses iBEC
entirely. Described in `pongo/README.md` as remaining work. Same kernel/DTB.

### E — pongoOS / kDFU app

**Do not work** on A5 for Linux (64-bit pongoOS; kDFU app = DFU only, PID 1227).

## What you need next (only thing that unblocks proof)

**UART serial** at **115200 8N1** on the iPad dock/debug pinout (same block Linux
uses: physical **0x32500000**, driver `earlycon=s3c6400,0x32500000` in DTB).

Without serial you are guessing from backlight alone.

Cheap CP2102/FT232 USB-TTL → read output after `go`.

## Quick checks (no serial)

```bash
# In Recovery, before go:
sudo ./status.sh          # note PID (want 1281)
lsusb | grep 05ac

# Upload + go:
sudo ./jump-kernel.sh direct

# Immediately on PC (new terminal), before unplugging iPad:
sudo ./status.sh          # PID changed or device gone => payload ran
```

If after `go` USB **disappears** or PID is no longer 1281, RAM code is running.

## Honest bottom line

- **Can checkm8 boot this iPad?** — It bootstraps the chain; it does not boot Linux alone.
- **Can Linux boot from RAM on P105?** — **Plausible, not proven** on your unit yet.
  Kernel + DTB + iBEC chain are built; handoff behaves like execution; **confirmation
  requires serial or a working framebuffer address from live iBoot.**
- **Is the project dead?** — No. You are at the **last mile**: prove kernel output,
  then fix DTB (clocksource / framebuffer) from what serial prints.

Exit stuck state: hold Home + Power ~10 s, or `sudo irecovery -n` in Recovery.
