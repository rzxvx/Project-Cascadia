#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
#include "cascadia-accessory-role.h"
struct fake_policy {
 unsigned int authorizations, switches;
 int auth_status, switch_status;
 u16 value, index;
 u64 epoch, current_epoch;
 bool detach_after_authorize;
 u8 copied_setup[8];
};
static int authorize(void *opaque, u64 epoch, u16 value, u16 index)
{
 struct fake_policy *p = opaque;
 p->authorizations++;
 p->epoch = epoch;
 if (epoch != p->current_epoch)
  return -ESTALE;
 if (p->detach_after_authorize)
  p->current_epoch++;
 p->value = value; p->index = index;
 return p->auth_status;
}
static int request_switch(void *opaque, u64 epoch, const u8 setup[8])
{
 struct fake_policy *p = opaque;
 assert(epoch==p->epoch);
 if (epoch != p->current_epoch)
  return -ESTALE;
 p->switches++;
 memcpy(p->copied_setup, setup, 8);
 return p->switch_status;
}
int main(void)
{
 const u8 query[8]={0xc0,0x53,0,0,0,0,4,0};
 const u8 set[8]={0x40,0x51,0x34,0,0x78,0,0,0};
 const u8 rejected_value[8]={0x40,0x51,0,0x80,0x78,0,0,0};
 const u8 rejected_index[8]={0x40,0x51,0x34,0,0,0x80,0,0};
 const u8 short_query[8]={0xc0,0x53,0,0,0,0,3,0};
 const u8 data_set[8]={0x40,0x51,0x34,0,0x78,0,1,0};
 const u8 zero_set[8]={0x40,0x51,0,0,0,0,0,0};
 const u8 swapped_query[8]={0xc0,0x53,0,0,0,0,0,4};
 const u8 nonzero_query[8]={0xc0,0x53,1,0,0,0,4,0};
 const u8 wrong_recipient[8]={0xc1,0x53,0,0,0,0,4,0};
 u8 reply[4]={0xa5,0xa5,0xa5,0xa5};
 struct fake_policy fake={.current_epoch=42};
 struct cascadia_accessory_role_policy policy={0x1234,0x5678,true,42,&fake,authorize,request_switch};
 struct cascadia_accessory_role_policy denied={0};
 assert(cascadia_accessory_role_setup(query,8,&policy,reply)==4);
 assert(!memcmp(reply,(u8[]){0x34,0x12,0x78,0x56},4));
 assert(fake.authorizations==0 && fake.switches==0);
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==0);
 assert(fake.authorizations==1 && fake.switches==1);
 assert(fake.epoch==42 && fake.value==0x34 && fake.index==0x78 && !memcmp(fake.copied_setup,set,8));
 assert(cascadia_accessory_role_setup(rejected_value,8,&policy,reply)==-EINVAL);
 assert(cascadia_accessory_role_setup(rejected_index,8,&policy,reply)==-EINVAL);
 assert(cascadia_accessory_role_setup(short_query,8,&policy,reply)==-EOPNOTSUPP);
 assert(cascadia_accessory_role_setup(data_set,8,&policy,reply)==-EOPNOTSUPP);
 assert(fake.authorizations==1 && fake.switches==1);
 fake.auth_status=-EIO;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-EIO);
 assert(fake.authorizations==2 && fake.switches==1);
 fake.auth_status=1;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-EACCES);
 assert(fake.switches==1);
 fake.auth_status=0;fake.switch_status=-EIO;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-EIO);
 assert(fake.switches==2);
 fake.switch_status=1;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-EIO);
 assert(fake.switches==3);
 fake.current_epoch=43;fake.switch_status=0;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-ESTALE);
 assert(fake.switches==3);
 fake.current_epoch=42;fake.detach_after_authorize=true;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-ESTALE);
 assert(fake.switches==3);
 fake.detach_after_authorize=false;fake.current_epoch=42;
 assert(cascadia_accessory_role_setup(query,8,&denied,reply)==4);
 assert(!memcmp(reply,(u8[]){0,0,0,0},4));
 assert(cascadia_accessory_role_setup(zero_set,8,&denied,reply)==-EACCES);
 policy.authorize=NULL;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-EACCES);
 policy.authorize=authorize;policy.request_switch=NULL;
 assert(cascadia_accessory_role_setup(set,8,&policy,reply)==-EACCES);
 assert(cascadia_accessory_role_setup(swapped_query,8,&policy,reply)==-EOPNOTSUPP);
 assert(cascadia_accessory_role_setup(nonzero_query,8,&policy,reply)==-EOPNOTSUPP);
 assert(cascadia_accessory_role_setup(wrong_recipient,8,&policy,reply)==-EOPNOTSUPP);
 assert(cascadia_accessory_role_setup(query,7,&policy,reply)==-EINVAL);
 assert(cascadia_accessory_role_setup(query,9,&policy,reply)==-EINVAL);
 assert(cascadia_accessory_role_setup(NULL,8,&policy,reply)==-EINVAL);
 assert(cascadia_accessory_role_setup(query,8,NULL,reply)==-EINVAL);
 assert(cascadia_accessory_role_setup(query,8,&policy,NULL)==-EINVAL);
 puts("24 bounded C transport/policy cases PASS; no I/O backend");
}
