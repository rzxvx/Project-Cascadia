# P105 touch clock RE — iBEC `function-clock_enable` / Cmwp

Analysis target: bring up SPI1 + grape-clk from bare-metal `touch-cursor` in Recovery.

## ADT blobs (multi-touch @ `/device-tree/arm-io/spi1/multi-touch`)

| Property | Hex | Meaning |
|----------|-----|---------|
| `function-clock_enable` | `00000035436d7770` | phandle **889192448** → `/arm-io/pwm/grape-clk` + fourcc **`Cmwp`** |
| `function-clock_enable-pmu` | `000000284f4950470000000001010002` | PMU **OIPG**: reg `0x0200←0x01`, `0x0201←0x02` |
| `function-power_ana` | `000000284c756d700c020000` | PMU **Lump** reg `0x020c` |
| `function-power_ldo` | `000000284c756d700c020000` | PMU **Lump** reg `0x0213` |

**8-byte `function-clock_enable`** is *not* OIPG — it is phandle + fourcc token. iBoot resolves
`grape-clk` and dispatches the **`Cmwp`** ARM-function handler.

## iBEC RE (`ibec/iBEC.patched`, load **0x9FF00000**)

Run: `python3 scripts/re_clock_enable.py` → `build/re_clock_enable_summary.txt`

| Finding | Result |
|---------|--------|
| String `function-clock_enable` | file **0x3cda5** / **0x3cdbb** (`-pmu`) |
| Fourcc **`Cmwp` (0x706D7743)** | **0 hits** in iBEC/iBSS |
| `grape-clk`, `SPI1-CLK`, `device-clocks` strings | **absent** from iBEC |
| PWM **0x33500300** / SPI **0x32100000** literals | **0 hits** |
| **`clock_set`** helper | file **0x2f54**, VA **0x9ff02f54** — OR **0x180** enable bits via PMGR table **0x9ff42160** |
| **`clock_gate_switch`** | file **~0x1f1ec**, VA **0x9ff1f1ed** — `0x3F101008 + gate*4`, OR/BIC **0xF**, poll |

**Conclusion:** **`Cmwp` lives in XNU** (AppleS5L8940XIO / PWM provider), not in Recovery iBEC.
iBEC only has generic PMGR gate/clock helpers + DT property name strings.

Search XNU: `python3 scripts/search_cmwp_kernel.py`

## Recovery experiment (Aug 2026)

PMGR reads show enabled (`gate=7DE`, `g68=2FF`, `c04/c07/cpw` have **0x180** bits) but **PWM TCON**
and **SPI1 MMIO** stay **0** — gate/clock register pokes alone do not power the blocks.

## `touch-cursor` implementation (Path B)

1. **`mt_ibec_clocks()`** — call resident iBEC helpers before local PMGR poke:
   - `clock_set` @ **0x9FF02F55** for clk IDs **286, 304, 307, 4**
   - `gate_switch` @ **0x9FF1F1ED** for gates **80, 68, 83**
2. Screen line **`ibc:`** — want **`1BEC001F`** (image OK + 7 calls); **`BAD00001`** = iBEC image gone
3. Still run local `mt_clocks_on()`, `mt_grape_clk_on()` (Samsung PWM @ **0x33500300** ch **2**)

## Next RE if `spi`/`pwm` still 0

1. Find **`Cmwp`** handler in **kernelcache** / AppleS5L8940X kext (PIC — no absolute literals)
2. Trace iBEC **device start** @ **0x9ff1f258** (calls **0x9ff1770c** = DT getProperty, loops gates/clks)
3. LIK ramdisk peek: compare **0x33500300** / **0x32100000** after `bootx` vs Recovery
