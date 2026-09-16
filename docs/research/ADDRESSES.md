# P105AP (iPad mini 1) — critical addresses for Linux bring-up

Hardware: **P105AP** (iPad2,5), SoC **S5L8942X** (Apple A5). Boot chain: checkm8 → primepwn iBSS → Recovery iBEC → payload or Linux.

All addresses are **physical** unless noted as a load VA.

---

## Linux boot layout (DRAM)

| Symbol | Address | Notes |
|--------|---------|--------|
| DRAM base | `0x80000000` | Staging for iBEC bundle, zImage, initrd |
| zImage entry | `0x80008000` | ARM Linux kernel load address |
| DTB handoff | `0x90000000` | Device tree blob for kernel |
| Initrd (optional) | after zImage | Set by boot script / ACE bundle |

---

## Recovery / iBEC

| Symbol | Address | Notes |
|--------|---------|--------|
| iBEC load VA | `0x9FF00000` | Where patched iBEC runs in Recovery |
| iBEC `clock_set` | `0x9FF02F55` | PMGR clock enable helper |
| iBEC `gate_switch` | `0x9FF1F1ED` | PMGR gate switch helper |
| Staging bundle | `0x80000000` | `staging-bundle.bin` target |
| Touch FW bundle | `0x80100000` | Z2 firmware + payload bundle |

Boot artifact: **`iBEC.patched.autogo.direct.dfu`** (autogo EOF loader — `go` via irecovery does not work on P105 Recovery).

---

## Framebuffer (bare-metal UI)

| Symbol | Value |
|--------|--------|
| Scanout base | `0x9F6FC000` |
| Resolution | 768 × 1024 |
| Stride | 3072 bytes (BGRA8888) |

---

## MMIO map (A5 / P105)

| Block | Base | Use |
|-------|------|-----|
| GPIO | `0x3FA00000` | Reset, CS, ATTN |
| PMGR | `0x3F100000` | Clock gates |
| PMGR gates (iBoot style) | `0x3F101008 + gate_id×4` | Per-gate register |
| AIC | `0x3F200000` | Interrupt controller |
| UART0 | `0x32500000` | Serial debug |
| I2C0 (HW) | `0x33200000` | PMU / sensors |
| SPI1 (touch) | `0x32100000` | Z2 multitouch |
| PWM / grape-clk | `0x33500300` | Channel 2, 32768 Hz |

---

## PMGR gates & clocks (multitouch path)

| Resource | Gate ID | Clock ID(s) |
|----------|---------|-------------|
| SPI1 | 68 | 304, 307 |
| I2C0 | 80 | 286 |
| PWM (grape) | 83 | 4 |

Touch GPIO (ADT): reset **5**, CS **7**, ATTN **22**.  
PMU I2C addr **0x3c**, regs **0x020c** / **0x0213**.

---

## XNU / IOKit (kernel RE, not callable from bare-metal Recovery)

| Symbol | Address | Notes |
|--------|---------|--------|
| `Cmwp` handler | `0x804ba3e8` | grape-clk PWM path in kernelcache |
| `function-clock_enable` string in iBEC | file offset ~`0x3cda5` | ADT phandle → grape-clk |

iBEC exposes generic `clock_set` / `gate_switch`; **`Cmwp` fourcc is XNU-only** — bare-metal SPI/PWM may stay dead until kernel `bootx` or LIK ramdisk path.

---

## Device tree

| Item | Location |
|------|----------|
| Raw ADT JSON | `dts/apple-p105ap-raw.json` |
| Compiled DTB | `dtb/apple-p105ap.dtb` |
| Patches | `patches/*.patch` |

---

## Boot paths (scripts)

| Path | Script | Description |
|------|--------|-------------|
| ACE bundle | `flashdrive/boot-linux-ace.sh` | staging-bundle + autogo EOF |
| LIK ramdisk | `flashdrive/boot-linux-lik.sh` | Legacy-iOS-Kit saved IPSW + `bootx` |
| Touch probe | `flashdrive/boot-touch-cursor.sh` | Bare-metal Z2 upload UI |

Entry points from repo root:

```bash
./scripts/run-linux-ace.sh
./scripts/run-linux-lik.sh
```

See [docs/boot-strategy.md](boot-strategy.md) for strategy and [README.md](../README.md) for setup.
