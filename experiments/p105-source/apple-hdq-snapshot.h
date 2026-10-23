/* SPDX-License-Identifier: GPL-2.0 */
#ifndef APPLE_HDQ_SNAPSHOT_H
#define APPLE_HDQ_SNAPSHOT_H
#include <linux/types.h>
struct power_supply;
/* A bounded sequential sampling interval, NOT simultaneous cell measurements.
 * gauge_present means successful complete reads, not independent cell-presence
 * detection. Signed current0xffff remains a valid -1mA sample. */
struct apple_hdq_sample {
	u64 generation, begin_ns, end_ns, valid_until_ns;
	int voltage_uv, current_ua, temp_decic, flags, gauge_present;
	/* Same completed burst as voltage/current; no separate sysfs cache read. */
	int capacity_percent, charge_now_uah, charge_full_uah;
};
/* Opaque borrow acquired only from the consumer platform driver's probe.
 * Caller must already hold a live power_supply reference. The managed link
 * targets the actual producer platform parent, not the driverless psy class.
 * Consumer removal/probe-error unwind MUST drain callbacks before close,
 * and MUST close before returning from its driver-core teardown. No call may
 * race close or use the handle afterward. A managed link is not manually freed.
 * API read/valid rejects consumer/supplier unbind before touching HDQ context.
 * There is no automatic DT/provider integration in this private candidate. */
struct device;
struct apple_hdq_reader;
/* Larger output requires a new exported symbol: an old consumer allocates a
 * smaller buffer. Reject mixed modules at symbol resolution, not at runtime.
 * Source aliases keep callers readable; all consumers must be rebuilt. */
#define apple_hdq_reader_snapshot apple_hdq_reader_snapshot_v2
struct apple_hdq_reader *apple_hdq_reader_open(struct device *,
					       struct power_supply *);
/* Managed variant closes before managed links are cleaned. Consumer remove
 * must still drain first; on probe errors register drain actions AFTER this
 * acquisition so reverse devres order drains before the close action. Do not
 * call manual close on a devm-managed handle. */
struct apple_hdq_reader *devm_apple_hdq_reader_open(struct device *,
						    struct power_supply *);
void apple_hdq_reader_close(struct apple_hdq_reader *);
int apple_hdq_reader_snapshot(struct apple_hdq_reader *, u64,
			      struct apple_hdq_sample *);
bool apple_hdq_reader_valid(struct apple_hdq_reader *,
			    const struct apple_hdq_sample *);
#endif
