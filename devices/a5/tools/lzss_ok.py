# Correct XNU-compatible LZSS. Capped chain search for speed.
N,F,THR=4096,18,2
MAXCHAIN=32
def compress(data: bytes) -> bytes:
    n=len(data); out=bytearray(); flag=0; fc=0; ibuf=bytearray()
    htab={}
    mv=data
    def key(p): return (mv[p]<<16)|(mv[p+1]<<8)|mv[p+2]
    i=0
    while i<n:
        best_len=0; best_d=0
        avail=F if n-i>=F else n-i
        if avail>=3:
            k=key(i); cand=htab.get(k)
            if cand:
                for idx in range(len(cand)-1, max(-1,len(cand)-1-MAXCHAIN), -1):
                    p=cand[idx]; d=i-p
                    if d<1 or d>N-1: continue
                    if mv[i+best_len-d]!=mv[i+best_len]:   # quick reject on current best edge
                        # still must try shorter? no: if can't beat best_len, skip
                        if best_len>0: continue
                    ml=0
                    while ml<avail and mv[i+ml]==mv[i+ml-d]:
                        ml+=1
                    if ml>best_len:
                        best_len=ml; best_d=d
                        if ml==avail: break
        if best_len>=THR+1:
            r=(N-F+i)&(N-1); pos=(r-best_d)&(N-1)
            ibuf.append(pos&0xFF)
            ibuf.append(((pos>>8)&0xF)<<4 | (best_len-(THR+1)))
            e=i+best_len
            while i<e:
                if i+2<n:
                    c=htab.setdefault(key(i),[])
                    c.append(i)
                    if len(c)>256: del c[:128]
                i+=1
        else:
            flag|=(1<<fc)
            ibuf.append(mv[i])
            if i+2<n:
                c=htab.setdefault(key(i),[]); c.append(i)
                if len(c)>256: del c[:128]
            i+=1
        fc+=1
        if fc==8:
            out.append(flag); out.extend(ibuf); flag=0; fc=0; ibuf=bytearray()
    if fc>0:
        out.append(flag); out.extend(ibuf)
    return bytes(out)
