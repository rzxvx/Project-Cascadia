# Hoolock A13 / T8030 → iPad 9 (J181) kernel-driver + device-tree gap list

Date: 2026-10-17. Pure public-source analysis — no hardware. Scope: Linux kernel
device trees and drivers only (no bootloader / entry).

Sources (tag → origin):

| tag | origin |
|-----|--------|
| **[dts …]** | `yhavry/Hoolock-linux` branch `t8030`, `arch/arm64/boot/dts/apple/` (`t8030.dtsi`, `t8030-pmgr.dtsi`, `t8030-common.dtsi`, `t8030-n104.dts`, `t8030-d421.dts`, `t8030-d79.dts`) |
| **[drv …]** | driver path in the same `t8030` kernel tree (verified via GitHub contents API / recursive git-tree) |
| **[A13.md] / [A11.md]** | `yhavry/Hoolock-Docs-A13` `features/A13.md`, `features/A11.md` |
| **[ADT …]** | local iPad12,1 Apple device-tree dump `a13_boot/ipad9/j181ap.txt` |
| **[notes]** | `a13_boot/ipad9/t8030_init_notes.md` (cross-validated register map) |

**Reference devices in the branch.** Three T8030 DTs exist. `t8030-d421.dts`
(iPhone 11 **Pro**) is the only *rich* one — it is the right template, not n104.
`t8030-n104.dts` (iPhone 11) and `t8030-d79.dts` (iPhone SE 2) are bare: they add
**only** USB + a loader-filled `simple-framebuffer` on top of `t8030.dtsi`
[dts n104.dts:26-46, d79.dts]. D421 additionally wires the MIPI display panel and
Wi-Fi/BT over PCIe [dts d421.dts:35-102].

---

## PART A — T8030 kernel driver / DT coverage

Status legend: **Present** = DT node + bound driver in the `t8030` tree (docs or
source confirm it runs); **WIP** = partially implemented; **Absent** = no driver
and/or no DT node anywhere in the branch.

| Subsystem | Status | Evidence |
|-----------|:------:|----------|
| **CPU / SMP** (2×Lightning P + 4×Thunder E) | **Present** | `cpus` 6 cores, `spin-table`, OPP tables [dts t8030.dtsi:37-100]; "CPU … validated" [A13.md]. m1n1 `chickens` for Lightning/Thunder is the one bring-up piece still unwritten, but that is **boot**, out of scope here [notes]. |
| **AIC** (interrupt controller) | **Present** | `interrupt-controller@23b100000` `apple,t8030-aic,apple,aic` [dts t8030.dtsi:263-268]; mainline `irq-apple-aic` (v1 path) [notes]. |
| **PMGR** (power/clock) | **Present** | `power-management@23b000000` `apple,t8030-pmgr,apple,t8103-pmgr` + pwrstate children [dts t8030.dtsi:217-261, t8030-pmgr.dtsi]; reuses M1 `apple,t8103-pmgr` driver. |
| **UART console** (`apple,s5l-uart`) | **Present** | `serial@235200000` [dts t8030.dtsi:424-431]; "serial-console shell" [A13.md]; mainline `apple-s5l` 8250 driver. |
| **DART** (IOMMU) | **Present** | `apple,t8103-dart` nodes (usb_dart0/1, pcie_dart2) [dts t8030.dtsi:441-514]; mainline `drivers/iommu/apple-dart.c` [drv] — t8020 family, unchanged from M1. |
| **SART** | **Present** | `iommu@27ac50000` `apple,t8030-sart` [dts t8030.dtsi:488-492] (NVMe scatter list). |
| **NVMe / ANS2 internal storage** | **Present (unverified in docs)** | `nvme@27acc0000` `apple,t8030-nvme-ans2`, **enabled** (not `disabled`), + `ans_mbox`, `sart`, `ps_ans2` [dts t8030.dtsi:473-505]. Driver has a **dedicated T8030 path**: `apple_nvme_submit_cmd_t8030`, `apple_nvme_t8030_hw`, and `{.compatible="apple,t8030-nvme-ans2"}` in of_match [drv drivers/nvme/host/apple.c:378,1870,1889]. **So the FINDINGS caution ("A11 only, A13 undocumented") is out of date at the source level** — the T8030 ANS2 is wired and has driver support. It is simply **not listed/validated in A13.md** (A11 NVMe shipped in 6.18 [A11.md:44]). Treat as *present, needs on-device confirmation*. |
| **SMC** (+ GPIO / reboot / poweroff / RTC / HWMON) | **Present** | `smc@23e400000` `apple,t8030-smc` with gpio/reboot/rtc/hwmon children + `smc_mbox` [dts t8030.dtsi:377-422]; all five "SMC …" rows + "Reboot and poweroff" + "cpufreq" marked done [A13.md]. HWMON sensor keys per device [dts t8030-common.dtsi:8-153]. |
| **SPMI PMU / PMIC + RTC + boot-NVMEM** | **Present** | `spmi@23d180700` → `apple,avus-pmic` → rtc + nvmem boot flags [dts t8030.dtsi:295-351]. |
| **Watchdog** | **Present** | `watchdog@23d2b0000` `apple,t8030-wdt,apple,t8103-wdt` [dts t8030.dtsi:353-359]. |
| **cpufreq** | **Present** | `performance-controller` E/P `apple,t8030-cluster-cpufreq` [dts t8030.dtsi:361-375]; "CPU frequency scaling validated" [A13.md]. (Top P-states disabled, `#if 0` [dts t8030.dtsi:177-190, A13.md].) |
| **USB — gadget/peripheral** (dwc3 + ATC phy) | **Present** | `usb@239010000` `apple,t8030-dwc3`, `usb2phy` `apple,t8030-ausb-phy`, dual DART [dts t8030.dtsi:433-471], enabled `dr_mode="peripheral"` per device [dts n104.dts:26-46, d421.dts:52-72]. "USB gadget (ethernet/mass-storage/serial)" works [A13.md]. |
| **USB — host mode** | **WIP/untested** | Same dwc3/atcphy hardware, but every device DT forces `dr_mode="peripheral"`, `maximum-speed="high-speed"` [dts n104.dts:43-44]. Host/OTG + SuperSpeed not exercised. |
| **PCIe RC** (`apple,t8030-pcie`) | **Present (disabled by default)** | `pcie@610000000` + `pcie_dart2` present but `status="disabled"` in SoC dtsi [dts t8030.dtsi:507-574]; **enabled only on D421** [dts d421.dts:74-81]. |
| **Wi-Fi** (BCM4355 over PCIe, brcmfmac) | **Present (D421 only)** | `wifi@0,0 compatible="pci14e4,4425"` under `port02` [dts d421.dts:84-91]; mainline `brcmfmac` PCIe [drv drivers/net/wireless/broadcom/brcm80211/brcmfmac/]. MAC/NVRAM "to be filled by loader". Not listed in A13.md → DT present, runtime unvalidated. |
| **Bluetooth** (Broadcom) | **Present (D421 only, PCIe variant)** | `bluetooth@0,1 compatible="pci14e4,5f69"` [dts d421.dts:93-99] → mainline `hci_bcm4377.c` (PCIe) [drv drivers/bluetooth/hci_bcm4377.c]. **Note the transport:** D421 BT is **PCIe**; the UART serdev path (`hci_bcm.c`) is also in-tree [drv]. |
| **Display — MIPI DSI command path + panel** | **Present (D421 only)** | `dsi@230400000 apple,t8030-display-pipe-mipi` + `panel@0 apple,summit` [dts d421.dts:36-50]. Driver: `drivers/gpu/drm/adp/adp-mipi.c` **explicitly matches `apple,t8030-display-pipe-mipi`** with t8030 codepaths [drv adp-mipi.c:181,268] + `drivers/gpu/drm/panel/panel-summit.c` [drv]. "MIPI DSI host — command transfers" done [A13.md]. |
| **Display — native KMS modesetting** | **WIP** | "Native display modesetting: **WIP** … Linux still uses the iBoot framebuffer"; "Native panel initialization: WIP" [A13.md]. `drivers/gpu/drm/adp/adp_drv.c` (CRTC) exists but T8030 modeset not validated. |
| **Display — framebuffer (simpledrm)** | **Present** | `framebuffer@0 apple,simple-framebuffer`, loader-filled, every device [dts n104.dts:16-22, d421.dts:25-31]; "Main framebuffer t8030 (simpledrm)" [A13.md]. |
| **Backlight** | **Present (D421, panel-integrated)** | Brightness through the summit panel (`max-brightness=2047`, `default=598`) via the MIPI command path [dts d421.dts:45-48, A13.md]. **No standalone backlight-chip driver node.** |
| **Audio DMA/I2S fabric** (Apple MCA/ADMAC) | **Driver present, no DT node** | `sound/soc/apple/mca.c` in tree [drv]; **no `mca`/`admac`/`nco` node in any T8030 DT.** Not mentioned in A13.md. |
| **Audio codec `cs42l83`** | **Absent (driver)** | **No `cs42l83.c`** in `sound/soc/codecs/` (404) — mainline ships `cs42l84.c` but **not** the `cs42l83` iPhone/iPad part [drv]. No audio node in any T8030 DT. |
| **Audio speaker amp `max98357`** | **Driver present, no DT node** | `sound/soc/codecs/max98357a.c` in tree [drv]; no DT node. |
| **Touch — SPI HID multi-touch** | **Absent** | No Apple-iOS SPI-HID touchscreen driver in-tree (`applespi.c` is the x86 MacBook keyboard/trackpad, not a touchscreen) [drv]; no touch node in any T8030 DT. |
| **GPU — AGX (G12)** | **Absent** | **No `drivers/gpu/drm/asahi/` in the branch** (404); only the mainline `apple,agx.yaml` binding doc exists [drv]. No GPU node in any T8030 DT; not mentioned in A13.md. This is greenfield on A13. |
| **SEP / ISP / ANE / AVE / AOP audio / scaler / JPEG** | **Absent** | Present in ADT [ADT: `iop,t8030`, `isp,t8030`, `aop-audio`, `scaler,t8030`, `jpeg,t8030`] but no DT nodes/drivers in the branch. Out of near-term scope. |

**Part A summary.** The entire *base platform* (CPU, AIC, PMGR, DART, SMC, SPMI/RTC,
watchdog, cpufreq, UART, USB-gadget, **NVMe**, PCIe, simpledrm) is present in the
T8030 branch. Wi-Fi, BT and the MIPI display/panel are present **but only wired on
D421** and **not validated in the docs**. Native modeset is WIP. **Audio, touch and
GPU are the real holes** (codec driver / touch driver / GPU driver all missing).

---

## PART B — iPad J181 device-tree diff vs the n104/D421 base

Same SoC (T8030), so everything in `t8030.dtsi` is reused unchanged. A new
`t8030-j181.dts` should be derived from **`t8030-d421.dts`** (the only base with
display + Wi-Fi/BT structure), changing the board-specific leaves below. Each row:
ADT property on J181 ⇄ the n104/D421 node it maps to.

| Area | iPad J181 (ADT) | n104/D421 base | What t8030-j181.dts must do |
|------|-----------------|----------------|------------------------------|
| **Compatible/model** | `iPad12,1`, board `J181AP` [ADT:3-5, `compatible:"iPad12,1"`] | `apple,d421`/`apple,n104` [dts d421.dts:12] | New `compatible="apple,j181","apple,t8030",…`, model "Apple iPad (9th gen)". |
| **Display panel** | **eDP LCD**: `disp0,t8030` + `displayport,t8027` + `lpdp-phy,t8020` + panel `lcd,edp` [ADT:4304 `compatible:"lcd,edp"`, + `displayport,t8027`,`lpdp-phy,t8020`] | **MIPI-DSI OLED**: `apple,t8030-display-pipe-mipi` + `apple,summit` [dts d421.dts:36-50] | **Fundamentally different pipe.** The adp-mipi + summit nodes do **not** apply. For bring-up keep only `simple-framebuffer` (loader-filled) — works regardless of panel tech. Native path needs a new eDP/DisplayPort driver (see Part C-3). Res ≈ 2160×1620 (iPad 9 spec; ADT `display-timing-info` blob present but not cleanly decodable in the dump). |
| **Backlight** | Discrete **`backlight,lp8559`** on an I²C bus [ADT:3968] | Panel-integrated brightness (summit) [dts d421.dts:45-48] | Add an `i2c@…`/`lp855x`-class backlight node. Mainline `lp855x_bl.c` covers LP8550-57 [drv]; **LP8559 is not a listed variant** → small driver/compat addition likely needed. |
| **Touch** | `multi-touch` / **`hid-transport,spi` on spi3** (`spi-1,samsung`, IRQ 447) [ADT:3917, spi3 block] | *absent in both* | Add `spi3` + touch HID node **and** a new driver (see Part C-3). |
| **Audio codec** | **`cs42l83`** on **I²C** [ADT:4068 `audio-control,cs42l83`, `device_type:"i2c"`] | *absent in both* | Add I²C codec node — **but the `cs42l83` driver does not exist** (Part C-3). |
| **Audio speaker amp** | **`max98357`** (`device_type:"aod"`) [ADT:5622] | *absent in both* | Add `max98357a` node (driver exists [drv]) + MCA/NCO/ADMAC I²S fabric nodes (driver `mca.c` exists). |
| **Accessory audio** | `cs46l10` "audio-mikeybus" on **SPI** (Lightning MikeyBus) [ADT:3841] | *absent* | Accessory-only; **skip** for Linux. |
| **Wi-Fi** | **BCM4355** `wlan-pcie,bcm4355` over `apcie,t8030` [ADT, `wlan-pcie,bcm4355`] | D421 `pci14e4,4425` under port02 [dts d421.dts:84-91] | **Reuse the D421 wifi node almost verbatim**; change `brcm,board-type="apple,j181"`, MAC from loader. Same chip/path. |
| **Bluetooth** | **`bluetooth,n88` over UART** (serdev; `power_enable`,`bt_wake` GPIOs) [ADT, `bluetooth,n88` under `device_type:"uart"`] | **D421 BT is PCIe** `pci14e4,5f69` [dts d421.dts:93-99] | **Transport differs.** Replace the PCIe BT child with a UART `serial@…{ bluetooth { compatible="brcm,bcm4355c0"… } }` serdev node → mainline `hci_bcm.c` [drv]. |
| **Buttons** | `buttons`: voldown, volup, hold (power), menu (home/Touch-ID), `halleffect1` (smart-cover) — GPIO-backed [ADT:`compatible:"buttons"` block] | *absent in both* | Add a `gpio-keys` node (driver `gpio_keys.c` exists [drv]). Home button = KEY_HOMEPAGE; hall = SW_LID. |
| **Ambient light** | `als,tcs3490` ×2 [ADT] | *absent* | Needs I²C node **+ driver**: no `tcs3490` driver in-tree [drv]. Low priority. |
| **Accelerometer** | `accel` (syscfg-calibrated) [ADT:3114-3126] | *absent* | I²C/iio node; part TBD from ADT. Low priority. (No gyro/compass in ADT — matches iPad 9 spec.) |
| **Smart Connector / Tristar** | `dock,orion` smart-connector, `tristar,cbtl1614` Lightning mux [ADT:port-smart-connector-1, `tristar,cbtl1614`] | n/a | iPad-only; no Linux need near-term. |
| **USB** | `usb-drd,t8030` + ATC phy (Lightning) [ADT] | dwc3 peripheral [dts d421.dts:52-72] | Reuse as-is (`dr_mode="peripheral"`), same `usb2phy` cfg values. |

---

## Prioritized conclusion

### (1) Works as-is from the n104/D421 base — no new code, same SoC
CPU/SMP, AIC, PMGR, UART console, DART, SART, **NVMe internal storage**
(`apple,t8030-nvme-ans2`, driver has a T8030 path), SMC (reboot/poweroff/RTC/
HWMON/GPIO), SPMI PMU + RTC + boot-NVMEM, watchdog, cpufreq, USB gadget, PCIe RC,
simpledrm framebuffer. [dts t8030.dtsi; drv; A13.md]

### (2) Needs a new `t8030-j181.dts` only (drivers already exist, just wire J181)
Derive from `t8030-d421.dts` and change the leaves:
- **Wi-Fi** BCM4355 node (reuse D421, change board-type) — brcmfmac present.
- **Bluetooth** re-wired as **UART serdev** (not PCIe) — `hci_bcm.c` present.
- **gpio-keys** buttons (vol/power/home/hall) — `gpio_keys.c` present.
- **Speaker amp** `max98357a` + **MCA/NCO/ADMAC** I²S fabric — both drivers present.
- **LP8559 backlight** over I²C — `lp855x_bl.c` present (may need an LP8559 compat).
- Keep **simple-framebuffer** for display bring-up.

### (3) Needs new driver work nobody has done on A13 (greenfield)
- **GPU — AGX G12.** No `drm/asahi` in the branch, no DT node, not in A13.md. The
  project's differentiated prize. **Highest effort.**
- **Native eDP/DisplayPort display** (`disp0,t8030`+`displayport,t8027`+`lpdp-phy`).
  The in-tree `adp` driver is **MIPI-only**; the iPad's eDP pipe is unsupported, and
  even MIPI native modeset is WIP. (Framebuffer-only bring-up avoids this.)
- **SPI-HID multi-touch** driver (iOS `multi-touch`/`hid-transport,spi`) — nothing
  comparable in mainline; bespoke.
- **`cs42l83` audio codec** driver — absent (only `cs42l84` exists upstream).
- Minor: `tcs3490` ALS and the accelerometer iio drivers (if the specific parts
  are unsupported upstream).

---

## Top 5 gaps, ranked by effort (largest first)

1. **GPU AGX G12 driver** — greenfield; no driver, no DT, firmware-ABI gap G12↔G13
   vs Asahi. The project's actual goal. *(Part C-3)*
2. **Native display for the iPad eDP pipe** — new DisplayPort/`lpdp-phy` + `disp0`
   KMS driver; the in-tree adp path is MIPI-only and still WIP even there. *(C-3)*
3. **SPI-HID multi-touch driver + DT** — new input driver for the iOS SPI touch
   transport; no mainline analog. *(C-3)*
4. **Audio stack** — write/port the **`cs42l83` codec driver** (absent upstream),
   then wire MCA/NCO/ADMAC + max98357 + cs42l83 into J181 DT. *(C-3 + C-2)*
5. **New `t8030-j181.dts`** — Wi-Fi, UART-BT (vs D421's PCIe-BT), gpio-keys,
   LP8559 I²C backlight, speaker amp; all drivers exist, so lowest-effort of the
   five but still net-new wiring. *(C-2)*

*Caveat:* Wi-Fi, BT, MIPI-display and NVMe are present in source but **not validated
in A13.md** — on-device confirmation is still required before calling them "working".
