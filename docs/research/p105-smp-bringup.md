# P105AP dual-core SMP — PMGR Core / AIC IPI / SCU

**Hardware:** S5L8942X A5 = **2× Cortex-A9** (not 4). ADT `#main-cpus = 2`.

## ADT contracts

| Hook | Target | Decode |
|------|--------|--------|
| cpu0 `function-enable_core` | PMGR | method `"Core"`, arg **1** |
| cpu1 `function-enable_core` | PMGR | method `"Core"`, arg **2**; `state = "waiting"` |

**XNU bringup path (kernelcache.p105.raw) — confirmed cpu10:**

```
processor_start → PE_cpu_start @0x8031A544
  → IOCPU vtable+0x350 = AppleARMCPU::startCPU @0x804AAF9C
       (vtable @0x804D0CE8 in AppleARMPlatform @0x804A9000)
```

`AppleARMCPU::startCPU` (waiting CPUs only — `this+0x85==0` means state≠`"running"`):

```
ldr   r0, [this, #0x7c]     ; AppleARMFunction* wrapper
movs  r1, #1
movs  r2, #0
movs  r3, #0
blx   [r0->vtable+0x3c]     ; invoke @0x804AB458
```

**cpu12 — invoke is not MMIO.** Wrapper `@0x804D10C8`:

| field | content |
|-------|---------|
| `+0x8` | provider IOService (PMGR) |
| `+0xc` | OSData = DT `function-enable_core` blob |
| `+0x10` | OSSymbol (property name; retained) |

`invoke` → `provider->callPlatformFunction(gSymbol?, false, OSData, 1, 0, 0)`
via **vtable+0x1e8**. Likely symbol **`callAppleARMFunction`**.

**ADT `function-enable_core` blob (from `src/devicetree_full.txt`):**

| CPU | raw hex (LE words) | decode |
|-----|---------|--------|
| cpu0 | `00 00 00 10 65 72 6f 43 01 00 00 00` | phandle **`0x10000000`** (= pmgr `AAPL,phandle`), FourCC **`'Core'`** (`0x436F7265`), arg **1** |
| cpu1 | `00 00 00 10 65 72 6f 43 02 00 00 00` | same + arg **2** |

So XNU SMP bringup is only: resolve that blob → `callAppleARMFunction(OSData)` →
PMGR method **`Core(n)`**. No `movw #0x6000` in XNU; MMIO lives entirely in
the PMGR/`Core` implementation (same contract as iBoot). **Lab focus returns to
iBoot/PMGR Core sequence**, not further AppleARMCPU RE.

**Next RE** — FourCC handler (not AppleARMCPU):

```bash
python3 scripts/xnu_smp_core_fourcc.py 2>&1 | tee build/xnu_smp_core_fourcc.txt
cat build/xnu_smp_core_fourcc_summary.txt
```

Looks for LE u32 **`0x436F7265`** (`'Core'`), movw/movt builders, pool
`ldr` xrefs, and `cmp` sites in AppleARMPlatform / S5L8940X / PRELINK.

**cpu16/fourcc result:** almost no live `0x436F7265` immediates in code
(movw/cmp = 0). One interesting DATA hit: **`0x80B8C300`** in S5L8940X IO
(possible `{FourCC, ptr}` table). Follow-up:

```bash
python3 scripts/xnu_smp_core_table.py 2>&1 | tee build/xnu_smp_core_table.txt
cat build/xnu_smp_core_table_summary.txt
```

**Table format (from dump):** `e00002c2` → FourCC name list → `e00002bd` →
parallel **MH-relative offsets** (not absolute fn ptrs). `e00002bc/02bd/02c2`
are reloc tags. S5L8940X: `'NCOf'`, **`'Core'`** then offs `0x3f5e` /
`0x3e86` → handlers @ `MH+off`.

```bash
python3 scripts/xnu_smp_core_handler.py 2>&1 | tee build/xnu_smp_core_handler.txt
cat build/xnu_smp_core_handler_summary.txt
```

**Handler note:** Table off `0x3e86` lands mid-body; real `'Core'` prologue is
**`0x80b8de38`**. Body does PMGR **`0x1000 + (arg<<2)`** via vtable
`+0x490`/`+0x494` (gate r/w + nibble poll) — i.e. **`Core(1)→+0x1004`**,
**`Core(2)→+0x1008`**. Lab already uses `PMGR_GATE0=0x1008`. Separate fn
`@0x80b8deb4` (`0x2100`/`0x2104`) is **not** ADT enable_core.

```bash
python3 scripts/xnu_smp_core_mmio2.py 2>&1 | tee build/xnu_smp_core_mmio2.txt
cat build/xnu_smp_core_mmio2_summary.txt
```

| Hook | Target | Decode |
|------|--------|--------|
| cpu0 IPI self/other | AIC `"IPID"` | hwirq **192 / 193** |
| cpu1 IPI self/other | AIC `"IPID"` | hwirq **194 / 195** |

There is no PSCI / spin-table. Linux uses `enable-method = "apple,pmgr-core"`.

## Lab results (aic1-lab v31–v32 — glass)

| Probe | Result |
|-------|--------|
| `PMGR+0x6000` | `100A0C07` → `100A0C0F` (sticky; bit1 already set in `07`) |
| `PMGR+0x6004` | **`0x54325032`** (ASCII LE `2P2T`) — do not clobber |
| SCU `CBAR+0` | already `0x2D` |
| SCU CFG | `0x511` → 2 CPUs |
| SCU PWR | `0x03030000` → **CPU0+CPU1 Normal**, CPU2+3 off |
| `IPI_SEND` BIT(1) | cpu1 EVENT `00040001` (type=4 IPI, OTHER) |
| `stuk` (v33) | `0x7` → **`0x6008/0C/10` accept stub PA** (writable ≠ boot) |
| PMGR scan | also `0x6000`, `0x2000` accept writes |
| `secondary_flag` | still 0 |

Writable start candidates are necessary but not sufficient — need a **kick**
and/or MMU-off entry. v34: cache-clean stub, plant @6008, try kick values on
`0x6004` / `0x2000` → **still `flag=0`**. CPU1 is already SCU-Normal / ADT
`waiting`; poking main PMGR start regs does not redirect a live park loop.

**v35:** `0x38C..38F` all read **`0x800C3800`** (writes do not stick);
SCU CPU1 off no-op; mailbox spray miss. `0x3A5`=`0x01043F00`, `0x3A6`=0.

**v36:** trampoline @`0x800C3800` was **all zeros**; patch (`E51FF004` /
`800000C0`) and stub copy stuck in DRAM; still `flag=0`. CPU1 is **not**
fetching that PA — either no real Core1 reset, or `38C` is a locked decoy.

**v37:** red glass (likely `38C` WO / gate / `z600` writes).

**v38:** safe path OK — `t0=EE110F10` (stub MRC @tramp); still `flag=0`.

**v39:** red after **`w3A54`** (`*(0x3A500004)=2`). `38C`/`38D` writes OK.

**v40:** `ALL DONE`, no stub; had skipped all `3A5`.

**v41:** `w3A50` OK — `bfr=2E043F00` → `aft=1B043800` (write of tramp PA
does **not** stick; not a start-addr). Still `flag=0`. **Lab PMGR/impl poke
path exhausted** for wake; need iBoot PMGR method `"Core"` RE (park mailbox /
real cpu1 reset).

## iBoot `"Core"` RE

```bash
python3 scripts/re-iboot-core.py ibec/iBEC.patched ibec/iBSS.patched 2>&1 | tee build/re-iboot-core.txt
python3 scripts/re-iboot-core2.py ibec/iBSS.patched ibec/iBEC.patched 2>&1 | tee build/re-iboot-core2.txt
```

### Findings from `re-iboot-core.txt` / `re-iboot-core2.txt`

| Item | Result |
|------|--------|
| `"Core Reset timed out"` | present; **no VA literal xrefs** (PC-rel / other) |
| iBSS `@0x7cd4` / iBEC `@0x1e5f8` | if `!(*0x6000 & 2)` → `bl` helper → **`str` to `0x6038` & `0x603C`** → `*0x6000 \|= 2` |
| Plant pool (twin iBEC literals) | `0x3F106000`, **`0x3F106038`**, **`0x3F10603C`** |
| `0x38C` / `0x3A5` | data table only @ iBSS `0x106d8` |

**v42 lab:** plant `secondary_stub` at `PMGR+0x6038/0x603C`, clear bit1, set bit1, IPI.

### v42 glass

| Probe | Value |
|-------|-------|
| `6038` before | `0x014245E1` (not a code PA) |
| `603C` before | `0x5F5F6600` (not DRAM entry) |
| `a638`/`a63C` | **stick** to `0x800000C0` |
| bit1 | `07 → 05 → 0F` (edge works) |
| `flag` | still **0** |

So `6038/603C` are **sticky plant slots** (iBoot bookkeeping / helper timing),
not cpu1 PC. Confirmed pools: `0x3F106000/6038/603C`.

### Core Reset MMIO (iBSS `@0x83cc`)

| Write | Address | Value |
|-------|---------|-------|
| rst | `0x3FD00008` | `0x808` |
| rst | `0x3FD0000C` | `0x808` |
| rst | `0x3FD00010` | `0x10` |
| SCU inv | `0x3E10000C` | `0xFF` |
| SCU ctrl | `0x3E100000` | `0x2D` |
| misc | `0x3FB00000` | `bfi` low2=1 |
| L2-ish | `0x3E000C00` | `1` |

(+ scripted tables via applicator `@0x8488`)

**v43 glass:** reset regs **already** held `808`/`10`; SCU `0x2D`; `3FB`
`2→1`; L2 `00100001→1`; bit1→`0F`; still **`flag=0`**. Hardcoded reset is a
no-op in Recovery (iBoot already ran it).

### Scripted tables (decoded)

Applicator `@0x8488`: `cmd=(base_idx<<16)|off` → `*(base_tbl[idx]+off)=val`.
`base_tbl` @ file `0x106d8`: `[0]=0x38C…` … `[8]=0x3FE…` `[9]=0x3FF…`
(`[4]=0x3A5…` present but **not** used by these two tables).

| Step | Gates | Targets |
|------|-------|---------|
| table0 `@0x34010408` | `0x1e` on | `0x3FE00xxx`, `0x3FF00408/40C/428/42C` (progressive 0…4) |
| table1 `@0x34010568` | `9`+`0xf` on | `0x38C00xxx`, `0x38D00408/40C` until `(0,0)` |

`@0x84cc` is a separate helper (`3FB` bfi=2 + `*0x3E000C04=0x80000000` + L2) — not the Core Reset path.

**v44 lab:** full table0+table1 → **whole display red** (likely `38C` table1 /
gates `9`/`0xf`, same class as v37).

**v45 glass:** `t0ok`; `3FE8=808` sticks; `3FF8=0` (WO/latch); still
`flag=0`. Table0 alone does not wake cpu1.

**v46 glass:** red immediately after **`g9f`** — gates **`9` and/or `0xf`**
alone are toxic in Recovery (before any `38C`/`38D` store).

**v47 glass:** `ALL DONE`, **CPU1 no stub**. Ungated table1 survived but did
not wake (clocks for `38C` likely still gated).

**v48 glass:** enable-only **safe**. `g9b=0x300→0x3FF` (was off); `gFb`
already `0x3FF`. **`38Cb=0x800C3800` → after table `38C4=0x808` (sticky!)**.
`aC04=0x80000000` sticks. Still `flag=0` — table1’s `808` likely **wiped**
the tramp entry that iBoot had left in `38C`.

**v49 glass:** `eC4=0x00003800` after writing `0x800C3800` — **only low-16
sticks** when gate 9 is on. Ungated `0x800C3800` was a decoy; `38C+4` is not a
full-PA entry mailbox. Still no stub.

**v50 glass:** `eC4=0x3800`, `eC8=0`, **`tins=ains=EE110F10`** (stub MRC at
both PAs) — cpu1 still **does not fetch**. `38C` entry experiments exhausted.

**Park-string scan (`find-cpu1-park.py`):** no cpu1 park/idle mailbox strings.
`"waiting"` hits are DMA/CDMA timeouts. **`"Core Reset timed out"` is a
substring of `USB Core Reset timed out`** — not a CPU bring-up panic string.
Most `0x800xxxxx` “PAs” are Thumb `bl` immediates. Notable real-ish lit:
iBSS `file+0x5c48 = 0x800c0800`.

**`re-ibec-helper.py`:** `@0x1c304` zeros a 64-bit slot, calls gettime
(`0x259bc`), returns **(lo,hi) timestamp** → stored at `6038`/`603C`. Not an
entry PC. `'Core'` fourccs are FS path fragments. All these fns unreferenced
by direct `bl` (ADT/dispatch).

**`find-ibec-wfi.py` (verified):** **zero** Thumb/ARM `wfi`/`wfe`/`sev` in
iBEC and iBSS. cpu1 is not parked in a plain iBoot WFI loop (ROM / reset-hold
/ busy-poll / XNU-only).

**v52 glass:** iBEC **mapped** (`i9F=0x480BB590` = `push`+`ldr`). PMCCNTR
lo/hi identical (`7FF83F3A`); still `flag=0`. Timestamp slots ≠ entry.

**v53 glass:** `BLX` iBEC bit1 plant **OK** — `a638/a63C` distinct timestamps,
`b1a=…07`, `cok`. Then `6008=stub`, `aC04=80000000`, `C6k2=…0F`, still
**`flag=0`**. Bit1/gettime path is necessary bookkeeping, **not** cpu1 fetch.

**`re-ibec-6034.py`:** `+0x6034` is **read** when `*0x6000 & 4` (bit2), then
`<<20` — parallel status/time to bit1’s `6038/603C`, **not** a start-addr.
Veneer `@0x1c20` is an unrelated bit0 flag check. iBEC PMGR CPU surface in
this window: `+0`,`+4`,`+34`,`+38`,`+3c` only.

**XNU (`kernelcache.p105.raw`):** has `function-enable_core` @ `0x804ca05e`,
`Core` in AppleS5L8940X cstr @ `0x80b8fecb`. No absolute `0x3F106xxx` (PIC).
`PE_cpu_start` / `processor_start` present. Next:

```bash
python3 scripts/xnu_smp_cpu2.py 2>&1 | tee build/xnu_smp_cpu2.txt
```

## PMGR `Core(n)` sequence (XNU — confirmed)

**Handler:** `AppleS5L8940XIO` `'Core'` @ `0x80b8de38` (table off `0x3e86`
points mid-body at the write). **Does not touch `+0x6000`.**

```c
/* r0=PMGR, r1=ADT arg (1|2), r2=dir (1=raise/enable), r3=target nibble */
u32 off = 0x1000u + (arg << 2);   /* Core(1)->0x1004, Core(2)->0x1008 */
u32 v = pmgr_rd(off);
u32 cur = v & 0xf;
/* enable path (dir==1): write only if cur < target */
/* disable path (dir==0): write only if cur > target */
if (need_update) {
    v = (v & 0xfffffef0u) | (target & 0xfu);  /* mask lit @0x80b8deb0 */
    pmgr_wr(off, v);
    do {
        v = pmgr_rd(off);
    } while (((v ^ (v >> 4)) & 0xf) != 0);    /* wait ack nibble */
}
```

Typical enable from `startCPU` path: **arg=2 (cpu1), dir=1, target=0xF** →
gate reg **`PMGR+0x1008`**. (Lab’s `PMGR_GATE0=0x1008` is that same word.)

**Recovery reality (v54–v55):** `+0x1008` reads **`0x2F0`** (ack=`F`,
desired=`0`); low-nibble writes **ignored**; **`|=bit31` hangs**. So
`function-enable_core` is already satisfied / inert here — not the missing
fetch.

`@0x80b8deb4` (`+0x2100`/`+0x2104`) is a **different** method — ignore for
`function-enable_core`.

### Lab v54 (`pongo/aic1_lab.c`)

`pmgr_xnu_core_enable(2, 0xF)` then stub @`6008` + `6000` pulse + IPI.
Glass tags: `g104`/`g108` before, `a108`/`Cret` after Core(2), `flag`.

**v54 glass:** `g108=a108=e108=0x2F0` — bits[7:4]=`F` (ack ON), bits[3:0]=0;
enable write did not change the reg. `'Core'` is **`clock_gate_switch`**
(same `@0x80b8de38`); gate already effectively on in Recovery → **not** the
missing cpu1 fetch. `C6ke=100A0C0F` as before.

### Lab v55

Force write + bit31 strobe readbacks (`fbef`/`frb0`/`frb1`), then same
stub/`6000`/IPI. Confirms whether low nibble is writable at all.

**v55 glass:** `Cret=FFFFFFFF` (poll timeout), `frb0=0x2F0` (write ignored),
**hung after `frb0` on `|= bit31`** — toxic. Low nibble RO; stop gate poking.

### Lab v56

No bit31. `Cret=-2` if write no-stick. Skip force probe; continue stub path.
**Gate/`Core` closed** for Recovery bringup — need a different cpu1 kick.

### Reset / entry plan (current)

XNU `startCPU` only runs `'Core'` = `clock_gate_switch` — **never** programs
a start-addr. Entry/reset must be iBoot/ROM or a SoC reset edge that
re-fetches. Lab already exhausted: `38C` decoy, `6038` timestamps, gate
`+0x1008` RO.

**v57 lab:** plant `6008/0C/10/2100…` → assert (`6000` bit1 clr + SCU CPU1
Off + `3FD` clear) → hold → deassert (`3FD=808`, SCU Normal, `6000|=F`) →
IPI. Glass: `PWRoff`/`PWRon`, `r8z`/`r8a`, `flag`.

```bash
python3 scripts/xnu_smp_entry.py 2>&1 | tee build/xnu_smp_entry.txt
cat build/xnu_smp_entry_summary.txt
bash scripts/build-aic1-lab.sh && bash scripts/deploy-aic1-lab.sh
```

**XNU entry verdict (cpu entry scripts):** `PE_cpu_start` only `bx`es
`vtable+0x350` (`startCPU`). `startCPU` only invokes `enable_core` (gate).
Kernel TEXT has **no** `movw` of `0x6004..0x6010` / `0x2100`. **Darwin never
plants cpu1 entry** — iBoot/ROM must, or a SoC reset edge must re-fetch.

```bash
python3 scripts/xnu_smp_entry2.py 2>&1 | tee build/xnu_smp_entry2.txt
python3 scripts/re_iboot_entry.py 2>&1 | tee build/re_iboot_entry.txt
cat build/re_iboot_entry_summary.txt
python3 scripts/re_iboot_entry2.py 2>&1 | tee build/re_iboot_entry2.txt
cat build/re_iboot_entry2_summary.txt
```

**iBSS `@0x800087a0` (entry2):** **PLL / clock / perf bring-up**, not SMP:
copy → `PMGR+0x220…`; OR into `+0x6004`; `*2100=2` / `*2104=0x1f` + poll;
PLL words at `+0x0/4/10…` (`0x8801xxxx`) + poll bit29. **No `0x3F106008`.**
`0x2100`/`0x2104` are not entry PAs; `0x6004` is config (`2P2T`), not a mailbox.

```bash
python3 scripts/re_iboot_entry3.py 2>&1 | tee build/re_iboot_entry3.txt
cat build/re_iboot_entry3_summary.txt
```

Hunt `0x3F106008` in **iBEC**. Lab: keep **v57 reset edge**; stop planting at `2100`.

**entry3/4 result:** **neither iBSS nor iBEC** has abs `0x3F106008/0C/10`,
relative `movw #0x6008`, or `0x800C3800`. iBoot never plants a PMGR start-addr
mailbox. Lab `6008` writes are off Apple’s path. Only `0x38C00000` hit is
iBSS `base_tbl` @ `file+0x106d8` (Core Reset tables — not an entry STR).

**Mailbox RE closed.** Remaining:

1. **v57 glass** — SCU CPU1 Off→On + `3FD` pulse + `6000` bit1; does anything
   edge? (`PWRoff` / `r8z` / `flag`)
2. If no edge → **earlier than Recovery** (iBSS hook before secondary park) or
   treat cpu1 as ROM-vector-only on cold reset.

**v57 glass:** `PWRoff=03030300` → `PWRon=03030000` (CPU1 Off→Normal **stuck**),
`r8z=0`→`r8a=808`, **`flag=0`**. Edges are real MMIO but **did not fetch stub** —
SCU PWR is likely soft status, not a pipeline reset to tramp/`6008`.

**v58 glass:** `EV0=0`→`EV1=00040001` while SCU Off → **cpu1 still takes IPI**
(not halted). `v0b=E59FF018`→`v0a=E51FF004` → phys0 is **writable iBoot
vectors**; reset-slot plant alone does not run stub (IPI uses **IRQ @+0x18**).

**v59 glass:** `v18=E59FF018`, `w38=80000220`→`newo=80000120` (hook planted),
`EV1=00040001`, **`flag=0`**. AIC delivers IPI but cpu1 does **not** enter the
phys0 IRQ vector — **VBAR≠0 / MMU alias**, or **CPSR.I masked** in park.

**v60 glass:** `nVT=1`, `oh0=EAFFFFBE` (correct `b hook`), `EV1=00040001`,
**`flag=0`**. Phys0 IRQ table + handler clobber do not run on cpu1.
**Recovery IRQ hijack path closed** (AIC EVENT ≠ CPSR taking the IRQ).

**v61:** Recovery abs-ptr spray / FIQ lit — last probe; still expect `flag=0`.

**USB memdump (Window A):** all regions `BAD0BAD0` — Pico pwn has **no**
USB memread. SecureROM dump over `memdump8942.py` is impossible on this
path. See `docs/memory-probe.md`.

**v66 glass:** `PWRa=03030300` (CPU1 Off sticks), `v00=E51FF004`,
`v04=800C3800`, **`trAL=0`**. Full Recovery toolkit exhausted: real `3FD`
edge, SCU hold, phys0 reset→tramp, sticky `eC4` — **cpu1 does not enter DRAM**.

**Recovery cpu1 wake: CLOSED.** Next: iBSS/iBEC-time plant before wipe, or
FB ROM probe / Path D bootkit — not more Recovery `3FD` labs.

**iBSS `@0x800087c4` (entry RE):** no `0x3F106008` literal. Sequence is:
progressive OR into **`PMGR+0x6004`**, then `*0x2100=2` / `*0x2104=0x1f` with
poll `bit16`. **`0x2100/2104` are CTRL/STATUS, not entry PA slots.** Stop
planting stub PAs there. Entry latch still unknown (copy-loop / iBEC / ROM).

Older shotgun list (revised):

1. Optional XNU apply: `0x1180|=0x80000000`, `0x1200=0x7FFE`, `0x1204=0x3fff8001`.
2. Plant entry only where a PA might latch (`0x6008/0C/10`) — **not** `0x2100`.
3. Avoid replaying iBSS PLL/`6004` OR under Recovery (already done by iBoot).
4. Reset edge (v57) + `IPI_SEND` + `sev`.

**Never** PMGR gate **`0x4D`** while AIC must live.

## AIC IPI registers (proven)

| Reg | Offset | Use |
|-----|--------|-----|
| `IPI_SEND` | `0x2008` | `BIT(cpu)` — **works** |
| `IPI_ACK` | `0x200c` | `SELF\|OTHER` |
| `IPI_MASK_CLR` | `0x2028` | unmask before send |
| `CPU_EVENT(cpu)` | `0x5004+(cpu<<7)` | shows `0x00040001` after send |

## SCU / CBAR under Linux

- Phys CBAR: **`0x3E100000`**, SCU already enabled in Recovery/iBoot.
- **Do not** `ioremap` CBAR under Linux (full window hangs). `platsmp` skips `scu_enable()`.

## Lab (v32)

`phase_smp` plants `secondary_stub` PA into start-addr candidates, pulses Core1, sends IPI, checks `secondary_flag == 0xC0DE0001`.

```bash
bash scripts/build-aic1-lab.sh && bash scripts/deploy-aic1-lab.sh
```

If glass shows `CPU1 STUB OK`, note which start-addr offset stuck — lock that into `platsmp.c`.

## Linux pieces

- `arch/arm/mach-apple/platsmp.c` — `apple,pmgr-core`
- `drivers/irqchip/irq-apple-aic1.c` — `ipi_mux` + `apple_aic1_ipi_wake()`
- `CONFIG_SMP=y`, `CONFIG_NR_CPUS=2`
