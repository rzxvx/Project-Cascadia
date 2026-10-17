/* Fake providers for actual candidate C; no hardware/API adapter. */
#include <assert.h>
#include <string.h>
#include <linux/regmap.h>
#include <linux/mutex.h>
#include "cascadia-controller.h"
static struct cascadia_controller c;
static struct mutex lock;
static struct regmap maps[2];
static unsigned int events[128][3];
static int num, io, fail_io, late_io, stale_io, checks, stale_check, epoch_error;
static int validate(void *ctx, u64 epoch)
{
	assert(ctx == &c && lock.held);
	checks++;
	if (checks == stale_check)
		epoch_error = -116;
	return epoch != 7 ? -116 : epoch_error;
}
static int check_io(void)
{
	assert(lock.held);
	io++;
	if (io == stale_io)
		epoch_error = -116;
	return io == fail_io ? -5 : 0;
}
int regmap_read(struct regmap *m, unsigned int off, unsigned int *value)
{
	int ret;
	assert(!(off & 3) && off / 4 < 2048);
	ret = check_io();
	if (ret)
		return ret;
	*value = m->values[off / 4];
	return io == late_io ? -5 : 0;
}
int regmap_write(struct regmap *m, unsigned int off, unsigned int value)
{
	int ret;
	assert(!(off & 3) && off / 4 < 2048);
	ret = check_io();
	if (ret)
		return ret;
	assert(num < 128);
	events[num][0] = m->id;
	events[num][1] = off;
	events[num++][2] = value;
	m->values[off / 4] = value;
	return io == late_io ? -5 : 0;
}
void udelay(unsigned long us)
{
	assert(lock.held && num < 128);
	events[num][0] = 2;
	events[num][1] = us;
	events[num++][2] = 0;
}
int start(int permit, unsigned int gate, unsigned int mux)
{
	memset(&c, 0, sizeof(c));
	memset(maps, 0, sizeof(maps));
	mutex_init(&lock);
	maps[0].id = 0;
	maps[1].id = 1;
	maps[0].values[0] = mux;
	maps[1].values[0x1088 / 4] = gate;
	num = io = fail_io = late_io = stale_io = checks = stale_check = epoch_error = 0;
	int ret = cascadia_controller_init(&c, &maps[0], &maps[1], &lock);
	c.permitted = permit;
	c.p105_binding_verified = c.complete_clock_and_phy_ownership = 1;
	c.role_lease_owned = c.gate89_owned = c.udc_quiesced = 1;
	c.ehci_all_ports_quiesced = c.hsic_wifi_quiesced = 1;
	c.external_power_contract_verified = 1;
	c.role_lease_epoch = 7;
	c.validate_epoch = validate;
	c.context = &c;
	return ret;
}
int operation(unsigned long long epoch, int reset, int host)
{
	int ret;
	mutex_lock(&lock);
	num = 0;
	ret = reset ? cascadia_controller_ehci_reset(&c, epoch) :
		cascadia_controller_mux(&c, epoch, host);
	mutex_unlock(&lock);
	return ret;
}
int inspect(int field, int i)
{
	assert(i >= 0 && i < 128);
	switch (field) {
	case 0: return num;
	case 1: return events[i][0];
	case 2: return events[i][1];
	case 3: return events[i][2];
	case 4: return io;
	case 5: return checks;
	case 6: return c.fault;
	case 7: return c.fault_error;
	case 8: return c.completed_operations;
	case 9: return c.pending_operation;
	case 10: return c.touched;
	default: assert(0); return 0;
	}
}
void inject(int field, int value)
{
	switch (field) {
	case 0: fail_io = value; break;
	case 1: late_io = value; break;
	case 2: stale_io = value; break;
	case 3: stale_check = value; break;
	case 4: c.p105_binding_verified = 0; break;
	case 5: c.complete_clock_and_phy_ownership = 0; break;
	case 6: c.role_lease_owned = 0; break;
	case 7: c.gate89_owned = 0; break;
	case 8: c.udc_quiesced = 0; break;
	case 9: c.ehci_all_ports_quiesced = 0; break;
	case 10: c.hsic_wifi_quiesced = 0; break;
	case 11: c.external_power_contract_verified = 0; break;
	case 12: c.validate_epoch = 0; break;
	case 13: c.role_lease_epoch = value; break;
	case 14: epoch_error = value; break;
	case 15: c.validate_epoch = validate; break;
	default: assert(0);
	}
}
unsigned int regvalue(int id)
{
	assert(id == 0 || id == 1);
	return id ? maps[1].values[0x1088 / 4] : maps[0].values[0];
}
int reinit(void)
{
	return cascadia_controller_init(&c, &maps[0], &maps[1], &lock);
}
int bad_init(int which)
{
	struct cascadia_controller tmp = {0};
	return cascadia_controller_init(&tmp, which == 0 ? (void *)-5 : &maps[0],
		which == 1 ? (void *)-5 : &maps[1], which == 2 ? 0 : which == 3 ? (void *)-5 : &lock);
}
