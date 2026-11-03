#!/usr/bin/env python3
"""Run the actual offline page JS with a tiny DOM fixture; not browser rendering."""
from pathlib import Path
import subprocess

page=(Path(__file__).resolve().parents[1]/'rootfs/k93/browser-test.html').read_text()
script=page.split('<script>',1)[1].split('</script>',1)[0]
fixture=r'''
const assert = require('node:assert/strict');
const elements = Object.fromEntries(['entry','check','result','engine'].map(name =>
  ['#'+name,{value:'',textContent:'',focus(){},addEventListener(type,fn){this[type]=fn;}}]));
global.document={title:'loading',querySelector(name){assert.ok(elements[name]);return elements[name];}};
'''
checks=r'''
setImmediate(() => {
  assert.equal(document.title,'K93_JS_READY');
  assert.match(elements['#engine'].textContent,/PASS/);
  elements['#entry'].value='wrong'; elements['#check'].click();
  assert.equal(document.title,'K93_INPUT_FAIL');
  elements['#entry'].value='k93-input'; elements['#check'].click();
  assert.equal(document.title,'K93_INPUT_PASS');
  assert.equal(elements['#result'].textContent,'Keyboard and button: PASS');
});
'''
subprocess.run(['node','-'],input=fixture+script+checks,text=True,check=True,timeout=10)
negative=script.replace('!== 14','!== 15')
subprocess.run(['node','-'],input=fixture+negative+"\nsetImmediate(() => assert.equal(document.title,'K93_JS_FAIL'));",text=True,check=True,timeout=10)
print('PASS: actual page JS/DOM transitions, input success/failure and JS failure; no rendering or physical-input claim')
