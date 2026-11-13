// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Corellium LLC
 * Modified for HoolockLinux J172 HBPP14/Z2FW support in 2026.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "eplist.h"
#include "syscfg.h"
#include "mtfw.h"

#define GEN_1   1
#define GEN_2   2

static const struct {
    const char *provider;
    const char *syscfg;
} mtfw_providers[] = {
    { "multi-touch-calibration", "MtCl" },
    { "orb-gap-cal", "OrbG" },
    { "orb-force-cal", "OFCl" },
    { "shape-dynamic-accel-cal", "SDAC" },
    { "prox-calibration", "PxCl" },
    { "multi-touch-calibration", "MtCl" } };

static mtfw_item_t *mtfw_item_add(mtfw_item_t ***pptail, unsigned type, void *data, unsigned size, int copy)
{
    mtfw_item_t *item = calloc(1, sizeof(mtfw_item_t));
    if(!item)
        return NULL;
    item->type = type;
    if(copy) {
        item->data = malloc(size);
        if(!item->data) {
            free(item);
            return NULL;
        }
        if(data)
            memcpy(item->data, data, size);
        else
            memset(item->data, 0, size);
    } else
        item->data = data;
    item->size = size;
    **pptail = item;
    *pptail = &(item->next);
    return item;
}

static inline void mtfw_put16be(uint8_t *buf, uint16_t val)
{
    buf[0] = val >> 8;
    buf[1] = val;
}

static inline void mtfw_put32le(uint8_t *buf, uint32_t val)
{
    buf[0] = val;
    buf[1] = val >> 8;
    buf[2] = val >> 16;
    buf[3] = val >> 24;
}

static inline void mtfw_put32xe(uint8_t *buf, uint32_t val)
{
    buf[0] = val >> 8;
    buf[1] = val;
    buf[2] = val >> 24;
    buf[3] = val >> 16;
}

static uint32_t mtfw_sum(uint8_t *buf, unsigned size)
{
    uint32_t sum = 0;
    while(size --) {
        sum += *buf;
        buf ++;
    }
    return sum;
}

static mtfw_item_t *mtfw_item_add_regwr(mtfw_item_t ***pptail, uint32_t addr, uint32_t mask, uint32_t val)
{
    uint8_t buf[16];

    mtfw_put16be(&buf[0], 0x1E33);
    mtfw_put32xe(&buf[2], addr);
    mtfw_put32xe(&buf[6], mask);
    mtfw_put32xe(&buf[10], val);
    mtfw_put16be(&buf[14], mtfw_sum(&buf[2], 12));
    return mtfw_item_add(pptail, MTFW_WRITE_ACK, buf, sizeof(buf), 1);
}

static mtfw_item_t *mtfw_item_add_raw_xfer(mtfw_item_t ***pptail,
                                            const uint8_t *tx,
                                            unsigned tx_size,
                                            unsigned rx_size,
                                            unsigned flags)
{
    uint8_t buf[12 + 16];

    if(tx_size > 16 || rx_size > 16 || (!tx_size && !rx_size))
        return NULL;

    mtfw_put32le(&buf[0], tx_size);
    mtfw_put32le(&buf[4], rx_size);
    mtfw_put32le(&buf[8], flags);
    if(tx_size)
        memcpy(&buf[12], tx, tx_size);

    return mtfw_item_add(pptail, MTFW_RAW_XFER, buf, 12 + tx_size, 1);
}

static int mtfw_item_add_regread(mtfw_item_t ***pptail, uint32_t addr)
{
    static const uint8_t exchange[8] = {
        0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1
    };
    uint8_t request[8];

    mtfw_put16be(&request[0], 0x1c73);
    mtfw_put32xe(&request[2], addr);
    mtfw_put16be(&request[6], mtfw_sum(&request[2], 4));

    return mtfw_item_add_raw_xfer(pptail, request, sizeof(request), 0,
                                  MTFW_RAW_XFER_RX_BIT_REVERSE) &&
           mtfw_item_add_raw_xfer(pptail, exchange, sizeof(exchange),
                                  sizeof(exchange),
                                  MTFW_RAW_XFER_RX_BIT_REVERSE);
}

static int mtfw_item_add_otp_preflight(mtfw_item_t ***pptail,
                                       const mtfw_config_t *cfg)
{
    uint32_t addr, index;
    unsigned words, i;

    if(!cfg ||
       (cfg->valid_mask & (MTFW_CONFIG_OTP_ADDRESS_VALID |
                           MTFW_CONFIG_OTP_SN_VALID)) !=
       (MTFW_CONFIG_OTP_ADDRESS_VALID | MTFW_CONFIG_OTP_SN_VALID))
        return 1;

    addr = cfg->otp_address;
    index = cfg->otp_sn[0];
    if(!addr || !cfg->otp_sn[1] || (cfg->otp_sn[1] & 3) ||
       cfg->otp_sn[1] > 256) {
        fprintf(stderr,
                "HBPP OTP: invalid address/selection 0x%x %u/%u.\n",
                addr, index, cfg->otp_sn[1]);
        return 0;
    }
    words = cfg->otp_sn[1] >> 2;

    if(!mtfw_item_add_regwr(pptail, addr + 0x00, -1u, 2) ||
       !mtfw_item_add_regwr(pptail, addr + 0x04, -1u, 0) ||
       !mtfw_item_add_regwr(pptail, addr + 0x08, -1u, 0))
        return 0;

    for(i=0; i<words; i++) {
        if(!mtfw_item_add_regwr(pptail, addr + 0x10, -1u, -1u) ||
           !mtfw_item_add_regwr(pptail, addr + 0x0c, -1u, index + i) ||
           !mtfw_item_add_regwr(pptail, addr + 0x04, -1u, 0x00e80000) ||
           !mtfw_item_add_regwr(pptail, addr + 0x04, -1u, 0x00e80001) ||
           !mtfw_item_add_regread(pptail, addr + 0x20) ||
           !mtfw_item_add_regwr(pptail, addr + 0x20, -1u, 1) ||
           !mtfw_item_add_regread(pptail, addr + 0x18))
            return 0;
    }

    fprintf(stderr,
            "HBPP OTP: preflight address=0x%x start=%u size=%u (%u RMWs).\n",
            addr, index, cfg->otp_sn[1], 3 + words * 5);
    return 1;
}

static void mtfw_copy16be(uint8_t *dst, uint8_t *src, unsigned len)
{
    unsigned i;
    for(i=0; i<len; i++)
        dst[i^1] = src[i];
}

/* Takes ownership of data and releases it on every return path. */
static mtfw_item_t *mtfw_item_add_calload(mtfw_item_t ***pptail, uint32_t addr,
                                        void *data, unsigned len,
                                        bool zero_based_words)
{
    mtfw_item_t *mtfw = mtfw_item_add(pptail, MTFW_WRITE_ACK, NULL, 16 + ((len + 3) & -4), 1);
    uint8_t *buf;

    if(!mtfw) {
        free(data);
        return NULL;
    }
    buf = (uint8_t *)mtfw->data;

    mtfw_put32xe(&buf[0], 0x300118E1);
    /*
     * The matched AppleHIDTransportSPI HBPP14 constructor adds its
     * protocol's -1 bias to the rounded-up payload word count.  The legacy
     * AppleMultitouchSPI constructor has different semantics; preserve
     * existing non-J171 callers rather than inferring their protocol here.
     */
    mtfw_put16be(&buf[4], ((len + 3) >> 2) - zero_based_words);
    mtfw_put32xe(&buf[6], addr);
    mtfw_put16be(&buf[10], mtfw_sum(&buf[4], 6));
    mtfw_copy16be(&buf[12], data, len);
    mtfw_put32xe(&buf[12 + ((len + 3) & -4)], mtfw_sum(data, len));
    free(data);

    return mtfw;
}

static void *mtfw_request_cal(const char *syscfg, const char *name, unsigned long *len)
{
    unsigned i;
    for(i=0; i<sizeof(mtfw_providers)/sizeof(mtfw_providers[0]); i++)
        if(!strcmp(mtfw_providers[i].provider, name))
            return syscfg_get(syscfg, mtfw_providers[i].syscfg, len);
    return NULL;
}

static int mtfw_dynamic_calibration_provider(const char *name, unsigned *provider)
{
    static const struct {
        const char *name;
        unsigned provider;
    } providers[] = {
        { "multi-touch-calibration", MTFW_CAL_MULTI_TOUCH },
        { "orb-gap-cal", MTFW_CAL_ORB_GAP },
        { "orb-force-cal", MTFW_CAL_ORB_FORCE },
        { "shape-dynamic-accel-cal", MTFW_CAL_SHAPE_ACCEL },
    };
    unsigned i;

    for(i=0; i<sizeof(providers)/sizeof(providers[0]); i++) {
        if(!strcmp(providers[i].name, name)) {
            *provider = providers[i].provider;
            return 1;
        }
    }

    return 0;
}

static int mtfw_item_add_payload(mtfw_item_t ***pptail, uint32_t addr,
                                 void *data, unsigned long len,
                                 bool j171_packetization)
{
    unsigned char *bytes = data;
    unsigned long offset = 0;

    if(!data || !len) {
        free(data);
        fprintf(stderr, "HBPP payload at 0x%x is empty.\n", addr);
        return 0;
    }

    if(j171_packetization) {
        /*
         * Keep J171 DATA packets within the existing 0x3f0 payload limit.
         * With the 12-byte header and four-byte payload
         * checksum this keeps every SPI transaction at or below 0x400 bytes.
         * Each packet is followed by its own ATN_ACK (0x4bc1).  Treating a
         * complete image/property as one packet overflows the 16-bit word
         * count for the main image and leaves the controller in HBPP mode.
         */
        while(offset < len) {
            unsigned long chunk = len - offset;
            unsigned char *copy;

            if(chunk > 0x3f0)
                chunk = 0x3f0;
            copy = malloc(chunk);
            if(!copy) {
                free(data);
                return 0;
            }
            memcpy(copy, bytes + offset, chunk);
            if(!mtfw_item_add_calload(pptail, addr + offset, copy, chunk, true)) {
                free(data);
                return 0;
            }
            offset += chunk;
        }
        free(data);
    } else if(!mtfw_item_add_calload(pptail, addr, data, len, false)) {
        return 0;
    }

    fprintf(stderr, "HBPP payload: load %lu bytes at 0x%x%s.\n", len, addr,
            j171_packetization ? " in <=0x3f0-byte J171 packets" : "");
    return 1;
}

static const char *mtfw_mem_find(const char *buf, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    size_t i;

    if(!nlen || nlen > len)
        return NULL;

    for(i=0; i<=len-nlen; i++)
        if(!memcmp(buf + i, needle, nlen))
            return buf + i;

    return NULL;
}

static const char *mtfw_find_key_value(const char *buf, size_t len,
                                       const char *key, size_t *out_len)
{
    char pattern[128];
    const char *pos;
    size_t plen;

    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);
    plen = strlen(pattern);
    pos = mtfw_mem_find(buf, len, pattern);
    if(!pos)
        return NULL;

    pos += plen;
    *out_len = len - (pos - buf);
    {
        const char *next_key = mtfw_mem_find(pos, *out_len, "<key>");

        if(next_key)
            *out_len = next_key - pos;
    }
    return pos;
}

static const char *mtfw_find_dict_end(const char *dict, size_t len)
{
    const char *pos = dict;
    const char *end = dict + len;
    int depth = 0;

    while(pos < end) {
        const char *open = mtfw_mem_find(pos, end - pos, "<dict");
        const char *close = mtfw_mem_find(pos, end - pos, "</dict>");
        const char *tag_end;

        if(!close)
            return NULL;

        if(open && open < close) {
            tag_end = memchr(open, '>', end - open);
            if(!tag_end)
                return NULL;
            depth++;
            pos = tag_end + 1;
            continue;
        }

        depth--;
        pos = close + strlen("</dict>");
        if(depth == 0)
            return pos;
        if(depth < 0)
            return NULL;
    }

    return NULL;
}

static int mtfw_find_raw_config_block(const char *buf, size_t len,
                                      const char **out, size_t *out_len)
{
    const char *pos = buf;
    const char *end = buf + len;

    while(pos < end) {
        const char *key = mtfw_mem_find(pos, end - pos, "<key>Config</key>");
        const char *dict, *dict_end;

        if(!key)
            return 0;

        dict = mtfw_mem_find(key, end - key, "<dict>");
        if(!dict)
            return 0;

        dict_end = mtfw_find_dict_end(dict, end - dict);
        if(!dict_end)
            return 0;

        if(mtfw_mem_find(dict, dict_end - dict, "Min DMA Transfer Size") &&
           mtfw_mem_find(dict, dict_end - dict, "Device Tree Overrides")) {
            *out = dict;
            *out_len = dict_end - dict;
            return 1;
        }

        pos = dict_end;
    }

    return 0;
}

static unsigned long long mtfw_parse_ull(const char *buf, size_t len)
{
    char tmp[64];
    size_t n = len < sizeof(tmp)-1 ? len : sizeof(tmp)-1;

    memcpy(tmp, buf, n);
    tmp[n] = 0;
    return strtoull(tmp, NULL, 0);
}

static int mtfw_parse_integer_element(const char *full, size_t full_len,
                                      const char *elem, size_t elem_len,
                                      unsigned *dst)
{
    const char *integer = mtfw_mem_find(elem, elem_len, "<integer");
    const char *tag_end, *text_end, *idref;
    size_t tag_len;

    if(!integer)
        return 0;

    tag_end = memchr(integer, '>', elem_len - (integer - elem));
    if(!tag_end)
        return 0;

    tag_len = tag_end + 1 - integer;
    idref = mtfw_mem_find(integer, tag_len, "IDREF=\"");
    if(idref) {
        char pattern[64];
        const char *id = idref + strlen("IDREF=\"");
        const char *id_end = memchr(id, '"', tag_end - id);
        const char *target;
        size_t id_len;

        if(!id_end)
            return 0;
        id_len = id_end - id;
        if(id_len >= 24)
            return 0;
        snprintf(pattern, sizeof(pattern), "<integer ID=\"%.*s\"", (int)id_len, id);
        target = mtfw_mem_find(full, full_len, pattern);
        if(!target)
            return 0;
        return mtfw_parse_integer_element(full, full_len, target,
                                          full_len - (target - full), dst);
    }

    if(tag_end > integer && tag_end[-1] == '/')
        return 0;

    text_end = mtfw_mem_find(tag_end + 1,
                             elem_len - (tag_end + 1 - elem),
                             "</integer>");
    if(!text_end)
        return 0;

    *dst = mtfw_parse_ull(tag_end + 1, text_end - (tag_end + 1));
    return 1;
}

static void mtfw_raw_get_uint(const char *full, size_t full_len,
                              const char *block, size_t block_len,
                              mtfw_config_t *cfg, const char *key,
                              unsigned valid_bit, unsigned *dst)
{
    const char *elem;
    size_t elem_len;

    elem = mtfw_find_key_value(block, block_len, key, &elem_len);
    if(!elem)
        return;

    if(mtfw_parse_integer_element(full, full_len, elem, elem_len, dst))
        cfg->valid_mask |= valid_bit;
}

static void mtfw_raw_get_bool(const char *block, size_t block_len,
                              mtfw_config_t *cfg, const char *key,
                              unsigned valid_bit, unsigned *dst)
{
    const char *elem;
    size_t elem_len;

    elem = mtfw_find_key_value(block, block_len, key, &elem_len);
    if(!elem)
        return;

    if(mtfw_mem_find(elem, elem_len, "<true/>")) {
        *dst = 1;
        cfg->valid_mask |= valid_bit;
    } else if(mtfw_mem_find(elem, elem_len, "<false/>")) {
        *dst = 0;
        cfg->valid_mask |= valid_bit;
    }
}

static int mtfw_b64_value(unsigned char ch)
{
    if(ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if(ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if(ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if(ch == '+')
        return 62;
    if(ch == '/')
        return 63;
    return -1;
}

static unsigned mtfw_b64_decode(const char *src, size_t src_len,
                                unsigned char *dst, unsigned dst_len)
{
    unsigned acc = 0, bits = 0, out = 0;
    size_t i;

    for(i=0; i<src_len; i++) {
        int val;
        unsigned char ch = src[i];

        if(ch == '=')
            break;
        val = mtfw_b64_value(ch);
        if(val < 0)
            continue;

        acc = (acc << 6) | val;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            if(out >= dst_len)
                break;
            dst[out++] = (acc >> bits) & 0xff;
        }
    }

    return out;
}

static unsigned mtfw_raw_get_data(const char *block, size_t block_len,
                                  const char *key, unsigned char *dst,
                                  unsigned dst_len)
{
    const char *elem, *data, *tag_end, *data_end;
    size_t elem_len;

    elem = mtfw_find_key_value(block, block_len, key, &elem_len);
    if(!elem)
        return 0;

    data = mtfw_mem_find(elem, elem_len, "<data");
    if(!data)
        return 0;

    tag_end = memchr(data, '>', elem_len - (data - elem));
    if(!tag_end)
        return 0;

    data_end = mtfw_mem_find(tag_end + 1,
                             elem_len - (tag_end + 1 - elem), "</data>");
    if(!data_end)
        return 0;

    return mtfw_b64_decode(tag_end + 1, data_end - (tag_end + 1), dst, dst_len);
}

static unsigned mtfw_get_le32(const unsigned char *buf)
{
    return ((unsigned)buf[0]) | ((unsigned)buf[1] << 8) |
           ((unsigned)buf[2] << 16) | ((unsigned)buf[3] << 24);
}

static void mtfw_raw_get_u32_data(const char *block, size_t block_len,
                                  mtfw_config_t *cfg, const char *key,
                                  unsigned valid_bit, unsigned *dst)
{
    unsigned char tmp[8];
    unsigned len;

    len = mtfw_raw_get_data(block, block_len, key, tmp, sizeof(tmp));
    if(len < 4)
        return;

    *dst = mtfw_get_le32(tmp);
    cfg->valid_mask |= valid_bit;
}

static void mtfw_raw_get_otp_sn(const char *block, size_t block_len,
                                mtfw_config_t *cfg)
{
    unsigned char tmp[8];
    unsigned len;

    len = mtfw_raw_get_data(block, block_len, "otp-sn", tmp, sizeof(tmp));
    if(len < 8)
        return;

    cfg->otp_sn[0] = mtfw_get_le32(tmp);
    cfg->otp_sn[1] = mtfw_get_le32(tmp + 4);
    cfg->valid_mask |= MTFW_CONFIG_OTP_SN_VALID;
}

static void mtfw_raw_get_sequence(const char *block, size_t block_len,
                                  mtfw_config_t *cfg, const char *key,
                                  unsigned valid_bit, unsigned char *dst,
                                  unsigned *dst_len)
{
    unsigned len;

    len = mtfw_raw_get_data(block, block_len, key, dst, MTFW_CONFIG_SEQ_MAX);
    if(!len)
        return;

    *dst_len = len;
    cfg->valid_mask |= valid_bit;
}

static void mtfw_dump_raw_bytes(const char *name, const unsigned char *buf,
                                unsigned len)
{
    unsigned i;

    fprintf(stderr, "HBPP raw config: %s[%u]=", name, len);
    for(i=0; i<len; i++)
        fprintf(stderr, "%s%02x", i ? " " : "", buf[i]);
    fprintf(stderr, "\n");
}

static int mtfw_parse_raw_hbpp_config(const char *fname, mtfw_config_t *cfg)
{
    FILE *f;
    char *buf;
    long sz;
    const char *block;
    size_t block_len;
    int ret = 0;

    memset(cfg, 0, sizeof(*cfg));

    f = fopen(fname, "rb");
    if(!f)
        return 0;
    if(fseek(f, 0, SEEK_END)) {
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if(sz <= 0) {
        fclose(f);
        return 0;
    }
    rewind(f);

    buf = malloc(sz + 1);
    if(!buf) {
        fclose(f);
        return 0;
    }
    if(fread(buf, 1, sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[sz] = 0;

    if(!mtfw_find_raw_config_block(buf, sz, &block, &block_len))
        goto out;

    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Min DMA Transfer Size", MTFW_CONFIG_MIN_DMA_VALID,
                      &cfg->min_dma_transfer_size);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Inter Packet Delay", MTFW_CONFIG_Z2_DELAY_VALID,
                      &cfg->z2_inter_packet_delay_us);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "CS Delay", MTFW_CONFIG_CS_DELAY_VALID,
                      &cfg->cs_delay_us);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Clock Phase", MTFW_CONFIG_CPHA_VALID,
                      &cfg->clock_phase);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Clock Polarity", MTFW_CONFIG_CPOL_VALID,
                      &cfg->clock_polarity);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Word Delay", MTFW_CONFIG_WORD_DELAY_VALID,
                      &cfg->word_delay);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Clock Period Ms", MTFW_CONFIG_CLOCK_PERIOD_VALID,
                      &cfg->clock_period_ms);
    mtfw_raw_get_bool(block, block_len, cfg, "Power Off On Reset",
                      MTFW_CONFIG_POWER_RESET_VALID,
                      &cfg->power_off_on_reset);
    mtfw_raw_get_uint(buf, sz, block, block_len, cfg,
                      "Normal Boot Ms", MTFW_CONFIG_BOOT_TIMEOUT_VALID,
                      &cfg->normal_boot_ms);

    mtfw_raw_get_u32_data(block, block_len, cfg, "otp-address",
                          MTFW_CONFIG_OTP_ADDRESS_VALID, &cfg->otp_address);
    mtfw_raw_get_u32_data(block, block_len, cfg, "chip-id-address",
                          MTFW_CONFIG_CHIP_ID_VALID,
                          &cfg->chip_id_address);
    mtfw_raw_get_u32_data(block, block_len, cfg, "hbpp-version",
                          MTFW_CONFIG_HBPP_VERSION_VALID,
                          &cfg->hbpp_version);
    mtfw_raw_get_otp_sn(block, block_len, cfg);
    mtfw_raw_get_sequence(block, block_len, cfg, "reset-sequence",
                          MTFW_CONFIG_RESET_SEQ_VALID, cfg->reset_sequence,
                          &cfg->reset_sequence_len);
    mtfw_raw_get_sequence(block, block_len, cfg, "power-sequence",
                          MTFW_CONFIG_POWER_SEQ_VALID, cfg->power_sequence,
                          &cfg->power_sequence_len);

    if(cfg->valid_mask) {
        fprintf(stderr,
                "HBPP raw config: valid=0x%x min_dma=%u z2_delay=%u cs_delay=%u cpha=%u cpol=%u word_delay=%u clock_period=%u power_off_reset=%u boot_timeout=%u otp=0x%x chip=0x%x hbpp=0x%x otp_sn=%u/%u.\n",
                cfg->valid_mask, cfg->min_dma_transfer_size,
                cfg->z2_inter_packet_delay_us, cfg->cs_delay_us,
                cfg->clock_phase, cfg->clock_polarity, cfg->word_delay,
                cfg->clock_period_ms, cfg->power_off_on_reset,
                cfg->normal_boot_ms, cfg->otp_address,
                cfg->chip_id_address, cfg->hbpp_version, cfg->otp_sn[0],
                cfg->otp_sn[1]);
        if(cfg->valid_mask & MTFW_CONFIG_RESET_SEQ_VALID)
            mtfw_dump_raw_bytes("reset-sequence", cfg->reset_sequence,
                                cfg->reset_sequence_len);
        if(cfg->valid_mask & MTFW_CONFIG_POWER_SEQ_VALID)
            mtfw_dump_raw_bytes("power-sequence", cfg->power_sequence,
                                cfg->power_sequence_len);
        ret = 1;
    }

out:
    free(buf);
    return ret;
}

static void mtfw_config_get_uint(mtfw_config_t *cfg, epelem_t dict,
                                 const char *key, unsigned valid_bit,
                                 unsigned *dst)
{
    epelem_t elem;
    const char *string;
    char *end;
    unsigned long value;

    if(!dict)
        return;

    elem = eplist_dict_find(dict, key, EPLIST_INTEGER);
    if(elem) {
        *dst = eplist_get_integer(elem);
        cfg->valid_mask |= valid_bit;
        return;
    }

    elem = eplist_dict_find(dict, key, EPLIST_STRING);
    if(!elem)
        return;
    string = eplist_get_string(elem);
    if(!string || !*string)
        return;
    value = strtoul(string, &end, 0);
    if(*end || value > UINT32_MAX)
        return;

    *dst = value;
    cfg->valid_mask |= valid_bit;
}

static void mtfw_config_get_bool(mtfw_config_t *cfg, epelem_t dict,
                                 const char *key, unsigned valid_bit,
                                 unsigned *dst)
{
    epelem_t elem;
    int val;

    if(!dict)
        return;

    elem = eplist_dict_find(dict, key, EPLIST_BOOL);
    if(!elem)
        return;

    val = eplist_get_bool(elem);
    if(val < 0)
        return;

    *dst = val;
    cfg->valid_mask |= valid_bit;
}

static void mtfw_parse_hbpp_config(epelem_t root, mtfw_config_t *cfg)
{
    epelem_t config, z2_config, spi_config, boot_timeout;

    memset(cfg, 0, sizeof(*cfg));

    config = eplist_dict_find(root, "Config", EPLIST_DICT);
    if(!config)
        config = root;

    mtfw_config_get_uint(cfg, config, "Min DMA Transfer Size",
                         MTFW_CONFIG_MIN_DMA_VALID,
                         &cfg->min_dma_transfer_size);
    mtfw_config_get_bool(cfg, config, "Power Off On Reset",
                         MTFW_CONFIG_POWER_RESET_VALID,
                         &cfg->power_off_on_reset);

    z2_config = eplist_dict_find(config, "Z2 Config", EPLIST_DICT);
    mtfw_config_get_uint(cfg, z2_config, "Inter Packet Delay",
                         MTFW_CONFIG_Z2_DELAY_VALID,
                         &cfg->z2_inter_packet_delay_us);

    spi_config = eplist_dict_find(config, "SPI Config", EPLIST_DICT);
    mtfw_config_get_uint(cfg, spi_config, "Clock Phase",
                         MTFW_CONFIG_CPHA_VALID, &cfg->clock_phase);
    mtfw_config_get_uint(cfg, spi_config, "Clock Polarity",
                         MTFW_CONFIG_CPOL_VALID, &cfg->clock_polarity);
    mtfw_config_get_uint(cfg, spi_config, "Word Delay",
                         MTFW_CONFIG_WORD_DELAY_VALID, &cfg->word_delay);
    mtfw_config_get_uint(cfg, spi_config, "CS Delay",
                         MTFW_CONFIG_CS_DELAY_VALID, &cfg->cs_delay_us);
    mtfw_config_get_uint(cfg, spi_config, "Clock Period Ms",
                         MTFW_CONFIG_CLOCK_PERIOD_VALID,
                         &cfg->clock_period_ms);

    boot_timeout = eplist_dict_find(config, "Boot Timeout", EPLIST_DICT);
    mtfw_config_get_uint(cfg, boot_timeout, "Normal Boot Ms",
                         MTFW_CONFIG_BOOT_TIMEOUT_VALID,
                         &cfg->normal_boot_ms);

    if(cfg->valid_mask) {
        fprintf(stderr,
                "HBPP config: valid=0x%x min_dma=%u z2_delay=%u cs_delay=%u cpha=%u cpol=%u word_delay=%u clock_period=%u power_off_reset=%u boot_timeout=%u.\n",
                cfg->valid_mask, cfg->min_dma_transfer_size,
                cfg->z2_inter_packet_delay_us, cfg->cs_delay_us,
                cfg->clock_phase, cfg->clock_polarity, cfg->word_delay,
                cfg->clock_period_ms, cfg->power_off_on_reset,
                cfg->normal_boot_ms);
    }
}

static mtfw_item_t *mtfw_load_hbpp_firmware(epelem_t seq, const char *syscfg,
                                            const mtfw_config_t *cfg,
                                            const mtfw_load_options_t *options)
{
    mtfw_item_t *head = NULL, **ptail = &head;
    epelem_t seql, elem;
    void *bits;
    unsigned long len;
    unsigned long long addr, mask, val;
    const char *type, *desc, *syscfg_key;
    int mode = GEN_2;

    if(!mtfw_item_add(&ptail, MTFW_SET_TYPE, &mode, 4, 1))
        goto fail;

    if(cfg && cfg->valid_mask)
        if(!mtfw_item_add(&ptail, MTFW_SET_CONFIG, (void *)cfg, sizeof(*cfg), 1))
            goto fail;

    /* AppleHIDTransportBootloaderHBPP checks HBPP14 with four bytes. */
    if(!mtfw_item_add(&ptail, MTFW_WRITE,
                     "\x1A\xA1\x18\xE1", 4, 1)) {
        goto fail;
    }

    if(options->otp_preflight)
        if(!mtfw_item_add_otp_preflight(&ptail, cfg))
            goto fail;

    seql = eplist_array_first(seq);
    while(seql) {
        if(eplist_type(seql) != EPLIST_DICT) {
            fprintf(stderr, "Non-dictionary item in HBPP firmware array.\n");
            goto fail;
        }

        type = eplist_get_string(eplist_dict_find(seql, "Type", EPLIST_STRING));
        desc = eplist_get_string(eplist_dict_find(seql, "Description", EPLIST_STRING));
        if(!type) {
            fprintf(stderr, "HBPP item has no type (%s).\n", desc ? desc : "no description");
            goto fail;
        }

        if(!strcmp(type, "Config")) {
            seql = eplist_next(seql);
            continue;
        }

        elem = eplist_dict_find(seql, "Address", EPLIST_INTEGER);
        if(!elem) {
            fprintf(stderr, "HBPP item has no address (%s).\n", desc ? desc : type);
            goto fail;
        }
        addr = eplist_get_integer(elem);

        if(!strcmp(type, "ReadModifyWrite")) {
            mask = eplist_get_integer(eplist_dict_find(seql, "Mask", EPLIST_INTEGER));
            val = eplist_get_integer(eplist_dict_find(seql, "Value", EPLIST_INTEGER));
            fprintf(stderr, "HBPP RMW: %s addr=0x%llx mask=0x%llx val=0x%llx.\n",
                    desc ? desc : "(unnamed)", addr, mask, val);
            if(!mtfw_item_add_regwr(&ptail, addr, mask, val))
                goto fail;
        } else if(!strcmp(type, "Binary") || !strcmp(type, "Property")) {
            bits = NULL;
            len = 0;
            syscfg_key = eplist_get_string(eplist_dict_find(seql, "SysConfigKey", EPLIST_STRING));
            if(syscfg_key && *syscfg_key) {
                bits = syscfg_get(syscfg, syscfg_key, &len);
                if(bits)
                    fprintf(stderr, "HBPP property: %s from SysCfg %s (%lu bytes).\n",
                            desc ? desc : "(unnamed)", syscfg_key, len);
            }
            if(!bits) {
                elem = eplist_dict_find(seql, "Payload", EPLIST_DATA);
                bits = eplist_get_data(elem, &len);
                if(bits)
                    fprintf(stderr, "HBPP payload: %s from MTFW (%lu bytes).\n",
                            desc ? desc : "(unnamed)", len);
            }
            if(!mtfw_item_add_payload(&ptail, addr, bits, len,
                                      options->j171_packetization))
                goto fail;
        } else {
            fprintf(stderr, "Unsupported HBPP item type '%s' (%s).\n",
                    type, desc ? desc : "no description");
            goto fail;
        }

        seql = eplist_next(seql);
    }

    if(!mtfw_item_add(&ptail, MTFW_WAIT_IRQ, NULL, 0, 0))
        goto fail;

    return head;

fail:
    return NULL;
}

mtfw_item_t *mtfw_load_firmware(const char *pers, const char *fname,
                                const char *syscfg,
                                const mtfw_load_options_t *options)
{
    static const mtfw_load_options_t default_options;
    mtfw_item_t *head = NULL, **ptail = &head;
    FILE *f;
    eplist_t epl = NULL;
    epelem_t root, fw, fwcfg, seq, seql, act;
    mtfw_config_t raw_config;
    void *bits, *fwcfgbits = NULL;
    unsigned long len;
    unsigned long long addr, mask, val, max_size;
    const char *acts;
    int mode, i;

    if(!options)
        options = &default_options;

    mtfw_parse_raw_hbpp_config(fname, &raw_config);

    f = fopen(fname, "r");
    if(!f) {
        fprintf(stderr, "Failed to open input file.\n");
        goto fail;
    }
    epl = eplist_load(EPLIST_LOAD_FILE, f);
    fclose(f);

    if(!epl) {
        fprintf(stderr, "Failed to load input file.\n");
        goto fail;
    }

    root = eplist_root(epl);
    fw = eplist_dict_find(root, pers, EPLIST_DICT);
    if(!fw) {
        fw = eplist_dict_find(root, pers, EPLIST_ARRAY);
        if(fw) {
            mtfw_config_t config = raw_config;

            if(!config.valid_mask)
                mtfw_parse_hbpp_config(root, &config);
            head = mtfw_load_hbpp_firmware(fw, syscfg, &config, options);
            eplist_free(epl);
            return head;
        } else {
            fprintf(stderr, "Firmware for the specified personality (%s) not found.\n", pers);
            goto fail;
        }
    }

    seq = eplist_dict_find(fw, "Constructed Firmware", EPLIST_ARRAY);
    if(!seq) {
        seq = eplist_dict_find(fw, "Constructed Firmware", EPLIST_DATA);
        if(!seq) {
            fprintf(stderr, "Firmware does not contain preconstructed blobs.\n");
            goto fail;
        }
        mode = GEN_1;
    } else
        mode = GEN_2;

    if(!mtfw_item_add(&ptail, MTFW_SET_TYPE, &mode, 4, 1))
        goto fail;

    switch(mode) {
    case GEN_1:

        if(!mtfw_item_add(&ptail, MTFW_WRITE, "\x19\xC1", 2, 1))
            goto fail;
        for(i=0; i<3; i++)
            if(!mtfw_item_add(&ptail, MTFW_WRITE, "\x1A\xA1\x18\xE1\x18\xE1\x18\xE1\x18\xE1\x18\xE1\x18\xE1\x18\xE1", 16, 1))
                goto fail;

        bits = mtfw_request_cal(syscfg, "prox-calibration", &len);
        if(bits)
            if(!mtfw_item_add_calload(&ptail, 0x10009600, bits, len, false))
                goto fail;

        bits = mtfw_request_cal(syscfg, "multi-touch-calibration", &len);
        if(!bits) {
            fprintf(stderr, "Calibration sequence provider unavailable (%s).\n", "multi-touc-calibration");
            goto fail;
        }
        if(!mtfw_item_add_calload(&ptail, 0x10009000, bits, len, false))
            goto fail;

        bits = eplist_get_data(seq, &len);
        if(!bits) {
            fprintf(stderr, "Preconstructed blob item did not decode correctly.\n");
            goto fail;
        }
        if(!mtfw_item_add(&ptail, MTFW_WRITE_ACK, bits, len, 0)) {
            free(bits);
            goto fail;
        }

        if(!mtfw_item_add_regwr(&ptail, 0x10003060, -1u, 6099))
            goto fail;
        if(!mtfw_item_add_regwr(&ptail, 0x1000305c, -1u, 2))
            goto fail;
        if(!mtfw_item_add_regwr(&ptail, 0x10003058, -1u, 6))
            goto fail;
        if(!mtfw_item_add_regwr(&ptail, 0x10003000, -1u, 3))
            goto fail;
        if(!mtfw_item_add_regwr(&ptail, 0x10003518, -1u, 1))
            goto fail;

        if(!mtfw_item_add(&ptail, MTFW_WRITE_ACK, "\x1F\x01", 2, 1))
            goto fail;
        if(!mtfw_item_add(&ptail, MTFW_WRITE, "\x1D\x53\x34\x00\x10\x00\x00\x01\x00\x00\x00\x45", 12, 1))
            goto fail;

        break;

    case GEN_2:
        if(!mtfw_item_add(&ptail, MTFW_WRITE, "\x1A\xA1\x18\xE1", 4, 1))
            goto fail;

        seql = eplist_array_first(seq);
        while(seql) {
            if(eplist_type(seql) != EPLIST_DATA) {
                fprintf(stderr, "Non-data item in preconstructed blob array.\n");
                goto fail;
            }
            bits = eplist_get_data(seql, &len);
            if(!bits) {
                fprintf(stderr, "Preconstructed blob item did not decode correctly.\n");
                goto fail;
            }
            if(!mtfw_item_add(&ptail, MTFW_WRITE_ACK, bits, len, 0)) {
                free(bits);
                goto fail;
            }
            seql = eplist_next(seql);
        }

        fwcfg = eplist_dict_find(fw, "Firmware Config", EPLIST_DATA);
        if(!fwcfg) {
            fprintf(stderr, "Firmware does not contain configuration blob.\n");
            goto fail;
        }

        fwcfgbits = eplist_get_data(fwcfg, &len);
        if(!fwcfgbits) {
            fprintf(stderr, "Configuration blob did not decode correctly.\n");
            goto fail;
        }

        eplist_free(epl);

        epl = eplist_load(EPLIST_LOAD_STRING, fwcfgbits);
        if(!epl) {
            fprintf(stderr, "Failed to load configuration blob.\n");
            goto fail;
        }

        root = eplist_root(epl);

        if(!raw_config.valid_mask)
            mtfw_parse_hbpp_config(root, &raw_config);
        if(raw_config.valid_mask &&
           !mtfw_item_add(&ptail, MTFW_SET_CONFIG, &raw_config,
                          sizeof(raw_config), 1))
            goto fail;

        seq = eplist_dict_find(root, "Calibration Sequence", EPLIST_ARRAY);
        if(!seq) {
            fprintf(stderr, "Failed to find calibration sequence.\n");
            goto fail;
        }

        seql = eplist_array_first(seq);
        while(seql) {
            if(eplist_type(seql) != EPLIST_DICT) {
                fprintf(stderr, "Non-dictionary item in calibration sequence array.\n");
                goto fail;
            }
            fw = eplist_dict_find(seql, "Address", EPLIST_INTEGER);
            if(!fw) {
                fprintf(stderr, "Incomplete item in calibration sequence array (no address).\n");
                goto fail;
            }
            addr = eplist_get_integer(fw);
            acts = eplist_get_string(eplist_dict_find(seql, "Provider", EPLIST_STRING));
            if(!acts) {
                fprintf(stderr, "Incomplete item in calibration sequence array (no provider).\n");
                goto fail;
            }
            if(options->dynamic_calibration) {
                mtfw_calibration_t calibration;

                fw = eplist_dict_find(seql, "Max Size", EPLIST_INTEGER);
                if(!fw) {
                    fprintf(stderr,
                            "Dynamic calibration has no maximum size (%s).\n",
                            acts);
                    goto fail;
                }
                max_size = eplist_get_integer(fw);
                if(!addr || addr > UINT32_MAX || !max_size ||
                   max_size > UINT32_MAX ||
                   !mtfw_dynamic_calibration_provider(acts,
                                                      &calibration.provider)) {
                    fprintf(stderr,
                            "Unsupported dynamic calibration (%s, address 0x%llx, max %llu).\n",
                            acts, addr, max_size);
                    goto fail;
                }
                calibration.address = addr;
                calibration.max_size = max_size;
                if(!mtfw_item_add(&ptail, MTFW_SEND_CALIBRATION,
                                  &calibration, sizeof(calibration), 1))
                    goto fail;
                fprintf(stderr,
                        "Dynamic calibration: %s provider=%u address=0x%x max=%u.\n",
                        acts, calibration.provider, calibration.address,
                        calibration.max_size);
            } else {
                bits = mtfw_request_cal(syscfg, acts, &len);
                if(!bits) {
                    fprintf(stderr,
                            "Calibration sequence provider unavailable (%s).\n",
                            acts);
                    goto fail;
                }
                if(!mtfw_item_add_calload(&ptail, addr, bits, len, false))
                    goto fail;
            }
            seql = eplist_next(seql);
        }

        seq = eplist_dict_find(root, "Boot Sequence", EPLIST_ARRAY);
        if(!seq) {
            fprintf(stderr, "Failed to find boot sequence.\n");
            goto fail;
        }

        seql = eplist_array_first(seq);
        while(seql) {
            if(eplist_type(seql) != EPLIST_DICT) {
                fprintf(stderr, "Non-dictionary item in boot sequence array.\n");
                goto fail;
            }
            act = eplist_dict_find(seql, "Action", EPLIST_STRING);
            acts = eplist_get_string(act);
            if(acts) {
                if(!strcmp(acts, "RequestCalibration")) {
                    if(!mtfw_item_add(&ptail, MTFW_WRITE_ACK, "\x1F\x01", 2, 1))
                        goto fail;
                } else {
                    fprintf(stderr, "Unexpected action item (%s) in boot sequence array.\n", acts);
                    goto fail;
                }
            } else {
                fw = eplist_dict_find(seql, "Address", EPLIST_INTEGER);
                if(!fw) {
                    fprintf(stderr, "Unexpected non-action item in boot sequence array.\n");
                    goto fail;
                }
                addr = eplist_get_integer(fw);
                mask = eplist_get_integer(eplist_dict_find(seql, "Mask", EPLIST_INTEGER));
                val = eplist_get_integer(eplist_dict_find(seql, "Value", EPLIST_INTEGER));
                if(!mtfw_item_add_regwr(&ptail, addr, mask, val))
                    goto fail;
            }
            seql = eplist_next(seql);
        }

        if(!mtfw_item_add(&ptail, MTFW_WAIT_IRQ, NULL, 0, 0))
            goto fail;

        break;
    }

    eplist_free(epl);
    free(fwcfgbits);

    return head;

fail:
    eplist_free(epl);
    free(fwcfgbits);
    return NULL;
}
