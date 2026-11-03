import sys,struct,zlib,time
sys.path.insert(0,'/home/claude')
from lzss_ok import compress
COMPLZSS=0x180
def build_complzss(unc,comp):
    cs=zlib.adler32(unc,1)&0xffffffff
    hdr=struct.pack(">6I",0x636f6d70,0x6c7a7373,cs,len(unc),len(comp),0)
    return hdr+b'\x00'*(COMPLZSS-len(hdr))+comp
def rewrap(templ,newdata):
    d=templ; assert d[:4]==b'3gmI'; ident=d[16:20]
    off=20; tags=[]
    while off+12<=len(d):
        tag=d[off:off+4]; tlen,dlen=struct.unpack_from('<II',d,off+4)
        tags.append((tag,d[off:off+tlen])); off+=tlen
        if tlen<12: break
    def mk(tag,data):
        pad=(-(12+len(data)))%4
        return tag+struct.pack('<II',12+len(data)+pad,len(data))+data+b'\x00'*pad
    ot=[mk(b'ATAD',newdata) if t==b'ATAD' else raw for t,raw in tags]
    body=b''.join(ot); full=20+len(body)
    sh=0
    for t in ot:
        if t[:4]==b'HSHS': break
        sh+=len(t)
    return b'3gmI'+struct.pack('<III',full,full-20,sh)+ident+body
def de(src,outlen):
    o=bytearray(); ring=bytearray(b' '*N); r=N-F; i=0;L=len(src)
    while i<L and len(o)<outlen:
        fl=src[i]; i+=1
        for b in range(8):
            if len(o)>=outlen or i>=L: break
            if fl&(1<<b): c=src[i];i+=1;o.append(c);ring[r]=c;r=(r+1)&(N-1)
            else:
                if i+1>=L:break
                b1=src[i];b2=src[i+1];i+=2;pos=b1|((b2>>4)&0xF)<<8;ln=(b2&0xF)+THR+1
                for k in range(ln): c=ring[(pos+k)&(N-1)];o.append(c);ring[r]=c;r=(r+1)&(N-1)
    return bytes(o)
N,F,THR=4096,18,2
macho=open(sys.argv[1],'rb').read(); templ=open(sys.argv[2],'rb').read(); out=sys.argv[3]
t=time.time(); comp=compress(macho); print("compress %.1fs -> %d (%.1f%%)"%(time.time()-t,len(comp),len(comp)*100/len(macho)))
back=de(comp,len(macho)); print("roundtrip:",back==macho)
if back!=macho:
    for j in range(min(len(back),len(macho))):
        if back[j]!=macho[j]: print("first diff @0x%x"%j); break
    sys.exit(1)
clz=build_complzss(macho,comp); img3=rewrap(templ,clz); open(out,'wb').write(img3)
print("wrote",out,len(img3))
