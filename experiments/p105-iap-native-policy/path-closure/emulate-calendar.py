# SPDX-License-Identifier: GPL-2.0
"""Original13G iAP Gregorian/GMT0 setup; CF arithmetic/dispatch boundaries fake."""
import argparse,datetime,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE,UcError
from unicorn.arm_const import *
from native13 import Sources,OBJ,STACK,END,CACHE_SHA,DAEMON_SHA
ap=argparse.ArgumentParser();ap.add_argument('cache',type=Path);ap.add_argument('daemon',type=Path);args=ap.parse_args();source=Sources(args.cache,args.daemon)
# Cache stubs are taken from their original nlists/indirect-symbol tables.
syms,sections,_,indirect=source.macho(source.cache,source.offset(0x20c7c000,28));stubs={}
for section in sections:
 va,size,offset,flags,r1,r2=section[2:]
 if flags&255==8:
  for i in range(size//r2):stubs[va+r2*i]=indirect[r1+i]
ENTRY=source.exports['_SecPolicyCreateiAP'];assert ENTRY==0x20cabb6c
PINS={'_CFCalendarCreateWithIdentifier':0x20846b80,'_CFCalendarSetTimeZone':0x208477a8,'_CFCalendarComposeAbsoluteTime':0x2091df58,'_CFTimeZoneCreateWithTimeIntervalFromGMT':0x2087831c,'_CFDateCreate':0x20810648}
for name,address in PINS.items():assert source.exports[name]==address
DICT,POLICY,DATE,CALENDAR,TIMEZONE,QUEUE=[OBJ+0x1000*i for i in range(1,7)]
TRAMP=END-0xc0


def run(arithmetic_status=True,arithmetic_value=None):
 mu=source.context();calls=[];returns=[];configured=False;date_value=None
 def read(va,n):
  try:return bytes(mu.mem_read(va,n))
  except UcError:return source.cache_read(va,n)
 def word(va):return struct.unpack('<I',read(va,4))[0]
 def put(va,value):
  page=va&~4095
  if not any(a<=va<=b for a,b,p in mu.mem_regions()):mu.mem_map(page,4096);mu.mem_write(page,source.cache_read(page,4096))
  mu.mem_write(va,struct.pack('<I',value&0xffffffff))
 def cstring(va):
  output=b''
  for i in range(128):
   value=read(va+i,1)
   if value==b'\0':return output.decode()
   output+=value
  raise ValueError('unterminated bounded string')
 def cfstring(va):
  isa,flags,pointer,n=struct.unpack('<4I',read(va,16));assert flags==0x7c8 and n<128
  return read(pointer,n).decode()
 def invoke(mc,block,kind,token=None):
  entry=word(block+12);assert entry&1
  returns.append((mc.reg_read(UC_ARM_REG_LR),kind,token))
  mc.reg_write(UC_ARM_REG_R0,block);mc.reg_write(UC_ARM_REG_LR,TRAMP|1);mc.reg_write(UC_ARM_REG_PC,entry)
 def callback(mc,pc,size,cookie):
  nonlocal configured,date_value
  if pc==TRAMP:
   caller,kind,token=returns.pop()
   if kind=='once':put(token,0xffffffff)
   mc.reg_write(UC_ARM_REG_R0,0);mc.reg_write(UC_ARM_REG_PC,caller);return
  r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
  name=stubs.get(pc)
  if name is None:name=next((k for k in PINS if PINS[k]==pc),None)
  if pc==source.exports['_CFDictionaryCreateMutable']:name='_CFDictionaryCreateMutable'
  if pc==source.exports['_CFDictionaryAddValue']:name='_CFDictionaryAddValue'
  if pc==source.exports['_SecPolicyCreate']:name='_SecPolicyCreate'
  if pc==source.exports['_CFRelease']:name='_CFRelease'
  if pc==source.exports['__Block_object_dispose']:name='__Block_object_dispose'
  out=0
  if name=='_dispatch_once':
   calls.append({'original_once_invoke':hex(word(r1+12)&~1)});invoke(mc,r1,'once',r0);return
  elif name=='_dispatch_queue_create':assert r1==0;calls.append({'synthetic_serial_queue':cstring(r0)});out=QUEUE
  elif name=='_dispatch_sync':assert r0==QUEUE;calls.append({'original_sync_invoke':hex(word(r1+12)&~1)});invoke(mc,r1,'sync');return
  elif name=='_CFDictionaryCreateMutable':out=DICT
  elif name=='_CFDictionaryAddValue':
   assert r0==DICT
   if cfstring(r1)=='NotValidBefore':assert r2==DATE;calls.append({'date_property':'NotValidBefore'})
  elif name=='_CFCalendarCreateWithIdentifier':
   identity=cfstring(r1);assert identity=='gregorian';calls.append({'calendar_identifier':identity});out=CALENDAR
  elif name=='_CFTimeZoneCreateWithTimeIntervalFromGMT':
   offset=struct.unpack('<d',struct.pack('<2I',r1,r2))[0];assert offset==0.0;calls.append({'timezone_offset_seconds':offset});out=TIMEZONE
  elif name=='_CFCalendarSetTimeZone':assert(r0,r1)==(CALENDAR,TIMEZONE);configured=True;calls.append('set_calendar_timezone')
  elif name=='_CFCalendarComposeAbsoluteTime':
   assert r0==CALENDAR and configured
   month,day=struct.unpack('<2I',read(mc.reg_read(UC_ARM_REG_SP),8));fmt=cstring(r2);assert(fmt,r3,month,day)==('yMd',2006,5,31)
   value=(datetime.datetime(r3,month,day,tzinfo=datetime.timezone.utc)-datetime.datetime(2001,1,1,tzinfo=datetime.timezone.utc)).total_seconds() if arithmetic_value is None else arithmetic_value
   mc.mem_write(r1,struct.pack('<d',value));calls.append({'synthetic_CF_arithmetic':{'format':fmt,'year':r3,'month':month,'day':day,'computed_reference_seconds':value,'synthetic_status':arithmetic_status}});out=int(arithmetic_status)
  elif name=='_CFDateCreate':
   value=struct.unpack('<d',struct.pack('<2I',r1,r2))[0];date_value=value;calls.append({'date_create_received_synthetic_value':value});out=DATE
  elif name=='_SecPolicyCreate':assert r1==DICT;calls.append({'policy_oid':cfstring(r0)});out=POLICY
  elif name in('_CFRelease','__Block_object_dispose'):out=0
  else:
   if name:raise AssertionError(('unprovided_api',name,hex(pc)))
   return
  mc.reg_write(UC_ARM_REG_R0,out);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
 mu.hook_add(UC_HOOK_CODE,callback)
 try:mu.emu_start(ENTRY|1,END,count=5000)
 except Exception as e:raise RuntimeError(f'pc={mu.reg_read(UC_ARM_REG_PC):x},calls={calls},returns={returns}')from e
 assert mu.reg_read(UC_ARM_REG_PC)==END and mu.reg_read(UC_ARM_REG_R0)==POLICY and not returns
 return {'synthetic_arithmetic_status':arithmetic_status,'calls':calls,'calendar_setup_original':True,'CF_arithmetic_synthetic':True,'received_date_value':date_value,'authorization_granted':False}
rows=[run(),run(arithmetic_status=False,arithmetic_value=567.0)]
assert [r['received_date_value']for r in rows]==[170726400.0,567.0]
assert all(r['calls'][4:7]==[{'calendar_identifier':'gregorian'},{'timezone_offset_seconds':0.0},'set_calendar_timezone']for r in rows)
print(json.dumps({'cache_sha256':CACHE_SHA,'securityd_sha256':DAEMON_SHA,'scope':'original queue/calendar/GMT0/setup/caller; synthetic CF arithmetic and dispatch execution model','cases':rows},indent=2))
