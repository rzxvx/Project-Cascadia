# J171 research notes

One file per part of the iPad 7 (iPad7,11, A10 / T8010), with what each one
took to bring up: the logs, the numbers, the tries that failed. The
step-by-step guide is [../QUICKSTART.md](../QUICKSTART.md); the overview,
[../bring-up.md](../bring-up.md).

| file | what |
|---|---|
| [j171-adt.md](j171-adt.md) | the Apple device tree: where every unit sits, nothing private |
| [j171-boot.md](j171-boot.md) | DFU, checkm8, iBSS, m1n1 (the NVMe link, S3E, the HMB), the payload, /init |
| [j171-nvme.md](j171-nvme.md) | the disk, shrinking APFS from iPadOS, the Linux partition, the write gate |
| [j171-cpu.md](j171-cpu.md) | Fusion's two-of-four cores, cpufreq, what idle still lacks |
| [j171-touch.md](j171-touch.md) | SmartIO, the Z2 controller, its firmware and SysCfg calibration |
| [j171-wifi-bt.md](j171-wifi-bt.md) | BCM4355C1: firmware from the AEA root image, Wi-Fi, Bluetooth |
| [j171-power.md](j171-power.md) | the battery over HDQ, the D2365, the Bellatrix NACK, the RTC |
| [j171-desktop.md](j171-desktop.md) | simpledrm, labwc on pixman, what the CPU draws, task-killer |
| [../gpu.md](../gpu.md) | the GPU (its own track) |

Measured on one iPad7,11. Values private to the iPad -- serials, its radios'
addresses, its factory calibrations, the SysCfg dump -- stay in `build/`
and are not quoted here.
