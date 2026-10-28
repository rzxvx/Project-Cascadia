# P105 PPN error-list evidence: bounded offline parsing only

Research date: 2026-10-11. This records static inspection of Cascadia's
documented iPad2,5 firmware, not a device test or a writable-driver design.
No firmware was executed or emulated. No device, NAND operation or ioctl
was used. The connected unit's 13G36 firmware was not inspected here.

## Artifact identity

Source: Apple's
[iPad2,5 8.4.1/12H321 IPSW](https://secure-appldnld.apple.com/ios8.4.1/031-31249-20150812-75235CC6-3C8F-11E5-848D-BE1A3A53DB92/iPad2,5_8.4.1_12H321_Restore.ipsw),
already selected by Cascadia. Local extracted Mach-O kernelcache SHA256:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Embedded IOP image SHA256:
`6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4`.
Hashes identify the inspected artifacts; they do not establish a new Apple
signature verification. Firmware bytes and complete instruction inventories
remain outside the repository.

The prelink inventory distinguishes `AppleIOPFMI` (AP kernel VA
`0x80c4f000`) from `IOP_s5l8942xiop_firmware` (kernel VA `0x80aaf000`).
Its `fwFirmwareImage` section is `0x24000` bytes at VA `0x80ab1000`.
IOP addresses below are offsets in that section, whose startup switches
from ARM into Thumb. AP addresses below are kernel virtual addresses.

## Independently observed field accesses

These are selected field observations, **not a complete ABI**. `C` denotes
the outer command buffer, `R` a translated PPN descriptor, and `L` the
separate error-list scratch buffer.

| Object | Observed access | Original callsites |
|---|---|---|
| `C` | AP clears 512 bytes; sets opcode at `+0`, channel index at `+0x10` | AP `0x80c54f80` |
| `C` | Data/meta list addresses at `+0x1c/+0x24`; lengths at `+0x20/+0x28`; descriptor address/length at `+0x2c/+0x30` | AP `0x80c57520–0x80c5754c`; IOP `0x8176–0x81c0` |
| `C` | Data/meta segment lengths use a count shifted by three; AP segment helper stores two 32-bit words per entry | AP `0x80c57526/0x80c57534`, `0x80c56df0/0x80c56df8` |
| `R` | Request count is an unsigned 16-bit field at `+8`; zero and counts above an internal queue limit take error paths | IOP `0x93d0–0x93ee` |
| `R` | Entries start at `+0x20`, stride 12; matching uses 32-bit page at `+0` and an 8-bit CE index at `+4` | IOP `0x64a8`, `0x64cc–0x64e6` |
| `R` | Matching entry's raw byte at `+6` is updated; aggregate result byte is separate at `+0xc` | IOP `0x650a–0x650e`, `0xa03c` |
| `L` | First 32-bit word is a count; subsequent 32-bit words are page addresses; observed loop checks count, not available buffer length | IOP `0x6496`, `0x64bc`, `0x6538/0x653a` |
| `L` | Scratch allocation requests 512 bytes; all three failure-list feature reads request 512 bytes into the same scratch pointer | IOP `0x77c8–0x77d6`; `0x9f42–0x9fe4` |

The three feature-reader callsites select `0x3080`, `0x3280`, and `0x3180`
and immediately reconcile the resulting list against the original request.
Diagnostic associations distinguish failed, pending, and retirement lists.
They do not prove retry, durability, or the meanings of every status bit.
The later `0x3380` call uses the feature **setter**; it is not a read-only
query and its acknowledgement/state meaning remains unproven.

For a future strict parser, `(512 - 4) / 4 = 127` is the largest count that
fits this buffer. **127 is our derived parser bound, not a proven firmware
or hardware limit.** A list contains no per-entry CE or status field:
those belong to the caller context. CE index must remain opaque; it is
not yet mapped to physical CE pins or Cascadia's four MTD die devices.

## Ownership evidence and unresolved boundaries

AP command submission stores a DMA-object context at `C+0xc`, increments
its pending count, and invokes a channel endpoint (`0x80c55d72–0x80c55d90`).
The event routine retrieves that context and channel (`0x80c51172/0x80c5117a`),
decrements the pending count, and calls completion when it reaches zero
(`0x80c511ec–0x80c511f2`). Completion separately reads the command's
32-bit result at `+8` and a raw result byte at `+0x68`
(`0x80c55a74`, `0x80c55b64`). These observations support a counted lifecycle;
they do not prove a generation token, cancellation safety, stale-event
protection, or buffer ownership on every failure path.

The IOP translates external addresses before dereferencing `R`, and brackets
descriptor use with cache-maintenance calls. The translations are not host
pointers. Full address-domain mappings, queue-size initialization, the whole
descriptor size, cache coherence, acknowledgement order, and error cleanup
remain open. Reused scratch results need an immutable snapshot before a
subsequent query can overwrite them.

A proposed next step is a separate synthetic 512-byte decoder with exact
request/page/CE binding, duplicate rejection and raw status preservation.
It would make no firmware calls and add no driver/opcode integration.
It cannot establish safe Linux-owned NAND storage, a power-cut contract,
or preservation of the iOS boot path. See
[storage constraints](p105-linux-storage.md) and
[read-only MLC metadata](p105-mtd-media-type.md).
