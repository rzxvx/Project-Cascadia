// SPDX-License-Identifier: GPL-2.0
/* Private fixed-peripheral/composite USB2 source observation reader.
 * No PMU, PHY, regulator, clock or USB-controller register access is added.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/usb/composite.h>
#include "core.h"
#include "p105-dwc2-source.h"

struct p105_dwc2_source_reader {
	struct device *consumer, *supplier;
	struct device_link *link;
	struct dwc2_hsotg *context;
	struct notifier_block *subscription;
};
static bool p105_source_usable(const struct p105_dwc2_source_reader *r)
{
	enum dl_dev_state consumer;
	enum device_link_state state;
	if (!r)
		return false;
	consumer = READ_ONCE(r->consumer->links.status);
	if (consumer != DL_DEV_PROBING && consumer != DL_DEV_DRIVER_BOUND)
		return false;
	state = READ_ONCE(r->link->status);
	return (state == DL_STATE_CONSUMER_PROBE || state == DL_STATE_ACTIVE) &&
	       READ_ONCE(r->supplier->links.status) == DL_DEV_DRIVER_BOUND &&
	       p105_dwc2_matches(r->supplier);
}
struct p105_dwc2_source_reader *p105_dwc2_source_open(struct device *consumer,
						      struct device *supplier)
{
	struct p105_dwc2_source_reader *r;
	struct dwc2_hsotg *h;
	struct device_link *link;
	enum device_link_state state;
	if (!consumer || !supplier || !dev_is_platform(consumer) ||
	    !dev_is_platform(supplier) || consumer == supplier ||
	    READ_ONCE(consumer->links.status) != DL_DEV_PROBING)
		return ERR_PTR(-EINVAL);
	link = device_link_add(consumer, supplier, DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link)
		return ERR_PTR(-EPROBE_DEFER);
	state = READ_ONCE(link->status);
	if ((state != DL_STATE_CONSUMER_PROBE && state != DL_STATE_ACTIVE) ||
	    READ_ONCE(supplier->links.status) != DL_DEV_DRIVER_BOUND)
		return ERR_PTR(-EPROBE_DEFER);
	if (!try_module_get(THIS_MODULE))
		return ERR_PTR(-ENODEV);
	if (!p105_dwc2_matches(supplier)) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENODEV);
	}
	h = dev_get_drvdata(supplier);
	if (!h || h->dev != supplier) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENODEV);
	}
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		module_put(THIS_MODULE);
		return ERR_PTR(-ENOMEM);
	}
	r->consumer = get_device(consumer);
	r->supplier = get_device(supplier);
	r->link = link;
	r->context = h;
	return r;
}
EXPORT_SYMBOL_GPL(p105_dwc2_source_open);
void p105_dwc2_source_close(struct p105_dwc2_source_reader *r)
{
	if (!r || IS_ERR(r))
		return;
	p105_dwc2_source_unregister(r);
	put_device(r->supplier);
	put_device(r->consumer);
	kfree(r);
	module_put(THIS_MODULE);
}
EXPORT_SYMBOL_GPL(p105_dwc2_source_close);

/* Caller holds controller lock. Only this reviewed fixed-peripheral composite
 * path is supported: no HNP/host-role or accessory-power inference. */
static bool p105_capture(struct dwc2_hsotg *h,
			 struct p105_udc_observation *seen)
{
	return h->dr_mode == USB_DR_MODE_PERIPHERAL && h->gadget_enabled &&
	       h->ll_hw_enabled && h->connected && !h->test_mode &&
	       h->lx_state == DWC2_L0 && !h->bus_suspended && h->driver &&
	       h->driver->setup == composite_setup &&
	       (h->gadget.speed == USB_SPEED_FULL ||
		h->gadget.speed == USB_SPEED_HIGH) &&
	       p105_udc_observer_try_read(&h->p105_power_observer, seen) &&
	       seen->completed && !seen->uncertain &&
	       seen->completed_configuration && seen->completed_draw_seen;
}
int p105_dwc2_source_snapshot(struct p105_dwc2_source_reader *r,
			      struct p105_usb_source_sample *out)
{
	struct p105_udc_observation seen;
	struct dwc2_hsotg *h;
	struct p105_usb_source_sample sample;
	unsigned long flags;
	bool valid;
	u64 now;
	if (!out)
		return -EINVAL;
	if (!p105_source_usable(r))
		return -ENODEV;
	h = r->context;
	spin_lock_irqsave(&h->lock, flags);
	valid = p105_capture(h, &seen);
	now = ktime_get_ns();
	if (valid)
		sample = (struct p105_usb_source_sample){
			.generation = seen.generation,
			.completed_cookie = seen.completed_cookie,
			.observed_ns = now,
			.valid_until_ns = now + 250 * NSEC_PER_MSEC,
			.budget_ua = min(seen.completed_draw_request_ma, 500U) *
				     1000,
			.kind = P105_SOURCE_USB2_CONFIGURED,
			.online = true,
		};
	spin_unlock_irqrestore(&h->lock, flags);
	if (!valid)
		return -EAGAIN;
	*out = sample;
	return 0;
}
EXPORT_SYMBOL_GPL(p105_dwc2_source_snapshot);
bool p105_dwc2_source_valid(struct p105_dwc2_source_reader *r,
			    const struct p105_usb_source_sample *sample)
{
	struct p105_udc_observation seen;
	struct dwc2_hsotg *h;
	unsigned long flags;
	bool valid;
	u64 now = ktime_get_ns();
	if (!sample || !sample->online ||
	    sample->kind != P105_SOURCE_USB2_CONFIGURED ||
	    sample->observed_ns > now || sample->valid_until_ns <= now ||
	    sample->valid_until_ns - sample->observed_ns >
		    250 * NSEC_PER_MSEC ||
	    !p105_source_usable(r))
		return false;
	h = r->context;
	if (!spin_trylock_irqsave(&h->lock, flags))
		return false;
	valid = p105_capture(h, &seen) &&
		seen.generation == sample->generation &&
		seen.completed_cookie == sample->completed_cookie &&
		min(seen.completed_draw_request_ma, 500U) * 1000 ==
			sample->budget_ua;
	spin_unlock_irqrestore(&h->lock, flags);
	return valid;
}
EXPORT_SYMBOL_GPL(p105_dwc2_source_valid);

int p105_dwc2_source_register(struct p105_dwc2_source_reader *r,
			      struct notifier_block *nb)
{
	int ret;
	if (!nb || !nb->notifier_call || !p105_source_usable(r))
		return -ENODEV;
	if (r->subscription)
		return -EBUSY;
	ret = p105_udc_observer_register(&r->context->p105_power_observer, nb);
	if (!ret)
		r->subscription = nb;
	return ret;
}
EXPORT_SYMBOL_GPL(p105_dwc2_source_register);
void p105_dwc2_source_unregister(struct p105_dwc2_source_reader *r)
{
	if (!r || !r->subscription)
		return;
	/* The managed supplier link preserves context until consumer teardown.
	 * Do not test bound status here: teardown must drain even after unbind starts. */
	p105_udc_observer_unregister(&r->context->p105_power_observer,
				     r->subscription);
	r->subscription = NULL;
}
EXPORT_SYMBOL_GPL(p105_dwc2_source_unregister);
