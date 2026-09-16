# XNU timer RE — P105 / A5 (option 2)

Goal: extract the MMIO sequence that makes the **Cortex-A9 global timer**
(`CBAR+0x200`) tick, by reverse-engineering iOS 8.4.1 (`xnu-2784.40.6`)
`kernelcache.p105.raw`.

Kernel artifact: `kernelcache/kernelcache.p105.raw` (or `KERNELCACHE=...`)  
(Mach-O armv7, `__TEXT` @ `0x80001000`)

## Critical finding (do not skip)

**XNU on this SoC does not program the A9 global timer.**

| Probe | Result |
|-------|--------|
| Literal `0x3E100200` (GT) in whole kernelcache | **0 hits** |
| Literal `0x3E100600` (PT) | **0 hits** |
| Literal `0x3E100000` (CBAR) | Only peripheral map tables (e.g. corecrypto) |
| `mach_absolute_time` | Hook via `cpu_data` function pointer; fallback reads cached fields |
| Timebase hardware | **AIC shared timestamps** (`AppleInterruptController`) |

So option 2 is **not** “find `str` to `GT_CTRL` in XNU.” It is:

1. Learn how XNU brings up **PMGR / clock gates / CPU** (`AppleS5L8940X`).
2. Check whether that path **side-effects PERIPHCLK** so GT starts ticking.
3. If yes → replay that path in pure ARM. If no → GT needs a path XNU never takes.

## How `mach_absolute_time` works

Stub chain:

```
_mach_absolute_time @ 0x800b5238
  -> stub @ 0x803433c4
  -> ARM code @ 0x800b2b28:

    mrc p15,0,ip,c13,c0,4      ; TPIDRPRW -> cpu_datap
    ldr r3, [ip, #0x4dc]
    ldr r2, [r3, #0x80]        ; timebase function pointer
    cmp r2, #0
    bxne r2                    ; ← platform installs this
    ; else read cached 64-bit time at [r3,#0x58]/[r3,#0x5c]
```

Platform installs the reader (AIC timestamp), not A9 GT.

## Kexts that matter

| Kext | Load | Role |
|------|------|------|
| `AppleARMPlatform` | `0x804a9000` +`0x47000` | PE, perf controller glue, WDT, `clock-gates` props |
| `AppleS5L8940X` | `0x80b8a000` +`0xa000` | **`_initPMGRState`**, `function-clock_gate`, device-clocks |
| `AppleInterruptController` | `0x80cb3000` +`0x4000` | AIC; `kAICIackVecTypeTimer`; `#shared-timestamps` |
| `AppleS5L8930X` | `0x80dec000` +`0xa000` | Related IO/WDT; `"clock speed does not match absolute time"` |

### AppleS5L8940X strings (PMGR fight)

- `AppleS5L8940XPerformanceController::_initPMGRState: … firmware-v-perf-states`
- `… firmware-p-perf-state`
- `… firmware-m-perf-states`
- `… cpu-clk-cfgs`
- `function-clock_gate` / `function-power_gate`
- `_enableDevicePowerGated: Power gate … enabled before clock was enabled`

These consume the same ADT props we already saw (`firmware-p-perf-state=5`, etc.).

### AppleInterruptController (actual timebase)

- `handleInterrupt: vectorType == kAICIackVecTypeTimer`
- `#shared-timestamps`
- `AICInterruptTimestampFunction`

## What bootx does (reminder)

`bootx` in iBEC only loads Mach-O + `jump_helper(r0=3)`. **No GT enable.**
Timers come from kext `start()` after XNU is running.

## Fight plan (pure ARM)

### Phase A — Prove whether XNU side-effects GT

Boot LIK SSH ramdisk (`boot-lik-sshrd.sh`). From a small armv7 tool (or
kloader stub), read:

```
CBAR     = mrc p15,4,c15,c0,0
GT_CTRL  = *(CBAR+0x208)
GT_CNT   = *(CBAR+0x200)  (sample twice)
```

- If GT ticks under XNU → PERIPHCLK was ungated by PMGR/AIC bring-up → **extract and replay that**.
- If GT still dead under XNU → XNU never enables it → option 2 cannot win GT via replay; need a different clock (AIC timestamp for Linux, or undiscovered PMGR bit).

### Phase B — Extract `_initPMGRState` + clock_gate

1. Disassemble `AppleS5L8940X` @ `0x80b8a000`.
2. Map `_initPMGRState` (perf-state / `cpu-clk-cfgs` / PMGR regs).
3. Map `function-clock_gate` callee (likely same formula as iBoot `0x3F101008+id*4`).
4. Produce a flat store list → `pongo/xnu_pmgr_replay.c`.

### Phase C — Replay in Recovery probe

Extend `cpu-freq` probe: run replay, then GT/PT/PMCCNTR again. Caches on.

## Scripts

- `scripts/xnu_timer_strings.py` — string/literal scan (copy from analysis)
- Kernel path: `kernelcache/kernelcache.p105.raw` (or `KERNELCACHE=...`)

## Recovery evidence (cpu-freq probes)

Through **v13** (checkm8 → Recovery payloads only):

- Gate-init replay, full `clock_gate_switch(0..78)`, SCU, cluster bringup → **GT/PT still dead**
- iBEC `clock_set` table @ `0x9FF42160` is **GPIO junk** (`b0=0x3FA00000`); helper correctly skipped
- Local OR of enable bits `0x180` @ `0x3F100040` → **write does not stick** (`before==after==80000001`)
- PMCCNTR + GIC + FB + GPIO work; A9 GT CTRL writes do not stick

**Working theory:** A9 PERIPHCLK / GT only comes up in a higher power domain that Recovery never enters — or XNU never enables A9 GT at all (AIC only). Next: static kext RE + **live SSHRD peek**.

## Status

- [x] Locate kernelcache Mach-O
- [x] Confirm GT literals absent
- [x] Identify AIC timebase + PMGR kext
- [x] Recovery probes v1–v13: GT dead; `0x180` non-sticky
- [ ] Live check: does GT tick under SSHRD/XNU?
- [ ] Full disasm of `_initPMGRState`
- [ ] Replay stub (only if live GT ticks or PMGR extract shows PERIPHCLK)
