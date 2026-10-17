# P105 desktop touch: registration and gesture discovery

The frozen RAM session at `1ff01fad8280632fa0a3f25ee9e4cd023a4a0be0`
registers the S5L8940X touchscreen with Xorg's libinput driver. The saved
Xorg log identifies a core-event TOUCHSCREEN device. Physical pointer motion,
click accuracy and gestures still need a human test. Touchégg's empty device
list does not by itself establish failure of that independent Xorg path.

## A possible delayed Touchégg device announcement

The installed versions are Touchégg 2.0.18 and libinput 1.31.3. Their original
sources establish the following sequence:

1. Touchégg creates its own libinput udev context and assigns the seat before
   entering `run()`. The first loop iteration calls an unbounded `poll()` on
   the libinput file descriptor before draining `libinput_get_event()`.
2. libinput's seat assignment enumerates initialized input devices. Device
   creation can synchronously enqueue DEVICE_ADDED in a RAM queue.
3. That queue insertion does not make the separate epoll file descriptor
   readable. A queued announcement may therefore wait for another FD event.

This is a source-backed explanation to investigate, not a demonstrated diagnosis
of the running daemon. Successful enumeration in that particular context has
not been observed. Xorg and Touchégg use separate contexts. Touchégg accepts
CAP_TOUCH as well as CAP_GESTURE; its compatibility message precedes the physical
size check, so missing size does not explain the missing message.

Do not use a broad udev trigger merely to populate the list. The installed
`90-libinput-fuzz-override.rules` also matches CHANGE and can run
`libinput-fuzz-to-zero`. Its source opens the event node read/write and can
change kernel ABS-axis fuzz. Other rules can run the keyboard builtin and
device-group helpers. No trigger, input injection, daemon restart or calibration
change was performed for this investigation.

## Incorrect physical-size metadata is a separate issue

The frozen and current `apple-z2.c` both normalize positions to 0..4096 and
set the two MT position resolutions to 1. Linux defines those resolutions in
units per millimetre. Without another override, libinput consequently derives
4096 by 4096 mm rather than a measured panel size. INPUT_MT_DIRECT copies the
position axes, including their resolution, to ABS_X/Y as well.

Touchégg accepts that nonzero size and calculates its touchscreen thresholds
as percentages of it. Its touch coordinates are also scaled by the same
resolution, so some percentage calculations can cancel that scale on an axis.
The false physical units do not establish that every gesture is broken, nor
do they explain the earlier missing compatibility message. No replacement
resolution was guessed. Panel dimensions, axis orientation and any active
overrides should be established before changing this contract.

## Next physical observation

Preserve the working SSH/Xorg session. During a planned human test, distinguish
ordinary tap/pointer behavior from the extra two-finger right-click provided by
Touchégg. Observe whether a real input event is followed by the compatibility
announcement. If not, inspect that context's initialized-device/seat/ignore
state before changing drivers. This observation has not yet been made.

## Pinned primary sources

- [Touchégg context and poll order](https://github.com/JoseExposito/touchegg/blob/fc30d316e163f4504043353d6610e670ce5c6439/src/gesture-gatherer/libinput-gesture-gatherer.cpp), constructor and `run()`.
- [Touchégg capabilities, size and thresholds](https://github.com/JoseExposito/touchegg/blob/fc30d316e163f4504043353d6610e670ce5c6439/src/gesture-gatherer/libinput-device-handler.cpp).
- [libinput queue and file descriptor](https://gitlab.freedesktop.org/libinput/libinput/-/blob/26191d396d74d505541d6311f0b4ae68d791b890/src/libinput.c), `libinput_post_event()` and `libinput_get_fd()`.
- [libinput seat enumeration](https://gitlab.freedesktop.org/libinput/libinput/-/blob/26191d396d74d505541d6311f0b4ae68d791b890/src/udev-seat.c).
- [libinput fuzz helper](https://gitlab.freedesktop.org/libinput/libinput/-/blob/26191d396d74d505541d6311f0b4ae68d791b890/udev/libinput-fuzz-to-zero.c).
- [Linux v6.12 axis units](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/include/uapi/linux/input.h?h=v6.12) and [MT axis copying](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/input/input-mt.c?h=v6.12).

The audit verified 59 frozen source/snapshot hashes and four installed binary
hashes. Original upstream source inspection does not establish binary equivalence
to all Alpine packaging patches or prove physical input behavior.
