/* SPDX-License-Identifier: GPL-2.0 */
#ifndef P105_DWC2_SOURCE_H
#define P105_DWC2_SOURCE_H
#include <linux/types.h>
struct device;
struct notifier_block;
struct p105_dwc2_source_reader;
enum p105_source_kind { P105_SOURCE_USB2_CONFIGURED = 1 };
/* USB protocol allowance, NOT measured draw or complete charging permission.
 * External accessory classification/handshake is a separate unimplemented API. */
struct p105_usb_source_sample {
	u64 generation, completed_cookie, observed_ns, valid_until_ns;
	unsigned int budget_ua;
	enum p105_source_kind kind;
	bool online;
};
/* Probe-time managed lifetime. Caller holds a source platform-device reference;
 * removal/error unwind drains callbacks before close, with no close race/use
 * afterward. The driver core owns the link; do not manually delete it. */
struct p105_dwc2_source_reader *p105_dwc2_source_open(struct device *,
						      struct device *);
void p105_dwc2_source_close(struct p105_dwc2_source_reader *);
int p105_dwc2_source_snapshot(struct p105_dwc2_source_reader *,
			      struct p105_usb_source_sample *);
bool p105_dwc2_source_valid(struct p105_dwc2_source_reader *,
			    const struct p105_usb_source_sample *);
/* Process-context, caller-serialized subscription lifetime; unregister drains
 * callbacks. Callback must not call source/observer reads or unregister itself. */
int p105_dwc2_source_register(struct p105_dwc2_source_reader *,
			      struct notifier_block *);
void p105_dwc2_source_unregister(struct p105_dwc2_source_reader *);
/* Internal same-DWC2-module identity proof supplied by platform.c. */
bool p105_dwc2_matches(struct device *);
#endif
