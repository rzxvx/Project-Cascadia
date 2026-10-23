/* Actual complete production C, with fake kernel primitives/regmap only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cascadia-ths-idbus.c"
struct operation { unsigned reg; size_t n; unsigned char bytes[32]; int read; };
struct fake_bus {
	struct mutex lock;
	struct regmap map;
	struct cascadia_ths_idbus ctx;
	struct operation op[16]; int count, fail_at, stale_at, denied, validate_count, invalidate_at, flip_epoch_at, flip_owner_at, positive_at;
	unsigned status; unsigned char raw[32];
};
static int validate(void *cookie, u64 owner, u64 epoch)
{
	struct fake_bus *b=cookie;
	assert(b->lock.locked && owner==b->ctx.owner && epoch==b->ctx.epoch);
	b->validate_count++;
	if (b->validate_count==b->invalidate_at)assert(!cascadia_ths_invalidate(&b->ctx));
	if (b->validate_count==b->flip_epoch_at)b->ctx.epoch++;
	if (b->validate_count==b->flip_owner_at)b->ctx.owner++;
	if (b->validate_count==b->positive_at)return 1;
	return b->denied || b->count==b->stale_at ? -EACCES : 0;
}
static void setup(struct fake_bus *b)
{
	memset(b,0,sizeof(*b)); pthread_mutex_init(&b->lock.value,NULL);
	b->map.cookie=b; b->map.val_bytes=1; b->fail_at=-1; b->stale_at=-1; b->status=0x10;
	struct cascadia_ths_lease l={validate,b};
	assert(!cascadia_ths_init(&b->ctx,&b->map,&b->lock,&l,9));
}
static int operation(struct regmap *map,unsigned reg,void *data,size_t n,int read)
{
	struct fake_bus *b=map->cookie; assert(b->lock.locked && n && n<=32 && b->count<16);
	struct operation *o=&b->op[b->count++]; o->reg=reg;o->n=n;o->read=read;
	if(read){ if(reg==0x0d){assert(n==1);*(unsigned char*)data=b->status;}else memcpy(data,b->raw,n); }
	memcpy(o->bytes,data,n); return b->count-1==b->fail_at ? -EIO : 0;
}
int regmap_bulk_write(struct regmap *m,unsigned reg,const void *data,size_t n)
{ return operation(m,reg,(void*)data,n,0); }
int regmap_write(struct regmap *m,unsigned reg,unsigned val)
{ assert(val<=255);unsigned char v=val;return operation(m,reg,&v,1,0); }
int regmap_read(struct regmap *m,unsigned reg,unsigned *val)
{ unsigned char v;int e=operation(m,reg,&v,1,1);*val=v;return e; }
int regmap_bulk_read(struct regmap *m,unsigned reg,void *data,size_t n)
{ return operation(m,reg,data,n,1); }
static void zero(const unsigned char *p,size_t n){for(size_t i=0;i<n;i++)assert(!p[i]);}
static void native_submissions(void)
{
	for(unsigned t=1;t<=32;t++)for(unsigned r=1;r<=32;r++){
		struct fake_bus b;setup(&b);unsigned char tx[32],out[32];u64 id=0;size_t n=0;
		for(unsigned i=0;i<32;i++){tx[i]=i;b.raw[i]=31-i;}
		assert(!cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,t,r,&id));
		assert(id && id!=b.ctx.owner && b.count==3 && b.ctx.pending_rx==r);
		assert(b.op[0].reg==0x20 && b.op[0].n==t && !memcmp(b.op[0].bytes,tx,t));
		assert(b.op[1].reg==0x41 && b.op[1].bytes[0]==2*(r-1));
		assert(b.op[2].reg==0x40 && b.op[2].bytes[0]==2*t-1);
		assert(!cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n));
		assert(n==r && b.op[3].reg==0x0d && b.op[4].reg==0x40-r);
		for(unsigned i=0;i<r;i++)assert(out[i]==b.raw[r-i-1]);
		zero(out+r,32-r);
		assert(b.ctx.state==CASCADIA_THS_IDLE && !b.ctx.pending_rx);
		assert(cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n)==-ESTALE && b.count==5 && !n);
		pthread_mutex_destroy(&b.lock.value);
	}
}
static void errors(void)
{
	for(int fail=0;fail<5;fail++){
		struct fake_bus b;setup(&b);b.fail_at=fail;unsigned char tx[21]={0x94},out[32];u64 id=0;size_t n=0;
		int e=cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,21,3,&id);
		if(fail>=3){assert(!e);e=cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n);zero(out,32);assert(!n);}
		assert(e==-EIO && b.ctx.state==CASCADIA_THS_UNKNOWN && b.ctx.pending_rx==3 && b.ctx.request);
		int count=b.count;b.fail_at=-1;
		assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,21,3,&id)==-EIO && !id && b.count==count);
		assert(cascadia_ths_receive(&b.ctx,b.ctx.owner,9,b.ctx.request,out,32,&n)==-EIO && b.count==count);
		zero(out,32); assert(!n);
	}
	for(int stale=1;stale<=5;stale++){
		struct fake_bus b;setup(&b);b.stale_at=stale;unsigned char tx[21]={0x94},out[32];u64 id=0;size_t n=0;
		int e=cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,21,3,&id);
		if(stale>=4){assert(!e);e=cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n);zero(out,32);assert(!n);}
		assert(e==-EACCES && b.count==stale && b.ctx.state==CASCADIA_THS_UNKNOWN && b.ctx.pending_rx==3);
		b.stale_at=-1;int count=b.count;assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,21,3,&id)==-EACCES && b.count==count);
	}
}
static void admission(void)
{
	struct fake_bus b;setup(&b);unsigned char tx[32]={0},out[32];u64 id=0;size_t n=0;
	struct cascadia_ths_idbus fresh={0},snap=b.ctx;
	assert(cascadia_ths_init(&b.ctx,&b.map,&b.lock,NULL,8)==-EALREADY);assert(!memcmp(&snap,&b.ctx,sizeof(snap)));
	assert(cascadia_ths_init(&fresh,ERR_PTR(-5),&b.lock,NULL,9)==-EINVAL);
	assert(cascadia_ths_init(&fresh,&b.map,ERR_PTR(-5),NULL,9)==-EINVAL);
	assert(cascadia_ths_init(&fresh,&b.map,&b.lock,ERR_PTR(-5),9)==-EINVAL);
	b.map.val_bytes=2;assert(cascadia_ths_init(&fresh,&b.map,&b.lock,NULL,9)==-EINVAL);b.map.val_bytes=1;
	b.ctx.lease.validate=NULL;assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,1,&id)==-EACCES && !b.count);
	b.ctx.lease.validate=validate;b.denied=1;assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,1,&id)==-EACCES && !b.count);b.denied=0;
	for(unsigned t=0;t<=33;t+=33)assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,t,1,&id)==-EINVAL && !b.count);
	for(unsigned r=0;r<=33;r+=33)assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,r,&id)==-EINVAL && !b.count);
	assert(cascadia_ths_submit(&b.ctx,b.ctx.owner+1,9,tx,1,1,&id)==-ESTALE && !b.count);
	assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,8,tx,1,1,&id)==-ESTALE && !b.count);
	assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,1,&b.ctx.owner)==-EINVAL);assert(!memcmp(&snap,&b.ctx,sizeof(snap)));
	assert(!cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,3,&id));u64 owned=id;
	assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,3,&id)==-EBUSY && !id && b.count==3);
	assert(cascadia_ths_receive(&b.ctx,b.ctx.owner,9,owned+1,out,32,&n)==-ESTALE && b.count==3);
	assert(cascadia_ths_receive(&b.ctx,b.ctx.owner,9,owned,out,2,&n)==-ENOSPC && !n && b.count==3);
	b.status=0;assert(cascadia_ths_receive(&b.ctx,b.ctx.owner,9,owned,out,32,&n)==-EAGAIN && !n && b.count==4 && b.ctx.state==CASCADIA_THS_PENDING);
	b.status=0x10;assert(!cascadia_ths_receive(&b.ctx,b.ctx.owner,9,owned,out,32,&n));assert(n==3);
	assert(!cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,3,&id));assert(id>owned);
	int count=b.count;assert(cascadia_ths_receive(&b.ctx,b.ctx.owner,9,owned,out,32,&n)==-ESTALE && b.count==count);
}
static void dirty_storage(void)
{
 struct fake_bus b;setup(&b);struct cascadia_ths_idbus dirty={0},saved;u8 tx[1]={0},out[32];u64 id;size_t n;
 dirty.touched=true;dirty.request=99;dirty.pending_rx=3;dirty.state=CASCADIA_THS_UNKNOWN;dirty.error=-EIO;atomic_set(&dirty.revoked,1);
 saved=dirty;assert(cascadia_ths_init(&dirty,&b.map,&b.lock,NULL,9)==-EALREADY);assert(!memcmp(&dirty,&saved,sizeof(dirty)));
 assert(cascadia_ths_init(ERR_PTR(-5),&b.map,&b.lock,NULL,9)==-EINVAL);
 assert(cascadia_ths_invalidate(ERR_PTR(-5))==-EINVAL);
 assert(cascadia_ths_submit(ERR_PTR(-5),1,9,tx,1,1,&id)==-EINVAL && !id);
 assert(cascadia_ths_receive(ERR_PTR(-5),1,9,1,out,32,&n)==-EINVAL && !n);zero(out,32);
}
static void revoked_after_completion(void)
{
 struct fake_bus b;setup(&b);unsigned char tx[1]={0},out[32];u64 id;size_t n;
 assert(!cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,1,&id));
 assert(!cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n));
 int count=b.count;b.ctx.lease.validate=NULL;
 assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,1,&id)==-EACCES && !id && b.ctx.state==CASCADIA_THS_UNKNOWN);
 b.ctx.lease.validate=validate;
 assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,1,&id)==-EACCES && b.count==count);
}
static u64 concurrent_ids[2048];
static void *allocate_tokens(void *p)
{
 size_t row=(size_t)p;
 for(size_t i=0;i<256;i++)assert(!ths_token(&concurrent_ids[row*256+i]));
 return NULL;
}
static int compare_token(const void *a,const void *b)
{u64 x=*(const u64*)a,y=*(const u64*)b;return(x>y)-(x<y);}
static void concurrent_tokens(void)
{
 pthread_t threads[8];
 for(size_t i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,allocate_tokens,(void*)i));
 for(size_t i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
 qsort(concurrent_ids,2048,sizeof(u64),compare_token);
 const u64 *p=concurrent_ids;
 for(size_t i=1;i<2048;i++)assert(p[i]>p[i-1]);
}
static void validator_spanning(void)
{
 for(int mode=0;mode<4;mode++)for(int call=1;call<=7;call++){
  struct fake_bus b;setup(&b);unsigned char tx[1]={0},out[32];u64 id=0;size_t n=0;
  if(mode==0)b.invalidate_at=call;
  if(mode==1)b.flip_epoch_at=call;
  if(mode==2)b.flip_owner_at=call;
  if(mode==3)b.positive_at=call;
  int e=cascadia_ths_submit(&b.ctx,b.ctx.owner,9,tx,1,3,&id);
  if(call>4){assert(!e);e=cascadia_ths_receive(&b.ctx,b.ctx.owner,9,id,out,32,&n);zero(out,32);assert(!n);}
  assert(e==(mode==3?-EPROTO:-ESTALE));
  assert(b.ctx.state==CASCADIA_THS_UNKNOWN || (mode==3 && call==1 && !b.ctx.touched));
  if(mode!=3 || call>1){int count=b.count;b.invalidate_at=b.flip_epoch_at=b.flip_owner_at=b.positive_at=0;
   assert(cascadia_ths_submit(&b.ctx,b.ctx.owner,b.ctx.epoch,tx,1,3,&id)==e && b.count==count && !id);}
 }
}
/* Generated exact original 32/32 AuthCPAID frames. */
#include "native-frames.inc"
static void tokens(void)
{
	struct fake_bus a,b;setup(&a);setup(&b);assert(a.ctx.owner!=b.ctx.owner);
	atomic64_set(&ths_tokens,0x7fffffffffffffffLL);
	unsigned char tx[1]={0};u64 id=99;
	assert(cascadia_ths_submit(&a.ctx,a.ctx.owner,9,tx,1,1,&id)==-EOVERFLOW && !id && !a.count);
	struct cascadia_ths_idbus fresh={0};assert(cascadia_ths_init(&fresh,&a.map,&a.lock,NULL,9)==-EOVERFLOW && !fresh.initialized);
}
int main(void){native_submissions();errors();admission();dirty_storage();revoked_after_completion();validator_spanning();native_frames();concurrent_tokens();tokens();puts("THS actual entire C: 1024 native length pairs, 17 actual32/32 auth frames, bus/epoch/admission/identity/overflow controls PASS");return 0;}
