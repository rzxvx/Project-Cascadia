#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Source objects only, separate prepared kernel tree. No install or device access.
set -euo pipefail
stage_dir=$(cd -- "$(dirname -- "$0")" && pwd)
private_linux=${1:?Pass the separate prepared Linux6.12 tree}
restricted_docker=${2:?Pass the existing resource-limited docker wrapper}
private_image=${CASCADIA_PRIVATE_BUILD_IMAGE:-cascadia-firemarco:a9bf28f}
"$restricted_docker" run --rm --network=none --user 1000:1000 \
  -v "$private_linux:/linux" -v "$stage_dir:/stage" "$private_image" \
  make -C /linux M=/stage ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1 \
  KCFLAGS='-DCONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT=1 -DCONFIG_APPLE_PMU_D1946_ADC6_OWNER_EXPERIMENT=1' \
  apple-pmu-d1946-bank0.o p105-adc6-poll.o p105-owned-adc6.o i2c-s5l8940x.o apple-pmu-i2c.o \
  > "$stage_dir/arm-enabled.log" 2>&1
cp -- "$stage_dir/apple-pmu-i2c.o" "$stage_dir/apple-pmu-i2c-enabled.o"
"$restricted_docker" run --rm --network=none --user 1000:1000 \
  -v "$private_linux:/linux" -v "$stage_dir:/stage" "$private_image" \
  make -C /linux M=/stage ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1 \
  KCFLAGS='-DCONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT=1 -DCONFIG_APPLE_PMU_D1946_ADC6_OWNER_EXPERIMENT=1 -DCONFIG_PM_SLEEP=1' \
  apple-pmu-i2c.o > "$stage_dir/arm-sleep-api.log" 2>&1
cp -- "$stage_dir/apple-pmu-i2c.o" "$stage_dir/apple-pmu-i2c-sleep-api.o"
"$restricted_docker" run --rm --network=none --user 1000:1000 \
  -v "$private_linux:/linux" -v "$stage_dir:/stage" "$private_image" \
  make -C /linux M=/stage ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- W=1 \
  KCFLAGS='' apple-pmu-i2c.o > "$stage_dir/arm-default-off.log" 2>&1
