/* Actual candidate with fake regmap/power-membership, never hardware. */
#include <assert.h>
#include <string.h>
#include <linux/regmap.h>
#include <linux/mutex.h>
#include "cascadia-reset-client.h"
static struct cascadia_reset_client c;
static struct mutex lock;
static struct regmap map;
static int events[1000][3],num,io_count,fail_io,stale_io,stale_validation;
static int validation_count,epoch_error,physical_lease,late_error,positive,allow_new_epoch;
static void event(int type,int a,int b){assert(num<1000);events[num][0]=type;events[num][1]=a;events[num++][2]=b;}
static int check_io(void){assert(lock.held);io_count++;if(io_count==stale_io)epoch_error=-116;return io_count==fail_io ? -5 : 0;}
int regmap_read(struct regmap *m,unsigned int off,unsigned int *value){assert(m==&map && off==8);int ret=check_io();if(ret)return ret;*value=map.values[2];return 0;}
int regmap_write(struct regmap *m,unsigned int off,unsigned int value){assert(m==&map && off==8);int ret=check_io();if(ret)return ret;map.values[2]=value;event(0,off,value);return 0;}
void udelay(unsigned long us){assert(lock.held);event(1,us,0);}
static int validate(void *ctx,u64 epoch){assert(ctx==&c && lock.held);validation_count++;if(validation_count==stale_validation)epoch_error=-116;return !allow_new_epoch && epoch!=7 ? -116 : epoch_error;}
static int power(void *ctx,u64 epoch,u32 mask){assert(ctx==&c && epoch==7 && lock.held && (mask==0 || mask==16));int ret=check_io();if(ret && !late_error)return ret;physical_lease=mask;event(2,mask,0);if(ret)return ret;return positive ? 1 : 0;}
int start(int permit,unsigned int reset){memset(&c,0,sizeof(c));memset(&map,0,sizeof(map));mutex_init(&lock);map.values[2]=reset;num=io_count=fail_io=stale_io=stale_validation=validation_count=epoch_error=physical_lease=late_error=positive=allow_new_epoch=0;int ret=cascadia_reset_client_init(&c,&map,&lock);c.permitted=permit;c.p105_binding_verified=c.complete_standard_group_owned=c.all_owners_quiesced=c.state_adopted=1;c.validate_epoch=validate;c.temporary_power_mask=power;c.context=&c;return ret;}
int update(unsigned long long epoch,unsigned int owner,unsigned int kind,int add){mutex_lock(&lock);num=0;int ret=cascadia_reset_client_update(&c,epoch,owner,kind,add);mutex_unlock(&lock);return ret;}
int inspect(int field,int i){assert(i>=0 && i<1000);switch(field){case 0:return num;case 1:return events[i][0];case 2:return events[i][1];case 3:return events[i][2];case 4:return c.memberships[i];case 5:return c.fault;case 6:return c.fault_error;case 7:return io_count;case 8:return validation_count;case 9:return c.lease_maybe_owned;case 10:return c.lease_known;case 11:return physical_lease;case 12:return c.pending_mask;default:assert(0);return 0;}}
void inject(int field,int value){switch(field){case 0:fail_io=value;break;case 1:stale_io=value;break;case 2:stale_validation=value;break;case 3:late_error=value;break;case 4:positive=value;break;case 5:c.p105_binding_verified=0;break;case 6:c.complete_standard_group_owned=0;break;case 7:c.all_owners_quiesced=0;break;case 8:c.state_adopted=0;break;case 9:c.validate_epoch=0;break;case 10:c.temporary_power_mask=0;break;case 11:allow_new_epoch=value;break;case 12:c.validate_epoch=validate;c.temporary_power_mask=power;break;default:assert(0);}}
int reinit(void){return cascadia_reset_client_init(&c,&map,&lock);}
unsigned int value(void){return map.values[2];}
