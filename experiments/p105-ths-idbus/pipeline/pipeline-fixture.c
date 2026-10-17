/* SPDX-License-Identifier: GPL-2.0 */
/* Actual transport/parser linked unchanged; synthetic regmap only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <linux/errno.h>
#include "cascadia-ths-idbus.h"
#include "cascadia-authcp-aid.h"
struct wire_frame { u8 tx[32],rx[32];size_t tx_len,rx_len; };
struct fake_bus {
 struct mutex lock;struct regmap map;struct cascadia_ths_idbus transport;
 u8 reply[32];size_t reply_len;unsigned writes,reads;
};
static int lease(void *cookie,u64 owner,u64 epoch)
{struct fake_bus *b=cookie;assert(b->lock.locked);return owner==b->transport.owner&&epoch==19?0:-ESTALE;}
int regmap_bulk_write(struct regmap *m,unsigned reg,const void *buf,size_t n)
{struct fake_bus *b=m->cookie;assert(b->lock.locked&&reg==0x20&&n>=1&&n<=32&&buf);b->writes++;return 0;}
int regmap_write(struct regmap *m,unsigned reg,unsigned value)
{struct fake_bus *b=m->cookie;assert(b->lock.locked&&(reg==0x40||reg==0x41)&&value<=63);b->writes++;return 0;}
int regmap_read(struct regmap *m,unsigned reg,unsigned *v)
{struct fake_bus *b=m->cookie;assert(b->lock.locked&&reg==0x0d);*v=0x10;b->reads++;return 0;}
int regmap_bulk_read(struct regmap *m,unsigned reg,void *buf,size_t n)
{struct fake_bus *b=m->cookie;assert(b->lock.locked&&reg==0x40-n&&n==b->reply_len);for(size_t i=0;i<n;i++)((u8*)buf)[i]=b->reply[n-i-1];b->reads++;return 0;}
static void run(const struct wire_frame *frames,size_t count,u8 version,bool late,bool stop)
{
 struct fake_bus b={0};struct cascadia_aid_exchange a={0};struct cascadia_aid_request q;
 struct cascadia_ths_lease l={lease,&b};u8 nonce[32],reply[32],signature[128],old[32];size_t n,len;u64 id,last=0;bool have_old=false,substituted=false;
 pthread_mutex_init(&b.lock.value,NULL);b.map.cookie=&b;b.map.val_bytes=1;
 assert(!cascadia_ths_init(&b.transport,&b.map,&b.lock,&l,19));
 for(size_t i=0;i<32;i++)nonce[i]=i;
 assert(!cascadia_aid_init(&a,19,true,version,32,32,nonce,version==3?32:20));
 for(size_t i=0;i<count;i++){
  assert(!cascadia_aid_plan(&a,19,&q));assert(q.tx_len==frames[i].tx_len&&q.rx_len==frames[i].rx_len);
  assert(!memcmp(q.tx,frames[i].tx,q.tx_len));
  assert(!cascadia_ths_submit(&b.transport,b.transport.owner,19,q.tx,q.tx_len,q.rx_len,&id));
  assert(id>last);last=id;
  if(stop && i==1){
   assert(!cascadia_ths_invalidate(&b.transport));assert(cascadia_ths_receive(&b.transport,b.transport.owner,19,id,reply,32,&n)==-ESTALE&&!n);
   assert(cascadia_aid_complete(&a,19,-ESTALE,NULL,0)==-ESTALE);
   assert(cascadia_aid_copy_signature(&a,19,signature,128,&len)==-ESTALE&&!len);
   unsigned accesses=b.reads+b.writes;assert(cascadia_ths_submit(&b.transport,b.transport.owner,19,q.tx,q.tx_len,q.rx_len,&id)==-ESTALE&&!id&&b.reads+b.writes==accesses);
   puts("timeout/invalidation stops the chain; no retry, no signature output");return;
  }
  b.reply_len=frames[i].rx_len;memcpy(b.reply,frames[i].rx,b.reply_len);
  if(late&&q.tx[0]==0x98&&q.rx_len==32){
   if(have_old&&!substituted){memcpy(b.reply,old,32);substituted=true;}
   else if(!have_old){memcpy(old,b.reply,32);have_old=true;}
  }
  assert(!cascadia_ths_receive(&b.transport,b.transport.owner,19,id,reply,32,&n));
  assert(!cascadia_aid_complete(&a,19,0,reply,n));
 }
 assert(!cascadia_aid_copy_signature(&a,19,signature,128,&len));assert(len==(version==3?64:128));
 if(late){assert(substituted);assert(!memcmp(signature,signature+31,31));assert(signature[31]==0);printf("v%u: late old99/31bytes accepted under newer unique request; parser completes UNVERIFIED incorrect signature\n",version);}
 else{for(size_t i=0;i<len;i++)assert(signature[i]==i);printf("v%u: native32/32 pipeline exact unverified signature PASS\n",version);}
}
#include "frames.inc"
int main(void){run(v2,sizeof(v2)/sizeof(v2[0]),2,false,false);run(v3,sizeof(v3)/sizeof(v3[0]),3,false,false);run(v2,sizeof(v2)/sizeof(v2[0]),2,true,false);run(v3,sizeof(v3)/sizeof(v3[0]),3,true,false);run(v3,sizeof(v3)/sizeof(v3[0]),3,false,true);return 0;}
