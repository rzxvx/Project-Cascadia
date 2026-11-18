# docs/

| File | What it is |
|---|---|
| `QUICKSTART.md` | From a clean machine to Linux on the iPad (on its own NAND beside iOS, started from the Cascadia app, or over the cable from an NFS root): what hardware is needed, `./cascadia a5 install`, the boot chain, flashing, the desktop, and a table of what the failures look like. |
| `GPU.md` | The GPU: its OpenGL, the demos, SuperTux, how the driver works. |
| `CASCADIA-CHEATSHEET.md` | The working reference: boot chain, flash procedure, every address, every driver, and the full log of what was tried and what it did. Start here. |
| `k93-bringup.md` | The iPad 2 Wi-Fi (iPad2,1 / K93AP): how it differs from the mini, what works, the boot from iOS without a computer (and its dark screen), a second iPad 2 (iOS 8.4.1, PPN flash), the root on the NAND, the Linux volume, Linux's own partitions through iOS's FTL, iOS's System read-write on demand, the swap (zram) and the NAND server's GC, the desktop, YouTube, videos from any site, games. |
| `touch-next-steps.md` | Open plan for the multi-touch digitizer. |
| `kernelcache-keys.txt` | Public fwkeydb IV/key for the iPad2,5 12H321 kernelcache. Needed to decrypt the stock kernelcache for RE. Public information, mirrored here for reproducibility. |
| `research/` | Lab notes, one file per investigation. Several are dead ends — kept deliberately, see `research/README.md`. |
