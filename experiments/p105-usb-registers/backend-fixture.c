/* Exact candidate C with bounded fake Regmap/mutex/epoch; no device backend. */
#include <assert.h>
#include <string.h>
#include <linux/regmap.h>
#include "cascadia-usb-registers.h"
struct cascadia_usb_registers backend;
struct regmap maps[3];
int events[4096][3], event_count, io_count, fail_io, stale_io, stale_validate;
int validate_count, epoch_error, reads, writes, stuck;
static int event(int kind,int a,int b)
{
	assert(event_count<4096);
	events[event_count][0]=kind;events[event_count][1]=a;
	events[event_count++][2]=b;return 0;
}
static int check_io(void)
{
	assert(backend.lock.held);io_count++;
	if (io_count==stale_io) epoch_error=-116;
	return io_count==fail_io ? -5 : 0;
}
int regmap_read(struct regmap *m,unsigned int off,unsigned int *value)
{
	int ret;assert(!(off&3) && off/4<2048);reads++;
	ret=check_io();if(ret)return ret;*value=m->values[off/4];return 0;
}
int regmap_write(struct regmap *m,unsigned int off,unsigned int value)
{
	int ret;assert(!(off&3) && off/4<2048);writes++;
	ret=check_io();if(ret)return ret;event(m->id,off,value);
	m->values[off/4]=value;
	if(m->id==1 && !stuck)
		m->values[off/4]=(value&~0xf0U)|((value&15)<<4);
	return 0;
}
void udelay(unsigned long usecs)
{
	assert(backend.lock.held);event(3,usecs,0);
}
static int validate(void *ctx,unsigned int owner,u64 epoch)
{
	assert(ctx==&backend && owner<16 && backend.lock.held);
	validate_count++;if(validate_count==stale_validate)epoch_error=-116;
	return epoch!=7 ? -116 : epoch_error;
}
int start(int permission,int complete,int release)
{
	memset(&backend,0,sizeof(backend));memset(maps,0,sizeof(maps));
	for(int i=0;i<3;i++)maps[i].id=i;
	maps[0].values[0]=0x2460;
	for(int off=0x1084;off<=0x1094;off+=4)maps[1].values[off/4]=0x300;
	maps[2].values[8/4]=0x30;maps[2].values[0x64/4]=0xa400;
	event_count=io_count=fail_io=stale_io=stale_validate=validate_count=epoch_error=reads=writes=stuck=0;
	int ret=cascadia_usb_registers_init(&backend,&maps[0],&maps[1],&maps[2]);
	backend.permitted=permission;backend.complete_clock_ownership=complete;
	backend.gate_release_verified=release;backend.validate_epoch=validate;
	backend.context=&backend;return ret;
}
int update(unsigned int owner,u64 epoch,unsigned int mask)
{
	event_count=0;return cascadia_usb_clock_update(&backend,owner,epoch,mask);
}
int tune(unsigned int owner,u64 epoch,int host,unsigned int level,int hsic,int quiesced,int lease)
{
	event_count=0;return cascadia_usb_host_tune(&backend,owner,epoch,host,level,hsic,quiesced,lease);
}
int inspect(int field,int index)
{
	assert(index>=0 && index<4096);
	switch(field){
	case 0:return event_count;case 1:return events[index][0];
	case 2:return events[index][1];case 3:return events[index][2];
	case 4:return backend.masks[index];case 5:return backend.fault;
	case 6:return io_count;case 7:return validate_count;case 8:return reads;
	case 9:return writes;case 10:return backend.pending_mask;
	case 11:return backend.touched;case 12:return backend.fault_error;
	default:assert(0);return 0;}
}
void inject(int field,int value)
{
	switch(field){case 0:fail_io=value;break;case 1:stale_io=value;break;
	case 2:stale_validate=value;break;case 3:stuck=value;break;
	case 4:backend.validate_epoch=0;break;case 5:epoch_error=value;break;default:assert(0);}
}
int reinit(void){return cascadia_usb_registers_init(&backend,&maps[0],&maps[1],&maps[2]);}
void seed(int map,unsigned int off,unsigned int value)
{
	assert(map>=0 && map<3 && !(off&3) && off/4<2048);maps[map].values[off/4]=value;
}
unsigned int regvalue(int map,unsigned int off)
{
	assert(map>=0 && map<3 && !(off&3) && off/4<2048);return maps[map].values[off/4];
}

int bad_init(int index, int error_pointer)
{
	struct cascadia_usb_registers fresh = {0};
	struct regmap *args[3] = { &maps[0], &maps[1], &maps[2] };
	assert(index >= 0 && index < 3);
	args[index] = error_pointer ? (struct regmap *)(long)-5 : 0;
	int ret = cascadia_usb_registers_init(&fresh, args[0], args[1], args[2]);
	assert(!fresh.initialized);
	return ret;
}
