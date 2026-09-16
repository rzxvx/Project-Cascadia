# P105AP (iPad mini 1) GPIO / buttons / touch — ADT map

Absolute MMIO = `arm-io` base **`0x30000000`** + child `reg`.

## Controllers

| Peripheral | Child `reg` | Absolute | Size | ADT path |
|------------|-------------|----------|------|----------|
| GPIO | `0x0fa00000` | **`0x3FA00000`** | `0x1000` | `/device-tree/arm-io/gpio` |
| SPI1 (touch bus) | `0x02100000` | **`0x32100000`** | `0x1000` | `/device-tree/arm-io/spi1` |
| AIC | `0x0f200000` | `0x3F200000` | `0x10000` | `/device-tree/arm-io/aic` |
| PMGR | `0x0f100000` | `0x3F100000` | `0x7000` | `/device-tree/arm-io/pmgr` |

GPIO: compatible `gpio,s5l8940x`; `#gpio-ports = 28`; AIC IRQ **119**.

## Buttons (`/device-tree/buttons`)

Apple `function-button_*` props: phandle → `"GPIO"` → **pin** → flags.

**Polarity (live):** all SoC GPIO buttons are **pull-ups, active-low** — idle `1`, pressed `0`.

**Authoritative layout:** Apple-style `*(GPIO_BASE + 4*pin) & 1` matches the ADT pin numbers below.

| Button | ADT prop | GPIO pin | Live |
|--------|----------|----------|------|
| **menu (Home)** | `function-button_menu` | **0** | confirmed |
| **hold** | `function-button_hold` | **1** | confirmed |
| **volup** | `function-button_volup` | **2** | confirmed |
| **voldown** | `function-button_voldown` | **3** | confirmed |
| ringerab | `function-button_ringerab` | **4** | ADT only (may be unused) |
| halleffect | `function-button_halleffect` | PMU GPIO 13 | Not SoC GPIO |
| halleffect1 | `function-button_halleffect1` | PMU GPIO 12 | Not SoC GPIO |

Wake variants (`function-wake_button_menu`, `function-wake_button_hold`) go through **PMU**, not the SoC GPIO block.

### Samsung-style PDAT vs Apple pins

`gpio-poll` also showed `*(GPIO_BASE + pin*0x20 + 4) & 1`. That view is **offset** relative to Apple/ADT labels:

- Samsung line labeled **MENU** (index 0) tracks the **Apple HOLD** pin (ADT pin 1), not Home.

Prefer the Apple `base+4*pin` map for software. Samsung PDAT bit0-per-port is not a 1:1 label match for these buttons.

## Touch (`/device-tree/arm-io/spi1/multi-touch`)

| Item | Value |
|------|-------|
| Compatible | `multi-touch,p105` |
| Transport | SPI1 @ **`0x32100000`**, CS0 |
| SPI rate (ADT) | 5000–10000 kHz (use ≤5 MHz) |
| Attention IRQ | GPIO parent, pin **22** |
| Protocol | Apple Z2 — not a MMIO framebuffer |

Phase 2 after buttons: SPI register dump, then CS/reset GPIO + Z2.

## Live confirmation (`gpio-poll`) — Aug 2026

Boot (laptop flashdrive):

```bash
bash ~/flashdrive/boot-gpio-poll.sh
```

| Button | ADT / Apple pin | Confirmed | Notes |
|--------|-----------------|-----------|-------|
| menu | 0 | **yes** | Home; active-low |
| hold | 1 | **yes** | Side; active-low; Samsung MENU line aliases this |
| volup | 2 | **yes** | active-low |
| voldown | 3 | **yes** | active-low |
| ringerab | 4 | not tested | |

**Read recipe (use this):**

```c
uint32_t pressed = !(*(volatile uint32_t *)(0x3FA00000 + 4 * pin) & 1);
```



## Touch (`touch-cursor`) — Phase 2b

| Item | Value |
|------|-------|
| Reset GPIO | **5** (ADT `function-reset`, active-low) |
| CS GPIO | **7** |
| ATTN GPIO | **22** |
| FW in bundle | **`0x80100000`** (file offset `0x100000`) |

Build + bundle:

```bash
python3 scripts/extract-mt-firmware.py -o firmware/mt-p105.z2fw   # once
bash scripts/bundle-touch-cursor.sh
bash flashdrive/boot-touch-cursor.sh
```

On-screen hex (top→bottom):

| Line | Field |
|------|-------|
| 1 | `attn_lo` — ATTN active-low count |
| 2 | `frames_ok` |
| 3 | `frames_fail` |
| 4 | `phase \| bootAttn \| fwOk \| hasFw` |
| 5 | `fw_size` |
| 6 | `x<<16\|y` |
| 7 | `gpio22_raw` (orange) |
| 8 | `last_rx0` — want **`000000E1`** |
| 9 | `poll_n` |

**hasFw=0** → run `extract-mt-firmware.py` and rebundle. **bootAttn=0** after reset → chip dead or needs PMU LDO (I2C).

## Touch power — Phase 2c

Before reset + Z2 upload, `touch-cursor` now runs the ADT power sequence:

| Step | ADT property | Action |
|------|--------------|--------|
| 1 | `clock-gates` on spi1/i2c0/pwm | PMGR gates **68** (SPI1), **80** (I2C0), **83** (grape-clk) |
| 2 | `clock-ids` | PMGR clk **286**, **304**, **307**, **4** enable (`0x180`) |
| 3 | `function-power_ana` | I2C → PMU `@0x3c`, reg **`0x020c`**, enable bit |
| 4 | `function-power_ldo` | I2C → PMU `@0x3c`, reg **`0x0213`**, enable bit |

I2C bit-bang on GPIO **4** (SDA) / **5** (SCL) — same pin 5 as reset, so power runs while reset is released (high).

On-screen (new lines):

| Line | Field | Want |
|------|-------|------|
| `gate:` | PMGR gates+clocks | **`000000DE`**+ (bit 7 = grape-clk programmed) |
| `rst:` | reset path | **`8`** = FW ok; **`1`** = ATTN on assert; **`2`** = ATTN on release; **`4`** = upload after release |
| `pmu:` | LDO writes | **`00000003`** (ana + ldo ok) |
| `i2c:` | probe path | **`000002xx`** — bit 9 set = bitbang ACK (your `2F1`) |
| `g80:` | raw PMGR gate-80 reg | debug if `gate:` stuck at `1E` |

Reset sequence matches upstream **`apple_z2_boot`**: assert reset → wait ATTN (100 ms) → upload FW.

Then existing `stat:` should reach **`03010101`** (bootAttn=1, fwOk=1) and `rx0:` → **`E1`**.

`err:` is upload fail latch — stays **0 or 1** (not `poll:`).
