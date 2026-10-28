import sys,pathlib,struct,hashlib,json
from capstone import Cs,CS_ARCH_ARM,CS_MODE_THUMB
sys.path.insert(0,'/repo/tools/nand');from ppn_12h321_inventory import segments
rows=[]
for name,path,expected,leaf,ptr in [('12H321','/repo/build/firmware/kernelcache.12H321.macho','db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c',0x80f418cc,0x80f4204c),('13G36','/references/p105-ios935-official/kernelcache.13G36.macho','8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125',0x811b48a8,0x811b504c)]:
 D=pathlib.Path(path).read_bytes();assert hashlib.sha256(D).hexdigest()==expected;T=segments(D,0)
 def off(va):
  found=[s['offset']+va-s['address']for s in T if s['address']<=va<s['address']+s['file_size']];assert len(found)==1;return found[0]
 def word(va):return struct.unpack_from('<I',D,off(va))[0]
 parent=word(ptr);target=word(parent+0x1f0)&~1
 getter=word((0x80f42088 if name=='12H321' else 0x811b507c)+0x1cc)&~1
 symbols=[];pos=28
 for _ in range(struct.unpack_from('<I',D,16)[0]):
  cmd,size=struct.unpack_from('<II',D,pos)
  if cmd==2:
   a,n,s,z=struct.unpack_from('<4I',D,pos+8)
   for i in range(n):
    j,t,se,de,v=struct.unpack_from('<IBBHI',D,a+i*12)
    if v in [target,getter]:symbols.append(D[s+j:D.index(b'\0',s+j)].decode())
  pos+=size
 assert '__ZN9IOService20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_' in symbols
 md=Cs(CS_ARCH_ARM,CS_MODE_THUMB)
 lines=[]
 for title,start,end in [('platform',leaf,leaf+0x84),('parent',target,target+0x60),('getter',getter,getter+0x60)]:
  lines.append(title+' '+hex(start))
  for ins in md.disasm(D[off(start):off(start)+end-start],start):lines.append('%08x %s %s'%(ins.address,ins.mnemonic,ins.op_str))
 pathlib.Path('/out/'+name+'-continuation.txt').write_text('\n'.join(lines)+'\n')
 rows.append({'profile':name,'kernel_sha256':expected,'platform':hex(leaf),'parent_pointer':hex(ptr),'parent_vtable':hex(parent),'parent_target':hex(target),'symbols':symbols,'getter':hex(getter)})
pathlib.Path('/out/bindings.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(rows,indent=2))
