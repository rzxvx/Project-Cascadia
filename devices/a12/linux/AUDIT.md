# XR kernel source audit (2026-11-04)

The existing build tree is `devices/a12/build/linux`, at HoolockLinux/linux commit
`6831bc701a6ce059e71e5aaa9488c9195bea6927`. It is a generated,
uncommitted build tree, **not** the patch source to publish.

Using a temporary Git index initialized from that commit, all 24 original
numbered patch files applied in lexical order with no
conflicts. The resulting tracked source matched the old worktree exactly
except one Makefile line adding `apple_hidspi_touch.o`. That line is staged by
`build-kernel.sh`; its untracked `apple_hidspi_touch.c` is byte-identical to
`devices/a12/linux/extras/apple_hidspi_touch.c`. The other four untracked source files
are added by the numbered patches. No unexplained source edits remain.

The old `.config` matches `devices/a12/build/kernel.config` byte-for-byte (SHA-256
`c24bdb11af73db206129ef78e807ab7db2ad09911728421e353c6b3621de6d50`).
The pinned Hoolock docs baseline `config_16k` hashes to
`a5cbcb0d3fe655888106672e6fe03477b63d51bd36bbcfc4f5145627d906b208`.
The known-good `Image.gz` hashes to
`4667bc3401a871797f5b400ce6fd7ac83798e092c467ca828a3efb2413d9f7c7`.

`prepare-kernel.sh` pins the source and config; `build-kernel.sh` now applies
the **complete** numbered patch series, not only its later incremental fixes.
The branch subsequently dropped three read-only Bluetooth diagnostics and
one failed no-flow experiment; the remaining 20 patches apply cleanly in
order. The corresponding diagnostic DeviceTree properties were removed,
while the working UART pins, RTS/CTS, baud, SMC power, wake GPIO and HCD path
remain. The cleaned source was rebuilt as kernel SHA256
`b98b87ec9c5ba0e4136d3eeb24495d95d4dada34d43fc87403c6a99e7fc8e57f`.
That kernel completed the phone-tested Debian/XFCE boot, touch, Wi-Fi and
Bluetooth sequence on 2026-11-04. A separate pre-push check applied all 20
patches in order to a clean archive of the pinned source; no diagnostic m1n1
CRC patch remains in the production series.
