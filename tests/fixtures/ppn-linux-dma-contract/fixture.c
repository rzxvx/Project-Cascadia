// SPDX-License-Identifier: GPL-2.0-only
/* Included actual Linux code retains Copyright(c) 2004 - 2006 Intel
 * Corporation and Copyright(c) 2014-2022 NVIDIA CORPORATION.
 * Actual Linux6.12 functions inserted from pinned GPL sources at build time.
 * Remaining structs/APIs are explicit process-local Fakes; no kernel ABI or
 * Tegra/A5 hardware equivalence is claimed. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
struct dma_chan;
struct dma_device {
    int (*device_terminate_all)(struct dma_chan *);
    void (*device_synchronize)(struct dma_chan *);
};
struct dma_chan { struct dma_device *device; };
struct virtual_dma_chan { struct dma_chan chan; int lock; };
struct descriptor { int vd; };
struct tegra_dma_channel;
struct chip_data { int (*terminate)(struct tegra_dma_channel *); };
struct tegra { struct chip_data *chip_data; };
struct tegra_dma_channel {
    struct virtual_dma_chan vc;
    struct tegra *tdma;
    struct descriptor *dma_desc;
    int irq;
};
struct list_head { unsigned int marker; };
#define LIST_HEAD(name) struct list_head name = {0}
static struct tegra_dma_channel fixture_channel;
static int trace[32], nt, chip_error, callback_active, irq_active, hardware_busy;
static unsigned int sleep_checks;
static void add(int value) { assert(nt < 32); trace[nt++] = value; }
#define spin_lock_irqsave(lockptr, flags) do { assert((lockptr)==&fixture_channel.vc.lock); (flags)=1; add(1); } while (0)
#define spin_unlock_irqrestore(lockptr, flags) do { assert((lockptr)==&fixture_channel.vc.lock && (flags)==1); add(3); } while (0)
static struct tegra_dma_channel *to_tegra_dma_chan(struct dma_chan *c)
{ assert(c==&fixture_channel.vc.chan); return &fixture_channel; }
static void vchan_terminate_vdesc(int *vd) { assert(vd==&fixture_channel.dma_desc->vd); add(4); }
static void tegra_dma_disable(struct tegra_dma_channel *t) { assert(t==&fixture_channel); add(5); }
static void tegra_dma_sid_free(struct tegra_dma_channel *t) { assert(t==&fixture_channel); add(6); }
static void vchan_get_all_descriptors(struct virtual_dma_chan *v, struct list_head *head)
{ assert(v==&fixture_channel.vc && !head->marker); add(7); }
static void vchan_dma_desc_free_list(struct virtual_dma_chan *v, struct list_head *head)
{ assert(v==&fixture_channel.vc && !head->marker); add(8); }
static void synchronize_irq(int irq) { assert(irq==55); irq_active=0; add(9); }
static void vchan_synchronize(struct virtual_dma_chan *v) { assert(v==&fixture_channel.vc); callback_active=0; add(10); }
static void might_sleep(void) { sleep_checks++; }
static int chip_terminate(struct tegra_dma_channel *t)
{ assert(t==&fixture_channel); add(2); if (!chip_error) hardware_busy=0; return chip_error; }
static int tegra_dma_pause(struct tegra_dma_channel *t)
{ assert(t==&fixture_channel); add(11); return chip_error; }
#include "actual-kernel-functions.h"

static int inadequate_terminate(struct dma_chan *c)
{ assert(c==&fixture_channel.vc.chan); add(12); return 0; }
int main(void)
{
    struct dma_device dev = {tegra_dma_terminate_all, tegra_dma_chan_synchronize};
    struct descriptor desc;
    struct chip_data chip = {chip_terminate};
    struct tegra controller = {&chip};
    unsigned int cases = 0;
    const int errors[] = {-ETIMEDOUT,-EIO,0,1};
    for (unsigned int active=0; active<2; active++) {
        for (unsigned int i=0; i<4; i++) {
            for (unsigned int callbacks=0; callbacks<2; callbacks++) {
                memset(&fixture_channel,0,sizeof(fixture_channel));
                fixture_channel.vc.chan.device=&dev;fixture_channel.tdma=&controller;fixture_channel.irq=55;fixture_channel.dma_desc=active?&desc:NULL;
                chip_error=errors[i];nt=0;sleep_checks=0;irq_active=callback_active=callbacks;hardware_busy=active;
                int result=dmaengine_terminate_sync(&fixture_channel.vc.chan);
                int expected_error=active?chip_error:0;
                assert(result==expected_error);
                if (expected_error) {
                    const int expected[]={1,2,3};
                    assert(nt==3 && !memcmp(trace,expected,sizeof(expected)));
                    assert(fixture_channel.dma_desc==&desc && hardware_busy && irq_active==(int)callbacks && callback_active==(int)callbacks);
                    assert(!sleep_checks);
                } else {
                    const int full[]={1,2,4,5,6,7,3,8,9,10};
                    const int idle[]={1,6,7,3,8,9,10};
                    assert(nt==(active?10:7) && !memcmp(trace,active?full:idle,(size_t)nt*sizeof(int)));
                    assert(!fixture_channel.dma_desc && !hardware_busy && !irq_active && !callback_active && sleep_checks==1);
                }
                cases++;
            }
        }
    }
    /* Invalid-provider control: generic API trusts terminate0, sync optional.
     * This deliberately violates provider contract; not a Linux/Tegra bug and
     * never a valid substitute for an A5 physical DMA fence. */
    dev.device_terminate_all=inadequate_terminate;dev.device_synchronize=NULL;
    fixture_channel.vc.chan.device=&dev;nt=0;hardware_busy=irq_active=callback_active=1;
    assert(!dmaengine_terminate_sync(&fixture_channel.vc.chan));
    assert(nt==1 && trace[0]==12 && hardware_busy && irq_active && callback_active);
    cases++;
    dev.device_terminate_all=NULL;nt=0;
    assert(dmaengine_terminate_sync(&fixture_channel.vc.chan)==-EINVAL && !nt);
    cases++;
    /* Actual chip-specific noerr helper deliberately discards pause status.
     * Its documented Tegra recovery rationale cannot be transplanted to A5. */
    for(unsigned int i=0;i<4;i++) {
        nt=0;chip_error=errors[i];
        assert(!tegra_dma_pause_noerr(&fixture_channel) && nt==1 && trace[0]==11);
        cases++;
    }
    printf("PASS %u actual Linux provider/API contract controls, hardware_calls=0\n",cases);
    return 0;
}
