/* Fake regmap/lock/epoch only. Actual candidate C is compiled separately. */
#include <assert.h>
#include <string.h>
#include <linux/regmap.h>
#include <linux/mutex.h>
#include "cascadia-native-clock.h"
static struct cascadia_native_clock c;
static struct cascadia_native_adoption a;
static struct mutex lock;
static struct regmap map;
static unsigned int events[512][2];
static int num, io, fail_io, late_io, stale_io, checks, stale_check, epoch_error;
static int auto_ack, busy, sleeps;
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
	if (busy && off < 0x1000)
		*value |= 0x40000000U;
	return io == late_io ? -5 : 0;
}
int regmap_write(struct regmap *m, unsigned int off, unsigned int value)
{
	int ret;
	assert(!(off & 3) && off / 4 < 2048);
	ret = check_io();
	if (ret)
		return ret;
	assert(num < 512);
	events[num][0] = off;
	events[num++][1] = value;
	m->values[off / 4] = value;
	if (auto_ack && off >= 0x1000)
		m->values[off / 4] = (value & ~0xf0U) | ((value & 15) << 4);
	return io == late_io ? -5 : 0;
}
void usleep_range(unsigned long min, unsigned long max)
{
	assert(lock.held && min == 1 && max == 2);
	sleeps++;
}
int start(int permit, unsigned int seed)
{
	unsigned int i;
	memset(&c, 0, sizeof(c));
	memset(&a, 0, sizeof(a));
	memset(&map, 0, sizeof(map));
	mutex_init(&lock);
	for (i = 0; i < 2048; i++)
		map.values[i] = seed;
	num = io = fail_io = late_io = stale_io = checks = stale_check = epoch_error = sleeps = busy = 0;
	auto_ack = 1;
	int ret = cascadia_native_clock_init(&c, &map, &lock);
	c.permitted = permit;
	c.poll_limit = 3;
	c.validate_epoch = validate;
	c.context = &c;
	a.p105_source_binding_verified = a.dynamic_provider_association_verified = 1;
	a.all_global_owners_adopted = a.inherited_hardware_verified = 1;
	a.all_shared_clients_quiesced = a.gate_power_policy_owned = 1;
	a.epoch = 7;
	return ret;
}
int adopt(void)
{
	int ret;
	mutex_lock(&lock);
	ret = cascadia_native_clock_adopt(&c, &a);
	mutex_unlock(&lock);
	return ret;
}
int request(unsigned long long epoch, unsigned int root, unsigned int owner, int on)
{
	int ret;
	mutex_lock(&lock);
	num = 0;
	ret = cascadia_native_clock_request(&c, epoch, root, owner, on);
	mutex_unlock(&lock);
	return ret;
}
unsigned int inspect(int field, int i)
{
	assert(i >= 0 && i < 512);
	switch (field) {
	case 0: return num;
	case 1: return events[i][0];
	case 2: return events[i][1];
	case 3: return io;
	case 4: return checks;
	case 5: return c.fault;
	case 6: return c.fault_error;
	case 7: return c.completed_operations;
	case 8: return c.pending_valid;
	case 9: return c.touched;
	case 10: assert(i < 6); return c.state.refs[i];
	case 11: assert(i < 2); return c.state.owners[i];
	case 12: assert(i < 6); return c.pending.refs[i];
	case 13: return sleeps;
	case 14: return c.state.foreign_roots;
	case 15: return c.adopted;
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
	case 4: auto_ack = value; break;
	case 5: busy = value; break;
	case 6: epoch_error = value; break;
	case 7: c.poll_limit = value; break;
	case 8: a.baseline.foreign_roots = value; break;
	case 9: c.permitted = value; break;
	case 10: c.validate_epoch = value ? 0 : validate; break;
	default: assert(0);
	}
}
void certificate_flag(int flag, int value)
{
	switch (flag) {
	case 0: a.p105_source_binding_verified = value; break;
	case 1: a.dynamic_provider_association_verified = value; break;
	case 2: a.all_global_owners_adopted = value; break;
	case 3: a.inherited_hardware_verified = value; break;
	case 4: a.all_shared_clients_quiesced = value; break;
	case 5: a.gate_power_policy_owned = value; break;
	default: assert(0);
	}
}
void baseline(int index, unsigned int value)
{
	assert(index >= 0 && index < 6);
	a.baseline.refs[index] = value;
}
void zero_policy(unsigned long long mask) { a.zero_off_gates = mask; }
void set_reg(unsigned int reg, unsigned int value)
{
	assert(!(reg & 3) && reg / 4 < 2048);
	map.values[reg / 4] = value;
}
unsigned int get_reg(unsigned int reg)
{
	assert(!(reg & 3) && reg / 4 < 2048);
	return map.values[reg / 4];
}
int reinit(void) { return cascadia_native_clock_init(&c, &map, &lock); }
int bad_init(int kind)
{
	struct cascadia_native_clock tmp = {0};
	return cascadia_native_clock_init(&tmp,
		kind == 0 ? 0 : kind == 1 ? (void *)-5 : &map,
		kind == 2 ? 0 : kind == 3 ? (void *)-5 : &lock);
}
int unlocked_request(void)
{
	return cascadia_native_clock_request(&c, 7, 89, 0, true);
}
#ifdef NATIVE_SANITIZER_SELFTEST
int main(void)
{
	unsigned int i;
	assert(start(1, 0x24600000) == 0 && adopt() == 0);
	assert(request(7, 89, 0, 1) == 0);
	assert(request(7, 87, 31, 1) == 0);
	assert(request(7, 89, 1, 1) == 0);
	assert(request(7, 89, 0, 0) == 0);
	assert(request(7, 89, 1, 0) == 0);
	assert(inspect(10, 4) == 2);
	assert(request(7, 87, 31, 0) == 0);
	for (i = 0; i < 6; i++)
		assert(inspect(10, i) == 0);
	assert(start(1, 0x24600000) == 0 && adopt() == 0);
	inject(1, 2);
	assert(request(7, 89, 0, 1) == -5 && inspect(5, 0));
	assert(inspect(10, 4) == 0 && inspect(12, 4) == 1);
	assert(request(7, 89, 0, 1) == -5);
	assert(reinit() == -114);
	assert(start(1, 0x24600000) == 0 && adopt() == 0);
	inject(4, 0);
	assert(request(7, 89, 0, 1) == -110);
	assert(inspect(13, 0) == 3);
	for (i = 0; i < 4; i++)
		assert(bad_init(i) == -22);
	return 0;
}
#endif
