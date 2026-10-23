// SPDX-License-Identifier: GPL-2.0
#include <linux/string.h>
#include "p105-udc-observer.h"

void p105_udc_observer_init(struct p105_udc_observer *o)
{
	memset(o, 0, sizeof(*o));
	spin_lock_init(&o->lock);
	o->seen.generation = 1;
	o->seen.uncertain = true;
}
/* lock held; both the setup outcome and status transfer must be observed. */
static void p105_finish(struct p105_udc_observer *o)
{
	if (!o->seen.pending || !o->seen.setup_ok || !o->seen.status_seen)
		return;
	o->seen.completed_cookie = o->seen.control_cookie;
	o->seen.completed_configuration = o->pending_configuration;
	o->seen.completed_draw_request_ma = o->pending_draw_request_ma;
	o->seen.completed_draw_seen = o->pending_draw_seen;
	o->seen.completed = true;
	o->seen.uncertain = false;
	o->seen.pending = false;
}
u64 p105_udc_setup_begin(struct p105_udc_observer *o, bool configuration,
			 u16 value)
{
	unsigned long flags;
	u64 cookie;
	spin_lock_irqsave(&o->lock, flags);
	if (o->seen.pending || configuration) {
		o->seen.generation++;
		o->seen.completed = false;
		o->seen.uncertain = true;
	}
	o->seen.pending = configuration;
	o->seen.setup_ok = false;
	o->seen.delayed_status = false;
	o->seen.status_seen = false;
	o->pending_configuration = value;
	o->pending_draw_request_ma = 0;
	o->pending_draw_seen = false;
	if (++o->seen.control_cookie == 0)
		o->seen.control_cookie++;
	cookie = o->seen.control_cookie;
	spin_unlock_irqrestore(&o->lock, flags);
	return cookie;
}
void p105_udc_setup_result(struct p105_udc_observer *o, u64 cookie, int result,
			   bool delayed)
{
	unsigned long flags;
	spin_lock_irqsave(&o->lock, flags);
	if (o->seen.pending && cookie == o->seen.control_cookie) {
		o->seen.setup_ok = result >= 0;
		o->seen.delayed_status = delayed;
		if (result < 0)
			o->seen.pending = false;
		else
			p105_finish(o);
	}
	spin_unlock_irqrestore(&o->lock, flags);
}
void p105_udc_observe_draw(struct p105_udc_observer *o, unsigned int ma)
{
	unsigned long flags;
	spin_lock_irqsave(&o->lock, flags);
	o->seen.last_draw_request_ma = ma;
	if (o->seen.pending) {
		o->pending_draw_request_ma = ma;
		o->pending_draw_seen = true;
	}
	spin_unlock_irqrestore(&o->lock, flags);
}
u64 p105_udc_pending_cookie(struct p105_udc_observer *o)
{
	unsigned long flags;
	u64 cookie;
	spin_lock_irqsave(&o->lock, flags);
	cookie = o->seen.pending ? o->seen.control_cookie : 0;
	spin_unlock_irqrestore(&o->lock, flags);
	return cookie;
}
void p105_udc_status_complete(struct p105_udc_observer *o, u64 cookie,
			      int status)
{
	unsigned long flags;
	spin_lock_irqsave(&o->lock, flags);
	if (cookie && o->seen.pending && cookie == o->seen.control_cookie) {
		if (status)
			o->seen.pending = false;
		else {
			o->seen.status_seen = true;
			p105_finish(o);
		}
	}
	spin_unlock_irqrestore(&o->lock, flags);
}
void p105_udc_invalidate(struct p105_udc_observer *o, unsigned int reason)
{
	unsigned long flags;
	spin_lock_irqsave(&o->lock, flags);
	o->seen.generation++;
	o->seen.control_cookie++;
	o->seen.pending = false;
	o->seen.completed = false;
	o->seen.uncertain = true;
	o->seen.invalidation_reason = reason;
	spin_unlock_irqrestore(&o->lock, flags);
}
void p105_udc_observer_read(struct p105_udc_observer *o,
			    struct p105_udc_observation *out)
{
	unsigned long flags;
	spin_lock_irqsave(&o->lock, flags);
	*out = o->seen;
	spin_unlock_irqrestore(&o->lock, flags);
}
