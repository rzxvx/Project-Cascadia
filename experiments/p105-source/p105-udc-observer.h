/* SPDX-License-Identifier: GPL-2.0 */
#ifndef P105_UDC_OBSERVER_H
#define P105_UDC_OBSERVER_H
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
/* Controller-owned observations with explicitly drained atomic listeners.
 * No physical-current measurement or charging permission is introduced. */
struct p105_udc_observation {
	u64 generation, control_cookie, completed_cookie;
	unsigned int last_draw_request_ma, completed_draw_request_ma;
	u16 completed_configuration;
	bool completed_draw_seen;
	bool pending, setup_ok, delayed_status, status_seen, completed,
		uncertain;
	unsigned int invalidation_reason;
};
struct p105_udc_observer {
	spinlock_t lock;
	struct atomic_notifier_head changes;
	struct p105_udc_observation seen;
	u16 pending_configuration;
	unsigned int pending_draw_request_ma;
	bool pending_draw_seen;
};
enum p105_udc_invalidation {
	P105_UDC_RESET = 1,
	P105_UDC_DISCONNECT,
	P105_UDC_SUSPEND,
	P105_UDC_ROLE,
	P105_UDC_REMOVE,
};
#if defined(CONFIG_P105_DWC2_POWER_OBSERVER)
int p105_udc_observer_register(struct p105_udc_observer *,
			       struct notifier_block *);
int p105_udc_observer_unregister(struct p105_udc_observer *,
				 struct notifier_block *);
void p105_udc_observer_init(struct p105_udc_observer *);
u64 p105_udc_setup_begin(struct p105_udc_observer *, bool, u16);
void p105_udc_setup_result(struct p105_udc_observer *, u64, int, bool);
void p105_udc_observe_draw(struct p105_udc_observer *, unsigned int);
u64 p105_udc_pending_cookie(struct p105_udc_observer *);
void p105_udc_status_complete(struct p105_udc_observer *, u64, int);
void p105_udc_invalidate(struct p105_udc_observer *, unsigned int);
bool p105_udc_observer_try_read(struct p105_udc_observer *,
				struct p105_udc_observation *);
void p105_udc_observer_read(struct p105_udc_observer *,
			    struct p105_udc_observation *);
#else
static inline void p105_udc_observer_init(struct p105_udc_observer *o)
{
	(void)o;
}
static inline u64 p105_udc_setup_begin(struct p105_udc_observer *o, bool c,
				       u16 v)
{
	(void)o;
	(void)c;
	(void)v;
	return 0;
}
static inline void p105_udc_setup_result(struct p105_udc_observer *o, u64 c,
					 int r, bool d)
{
	(void)o;
	(void)c;
	(void)r;
	(void)d;
}
static inline void p105_udc_observe_draw(struct p105_udc_observer *o,
					 unsigned int m)
{
	(void)o;
	(void)m;
}
static inline u64 p105_udc_pending_cookie(struct p105_udc_observer *o)
{
	(void)o;
	return 0;
}
static inline void p105_udc_status_complete(struct p105_udc_observer *o, u64 c,
					    int s)
{
	(void)o;
	(void)c;
	(void)s;
}
static inline void p105_udc_invalidate(struct p105_udc_observer *o,
				       unsigned int r)
{
	(void)o;
	(void)r;
}
static inline void p105_udc_observer_read(struct p105_udc_observer *o,
					  struct p105_udc_observation *s)
{
	(void)o;
	*s = (struct p105_udc_observation){};
}
#endif
#endif
