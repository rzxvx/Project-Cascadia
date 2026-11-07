// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Convert an Apple MTFW HBPP firmware sequence into the simple Z2FW container
 * consumed by Linux's apple_z2 driver.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mtfw.h"

#define APPLE_Z2_FW_MAGIC            0x5746325aU
#define APPLE_Z2_FW_VERSION          1U
#define LOAD_COMMAND_INIT_PAYLOAD    0U
#define LOAD_COMMAND_SEND_BLOB       1U
#define LOAD_COMMAND_WAIT_IRQ        3U
#define LOAD_COMMAND_SET_CONFIG      4U
#define LOAD_COMMAND_RAW_XFER        5U
#define LOAD_COMMAND_SEND_CALIBRATION_V2 6U

#define Z2FW_CONFIG_WORDS            11U

static int write_all(FILE *f, const void *buf, size_t len)
{
    return fwrite(buf, 1, len, f) == len ? 0 : -1;
}

static int write_le32(FILE *f, uint32_t val)
{
    unsigned char buf[4];

    buf[0] = val;
    buf[1] = val >> 8;
    buf[2] = val >> 16;
    buf[3] = val >> 24;
    return write_all(f, buf, sizeof(buf));
}

static void put_le32(unsigned char *buf, uint32_t val)
{
    buf[0] = val;
    buf[1] = val >> 8;
    buf[2] = val >> 16;
    buf[3] = val >> 24;
}

static int write_padding(FILE *f, size_t len)
{
    static const unsigned char zeroes[4];
    size_t pad = (-len) & 3;

    if(!pad)
        return 0;

    return write_all(f, zeroes, pad);
}

static int write_command(FILE *f, uint32_t cmd, const unsigned char *data,
                         uint32_t size)
{
    if(write_le32(f, cmd) || write_le32(f, size))
        return -1;
    if(size && write_all(f, data, size))
        return -1;
    return write_padding(f, size);
}

static int write_config_command(FILE *f, const mtfw_config_t *cfg)
{
    unsigned char buf[Z2FW_CONFIG_WORDS * 4];

    put_le32(buf + 0, cfg->valid_mask);
    put_le32(buf + 4, cfg->min_dma_transfer_size);
    put_le32(buf + 8, cfg->z2_inter_packet_delay_us);
    put_le32(buf + 12, cfg->cs_delay_us);
    put_le32(buf + 16, cfg->clock_phase);
    put_le32(buf + 20, cfg->clock_polarity);
    put_le32(buf + 24, cfg->word_delay);
    put_le32(buf + 28, cfg->clock_period_ms);
    put_le32(buf + 32, cfg->power_off_on_reset);
    put_le32(buf + 36, cfg->normal_boot_ms);
    put_le32(buf + 40, cfg->hbpp_version);

    return write_command(f, LOAD_COMMAND_SET_CONFIG, buf, sizeof(buf));
}

int main(int argc, char **argv)
{
    mtfw_item_t *items, *item;
    FILE *out;
    unsigned config_count = 0, init_count = 0, blob_count = 0, wait_count = 0;
    unsigned raw_count = 0, calibration_count = 0, skipped = 0;
    uint32_t wait_timeout_ms = 1000;
    uint64_t payload_bytes = 0;
    unsigned char wait_buf[4];
    const char *personality, *mtfw, *syscfg, *output;
    int dynamic_calibration;
    mtfw_load_options_t load_options;

    if(argc != 5) {
        fprintf(stderr,
                "usage: makez2fw <personality> <mtfw> <syscfg> <out-fw>\n"
                "       makez2fw --dynamic-calibration <personality> <mtfw> <out-fw>\n");
        return 1;
    }

    dynamic_calibration = !strcmp(argv[1], "--dynamic-calibration");
    if(dynamic_calibration) {
        personality = argv[2];
        mtfw = argv[3];
        syscfg = NULL;
        output = argv[4];
    } else {
        personality = argv[1];
        mtfw = argv[2];
        syscfg = argv[3];
        output = argv[4];
    }

    /*
     * The raw MTFW loader's native HBPP14 construction matches Apple's
     * preconstructed OSData stream. OTP preflight is an explicit converter
     * policy; no parser behavior is inherited from the process environment.
     */
    load_options.dynamic_calibration = dynamic_calibration;
    load_options.otp_preflight = true;
    load_options.j171_packetization = !strcmp(personality, "K1F19,9");

    items = mtfw_load_firmware(personality, mtfw, syscfg, &load_options);
    if(!items) {
        fprintf(stderr, "makez2fw: failed loading MTFW sequence\n");
        return 1;
    }

    out = fopen(output, "wb");
    if(!out) {
        fprintf(stderr, "makez2fw: failed opening %s: %s\n",
                output, strerror(errno));
        return 1;
    }

    if(write_le32(out, APPLE_Z2_FW_MAGIC) ||
       write_le32(out, APPLE_Z2_FW_VERSION))
        goto write_fail;

    for(item = items; item; item = item->next) {
        switch(item->type) {
        case MTFW_SET_CONFIG:
            if(item->data && item->size >= sizeof(mtfw_config_t)) {
                const mtfw_config_t *cfg = (const mtfw_config_t *)item->data;

                if((cfg->valid_mask & MTFW_CONFIG_BOOT_TIMEOUT_VALID) &&
                   cfg->normal_boot_ms)
                    wait_timeout_ms = cfg->normal_boot_ms;
                if(write_config_command(out, cfg))
                    goto write_fail;
                config_count++;
            } else {
                skipped++;
            }
            break;
        case MTFW_WRITE:
            if(write_command(out, LOAD_COMMAND_INIT_PAYLOAD,
                             item->data, item->size))
                goto write_fail;
            init_count++;
            payload_bytes += item->size;
            break;
        case MTFW_WRITE_ACK:
            if(write_command(out, LOAD_COMMAND_SEND_BLOB,
                             item->data, item->size))
                goto write_fail;
            blob_count++;
            payload_bytes += item->size;
            break;
        case MTFW_WAIT_IRQ:
            put_le32(wait_buf, wait_timeout_ms);
            if(write_command(out, LOAD_COMMAND_WAIT_IRQ,
                             wait_buf, sizeof(wait_buf)))
                goto write_fail;
            wait_count++;
            break;
        case MTFW_RAW_XFER:
            if(write_command(out, LOAD_COMMAND_RAW_XFER,
                             item->data, item->size))
                goto write_fail;
            raw_count++;
            payload_bytes += item->size;
            break;
        case MTFW_SEND_CALIBRATION:
            if(item->data && item->size == sizeof(mtfw_calibration_t)) {
                const mtfw_calibration_t *cal =
                    (const mtfw_calibration_t *)item->data;
                unsigned char cal_buf[12];

                put_le32(cal_buf, cal->address);
                put_le32(cal_buf + 4, cal->max_size);
                put_le32(cal_buf + 8, cal->provider);
                if(write_command(out, LOAD_COMMAND_SEND_CALIBRATION_V2,
                                 cal_buf, sizeof(cal_buf)))
                    goto write_fail;
                calibration_count++;
            } else {
                fprintf(stderr, "makez2fw: invalid calibration item\n");
                fclose(out);
                return 1;
            }
            break;
        case MTFW_SET_TYPE:
            skipped++;
            break;
        default:
            fprintf(stderr, "makez2fw: unsupported item type %u\n",
                    item->type);
            fclose(out);
            return 1;
        }
    }

    if(fclose(out)) {
        fprintf(stderr, "makez2fw: failed closing %s: %s\n",
                argv[4], strerror(errno));
        return 1;
    }

    fprintf(stderr,
            "makez2fw: wrote %s configs=%u init=%u blobs=%u raw=%u calibrations=%u waits=%u skipped=%u payload=%llu bytes\n",
            output, config_count, init_count, blob_count, raw_count,
            calibration_count, wait_count, skipped,
            (unsigned long long)payload_bytes);
    return 0;

write_fail:
    fprintf(stderr, "makez2fw: failed writing %s: %s\n",
            output, strerror(errno));
    fclose(out);
    return 1;
}
