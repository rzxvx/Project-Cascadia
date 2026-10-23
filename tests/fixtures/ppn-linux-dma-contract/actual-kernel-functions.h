/* SPDX-License-Identifier: GPL-2.0-only
 * Actual pinned Linux6.12 functions.
 * Copyright(c) 2004 - 2006 Intel Corporation. All rights reserved.
 * Copyright (c) 2014-2022, NVIDIA CORPORATION. All rights reserved.
 * Intel DMAengine source GPL-2.0-or-later; NVIDIA source GPL-2.0-only.
 */
static int tegra_dma_terminate_all(struct dma_chan *dc)
{
	struct tegra_dma_channel *tdc = to_tegra_dma_chan(dc);
	unsigned long flags;
	LIST_HEAD(head);
	int err;

	spin_lock_irqsave(&tdc->vc.lock, flags);

	if (tdc->dma_desc) {
		err = tdc->tdma->chip_data->terminate(tdc);
		if (err) {
			spin_unlock_irqrestore(&tdc->vc.lock, flags);
			return err;
		}

		vchan_terminate_vdesc(&tdc->dma_desc->vd);
		tegra_dma_disable(tdc);
		tdc->dma_desc = NULL;
	}

	tegra_dma_sid_free(tdc);
	vchan_get_all_descriptors(&tdc->vc, &head);
	spin_unlock_irqrestore(&tdc->vc.lock, flags);

	vchan_dma_desc_free_list(&tdc->vc, &head);

	return 0;
}

static void tegra_dma_chan_synchronize(struct dma_chan *dc)
{
	struct tegra_dma_channel *tdc = to_tegra_dma_chan(dc);

	synchronize_irq(tdc->irq);
	vchan_synchronize(&tdc->vc);
}

static inline int tegra_dma_pause_noerr(struct tegra_dma_channel *tdc)
{
	/* Return 0 irrespective of PAUSE status.
	 * This is useful to recover channels that can exit out of flush
	 * state when the channel is disabled.
	 */

	tegra_dma_pause(tdc);
	return 0;
}

static inline int dmaengine_terminate_async(struct dma_chan *chan)
{
	if (chan->device->device_terminate_all)
		return chan->device->device_terminate_all(chan);

	return -EINVAL;
}

static inline void dmaengine_synchronize(struct dma_chan *chan)
{
	might_sleep();

	if (chan->device->device_synchronize)
		chan->device->device_synchronize(chan);
}

static inline int dmaengine_terminate_sync(struct dma_chan *chan)
{
	int ret;

	ret = dmaengine_terminate_async(chan);
	if (ret)
		return ret;

	dmaengine_synchronize(chan);

	return 0;
}
