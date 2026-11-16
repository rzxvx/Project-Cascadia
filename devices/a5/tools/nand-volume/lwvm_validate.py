# SPDX-License-Identifier: GPL-2.0-only
# iOS's own LwVM header checks (12H321 kernelcache), run under Unicorn, as spec
# section 6.6 describes.  Written by the spec session for the spec; copied here.
# The kernelcache comes from the user's IPSW (build/firmware); override with
# CASCADIA_KC=/path/to/kernelcache.12H321.macho.
import os,struct,zlib,hashlib
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE
from unicorn.arm_const import *
KC=os.environ.get('CASCADIA_KC') or os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../build/firmware/kernelcache.12H321.macho')
b=open(KC,'rb').read()
assert hashlib.sha256(b).hexdigest().startswith('db1ddc83')
INIT,CRC,VAL=0x80a9eb60,0x80a9eebc,0x80a9ef80
UUIDCMP,CRC32,LOG=0x80aa7b4c,0x80aa7aac,0x80aa7abc
u=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
u.mem_map(0x80000000,0x1000000)
n=struct.unpack_from('<I',b,16)[0];o=28
for _ in range(n):
    cmd,sz=struct.unpack_from('<II',b,o)
    if cmd==1:
        va,vs,fo,fs=struct.unpack_from('<4I',b,o+24)
        if fs and va>=0x80000000 and va+fs<=0x81000000: u.mem_write(va,b[fo:fo+fs])
    o+=sz
HEAP,STACK,STOP=0x70000000,0x71000000,0x72000000
for a in (HEAP,STACK,STOP): u.mem_map(a,0x100000)
PROP,HDR=HEAP,HEAP+0x1000
msgs=[]
def cs(a): return bytes(u.mem_read(a,200)).split(b'\0')[0].decode()
def hook(uc,a,size,_):
    if a==STOP: uc.emu_stop(); return
    R=lambda r: uc.reg_read(r)
    if a==UUIDCMP: r=0 if uc.mem_read(R(UC_ARM_REG_R0),16)==uc.mem_read(R(UC_ARM_REG_R1),16) else 1
    elif a==CRC32: r=zlib.crc32(bytes(uc.mem_read(R(UC_ARM_REG_R1),R(UC_ARM_REG_R2))),R(UC_ARM_REG_R0))&0xffffffff
    elif a==LOG:
        f=cs(R(UC_ARM_REG_R0)); args=[R(UC_ARM_REG_R1),R(UC_ARM_REG_R2),R(UC_ARM_REG_R3)]
        try: msgs.append((f%tuple(args[:f.count('%')])).strip())
        except Exception: msgs.append(f.strip()+' '+str(args))
        r=0
    else: return
    uc.reg_write(UC_ARM_REG_R0,r); uc.reg_write(UC_ARM_REG_PC,R(UC_ARM_REG_LR))
for a in (STOP,UUIDCMP,CRC32,LOG): u.hook_add(UC_HOOK_CODE,hook,begin=a,end=a)
def call(e,*args):
    for i in range(13): u.reg_write(UC_ARM_REG_R0+i,0)
    for i,v in enumerate(args): u.reg_write(UC_ARM_REG_R0+i,v)
    u.reg_write(UC_ARM_REG_SP,STACK+0xff000); u.reg_write(UC_ARM_REG_LR,STOP|1)
    u.emu_start(e|1,STOP,count=50000000)
    assert u.reg_read(UC_ARM_REG_PC)==STOP
    return u.reg_read(UC_ARM_REG_R0)
def props(media_size,blk=4096):
    u.mem_write(PROP,bytes(64))
    r=call(INIT,PROP,media_size&0xffffffff,media_size>>32,blk)
    return r,struct.unpack('<9I',u.mem_read(PROP,36))
def check(hdr,media_size=16000000000,fixcrc=True):
    h=bytearray(hdr)
    if fixcrc:
        struct.pack_into('<I',h,0x2c,0); struct.pack_into('<I',h,0x2c,zlib.crc32(h)&0xffffffff)
    props(media_size)
    u.mem_write(HDR,bytes(h)); msgs.clear()
    c=call(CRC,HDR); v=call(VAL,PROP,HDR)
    return c,(v if v<0x80000000 else v-(1<<32)),list(msgs)
