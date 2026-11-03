# RAM boot via checkm8 (iPad mini 1 / P105AP)

Boot Alpine/Linux entirely from RAM using **pwned DFU + irecovery**. No
jailbreak SSH, no kloader, no CoolBooter repartitioning, nothing written to
NAND.

## What you need

| Piece | Location / notes |
|---|---|
| Patched iBSS + iBEC | `ibec/iBSS.patched.dfu`, `ibec/iBEC.patched.dfu` — `make ibec` |
| Kernel + DTB + rootfs | `build/out/zImage-dtb` — `bash scripts/build-ram-boot.sh` |
| **checkm8 pwn (A5)** | **Raspberry Pi Pico** with [checkm8-a5](https://github.com/LukeZGD/checkm8-a5) UF2 for **8942** — see below |
| **gaster** (checkm8) | Works on A6+ from a PC; **does not reliably pwn A5/8942 from Windows** |
| **irecovery** | `tools/libirecovery-win/irecovery.exe` (native, libusb) |

## A5 checkm8: PC gaster is not enough

The iPad mini 1 uses **S5L8942X (CPID 0x8942)**. This is a known limitation:

- Windows **does** see DFU (PID 1227) and Zadig/WinUSB can bind correctly.
- `gaster` opens the device, but **USB control transfers return 0 bytes** on A5
  when driven by a normal PC USB stack. It never reads the serial/SRTG string,
  never prints `CPID: 0x8942`, and loops at "Waiting for the USB handle…"
- This is **not a broken gaster build** — axi0mX/checkm8-a5 document that A5
  needs a **raw USB host** (Pico or Arduino+MAX3421E) because the OS sends
  standard USB setup packets before user tools can run the exploit.

**Practical options for this iPad:**

| Method | Hardware | Works from PC? |
|---|---|---|
| **checkm8-a5 Pico** | Raspberry Pi Pico (~$5) | Yes, after Pico pwns |
| **checkm8-a5 Arduino** | Arduino + USB Host Shield | Yes, after Arduino pwns |
| **Jailbreak kDFU app** | None (iOS already jailbroken) | Sometimes — enters pwned DFU from iOS |
| **gaster / ipwndfu on PC** | USB cable only | **No** (A5 USB stack timing) |

### Pico setup (8942 — iPad mini 1)

1. Download UF2 for **8942** from [Legacy-iOS-Kit checkm8-pico](https://github.com/LukeZGD/Legacy-iOS-Kit-Keys/releases/download/a/checkm8-pico.zip) (file `8942.uf2` or similar).
2. Hold **BOOTSEL** on the Pico, plug into PC, copy the UF2 onto the `RPI-RP2` drive.
3. Wire: Pico USB to PC (power), Pico USB-A host port to iPad (DFU).
4. Put iPad in DFU → Pico LED blinks → device becomes **pwned DFU**.
5. On the PC (WinUSB via Zadig on the pwned device), run irecovery only:

```text
irecovery.exe -q
irecovery.exe -f ibec\iBSS.img3
irecovery.exe -q                    rem SRTG must show iBoot-2261, NOT rom
irecovery.exe -f ibec\iBEC.img3
irecovery.exe -q                    rem MODE: Recovery, PID 1281
irecovery.exe -f build\out\zImage-dtb
irecovery.exe -c "getenv loadaddr"
irecovery.exe -c "go <loadaddr>"
```

Or use `host-boot-checkm8.cmd -SkipPwn` after the Pico has pwned the device.

Close **3uTools** completely before boot — it installs an Apple USB driver that
blocks libusb/gaster.

### Windows USB driver (one-time, required for gaster)

Windows sees DFU in Device Manager, but **gaster waits forever** until the DFU
device uses a **WinUSB** driver instead of Apple's.

Your iPad currently shows:

- `Apple Mobile Device USB Device` (PID 1227)
- Driver: `oem155.inf` (Apple / 3uTools) — **libusb cannot open this**

Fix with [Zadig](https://zadig.akeo.ie/) while the iPad is in DFU:

1. Quit **3uTools** entirely (tray icon too).
2. Put the iPad in DFU (black screen, PID 1227).
3. Run **Zadig** as Administrator.
4. Menu **Options → List All Devices**.
5. Select **Apple Mobile Device (DFU Mode)** or the entry with `PID 1227`.
6. Set the target driver to **WinUSB**.
7. Click **Replace Driver** / **Install Driver** and wait until it succeeds.
8. Unplug/replug USB, stay in DFU, run `gaster.exe pwn` again.

After pwn succeeds, `irecovery -q` should return device info (ECID, CPID, etc.).

To restore Apple's driver later (for 3uTools SSH on iOS): Device Manager →
right-click the DFU/recovery device → **Uninstall device** → check **Delete
driver** → replug on iOS. Or reinstall 3uTools / Apple Mobile Device Support.

## Terminology

| Term | On A5 (S5L8942X) |
|---|---|
| **DFU** | Black screen, USB PID **1227**. Bootrom, not yet exploited. |
| **pwned DFU / checkm8** | Bootrom exploited; host can upload via `irecovery`. |
| **Recovery / iBEC** | USB PID **1281** after iBSS chains to iBEC. Has `go` command. |
| **After iBSS runs** | Still PID **1227**, but **SRTG** changes from `rom` to `iBoot-2261` |
| **kDFU (checkra1n pongoOS)** | Does **not** work on A5 — pongoOS is AArch64-only. |

## Build (WSL, one time)

```bash
cd ~/a5-linux-dt-port
bash scripts/build-ram-boot.sh
```

Copy `tools/gaster.exe` to `tools/gaster.exe` on Windows if you build in WSL.

## Every boot

### 1. Enter DFU manually

1. Connect USB (direct to PC, not through a hub if possible).
2. Hold **Home + Power** ~10 s.
3. Release **Power**, keep **Home** ~10 s more.
4. Screen stays **black**.
5. Device Manager should show **Apple Mobile Device (DFU Mode)** — PID 1227.

### 2. Pwn the device, then upload (Windows)

**A5:** pwn with Pico (or kDFU app on jailbroken iOS), not `gaster pwn`.

Then either run the host script skipping gaster:

```powershell
cd path\to\iBSSloader\scripts
.\host-boot-checkm8-native.cmd
```

Or WSL (upload may not execute iBSS — prefer native above):

```powershell
powershell -ExecutionPolicy Bypass -File .\host-boot-checkm8.ps1 -SkipPwn -UseWsl
```

### Manual equivalent

```text
irecovery.exe -f ibec\iBSS.img3
irecovery.exe -q
irecovery.exe -f ibec\iBEC.img3
irecovery.exe -f build\out\zImage-dtb
irecovery.exe -c "getenv loadaddr"
irecovery.exe -c "go <loadaddr>"
```

Order matters: **iBSS first**, then **iBEC**, then the kernel.

## Why one zImage-dtb file

iBoot's `go` does not set ARM Linux register args (`r0`–`r2`), so there is no
clean way to pass a separate initrd pointer. The DTB is appended
(`CONFIG_ARM_APPENDED_DTB`) and the rootfs is embedded
(`CONFIG_INITRAMFS_SOURCE`). One upload, no address guessing.

## Back to iOS

Hold **Home + Power** ~10 seconds. Nothing was written to NAND.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| gaster **Waiting for USB handle PID 1227** forever (WinUSB already installed) | **A5/8942 cannot be pwned from PC USB** | Use Pico checkm8-a5, or jailbreak kDFU app, then irecovery |
| gaster waiting before Zadig | Apple/3uTools driver, not WinUSB | Zadig → WinUSB on DFU device; quit 3uTools |
| `gaster pwn` fails | USB timing / hub / cable | Direct port, retry, different cable |
| irecovery prints Usage/help | Device not pwned or 3uTools owns USB | Close 3uTools USB; rerun gaster |
| irecovery Unable to connect (WinUSB/libusbK) | usbipd still attached, or wrong Zadig driver | `usbipd detach --busid 2-1`; try **WinUSB** in Zadig; see `tools/libirecovery-win/README.txt` |
| SRTG still `rom` after iBSS (WSL path) | usbipd breaks DFU execute handshake | Use **native** `host-boot-checkm8-native.cmd` instead of `-UseWsl` |
| Stuck on PID 1227 after iBSS | iBSS did not run (check **SRTG**, not PID) | Same as above — SRTG must show `iBoot-2261` before iBEC |
| PID 1281 but upload fails | Wrong irecovery / driver conflict | Close 3uTools; replug USB |
| PID 1281, upload 100%, still 1281 | **irecovery -c dead on P105 Recovery** | iBEC must auto-jump after upload (patch in progress) |
| `go` / `setenv` / `getenv` all silent | USB **bulk (-f) works; control (-c) does not** | Do not rely on host commands — patch iBEC binary |
| Black screen after upload | Jump never happened (see above) or wrong FB in DTB | Fix auto-go iBEC first; then `fdt_fixup.py` vram |
| Clocksource panic | Expected first success | Rebuild with `USE_A9_TIMERS=1` |
| Immediate reboot after jump | Wrong load address or corrupt image | Check staging-bundle layout |

## Expected first Linux boot

- **Clocksource panic** with `p105ap.dtb` = success (DT parsed, AIC bound).
  Rebuild with `USE_A9_TIMERS=1 scripts/build-ram-boot.sh` and retry.
- **Black screen, no panic** = framebuffer base wrong; fix DTB and rebuild.
success (DT parsed, AIC bound).
  Rebuild with `USE_A9_TIMERS=1 scripts/build-ram-boot.sh` and retry.
- **Black screen, no panic** = framebuffer base wrong; fix DTB and rebuild.
.
success (DT parsed, AIC bound).
  Rebuild with `USE_A9_TIMERS=1 scripts/build-ram-boot.sh` and retry.
- **Black screen, no panic** = framebuffer base wrong; fix DTB and rebuild.
