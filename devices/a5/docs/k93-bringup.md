# iPad 2 Wi-Fi (K93AP)

The iPad 2 Wi-Fi (iPad2,1, K93AP, S5L8940X -- the original 45 nm A5) is a
board of the A5 tree now, built and booted like the iPad mini 1 with one
word changed:

```bash
./cascadia k93 firmware   # the boot chain out of iPad2,1_8.4.1_12H321_Restore.ipsw,
                          # touch/Wi-Fi/BT files and the panel's calibration out of
                          # the iPad's own jailbroken iOS (tools/k93-firmware.sh)
./cascadia k93 build      # the A5 kernel + config/k93ap.config, dts/k93ap.dts
./cascadia k93 flash --kdfu   # from the jailbroken iOS: kloader, iBEC, the bundle
```

`./cascadia k93 ...` is `./cascadia a5 ...` with `CASCADIA_BOARD=k93`: one
kernel source and one Alpine base for both iPads; the board picks the device
tree, an extra config fragment, the loader's framebuffer geometry, Apple's
files (`build/k93/firmware`) and the machine's own layer (`build/k93/keep`:
the ssh host key, this panel's touch calibration).  On the device,
`cascadia-board` says which iPad it booted on, and the helpers that only one
of them can run ask it first -- the mini's PPN NAND tools refuse to touch the
iPad 2's flash outright (tools/nand/ppn.c).

## Where it stands (2026-11-11, one iPad2,1 on iOS 6.1.3, p0sixspwn)

Booted from the jailbroken iOS 6.1.3 to Linux 6.12 -- with `./cascadia k93
flash --kdfu` from a computer (kloader takes 12H321's iBSS from there), or
with `linux` on the iPad itself (below).  iOS stays: Linux reads its System
partition and writes only blocks of the NAND that iOS keeps free and never
reaches (below):

| | |
|---|---|
| both CPUs, MPIDR 0x900/0x901, power-down idle as on the mini | works |
| L2 1 MB + CIF, AIC, its 24 MHz timer (self-test PASS), watchdog clocksource | works |
| `reboot` | works: the watchdog's bite, back in iOS by itself in 30-140 s |
| USB gadget (ACM + ECM, `02:93:00:00:00:01/02`), ssh | works; `ssh root@fe80::93:ff:fe00:2%<host interface>` needs no address on the host at all |
| simplefb 1024x768, console | works |
| buttons (gpio-keys) | all work (2026-11-10), through `p105-keys` as on the mini: volume up/down step the speaker's volume -- the CS42L63's master, 16 steps of 4 dB -- or, with the side switch flipped, the backlight, and repeat when held; Hold puts the screen out and back; Home held 0.8 s turns the rotation lock on or off (the backlight dips once or twice) |
| backlight | works over I2C (the PMU's WLED, 0xd0/0xd1/0xd2), 1796/2047 as iOS left it |
| battery (bq27540 over HDQ) | works: charge, voltage, current, cycles |
| charging | works with the charger armed from boot: iBEC leaves the limit at 100 mA, the iPad drained at -750 mA; armed, +340..470 mA |
| accelerometer (LIS331DLH) | works through st_accel; mount matrices in the landscape screen's axes (upright = Home on the left), so autorotate turns the desktop (2026-11-10) |
| gyro (L3G4200D: ADT "ap3gdl", WHO_AM_I 0xd3) | works through st_gyro, I2C2 0x68 |
| compass (AK8975) | works, I2C1 0x0e (uncalibrated: the cover magnets dominate) |
| light sensor (TSL2581) | works through tsl2583, I2C2 0x39 |
| touch | works: the digitizer's frames reach the kernel (24 + 28 n bytes: the Z2F13's touch record is the mini's 30-byte one without its last field, apple-z2.c takes the stride from the frame), and `touchscreen-swapped-x-y` maps the portrait digitizer onto the landscape framebuffer (found with squares in the four corners) |
| Wi-Fi (BCM4329 on SDIO) | works: brcmfmac loads iOS's own firmware at boot; `wifi connect SSID PASSWORD` joins (WPA2-PSK tested, 2-3.5 MB/s from Alpine's CDN), `wifi auto on` joins at every boot |
| booting without a computer | works: `tools/k93-pcless.sh install`, then `linux` as root on iOS -- Linux from the NAND in ~30 s (below) |
| Bluetooth (BCM4329C0 on UART3) | works: `bluetooth on` loads iOS's own patchram (BlueTool's `BCM43291A0 ... 2ND`, kept as `brcm/BCM4329C0.hcd`; `patches/tree/0005` tells btbcm the chip and its placeholder address 43:29:1A:0A:00:00, so the tree's 02:93:00:00:00:04 is used); classic only, no LE; pairing not yet tried |
| GPU (SGX543MP2) | works: the microkernel boots, the 2D engine draws (sgx2d's `sprites`: 200 sprites at 206 fps), and the mini's Mesa driver gives OpenGL ES 2.0 / GL 2.1 in the desktop root on the NAND -- Xorg's glamor, mpv's video |
| NAND (raw, under VSVFL and YAFTL) | iOS's partitions read-only: `ios mount` serves iOS's System off the flash and mounts it on `/mnt/ios` (`tools/nand/k93iosnand.c`, below): the map in ~5 s, 12 MB/s on both buses, byte-exact against known files.  Data is encrypted by iOS, file by file.  **Written**: the Linux volume, 333 MB on the 224 blocks of iOS's reserve Linux may own (below) |
| persistent storage | works (2026-11-10): the root's changes go to ext4 on the Linux volume and survive restarts and iOS sessions; 2.7 MB/s writes, ~5 MB/s reads (below) |
| a root bigger than RAM | works: an image on iOS's System partition (`tools/k93-root.sh install`), served off the NAND at boot and mounted under an overlay whose changes are kept on the Linux volume -- from iOS's `linux` to stage 2 in ~13 s; 430 MB of RAM free (below) |
| audio (CS42L63 on I2S0) | works: ALSA card `iPad2`, I2S0 + CDMA both ways at 44.1/48 kHz, the codec run with iOS's own sequences.  The speaker; **the headphones**, playback moving there by itself when a plug goes in and back when it comes out; **the built-in microphone** and **a headset's microphone**: when a plug goes in, the driver measures its mic contact as iOS does, sets the TS3A8235 to the plug's wiring (CTIA or OMTP) and moves capture to a mic it finds, biased while it records -- no clicks going in, recording or coming out ("Headphone Jack", "Headset Mic Jack").  Stereo, iOS's own input-gain tables, "Capture Volume" 0-128 = -96 to +32 dB.  All confirmed by ear and by recording (2026-11-10); a headset's button and Apple's remote not yet ([research/k93-audio.md](research/k93-audio.md) sections 5-6) |
| desktop | works: XFCE on the GPU (Xorg modesetting with glamor on the SGX543MP2, OpenGL 2.1), from the root on the NAND, started by itself at boot; touch, the on-screen keyboard (panel button), autorotate and its lock |
| YouTube | works: the desktop's YouTube launcher (yad search, mpv, yt-dlp): H.264 480p through the GPU with sound, ~1 frame in 700 dropped; ~30 s from Play to the picture after a boot, ~15 s after that (below) |
| games | work (2026-11-11): Game Boy, Game Boy Color and Game Boy Advance by touch -- the desktop's Games launcher (the games in `~/Games`) starts RetroArch full screen with on-screen controls drawn for this screen, several held at once (our RetroArch build: stock RetroArch reads no touch screen); mGBA and Gambatte at full speed, RetroArch's video on the A5's second core.  PSP games in PPSSPP (our build, three of its bugs fixed) with its own touch controls: the PSPSDK's samples at full speed, a demanding 3D game (God of War: Ghost of Sparta) in its menus at ~21% of its speed (~12.6 frames/s; 3% at first) -- the GPU's pixel programs the limit, being worked on (research/p105-mesa.md M46).  Games are copied in with `scp` or a file manager's `sftp://`; PSP games, bigger than the Linux volume, also stream from a computer (`tools/games/psp-share.py`) |
| clock | set by NTP once there is a network (stage 2); the build machine's time zone |
| cameras (the A5's H4 ISP: sensor 0x9726 rear, 0x7739 front) | work (2026-11-10): V4L2 devices `/dev/video0` (the rear: 1280x720, 1280x800, 640x360) and `/dev/video1` (the front: 640x480), YUYV at 30 fps, both pictures upright on the landscape desktop (the front one unmirrored, as V4L2 has it).  The ISP's own CPU runs iOS 8.4.1's firmware behind its IOMMU (`drivers/media/platform/apple/apple-h4isp.c`): booted when a camera starts streaming, powered down when it stops; one camera at a time.  In low light the firmware's exposure takes the rate down to 15 fps, as iOS lets it; `exposure_auto_priority` 0 holds the mode's rate.  Picture controls (2026-11-11): exposure bias (-2 to +2 EV), brightness, contrast, saturation, hue, sharpness (by default the firmware's own, which follows the light) and the mains frequency (50/60 Hz or the firmware's own detection), kept across streams and changed live.  `mpv av://v4l2:/dev/video1` shows it on the desktop; the firmware and the set-files come out of the IPSW with `./cascadia k93 firmware` ([research/k93-camera.md](research/k93-camera.md)) |
| USB host | not yet: waits for a 30-pin USB adapter |

### Touch: the Z2F13's bootload, as iOS 8.4.1 does it

AppleMultitouchZ2SPI's `MTSPIBootloader_Z2` (12H321, K93 kernelcache), with
`tools/z2-boot.c` options for each step:

1. enable_download asserted around the reset and held through the
   bootload (`-d`); without it the chip never enters HBPP and answers zeros;
2. the firmware (`K93.mtprops`, `Z2F13,2`), then the panel's calibration
   (syscfg MtCl, `mtcal.bin`) to `cal-dl-addr` 0x1000c000 (`-C`);
3. `performCalibSeq`, the Z2 one (`-Z`): unless the chip reads version
   0x5a020028 at 0x10008ffc, `fll-mval` (the K94 personality's 0x18da, `-F`)
   to 0x10001c04 (mask 0x1fff), 0x840000 to 0x10001c08 (mask 0xff0000), 5
   to 0x1000300c (mask 0x85), 0x20 to 0x1000304c; then `1f 01`, 65 ms, ack
   `4969`;
4. EXECUTE at `fw-execute-addr`, the Z2 default 0x10001800 (`-E`), and
   enable_download released at once -- held, the firmware runs for a moment
   and falls back into HBPP (`1f 01` forever);
5. the 32 kHz clock is PMU GPIO 0 at 0x51 (mode bits 7:5 = 010, as on the
   iPad 3's D1974); every other mode tried left the firmware dead (`4f81`).

Then device info `e2` (family 0x13, max packet 4066), GET `d9` (8 bytes:
14745 x 19660, 0.01 mm), SET `bf` = 9f 0b 0b 02 and `af` = 00, READY.

### Wi-Fi: what it took

Four things stood between the BCM4329 and a scan, each found on the iPad
(`tools/k93-sdio-probe.c` drives the controller by hand; booting with
`sdhci_apple_s5l.hold=1` keeps the MMC core off it until `echo 1 >
/sys/module/sdhci_apple_s5l/parameters/release`, so the whole card init can
be traced with the `mmc` trace events):

1. **An SDHCI that is a 2.00 one.**  The IOP firmware drives `arm-io/sdio`
   as an SDHCI 2.00 host.  The version register says 3.00, and the SDHCI
   core then writes Host Control 2 (0x3e) whenever it sets the bus timing:
   that write stops the card clock dead.  The driver reads the capabilities
   with the version forced to 2.00 and sets the clock the IOP's way
   (read-modify-write, power-of-two divisors).
2. **A CIS with no FUNCE.**  Apple's module carries its own common CIS:
   MANFID 02d0:4329, VERS_1 `s=C0 P=K94 m=1.0 V=a`, one vendor tuple (0x80,
   the module serial) and END.  No FUNCE, so Linux never learns a transfer
   speed and set the clock to 0 Hz right after the CIS; and the three
   functions have no CIS at all (FBR 0x109/0x209/0x309 all point at the
   END byte), so no `max_blksize` and 0-byte byte-mode transfers.  The
   driver's `init_card` gives the card the host's `max-frequency` as a
   default and `MMC_QUIRK_BLKSZ_FOR_BYTE_MODE`.
3. **A data buffer that nothing resets.**  The controller's buffer is two
   banks, and neither the DAT-line reset, nor a reset-all, nor the PMGR
   power state going off and on touches them.  A read whose data reached the
   buffer and was not read out -- any read that ends in a data error -- leaves
   the banks one apart, and every read after it returns the data of the one
   before.  brcmfmac runs into this on every boot: the chip answers its read
   of chipcommon `capabilities_ext` (0x180000ac, absent on this old
   chipcommon) with a bad end bit; every register read after it came back one
   late, the read-modify-write of the PMU control register wrote the PMU
   capabilities into it, and the firmware download then hung after two
   blocks with DAT0 held low.  After each DAT-line reset the driver now reads
   two words that never change (the CIS's first, CCCR 0x08..0x0b) with
   4-byte CMD53s; one late, it leaves one read unread across a reset, which
   swaps the banks back (`data buffer one word late after an error:
   realigned` in the log, once per boot).
4. **Apple's firmware** is a 4.221.122 `minioctl` build: no
   `SET_SCAN_CHANNEL_TIME`/`UNASSOC_TIME`, no `per_chan_info`, no
   frameburst, and scan results in the `wl_bss_info` version 108 format.
   `patches/tree/0004-brcmfmac-apple-minioctl.patch` makes brcmfmac take all
   four (108 differs from 109 in no field brcmfmac reads).

The bus runs at 24 MHz, 4 bits, default speed; iOS runs 51.3 MHz high
speed (ADT `sdio.card.clock-rate`, `speed-mode` 17), which needs a faster
base clock than the 48 MHz the capabilities give and the CCCR's high-speed
bit, which Linux ignores on a revision-1.10 CCCR.

### Booting Linux from iOS, without a computer

`tools/k93-pcless.sh install` (the iPad in its jailbroken iOS, on the cable)
puts the boot files on iOS's System partition and a command, `linux`; from
then on, `linux` as root on iOS (a terminal app, or ssh) boots Linux from
the NAND in about 30 s, and Linux's `reboot` returns to iOS.  It is the
mini's route ([research/p105-pcless-boot.md](research/p105-pcless-boot.md)):
kloader starts 12H321's iBEC, patched so that its OTA boot, `upgrade`, runs
by itself from System -- iBEC brings its own NAND, FTL and HFS+ code, so
Linux needs none.  The K93's iBEC is the same iBoot build as the mini's: the
code sites are at the same addresses, only the strings, the boot-command
table, the "Update" name and three call targets moved
(`scripts/patch-ibec-upgrade.py` has both boards).  On iOS:

    /kernelcache /ramdisk /devicetree     System's root, ~32 MB (the bundle
                                          of ./cascadia k93 build, wrapped by
                                          scripts/make-pcless.py --board k93)
    /usr/local/cascadia/kloader           Legacy iOS Kit's, hash-pinned
    /usr/local/cascadia/iBEC.upgrade      Apple's code, patched here
    /usr/bin/linux                        the command

Run `install` again after a build to update the kernel there.  This iOS
(6.1.3) has no `awk`, `wc`, `cut`, `head` or `sha1sum`: the installer
checks what arrived with `ls` and `openssl sha1`.  Everything came up on the
first boot this way: the USB gadget (the PHY driver programs the PHY iBEC
leaves off), Wi-Fi joining at boot, Bluetooth, touch.

### The GPU: the mini's, as it is

The SGX543MP2 is the mini's, and so is its microkernel: in the K93's 12H321
kernelcache, IMGSGX543's `__const` (the PDS templates) is byte for byte the
mini's, and of its `__data`'s 22565 words three differ (0x1607c, 0x16088,
0x1608c), each by exactly the kext's own displacement in that kernelcache --
kernel pointers, past the 0x15fe8 bytes the driver reads.  So
`scripts/extract-sgx-firmware.py --board k93` checks the used part
(0x16000 bytes) against the hash both share, and the driver needed nothing.
The GL driver's event template (the one PDS program the pack takes from iOS)
is the same 168 bytes, at 0x46dbd8 in the K93's dyld cache instead of the
mini's 0x46ebd8 (found by its hash; the root DMG `058-24482-024.dmg` opens with
its published key, The Apple Wiki's `Keys:Donner_12H321_(iPad2,1)`).  The
pack (`tools/sgx/mkpack.py`) is then the mini's, built from the mini's
kernelcache (the same code) -- and it draws: `sprites /usr/local/lib/sgx2d 200
300` ran at 206 fps on the K93's 1024x768 screen.

### iOS's System partition, read from Linux

The flash is raw NAND behind the H2FMI's BCH engines, under Apple's VSVFL
and YAFTL -- not the mini's PPN, so none of the mini's tools apply.  Every
layer is in [research/k93-nand.md](research/k93-nand.md);
`tools/nand/k93nand.c` drives the chips (PIO page reads, the ECC results, the
metadata's whitening, AES-128-CBC with the FTL's two keys), and
`tools/nand/k93iosnand.c` builds the rest on it:

1. **VSVFL**: the context of each virtual CE -- page 0 of blocks 1-199 names
   the four context blocks, the current one has the smallest usn (it counts
   down from 0xffffffff, which is where this iPad's still is), the newest of
   its sets of eight copies is the one, checked by its two checksums.  It
   gives the usable superblocks (1952 of 2048 per bank here) and the
   reserved-block map: blocks 0-19 of every CE (the boot area and the
   context blocks themselves) and 10-26 grown or factory bad blocks per CE
   live in the pool at 3904+.  Without the map, reads in such a block fail
   ECC: that is how it was found (System's dyld cache hit block 3418 of
   vCE 2, replaced by 3936).
2. **YAFTL, by restore**: the FTL's own context is not parsed.  As iBEC's
   "full NAND R/O restore" does: each superblock is classified by its first
   readable page, the user-data ones are walked newest first by the sequence
   number every page of a superblock carries, and each one's table of
   contents (its last page: one LPN per page, META key, IV = row) fills in the
   LPNs not seen yet; the one superblock still being filled is read page by
   page.  ~2 page reads per superblock: the map of all 1.74 M live LPNs in
   ~5 s.
3. **disk0 -> LwVM -> NBD**: a logical page is 8 KB, decrypted with the user
   key, IV = LPN; LwVM's header at LPN 0 has System (1.2 GB, 39 chunks of
   32 MB) and Data; each served partition is a read-only `/dev/nbdN`, and
   System mounts with Linux's hfsplus.

`ios mount` on the iPad (the mini's front end, which now picks the iPad 2's
reader) does it all: System on `/mnt/ios`, read-only.  Checked byte for byte:
`/usr/local/cascadia/kloader` hashes to the SHA-256 tools/k93-pcless.sh pins
it by.  A check that looks like a failure and is not one: the dyld shared
cache hashes differently in iOS (and differently at every iOS boot).  Of the
two 246 MB copies, 2% of the words differ, all in one 27 MB stretch (the
cache's data, 0x9518000-0xafbd558), and 99.5% of those by exactly the same
0x02073000: iOS 6 slides the cache's pointers in its page cache at boot
(ASLR) and its own reads of the file see the slid pages; the rest is the
Objective-C runtime writing into the same pages.  Linux reads the file as it
is on the flash, the same bytes at every boot.  Speed: ~7 MB/s of NAND pages one at a time by PIO, 12.2 MB/s with both buses
at once (`-j2`: FMI1's pages go to a second thread; `k93iosnand -j2 bench
System 1244` read all of System both ways, the same bytes), which `ios` and
the NAND root use.  Two things made it that fast: the AES
is table-driven (the byte-wise one in k93nand.c spent milliseconds a page),
and the map is kept in 8 MB blocks, cut down to what is served (16 MB for all
of disk0, 0.5 MB for System).  Nothing writes to the NAND: k93nand's register
allowlist has no program or erase command.

Data is out of reach: its pages are not under the FTL's user key but under
per-file keys (iOS's content protection), wrapped by keys behind the
device's UID key, so even served raw it would read as noise.

### The root on the NAND

RAM is 512 MB, and a desktop with a video player does not fit in it next to
itself.  So the big root is a squashfs image on iOS's System partition --
which Linux reads (above) but never writes -- under an overlay whose changes
are kept on the Linux volume (below):

    tools/k93-root.sh tree       the desktop root, built here (tools/desktop/
                                 make-root.sh: XFCE with touch, Mesa on the GPU,
                                 the GPU's pack, screenpad, YouTube), no SuperTux
    tools/k93-root.sh install    squashfs (zstd 19), copied to iOS as
                                 /cascadia/root.sqfs, checked, synced twice
    tools/k93-root.sh remove     back to the RAM root

At boot `initramfs/init` (`nand_root`) runs `k93nand id`, starts
`k93lv serve System` -- k93iosnand's reader serving System, plus the Linux
volume; `k93iosnand serve` when there is no k93lv -- in its own session, out
of the OOM killer's reach (every page the root reads goes through it), mounts
System read-only on `/mnt/ios`, the image on a loop device, the overlay's
upper layer (the Linux volume, else a tmpfs) over it, and switches root;
stage 2 is the image's own, refreshed from the initramfs like an NFS root's,
with the Wi-Fi networks of the keep layer.  `/run` -- the server's bus
locks (`/run/k93nand`), pid and log -- moves along, and `ios` knows the root
stands on System and will not unmount it.  Without the image, if anything
fails, or with `cascadia.root=ram` on the command line, the RAM root boots
as before.  First boot this way (a 16 MB test image of the RAM root itself,
2026-11-09): `linux` on iOS to stage 2 in 11 s, Wi-Fi joined, touch up, and
430 MB of RAM free instead of ~150 -- the initramfs is let go.

Since 2026-11-10 the overlay's changes are kept: `k93lv` (below) serves the
Linux volume next to System, and the init puts an ext4 on it (once, mke2fs)
and the overlay's upper layer in it.  `cascadia.lv=off` keeps them in RAM for
one boot; `cascadia-volume` says how full it is, `cascadia-volume reset`
empties it at the next boot.  A new root image gets an upper layer of its own
(the old one is kept as `old-upper`, its /home and /root move across), so old
changes never hide a newer image's files.

### Audio: the speaker, as iOS drives it

`sound/soc/apple/s5l8940-audio.c` and `sound/soc/codecs/cs42l63.c`, from
[research/k93-audio.md](research/k93-audio.md).  There is no MCA on the
original A5: I2S0 is the port, clock master, 16-bit stereo I2S, MCLK 6 MHz
(nclk/4) for the codec, BCLK from NCO 0 at 32 fs; CDMA channel 0x1f
(request line 1) feeds its FIFO, with the mini's ring and hrtimer.  The
CS42L63 has no datasheet and no Linux driver; the codec driver replays what
iOS's AppleCS42L63Audio does, step for step -- its sequences, taken by
running iOS's code in an emulator, compiled in from `cs42l63-seq.h`
(`tools/audio/k93-seq2c.py`, the same steps `k93-seq2sh.py` makes shell
scripts of).  Reset is PMU GPIO 13, the speaker amplifier's enable SoC GPIO
39, both through their gpiochips.  Two controls in iOS's units (0-62):
`Master Playback Volume` (the mixer's attenuation; 48 at boot) and `Speaker
Playback Volume`.  `/etc/asound.conf` makes "default" a 48 kHz dmix on card
0, on either iPad.

Checked on the iPad (2026-11-09): the card probes (`CS42L63, device ID 42 a6
30, revision b0`), `aplay` of a 3 s tone takes 3.13 s, and while it plays
the codec's registers are iOS's playing state (06=1e 07=67 08=7c 11=09 1d=19
1f=20=00 21=22=7f 3b=01), I2S0 runs (CLKCON 0x181, TXCOM 0x01feca06), the
switch routes pin group 0 (0x44, MCK 1), GPIO 39 is high (low again after).
And it is heard: every tone played through the driver came out of the
speaker.

### The desktop and YouTube

`tools/k93-root.sh tree` builds the mini's desktop root (XFCE with touch,
the patched X server and xfwm4, Mesa on the GPU, the GPU's pack, screenpad)
plus a `youtube` step, without SuperTux: 758 MB of files, a 262 MB image.
Stage 2 starts the desktop by itself on it (`/etc/cascadia/desktop-at-boot`,
which the image carries): X on the GPU -- "glamor X acceleration enabled on
PowerVR SGX543MP2" -- XFCE in ~40 s from `linux` on iOS.  The home is in
RAM, so every boot is a first login: the two first-login scripts that put
the keyboard and rotation-lock buttons on the panel restarted it with
`xfce4-panel -r`, which put up "Failed to restart the panel" at every boot;
they now ask over D-Bus, once the panel answers, without waiting.

YouTube (`tools/desktop/youtube-setup.sh`): a search box and a list (yad,
touch-sized), then mpv, with yt-dlp finding the streams.  What it took on
this CPU, measured:

- **The picture.**  480p H.264 decodes at 2.5x real time (FFmpeg's NEON
  code, chosen at run time even in Alpine's armhf build), but mpv's GPU
  output managed 30 s of video in 150 s: its default float framebuffers
  are copied by the SGX's Mesa on the CPU at every frame.  `fbo-format=rgba8`
  (and the fast profile) in /etc/mpv/mpv.conf: real time, 1 frame in 700
  dropped.  Software conversion is no way round it (80 ms a frame: FFmpeg's
  NEON YUV-to-RGB wants widths in 16s and BGRA).
- **The sound**: AAC stereo (the best m4a is often 5.1).
- **The wait.**  yt-dlp has YouTube's JavaScript challenge solved by Node:
  20 s on the A9s for each video, most of it parsing the 2.5 MB player.
  yt-dlp can keep the parse but ships with that off (big files, never
  rotated); the `cascadia_ejs_cache` plugin turns it on -- the cache is in
  the RAM home -- so the first video of a boot takes ~30 s to start and the
  next ones ~15.  The app clients that skip the challenge get no video
  without a "PO token" now (android_vr: HTTP 403; ios: images only).

The clock: no clock chip Linux reads, so stage 2 asks NTP once a route out
exists (~40 s into a boot with Wi-Fi), and the image takes the build
machine's time zone (`/etc/localtime`, exported as `TZ` for GLib).

### Games: the Game Boy, the GBA and the PSP by touch

`tools/desktop/games-setup.sh` (the root's `games` step): RetroArch with
Gambatte (Game Boy, Game Boy Color) and mGBA (Game Boy Advance) from Alpine,
our own touch controls, PPSSPP for the PSP (below), and a launcher -- a
touch-sized list of the games in `~/Games` (yad) that starts one full screen
and holds the screen in its landscape while it runs.  What it took,
measured:

- **The touch screen.**  Alpine's RetroArch (1.22.2) saw no touch screen at
  all: its udev input driver opens touch screens only when built with
  `UDEV_TOUCH_SUPPORT`, which nothing defines; built with it, a touch
  screen's pointer queries returned nothing (a `break` before the switch)
  and there was one pointer, not one a finger, so an overlay's controls
  could not be held together.  `tools/desktop/retroarch/build.sh` builds
  Alpine's package with the define and `retroarch-udev-multitouch.patch`
  (each finger down is a pointer of its own), for the A5 (ARMv7, NEON) and
  without Qt (30 MB less) -- 35 minutes under emulation.  Checked with
  touches written into the touch screen's event device (protocol B, as
  the panel reports them) and Tonc's `key_demo`, which shows the keys a
  game sees: D-pad + A, and L + R + START, held together.
- **Speed.**  Unthrottled on the GPU (the gl driver, the SGX's Mesa):
  Gambatte 68 frames/s, mGBA 64 -- too close to the GBA's 59.7.  With
  RetroArch's video on a thread of its own (the A5's second core): mGBA
  98, gpSP 247, Gambatte 440; playing, RetroArch takes 65% of one core.
  xfwm4 composited every frame again: with `video_disable_composition`
  RetroArch asks it not to (`_NET_WM_BYPASS_COMPOSITOR`) and the frames
  its video thread drops went from 2.8% to 1.2%.
- **The picture and the controls.**  The controls
  (`tools/games/make-overlays.py`, drawn for 1024x768; one image a
  control, so RetroArch lights it while it is held) sit in the margins
  of the picture at an integer scale: x3 for the GBA (720x480), x4 for
  the Game Boy (640x576).  RetroArch 1.22 adds a custom viewport's x and y
  to the centred position, so 0, 0 is the middle.
- **The screen held still.**  RetroArch reads the touch screen itself, in
  the panel's own landscape, past X's rotation; the launcher locks the
  rotation (`/run/rotation-lock`) while a game runs and autorotate takes
  over again after.
- **Copying games in.**  OpenSSH's `scp` speaks SFTP since 9.0, which
  dropbear hands to `/usr/lib/ssh/sftp-server`: `openssh-sftp-server`.

PSP games run in PPSSPP 1.20.4, which Alpine does not have:
`tools/desktop/ppsspp/build.sh` builds it -- the SDL frontend, desktop
OpenGL through GLEW, the ARM JIT, and the FFmpeg 3.0.2 it carries (its media
code wants that API; the release has the source, not the libraries) -- for
the A5, 107 minutes under emulation the first time, minutes after (ccache).
The launcher takes `.iso`, `.cso`, `.chd` and `.pbp` from `~/Games` and
starts PPSSPP full screen with its own touch controls.  What it took:

- **Three bugs of PPSSPP's own**, patched (`tools/desktop/ppsspp/*.patch`):
  - On ARM its CMake turns `USING_EGL` on for every build, and the SDL
    frontend then traded SDL's GLX context (desktop OpenGL 2.1) for an EGL
    OpenGL ES one, while the build calls desktop GL through GLEW:
    `glGetShaderPrecisionFormat` (desktop GL only with
    `ARB_ES2_compatibility`) was a null pointer, and the first frame
    crashed.  A desktop-GL build keeps SDL's context now.
  - A game streamed over HTTP hung PPSSPP for good wherever less than
    768 MB is free: its disk cache's `InitCache()` holds a mutex while the
    new cache, short of room, garbage-collects old files -- which takes
    the same mutex again.  It is recursive now; the cache turns itself off
    ("Not enough free space") and the game streams through RAM.
  - The touch controls never pressed: SDL's finger IDs are X.Org's touch
    IDs, which only grow (946, 947, ...), and the controls index masks and
    arrays of ten by them -- a touch still woke them up.  Each finger gets
    a slot of its own; several held together work.
- **The GPU.**  PPSSPP turns depth clamping on for its 3D draws wherever
  the driver has it, and the SGX's Mesa clamps depth on the CPU (its draw
  module, interpreted: research/p105-mesa.md M43) -- so not one of
  PPSSPP's draws had its vertex shader on the GPU, and God of War: Ghost
  of Sparta drew 2-3 frames/s even in its menus (3.3% of its speed), each
  frame ~370 ms of PPSSPP's GL thread.  The games step hides depth
  clamping from PPSSPP alone (a driconf file in the driver's
  `share/drirc.d`); PPSSPP's hardware transform stays on, vertical sync is
  off (55 frames of 60 with it).  The PSPSDK's samples run at full speed
  (reflection 60/60, spharm 21 -> 57-60 frames/s), God of War's menu at
  9%: then its frame was the GPU's pixel programs, not its vertices
  (M46).  The driver now lets opaque draws through the GPU's hidden
  surface removal, blends PPSSPP's colour masks in one instruction,
  copies PPSSPP's framebuffers on the GPU instead of waiting to copy them
  on the CPU, makes PPSSPP's programs shorter (most of them F16 now, their
  texture lookups the PDS's): the menu at ~21% (~12.6 frames/s on the
  screen).  What is left is in M46.
- **Games from a computer.**  PSP games are bigger than the Linux volume:
  `tools/games/psp-share.py DIR`, on a computer, serves its disc images the
  way PPSSPP's own "Share games" does (a listing, byte ranges; the
  computer's firewall has to let TCP 9123 in), and the launcher lists them
  too when `~/Games/psp-server` holds its address.  Over the Wi-Fi a game
  loads at ~1.1 MB/s.  A `.pbp` cannot be streamed -- PPSSPP runs one from
  its folder -- so those are copied in.
- **Leaving a game.**  The ≡ at the top opens PPSSPP's pause menu; its Exit
  (`--pause-menu-exit`) ends the game back in Games.  The touch controls
  fade after 8 s without a touch, and the first touch brings them back.

### Writing the NAND: the Linux volume

Writing through iOS's FTL is out (iOS mounts from its own context and would
erase what Linux wrote); [research/k93-nand-write.md](research/k93-nand-write.md)
has the other way: the program and erase sequences out of the I/O
processor's firmware (the A5's NAND driver runs there, not in the kernel),
and the VSVFL's reserve of spare blocks, which iOS takes from only when a
block wears out -- a band in the middle of it, 56 blocks per CE, that iOS's
allocator never reaches first.  Its tests T0-T5 passed on 2026-11-10
(`tools/nand/k93nandw.c`; the results are at the top of that note): all 224
blocks of the band free and erased on this iPad, programs and erases as the
IOP does them, and blocks left written came through an iOS session that
wrote 64 MB untouched, iOS's own maps byte-identical.

`tools/nand/k93lv.c` turns the band into a block device: page 0 of each
block a header, 254 pages of two 4 KB sectors with their numbers in the
page's metadata, a summary page; a write is on the flash before it is
acknowledged, garbage collection keeps a quarter of the space; at every
start the pool check runs again and only blocks that pass may be armed.
Its FTL is tested on the host against an in-memory flash with power cuts
(`k93lv selftest`, the -DK93LV_SIM build: 6 x 1000 rounds, 852 cuts, no
sector lost).  On the iPad: 333 MB, mounted in 0.5 s, 2.7 MB/s written
(the AES with tables: 0.4 ms an 8 KB page), ~5 MB/s read.

## The hardware, out of K93's own ADT

`tools/k93-extract-firmware.py` writes the 12H321 DeviceTree out as
`build/k93/firmware-12H321/adt.txt`; `scripts/adt-pmgr-map.py` reads its
`device-clocks` (44-byte entries here, 36 on the mini).  What it says:

| | iPad 2 (K93) | iPad mini 1 (P105) |
|---|---|---|
| CPU affinity | `0x900` | `0xa00` |
| framebuffer | 1024x768, stride 4096 (landscape scanout) | 768x1024, stride 3072 |
| PMU | Dialog D1946 at I2C0 `0x3c` | the same |
| AIC, GPIO, PMGR, watchdog counter, PL310 + CIF, OTG, SGX543MP2, bq27540 HDQ | the same blocks at the same addresses | |
| Wi-Fi | BCM4329 on **SDIO** (`arm-io/sdio`, SDHC at `0x30000000`, IRQ 39, SDIO-WIFI power state `0x3f1010b8`); REG_ON PMU GPIO 3 | BCM4334 on HSIC |
| Bluetooth | the BCM4329's, on **UART3** (`0x32800000`, IRQ 24); reset PMU GPIO 2, wake GPIO 7; patchram "BCM43291A0 ... Olympic Second Module" in BlueTool | BCM4334 on UART1 |
| touch | Zephyr2 `Z2F13,2` on SPI1 (`AppleMultitouchZ2SPI`, calibration at `0x1000c000`, fll-mval `0x18da`); reset GPIO 141, download GPIO 140, CS 159, ATTN 21; clock PMU GPIO 0, rail PMU LDO index 0 (`0x20` bit `0x20`) | N1 on SPI1, clock from the PWM |
| buttons | Home 0, Hold 1, volume 57/59, side switch 50; Smart Cover = PMU GPIO 14 | 0..4 |
| backlight | the PMU's WLED, level sent through the **DWI** block by iOS | the PMU's WLED over I2C |
| audio | Cirrus CS42L63 codec at I2C0 `0x4a`, on I2S0 | CS35L19 amps on MCA0 |
| sensors | LIS331DLH accel (I2C2 `0x19`), AK8975 compass (I2C1 `0x0e`), TSL2581 light (I2C2 `0x39`), "ap3gdl" gyro (I2C2 `0x68`) | LIS3DSH, gyro, TSL2581 |
| NAND | H2FMI with its BCH engines (`FMIx-BCH` clocks); iBEC carries raw and PPN paths | PPN |

The SDIO host is a standard SDHCI: iOS reaches it only through the IOP, whose
firmware (in the kernelcache, section `fwFirmwareImage`, "EmbeddedIOP for
s5l8940xiop") programs clock control at `+0x2c`, software reset at `+0x2f`,
the interrupt status/enable registers at `+0x30..+0x36` and feeds the data
port at `+0x20` through CDMA -- SDHCI v2's layout.  Linux drives it directly:
`drivers/mmc/host/sdhci-apple-s5l.c`, PIO.

## History: the experimental RAM boot (2026-10)

Hardware tested: iPad2,1, S5L8940 / CPID 8940, BDID 04, K93AP.
This is an initial bring-up, not parity with the P105 port or an installer.
No Apple firmware binaries, device identifiers, credentials, or personal logs
are included. This workflow does not mount or write internal storage.

## Verified on hardware

- Linux 6.12 reaches the embedded `/init` and an interactive root shell over
  CDC ACM (`/dev/ttyACM0` on the Linux host).
- USB gadget enumerates as 0525:a4aa; USB interrupts reach the CPU.
- CPU MPIDR is 0x80000900, not the P105 CPU affinity.
- The watchdog counter at physical 0x3f103020 is a single 16-byte bank running
  at 24 MHz. The existing read-only platform clock code measures CPU frequency
  at approximately 1 GHz and registers the counter as clocksource/delay timer.
- The existing AIC CPU0 timer reports a successful 24 MHz clockevent self-test.
- simplefb registers 1024x768, 32 bpp, stride 4096 at 0x9f6fc000 and switches
  the console to 128x48. GUI4 also rendered a native Xorg/TWM/xterm desktop
  through fbdev; touch and everyday desktop usability remain unverified.
- `/proc/mounts` contains only rootfs and pseudo/RAM filesystems.
- GUI3 now also boots a complete Alpine3.24.2 ARMv7 root as read-only
  SquashFS plus bounded RAM overlay. A small boot payload avoids the observed
  Recovery transfer cutoff; the separate hash-pinned image is pushed over
  direct USB IP, with ACM kept closed during that transfer. Guest hash,
  mount/memory proof and native Xorg/xterm **version execution** are recorded
  in local `build/k93/bundle-gui-v3/root-verified.json`. About387MiB was
  available before starting graphics. Version execution alone is not evidence
  of rendering; the separate GUI4 framebuffer capture below provides that.

Direct USB IP networking is now verified by repeated scoped host/guest ping
tests (V41–V44), without Internet routing. Touch, Wi-Fi, audio, suspend, CPU1,
persistent storage and browser usability are **not verified on K93**.
Do not inherit P105 support claims. Current touch diagnostic sessions reset
after120s; the original V8 session described below used105s. See
the GUI4 evidence section below for the newer userspace result.

## GUI4 native framebuffer desktop (2026-10-18)

On physical K93 hardware, Alpine 3.24.2 ARMv7 ran from a read-only SquashFS
image with a 64 MiB RAM overlay, 96 MiB `/run` and 32 MiB `/tmp`. Xorg
1.21.1.25, TWM and xterm 410 started successfully. A read-only `/dev/fb0`
capture was decoded and visually inspected: the TWM title bar, terminal text
and shell prompt were rendered at 1024x768. This is software framebuffer
graphics, not GPU acceleration or a usable touch/browser demonstration.

The guest diagnostic sources are `rootfs/k93/{squash-init,alpine-init,
desktop-test.sh,xorg-fbdev.conf,twm-test.conf}`. The desktop is manual and
bounded: Xorg has a 35-second limit, clients 30 seconds, private MIT-cookie
authentication and TCP listening disabled. Input auto-discovery is disabled;
the diagnostic terminal runs as root, not as a production browser account.
`tools/k93-fb-read.c` accepts only the verified K93 framebuffer mode and
performs read-only GET ioctls/pixel reads. The decoder requires successful
test/cleanup markers and explicitly requires human/visual review afterward.

Evidence digests from this run:

- Full root image: 67,293,184 bytes, SHA256
  `d7413f2ca57ca2629e624b44e73b6e00d54d889527252eb40b11fdf711fc4d5f`.
- Boot bundle: 11,272,707 bytes, SHA256
  `7a55dbc59c9273ec51c56963c1ba5cf3656a203b5e70ec4ecec0baff7e3c9808`.
- Captured RGB PPM SHA256:
  `10da2714202408d956b5f142643b947615beb9a48421676ea5da7a1a1666a49b`.
- Converted PNG SHA256:
  `07b6cbc65246fbd7c254140b4a6ef570e47aa72749ca8f981842d3ffa304b623`.

All three Recovery uploads reached 100%. Linux USB appeared after 3.321s.
The separate root image was pushed over direct USB IP; the guest checked its
hash before mounting. Keep ACM closed during the bulk ECM transfer: an
earlier overlapping capture stalled the transfer. Root-transfer TCP output
drained before host network cleanup. Subsequent USB ping returned 4/4 packets,
zero loss, average 0.320ms. The watchdog reset at 120.117s; iOS USB returned
at 183.419s, and a new boot with automatic root access was verified afterward.
No internal storage was mounted or written.

The observed Recovery transfer cutoff makes embedding this full image unsafe;
the tested split-image bootstrap receives only on guest USB 10.93.0.2:48932,
with host peer 10.93.0.1. It does not route Internet traffic. The image builders
and diagnostic sources are published here; the experiment-specific kernel
configuration, device-selection/transport harness and jailbreak credentials
are not. This is not a turnkey reproduction recipe or persistent installer.
The original diagnostic build recipe below remains a separate baseline.

Host-only checks (no device or mount access):

```sh
TMPDIR=/var/tmp python3 tools/k93-test-fb-read.py
python3 tools/k93-test-desktop-frame.py
python3 tools/k93-test-squash-init.py
python3 tools/k93-test-alpine-root.py
```

## Important calibration dependency

Without the `apple,s5l8940x-wdt-timer` node, the watchdog clock/delay timer is
not registered. The PMCCNTR fallback skips registration in an SMP build, even
with `maxcpus=1`. Generic delay calibration then waits for ticks before the
later AIC timer initialization can provide them. Hardware trace stopped inside
`calibrate_delay()` in this configuration. Adding the verified counter node
allowed calibration to be skipped using its frequency and userspace to boot.

## Reproducing the tested diagnostic build

Use the upstream toolchain and patched Linux tree at `build/linux`; run build
commands in the same environment/path namespace (e.g. the project's container).
The Python preparation step needs network access for its SHA256-pinned official
Alpine BusyBox package. Firmware extraction also needs PyCryptodome.

```sh
python3 tools/k93-prepare-root.py
CONFIG_FRAGMENT="$PWD/build/k93/k93.config" \
INITRAMFS_DIR="$PWD/build/k93/rootfs" bash scripts/build-kernel.sh
bash tools/k93-build-kernel-trace.sh --late-stages
python3 tools/k93-build-bundle.py --counter-clock
```

Output is `build/k93/bundle-diag-v8-wdt120/`, with a hash manifest. The bundle
contains the loader, padding to the kernel entry, traced zImage, and appended
DTB. The diagnostic loader uses stack 0x80007000, below the kernel at
0x80008000, and the verified physical watchdog base (not the P105 default).

The optional trace mechanically instruments the ignored kernel tree using
unique, prevalidated anchors. It preserves upstream source outside the selected
sites; repeat application is idempotent. It adds temporary identity mappings
for diagnostics and must not be treated as a production MMU configuration.
Late C checkpoints set watchdog reset deadlines to 30/45/60/75/90/105 seconds:
post-console check, before/after delay calibration, before `rest_init`, before
first schedule, and init task before `kernel_init_freeable`, respectively.
These are absolute counter deadlines, not delays at each checkpoint.

## Boot chain

`tools/k93-extract-firmware.py` reads a user-supplied stock
`iPad2,1_8.4.1_12H321_Restore.ipsw` in the repository root. It uses public
firmware decryption material and validates the board. Then:

```sh
python3 tools/k93-extract-firmware.py
python3 tools/k93-build-bootchain.py
```

The latter needs upstream `iBoot32Patcher` and builds into
`build/k93/bootchain-v2`. Its iBEC trampoline sits in preserved text alignment
padding, not the file tail/BSS (which startup erases). Layout, hook bytes,
relocation survival, changed-byte scope and plaintext IMG3 packing are checked.
The plaintext iBEC is for an already exploited kDFU boot chain; stock DFU is
not sufficient. Establishing kDFU is deliberately outside these offline tools.

The tested upload sequence is patched iBEC, wait for Recovery, staging bundle,
then the matching standalone loader if Recovery remains responsive. Select
and verify the exact device before every transfer. Do not run a restore or
flash persistent storage. Open the CDC ACM console promptly: the diagnostic
watchdog still resets the machine at its last checkpoint deadline.

Tested V8 bundle SHA256:
`db114e51c9d2acf1ae3153f68b0ab31b7d3f1d61ab38423fd1f101be320e4281`.
Build timestamps can change the kernel/bundle hash on a rebuild.

## Controlled reboot from a working K93 RAM session

The local `pongo/k93_user_watchdog.S` helper is a tiny static Linux ARM ELF,
built offline by `bash tools/k93-build-user-watchdog.sh`. It first validates
the compatible string `apple,k93ap`, maps only the verified watchdog page,
requires CTRL=0 and checks that CUR_TIME advances. With no arguments it is
read-only and prints `K93_WDT_AUDIT_OK`. Only explicit `--reset` arms a
five-second reset: BITE_TIME=current+120000000, then CTRL=4. It never resets
CUR_TIME, never accesses storage, and refuses deadline addition overflow.

Exit codes:2 invalid arguments,3 wrong/missing board,4 mapping failure,
5 unexpected control state/readback,6 stopped counter,7 impending counter
wrap. Do not bypass these checks or use this helper on another board.
Run the read-only audit first and retain a recovery path. The reset mode was
tested on the live V11 K93 session: it returned `K93_RESET_ARMED_5S`, followed
by USB disappearance and normal iOS enumeration without button presses.
This does not itself prove that untether/root recovery completed.

Validated build:816 bytes, SHA256
`956994bb6482309446f63a83e8a3aecac67aa35e5c51138637da665b246f4c36`.
It can be transferred to `/tmp` over the known USB shell using BusyBox base64;
never write it to internal storage. Invalid-argument and wrong-board guards
were checked with host-side ARM emulation; MMIO/reset execution was only on K93.
