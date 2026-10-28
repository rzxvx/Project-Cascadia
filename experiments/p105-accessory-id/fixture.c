#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "cascadia-accessory-id.h"

static void clear_record(const struct cascadia_id_record *r)
{
	const unsigned char *p = (const unsigned char *)r;
	size_t i;

	for (i = 0; i < sizeof(*r); i++)
		assert(!p[i]);
}

int main(void)
{
	struct cascadia_id_policy p = { 0 };
	struct cascadia_id_record r;
	unsigned int a, b, c, d, e, f, debug, slot, prior;
	u8 raw[6], frame[7] = { 5, 4, 3, 2, 1, 8, 0x75 }, out[6];
	int error, n;

	memset(out, 0xa5, sizeof(out));
	assert(cascadia_ths_id_frame(frame, 7, 0x80, out) == 0);
	assert(memcmp(out, (u8[]){ 8, 1, 2, 3, 4, 5 }, 6) == 0);
	assert(cascadia_ths_id_frame(frame, 7, 0, out) < 0);
	assert(memcmp(out, (u8[6]){ 0 }, 6) == 0);
	frame[6] = 0x74;
	assert(cascadia_ths_id_frame(frame, 7, 0x80, out) < 0);
	assert(memcmp(out, (u8[6]){ 0 }, 6) == 0);
	frame[6] = 0x75;
	assert(cascadia_ths_id_frame(frame, 6, 0x80, out) < 0);
	assert(cascadia_ths_id_frame(NULL, 7, 0x80, out) < 0);
	assert(cascadia_ths_id_frame(frame, 7, 0x80, NULL) < 0);
	assert(cascadia_ths_id_frame(frame, 7, 0x80, frame) == 0);
	assert(memcmp(frame, (u8[]){ 8, 1, 2, 3, 4, 5 }, 6) == 0);
	raw[0] = 0x2a;
	memset(raw + 1, 0, 5);
	memset(&r, 0xa5, sizeof(r));
	assert(cascadia_accessory_id_decode(raw, 6, &p, &r) < 0);
	clear_record(&r);
	raw[0] = 0x10;
	assert(cascadia_accessory_id_decode(raw, 6, &p, &r) < 0);
	clear_record(&r);
	raw[0] = 0x18;
	assert(cascadia_accessory_id_decode(raw, 6, &p, &r) < 0);
	clear_record(&r);
	raw[0] = 8;
	assert(cascadia_accessory_id_decode(raw, 6, &p, &r) == 0);
	assert(r.valid && r.pair[0] == 0x75736230 && r.usb_control_low_flags == 5);
	assert(cascadia_accessory_id_decode(NULL, 6, &p, &r) < 0);
	clear_record(&r);
	assert(cascadia_accessory_id_decode(raw, 5, &p, &r) < 0);
	clear_record(&r);
	assert(cascadia_accessory_id_decode(raw, 6, NULL, &r) < 0);
	clear_record(&r);
	assert(cascadia_accessory_id_decode(raw, 6, &p, NULL) < 0);
	memset(&r, 0xa5, sizeof(r));
	memcpy(&r, (u8[]){ 8, 0, 0, 0, 0, 0 }, 6);
	assert(cascadia_accessory_id_decode((const u8 *)&r, 6, &p, &r) == 0);
	assert(r.valid && r.pair[0] == 0x75736230 && r.usb_control_low_flags == 5);
	/* Snapshot caller policy before an overlapping result is cleared. */
	memset(&r, 0, sizeof(r));
	p = (struct cascadia_id_policy) { .source_ignore_vbus_known = true,
		.source_ignore_vbus_present = true };
	memcpy(&r, &p, sizeof(p));
	raw[0] = 0x10;
	assert(cascadia_accessory_id_decode(raw, 6,
		(const struct cascadia_id_policy *)&r, &r) == 0);
	assert(r.valid && r.usb_control_low_flags == 4);
	while ((n = scanf("%x %x %x %x %x %x %u %u %u", &a, &b, &c,
			  &d, &e, &f, &debug, &slot, &prior)) == 9) {
		assert(a < 256 && b < 256 && c < 256 && d < 256 && e < 256 && f < 256);
		raw[0] = a; raw[1] = b; raw[2] = c; raw[3] = d; raw[4] = e; raw[5] = f;
		p = (struct cascadia_id_policy) {
			.debugger_known = true,
			.debugger_allowed = !!debug,
			.source_ignore_vbus_known = true,
			.source_ignore_vbus_present = slot,
			.source_md_root_known = true,
			.source_md_root = prior,
		};
		memset(&r, 0xa5, sizeof(r));
		error = cascadia_accessory_id_decode(raw, 6, &p, &r);
		if (error)
			clear_record(&r);
		printf("%d %u %x %x %x\n", error, r.valid, r.pair[0],
		       r.pair[1], r.usb_control_low_flags);
	}
	assert(n == EOF);
	return 0;
}
