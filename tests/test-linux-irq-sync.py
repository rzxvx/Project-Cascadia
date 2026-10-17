#!/usr/bin/env python3
"""Pinned actual GPL IRQ bodies, explicit Fake APIs; never touch an IRQ/device."""
import hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];HERE=ROOT/'tests/fixtures/linux-irq-sync'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def function(source,signature):
 start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
assert digest(HERE/'actual_irq.c')=='a3012aa71be10e2f441bc1cfac851b3b0f39f67c4f2158f86406f7d45d4045c0'
assert digest(HERE/'source-test-results.json')=='d064b7e04b5a391641bfe8247d737407d1ad77a8cfb0400506b78c19a13cc0db'
record=json.loads((HERE/'source-test-results.json').read_text());fixture=(HERE/'actual_irq.c').read_text()
aic=ROOT/'patches/files/drivers/irqchip/irq-apple-aic1.c';assert digest(aic)==record['source_hashes']['irq-apple-aic1.c'];driver=aic.read_text()
assert 'irq_get_irqchip_state' not in re.search(r'static struct irq_chip aic1_chip = \{([^}]+)\};',driver).group(1)
assert 'irq_domain_add_linear(node, aic->nr_irq,'in driver
signatures={'/* ACTUAL_LINUX_IRQ_GET_STATE */':'int __irq_get_irqchip_state(','/* ACTUAL_LINUX_SYNCHRONIZE_HARDIRQ */':'static void __synchronize_hardirq(','/* ACTUAL_LINUX_SYNCHRONIZE_IRQ */':'static void __synchronize_irq('}
manage=ROOT/'build/linux/kernel/irq/manage.c'
if manage.exists():assert digest(manage)==record['source_hashes']['kernel/irq/manage.c']
for marker,signature in signatures.items():
 body=function(fixture,signature);assert hashlib.sha256(body.encode()).hexdigest()==record['actual_source_bodies_sha256'][marker]
 if manage.exists():assert body==function(manage.read_text(),signature)
env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
with tempfile.TemporaryDirectory(prefix='cascadia-irq-sync-')as temp:
 for hierarchy,expected in [(False,189),(True,192)]:
  exe=Path(temp)/('hierarchy'if hierarchy else'linear')
  args=['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
  if hierarchy:args+=['-DCONFIG_IRQ_DOMAIN_HIERARCHY=1']
  subprocess.run([*args,str(HERE/'actual_irq.c'),'-o',str(exe)],check=True,timeout=30)
  result=subprocess.run([str(exe)],check=True,capture_output=True,text=True,env=env,timeout=10)
  assert str(expected)in result.stdout,result.stdout
  print(result.stdout,end='')
print('PASS exact GPL IRQ functions/current linear AIC; ASan/UBSan; no real IRQ or device calls')
