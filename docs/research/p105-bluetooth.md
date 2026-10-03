# P105 Bluetooth — the BCM4334's other half (2026-10-03)

The Wi-Fi chip is a BCM4334 combo; its Bluetooth half sits on UART1 and
`hci_bcm` drives it. Five things stood between the ADT and an LE keyboard
typing into the console. Each is small; none was guessable.

## What the ADT says

`/arm-io/uart1/bluetooth` ("bluetooth,n88"):

| Property | Value | Meaning |
|---|---|---|
| uart1 `reg` | `0x02600000` | UART1 at `0x32600000`, IRQ 22, clock gate 73 (on) |
| `transport-speed` | `0x2dc6c0` | iOS runs it at 3 Mbaud |
| `function-power_enable` | PMU, `GPIO`, 2 | REG_ON: PMU GPIO 2, register `0x61 + 2`, bit 1 the level |
| `function-bt_wake` | SoC GPIO `0x0101` | BT_WAKE, pin 9, active high |
| uart1 `function-rts` | SoC GPIO `0x0502` | pin 42, the RTS line, as a GPIO |
| `local-mac-address` | zeros | iBoot fills it from syscfg; this tree's copy is empty |

The PMU GPIO numbering checks out against Wi-Fi: its REG_ON is the ADT's GPIO 3
and has always been `0x64`.

## The five things

1. **The firmware.** iOS 8 ships no `.hcd`. `/usr/sbin/BlueTool` carries 87
   patchram images, one per chip and module, each a plain HCD stream
   (Write_RAM `0xfc4c` records, one Launch_RAM `0xfc4e`) naming itself, e.g.
   "BCM4334B1 37.4 MHz Borg TDK". Borg is the module the Wi-Fi firmware is
   named after (`wifi/4334b1/borg.trx`) and TDK the maker its NVRAM says
   (`borg-t-st.txt`). Three byte-identical copies; 73110 bytes, sha256
   `b6791a67…`. `scripts/extract-bt-firmware.py`, run by `./cascadia
   firmware`; btbcm loads it as `brcm/BCM4334B0.hcd`.

2. **REG_ON as a GPIO.** `hci_bcm` only raises the baud rate when it can reset
   the chip (`shutdown-gpios`); with REG_ON as a regulator it stays at 115200
   for good. `apple-pmu-i2c.c` now registers the PMU's output GPIOs as a
   gpiochip.

3. **RTS.** iBoot leaves pin 42 a GPIO driven high: RTS deasserted, so the
   chip never sends a byte, and every command timed out (`0xfc18 tx timeout`,
   `Reset failed (-110)`). iOS holds it so while the chip powers up and hands
   it to the UART after. A pinctrl state puts 40–43 on the UART.

4. **The UART clock.** `UCON` bit 10 selects the 24 MHz reference. iBoot sets
   it on the ports it uses — UART0 `0x5c85`, UART5 (HDQ) `0x405` — and leaves
   UART1 at reset, `0x1885` after `samsung_tty` had been at it: bit 10 clear,
   some other clock, a wrong rate. `samsung_tty` gains an
   `apple,s5l8940x-uart` variant whose reset value carries the bit. Without a
   fractional divider, 24 MHz gives 1.5 Mbaud (`UBRDIV` 0) as the fastest
   rate; that is `max-speed`. 3 Mbaud would need the other clock, whose rate
   is unknown.

5. **Two kernel details.**
   - The patchram reports `43:34:43:34:1F:AC` on every iPad. btbcm now knows
     it as a placeholder, so `local-bd-address` in the dts is used:
     `02:10:5A:05:00:04`, next to the gadget's and Wi-Fi's MACs.
   - Apple's patchram sets bits 4 and 5 of the legacy LE advertising report's
     event type (`0x20` for an ADV_IND, `0x24` for its SCAN_RSP, `0x13` for an
     ADV_NONCONN_IND). Linux dropped such reports whole ("unknown advertising
     packet type"), so the keyboard, seen by the controller at −49 dBm, never
     reached bluetoothd. `hci_event.c` masks the type to its low three bits.

And one config line: `CONFIG_CRYPTO_AES` was a module, nothing loads modules
here, and LE pairing (SMP) needs `cmac(aes)`: "Unable to create CMAC crypto
context", a connection that could never pair. Built in now.

## Result

510 HCI commands without an error at 1.5 Mbaud; LE and classic discovery; an
EPOMAKER TH40 (BLE HID) paired Just Works, bonded, and showed up as
`EPOMAKER TH40-3 Keyboard` and `… Mouse` input devices. bluez and dbus are in
the image and `bluetooth` (initramfs/usr/bin) runs them; in a RAM boot the bond
is gone at the next boot.
