"""Bounded composition: pinned audited signer emulator, explicit THS 32/32 windows."""
import sys,hashlib,json,argparse
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('signer_emulator',type=Path);ap.add_argument('firmware_directory',type=Path);ap.add_argument('--source-root',type=Path,required=True);args=ap.parse_args()
p=args.signer_emulator;s=p.read_text();h=hashlib.sha256(p.read_bytes()).hexdigest();assert h=='d33a063308d4026e6cc21b5af1bb5e7ffad264ef2b688b1b297198fa18a95c23'
sys.argv=[str(p),str(args.firmware_directory),'--source-root',str(args.source_root)]
x={};exec(compile(s.split('\nrows=')[0],str(p),'exec'),x)
rows=[x['run'](2,max_tx=32,max_rx=32),x['run'](3,max_tx=32,max_rx=32)]
for r in rows:
 assert r['return']=='0x0'
 assert bytes.fromhex(r['output'])[:r['output_size']]==bytes(range(r['output_size']))
assert [t['tx']for t in rows[1]['transfers'][:2]]==['9400000010'+bytes(range(16)).hex(),'9400100010'+bytes(range(16,32)).hex()]
assert len(rows[0]['transfers'])==9 and len(rows[1]['transfers'])==8
print(json.dumps({'source_sha256':x['SHA'],'signer_emulator_sha256':h,'scope':'original signer method + synthetic provider advertised 32/32, matching independently emulated THS capability. No real provider ownership/transport/electrical access','cases':rows},indent=2))
