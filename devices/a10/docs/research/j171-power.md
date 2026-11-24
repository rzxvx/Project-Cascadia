# J171: power -- battery, charging, the USB source, the clock

## Battery

The fuel gauge is a TI bq27540-family part on **HDQ** (`gas-gauge,bq27540`
in the ADT; Linux binds `ti,bq27545-hdq`), its one wire a UART run as HDQ
(`ti,hdq-uart`: the w1 master over serdev, through the SN2400 mux),
Pauli1Go's drivers. A cache worker does the HDQ reads, so userspace reads
do not each start a transaction.

```
w1_master_driver w1_bus_master1: Attaching one wire slave 01.000000000000 crc 3d
```

`/sys/class/power_supply/battery`: status, capacity, voltage, current,
temperature, cycles. Seen right after boot: "Charging", 59 %. At the very
first boots, before the cache filled, `battery cache refresh failed: -5`
and `driver failed to report 'status'` -- gone once it had a value.

## Charging

The PMU is a Dialog **D2365** (`pmu,d2365` at I2C0 0x3c); its charger block
(`apple,d2365-charger`) and GPIOs have Pauli1Go's drivers. Linux shows
`charger` (status "Charging" while on the cable). Charging control -- the
input current, the policy -- is not touched here.

## The USB source: `power_supply usb` (not yet)

The device: **Bellatrix**, `orion,bellatrix` at I2C1 0x13 in the J171's
ADT (`orion-ic`), `apple,bellatrix` in the DT; Pauli1Go's driver reports
the USB power source as `power_supply usb`. On the J171 it does not answer:

```
i2c-apple 20a111000.i2c: NACK, status 0x10210100
power_supply usb: driver failed to report `online' property: -6
i2c-apple 20a111000.i2c: NACK, status 0x00210118
i2c-apple 20a111000.i2c: Bus stuck, status 0x18010110
power_supply usb: driver failed to report `online' property: -5
```

The chip is there (the ADT says so); what the J172 does differently --
its power-up, a reset line, the bus's speed -- is the open question.
Nothing depends on it yet: charging goes on without it.

## The clock

The PMU's RTC (`rtc-apple-pmic`, `pmic@3c:rtc@500`) sets the system clock at
boot. No NTP yet; on Wi-Fi it would be one `ntpd` away.

## Elsewhere on the buses

- I2C0 also: `tristar` (`tristar,cbtl1610`, the Lightning port's mux). The
  DT warns `OF: Bad cell count for /soc/i2c@20a110000/pmic@3c` six times at
  boot; harmless so far.
- I2C2 0x39: `als1`, a **CT819** ambient light sensor (`als,ct819`, with its
  calibration). Pauli1Go has it working on the J172 (CT819 calibration in
  his m1n1); not looked at here yet.
- The backlight: the PMU's WLED (`wled-enables` in the ADT); left as iBoot
  sets it.
