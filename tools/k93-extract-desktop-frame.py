#!/usr/bin/env python3
"""Decode bounded guest PPM capture; format conversion only, no image editing."""
import argparse
import base64
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
HEADER=b'P6\n1024 768\n255\n'
SIZE=len(HEADER)+1024*768*3

def decode(text):
    text=text.replace('\r','')
    for marker in ['K93_DESKTOP_PROCESSES_ALIVE\n','K93_FB_READ 1024x768 bpp32 stride4096 RGB16/8/0\n',
                   'K93_DESKTOP_END\n','K93_DESKTOP_CLEANUP\n','DESKTOP_EXIT=0\n']:
        assert marker in text,'Incomplete desktop evidence: '+marker.strip()
    start='\nK93_FB_PPM_BEGIN\n';end='\nK93_FB_PPM_END\n'
    assert text.count(start)==1 and text.count(end)==1
    body=text.split(start,1)[1].split(end,1)[0]
    return decode_payload(body)

def decode_payload(body):
    compressed=base64.b64decode(''.join(body.split()),validate=True)
    assert len(compressed)<2*1024*1024,'Unexpected capture size'
    with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
        ppm=stream.read(SIZE+1)
    assert len(ppm)==SIZE and ppm.startswith(HEADER),'Invalid framebuffer PPM'
    return ppm

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('experiment',type=Path)
    parser.add_argument('--output-prefix',default='desktop')
    args=parser.parse_args();experiment=args.experiment.resolve()
    assert experiment.is_relative_to(ROOT/'build/k93') and experiment.is_dir()
    assert re.fullmatch(r'[a-zA-Z0-9_-]+',args.output_prefix)
    log=experiment/'desktop-serial.log';ppm=decode(log.read_text())
    prefix=experiment/args.output_prefix
    raw=prefix.with_suffix('.ppm');png=prefix.with_suffix('.png');proof=prefix.with_suffix('.json')
    assert all(not p.exists() for p in [raw,png,proof]),'Preserve previous capture'
    with raw.open('xb') as output:output.write(ppm)
    subprocess.run(['magick',str(raw),str(png)],check=True,timeout=15)
    result={'width':1024,'height':768,'format':'RGB PPM from read-only guest fb0',
            'ppm_sha256':hashlib.sha256(ppm).hexdigest(),
            'png_sha256':hashlib.sha256(png.read_bytes()).hexdigest(),
            'serial_sha256':hashlib.sha256(log.read_bytes()).hexdigest(),
            'visual_review_required':True,'touch_verified':False,'persistent_install':False}
    with proof.open('x') as output:json.dump(result,output,indent=2);output.write('\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
