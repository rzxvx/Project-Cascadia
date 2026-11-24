# J171: the CPU -- Fusion, cpufreq, idle

## Two cores, four physical

The A10 Fusion pairs two Hurricane (performance) and two Zephyr
(efficiency) cores: each logical CPU is one of its pair at a time, the
switch made by the performance state. iPadOS sees two CPUs; so does Linux.
m1n1: `CPU: A10/T2 Hurricane-Zephyr`, MIDR 0x611f0061. The kernel:
`CPU features: detected: Apple Fusion Architecture`, tainted
(`ARM64_ERRATUM_APPLE_FUSION`: the Zephyr cores cannot run 32-bit EL0, so
32-bit userspace is off), and a PMU driver of its own (`apple_fusion_pmu`,
10 counters). Both CPUs come up through m1n1's spin table (`RVBAR entry on
secondary CPU`).

## cpufreq

`/soc/performance-controller@202f20000` (`apple,t8010-cluster-cpufreq`, the
M1's driver), ten states in the DT's OPP table: levels 1-4 the Zephyr
cores, 5-10 the Hurricane ones. The table's frequencies are labels more than
measurements (its lowest, 172000 kHz, says "300 MHz, E-core" next to it);
cpufreq reports the top as 2244000 kHz.

`cpufreq.off=1` came along from the A13's boot arguments (the A13's DVFS
hangs now and then) and kept the A10 at the state iBoot left: the lowest
Zephyr one -- the first 10 s after a boot, `time_in_state` had 1013 ticks at
172000 kHz. Without it, apple-cpufreq + schedutil:

| | before | after |
|---|---|---|
| a busybox `sh` loop to 300000 | 3.38 s | 0.70 s |
| 60 s, both cores busy | -- | 2244000 kHz all the way, 30 -> 32 °C, no stall |

Idle drops back to the Zephyr states. No boost states beyond the table.

## Not yet

- **cpuidle**: no states; idle is `WFI`. iPadOS powers cores down deeper --
  a battery matter, not a speed one.
- **KPTI** is on (`Mitigation: PTI`, forced by KASLR): each syscall pays a
  page-table switch. Whether the A10 needs it is not checked.
- The real frequencies: the PMU's cycle counter against the clock would
  say.
