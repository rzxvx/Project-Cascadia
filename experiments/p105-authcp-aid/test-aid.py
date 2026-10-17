"""Build actual staged C against synthetic pinned original-method oracle."""
from pathlib import Path
import hashlib,json,os,subprocess,tempfile
root=Path(__file__).resolve().parent
oracle=root/'authcp-aid13-oracle.json'
assert hashlib.sha256(oracle.read_bytes()).hexdigest()=='479ef0c6eca1558fb3da68989efd052058f3cf7c8794003a4135ceb74bfe7000'
r=json.loads(oracle.read_text());assert r['source_sha256']=='8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125'
ths=root/'authcp-ths-window13-oracle.json'
assert hashlib.sha256(ths.read_bytes()).hexdigest()=='78b0091f7f704676495e3121d378eba9d6abfe8234318f686af8aacb37e48e75'
r['cases']+=json.loads(ths.read_text())['cases']
# Two additional source-backed P10532/32-window sequences, no real I2C.
headers=[];runs=[]
def arr(bs):return '{'+','.join(hex(b)for b in bs)+'}'
for i,row in enumerate(r['cases']):
 for j,t in enumerate(row['transfers']):
  headers.append(f'static const u8 tx{i}_{j}[]='+arr(bytes.fromhex(t['tx']))+';')
  headers.append(f'static const u8 rx{i}_{j}[]='+arr(bytes.fromhex(t['rx']))+';')
 if i in range(5,9):
  runs.append(f'{{ struct cascadia_aid_exchange c={{0}}; u8 n[32]={{0}}; assert(cascadia_aid_init(&c,7,true,{row["version"]},{row["max_tx"]},{row["max_rx"]},n,{row["nonce_length"]})==-EINVAL || {i}==5); }}')
  # Output capacity64 is a userclient boundary absent from the narrow C init.
  if i==5:runs.pop()
  continue
 lines=[f'{{ struct cascadia_aid_exchange c={{0}}; struct cascadia_aid_request q; u8 n[32],out[128]; size_t len=99; int e=0; unsigned j; for(j=0;j<32;j++)n[j]=j;',f'assert(cascadia_aid_init(&c,7,true,{row["version"]},{row["max_tx"]},{row["max_rx"]},n,{row["nonce_length"]})==0);']
 for j,t in enumerate(row['transfers']):
  lines.append(f'if(!e){{ assert(cascadia_aid_plan(&c,7,&q)==0); assert(q.tx_len==sizeof(tx{i}_{j}) && q.rx_len==sizeof(rx{i}_{j})); assert(!memcmp(q.tx,tx{i}_{j},q.tx_len)); e=cascadia_aid_complete(&c,7,{"-EIO" if t["error"]!="0x0" else "0"},rx{i}_{j},sizeof(rx{i}_{j})); }}')
 if row['return']=='0x0':
  lines +=[f'assert(!e); memset(out,0xcc,sizeof(out)); assert(cascadia_aid_copy_signature(&c,7,out,sizeof(out),&len)==0); assert(len=={row["output_size"]}); for(j=0;j<len;j++)assert(out[j]==j); for(;j<128;j++)assert(out[j]==0); assert(cascadia_aid_plan(&c,7,&q)==-EALREADY);']
 else:
  lines+=['assert(e<0 && c.fault && c.touched && c.waiting); memset(out,0xcc,sizeof(out)); assert(cascadia_aid_copy_signature(&c,7,out,sizeof(out),&len)==e); assert(len==0); for(j=0;j<128;j++)assert(out[j]==0); assert(cascadia_aid_plan(&c,7,&q)==e);']
 lines+=['}'];runs.append('\n'.join(lines))
fixture='''#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <linux/errno.h>
#include "cascadia-authcp-aid.h"
'''+ '\n'.join(headers)+'''\nint main(void) {\n'''+ '\n'.join(runs)+r'''
{ struct cascadia_aid_exchange c={0},snap; struct cascadia_aid_request q; u8 n[32]={0};
 assert(cascadia_aid_init(&c,7,false,2,64,64,n,20)==-EACCES); assert(!c.initialized);
 assert(cascadia_aid_init(&c,7,true,2,64,0,n,20)==-EINVAL);
 assert(cascadia_aid_init(&c,7,true,2,64,2,n,20)==-EINVAL);
 assert(cascadia_aid_init(&c,7,true,2,20,64,n,20)==-EINVAL);
 for(unsigned int w=33;w<37;w++)assert(cascadia_aid_init(&c,7,true,3,w,64,n,32)==-EINVAL);
 assert(cascadia_aid_init(&c,7,true,2,64,130,n,20)==-EINVAL);
 assert(cascadia_aid_init(&c,7,true,2,64,64,n,20)==0); snap=c;
 assert(cascadia_aid_init(&c,8,true,3,64,64,n,32)==-EALREADY); assert(!memcmp(&c,&snap,sizeof(c)));
 assert(cascadia_aid_plan(&c,8,&q)==-ESTALE && c.fault && !c.touched);
 assert(cascadia_aid_plan(&c,7,&q)==-ESTALE);
}
{ struct cascadia_aid_exchange c={0}; struct cascadia_aid_request q; u8 n[32]={0},ack[]={0x95,0xff,0};
 assert(!cascadia_aid_init(&c,7,true,2,64,64,n,20)); assert(!cascadia_aid_plan(&c,7,&q));
 assert(cascadia_aid_plan(&c,7,&q)==-EBUSY); assert(cascadia_aid_complete(&c,8,0,ack,3)==-ESTALE);
 assert(c.waiting && c.touched && c.fault); assert(cascadia_aid_complete(&c,7,0,ack,3)==-ESTALE);
}
{ struct cascadia_aid_exchange c={0}; u8 n[32]={0},ack[]={0x95,0xff,0};
 assert(!cascadia_aid_init(&c,7,true,2,64,64,n,20)); assert(cascadia_aid_complete(&c,7,0,ack,3)==-EPROTO);
}
{ struct cascadia_aid_exchange c={0}; struct cascadia_aid_request q; u8 n[32]={0},ack[]={0x95,0xff,0};
 assert(!cascadia_aid_init(&c,7,true,2,64,64,n,20)); assert(!cascadia_aid_plan(&c,7,&q));
 assert(cascadia_aid_complete(&c,7,0,ack,2)==-EPROTO); assert(c.waiting && c.fault);
}
{ struct cascadia_aid_exchange c={0}; struct cascadia_aid_request q; u8 n[32]={0};
 assert(!cascadia_aid_init(&c,7,true,2,64,64,n,20)); assert(!cascadia_aid_plan(&c,7,&q));
 c.owner_authorized=false; assert(cascadia_aid_plan(&c,7,&q)==-EACCES); c.owner_authorized=true;
 assert(cascadia_aid_plan(&c,7,&q)==-EACCES && c.fault && c.waiting);
}
{ struct cascadia_aid_exchange c={0},snap; u8 n[32]={0}; size_t len=9;
 assert(!cascadia_aid_init(&c,7,true,2,64,64,n,20)); snap=c;
 assert(cascadia_aid_plan(&c,7,(void*)&c)==-EINVAL); assert(!memcmp(&c,&snap,sizeof(c)));
 assert(cascadia_aid_copy_signature(&c,7,c.signature,128,&len)==-EINVAL); assert(!memcmp(&c,&snap,sizeof(c)));
}
{ struct cascadia_aid_exchange c={0}; u8 n[32]={0},out[128]; size_t len=9;
 assert(!cascadia_aid_init(&c,7,true,2,64,64,n,20)); memset(out,0xcc,sizeof(out));
 assert(cascadia_aid_copy_signature(&c,7,out,sizeof(out),&len)==-EAGAIN && len==0);
 for(size_t j=0;j<128;j++)assert(!out[j]);
}
{ struct cascadia_aid_exchange c={0},saved; u8 n[32]={0};
 c.waiting=true; c.touched=true; c.fault=true; c.error=-EIO; saved=c;
 assert(cascadia_aid_init(&c,7,true,2,64,64,n,20)==-EINVAL);
 assert(!memcmp(&c,&saved,sizeof(c))); }
puts("AuthCPAID actual-C: 7 original positive sequences, 7 failure controls, 3 native admission controls, 8 ownership/bounds groups PASS"); return 0; }
'''
with tempfile.TemporaryDirectory(prefix='cascadia-authcp-aid-')as td:
 p=Path(td);(p/'fixture.c').write_text(fixture);exe=p/'fixture'
 cmd=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-I'+str(root/'include'),'-I'+str(root),str(root/'cascadia-authcp-aid.c'),str(p/'fixture.c'),'-o',str(exe)]
 subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1'})
