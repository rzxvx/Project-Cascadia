# SPDX-License-Identifier: GPL-2.0
"""Original13G securityd policy facets; synthetic certificate/CF/path providers."""
import argparse,json,struct
from pathlib import Path
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import *
from native13 import Sources,OBJ,STACK,END,CACHE_SHA,DAEMON_SHA
ap=argparse.ArgumentParser();ap.add_argument('cache',type=Path);ap.add_argument('daemon',type=Path);args=ap.parse_args();source=Sources(args.cache,args.daemon)
PINS={'_SecPolicyCheckBasicCertificateProcessing':0x31904,'_SecPolicyCheckSubjectCommonNamePrefix':0x329ec,'_SecPolicyCheckNotValidBefore':0x32b5c,'_SecPVCSetResultForced':0x33840,'_SecPVCParentCertificateChecks':0x33980,'_SecPolicyCheckIdLinkage':0x32038,'_SecPolicyCheckCriticalExtensions':0x32034,'_SecPolicyCheckBasicContraints':0x3220c}
for name,a in PINS.items():assert source.symbols[name]['value']==a,(name,source.symbols.get(name))
POLICIES,POLICY,OPTIONS,PATH,LEAF,ROOT,PREFIX,COMMONS,DATE,CN,BASIC,KEYDATA=[OBJ+0x1000*i for i in range(1,13)]
NATIVE_OPTIONS={'CriticalExtensions','IdLinkage','BasicContraints','NonEmptySubject','QualifiedCertStatements','WeakIntermediates','WeakLeaf','WeakRoot','SubjectCommonNamePrefix','NotValidBefore'}
def cfkey(name):return struct.unpack('<I',source.cache_read(source.exports['_kSecPolicyCheck'+name],4))[0]
KEYS={k:cfkey(k)for k in NATIVE_OPTIONS|{'ValidRoot','ValidLeaf','ValidIntermediates','KeyUsage'}}
REVERSE={v:k for k,v in KEYS.items()}


def run(kind,common_names=('IPA_synthetic',),not_before=10.0,cutoff=0.0,valid=True,weak=False,anchored=True,is_ca=False,ku=0,options=None,forced=False,SKI=None,AKI=None,leaf_critical=False,root_critical=False):
    options=NATIVE_OPTIONS if options is None else set(options)
    mu=source.context();calls=[];failures=[];heap=OBJ+0xc000;allocations=set();arrays={};cf_next=OBJ+0xa100
    def put(a,v):mu.mem_write(a,struct.pack('<I',v&0xffffffff))
    put(OBJ+4,POLICIES);put(OBJ+0x10,PATH);put(OBJ+0x24,0);mu.mem_write(OBJ+0x35,b'\1');put(POLICY+0xc,OPTIONS);mu.mem_write(OBJ+8,struct.pack('<d',0.0));mu.mem_write(BASIC,b'\0\0\1\0')
    def callback(mc,pc,size,cookie):
        nonlocal heap,cf_next
        r0,r1,r2,r3=[mc.reg_read(r)for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3)]
        name=source.stubs.get(pc);out=0
        if pc==source.exports['_CFRelease']:name='_CFRelease'
        if pc==source.exports['_CFRetain']:name='_CFRetain'
        if name=='_CFArrayGetValueAtIndex':
            if r0==POLICIES:assert r1==0;out=POLICY
            elif r0==COMMONS:assert r1<len(common_names);out=CN+r1*4
            elif r0 in arrays:assert r1<len(arrays[r0]);out=arrays[r0][r1]
            else:raise AssertionError(('array',r0,r1))
        elif name=='_CFDictionaryGetValue':
            assert r0==OPTIONS
            if r1==KEYS['SubjectCommonNamePrefix']:out=PREFIX if 'SubjectCommonNamePrefix'in options else 0
            elif r1==KEYS['NotValidBefore']:out=DATE if 'NotValidBefore'in options else 0
            else:raise AssertionError(('dictionary_get',hex(r1)))
        elif name=='_CFDictionaryContainsKey':assert r0==OPTIONS;out=int(REVERSE.get(r1)in options)
        elif name=='_CFGetTypeID':out=21 if r0==PREFIX else 42 if r0==DATE else 0
        elif name=='_CFStringGetTypeID':out=21
        elif name=='_CFDateGetTypeID':out=42
        elif name=='_SecCertificatePathGetCertificateAtIndex':assert r0==PATH and r1 in(0,1);out=LEAF if r1==0 else ROOT
        elif name=='_SecCertificatePathGetCount':assert r0==PATH;out=2
        elif name=='_SecCertificatePathIsAnchored':assert r0==PATH;out=int(anchored)
        elif name=='_SecCertificateCopyCommonNames':assert r0==LEAF;out=COMMONS if common_names is not None else 0
        elif name=='_CFArrayGetCount':
            if r0==COMMONS:out=len(common_names)
            else:assert r0 in arrays;out=len(arrays[r0])
        elif name=='_CFArrayCreateMutable':out=cf_next;cf_next+=32;assert cf_next<OBJ+0xb000;arrays[out]=[]
        elif name=='_CFArrayAppendValue':assert r0 in arrays and len(arrays[r0])<8;arrays[r0].append(r1)
        elif name=='_CFStringHasPrefix':assert r1==PREFIX and r0 in(CN,CN+4);out=int(common_names[(r0-CN)//4].startswith('IPA_'))
        elif name in('_CFDateGetAbsoluteTime','_SecCertificateNotValidBefore'):
            assert r0==(DATE if name=='_CFDateGetAbsoluteTime'else LEAF)
            value=cutoff if name=='_CFDateGetAbsoluteTime'else not_before
            low,high=struct.unpack('<2I',struct.pack('<d',value));mc.reg_write(UC_ARM_REG_R1,high);out=low
        elif name=='_SecCertificateIsValid':assert r0 in(LEAF,ROOT);out=int(valid);calls.append({'is_valid':'leaf'if r0==LEAF else'root'})
        elif name=='_SecCertificateIsWeak':assert r0 in(LEAF,ROOT);out=int(weak);calls.append({'is_weak':'leaf'if r0==LEAF else'root'})
        elif name=='_SecCertificateGetBasicConstraints':assert r0 in(LEAF,ROOT);out=0;calls.append({'basic_constraints':'leaf'if r0==LEAF else'root'})
        elif name=='_SecCertificateGetKeyUsage':assert r0==ROOT;out=ku;calls.append('key_usage')
        elif name=='_SecCertificateGetSubjectKeyID':out=KEYDATA if(r0==ROOT and SKI is not None)else 0
        elif name=='_SecCertificateGetAuthorityKeyID':assert r0==LEAF;out=KEYDATA+4 if AKI is not None else 0
        elif name=='_CFEqual':assert r0==KEYDATA and r1==KEYDATA+4;out=int(SKI==AKI)
        elif name=='_SecCertificatePathSelfSignedIndex':assert r0==PATH;out=1
        elif name=='_SecCertificateHasUnknownCriticalExtension':
            assert r0 in(LEAF,ROOT);out=int(leaf_critical if r0==LEAF else root_critical);calls.append({'unknown_critical':'leaf'if r0==LEAF else'root'})
        elif name in('_SecCertificateGetCertificatePolicies','_SecCertificateGetPolicyMappings','_SecCertificateGetNameConstraints','_SecCertificateGetPolicyConstraints'):assert r0 in(LEAF,ROOT);out=0
        elif name=='_SecCertificateGetInhibitAnyPolicySkipCerts':assert r0 in(LEAF,ROOT);out=0xffffffff
        elif name in('_malloc','_calloc'):
            length=r0 if name=='_malloc'else r0*r1
            assert 0<length<=2048 and heap+length<OBJ+0x10000
            out=heap;heap+=(length+15)&~15;allocations.add(out);mc.mem_write(out,b'\0'*length);calls.append({'synthetic_zero_allocator':length})
        elif name=='_free':assert not r0 or r0 in allocations;out=0
        elif name=='_memset':assert r2<=2048;mc.mem_write(r0,bytes([r1&255])*r2);out=r0
        elif name=='_memcpy':assert r2<=2048;mc.mem_write(r0,bytes(mc.mem_read(r1,r2)));out=r0
        elif name=='_CFRetain':out=r0
        elif name=='_CFRelease':out=0
        else:
            if name:raise AssertionError(('unprovided API',name,hex(pc)))
            # Observe actual setter admission; its full native body still runs.
            if pc==PINS['_SecPVCSetResultForced']:
                force=struct.unpack('<I',mc.mem_read(mc.reg_read(UC_ARM_REG_SP),4))[0]
                failures.append({'key':REVERSE.get(r1,'unknown'),'index':r2,'forced':bool(force),'policy_contains':REVERSE.get(r1)in options})
            return
        mc.reg_write(UC_ARM_REG_R0,out);mc.reg_write(UC_ARM_REG_PC,mc.reg_read(UC_ARM_REG_LR))
    mu.hook_add(UC_HOOK_CODE,callback)
    if kind=='basic':entry=PINS['_SecPolicyCheckBasicCertificateProcessing'];key=KEYS['CriticalExtensions']
    elif kind=='prefix':entry=PINS['_SecPolicyCheckSubjectCommonNamePrefix'];key=KEYS['SubjectCommonNamePrefix']
    elif kind=='date':entry=PINS['_SecPolicyCheckNotValidBefore'];key=KEYS['NotValidBefore']
    elif kind=='linkage':entry=PINS['_SecPolicyCheckIdLinkage'];key=KEYS['IdLinkage']
    elif kind=='parent':entry=PINS['_SecPVCParentCertificateChecks'];key=1
    elif kind=='setter':entry=PINS['_SecPVCSetResultForced'];key=KEYS['ValidRoot'];put(STACK+0xff00,int(forced));mu.reg_write(UC_ARM_REG_R2,1);mu.reg_write(UC_ARM_REG_R3,0)
    else:raise ValueError(kind)
    mu.reg_write(UC_ARM_REG_R0,OBJ);mu.reg_write(UC_ARM_REG_R1,key)
    try:mu.emu_start(entry|1,END,count=2000)
    except Exception as e:raise RuntimeError(f'{kind}:PC={mu.reg_read(UC_ARM_REG_PC):x}, calls={calls},failures={failures},regs={[hex(mu.reg_read(r))for r in(UC_ARM_REG_R0,UC_ARM_REG_R1,UC_ARM_REG_R2,UC_ARM_REG_R3,UC_ARM_REG_R4,UC_ARM_REG_R5,UC_ARM_REG_R6)]}')from e
    assert mu.reg_read(UC_ARM_REG_PC)==END
    return {'kind':kind,'synthetic_common_names':list(common_names)if common_names is not None else None,'synthetic_not_before':not_before,'synthetic_cutoff':cutoff,'synthetic_valid':valid,'synthetic_weak':weak,'synthetic_anchored':anchored,'synthetic_is_ca':is_ca,'synthetic_ku':ku,'synthetic_ski':SKI,'synthetic_aki':AKI,'synthetic_force':forced,'synthetic_leaf_critical':leaf_critical,'synthetic_root_critical':root_critical,'policy_options':sorted(options),'native_result_flag':bool(mu.mem_read(OBJ+0x35,1)[0]),'failure_setter_attempts':failures,'calls':calls}

rows=[run('basic'),run('basic',root_critical=True),run('basic',leaf_critical=True),run('basic',valid=False),run('basic',valid=False,options=NATIVE_OPTIONS|{'ValidLeaf'}),run('basic',weak=True)]
assert [r['native_result_flag']for r in rows]==[True,True,False,True,False,False]
assert all({'unknown_critical':'root'}not in r['calls']for r in rows)
assert all({'unknown_critical':'leaf'}in r['calls']for r in rows[:4])
print(json.dumps({'cache_sha256':CACHE_SHA,'securityd_sha256':DAEMON_SHA,'scope':'original basic processor under synthetic direct anchored two-cert metadata; no signature or full trust','cases':rows},indent=2))
