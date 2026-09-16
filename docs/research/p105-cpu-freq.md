# P105 CPU / A9 timer bring-up

## Recovered from iBSS/iBEC (`clock_gate_switch`)

```
addr = 0x3F101008 + gate_id * 4
if addr > 0x3F101140: return   # gate_id 0..78 ONLY
ON:  *addr |= 0xF;  poll (target bits0-3 == actual bits4-7)
OFF: *addr &= ~0xF; same poll
```

ADT gates **121/122 are out of range** — they are not programmed by this function (so not the A9 timer path).

## v6 probe lines

| # | Field |
|---|--------|
| 0 | gate 68 (SPI1) @ correct base — expect low nibble `F` if ON |
| 1 | gate 72 (UART0) |
| 2 | PMGR+0x200 (expect `01010101`) |
| 3 | PMGR+0x204 |
| 4 | PMGR+0x208 |
| 5 | PMGR+0x6004 |
| 6 | GT_CTRL after cluster bring-up |
| 7 | GT delta |
| 8 | PT_CTRL |
| 9 | PT delta |
| 10 | clock_gate_switch(68,1) status (0=ok) |
| 11 | CBAR |
| 12 | PMGR+0x2104 after write |

Top bar green if GT or PT ticks. Baseline hex is painted **before** cluster writes.


## v7–v10 results (2026-08-23)

| Probe | Result |
|-------|--------|
| Gate-init @ `0x3F100038` (52 entries from iBSS) | OK (`ig0=90011044`, tmo=0) |
| `clock_gate_switch(0..78)` @ `0x3F101008` | 78/79 OK; does **not** ungate GT/PT |
| SCU enable (CBAR+0) | OK (`0x2D`) |
| PMGR+0x6000 poke | sticks (`100A0C01`) |
| A9 global timer (CBAR+0x200) | **DEAD** — CTRL writes don't stick |
| A9 private timer (CBAR+0x600) | **DEAD** |
| PMCCNTR (v10, PMCNTENSET bit31) | **LIVE** — delta `0x0657E8F2` over 2M nops |

**Conclusion:** In checkm8→Recovery payloads, PERIPHCLK to the MPCore timer block never comes up. CPU cycle counter works. Linux uses a **PMCCNTR clocksource** (`arch/arm/mach-apple/pmccntr.c` via `init_time`), not `arm,cortex-a9-global-timer`.

### v11–v13 (clock-ID / enable bits)

| Probe | Result |
|-------|--------|
| iBEC `clock_set` table @ `0x9FF42160` | Bank0 = GPIO `0x3FA00000` — helper correctly skipped |
| Local OR `0x180` @ `0x3F100040` | Write does **not** stick (`before==after`) |
| GT/PT | Still dead |

### Linux follow-up

- Primary clocksource: **PMCCNTR** (done in mach-apple).
- Secondary candidate: AIC timestamps `0x3F206008/0x600C` (what XNU uses).
- Path B (SSHRD→Linux jump) still needs a clocksource first — this is that prerequisite.
