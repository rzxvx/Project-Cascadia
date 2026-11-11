# G11 off-screen iOS reference capture (iPad 8 / 21B91)

`g11cap` is an **iPadOS-side** diagnostic, not a Linux boot payload. The A12
`Build A12 iOS GPU capture tools` workflow builds and signs `g11cap` and
`kmem` with Xcode's iPhoneOS SDK on a macOS runner. No Apple binary or capture
is stored in the repository.

The executable submits one 300×200 off-screen Metal render: red clear plus a
green triangle. It requires red at the corner and green at the centre before
reporting `PASS`. Nothing appears on the iPad display. It then records the
process's VM map and searches at most 128 MiB of readable VM-tag-100 regions
for the distinctive vertex buffer GPU address or marker, saving at most 64
small hit windows. It does **not** dump all process memory or automatically
capture the kernel TA/PM objects.

On a jailbroken iPad, copy both workflow artifacts to a writable executable
directory, then run as the mobile user:

```sh
g11cap /var/mobile/g11cap 100 120
```

Arguments are output directory, VM tag (default 100), and hold seconds
(default 60). Inspect `regions.txt` if no tag-100 hits appear; a different
tag may need a second capture. Keep the process alive during the hold window
while a root shell uses the existing `kmem` tool to locate the live
`AGXAcceleratorG11P_B0` IOService and its iPad-native firmware allocations.
`kmem`/A13 offsets are **not** interchangeable: validate the iPad object
layout from the iPad 21B91 kernel before `kmem kd` of TA, BMD, PM scene,
PM slot table, or page mappings. A completed command may be recycled before
the dump; if the captured TA cannot be tied to this render's process/job,
the next step is a pre-submit hold/hook, not comparing unrelated bytes.

This capture is read-only after Metal submission. Do not infer the Linux PM
fault's cause from `g11cap`'s user-space hits alone; the native kernel-side
structures and pointer provenance are the discriminating evidence.
