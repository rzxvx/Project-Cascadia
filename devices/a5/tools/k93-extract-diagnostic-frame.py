#!/usr/bin/env python3
"""Decode failure-only framebuffer image. NEVER establishes browser success."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('frame',Path(__file__).with_name('k93-extract-desktop-frame.py'))
frame=importlib.util.module_from_spec(spec);spec.loader.exec_module(frame)

def decode(text):
    text=text.replace('\r','')
    assert '\nK93_BROWSER_STARTUP_SNAPSHOT\n' in text
    assert '\nK93_DIAGNOSTIC_FRAME_UPTIME\n' in text
    assert '\nK93_FB_READ 1024x768 bpp32 stride4096 RGB16/8/0\n' in text
    start='\nK93_DIAGNOSTIC_FB_PPM_BEGIN\n';end='\nK93_DIAGNOSTIC_FB_PPM_END\n'
    assert text.count(start)==1 and text.count(end)==1
    return frame.decode_payload(text.split(start,1)[1].split(end,1)[0])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('experiment',type=Path)
    args=parser.parse_args();experiment=args.experiment.resolve()
    assert experiment.is_relative_to(ROOT/'build/k93') and experiment.is_dir()
    log=experiment/'desktop-serial.log';ppm=decode(log.read_text())
    raw=experiment/'diagnostic.ppm';png=experiment/'diagnostic.png';proof=experiment/'diagnostic.json'
    assert all(not p.exists() for p in (raw,png,proof)),'Preserve existing evidence'
    with raw.open('xb') as stream:stream.write(ppm)
    subprocess.run(['magick',str(raw),str(png)],check=True,timeout=15)
    result={'diagnostic_only':True,'width':1024,'height':768,
            'ppm_sha256':hashlib.sha256(ppm).hexdigest(),
            'png_sha256':hashlib.sha256(png.read_bytes()).hexdigest(),
            'serial_sha256':hashlib.sha256(log.read_bytes()).hexdigest(),
            'visual_review_required':True,'page_js_verified':False,
            'xtest_input_verified':False,'touch_verified':False,'persistent_install':False}
    with proof.open('x') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
