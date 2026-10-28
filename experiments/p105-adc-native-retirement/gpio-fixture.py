# SPDX-License-Identifier: GPL-2.0-only
"""Private actual 12H321 GPIO IRQ init/type with fake OSData and MMIO RAM."""
import hashlib,struct
from unicorn import Uc,UC_ARCH_ARM,UC_MODE_THUMB,UC_HOOK_CODE,UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_PC,UC_ARM_REG_SP,UC_ARM_REG_LR
SHA='db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c'
OBJ,SHADOW,MMIO,LOCK,NUB,ARRAY,DATA,VT,RAW,VECTOR,OUT,STACK,TRAP,END=(0x10000000,0x10001000,0x10002000,0x10003000,0x10004000,0x10004100,0x10004200,0x10004300,0x10004400,0x10004500,0x10004600,0x20000000,0x30000000,0x30000ffc)
class GPIO:
 def __init__(self,data,pin=19,flags=1,shadow=0x1d3):
  if hashlib.sha256(data).hexdigest()!=SHA:raise ValueError('wrong image')
  if type(pin)is not int or not 0<=pin<224 or type(flags)is not int or not 0<=flags<8 or type(shadow)is not int or not 0<=shadow<65536:raise ValueError('fixture bounds invalid')
  self.u=Uc(UC_ARCH_ARM,UC_MODE_THUMB);self.pin=pin;self.writes=[]
  self.u.mem_map(0x80dec000,0xa000);o=0x418000+0x80dec000-0x80460000;self.u.mem_write(0x80dec000,data[o:o+0xa000])
  for a,s in ((OBJ,0x10000),(STACK,0x10000),(TRAP,0x1000)):self.u.mem_map(a,s)
  for a,v in ((OBJ,0x80df1938),(OBJ+0x80,SHADOW),(OBJ+0x64,MMIO),(OBJ+0x68,LOCK),(NUB+0x4c,ARRAY),(ARRAY+4,DATA),(DATA,VT),(VT+0x68,TRAP|1),(VECTOR+8,NUB),(VECTOR+12,0),(RAW,pin),(RAW+4,flags),(MMIO+pin*4,shadow)):
   self.w32(a,v)
  self.u.mem_write(SHADOW+pin*2,struct.pack('<H',shadow));self.u.hook_add(UC_HOOK_CODE,self.fake);self.u.hook_add(UC_HOOK_MEM_WRITE,self.observe)
 def w32(self,a,v):self.u.mem_write(a,struct.pack('<I',v))
 def fake(self,u,a,n,user):
  if a==TRAP:
   if u.reg_read(UC_ARM_REG_R0)!=DATA:raise RuntimeError('unexpected fake OSData owner')
   u.reg_write(UC_ARM_REG_R0,RAW);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
  elif a in (0x80df0874,0x80df05a4,0x80df05b4):
   u.reg_write(UC_ARM_REG_R0,0);u.reg_write(UC_ARM_REG_PC,u.reg_read(UC_ARM_REG_LR))
 def observe(self,u,access,a,size,v,user):
  if MMIO<=a<MMIO+0x1000:
   if a!=MMIO+self.pin*4 or size!=4:raise RuntimeError('unexpected fake MMIO write')
   self.writes.append({'offset':a-MMIO,'value':v})
 def call(self,entry,args):
  for r,v in zip((UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3),args):self.u.reg_write(r,v)
  self.u.reg_write(UC_ARM_REG_SP,STACK+0xff00);self.u.reg_write(UC_ARM_REG_LR,END|1)
  self.u.emu_start(entry|1,END,count=10000)
  if self.u.reg_read(UC_ARM_REG_PC)!=END:raise RuntimeError('instruction budget exhausted')
 def run(self):
  self.call(0x80ded634,(OBJ,NUB,0,OUT))
  kind=struct.unpack('<I',self.u.mem_read(OUT,4))[0]
  self.call(0x80ded764,(OBJ,self.pin,VECTOR,0))
  return {'flags':struct.unpack('<I',self.u.mem_read(RAW+4,4))[0],'type':kind,'writes':self.writes,'hardware_access':False}
