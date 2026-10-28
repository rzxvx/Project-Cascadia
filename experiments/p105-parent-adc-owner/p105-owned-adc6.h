// SPDX-License-Identifier: GPL-2.0-only
#ifndef P105_OWNED_ADC6_H
#define P105_OWNED_ADC6_H
#include <linux/device.h>
#include "apple-pmu-d1946-bank0.h"
#include "p105-adc6-poll.h"
struct p105_owned_adc6;
/* Only parent probe may create, before children. No first-quiescence grant. */
struct p105_owned_adc6 *p105_owned_adc6_create(struct device *dev,
					    struct d1946_bank0_bus *bus);
int p105_owned_adc6_read(struct p105_owned_adc6 *owner,
			struct p105_adc6_sample *out);
void p105_owned_adc6_close(void *context);
#endif
