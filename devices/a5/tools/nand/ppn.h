/* ppn.h -- the A5's NAND (two Hynix PPN packages on two H2FMI buses), driven
 * from userspace through /dev/mem, by PIO.  Read side only: see ppn.c for the
 * opcode allowlist and where every sequence comes from.
 *
 * Geometry (p105): one PPN on CE0 of each bus, 2 CAUs per PPN, 1064 blocks per
 * CAU, 256 pages per block.  A "die" here is a (bus, CAU) pair, 0-3 =
 * bus << 1 | cau.  A page comes off the bus as 16448 bytes: four times 1024
 * data + 16 FTL metadata + 3072 data.
 */
#ifndef PPN_H
#define PPN_H

#include <stdint.h>

#define PPN_BUSES               2
#define PPN_CAUS                2
#define PPN_DIES                (PPN_BUSES * PPN_CAUS)
#define PPN_BLOCKS              1064
#define PPN_PAGES               256
#define PPN_PAGE_RAW            16448
#define PPN_CHUNK               4112            /* 1024 + 16 meta + 3072 */
#define PPN_META_OFF            1024

/* Page status bytes: 0x40 good, 0x42 good but due for a refresh, 0x49 erased,
 * 0x45 on every page of a retired block. */
#define PPN_ST_OK(s)            ((s) == 0x40 || (s) == 0x42)
#define PPN_ST_ERASED           0x49

/* row address for the page-read command: page | block << 8 | cau << 19 */
#define PPN_ROW(cau, blk, pg)   ((uint32_t)(pg) | (uint32_t)(blk) << 8 | (uint32_t)(cau) << 19)

/* Registers, for the tools that poke them directly (nandctl regs/sdr/ddr). */
#define FMI_BASE(bus)           (0x31200000u + (bus) * 0x100000u)
#define FMC_OFF                 0x40000u
#define ECC_OFF                 0x80000u
#define PMGR_GATES              0x3f101000u

#define FMI_CONFIG              0x00
#define FMI_CONTROL             0x04
#define FMI_STATUS              0x0c
#define FMI_INTEN               0x10
#define FMI_DATA                0x14    /* PIO FIFO: a read pops it */
#define FMI_DMA_STATUS          0x1c
#define FMI_PIO_CONFIG          0x34

#define FMC_ON                  0x00
#define FMC_IF_CTRL             0x08
#define FMC_CE_CTRL             0x0c
#define FMC_RW_CTRL             0x10
#define FMC_CMD                 0x14
#define FMC_ADDR0               0x18
#define FMC_ADDRNUM             0x20
#define FMC_DATANUM             0x24
#define FMC_INTMASK             0x40
#define FMC_STATUS              0x44
#define FMC_NAND_STATUS         0x48
#define FMC_STATUS_MASK         0x4c

#define PPN_R(b, o)             ((b)[(o) / 4])
#define PPN_W(b, o, v)          ((b)[(o) / 4] = (v))

extern volatile uint32_t *ppn_fmi[PPN_BUSES], *ppn_fmc[PPN_BUSES], *ppn_ecc[PPN_BUSES];
extern volatile uint32_t *ppn_pmgr;

int ppn_open(void);                     /* map everything; exits on failure */
int ppn_ce_ok(int ce);                  /* a chip behind this CE (ce-bitmap 0x101) */
int ppn_gates_on(int bus);              /* clocked */
int ppn_lock(int bus);                  /* flock /tmp/nandctl-busN.lock, held until exit */
int ppn_claim(int bus);                 /* clocked and ours */

int ppn_nand_reset(int bus, int ce);
int ppn_read_id(int bus, int ce, uint8_t addr, uint8_t id[8]);
int ppn_get_feature(int bus, int ce, uint16_t feat, uint8_t *buf, uint32_t len, uint8_t *st);
int ppn_params(int bus, int ce, uint8_t *buf, uint8_t *st);
int ppn_set_power_state(int bus, int ce, uint32_t ps, uint8_t *st);
void ppn_recover(int bus);

/* One page, the first len bytes of it (a multiple of 4).  Returns 0 when the
 * data came out; *st is the PPN's status byte either way. */
int ppn_read_page(int bus, int ce, uint32_t row, uint8_t *buf, uint32_t len, uint8_t *st);

#define PPN_FEATURE_POWER_STATE 0x180
#define PPN_PS_ASYNC            0x01
#define PPN_PS_DDR              0x0a

#endif
