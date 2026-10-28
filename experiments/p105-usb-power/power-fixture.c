/* Actual candidate C, fake regmap/clock/membership, never a hardware adapter. */
#include <assert.h>
#include <string.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/mutex.h>
#include "cascadia-phy-power.h"
static struct cascadia_phy_power power;
static struct mutex lock;
static struct regmap maps[2];
static struct clk clock;
static int events[4096][3],num,io_count,fail_io,stale_io,stale_validate;
static int validation_count,epoch_error,clock_count,clock_change;
static int fault_clock;
static void event(int type,int a,int b)
{
	assert(num<4096);events[num][0]=type;events[num][1]=a;
	events[num++][2]=b;
}
static int check_io(void)
{
	assert(lock.held);io_count++;
	if (io_count==stale_io) epoch_error=-116;
	return io_count==fail_io ? -5 : 0;
}
int regmap_read(struct regmap *m,unsigned int off,unsigned int *value)
{
	int ret;assert(!(off&3) && off/4<2048);
	ret=check_io();if(ret)return ret;*value=m->values[off/4];return 0;
}
int regmap_write(struct regmap *m,unsigned int off,unsigned int value)
{
	int ret;assert(!(off&3) && off/4<2048);
	ret=check_io();if(ret)return ret;
	event(m->id,off,value);m->values[off/4]=value;return 0;
}
void udelay(unsigned long us){assert(lock.held);event(2,us,0);}
unsigned long clk_get_rate(struct clk *c)
{
	assert(c==&clock && lock.held);clock_count++;
	if(clock_count==clock_change)c->hz=fault_clock;
	return c->hz;
}
static int validate(void *ctx,u64 epoch)
{
	assert(ctx==&power && lock.held);validation_count++;
	if(validation_count==stale_validate)epoch_error=-116;
	return epoch!=7 ? -116 : epoch_error;
}
static int membership(void *ctx,u64 epoch,u32 mask)
{
	int ret;assert(ctx==&power && epoch==7 && lock.held);
	ret=check_io();if(ret)return ret;event(3,mask,0);return 0;
}
int start(int permit,unsigned int level,unsigned long hz)
{
	memset(&power,0,sizeof(power));memset(maps,0,sizeof(maps));
	mutex_init(&lock);maps[0].id=0;maps[1].id=1;clock.hz=hz;
	maps[0].values[0]=6;maps[0].values[4/4]=1;maps[0].values[8/4]=0x30;
	maps[0].values[0x40/4]=0x549;maps[0].values[0x44/4]=0x2ff3;
	maps[1].values[0]=0x800000e0;
	num=io_count=fail_io=stale_io=stale_validate=validation_count=epoch_error=clock_count=clock_change=fault_clock=0;
	int ret=cascadia_phy_power_init(&power,&maps[0],&maps[1],&clock,&lock);
	power.permitted=permit;power.p105_binding_verified=power.power_gates_absent_verified=power.complete_owned_membership=power.all_owners_quiesced=power.transition_lease_verified=power.state_adopted=1;
	power.level=level;power.adopted_clock_hz=level ? hz : 0;
	power.validate_epoch=validate;power.clock_membership=membership;power.context=&power;return ret;
}
int transition(unsigned long long epoch,unsigned int level)
{
	mutex_lock(&lock);num=0;int ret=cascadia_phy_power_transition(&power,epoch,level);mutex_unlock(&lock);return ret;
}
int inspect(int field,int i)
{
	assert(i>=0 && i<4096);
	switch(field){case 0:return num;case 1:return events[i][0];case 2:return events[i][1];case 3:return events[i][2];case 4:return power.level;case 5:return power.fault;case 6:return power.fault_error;case 7:return io_count;case 8:return validation_count;case 9:return power.touched;case 10:return clock_count;case 11:return power.pending_level;default:assert(0);return 0;}
}
void inject(int field,int value)
{
	switch(field){case 0:fail_io=value;break;case 1:stale_io=value;break;case 2:stale_validate=value;break;case 3:clock_change=value;break;case 4:fault_clock=value;break;case 5:power.p105_binding_verified=0;break;case 6:power.power_gates_absent_verified=0;break;case 7:power.complete_owned_membership=0;break;case 8:power.all_owners_quiesced=0;break;case 9:power.transition_lease_verified=0;break;case 10:power.state_adopted=0;break;case 11:power.validate_epoch=0;break;case 12:power.clock_membership=0;break;case 13:power.adopted_clock_hz=value;break;case 14:power.validate_epoch=validate;power.clock_membership=membership;break;default:assert(0);}
}
unsigned int regvalue(int map,unsigned int off){assert(map>=0 && map<2 && !(off&3) && off/4<2048);return maps[map].values[off/4];}
void seed(int map,unsigned int off,unsigned int value){assert(map>=0 && map<2 && !(off&3) && off/4<2048);maps[map].values[off/4]=value;}
int reinit(void){return cascadia_phy_power_init(&power,&maps[0],&maps[1],&clock,&lock);}
int bad_init(int which)
{
	struct cascadia_phy_power tmp={0};
	return cascadia_phy_power_init(&tmp,which==0?(void*)-5:&maps[0],which==1?(void*)-5:&maps[1],which==2?(void*)-5:&clock,which==3?0:&lock);
}
