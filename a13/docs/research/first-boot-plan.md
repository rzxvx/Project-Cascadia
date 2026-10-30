> Historical: the Phase 0-1 plan as written on 2026-10-29, before the first
> boot. What actually happened is in `bringup-status.md`.

# First boot: pwned DFU -> m1n1 -> Linux shell (Phases 0-1)

Chain: **usbliter8 (pwned DFU) -> iBSS -> iBEC (both from the IPSW, patched on
the host) -> m1n1 (`m1n1-idevice.macho`, booted by iBoot in place of the
kernelcache) -> Linux (`Image` + `t8030-n104.dtb` + initramfs, loaded over USB
by m1n1's proxy)**.

Everything up to iBEC is the existing usbliter8 tooling, used as is. Our work
starts where iBoot would load XNU.

## Step 0.1 - m1n1 tree (done 2026-10-29)

`m1n1/` = `HoolockLinux/m1n1` branch `idevice`, shallow, commit d5a10ac.

What it does on the A13 (source read):
- `src/midr.h` knows T8030 Lightning (0x12) and Thunder (0x13), but
  `src/chickens.c` has no entry for them: m1n1 prints `CPU part: 0x12` and runs
  the "Unknown" path (no chicken bits, `SLEEP_NONE`). Hoolock's D421 boots like
  this, so it is not a blocker for Phase 1. Small follow-up patch: A12/A13 rows
  with the A11 feature set.
- `m1n1-idevice.ld` links a Mach-O at VA 0xfffffff007004000, i.e. it is shaped
  like a kernelcache for iBoot to boot.

## Step 0.2 - build m1n1 (on the Mac, done 2026-10-29)

Built with Homebrew `llvm` 23.1.3 + `lld`, Rust 1.99.1, target
`aarch64-unknown-none-softfloat`:

```bash
cd m1n1 && PATH=/opt/homebrew/opt/llvm/bin:$PATH make -j8 build/m1n1-idevice.macho
```

Result: `build/m1n1-idevice.macho`, 917504 B (~896 KB). Shape (from
`m1n1-idevice.ld`, confirmed in the output file): ARM64 Mach-O, magic
`0xfeedfacf`, cputype `0x100000c`, filetype `0x02` (`MH_EXECUTE`), entry via
`LC_UNIXTHREAD`, VA base `0xfffffff007004000`. That is exactly the shape iBoot
boots as a kernelcache, so the tooling sends this file in the kernelcache's
place and runs `bootx` - nothing else in-tree is needed (no `remote_boot`
script exists in this fork; the idevice support is just this linker script and
the Makefile target).

Rust target, if a clean tree needs it: `rustup target add aarch64-unknown-none-softfloat`.

Python side for the proxy (done 2026-10-29) - `build/venv` (Python 3.15) with
`construct==2.10.70` + `pyserial==3.5`; `m1n1.proxy`/`m1n1.proxyutils` import
clean. Rebuild with:

```bash
cd m1n1 && python3 -m venv ../build/venv && ../build/venv/bin/pip install -r requirements.txt
```

## Step 0.3 - how the usbliter8 tooling boots

From Step 0.2 the format side is settled: `m1n1-idevice.macho` is a kernelcache-
shaped `MH_EXECUTE` at VA `0xfffffff007004000`. So the only open question is the
sequence the tooling runs after pwned DFU, and that it feeds this file where the
kernelcache goes, then `bootx`. What to record here from a real run:

- [ ] Tool + version used, host command line.
- [ ] Order: iBSS, iBEC, then what (logo? devicetree? kernelcache? `bootx`?).
- [ ] Which iOS version's iBSS/iBEC (16.6.1 pieces exist in the main tree,
      `a13_boot/ibootstuff/16.6.1`).
- [ ] Whether iBEC is patched to accept an unsigned `MH_EXECUTE` (it must, for
      m1n1 to replace the signed kernelcache) - note the patch/flag used.
- [ ] Whether a devicetree image is sent before `bootx` (it will be for Linux
      later; record if the tooling sends one for iOS too).

### Step 0.3 - live findings (2026-10-29)

Device confirmed `iPhone12,1 / n104ap`, ECID recorded.

Observed on the device with the host tooling (`usbobliter8`, wrapper
`usbliter8ctl boot <raw iBoot>`; device query via `irecovery -q`):

- From pwned SecureROM DFU (`MODE: DFU`, `SRTG: iBoot-4479...`) a direct
  kernelcache hand-off does nothing - expected, the first stage boots no
  kernelcache.
- `usbliter8ctl boot iBSS.patchboot` -> `MODE: Recovery`, serial populated.
  iBSS alone reaches a recovery command prompt ("Entering recovery mode,
  starting command prompt"). iBEC was not required to get a prompt.
- Handing m1n1 over as the kernelcache is rejected by iBoot:
  **`Kernelcache image not valid`**, for all three forms tried:
  raw `m1n1-idevice.macho`, `krnl` IM4P (plain), `krnl` IM4P (LZSS).
  So the recovery stage in use validates the kernelcache structure/trust
  and our image does not pass it.

Boot-method note (public Asahi/Hoolock overview, high level only): m1n1 has
two hand-off methods - **pongoOS** (`build/m1n1.bin`) and **iBoot**
(`build/m1n1-idevice.macho`, plus `build/monitor-stub.macho` on some devices);
full build is `CHAINLOADING=1`. Our build so far produced only
`m1n1-idevice.macho`.


## Step 1.1 - m1n1 alive

Pass criteria, any of:
- m1n1 logo on the panel (it draws on iBoot's framebuffer);
- a new USB device on the Mac: `ls /dev/cu.usbmodem*` shows m1n1's proxy port.

## Step 1.2 - proxy check

```bash
cd m1n1 && M1N1DEVICE=/dev/cu.usbmodemXXXX ../build/venv/bin/python3 proxyclient/tools/shell.py
```

In the shell: `p.read64(0x235200000)` style reads work, `u.adt` lists the ADT.
Record: MIDR, framebuffer base/size/stride, RAM size.

## Step 1.3 - Linux

Kernel tree: `yhavry/Hoolock-linux` branch `t8030` (to clone into `linux/`),
`defconfig` + Apple options, build in Docker or on the Arch box (not on the
Mac). DT: `t8030-n104.dtb` (USB gadget + simple-framebuffer only).

```bash
cd m1n1 && M1N1DEVICE=/dev/cu.usbmodemXXXX ../build/venv/bin/python3 proxyclient/tools/linux.py ../build/Image ../build/t8030-n104.dtb ../build/initramfs.cpio.gz
```

Pass criteria: kernel text on the panel (simpledrm), then a USB ACM/ECM gadget
on the Mac.

## Risks

| Risk | What we do |
|---|---|
| iBoot refuses m1n1 as a kernel (format, size, signature path) | compare with Hoolock's `remote_boot`; patched iBEC must accept an unsigned Mach-O |
| m1n1 runs but no USB proxy | the T8030 USB path in m1n1 `idevice` is unverified; fall back to the panel output |
| Linux hangs before the console | `earlycon` on uart0 needs a Lightning serial cable; otherwise m1n1 `bootlog` |
| CPU errata without chicken bits | only matters for stability under load (Phase 3, all cores) |
