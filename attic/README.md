
## Added 2026-09-16 (repo tidy-up)

| Item | Why it's here and not in the tree |
|---|---|
| `files-stale-dup/` | A top-level `files/` directory that duplicated `patches/files/` at an earlier point. It had since gone stale — the AIC driver there predates the re-arm fix, and `apple_sof_clkevt.c` / `apple_wdt_clkevt.c` were missing entirely. `patches/files/` is the single source of truth. |
| `drivers-loose-copy/` | A loose top-level `drivers/` holding two divergent copies of the USB OTG PHY driver. The current one now lives at `patches/files/drivers/phy/phy-apple-s5l-usb.c` and is installed by `apply-kernel-patches.py` like every other new file. |
| `apply-usb-phy.py` | Installed the PHY driver and its Kconfig/Makefile entries out-of-band. It was never called by `build-kernel.sh`, so a clean tree silently got no PHY. Its job is now done by `NEW_FILES` + `GLUE` + `PHY_KCONFIG` in `apply-kernel-patches.py`. |
| `arch_arm_Kconfig.patch`, `arch_arm_Makefile.patch`, `drivers_tty_serial_Kconfig.patch` | Generated diffs that were sitting loose in the repo root, byte-identical to the copies under `patches/`. |
| `_to_delete/` | Not history — actual junk (`.DS_Store`, `*.bak-*`, stray `.git/index.lock` files). Gitignored. Delete the directory whenever. |

## Added 2026-09-17

| Item | Why it's here and not in the tree |
|---|---|
| `apply-p105-boot-hacks.py` | Applied every edit to an existing kernel file by matching an anchor string and inserting near it. Superseded by `patches/tree/0001-cascadia.patch` + `scripts/apply-kernel-edits.sh`. Anchors made sense while the kernel version floated; once it was pinned they were only a liability, because `apply_replacements()` SKIPS a stale anchor silently. The first one it skipped was the `apple_aic1_rearm()` call in `init/main.c` — so a clean clone built green, booted, and took no interrupts at all. Kept for the record of which edits existed and why each was made; the reasoning in its comments is still the best explanation of the boot hacks. |
