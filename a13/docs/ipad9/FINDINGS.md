# iPad 9 (A13 / T8030) — Linux port: no-hardware recon

Date: 2026-10-17. Status: analysis + emulator bench up; no device in hand.

Goal — evaluate iPad 9 (A13) as the next target after iPad mini 1, with the GPU in
mind (A13 is an M1 predecessor, already reverse-engineered by Asahi/Mesa).
Everything below comes from IPSW `iPad_10.2_2021_27.0.1_24A446` and from Asahi /
HoolockLinux sources, plus a locally built T8030 emulator (see below).

## Device

- Model: **iPad12,1** (Wi-Fi) / iPad12,2 (Cellular). Board: **J181AP** / J182AP.
- SoC: **T8030** (A13). ADT: `arm-io,t8030`, `pmgr1,t8030`, `aic,1` (aic-version 2).
- CPU: 6 cores — 2×P `apple,lightning` (MIDR part 0x12) + 4×E `apple,thunder` (0x13).
- iOS: 27.0.1 — latest (device still serviced).

## GPU — the main point

- GPU node: `sgx`, compatible **`gpu,t8030`**, has `gfx-asc` (`iop,ascwrap-v2`,
  role GFX) — i.e. a **firmware-managed coprocessor**, like AGX on M1, not a bare
  SGX. `metal-standard: 256`, `opengl-standard: 768`.
- GPU firmware in IPSW: **`Firmware/agx/armfw_g12p.im4p`** → generation **G12**
  (A12 = G11, A14/M1 = G13). Confirms AGX architecture, not PowerVR.
- Takeaway: the driver is written against the **Asahi AGX** model (shared
  ISA/firmware-ABI), not from scratch like SGX543 on A5. But G12 ≠ G13 (M1) —
  Asahi targets G13, so the firmware ABI and some registers will need adapting for
  the generation gap.
- GPU DART: `mapper-gfx-asc` (`iommu-mapper,gfx`) — same DART type.

## Display

- **No DCP.** On T8030 the display path is `disp0` (`disp0,t8030`,
  display-subsystem) + `displayport0` (`displayport,t8027`) + `lpdp-phy0`
  (`lpdp-phy,t8020`) + `lcd` panel (`lcd,edp`), backlight `backlight,lp8559`
  (i2c0). So it is a **direct controller + eDP**, closer to our mini 1 experience
  than to the M1 DCP model. Simpler than DCP, but Asahi has no driver for it — we
  write our own KMS (as on mini 1).
- Display DART: `dart,t8020`, page-size 16384.

## Peripherals (per ADT)

- **Storage:** ANS2 (`ans`, `iop,ascwrap-v2`, role ANS2) + `sart,coastguard` —
  NVMe-like, as on M1. `apple-nvme`/ANS driver exists in Asahi/Linux.
- **USB:** `usb-drd,t8030` + `atc-phy,t8030` (ATC PHY). Type-C controller family
  (Lightning connector notwithstanding). dwc3 + atcphy drivers exist in Asahi.
- **Wi-Fi/BT:** BCM4355 (`wlan-pcie,bcm4355`) over PCIe (`apcie,t8030`) + BT over
  UART (`bluetooth,n88`). Covered by brcmfmac.
- **Touch:** `hid-transport,spi` over spi3 (`multi-touch`), bootloader-type HBPP —
  same class as mini 1 (touch firmware loaded over SPI HBPP).
- **Audio:** `cs42l83` (codec, i2c3/mca4) + `max98357` (speaker) via mca-switch
  t8030. Same structure as our mini 1 audio (MCA + CS + amp).
- **Other:** SEP (`iop,t8030`), ISP, AVE/AVD, ANE, SMC, spmi-pmu (`pmu,avus`).

## What already exists — MAJOR UPDATE 2026-10-17: a working A13 port exists

Earlier this doc assumed "no A13 port anywhere" (Hoolock DTS stopping at t8015).
**That is wrong.** A lead (yhavry's forks) shows an active bare-metal Linux-on-A13
port. This reshapes the whole plan.

- **Active bare-metal Linux-on-A13 port exists:** `yhavry/Hoolock-linux` branch
  **`t8030`** (full kernel tree, pinned commit `38e339f`) + docs
  `yhavry/Hoolock-Docs-A13`. Author yhavry (matches the "Hoolock Linux on iPhone 11
  Pro" video claim). Built on top of HoolockLinux.
- **T8030 device trees exist and build:** `t8030-n104` (iPhone 11), `t8030-d421`
  (iPhone 11 Pro), `t8030-d79` (SE 2). n104/d79 are **USB + framebuffer only**;
  **`t8030-d421` is the full-featured template** (display panel + Wi-Fi/BT), so a new
  iPad `t8030-j181` DT should derive from **D421, not n104**. No J181 DT yet.
- **m1n1 `idevice` — nuance (verified from source 2026-10-17):** this branch has
  **no explicit T8030 target**. `src/soc.h` jumps T8015 (A11) → T8103 (M1): no
  `T8030` define, no `EARLY_UART_BASE`, and `chickens.c` has **no Lightning/Thunder
  case**. The actual T8030 bring-up lives in the **kernel DTS** (yhavry `t8030`
  branch), which wires T8030 to m1n1's *generic* drivers via `apple,t8103-*`
  compatible fallbacks. So m1n1 is reused as-is for peripherals; the one real piece
  of new m1n1 code is the **Lightning/Thunder CPU chickens init** (slots between A11
  `twister` and M1 `firestorm`) plus the soc.h wiring. Not greenfield, but not "done"
  either. All peripheral bases are cross-validated — see `t8030_init_notes.md`.
- **Working on A13 today** (A13.md, device iPhone 11 Pro D421): CPU, SMC
  (RTC/reboot/poweroff/hwmon), cpufreq, serial-console shell, USB gadget
  (ethernet/mass-storage/serial), MIPI DSI command path, backlight, framebuffer via
  **simpledrm on the iBoot framebuffer**.
- **Status refined by source read (see `hoolock_a13_gap.md`):** **NVMe is present**
  on T8030 (`apple,t8030-nvme-ans2` + a dedicated `apple_nvme_t8030_hw` driver path) —
  unvalidated in docs, but NOT missing. **Wi-Fi/BT are present on the D421 DT**
  (also unvalidated). Genuinely absent / greenfield: **GPU (AGX)**, the **cs42l83
  audio codec driver** (only cs42l84 exists upstream), **SPI-HID multi-touch**, and
  **native display** (the only in-tree display driver `adp-mipi` is MIPI-only and
  WIP — but the iPad is **eDP LCD**, not MIPI). Native KMS modesetting is WIP.
- **Linux (apple-dart etc.):** the **t8020** DART format is already upstream (M1
  uses it) and T8030 uses `dart,t8020`. Still a plus.
- Bottom line: **base-platform bring-up on A13 is already solved by Hoolock** (incl.
  NVMe storage). Our differentiated value is what they lack: the **GPU (AGX G12)**
  above all, plus native display/KMS (iPad eDP), SPI-HID touch, the cs42l83 audio
  codec, and the iPad 9 DT.

## Boot — entry exists, gate passed

- **checkm8 does not cover A12/A13** (BootROM fixed) — the old Cascadia path
  (checkm8 → own loader) does not carry over directly.
- **BootROM entry on this SoC exists** (method not recorded here; it is the same
  class that opened A5). So "how to get in" is no longer the main open question:
  the gate is effectively passed. Still to confirm separately: reproducibility on
  J181/iPad12,1 specifically — but that is a device-in-hand task.
- Hoolock's **documented** A13 chain is checkm8/pongoOS → m1n1 (HoolockLinux/m1n1
  `idevice`) → Linux, with an alternate "load m1n1-idevice.macho from iBoot" path.
  CAVEAT: the checkm8 credit is inherited from the A7–A11 lineage and **checkm8 does
  not reach A12/A13** — so the real A13 entry is a different (undisclosed) door; the
  Hoolock m1n1 + kernel ride on top of it. Reproducing entry is the device-side task.
- So the bottleneck is NOT "write m1n1 from scratch" (it exists). It is: (a) stand up
  yhavry's m1n1 + t8030 kernel, (b) add the iPad 9 DT, (c) do the GPU — the real prize.
- The repo holds iPhone 11 files (`a13_boot/ibootstuff`: iBSS/iBEC n104, patched
  iBSS, ramdisk, kernelcache iphone12b/x, dtree_patcher) — **same SoC T8030**.
  NOTE: **no live n104 in hand anymore** (had one before), so these are only
  sample files (firmware/DT), not a flashing bench.

## Emulator (qemu-t8030 / Inferno) — BUILT & iOS 14 BOOTED 2026-10-17

- **Built and verified locally** on this Mac: upstream **`TrungNguyen1909/qemu-t8030`**
  (commit `fd4b0f7`, QEMU 7.1.0). Configure flags
  `--target-list=aarch64-softmmu,x86_64-softmmu --disable-capstone --enable-lzfse
  --disable-werror`. Binary: `build/qemu-system-aarch64`, machine `-M t8030` present.
  All work lives in `~/Desktop/t8030-emu-build/` (outside the Cascadia repo).
- **iOS 14 booted.** Downloaded the specific IPSW iPhone12,1 14.0 **`18A5351d`**
  (~5.5 GB), generated `root_ticket.der` + NVMe namespace images, booted `-M t8030`.
  **UART printed 419 lines of real XNU bring-up** via `-serial mon:stdio`. **lldb
  attached to the gdb stub on :1234**, read the kernel UUID + registers, detached.
  So UART + gdb stub are both confirmed working.
- Upstream status: `TrungNguyen1909/qemu-t8030` is **archived (read-only since
  2023-04-14)**; the active successor is the fork **`ChefKissInc/Inferno`**
  (ex-QEMUAppleSilicon), docs at chefkiss.dev (403 to a plain fetcher). We built the
  archived upstream and it works; Inferno is the one to track for newer features.
- **CRITICAL:** the `-M t8030` machine is **built to boot an XNU kernelcache, not an
  arbitrary payload.** `-kernel` expects a mach-o XNU, `-dtb` an Apple `.im4p`, and
  it consumes trustcache + APTicket. **There is no documented path to load m1n1 / a
  raw macho / a custom loader on t8030** (the `-M s8000`/iPhone 6s machine can boot
  a SecureROM via `-bios`, but that is a different SoC and still an Apple image).
- **Role of the emulator:** NOT a turnkey m1n1 runner. It is an **observable
  reference T8030 on the desk**: gdb-walk the AIC/PMGR/DART/GPU bring-up the way iOS
  itself does it, with no hardware and without even a jailbreak for the early SoC
  part — exactly the kernel observability we lacked on A5.
- **Can it boot Linux? No — tested & characterized 2026-10-17 (`t8030_emu_linux_recon.md`).**
  `-M t8030` has no DFU/pongoOS/SecureROM staging (only `-M s8000`/iPhone 6s has
  `-bios`), so the only entry is `-kernel <macho>`. And that path runs the XNU
  patchfinder **unconditionally**: `t8030_patch_kernel()` → `kpf()` looks up XNU
  kexts via `__PRELINK_INFO`/`__PRELINK_TEXT`; m1n1/pongoOS lack those → qemu aborts
  `unsupported xnu` before any payload code runs. Proven empirically (built m1n1
  idevice, loaded it, got the abort). So booting Linux in the VM needs a real
  **two-part mini-project**: (a) a qemu machine patch gating kpf + the downstream XNU
  assumptions (dtb, trustcache/APTicket, bootargs, entry EL, rtbuddy) for non-XNU
  payloads, AND (b) the m1n1 A13 CPU port. Neither small. For now the emulator stays
  an **XNU observation bench**, not a Linux bench; real Linux boot is cheaper on
  actual hardware (where Hoolock already boots) than by fighting qemu.

## Proposed plan (no hardware)

1. **Emulator as reference bench — DONE (2026-10-17).** qemu-t8030 built, iOS 14
   booted on `-M t8030`, UART + gdb stub confirmed. Next on this axis: use the gdb
   stub to capture real AIC/PMGR/DART/UART (and GPU) bring-up sequences, and scope
   the work of adding arbitrary-macho loading so m1n1 can run inside it.
2. **Stand on yhavry's A13 stack — do NOT rewrite m1n1.** Read HoolockLinux/m1n1
   `idevice` + the `t8030` kernel branch, understand what the T8030 init actually
   does, and (with a device, later) bring up n104/d421 to a serial shell. Diff their
   checkm8-era documented entry against our actual door.
3. **iPad 9 (J181) device tree:** derive `t8030-j181` from **`t8030-d421`** (the DT
   that has panel + Wi-Fi/BT). Per our ADT the iPad differs: **eDP LCD** (not
   MIPI/OLED), **BT over UART serdev** (not PCIe), LP8559 I²C backlight, gpio-keys,
   speaker amp. Lowest-effort gap — the drivers exist. See `hoolock_a13_gap.md` Part B.
4. **GPU (AGX G12) — the real prize; nobody has it on A13.** Unpack `armfw_g12p`,
   compare firmware ABI G12 vs G13 (Asahi); use the emulator gdb bench + (later)
   on-device jailbreak kernel R/W to capture GPU bring-up. This is where our mini-1
   GPU experience + the emulator pay off.
5. **Fill the gaps Hoolock lacks:** internal NVMe storage (unconfirmed on A13), touch
   (SPI HBPP, like mini 1), Wi-Fi/BT (brcmfmac / BCM4355), audio (cs42l83+max98357),
   native KMS (their modesetting is WIP; we did KMS on mini 1).
6. **Hardware list:** iPad12,1 on a jailbreakable iOS (kernel R/W for GPU RE), the rig
   for device entry, cables.

## Assessment (2026-10-17)

**Will Linux boot on real iPad 9 hardware? — high probability for a serial console.**
The SoC is identical to the iPhone 11 that Hoolock already boots to a shell; all
peripheral bases are cross-validated (`t8030_init_notes.md`) and bind to existing
`apple,t8103-*` fallback drivers. Residual risk is concentrated and mostly
engineering, not research:
- m1n1 Lightning/Thunder chickens init (small, untested).
- Device entry reproducibility on J181 specifically (the door).
- A correct `t8030-j181.dts` (mechanical, derived from D421).
So early console bring-up is likely; a *usable* framebuffer + USB system follows once
the J181 DT is right. GPU / touch / Wi-Fi / audio are separate, later work. The
research-grade uncertainty is the GPU, not the boot.

**Would a fuller emulator strongly help the RE? — strong for observation, not a
hardware substitute.** For CPU/SoC/driver RE the bench is already faithful (it boots
iOS to a shell). For the GPU prize it is **observation-only**: qemu's "GPU" is a stub,
so it will NOT run real AGX firmware or render — but gdb-watching how iOS *drives* the
GPU interface (MMIO order, DART maps, firmware handshake) is exactly the observability
we lacked on A5 and genuinely accelerates understanding the G12 ABI. Final
driver/firmware validation still needs hardware. Building a faithful AGX model is not
worth it. Net: the emulator is a strong *complement* to hardware RE — decisive for
platform/DT work, observation-only for the GPU.

## Files (in this directory)

- `24A446__iPad12,1_2/` — pulled from the IPSW: DeviceTree j181/j182, iBoot/LLB,
  SEP, `Firmware/agx/armfw_g12p.im4p`, BuildManifest.
- `j181ap.txt` / `j181ap.json` — unpacked ADT (readable dump).
- `t8030_init_notes.md` — cross-validated UART/AIC/PMGR/DART bases + init sequences
  (sources: yhavry t8030 DTS, HoolockLinux/m1n1 idevice, qemu-t8030, live gdb). The
  m1n1 porting reference for the CPU-init that remains.
- `hoolock_a13_gap.md` — T8030 kernel driver/DT coverage (Part A) + iPad J181 DT diff
  vs D421 (Part B), with a prioritized gap list. The porting worklist for iPad 9.
- `t8030_emu_linux_recon.md` — why the emulator can't boot Linux as-is (XNU
  patchfinder wall) and the two-part fix it would take.
