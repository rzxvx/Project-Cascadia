# P105 touch power-path peek — kernel-side UART dump plan

Goal: capture the POWERED state of SPI1/PWM/PMGR under live XNU (where Cmwp has
run), since userland peek is impossible on the LIK SSH ramdisk (no /dev/mem,
gutted busybox — only dd+printf). Read the registers from inside XNU and print
them over the DCSD UART, then diff vs Recovery.

## Resolved kernel symbols (kernelcache.macho, LC_SYMTAB present, 12H321)

    serial_putc   0x8027a7d4  thumb   (call 0x8027a7d5; char in r0)
    kprintf       0x8027a7f0  thumb
    ml_phys_read  0x8007f3d4  thumb   (call 0x8007f3d5; arg = PHYSICAL addr, ret u32)
    ml_phys_write 0x8007f78c  thumb
    ml_io_map     0x80086ca0  thumb
    serial_getc   0x8027a888  thumb
    serial_init   0x8027a8cc  thumb

ml_phys_read takes a PHYSICAL address -> every reg (PMGR/GPIO/SPI1/PWM/I2C0) is
read the same way; no fixed-VA vs dynamic-map split needed.

__TEXT: vm=0x80001000 foff=0 fsize=0x2CF000  ->  file_off = VA - 0x80001000.
Zero code-caves in __TEXT (>=64B): 0x80083108 (3900B), 0x8000193e (1730B),
0x80082020 (480B), plenty more (41 total). Use 0x80083108 for the stub+strings.

## Hook strategy: PERIODIC, not location-based

The multitouch kext is stripped (no symbol), so we can't hook its start() by
name. Instead trampoline at kprintf: every Nth call -> mt_peek_dump(). Benefits:
  - robust to not knowing exactly when Cmwp powers the block
  - DIAGNOSTIC: if SPI1/PWM never leave 0 across the whole boot, the SSH ramdisk
    never started the touch driver at all — we learn that instead of guessing.
No recursion: mt_peek_dump prints via serial_putc directly, not kprintf.

Payload: tools/xnu_mt_peek_payload.c  (mt_peek_dump + REGS table).
Parse:   scripts/parse-mt-uartlog.py -> scripts/diff-mt-peek.py

## OPEN: which kernelcache is booted + repack path

- ibootfiles/kernelcache.macho = 14MB feedface WITH symtab (RE copy).
- ibootfiles/Kernelcache.dec   = 7.8MB but actually IMG3 container (magic img3).
- LIK ramdisk_12H321/saved/ does NOT exist — LIK booted the ramdisk via its own
  menu, so the exact kernelcache it loaded is not at the boot-lik-sshrd.sh path.
Need: confirm the bootable kernelcache, patch the Mach-O it decompresses to,
re-wrap (scripts/create_lzss_payload.py / unpack-kernelcache.py) and boot via
bootx. Verify iBEC accepts the patched (unsigned, checkm8) image.

## KASLR gotcha (first hook attempt panicked)

XNU here boots with a kernel slide (observed 0x19a00000; text base 0x99a01000),
and boot-args disable sig/cs enforcement + enable unrestricted task_for_pid.
=> NEVER hardcode absolute kernel addresses in the hook payload. First attempt
hardcoded serial_putc=0x8027a7d5 (unslid) -> "prefetch abort, fault_addr=0x8027a7d4".
The b.w hook at kprintf itself was fine (PC-relative branches survive the slide).
Fix pattern (mt-hook/stub.S):
  - serial_putc: compute at runtime = adr(mt_stub) + (serial_putc - mt_stub)=0x1F76CC, orr #1
  - return to kprintf+4: relative `b.w 0x8027a7f4`, not absolute ldr/bx
  - MMIO reads (0x3F1xxxxx PMGR / 0x3FA GPIO) are fixed-VA, do NOT slide -> direct deref OK

## MMIO mapping gotcha (2nd panic)

Second attempt (hook moved to IOFindBSDRoot, one-shot) panicked:
"kernel abort type 4: fault_type=0x1, fault_addr=0x3f100010" — a DATA abort on the
FIRST register READ. serial_putc worked (r4 = slid serial_putc; it printed
"MTPEEK CLK10=" before the fault), and the slide was 0xE730000 this boot vs
0x19a00000 the previous boot => KASLR slide is RANDOM per boot (never hardcode).
LESSON: XNU maps each MMIO block at a DYNAMIC kernel VA (log shows i2c0 phys
0x88d15000-region, uart0 "0x32500000(0x88d5d000)"), NOT at its physical address.
So a raw deref of 0x3F100010 faults. All MMIO reads MUST go through
ml_phys_read(phys) (0x8007f3d4), which does phys->temp-map->read. Current hook
passes BOTH serial_putc (mt_stub+0x1F76CC) and ml_phys_read (mt_stub-0x3D34) as
runtime pointers to mt_peek_dump; every reg read via ml_phys_read. Reads
PMGR+GPIO+SPI1+PWM+I2C0 in one one-shot dump.

## DECISIVE FINDING: ramdisk never powers touch

Serial console enabled (iBEC.serial: boot-args pio-error=0 -> serial=3) => full
TEXT logs to DCSD. First clean hook boot panicked:
  panic: "PIO0 read slave/decode error at address 0x32100000 ... agent: cpu"
i.e. reading SPI1 under XNU = bus decode error because the block is UNPOWERED.
Driver ::start list from the log has NO multitouch/SPI/Zephyr driver (only
backlight, I2C, BCMWLAN-HSIC, SynopsysOTG(USB), TSL2581, AP3GDL, AKM8963).
=> the LIK SSH ramdisk does not bring up the touch/SPI stack at all, so there
is no powered state to peek here. PMGR/GPIO (0x3F1/0x3FA) read fine.
Options: (a) full-iOS boot where touch is powered + hook there; (b) ACTIVE hook
that replays/triggers the power-up under XNU (we have ml_phys_write 0x8007f78c)
then reads SPI1 back — empirical RE loop via the now-working patch/boot/serial
cycle; (c) static Cmwp reverse in Ghidra. Current hook (PMGR/GPIO only, kprintf)
is the clean baseline + pipeline proof.
