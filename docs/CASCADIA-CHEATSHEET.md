# Project Cascadia — памятка (Linux на iPad mini 1 / Apple A5)

## Железо
- Устройство: iPad mini 1, iPad2,5 / A1432, board `p105ap`, SoC Apple A5 (**S5L8942X**)
- 512MB RAM, dual Cortex-A9
- Exploit: checkm8 через Raspberry Pi Pico (checkm8-a5, LukeZGD)
- DCSD кабель: `/dev/cu.usbserial-A506VZ9Q`, 115200 baud, требует `sudo`. Кабель проводит и UART, и USB D+/D- одновременно.
- Прошивка, к которой всё прибито: **12H321** (iOS 8.4.1, "Donner").
  В репозитории её нет — `./cascadia firmware` выводит iBSS/iBEC из твоего IPSW.
  Пин не произволен: autogo-хук патчит адрес внутри именно этой сборки iBEC.

## Всё делается через ./cascadia

Репозиторий самодостаточен с 2026-09-17. `~/iBSSloader` больше НЕ нужен ни для
чего: dts, config, patches, rootfs-оверлей и boot chain живут здесь.

```bash
./cascadia doctor      # чего не хватает на этой машине
./cascadia kernel      # склонировать Linux на пине v6.12 в build/linux
./cascadia rootfs      # собрать Alpine armhf для initramfs
./cascadia build       # dtb + ядро + output/staging-bundle.bin
./cascadia firmware    # iBSS/iBEC из своего IPSW (нужен Legacy iOS Kit + ipad25.ipsw)
./cascadia flash       # прошить:  iBSS → iBEC → bundle → loader
```

Уже есть дерево ядра и не хочется второго на 2.5 ГБ:
```bash
KERNEL_SRC=~/Desktop/linux-kernel ./cascadia kernel   # симлинк; build монтирует
                                                      # цель внутрь контейнера
```

### Где что лежит
```
dts/p105ap.dts              ИСТОЧНИК ИСТИНЫ для DTS (не дерево ядра!)
config/p105ap.config        фрагмент Kconfig, мерджится с multi_v7_defconfig
patches/files/**            целые НОВЫЕ файлы ядра, копируются как есть
patches/tree/*.patch        правки СУЩЕСТВУЮЩИХ файлов ядра, git apply
rootfs/alpine/**            оверлей со стороны Alpine: репозитории apk, inittab, motd
initramfs/init              stage 1: монтирования, часы, USB, сеть, выбор корня
initramfs/sbin/p105-stage2  stage 2: getty, dropbear — общий для обоих корней
pongo/                      bare-metal трамплин linux-boot
build/                      всё генерируемое: ядро, rootfs, прошивка
output/                     staging-bundle.bin + staging-loader.bin
```

### Как собирается initramfs

`./cascadia rootfs` разворачивает alpine-minirootfs, кладёт сверху два оверлея —
`rootfs/alpine/` (сторона Alpine) и `initramfs/` (наши stage 1 и stage 2), —
доставляет то, что нельзя поставить после первой загрузки (dropbear с твоим
публичным ключом и `mount.nfs`), и копирует поверх всё, что лежит в
`build/keep/`. Последнее — для файлов, которые пока не воспроизводятся из
репозитория: там сейчас `hx-touchd`, демон тача из Sandcastle.

**Внутри `build/initramfs-root` руками ничего не правят.** До 2026-09-18 правили
именно там, а в репозитории лежала копия `/init` времён до USB-сети, и чистый
клон собирал ядро, которое поднималось на стекле — без консоли по кабелю, без
10.55.0.2 и без ssh. Теперь `./cascadia build` проверяет собранный архив на
`P105: stage1 start`, `p105-stage2`, `ttyGS0` и `10.55.0.2` и падает, если их нет.

Если docker не запущен, шаг с пакетами пропускается с предупреждением: дерево
всё равно грузится, но без ssh и без NFS-корня.

### Два механизма патчей, не путать
**Новые файлы** — `patches/files/` копируются `apply-kernel-patches.py --files-only`.
**Правки существующих** — один `patches/tree/0001-cascadia.patch`, накладывается
`git apply` через `apply-kernel-edits.sh`, с reverse-check на идемпотентность.

Раньше второе делалось подбором anchor-строк, и несовпавший anchor **пропускался
молча** — первым пропадал вызов `apple_aic1_rearm()`, после чего ядро собиралось
зелёным, грузилось и не брало ни одного прерывания. Патч либо ложится, либо
объясняет почему. Это возможно только потому, что версия ядра запинена.

### Прошивка
```bash
./cascadia flash                 # свежесобранный iBEC, checkm8 через primepwn
./cascadia flash --kdfu          # то же, но БЕЗ Pico — через джейл
./cascadia flash --skip-pwn      # устройство уже в pwned DFU
./cascadia flash --known-good    # августовский образ — отделить плохую сборку
                                 # от плохого стенда
./cascadia flash --no-uart       # без последовательного захвата
./cascadia flash --no-link       # не трогать сеть на стороне мака
./cascadia link                  # только шаг сети, без прошивки
```

В конце `flash` сам выставляет на маке `10.55.0.1/24` на интерфейсе гаджета и
ждёт, пока ответит `10.55.0.2`. Интерфейс ищется по MAC из bootargs
(`g_cdc.host_addr`), а не по имени `enN`. Руками это не запоминается: интерфейс
создаётся при энумерации и уничтожается при уходе устройства, а за одну загрузку
он уходит дважды — когда iBEC передаёт управление и когда `/init` форсирует
переэнумерацию. Без адреса пакеты к 10.55.0.2 идут по маршруту по умолчанию в
интернет, и ssh просто **висит**, ничего не говоря. Время тоже важно: stage 1
монтирует NFS-корень с 10.55.0.1 сразу после подъёма usb0, и `mount.nfs`
сдаётся примерно через две минуты — после этого корень остаётся в RAM.

### Два пути в pwned DFU
**primepwn** — checkm8, нужен Pi Pico или Arduino с USB host shield: на A5
эксплойт требует тайминга по USB, который обычный хост не выдаёт.

**kDFU** — из джейлнутой iOS `kloader` грузит пропатченный iBSS напрямую,
железа не нужно вообще. EverPwnage джейлит A5 на 7–9.3.6 **untethered**, то есть
устройство поднимается джейлнутым каждый раз и шаг остаётся однокомандным.

Оба реализованы в Legacy iOS Kit, мы их вызываем. Нюанс: kDFU отправляет **свой**
pwned iBSS, не наш. Отличаются они только boot-args, которые вписал
iBoot32Patcher; значим патч подписи, он есть у обоих, а boot-args всё равно
задаёт iBEC.

Второй нюанс, стоивший одного цикла прошивки: **после kloader образ должен быть
расшифрованным**. К моменту, когда iOS загрузилась, ключ GID у AES-движка уже
погашен, поэтому KBAG расшифровывать нечем — iBSS получает мусор и прыгает в
него. Снаружи это выглядит обманчиво: `irecovery` отчитывается о 100%, а потом
устройство пропадает с шины и кажется выключенным. Поэтому `./cascadia firmware`
собирает iBEC в двух видах:

| файл | для чего |
|---|---|
| `iBEC.patched.autogo.dfu` | checkm8: холодный DFU, GID жив, образ упакован как сток |
| `iBEC.patched.autogo.plain.dfu` | kDFU: img3 без KBAG, полезная нагрузка открытым текстом |

`flash --kdfu` берёт второй сам, а `--ibec` и `--known-good` его не
переопределяют. Ровно так же устроен pwnediBSS у LIK: `xpwntool ... -iv -k`
расшифровывает, `iBoot32Patcher --rsa` патчит, `xpwntool ... -t iBSS` пакует
обратно **без** шифрования, обнуляя sigCheckArea. `scripts/img3pack.py`
повторяет эту раскладку, а не изобретает свою.
Порядок заливки и паузы не декоративны: `primepwn` выполняет checkm8 и оставляет
работающий pwned iBSS; тот принимает неподписанный iBEC; autogo-хук в iBEC
срабатывает на конце заливки бандла и запускает загрузчик — поэтому loader
отправляется последним и нигде нет `irecovery -c go`.

Порт UART определяется сам (`/dev/cu.usbserial-*`, `/dev/ttyUSB*`), захват
необязателен. Ранний лог дублируется в фреймбуфер, а UART имеет привычку
обрываться посреди загрузки — так что отсутствие переходника это неудобство,
а не блокер.

### Чтение вердиктов
UART обрывается почти всегда, поэтому надёжнее забрать их с устройства:
```bash
ssh root@10.55.0.2 'dmesg | grep -E "AIC1-REARM|LATE-SMOKE|SOF-TIMER|AIC-TIMER|P105:"'
```

## Текущий статус ядра (Linux 6.12.0 на A5) — обновлено 2026-09-14
✅ Boot до interactive shell (framebuffer console, tty0), Alpine 3.24 initramfs
✅ Кастомный AIC1 interrupt controller driver (**с двумя фиксами 2026-09-13**), PMCCNTR clocksource, simplefb
✅ **USB PHY driver** — регистры полностью декодированы, iBoot уже оставляет ВАЛИДНОЕ state,
   driver только освобождает reset (см. секцию "USB PHY" ниже)
✅ **DWC2 + g_ether** — Mac физически видит iPad и пытается SET_ADDRESS (visible в `log stream`)
❌ **USB enumeration полностью не работает** — Mac получает STALL (result code 25) потому что
   IRQ 50 (mapped на AIC1 IRQ 11 Edge) ни разу не поднимается. Корневая причина не в USB,
   а в **общем блокере IRQ delivery** — см. "AIC1 IRQ delivery" ниже
✅ **Touch stack — kernel-side поднят** (сентябрь 2026 сессия):
   ✅ Порт 7 hx-* драйверов из Corellium Sandcastle (5.4 → 6.12 API)
   ✅ Cmwp @ `0x33500300` — single-register layout (32768 Hz target) от Sandcastle
   ✅ PMU d2333 @ i2c0 0x3c, регуляторы `touch_ana@0x20c` + `touch_ldo@0x213`
   ✅ `input: S5L8940X Capacitive TouchScreen` → `/dev/input/event0`
❌ Touch **события не идут** — SPI1 регистры возвращают 0x22 (bus abort), Cmwp XNU-handler
   не отреверсен (2026-09-12/13 сессия). Даже если бы работал — IRQ 51 всё равно
   не доедет до CPU (тот же блокер)
❌ UART интерактивный ввод — нет; tty0 framebuffer — единственный рабочий ввод
❌ CPU1/SMP, WiFi — не начинали

# ═══════════════════════════════════════════════════════════════
# ГЛАВНЫЙ БЛОКЕР 2026-09-13/14: IRQ delivery AIC1 → CPU
# ═══════════════════════════════════════════════════════════════

## AIC1 IRQ delivery — что найдено, что исправлено, где встали

### Найдено (баги в `patches/files/drivers/irqchip/irq-apple-aic1.c`)

**BUG-1: CONFIG.ENABLE никогда не включался.**
`aic1_of_init()` вызывает `aic1_clear_sticky_nirq()`, которая среди прочего сбрасывает
`AIC1_CONFIG.ENABLE (BIT(0))`. Никакого последующего `aic1_enable_hw()` в коде не было —
AIC1 hardware оставался **навсегда выключен**. IPI setup работал (`ipi_mux_create`,
`set_smp_ipi_range`), но hardware IRQ не поднимал бы никогда.

**BUG-2: `apple_aic1_early_irq_escape` никогда не гасился.**
Флаг стартует как `true` (см. `arch/arm/mach-apple/apple.c:41`). Пока он true,
`aic1_handle_irq` в самом начале делает `regs->ARM_cpsr |= PSR_I_BIT` — то есть даже
если бы handler всё-таки вызвался, он бы форсил маску IRQ на return, гарантируя
что второй IRQ не придёт. Функция `apple_aic1_release_escape()` существовала, но
её никто не вызывал (комментарий говорил "call from irq_thaw" — а самого irq_thaw
не существует).

### Фиксы (2026-09-13)
В `iBSSloader/patches/files/drivers/irqchip/irq-apple-aic1.c` в конце `aic1_of_init()`:
```c
/* FIX-1: включить AIC1 обратно после clear_sticky_nirq */
aic1_enable_hw(aic);
/* FIX-2: сбросить escape flag сразу — ждать некого */
apple_aic1_early_irq_escape = false;
```
Плюс сразу после — bulk unmask всех IRQ (write-only регистры, readback врёт):
```c
for (i = 0; i < nr_words; i++)
    aic1_write(aic, AIC1_MASK_CLR + i*4, ~0U);
for (i = 0; i < aic->nr_irq; i++)
    aic1_write(aic, AIC1_TARGET_CPU + i*4, BIT(0));
```

### Результат фиксов
- `CONFIG=0x10773` (bit 0 = ENABLE установлен, `0xE0000000` IMPL biтам записываем но
  readback их не показывает — то ли они write-1-clear, то ли read-only-status, не критично)
- Software test: `AIC1_SW_SET` на hwirq 0 → `EVENT` = `0x00010000` (type=HW num=0) —
  **AIC1 hardware корректно queue'ит events**
- **НО** `/proc/interrupts` всё равно показывает 0 для ВСЕХ IRQ (49 SPI, 50 USB, 51 touch,
  IPI0-6, Err). ARM CPU не берёт IRQ exception несмотря на queued события.

### Где встали
Между AIC1 output line и CPU nIRQ pin есть gap. Три возможные причины:
1. `CPSR.I` остаётся `1` даже после `local_irq_enable()` в `start_kernel()`
2. AIC1 hardware IRQ output line физически не разведена на CPU nIRQ pin,
   и нужен какой-то дополнительный enable-gate (пока неизвестный)
3. `handle_arch_irq = aic1_handle_irq` не устанавливается / затирается кем-то

**Не подтверждено пока: apple_a9_gic_drain()** — на A9 MPCore обычно IRQ идёт через
GIC cpu-interface в PERIPHBASE window (CBAR + 0x100). У A5 CBAR = `0x3E100000`, но
любой ioremap/MT_DEVICE этого окна **вешает Linux** (`aic1q33 stuck`) — Apple либо
удалил GIC, либо ключевые регистры не отвечают на MMIO. Функция `apple_a9_gic_drain()`
сейчас **no-op**. Если Apple оставил GIC-компаунт cpu-interface, но убрал distributor —
может потребоваться прямая инициализация CPU interface без distributor.

### Диагностический патч 2026-09-13/14 (**собран, не тестировался**)
Добавлен в `patches/files/drivers/irqchip/irq-apple-aic1.c`:

**(а) Счётчик входов в handler:**
```c
static unsigned int aic1_handler_entries;
/* в самом начале aic1_handle_irq: */
aic1_handler_entries++;
```

**(б) `late_initcall(aic1_late_smoke)`** — запускается ПОСЛЕ `local_irq_enable()` в
start_kernel:
```c
static int __init aic1_late_smoke(void) {
    /* 1. Дамп CPSR (если I=1 — local_irq_enable() no-op) */
    asm volatile("mrs %0, cpsr" : "=r"(cpsr_before));
    /* 2. Baseline handler_entries + EVENT */
    /* 3. Fire AIC1_SW_SET на hwirq 0, mdelay(50) */
    aic1_write(aic, AIC1_SW_SET + 0*4, BIT(0));
    mdelay(50);
    /* 4. Re-read, классифицировать вердикт */
    if (handler_after > handler_before)
        pr_err("LATE-SMOKE: PASS -- CPU takes AIC1 IRQ. Root cause elsewhere.");
    else if (cpsr_after & 0x80)
        pr_err("LATE-SMOKE: FAIL -- CPSR.I=1 late; local_irq_enable() didn't stick.");
    else if (ev_after == 0x10000)
        pr_err("LATE-SMOKE: FAIL -- CPSR.I=0 but AIC1 line never reaches CPU nIRQ pin.");
    else pr_err("LATE-SMOKE: WEIRD -- EVENT=%#x", ev_after);
}
late_initcall(aic1_late_smoke);
```

**(в) Bootargs почистили** — `initcall_debug` убрано из `dts/p105ap.dts`
чтобы вердикт LATE-SMOKE поместился в UART до truncation.

**(г) `/init` теперь дампит:**
- `dmesg | grep "LATE-SMOKE|AIC1-DIAG|aic,1:"` в самом начале (гарантированно на fb)
- Периодически (каждые 10 iter) все IRQ 49/50/51/Err counters

### Что делать по результату LATE-SMOKE
| Вердикт | Диагноз | Следующий шаг |
|---------|---------|----------------|
| PASS (handler_entries>0) | Pipeline работает, реальные IRQ mask/target readback врёт | Копать per-line enable, проверить чем irq_data mask/unmask реально пишет |
| FAIL CPSR.I=1 | `local_irq_enable()` где-то no-op'ается | grep `raw_local_irq_enable` в arch/arm/, проверить нет ли патча в mach-apple |
| FAIL EVENT=0x10000 | AIC1→CPU wire dead, нужен gate | RE Apple SoC — искать какой-то ENABLE в PMGR/AIC region, посмотреть pongoOS interrupt_init более внимательно |
| WEIRD | Что-то поехало | Читать раз за разом до понимания |

### Как продвинуться в следующей сессии
1. Собрать → прошить → UART capture + фото fb → grep LATE-SMOKE
2. По вердикту — один из трёх путей из таблицы
3. Пока не решится IRQ delivery — **USB, touch, любая IRQ-driven периферия ЗАПАРКОВАНА**.
   Всё работающее в системе на **polling** (I2C QUIRK_POLL, PMCCNTR polling clocksource,
   framebuffer passive)

# ═══════════════════════════════════════════════════════════════
# USB PHY — рабочее состояние 2026-09-13
# ═══════════════════════════════════════════════════════════════

## Полная карта регистров (empirical, live dump с работающего iBoot USB DFU)
База: `0x36000000` (otgphyctrl). **Все значения — то что iBoot ОСТАВЛЯЕТ при выходе из DFU:**

| Offset | Имя | Value | Комментарий |
|--------|-----|-------|-------------|
| 0x00 | OPHYPWR | 0x00000006 | PLL power / XO power bits |
| 0x04 | OPHYCLK | 0x00000001 | CLKSEL_24MHZ (bit0) |
| 0x08 | ORSTCON | 0x00000000 | reset released |
| 0x1C | OPHYUNK1 (uotgtune0?) | 0x00000006 | |
| 0x30 | (unused) | 0x00000000 | |
| 0x34 | (unused) | 0x00000000 | |
| 0x40 | **UOTGTUNE1** | **0x00000549** | ADT: uotgtune1-device |
| 0x44 | **UOTGTUNE2** | **0x00002FF3** | ADT: uotgtune2-device — **не 0xF8!** |
| 0x60 | OPHYUNK4 | 0x00000200 | |

⚠️ **КРИТИЧНО**: старая мастер-копия драйвера пыталась ПЕРЕПИСАТЬ `OPHYUNK2 (0x44) = 0xF8` —
это КРАШИЛО ядро при probe. Правильное значение уже установлено iBoot (0x2FF3 == ADT
`uotgtune2-device`). **Driver теперь не переписывает никакие tune-регистры**, только:
1. Включает 7 PMGR clock gates: 87, 88, 89, 90, 91 (usb-complex), 5 (PHY), 292 (main OTG)
2. Тогглит `ORSTCON.PHYSWRESET (bit0)` = 1, wait, обратно 0, wait 1ms
3. Логирует финальное состояние регистров

Файл-мастер: `~/Desktop/linux-kernel/drivers/phy/phy-apple-s5l-usb.c` (не в NEW_FILES,
правится напрямую в kernel-дереве).

## ADT tuning constants (справочно, для сверки)
```
uotgtune1-device = 0x549   uotgtune1-host = 0x54F
uotgtune2-device = 0x2FF3  uotgtune2-host = 0x6DF3
ref-clock-sel = 3
usbhostset-en-incrx = 0xE0
usb-complex clock-gates = [87, 88, 89, 90, 91], main clock-id = 292
otgphyctrl clock-ids = [5]
usb-device @ 0x36100000, IRQ 11, num-of-eps = 14, fifo-depth = 0x820
```

## Изменения в DTS (2026-09-13, в `dts/p105ap.dts`)
```dts
otgphy: phy@36000000 {
    compatible = "apple,s5l8940x-otgphy";
    reg = <0x36000000 0x1000>;
    #phy-cells = <0>;
    status = "okay";        /* ← было "disabled" */
};

dwc2: usb@36100000 {
    compatible = "apple,s5l8940x-dwc2", "snps,dwc2";
    reg = <0x36100000 0x40000>;
    interrupts = <11>;
    interrupt-parent = <&aic>;
    phys = <&otgphy>;
    phy-names = "usb2-phy";
    dr_mode = "peripheral";
    status = "okay";
};
```

## Kconfig-фрагмент (в `config/p105ap.config`)
```
CONFIG_GENERIC_PHY=y
CONFIG_PHY_APPLE_S5L_USB=y
CONFIG_USB=y
CONFIG_USB_DWC2=y
CONFIG_USB_DWC2_PERIPHERAL=y
# CONFIG_USB_DWC2_DUAL_ROLE is not set   ← OTG state machine виснет ждущи session-valid
CONFIG_USB_DWC2_DEBUG=y
# CONFIG_USB_DWC2_VERBOSE is not set     ← VERBOSE топит UART
CONFIG_DEBUG_FS=y
CONFIG_USB_GADGET=y
CONFIG_USB_ETH=y
CONFIG_USB_ETH_RNDIS=y
```

## Что реально происходит на Mac
Даже с полностью рабочим DWC2 (Core Release: 2.72a, `snpsid=4f54272a` — реальный чип отвечает),
`log stream --predicate 'process == "usbd"'` показывает попытки enumeration:
```
USB device attached: iPad ... at 480 Mbps
Sending USB SETUP: SET_ADDRESS ...
Received USB response: STALL (25)
Sending USB SETUP: SET_ADDRESS ...
Received USB response: STALL (25)
...
```
Mac видит устройство, поднимает HS, шлёт SET_ADDRESS. DWC2 hardware принимает bus reset,
но `dwc2_handle_common_intr` **никогда не вызывается** — IRQ 50 counter в `/proc/interrupts`
остаётся 0. → корневая причина = блокер IRQ delivery (см. выше), НЕ проблема USB.

## Живая проверка что PHY-регистры остались валидны
После `dwc2 36100000.usb: Core Release: 2.72a` в UART log ищи:
```
PHY dump post-init: PWR=0x00000006 CLK=0x00000001 RST=0x00000000 U1C=0x00000006 R30=0x00000000 R34=0x00000000 R40=0x00000549 U44=0x00002ff3 U60=0x00000200
```
Если U44 ≠ `0x2ff3` — patch driver'а откатился, что-то переписало register. Тревога.

# ═══════════════════════════════════════════════════════════════
# Пути и файлы touch stack (сентябрь 2026)
# ═══════════════════════════════════════════════════════════════

**Драйверы (мастер-копии — правь ЗДЕСЬ, не в linux-kernel!):**
```
~/iBSSloader/patches/files-sandcastle-port/drivers/
  clk/clk-s5l8940x-pmgr.c           — Cmwp/grape clock gate
  pinctrl/pinctrl-s5l8940x-gpio.c   — GPIO+pinctrl (arch_initcall)
  spi/spi-s5l8940x.c                — SPI controller
  i2c/busses/i2c-s5l8940x.c         — I2C controller
  mfd/apple-pmu-i2c.c               — PMU d2333 parent (regmap)
  regulator/apple-pmu-pwrsw.c       — power switch regulators
  input/touchscreen/apple-z2.c      — Z2 multitouch (miscdev "apple-z2")
```

**DTS ноды (в `~/iBSSloader/dts/p105ap.dts`):**
- `refclk24mhz` (fixed-clock 24MHz, ROOT level not в soc!)
- `touchclk: touchclk@3500300` — Cmwp, `apple,s5l8940x-pmgr-clk-touch`, 32768 Hz
- `i2c0: i2c@3200000` — `apple,s5l8940x-i2c`
- `pmu: pmu@3c` — `apple,pmu-d2333`, 16-bit reg
- `touch_ana@20c` + `touch_ldo@213` — `apple,pmu-pwrsw`
- `touch@0` (в spi1) — `apple,z2-multitouch`, generation=1, hv-supply/core-supply/gpios

## Cmwp / SPI1 — блокер сентябрь 2026
SPI1 регистры при `0x32100000` возвращают `0x22` (bus abort default). Не помогло:
- Full pmgr_bootstrap-first ordering
- CLK 4/304/307 включены
- Gate pokes replicating pongo touch_cursor exactly

**Empirical evidence**: PMGR gate table last non-zero at `0x1140` (ibot_id 78) — gate 83 (PWM)
at `0x1154` reads 0 and writes drop silently. Confirms Cmwp handler в XNU
(kernelcache `0x804ba3e8`) does more than any known MMIO sequence. Реверс Cmwp — единственный
разблокатор для touch.

> **Вывод выше неверен, снято на железе 2026-09-18.** Регистр гейта 83 не на
> `0x1154`, а на `0x1124`: массив начинается на 10 id ниже, чем читалось в
> дизассемблере (`0x3f100fd8 + id*4`). `0x1154` — это дырка в массиве, она и
> «глотала запись». SPI1 и PWM включаются из работающего Linux одной записью
> каждый, без всякого `Cmwp`. См. секцию «ПИТАНИЕ ТАЧА» в конце файла и
> `docs/research/p105-pmgr-gates.md`.

**Проверено 2026-09-18, путь kDFU ничего не даёт.** Загрузка через `kloader` из
живой джейлнутой iOS, где тач работает, состояние питания не наследует:
`peek r 32100000 8` и `peek r 33500300 1` отдают `0xd00c3ccc` во всех словах —
столько же смысла, сколько в `peek r 38000000 2` (ничей адрес, тот же ответ),
при том что PMGR, GPIO и UART0 в той же загрузке читаются нормально. IRQ 50
(`32100000.spi`) и IRQ 52 (`apple-z2`) остались нулевыми. iBSS и iBEC
переинициализируют клоки независимо от того, что было поднято в iOS.

**Даже если бы touch отработал** — touch IRQ 51 тоже не доедет до CPU (тот же блокер).

# ═══════════════════════════════════════════════════════════════
# XNU реверс (историческая справка, не активно)
# ═══════════════════════════════════════════════════════════════

Класс USB PHY: `AppleS5L8930XUSBPhy` (kernelcache.release.p105, iOS 8.4.1)
- `start()` = `FUN_80cab844` @ `0x80cab844`
- Реальная vtable: **`0x80cad090`**
- state-machine = `FUN_80cabea8` @ vtable-слот `0x35c`

## Расшифровка kernelcache (если понадобится другой build)
```bash
git clone --depth 1 https://github.com/tihmstar/fwkeydb /tmp/fwkeydb
# ключи: /tmp/fwkeydb/keys/firmware/iPad2,5/0x8942/<BUILD>
# IV/Key для 12H321 (Donner) подтверждены рабочими
```
Формат: IMG3 → AES-CBC(IV,Key) → `comp`+`lzss` заголовок → Apple LZSS decompress → Mach-O.

## Ghidra — практические уроки
- Headless без `-analysis` НЕ резолвит C++ vtable/RTTI
- GUI CodeBrowser с полным Auto-Analyze резолвит строки полностью, но не vtable сторонних kext'ов
- Поиск vtable: сканировать не-`__TEXT` память на длинные (~90+) подряд идущие 4-байтные
  прямые адреса в `.text`, найти таблицу с уже известной функцией

## LZSS repack (для XNU-хуков — не USB)
Оригинальный `create_lzss_payload.py` **сломан** (output not decodable).
Правильный replacement: `ipad-mini-linux/lzss_ok.py` + `repack2.py` (в /home/claude/ есть копии).

## Активная XNU-hook инфраструктура (WORKING)
Патч iBEC: `iBEC.serial` — replaced `pio-error=0` with `serial=3` — XNU console идёт в
uart0/DCSD, полные TEXT логи. Kernel-hook в `IOFindBSDRoot` (trampoline @ code-cave
`0x80083108`), reads via `ml_phys_read` (`0x8007f3d4`), output via `kprintf` (`0x8027a7f0`).
PMGR/GPIO читаются без проблем; unpowered blocks (0x321/0x335/0x332) fault на первом read.
Details: `iBSSloader/docs/p105-mt-peek.md`.

# ═══════════════════════════════════════════════════════════════
# Приоритетный порядок работ (после IRQ delivery)
# ═══════════════════════════════════════════════════════════════

1. **AIC1 IRQ delivery** ← блокер, решить первым (LATE-SMOKE test → next steps)
2. **USB enumeration** — сразу разблокируется если IRQ решится, DWC2 полностью готов
3. **Touch events** — потребует ещё Cmwp reverse (но UART/framebuffer как ввод достаточен)
4. **WiFi (BCM4334 HSIC)** — альтернативный путь к SSH, независим от USB и touch
5. **CPU1/SMP** — есть IPI код в AIC1, ждёт разблокирования IRQ

## WiFi — заметка на будущее
BCM4334 combo (WiFi+BT). SDIO/UART, HSIC для WiFi. Полностью независим от AIC1 IRQ decision
(если WiFi driver может работать через polling — TBD). Хардкод SSID/pass в init для теста:
```
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
udhcpc -i wlan0
```

# ═══════════════════════════════════════════════════════════════
# Долгая история USB реверса (Unicorn + QEMU) — итог
# ═══════════════════════════════════════════════════════════════

Прошли: kernelcache decrypt, Ghidra GUI reverse (vtable `0x80cad090`, state-machine
`FUN_80cabea8`), Unicorn эмуляция (дошли до конца без крашей, но 0 MMIO writes —
child refcount-заглушки возвращали не те значения), QEMU danzatt/QEMU-s5l89xx-port
native build + offset trick (дошли до правильной функции, встали на child-object stub).

**Вывод**: технически близко, но с открытием AIC1 фиксов и empirical PHY dump'а —
эта дорога сейчас **менее приоритетна**. Драйвер работает на реальном железе; после
разблокирования IRQ delivery можно вернуться если понадобится тонкая tuning constants.

Ссылки на артефакты этого пути:
- `/tmp/emulate_phy6.py` — Unicorn каркас (пересоздать по этой памятке при необходимости)
- danzatt/QEMU-s5l89xx-port: `-M ipad1g` эмулирует A4, не A5 (готового A5-порта нет)
- Offset trick: `restore /tmp/seg_PRELINK_TEXT.bin binary 0x40460000` (kernelcache_vaddr - 0x40000000)

## Диагностика USB на физическом уровне (если понадобится)
Логическая проверка "физически ли PHY подаёт сигнал" — не декодирование протокола
(для honest HS 480 Mbps нужны GHz-анализаторы). 24 MHz достаточно для activity/chirp.
Варианты: Raspberry Pi Pico (уже есть) > дешёвый CY7C68013A Saleae clone (~$10-15,
sigrok/PulseView) > оригинальный Saleae.

**Пользователь имеет Saleae Logic 24 MHz** — можно сравнить USB signalling при iBEC-boot
(где USB работает — DFU/Recovery) vs Linux-boot чтобы поймать различие. Не сделано —
блокер IRQ delivery отменил необходимость на данном этапе.

# ═══════════════════════════════════════════════════════════════
# РАЗГАДКА IRQ-БЛОКЕРА (2026-09-14) — AIC1 глушился в kernel_init()
# ═══════════════════════════════════════════════════════════════

## Корневая причина

`init/main.c` → `kernel_init()` содержит ДВА Apple-блока. Второй из них выполняется
**прямо перед `kernel_init_freeable()` → `do_initcalls()`** и делает:

```c
apple_aic1_quiesce();          /* aic1_clear_sticky_nirq(): MASK_SET всем,
                                  TARGET_CPU=0 всем, IPI_MASK_SET, drain,
                                  CFG &= ~ENABLE                              */
apple_aic1_hw_quiesce_quiet(); /* то же самое ещё раз через статический маппинг */
early_boot_irqs_disabled = false;
local_irq_enable();
```

Ничего после этого AIC не включает. `apple_aic1_enable()` / `aic1_enable_hw()`
вызываются только из `aic1_of_init()` (очень рано) и из ПЕРВОГО блока, который
второй затем перетирает.

**Состояние AIC на момент, когда dwc2 делает `request_irq(50)`:**
- `AIC1_CONFIG.ENABLE = 0` — контроллер выключен
- `TARGET_CPU[0..191] = 0` — ни одна линия не нацелена ни на один CPU
- всё замаскировано, IPI тоже

Второй независимый убийца: `aic1_irq_unmask()` писал **только** `MASK_CLR` и не
трогал `TARGET_CPU`, поэтому даже размаскированная драйвером линия оставалась
без маршрута.

Это полностью объясняет прошлые наблюдения:
- SW-тест в `aic1_of_init` показывал корректную постановку событий (`EVENT=0x10000`) —
  он выполняется ДО второго блока;
- `/proc/interrupts` = 0 по ВСЕМ линиям, включая IPI0-6 — quiesce глушит и IPI;
- вывод «между выходом AIC1 и nIRQ пином CPU железный gap» был **преждевременным** —
  железо ни разу не проверялось во включённом состоянии.

Также: `apple_aic1_arm_cpu0()` вопреки названию делает `MASK_SET(~0)` дважды и
ни разу `MASK_CLR` — то есть «arm» = «замаскировать всё, нацелить на CPU0».
Поведение правильное, имя обманчивое.

## Фикс (2026-09-14)

1. `iBSSloader/patches/files/drivers/irqchip/irq-apple-aic1.c`:
   - `aic1_irq_unmask()` теперь пишет `AIC1_TARGET_CPU + hw*4 = BIT(0)` перед `MASK_CLR`
   - новая публичная `apple_aic1_rearm()`: маскирует все линии, ставит TARGET=CPU0
     всем, дренит EVENT, размаскирует IPI, `aic1_enable_hw()`, печатает
     `AIC1-REARM: CONFIG=... (ENABLE=1)`
   - LATE-SMOKE печатает CONFIG/ENABLE (иначе вердикт ложноотрицательный)
2. `~/Desktop/linux-kernel/init/main.c`: вызов `apple_aic1_rearm()` +
   стамп `irq_rearm` между `apple_aic1_hw_quiesce_quiet()` и `early_boot_irqs_disabled = false`
3. `iBSSloader/scripts/apply-p105-boot-hacks.py`: добавлен REPLACEMENT для того же
   (чистое дерево получит фикс автоматически)
4. `iBSSloader/build/initramfs-root/init`: в grep добавлен `AIC1-REARM`
5. `iBSSloader/dtb/p105ap.dtb` пересобран — `initcall_debug` убран (освобождает ~70 KB UART)

Бэкапы: `*.bak-prerearm` рядом с каждым изменённым файлом.

**Штормом не грозит**: все железные линии остаются ЗАМАСКИРОВАННЫМИ, включается
только контроллер и маршрутизация. Размаскирует линию только `request_irq()`
конкретного драйвера. Липкая IRQ 110 (виднелась как `EVENT pre=0x1006e`) останется
под маской.

## Сборка и прошивка

```bash
cd ~/Desktop/ipad-mini-linux
./rebuild-rearm.sh     # kernel + splice + bundle + verify маркеров в vmlinux
./flash-rearm.sh       # UART capture в фон + checkm8 boot chain, лог в uart_rearm.txt
```

## Что читать в логе

| Строка | Значение |
|--------|----------|
| `AIC1-REARM: CONFIG=0x10773 (ENABLE=1)` | re-arm отработал, контроллер жив на входе в initcalls |
| `LATE-SMOKE: ... CONFIG=... (ENABLE=1) ... handler_entries=N` | N>0 → CPU БЕРЁТ IRQ-исключение, pipeline живой |
| `/proc/interrupts` ненулевой у 50 | USB enumeration должен пойти сам собой |
| `LATE-SMOKE: FAIL ... ENABLE=0` | re-arm не доехал — искать, кто ещё гасит CFG |
| `LATE-SMOKE: FAIL ... ENABLE=1, handler_entries=0` | ВОТ ТОГДА гипотеза железного gate становится актуальной |

## Замеченная мина на будущее

Дерево `~/Desktop/linux-kernel/init/main.c` **разошлось** с
`apply-p105-boot-hacks.py`: у скрипта anchor'ы уже не совпадают с тем, что лежит в
дереве (в дереве больше forward-деклараций). `apply_replacements()` молча
пропускает несовпавшие anchor'ы, так что сборка не ломается — но **чистое дерево
получит более старый main.c**. Дерево сейчас де-факто источник истины для main.c.

# ═══════════════════════════════════════════════════════════════
# ПОСЛЕ IRQ-ФИКСА (2026-09-14/15) — что заработало и куда дальше
# ═══════════════════════════════════════════════════════════════

## Результат re-arm фикса (лог uart_rearm.txt)
```
AIC1-REARM: CONFIG=0x10773 (ENABLE=1) all lines masked, TARGET=CPU0, IPI live
LATE-SMOKE: CPSR=0xa0000053 (I=0) CONFIG=0x10773 (ENABLE=1) handler_entries=27
LATE-SMOKE: after SW_SET+50ms: handler_entries=28 (delta=1) -> PASS
IRQ: 50:  116  APPLE-AIC1  11 Edge  36100000.usb     Err: 0
```
`handler_entries=27` ДО smoke-теста = 27 реальных железных IRQ уже взято CPU.
Гипотеза «AIC1 output не доходит до nIRQ пина» — закрыта.

**USB энумерировался целиком**: USBRst → EnumDone (high-speed) → SET_ADDRESS(5)
принят (никаких STALL 25) → GET_DESCRIPTOR device/config/strings →
**SET_CONFIGURATION(1)** → SET_INTERFACE(1,alt1) → CDC ECM SET_ETHERNET_PACKET_FILTER.
macOS забиндил гаджет. `DCFG=0x00840050` (адрес 5), ep1–ep4 активны.

Touch не изменился: `irq/51-apple-z2` регистрируется, но 0 срабатываний;
SPI1 (IRQ 49) = 0. Cmwp остаётся блокером тача.

## Ловушка в логах
PMCCNTR 32-битный и на ~1 ГГц заворачивается каждые **~4.3 с**, поэтому
printk-таймстемпы циклятся ВНУТРИ одного бута (`4.32 → 0.03`). Это не
перезагрузки. Не пугаться.

## Сборка N+1: CDC ACM вместо g_ether
```
config/p105ap.config:  USB_ETH off, USB_G_SERIAL=y, USB_U_SERIAL=y,
                       USB_DWC2_VERBOSE off (он занимал ~90% лога)
/init:                 respawn-цикл setsid /bin/sh на /dev/ttyGS0
На Mac:                ls /dev/cu.usbmodem* ; screen /dev/cu.usbmodem<...> 115200
```
Только один legacy-гаджет может забиндить UDC — поэтому g_ether выключен.

## /bin/peek — MMIO-инструмент в initramfs
В busybox этого rootfs **нет applet'а devmem**, поэтому собран статический
ARM-бинарь `iBSSloader/tools/p105-peek.c` → `build/initramfs-root/bin/peek`
(вшивается в zImage через CONFIG_INITRAMFS_SOURCE; `CONFIG_DEVMEM=y`,
`STRICT_DEVMEM` выключен — проверено).
```
peek r    ADDR [WORDS]         дамп 32-битных слов
peek w    ADDR VAL             запись слова
peek scan ADDR WORDS [SPIN]    два замера окна, печатает только изменившиеся слова
```
Две вещи, которых нет у обычного devmem:
- SIGBUS/SIGSEGV перехвачены → чтение необеспеченного блока (SPI1 0x321*,
  grape 0x335*) печатает `--------` вместо убийства процесса. Ровно то, что
  нужно для поиска запитанных блоков.
- задержка в `scan` — busy-loop, НЕ sleep. jiffies стоят (см. ниже), любой
  `nanosleep` повесит шелл намертво.

## ГЛАВНОЕ ОТКРЫТИЕ ADT: где искать таймер
Clockevent'а в системе НЕТ вообще — `pmccntr.c` регистрирует только
clocksource + sched_clock + delay_timer. Тика нет, jiffies стоят,
`sleep`/`msleep`/`schedule_timeout` не возвращаются никогда (поэтому в /init
стоит `busywait`). Подтверждение: `IPI1: Timer broadcast interrupts: 0`.

Из `dts/apple-p105ap-raw.json`:
```
/device-tree/arm-io/pmgr   device_type = "timer"     ← !!!
                           reg[0] = 0x0F100000 +0x7000 → phys 0x3F100000
/device-tree/arm-io/wdt    reg = 0x0F103020 +0x10     → phys 0x3F103020, IRQ 4
                           (то есть wdt — под-окно внутри pmgr)

/device-tree/cpus/cpu0  interrupts = <192, 135, 193>
/device-tree/cpus/cpu1  interrupts = <194, 139, 195>
/device-tree/arm-io/aic target-destinations = <135 cpu0, 139 cpu1>
        function-ipi_dispatch       = <aic 'IPID' 192>   (cpu0)
        function-ipi_dispatch_other = <aic 'IPID' 193>   (cpu0)
```
Декодировка тройки: **interrupts[0]=IPI self, interrupts[1]=ТАЙМЕР,
interrupts[2]=IPI other**. Значит **IRQ 135 — таймерное прерывание CPU0**
(и 139 — CPU1), жёстко прибитое к своему ядру через target-destinations.

Итого рецепт clockevent: регистры таймбазы лежат в окне PMGR
`0x3F100000..0x3F107000` (регион читается — подтверждено XNU-хуком),
прерывание = AIC hwirq 135. Осталось найти точные offset'ы счётчика/компаратора.

**Как найти без единой лишней прошивки** — с шелла по ACM:
```sh
peek scan 3f100000 512 4000000      # свободно бегущие слова = счётчик
peek r    3f103000 16               # wdt-подблок целиком
peek scan 3f103000 16  4000000
```
Слово, у которого delta растёт линейно со SPIN — это и есть таймбаза.
Дальше: искать рядом compare-регистр и бит enable, повесить handler на IRQ 135.

## Периферийная карта из ADT (bus-local +0x30000000)
```
aic        0x0F200000  —        gpio  0x0FA00000 irq 119    pmgr 0x0F100000 —
wdt        0x0F103020 irq 4     pl310 0x0E000000 —          perfcounter 0x0F104000
spi1       0x02100000 irq 29    spi2  0x02200000 irq 30
uart0      0x02500000 irq 21    i2c0  0x03200000 irq 18     pwm  0x03500000 irq 16
otgphyctrl 0x06000000 —         usb-complex 0x0F104400 —
clcd       0x0A100000 irq 43,42 mipi-dsim 0x09500000 irq 41
usb-ehci   0x06400000 irq 12  ← занят HSIC-WiFi (child-нода "wlan"!)
usb-ohci0  0x06500000 irq 13    usb-ohci1 0x06600000 irq 14
```
Важно для плана с USB-клавиатурой: **свободного host-порта нет**. EHCI — это
WiFi по HSIC. Клавиатура = dwc2 в host-режиме, то есть взаимоисключающе с ACM
на том же порту, и почти наверняка требует сперва рабочий таймер
(hub_port_init весь на msleep).

# ═══════════════════════════════════════════════════════════════
# ТИК ЗАРАБОТАЛ (2026-09-15) — USB SOF   (с 2026-09-18 только запасной, см. «ТАЙМЕР ВНУТРИ AIC»)
# ═══════════════════════════════════════════════════════════════

```
SOF-TIMER: PASS -- clockevent registered at 8000 Hz (80 SOFs per jiffy). jiffies are live.
sleep 1 && echo ok   -> ok
```

Первый признак был визуальный: **замигал курсор fbcon**. Он моргает по таймеру,
так что при замороженных jiffies моргать не мог в принципе.

## Что перебрано и отвергнуто (всё проверено на железе)
| источник | вердикт |
|----------|---------|
| Apple watchdog 0x3F103020 | счётчик 24 МГц и компаратор РАБОТАЮТ (RESET_EN ребутит по расписанию), прерывание не разведено ни при одной комбинации CTRL |
| остальной PMGR (28 КБ) | ровно один свободно бегущий регистр на всё окно — тот же счётчик |
| A9 private timer PERIPHBASE+0x600 | нули, записи не залипают — PERIPHCLK не подан |
| A9 global timer PERIPHBASE+0x200 | то же самое |
| PMU overflow | `PMCR N=6`, но `PMCEID0=0x0` — ни одного архитектурного события, CPU_CYCLES в том числе |

При этом SCU (+0x000) и GIC CPU interface (+0x100) в том же окне отвечают
осмысленно (`SCU CTRL=0x2d` enable, `CFG=0x511` = два ядра, CPU0 в SMP),
так что окно живое — мертвы именно таймеры.

## Как сделано
```
arch/arm/mach-apple/apple_sof_clkevt.c   clockevent 8 кГц, rating 250
drivers/usb/dwc2/gadget.c                +GINTSTS_SOF в intmsk (под ifdef)
                                         хук ДО spin_lock(&hsotg->lock)
```
Хук намеренно вне замка dwc2: обработчик тика уходит в таймерный и
планировщиковый код, держать поперёк этого блокировку USB-драйвера нельзя.
Регистрация — только после реально увиденных SOF, на late_initcall.
Обе правки dwc2 лежат в apply-p105-boot-hacks.py.

## Ограничения (честные)
- тика нет до энумерации USB — но так было и раньше, регресса нет
- suspend шины хостом останавливает тик
- 8000 прерываний/с — несколько процентов CPU, линия 51 в /proc/interrupts улетает в тысячи
- **host-режим (USB-клавиатура) убьёт этот тик**: SOF в device-режиме исчезнет.
  В host-режиме dwc2 генерирует SOF сам, так что хук надо будет продублировать
  в hcd.c — решаемо, но не бесплатно

## Побочно: частота CPU теперь измеряется
```
CALIB: 50000549 CPU cycles per 1200013 ticks of 24 MHz => CPU = 1000000146 Hz
```
Калибровка PMCCNTR против 24 МГц счётчика ватчдога на старте. Старая
захардкоженная догадка «fixed 1 GHz» оказалась верной с точностью до 0.6 ppm,
но теперь это измеренный факт и он уходит в clocksource, sched_clock и udelay.

# ═══════════════════════════════════════════════════════════════
# СЕТЬ ПО USB ЗАРАБОТАЛА (2026-09-16) — g_cdc
# ═══════════════════════════════════════════════════════════════

```
UDC: 36100000.usb state=configured function=g_cdc speed=high-speed
usb0: addr=10.55.0.2/24 carrier=1 oper=up mac=6e:f1:1c:27:f4:65
ACM nodes: /dev/ttyGS0
SOF-TIMER: PASS -- 835 SOFs counted, clockevent registered at 8000 Hz
LATE-SMOKE: PASS -- handler_entries=1015     (14 сентября было 27)
```
На маке: `networksetup -listallhardwareports` → `Hardware Port: CDC Composite
Gadget`, `Device: en10`. Пинг 0.58/0.76/0.99 мс, потерь нет.

## Почему g_cdc, а не configfs (ГЛАВНОЕ, не сломать)
Тик = SOF-прерывание dwc2. SOF идут только когда хост энумерировал устройство,
а это происходит только когда гаджет ЗАБИНДИЛСЯ и подтянул D+. Legacy-гаджет
биндится из своего initcall, то есть ДО `late_initcall`, где регистрируется
`apple_sof_clkevt`. configfs-гаджет биндит userspace записью в `$GADGET/UDC` —
это `/init`, сильно позже: таймер не увидит SOF, откажется регистрироваться,
и система поднимется с замороженными jiffies.

То же внутри `/init`: между soft-disconnect и reconnect SOF нет, значит нет
jiffies, значит `sleep` НЕ ВЕРНЁТСЯ. Это окно проходится только CPU-спином.

## Настройка на маке
```bash
networksetup -listallhardwareports | grep -B1 -A2 'CDC Composite'
sudo ifconfig enN 10.55.0.1 netmask 255.255.255.0 up
ping 10.55.0.2
```

## Цена итерации шелл-цикла: ~60 мкс
Измерено по таймстемпам лога: disconnect в 2.88 с, "configured after 2 polls"
в 26.77 с → 200000 итераций `i=$((i+1))` в ash = ~12 СЕКУНД. Унаследованный
комментарий «~200 ms» был враньём и в одиночку составлял весь долгий USB-инит.
Сейчас `settle()` = 5000 итераций (~300 мс), `blip()` = 1700 (~100 мс).

## Сдвинулась нумерация IRQ
Было `50: ... 36100000.usb`. Стало:
```
50:      0  APPLE-AIC1  29 Edge  32100000.spi
51: 107847  APPLE-AIC1  11 Edge  36100000.usb, 36100000.usb
```
hwirq не менялись (USB = 11, SPI = 29) — переехали только номера Linux.
Скрипты, которые грепают по номеру, должны брать обе строки.

## Шелл по сети — пока костыль
В busybox этого rootfs НЕТ ни `telnetd`, ни `httpd`, ни `udhcpd`, ни `sshd`
(проверено грепом по бинарю — это отдельные пакеты Alpine). Временно стоит
FIFO-трюк на `nc`, порт 2323. `/bin/sh` со stdin из пайпа считает себя
неинтерактивным и не печатает приглашение — отсюда «висит». Лечится `sh -i`.
Нормальное решение — положить в initramfs dropbear.

## Три дефекта, которые показал сетевой шелл (2026-09-16, вечер)
1. **MAC гаджета случайный на каждый буст.** В логе `6e:f1:1c:27:f4:65`,
   на следующем busте `da:5b:e0:9b:ef:1d`. macOS именует интерфейсы по MAC,
   значит каждый раз новый `enN` и заново `ifconfig`. Закреплено в bootargs:
   `g_cdc.host_addr=02:10:5a:05:00:01 g_cdc.dev_addr=02:10:5a:05:00:02`.
2. **`lo` поднимается DOWN** — в голом initramfs его некому поднять. Всё, что
   ходит на 127.0.0.1, молча не работает. `ip link set lo up` в /init.
3. **hostname `(none)`** — просто не задан. `hostname p105`.

## DTB теперь собирается внутри rebuild-rearm.sh
Раньше это был ручной шаг, и правка `dts/p105ap.dts` могла тихо не доехать:
сборка сплайсит тот .dtb, что лежит, и никто не ругается. Теперь шаг 1/6
делает `rm` + `dtc` и проверяет, что MAC'и в bootargs на месте.

## SSH: почему apk нельзя просто установить
Rootfs — Alpine **armhf**, хост — Apple Silicon, а на M-серии **нет AArch32
EL0 вообще**: 32-битный ARM-код не исполняется ни нативно, ни в Docker. Значит
`apk add` против этого дерева не запустить никогда. `scripts/add-dropbear.sh`
качает .apk (это просто склеенные gzip-потоки tar) и распаковывает руками —
ничего целевой архитектуры не исполняется.

Ключ хоста (с 2026-09-19) делается **один раз на сборке**:
`./cascadia build` → `scripts/dropbear-hostkey.py` → `build/keep/etc/dropbear/`
→ в образ, а stage 1 переносит его и на NFS-корень. Отпечаток печатается в
шаге 2/6. Раньше ключ генерил `dropbear -R` на устройстве, и на RAM-корне он
был новый на каждый буст (`REMOTE HOST IDENTIFICATION HAS CHANGED`). После
перехода один раз: `ssh-keygen -R 10.55.0.2`, дальше просто
```bash
ssh root@10.55.0.2
```

# ═══════════════════════════════════════════════════════════════
# apk И NFS-ROOT (2026-09-17)
# ═══════════════════════════════════════════════════════════════

```
10.55.0.1:/Users/k/cascadia-root on / type nfs (rw,vers=3,nolock,proto=tcp)
df -h /   ->   926.3G, использовано 58%
apk update -> OK: 25140 distinct packages available
```

## Ключевое заблуждение, которое стоило круга
«apk против этого дерева не запустить никогда» — верно ТОЛЬКО про мак.
На M-серии нет AArch32 EL0, 32-битный ARM там не исполняется вообще. Но
устройство — это armv7-машина, и `/sbin/apk` в Alpine minirootfs лежит с
самого начала, вместе с ключами подписи и CA-бандлом. Как только появилась
сеть, `apk add` заработал сам.

Через `apk-unpack.py` имеет смысл ставить только то, что нужно ДО apk:
dropbear (без него нет шелла по сети) и `mount.nfs` (корень не смонтировать
бинарём, который лежит на корне). Остальное — `apk add` по ssh.

## Порядок поднятия
```bash
# один раз
./cascadia rootfs          # dropbear с твоим ключом и mount.nfs ставятся здесь
./cascadia nfs on          # экспорт корня с мака + /etc/nfsroot в initramfs
./cascadia build && ./cascadia flash

# каждый раз после прошивки -- адрес 10.55.0.1 на маке flash выставляет сам
# (отдельно: ./cascadia link)
./cascadia net on
ssh root@10.55.0.2
```

## Грабли, собранные по дороге
| что | почему |
|---|---|
| `flash` без `rebuild` | заливает старый бандл; uptime свежий, содержимое старое. Проверка свежести теперь обходит ВСЁ дерево initramfs, а не только `/init` |
| `# CONFIG_NFS_FS is not set` в секции trim | merge_config берёт последнее слово, и трим двадцатью строками ниже отменял включение. Ловится `scripts/check-config-fragment.awk` |
| маркер `# cascadia` в `/etc/exports` | у macOS комментарий — только целая строка; хвостовой текст парсится как имена хостов |
| `PTY allocation request failed` | не смонтирован devpts. `/dev/ptmx` даёт devtmpfs, слейв-сторона живёт в devpts |
| ключ хоста меняется каждый буст | `dropbear -R` пишет его на корень. На RAM — новый каждый раз, на NFS — постоянный. Заодно это индикатор, какой корень загрузился |
| повторный `mac-nfs-export.sh on` | `~/cascadia-root` теперь ЖИВОЙ корень; `rsync --delete` снёс бы всё установленное. Скрипт отказывается перезаписывать непустой каталог без `FORCE_SYNC=1` |

# ═══════════════════════════════════════════════════════════════
# BOOT CHAIN ВОСПРОИЗВОДИТСЯ ИЗ IPSW (2026-09-17)
# ═══════════════════════════════════════════════════════════════

```bash
./cascadia firmware    # iBSS.patched + iBEC.patched.autogo.dfu из своего IPSW
./cascadia flash       # свежесобранный
./cascadia flash --known-good   # августовский образ, если надо отделить
                                # плохую сборку от плохого стенда
```
Свежесобранный **загрузился на железе**. Расшифрованная прошивка Apple больше
не нужна ни в репозитории, ни рядом с ним.

## Что выяснилось про autogo
Это НЕ `iBoot32Patcher -c "go"`: та опция выполняет команду сразу при разборе,
и 13-мегабайтный бандл не успевает доехать — загрузчик стартует после первого
32 КБ чанка. Реальный autogo — рукописный хук в колбэк завершения USB-передачи,
срабатывающий только на коротком пакете (EOF). Суффикс `.lk` = linux-boot.

Из-за жёстко зашитого адреса хука всё прибито к **iPad2,5 / 12H321**.

## Грабли сборки прошивки
| что | почему |
|---|---|
| `hook target wrong: #0x9ff43dea`, промах на 0x24 | gcc в Ubuntu включает `--build-id`, линкер кладёт `.note.gnu.build-id` по адресу из скрипта — ровно 36 байт — и `.text` съезжает. Лечится `-Wl,--build-id=none`, как уже сделано в `build-staging-bundle.sh` |
| образ собрался, а `iBoot32Patcher` отсутствует | `RUN` идёт через `/bin/sh` без раскрытия `{a,b,c}`, исходники лежат в корне репо, `-I` должен быть корнем, а хвостовой `|| true` глушил всё это |
| `.dfu` не совпадает с эталоном, хотя plaintext совпадает | эталон — сохранённый в августе бинарь, старше текущего `patch-ibec-autogo.py`. Сверять надо открытый текст, а не шифротекст: один изменённый блок в AES-CBC портит всё после себя |
| `duser[@]: unbound variable` только на маке | bash 3.2 считает раскрытие пустого массива под `set -u` ошибкой; в 4.4 разрешили |
| по kDFU iBEC заливается на 100%, потом устройство «выключается» | образ был шифрованный: после загрузки iOS ключа GID нет, KBAG расшифровать нечем. Нужен `iBEC.patched.autogo.plain.dfu` |

# ═══════════════════════════════════════════════════════════════
# ТАЙМЕР ВНУТРИ AIC (2026-09-18) — найден в iBEC, счётчик подтверждён
# ═══════════════════════════════════════════════════════════════

Окно AIC ни разу не сканировали — искали по PMGR. А iBoot, в отличие от XNU,
ходит в регистры литералами, и его драйвер AIC читается напрямую
(`build/firmware/iBEC.dec`, база 0x9FF00000, всё в 0x9FF01500..0x9FF01B40):

```
0x9ff01b00  timer_get_ticks:  hi=[AIC+0x28]; lo=[AIC+0x20]; hi2=[AIC+0x28]; повтор, пока hi!=hi2
0x9ff01b2c  tick_rate:        return 24000000
0x9ff01a68  deadline_enter:   [+0x2014]=~0; [+0x2010]|=1; [+0x2018]=1; [+0x2014]=deadline-now
0x9ff01a24  timer ISR:        [+0x2010]&=~1; [+0x2018]|=1; callback()
0x9ff018ec  IRQ dispatch:     EVENT 0x00070001 (тип 7, №1) -> таймер; потом [+0x2020]=2
0x9ff019e4  init:             [+0x2010]=0xe; обработчик на «irq 0xC1»; [+0x2020]=2
```

| регистр | что это |
|---|---|
| AIC+0x20 / +0x28 | TIME_LO / TIME_HI, 64 бита, 24 МГц |
| +0x2010 | конфиг локальных событий, bit0 = таймер включён |
| +0x2014 | обратный отсчёт в тиках таймбазы |
| +0x2018 | статус, запись 1 = сброс |
| +0x201c / +0x2020 | маска локальных событий set / clear, таймер = bit 1 |

**Подтверждено на железе (peek, чтение):** `3f200020` бежит, за `sleep 1`
прибавляет ~24.35M — ровно как счётчик ватчдога `3f103020`, и младшие 32 бита
у них совпадают с точностью до задержки между чтениями. Это одна таймбаза.

**Не читать `3f202004` и `3f205004` из peek**: это EVENT, чтение подтверждает
прерывание, линия остаётся замаскированной — можно убить USB, тик и сессию.

**Clockevent** — в `irq-apple-aic1.c`, `late_initcall(aic1_timer_init)`:
взводит 10 мс сначала через окно-алиас 0x2000, потом через per-CPU 0x5000,
ждёт до 200 мс по самой таймбазе. Регистрируется (rating 400, выше SOF 250)
только если событие пришло; иначе всё остаётся как было.

| строка в dmesg | значит |
|---|---|
| `AIC-TIMER: ... a 10 ms shot fired after ~10000 us` + `PASS` | таймер работает, единицы — тики 24 МГц, тик больше не зависит от хоста |
| `fired after` сильно не 10000 | работает, но единицы другие — смотреть число |
| `nothing in 200 ms ... CNT a -> b` | не сработало; если CNT менялся — отсчёт идёт, а событие не доходит |
| `unexpected local event` | событие типа 7, но с другим номером — тоже зацепка |

Приёмочный тест — как с SOF: после загрузки **выдернуть кабель** и смотреть на
курсор fbcon. С тиком от SOF он замирает, с таймером AIC должен мигать дальше.

## Итог (2026-09-18/19): работает

```
AIC-TIMER: alias window 0x2000: nothing in 200 ms. CFG was 0x0, CNT 0x0 -> 0x0, STAT 0x0
AIC-TIMER: cpu0 window 0x5000: a 10 ms shot fired after 10003 us (CFG was 0x0)
AIC-TIMER: PASS -- clockevent registered at 24 MHz, rating 400.
/sys/devices/system/clockevents/clockevent0/current_device -> apple-aic1-timer
```

- Алиас `0x2000` из Linux мёртвый (всё нули, события нет) — так же, как
  «классический 0x2004 всегда 0». Живая копия — per-CPU `0x5000 + (cpu << 7)`,
  значит у каждого ядра свой таймер.
- 10003 мкс на выстрел 10 мс: единицы — тики 24 МГц, рецепт iBoot точный.
- Таймер регистрируется на **arch_initcall** — раньше, чем dwc2 впервые
  собирает маску прерываний. dwc2 спрашивает `apple_s5l_usb_sof_wanted()` и
  при живом таймере AIC **не включает SOF**: 8000 прерываний/с больше нет, а
  `SOF-TIMER` пишет `not needed`. Если самопроверка таймера провалится, всё
  возвращается к SOF, как было.
- LATE-SMOKE больше не «дренирует» EVENT в конце: чтение EVENT подтверждает
  и маскирует ожидающее событие, а проглоченное событие таймера навсегда
  остаётся замаскированным — тик бы просто встал.

**Правка `patches/tree/0001-cascadia.patch` больше не требует перекачивать
ядро.** `apply-kernel-edits.sh` помнит применённую версию
(`<дерево>/.cascadia-applied-0001-cascadia.patch`), а для дерева, пропатченного
до появления этой записи, ищет применённую версию в истории git самого
репозитория, откатывает её и накладывает новую. Проверено на трёх сценариях:
старая версия без записи, повторный запуск, новая правка поверх записи.

# ═══════════════════════════════════════════════════════════════
# ПИТАНИЕ ТАЧА (2026-09-18/19): гейты лежали на 10 id ниже
# ═══════════════════════════════════════════════════════════════

Всё, что три недели пыталось включить SPI1 и PWM, писало не в тот регистр.
Не не тот бит и не слишком рано — не тот регистр, ровно на 10 id.

```
регистр power-state = 0x3f100fd8 + <id из ADT clock-gates> * 4
```

Найдено перебором с живого Linux (`tools/pmgr-map.sh`): дамп всего окна PMGR,
затем по очереди включить каждый регистр, похожий на выключенное устройство, и
после каждого смотреть, не начал ли отвечать блок.

```
SPI1 (0x32100000) ожил на записи в 0x3f1010e8  ->  ADT gate 68
PWM  (0x33500000) ожил на записи в 0x3f101124  ->  ADT gate 83
```

| Откуда формула | Формула | Куда попадала для spi1 (68) |
|---|---|---|
| измерено | `0x3f100fd8 + id*4` | `0x3f1010e8` — spi1 |
| дизасм XNU | `pmgr + 0x1000 + id*4` | `0x3f101110` — дырка, регистра нет |
| хелпер iBEC | `pmgr + 0x1008 + id*4` | `0x3f101118` — **i2c0**, и так включён |

Отсюда старое «гейт читается включённым (`g68=2FF`), а блок мёртв»: читался
гейт I2C-контроллера, на котором висит PMU, включённый задолго до нас.

**Массив:** id 12..90, 71 регистр, дырки на 29, 30, 63–66, 71, 78. Выше id 90
регистров нет вообще — дисплей (clcd 103/127), sgx, isp переключаются чем-то
другим (кандидаты: битовые маски `0x3f101200/0x1204`, и `0x3f101180`, которому
XNU ставит бит 31). Бит 9 стоит у каждого реализованного регистра; сплошной
ноль — это не «выключено», а «регистра здесь нет».

**Как включать** (рецепт XNU `clock_gate_switch`, он всегда был верным):

```
записать (v & ~0x10f) | 0xf, затем ждать, пока биты 7:4 не сравняются с 3:0
```

**Как читается мёртвый блок.** Не константа: шина отдаёт то, что по ней в
последний раз ехало — видели `0xd00c3ccc`, `0x0015006b`, `0x006b18f4`. Поэтому
проверка «жив ли блок» — сравнение с чтением `0x38000000`, где нет ничего:
одинаково два раза подряд = мёртв. После включения оба блока читаются нулями.

**Состояние массива на свежей загрузке Linux** (то, что оставил iBoot):
включены uart0–uart3, uart5, i2c0–i2c2, флеш-контроллер, usb-complex (87–89);
выключены spi1, spi2, pwm, pke, sha2, iop.

**Скрипты** (все гоняются по ssh: `ssh root@10.55.0.2 sh -s < tools/<скрипт>`):

```
tools/touch-power-probe.sh   состояние как загрузилось + старый рецепт
tools/pmgr-map.sh            перебор, которым найдена формула
tools/touch-power-on.sh      включить spi1 и pwm, снять регистры блоков
```

**Что это не решает.** `Cmwp` всё ещё нужен, но уже не для питания, а для
программирования: PWM-канал 2 должен выдавать 32768 Hz (ADT
`/arm-io/pwm/grape-clk`: `reg = 2`, `default-hz = 0x8000`). И карта clock-id →
регистр всё ещё догадка (`0x3f100010/20/2c` для id 4/304/307): гейты оказались
на 10 ниже, к клокам то же недоверие.
