# SPDX-License-Identifier: GPL-2.0
"""Bounded source loaders for actual securityd/native-cache option proofs.

No Apple byte fixtures, native process execution, hardware or IPC endpoints.
"""
from pathlib import Path
import hashlib,mmap,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_MEM_READ_UNMAPPED,UC_HOOK_MEM_FETCH_UNMAPPED
from unicorn.arm_const import UC_ARM_REG_SP,UC_ARM_REG_LR,UC_ARM_REG_C1_C0_2,UC_ARM_REG_FPEXC
CACHE_SHA='4f5b5d73244c53fd0bdc5b8012106b10d0dd7607cc850bf7c8f9e9ef3a3f733b'
DAEMON_SHA='1c12fbb48ead1edd3dbf2f4b0d635b45452573499fd9592562bbd76e40b56097'
OBJ,STACK,END=0x10000000,0x20000000,0x30000ffc


def hash_file(stream):
    result=hashlib.sha256()
    while block:=stream.read(2**20):result.update(block)
    return result.hexdigest()


class Sources:
    def __init__(self,cache,daemon):
        self.file=Path(cache).open('rb')
        if hash_file(self.file)!=CACHE_SHA:raise ValueError('wrong official cache SHA')
        self.cache=mmap.mmap(self.file.fileno(),0,access=mmap.ACCESS_READ)
        self.daemon=Path(daemon).read_bytes()
        if hashlib.sha256(self.daemon).hexdigest()!=DAEMON_SHA:raise ValueError('wrong official daemon SHA')
        mo,mc,io,ic=struct.unpack_from('<4I',self.cache,16)
        self.maps=[struct.unpack_from('<3Q2I',self.cache,mo+32*i)for i in range(mc)]
        self.exports={};self.export_thumb={}
        for i in range(ic):
            va,_,_,po,_=struct.unpack_from('<3Q2I',self.cache,io+32*i)
            name=self.cache[po:self.cache.find(b'\0',po)].decode()
            if name.endswith(('/Security.framework/Security','/CoreFoundation.framework/CoreFoundation')) or name.startswith('/usr/lib/system/libsystem_'):
                symbols,_,_,_=self.macho(self.cache,self.offset(va,28))
                self.exports.update({k:v['value']for k,v in symbols.items()if v['value'] and v['type']&0x0e==0x0e})
                self.export_thumb.update({k:bool(v['desc']&8)for k,v in symbols.items()if v['value'] and v['type']&0x0e==0x0e})
        self.symbols,self.sections,self.segments,self.indirect=self.macho(self.daemon,0)
        self.stubs={};self.bindings={}
        for section in self.sections:
            va,size,offset,flags,r1,r2=section[2:]
            kind=flags&255
            if kind==8:
                for i in range(size//r2):self.stubs[va+i*r2]=self.indirect[r1+i]
            if kind in(6,7):
                for i in range(size//4):
                    name=self.indirect[r1+i]
                    if name in self.exports:self.bindings[va+4*i]=self.exports[name]|int(self.export_thumb[name])

    def offset(self,va,n):
        row=next((r for r in self.maps if r[0]<=va and va+n<=r[0]+r[1]),None)
        if row is None:raise ValueError('source address outside cache')
        return row[2]+va-row[0]

    def cache_read(self,va,n):
        offset=self.offset(va,n);return self.cache[offset:offset+n]

    @staticmethod
    def macho(data,header):
        if struct.unpack_from('<I',data,header)[0]!=0xfeedface:raise ValueError('not ARM32 Mach-O')
        pos=header+28;sections=[];segments=[]
        for _ in range(struct.unpack_from('<I',data,header+16)[0]):
            command,size=struct.unpack_from('<II',data,pos)
            if size<8:raise ValueError('bad load command')
            if command==2:so,n,st,ss=struct.unpack_from('<4I',data,pos+8)
            if command==11:indirect_offset,indirect_count=struct.unpack_from('<2I',data,pos+56)
            if command==1:
                vm,vm_size,file_offset,file_size=struct.unpack_from('<4I',data,pos+24)
                segments.append((vm,vm_size,file_offset,file_size))
                for i in range(struct.unpack_from('<I',data,pos+48)[0]):
                    q=pos+56+68*i;name=data[q:q+16].split(b'\0')[0].decode();segment=data[q+16:q+32].split(b'\0')[0].decode()
                    values=struct.unpack_from('<9I',data,q+32)
                    sections.append((name,segment,values[0],values[1],values[2],values[6],values[7],values[8]))
            pos+=size
        symbols={};names={}
        for i in range(n):
            si,kind,section,desc,value=struct.unpack_from('<IBBHI',data,so+12*i)
            name=data[st+si:data.find(b'\0',st+si)].decode();names[i]=name
            symbols[name]={'value':value,'desc':desc,'type':kind}
        indirect={i:names.get(struct.unpack_from('<I',data,indirect_offset+4*i)[0],'<local>')for i in range(indirect_count)}
        return symbols,sections,segments,indirect

    def context(self):
        mu=Uc(UC_ARCH_ARM,UC_MODE_THUMB)
        for vm,vm_size,file_offset,file_size in self.segments:
            if vm_size and vm:
                mu.mem_map(vm,(vm_size+4095)&~4095)
                if file_size:mu.mem_write(vm,self.daemon[file_offset:file_offset+file_size])
        mu.mem_map(OBJ,0x10000);mu.mem_map(STACK,0x10000);mu.mem_map(END&~4095,4096)
        for slot,value in self.bindings.items():mu.mem_write(slot,struct.pack('<I',value))
        mapped=set()
        def unmapped(mc,kind,va,size,value,cookie):
            page=va&~4095
            if page not in mapped:
                raw=self.cache_read(page,4096);mu.mem_map(page,4096);mu.mem_write(page,raw);mapped.add(page)
            return True
        mu.hook_add(UC_HOOK_MEM_READ_UNMAPPED,unmapped);mu.hook_add(UC_HOOK_MEM_FETCH_UNMAPPED,unmapped)
        mu.reg_write(UC_ARM_REG_C1_C0_2,0xf00000);mu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
        mu.reg_write(UC_ARM_REG_SP,STACK+0xff00);mu.reg_write(UC_ARM_REG_LR,END|1)
        return mu
