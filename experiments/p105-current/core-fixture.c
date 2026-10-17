/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d1946-current-core.h"

struct fake {
	u8 regs[256];
	unsigned int calls, writes, fail_at, corrupt_at;
};
static int read_fake(void *p, unsigned int reg, u8 *out, unsigned int n)
{
	struct fake *f = p;
	assert(reg + n <= 256);
	if (++f->calls == f->fail_at)
		return -EREMOTEIO;
	memcpy(out, f->regs + reg, n);
	if (f->calls == f->corrupt_at)
		out[0] ^= 0x40;
	return 0;
}
static int write_fake(void *p, unsigned int reg, const u8 *in, unsigned int n)
{
	struct fake *f = p;
	assert(reg + n <= 256);
	if (++f->calls == f->fail_at)
		return -EREMOTEIO;
	memcpy(f->regs + reg, in, n);
	f->writes++;
	return 0;
}
static void setup(struct fake *f, unsigned int control)
{
	memset(f, 0, sizeof(*f));
	f->regs[0x1b] = 0xa1;
	f->regs[0x1c] = 0x13;
	f->regs[0x3d] = 0x54;
	f->regs[0x3f] = control;
}
static int operation(struct d1946_io *io, const char *kind, unsigned int value,
		     unsigned int ceiling)
{
	if (!strcmp(kind, "charge"))
		return d1946_current_limit(io, value, ceiling);
	return d1946_input_limit(io, value);
}
static void negative_tests(void)
{
	struct fake f;
	struct d1946_io io = { &f, read_fake, write_fake };
	unsigned int total, fail, kind;
	u8 committed;
	int ret;
	/* Error at every actual read/write must stop the transaction immediately. */
	for (kind = 0; kind < 2; kind++) {
		const char *name = kind ? "input" : "charge";
		unsigned int value = kind ? 2100 : 720;
		setup(&f, 0xff);
		assert(!operation(&io, name, value, 63));
		total = f.calls;
		for (fail = 1; fail <= total; fail++) {
			setup(&f, 0xff);
			f.fail_at = fail;
			ret = operation(&io, name, value, 63);
			assert(ret == -EREMOTEIO && f.calls == fail);
		}
	}
	/* Mask ownership preserves neighbouring bits and commits only on success. */
	setup(&f, 0xbf);
	f.regs[0x12] = 0xf7;
	committed = 0x55;
	assert(!d1946_mask_checked(&io, 4, false, &committed));
	assert(committed == 0xe7 && f.regs[0x12] == 0xe7);
	total = f.calls;
	for (fail = 1; fail <= total; fail++) {
		setup(&f, 0xbf);
		f.regs[0x12] = 0xf7;
		f.fail_at = fail;
		committed = 0x55;
		assert(d1946_mask_checked(&io, 4, false, &committed) ==
		       -EREMOTEIO);
		assert(committed == 0x55 && f.calls == fail);
	}
	setup(&f, 0xbf);
	f.corrupt_at = 3;
	assert(d1946_current_limit(&io, 0, 63) == -EIO);
	assert(f.calls == 3 && f.writes == 1);
	setup(&f, 0xff);
	assert(!d1946_current_limit(&io, 0, 63));
	assert(f.writes == 0 && f.calls == 2);
	setup(&f, 0xbf);
	assert(d1946_input_limit(&io, 101) == -EINVAL);
	assert(d1946_current_limit(&io, 2521, 63) == -EINVAL);
	assert(d1946_current_limit(&io, 39, 63) == -ERANGE);
	assert(d1946_current_limit(&io, 720, 0) == -ERANGE);
	assert(d1946_mask_checked(&io, 72, false, &committed) == -EINVAL);
	assert(f.calls == 0);
	assert(d1946_charge_request(1) == -EINVAL);
	assert(d1946_charge_request(39) == -EINVAL);
	assert(d1946_charge_request(721) == -EINVAL);
	assert(!d1946_charge_request(0));
	assert(!d1946_charge_request(40));
	assert(!d1946_charge_request(720));
	puts("error-stop/mask-commit/readback/range fixtures PASS");
}
int main(int argc, char **argv)
{
	struct fake f;
	struct d1946_io io = { &f, read_fake, write_fake };
	unsigned int i, control;
	if (argc == 2 && !strcmp(argv[1], "negative")) {
		negative_tests();
		return 0;
	}
	assert(argc == 4);
	control = strtoul(argv[3], NULL, 0);
	setup(&f, control);
	assert(!operation(&io, argv[1], strtoul(argv[2], NULL, 0),
			  control & 63));
	for (i = 0; i < 256; i++)
		printf("%02x", f.regs[i]);
	puts("");
	return 0;
}
