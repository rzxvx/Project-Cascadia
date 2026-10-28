# Actual13G36 iAP Gregorian/GMT0 date setup

This separate proof runs the original iAP policy constructor, original
queue/calendar once blocks and original date block/caller. It closes the
previously unprovided calendar/timezone setup: the source requests a Gregorian
calendar, creates timezone offset0, installs that timezone, then composes `yMd`
with2006/5/31. UTC/GMT0 is therefore an actual producer argument, not a substituted
TLS rule or a guess from a date literal.

Caller-owned official inputs are SHA-pinned exactly as the native path group:
cache `4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b`;
securityd `1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097`.
The latter is used only by the shared synthetic loader; the executed constructor
and blocks are original cache instructions. Named cache exports and original
Security indirect-symbol tables validate API boundaries before execution.

Actual executed chain:

1. `SecPolicyCreateiAP`,0x20cabb6c.
2. Calendar/queue helper0x20cc5c4c; queue once invoke0x20cc6190 creates named
   serial `ZuluCalendar` queue.
3. Original synchronous invoke0x20cc5cc8, whose calendar once invoke0x20cc61b4
   calls actual named `CFCalendarCreateWithIdentifier` for `gregorian`,
   `CFTimeZoneCreateWithTimeIntervalFromGMT` with double0, then
   `CFCalendarSetTimeZone` with those objects.
4. Actual original date block0x20cacd14 calls named
   `CFCalendarComposeAbsoluteTime` with format `yMd`, year2006/month5/day31.
5. Original caller passes the output double to `CFDateCreate` and adds that
   object under `NotValidBefore` before `SecPolicyCreate` for OID `iAP`.

CF objects, arithmetic, allocation/cleanup and dispatch scheduling are explicitly
synthetic API boundaries. Original once/block/date instructions execute with
bounded synthetic call/return trampolines; no live queues or daemon are started.
The success control supplies a Python Gregorian/GMT0 conversion at the CF
arithmetic boundary. Its numeric170726400 seconds relative to2001 corresponds
to UNIX1149033600; this number is **not claimed as output of executed original
CF arithmetic**. Apple's primary
[CFAbsoluteTime documentation](https://developer.apple.com/documentation/corefoundation/cfabsolutetime)
defines the reference date and units. Combined with the actual Gregorian/GMT0
arguments, it specifies the intended cutoff as2006-05-31T00:00:00Z. The native
checker separately rejects notBefore less than or equal to that cutoff.

A second negative control deliberately returns CF arithmetic statusfalse while
placing a known synthetic567.0 in the output buffer. The original caller still
passes567.0 onward: it does not inspect that return status. This proves caller
handling of the supplied failure/output combination, not that the real CF API
writes567.0 or any defined value on failure. It is not used as an acceptable
Linux fallback. A future host verifier must require successful deterministic
conversion or use the qualified fixed cutoff; unknown/failed arithmetic must
never weaken the rule or grant trust.

The tests independently assert calendar identity, offset0/set order, both
received doubles and successful bounded returns. No Apple binary/cert/key bytes,
current wall-time measurement, accessory chain validation, authentication
property or USB/current/VBUS grant is included. Existing source-policy/path
fixtures remain unchanged.

```
python3 emulate-calendar.py DYLD_CACHE SECURITYD
```

Existing Unicorn suffices.2006/GMT0 is a fixed certificate-policy producer,
not a claim about the iPad's clock or Linux charging/host readiness. Native
execution is bounded to5000 instructions per case; unexpected provider APIs
fail loudly. All hardware/device/service/network paths remain unused.
