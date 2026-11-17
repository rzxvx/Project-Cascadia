// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Corellium LLC
 * Modified for HoolockLinux J172 HBPP14/Z2FW support in 2026.
 */

#ifndef _MTFW_H
#define _MTFW_H

#include <stdbool.h>
#include <stdint.h>

#define MTFW_WRITE      1
#define MTFW_WRITE_ACK  2
#define MTFW_WAIT_IRQ   3
#define MTFW_SET_TYPE   4
#define MTFW_SET_CONFIG 5
#define MTFW_RAW_XFER   6
#define MTFW_SEND_CALIBRATION 7

#define MTFW_CAL_MULTI_TOUCH 0
#define MTFW_CAL_ORB_GAP     1
#define MTFW_CAL_ORB_FORCE   2
#define MTFW_CAL_SHAPE_ACCEL 3

#define MTFW_RAW_XFER_RX_BIT_REVERSE (1u << 0)

#define MTFW_CONFIG_MIN_DMA_VALID       (1u << 0)
#define MTFW_CONFIG_Z2_DELAY_VALID      (1u << 1)
#define MTFW_CONFIG_CS_DELAY_VALID      (1u << 2)
#define MTFW_CONFIG_CPHA_VALID          (1u << 3)
#define MTFW_CONFIG_CPOL_VALID          (1u << 4)
#define MTFW_CONFIG_WORD_DELAY_VALID    (1u << 5)
#define MTFW_CONFIG_CLOCK_PERIOD_VALID  (1u << 6)
#define MTFW_CONFIG_POWER_RESET_VALID   (1u << 7)
#define MTFW_CONFIG_BOOT_TIMEOUT_VALID  (1u << 8)
#define MTFW_CONFIG_OTP_ADDRESS_VALID   (1u << 9)
#define MTFW_CONFIG_CHIP_ID_VALID       (1u << 10)
#define MTFW_CONFIG_HBPP_VERSION_VALID  (1u << 11)
#define MTFW_CONFIG_OTP_SN_VALID        (1u << 12)
#define MTFW_CONFIG_RESET_SEQ_VALID     (1u << 13)
#define MTFW_CONFIG_POWER_SEQ_VALID     (1u << 14)

#define MTFW_CONFIG_SEQ_MAX             160

typedef struct mtfw_config {
    unsigned valid_mask;
    unsigned min_dma_transfer_size;
    unsigned z2_inter_packet_delay_us;
    unsigned cs_delay_us;
    unsigned clock_phase;
    unsigned clock_polarity;
    unsigned word_delay;
    unsigned clock_period_ms;
    unsigned power_off_on_reset;
    unsigned normal_boot_ms;
    unsigned otp_address;
    unsigned chip_id_address;
    unsigned hbpp_version;
    unsigned otp_sn[2];
    unsigned reset_sequence_len;
    unsigned char reset_sequence[MTFW_CONFIG_SEQ_MAX];
    unsigned power_sequence_len;
    unsigned char power_sequence[MTFW_CONFIG_SEQ_MAX];
} mtfw_config_t;

typedef struct mtfw_item {
    unsigned type;
    unsigned char *data;
    unsigned size;
    struct mtfw_item *next;
} mtfw_item_t;

typedef struct mtfw_calibration {
    uint32_t address;
    uint32_t max_size;
    uint32_t provider;
} mtfw_calibration_t;

typedef struct mtfw_load_options {
    bool dynamic_calibration;
    bool otp_preflight;
    /* J171's HBPP14 loader accepts one <= 0x400-byte DATA packet per ACK. */
    bool j171_packetization;
} mtfw_load_options_t;

mtfw_item_t *mtfw_load_firmware(const char *pers, const char *fname,
                                const char *syscfg,
                                const mtfw_load_options_t *options);

#endif
