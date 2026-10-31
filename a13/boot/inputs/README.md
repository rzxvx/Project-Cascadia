# Boot inputs you provide

Nothing from Apple is shipped with Cascadia. These four files are made on your
machine from Apple's firmware, with the tools named below. The checked
combination is **iOS 16.4 (20E247) for iPhone12,1 / N104AP**; the iOS installed
on the phone does not matter (these files only live in RAM).

| File | What it is | How to get it |
|---|---|---|
| `iBSS.n104.20E247.sigpatched.raw` | decrypted 20E247 iBSS with the signature check patched | `Firmware/dfu/iBSS.n104.RELEASE.im4p` from the IPSW, decrypted with its public key and patched with the iBoot patcher, both from [surrealra1n](https://github.com/pwnerblu/surrealra1n). The patch changes one instruction (file offset `0x293d8`: `e00314aa` → `000080d2`). |
| `IM4M` | an IM4M for CHIP `0x8030`, BORD `0x04` | any iPhone 11 (iPhone12,1) manifest, e.g. from a saved SHSH blob (`ipsw img4 im4m extract`). With the patched iBSS it is only the container's shape, not a personal ticket. |
| `DeviceTree.n104.rdtr.img4` | Apple's DeviceTree, as a restore DeviceTree | `Firmware/all_flash/DeviceTree.n104ap.im4p` from the IPSW: `ipsw img4 im4p extract DeviceTree.n104ap.im4p -o dt.raw`, then `ipsw img4 create --input dt.raw --type rdtr --im4m IM4M --output DeviceTree.n104.rdtr.img4` |
| `n104-mtfw.bin`, `n104-mtcal.bin` | the touchscreen controller's firmware and the default panel calibration | `Firmware/N104_Multitouch.im4p` from the IPSW: `ipsw img4 im4p extract N104_Multitouch.im4p -o mt.raw`, then `a13/tools/mtfw.py mt.raw C1FC2,2 n104-mtfw.bin n104-mtcal.bin` |
| `empty.rtsc.img4` | an empty RestoreTrustCache | a 24-byte version-1 trust cache with no entries (any UUID): `{ printf '\001\0\0\0'; head -c 16 /dev/urandom; printf '\0\0\0\0'; } > empty_trustcache`, then `ipsw img4 create --input empty_trustcache --type rtsc --im4m IM4M --output empty.rtsc.img4` |

The IPSW pieces can be fetched one by one with
`ipsw download ipsw --device iPhone12,1 --build 20E247 --pattern '...'`.

You also need the entry tool, usbliter8 (Paradigm Shift): its RP2350 board and
its `usbliter8ctl` client, which bring the phone to pwned DFU and send the iBSS.
Related tooling: [Leeksov/usbliter8ra1n](https://github.com/Leeksov/usbliter8ra1n).
Cascadia starts after pwned DFU.

Checksums of what the project was tested with. The iBSS and the raw
DeviceTree payload are deterministic and should match; the `.img4` files
depend on the IM4M you used.

```
5b77df1fceb136c42ced74003f6d8b5b1acacb56bda5f88d112895e4853034f2  iBSS.n104.20E247.sigpatched.raw
62b7e44eb0b3f220124283d012cacb7cfdef259444c6cf8e0539ad255dafe9e2  dt.raw (payload of DeviceTree.n104ap.im4p, 20E247)
de5dd11f00abae27b07f1a637686fcd03df748d1d4712a01592ac4bea4015036  n104-mtfw.bin
```
