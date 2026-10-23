from pathlib import Path
import json,subprocess,hashlib,tempfile
HERE=Path(__file__).resolve().parent
ORACLE=HERE/'oracle-digital-id.json'
assert hashlib.sha256(ORACLE.read_bytes()).hexdigest()=='069bcc457b479eba20ae1ce45ebd4060b9aa67ae159a038ea1405698c0e40aa6'
x=json.loads(ORACLE.read_text());cases=x['cases']
lines=[]
for e in cases:
 values=list(bytes.fromhex(e['digital_id']))
 lines.append(' '.join([*(f'{b:x}'for b in values),str(int(e['debugger_permission'])),str(e['method3e8']),str(e['prior_byte7ed'])]))
with tempfile.TemporaryDirectory(prefix='cascadia-id-') as temp:
 binary=Path(temp)/'fixture'
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(HERE/'include'),'-I'+str(HERE),str(HERE/'cascadia-accessory-id.c'),str(HERE/'fixture.c'),'-o',str(binary)],check=True)
 r=subprocess.run([str(binary)],input='\n'.join(lines)+'\n',text=True,capture_output=True,check=True)
outputs=r.stdout.splitlines();assert len(outputs)==len(cases)
for i,(e,row)in enumerate(zip(cases,outputs)):
 error,valid,first,second,flags=row.split();error=int(error);valid=int(valid)
 if e['decoder_valid']:
  assert error==0 and valid==1,(i,e,row)
  assert [int(first,16),int(second,16)]==[int(v,16)for v in e['selectors']],(i,e,row)
  assert int(flags,16)==int(e['USBd_low_flags'],16),(i,e,row)
 else:assert error<0 and valid==0 and int(first,16)==int(second,16)==int(flags,16)==0,(i,e,row)
print(json.dumps({'status':'PASS','native_semantic_cases':len(cases),'transport_native_cases':len(x['transport_experiments']),'strict_C_error_groups':'frame malformed/status/marker/null/alias; stale records and unknown required policy; native invalid records cleared','oracle_sha256':hashlib.sha256(ORACLE.read_bytes()).hexdigest()}))
