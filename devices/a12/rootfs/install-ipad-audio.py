#!/usr/bin/env python3
"""Install J171 audio policy into a selected root, never contact or reboot a device."""
import argparse,json,os,shutil,xml.etree.ElementTree as ET
from pathlib import Path
HERE=Path(__file__).resolve().parent
POWER_HELPER='''def cascadia_power_key_device(name):
    if name == 'gpio-keys':
        return True
    if name != 'Apple SMC power/lid events':
        return False
    try:
        return b'apple,j171a' in Path('/proc/device-tree/compatible').read_bytes().split(b'\\0')
    except OSError:
        return False


'''
def safe(root,relative):
 p=root/relative
 if not p.parent.resolve().is_relative_to(root) or p.is_symlink():
  raise ValueError('installation path escapes root or is a symlink: '+relative)
 return p

def install(root,device,desktop='auto',attenuation=4):
 # A no-op before root inspection for every other board.
 if device!='ipad8':return {'device':device,'installed':False}
 root=root.resolve()
 if not (root/'etc/passwd').is_file():raise ValueError('not a prepared Linux root')
 if attenuation not in (1,2,4):raise ValueError('divisor must be1,2,4')
 policy=safe(root,'etc/cascadia/ipad8-audio.conf')
 if policy.exists():
  values=[line.split('=',1)[1] for line in policy.read_text().splitlines() if line.startswith('attenuation=')]
  if len(values)!=1 or values[0] not in ('1','2','4'):raise ValueError('Invalid existing attenuation policy')
  attenuation=int(values[0])
 omarchy=(root/'home/tablet/.local/share/omarchy-ipad/shell/shell.qml').is_file()
 if desktop=='omarchy' and not omarchy:raise ValueError('Omarchy shell not installed')
 omarchy=omarchy and desktop!='none'
 if omarchy and not (root/'usr/local/bin/omarchy-volume').is_file() and not (root/'home/tablet/.local/share/omarchy-ipad/bin/omarchy-volume').is_file():
  raise ValueError('This Omarchy version lacks omarchy-volume; install v0.3 userspace first')
 # Preflight all destination paths before copying anything.
 overlay=HERE/'audio-overlay'
 files=[p for p in overlay.rglob('*') if p.is_file() and (omarchy or 'omarchy-platform' not in str(p))]
 for src in files:safe(root,str(src.relative_to(overlay)))
 # Retire only the known research-era filenames, never unknown user policy.
 legacy=[]
 for parent in ['etc/pipewire/pipewire.conf.d','etc/wireplumber/wireplumber.conf.d']:
  previous=safe(root,parent+'/90-j171-speaker-test.conf')
  current=overlay/parent/'90-j171-speakers.conf'
  if previous.exists():
   normalized=previous.read_text().replace('iPad 8 Speakers (test)','iPad 8 Speaker Hardware')
   if normalized!=current.read_text():raise ValueError('Unknown legacy audio configuration: '+str(previous))
   legacy.append(previous)
 daemon=safe(root,'usr/libexec/omarchy-powerd')
 patched=None
 if omarchy and daemon.is_file():
  text=daemon.read_text()
  old="            if name != 'gpio-keys' or path in self.keys.values():"
  if old in text:
   patched=text.replace('class Buttons:',POWER_HELPER+'class Buttons:').replace(old,"            if not cascadia_power_key_device(name) or path in self.keys.values():")
  elif 'def cascadia_power_key_device(' not in text and 'def power_key_device(' not in text:
   raise ValueError('Unknown powerd version; review before patching')
 for src in files:
  rel=str(src.relative_to(overlay));dst=safe(root,rel);dst.parent.mkdir(parents=True,exist_ok=True)
  if rel.endswith('omarchy-platform/audio.json') and dst.exists():
   new=json.loads(src.read_text());current=json.loads(dst.read_text())
   for key in ('hidden','replaced'):
    current.setdefault(key,[])
    for entry in new[key]:
     if entry not in current[key]:current[key].append(entry)
   dst.write_text(json.dumps(current,indent=2)+'\n')
  else:shutil.copyfile(src,dst)
  dst.chmod(0o755 if '/sbin/' in rel else 0o644)
 for previous in legacy:
  backup=previous.with_suffix('.conf.before-cascadia-audio')
  if not backup.exists():shutil.copy2(previous,backup)
  previous.unlink()
 conf=safe(root,'etc/cascadia/ipad8-audio.conf');conf.parent.mkdir(parents=True,exist_ok=True)
 if not conf.exists():conf.write_text('attenuation='+str(attenuation)+'\n');conf.chmod(0o600)
 link=root/'etc/systemd/system/multi-user.target.wants/ipad8-audio-enable.service'
 if not link.parent.resolve().is_relative_to(root):raise ValueError('unit parent escapes root')
 link.parent.mkdir(parents=True,exist_ok=True)
 if not link.is_symlink() and link.exists():raise ValueError('existing non-symlink unit')
 if link.is_symlink():
  if link.readlink()!=Path('../ipad8-audio-enable.service'):raise ValueError('unexpected unit link')
 else:link.symlink_to('../ipad8-audio-enable.service')
 if patched is not None:
  backup=daemon.with_name('omarchy-powerd.before-cascadia-audio')
  if not backup.exists():shutil.copy2(daemon,backup)
  daemon.write_text(patched);daemon.chmod(0o755)
 if omarchy:
  cfg=safe(root,'home/tablet/.config/omarchy-ipad/shell.json')
  if cfg.exists():
   data=json.loads(cfg.read_text());data['disabledPlugins']=[x for x in data.get('disabledPlugins',[]) if x!='omarchy.osd']
   layout=data.setdefault('bar',{}).setdefault('layout',{})
   if not any(x.get('id')=='omarchy.audio' for items in layout.values() if isinstance(items,list) for x in items if isinstance(x,dict)):
    layout.setdefault('right',[]).append({'id':'omarchy.audio'})
   cfg.write_text(json.dumps(data,indent=2)+'\n')
  rc=safe(root,'home/tablet/.config/labwc/rc.xml')
  if rc.exists():
   tree=ET.parse(rc);k=tree.getroot().find('keyboard')
   if k is None:raise ValueError('No labwc keyboard configuration')
   for key,action in [('XF86AudioRaiseVolume','raise'),('XF86AudioLowerVolume','lower'),('XF86AudioMute','mute-toggle')]:
    for n in list(k):
     if n.tag=='keybind' and n.get('key')==key:k.remove(n)
    n=ET.SubElement(k,'keybind',{'key':key,'allowWhenLocked':'yes'})
    ET.SubElement(n,'action',{'name':'Execute','command':'omarchy-volume '+action})
   tree.write(rc,encoding='UTF-8',xml_declaration=True)
  if not (root/'usr/local/bin/omarchy-volume').exists() and not (root/'home/tablet/.local/share/omarchy-ipad/bin/omarchy-volume').is_file():
   raise ValueError('This Omarchy version lacks omarchy-volume; install v0.3 userspace first')
 return {'device':device,'installed':True,'omarchy':omarchy,'attenuation_default':attenuation}
if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--root',type=Path,required=True);p.add_argument('--device',choices=['ipad8','xr','xsmax'],required=True);p.add_argument('--desktop',choices=['auto','omarchy','none'],default='auto');p.add_argument('--attenuation',type=int,choices=[1,2,4],default=4);a=p.parse_args()
 print(json.dumps(install(a.root,a.device,a.desktop,a.attenuation),indent=2))
