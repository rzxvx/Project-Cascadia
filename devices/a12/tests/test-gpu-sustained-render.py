#!/usr/bin/env python3
"""Compile shipped J171 queue/stamp code; exercise 20000 renders and wrap.
Host-only simulation, not a firmware or hardware endurance result.
"""
from pathlib import Path
import subprocess,tempfile
a12=Path(__file__).resolve().parents[1]
# Reconstruct the shipped source, not an ignored/local kernel build.
source_root=tempfile.TemporaryDirectory(prefix='j171-render-test-')
d=Path(source_root.name)
patch=(a12/'linux/patches/linux-0020-agx.patch').read_text()
for block in patch.split('diff --git ')[1:]:
 header=block.splitlines()[0]
 if ' b/drivers/gpu/drm/agx/' not in header:continue
 name=header.split(' b/drivers/gpu/drm/agx/',1)[1]
 text='\n'.join(line[1:] for line in block.splitlines() if line.startswith('+') and not line.startswith('+++'))+'\n'
 (d/name).write_text(text)
s=(d/'agx_21b91_transport.c').read_text()
assert 'agx21_render_jobs' not in s
enq=s[s.index('static int agx21_render_enqueue('):s.index('/* A13 measured a fault')]
sta=s[s.index('int agx21_render_next_stamp('):s.index('int agx21_asahi_render(')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "agx_21b91_memory.h"
#include "agx_21b91_queue.h"
#define READ_ONCE(x) (x)
#define atomic_read(p) (*(p))
static u64 rd(const void *p,unsigned n){u64 x=0;const u8*b=p;for(unsigned i=0;i<n;i++)x|=(u64)b[i]<<(i*8);return x;}
static void wr(void*p,u64 x,unsigned n){u8*b=p;for(unsigned i=0;i<n;i++)b[i]=x>>(i*8);}
#define get_unaligned_le32(p) rd(p,4)
#define put_unaligned_le32(x,p) wr(p,x,4)
#define put_unaligned_le64(x,p) wr(p,x,8)
struct agx21_protocol {struct {void*cookie;void(*release)(void*);int(*send)(void*,u8,u64);}io;};
struct owner{struct agx21_memory memory;};
struct agx21_transport{struct owner*owner;struct agx21_protocol asahi_protocol;u32 render_completed;bool render_failed;int crashed;};
static unsigned sends;static void release(void*p){(void)p;}
static int send(void*p,u8 ep,u64 value){(void)p;assert(ep==0x21&&value>>48==0x83);sends++;return 0;}
'''+enq+sta+r'''
int main(void){
 struct owner*o=calloc(1,sizeof(*o));struct agx21_memory*m=&o->memory;
 u8*a=calloc(1,AGX21_ARENA_BYTES),*i=calloc(1,AGX21_PAGE),*t=calloc(1,AGX21_PAGE),*b=calloc(1,AGX21_PAGE);
 assert(!agx21_memory_prepare(m,a,0x810000000ULL,i,0x820000000ULL,t,0x820004000ULL,b,0x820008000ULL));
 struct agx21_render_staging ids;assert(!agx21_memory_reserve_render_staging(m,&ids));assert(!agx21_queue_initialize_render_staging(m,&ids));
 struct agx21_transport tr={.owner=o};tr.asahi_protocol.io.release=release;tr.asahi_protocol.io.send=send;
 for(unsigned job=0;job<20000;job++){
  u32 stamp=0;assert(!agx21_render_next_stamp(&tr,&stamp));assert(stamp==(job+1)*0x100);
  for(unsigned ch=0;ch<2;ch++){
   unsigned count=ch?1:2,head=((job+1)*count)%256;const unsigned*q=ids.queue[ch];
   u8*state=agx21_memory_cpu(m,AGX21_MEM_INPUT_STATE_BASE+ch),*info=agx21_memory_cpu(m,q[0]),*ptr=agx21_memory_cpu(m,q[1]);
   assert(!agx21_render_enqueue(&tr,ch,0x10000,0x20000,ch?7:4));
   assert(rd(state+0x20,4)==(job+1)%256&&rd(ptr+0x40,4)==head);
   u8*ring=agx21_memory_cpu(m,AGX21_MEM_INPUT_RING_BASE+ch)+(job%256)*AGX21_RUN_RECORD_SIZE;
   assert(rd(ring+0x14,2)==head);
   if(job){u8*prev=agx21_memory_cpu(m,AGX21_MEM_INPUT_RING_BASE+ch)+((job-1)%256)*AGX21_RUN_RECORD_SIZE;assert(rd(prev+0x14,2)==(head+256-count)%256);}
   unsigned before=sends;assert(agx21_render_enqueue(&tr,ch,0x10000,0x20000,ch?7:4));assert(sends==before);
   wr(state,(job+1)%256,4);wr(state+4,(job+1)%256,4);wr(info+0x20,head,4);wr(info+0x28,head,4);wr(ptr,head,4);wr(ptr+0x30,head,4);
  }
  tr.render_completed++;
 }
 assert(sends==40000);
 u32 next=0;assert(!agx21_render_next_stamp(&tr,&next));assert(next==20001U*0x100U);
 tr.render_failed=true;assert(agx21_render_next_stamp(&tr,&next)==-EIO);tr.render_failed=false;
 tr.crashed=1;assert(agx21_render_next_stamp(&tr,&next)==-EIO);tr.crashed=0;
 assert(agx21_render_next_stamp(NULL,&next)==-EINVAL);
 assert(agx21_render_next_stamp(&tr,NULL)==-EINVAL);
 tr.render_completed=0x00fffffeU;assert(!agx21_render_next_stamp(&tr,&next));assert(next==0xffffff00U);
 tr.render_completed++;assert(!agx21_render_next_stamp(&tr,&next));assert(next==0);
 tr.render_completed++;assert(!agx21_render_next_stamp(&tr,&next));assert(next==0x100);
 tr.render_completed=0xfffffffeU;assert(!agx21_render_next_stamp(&tr,&next));assert(next==0xffffff00U);
 tr.render_completed++;assert(!agx21_render_next_stamp(&tr,&next));assert(next==0);
 tr.render_completed++;assert(!agx21_render_next_stamp(&tr,&next));assert(next==0x100);
 puts("PASS: actual shipped C, 20000 jobs/40000 doorbells; ring reuse/retirement guards, no 4096 cap, zero-stamp/u32 wrap and fail-closed checks");
 free(a);free(i);free(t);free(b);free(o);
}
'''
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'t.c').write_text(code)
 files=['agx_21b91_memory.c','agx_21b91_queue.c','agx_21b91_init.c','agx_21b91_hwdata.c','agx-21b91-layout.c']
 subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DAGX_ABI_HOST_TEST','-I'+str(d),str(p/'t.c'),*[str(d/f) for f in files],'-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t')],check=True)

source_root.cleanup()
