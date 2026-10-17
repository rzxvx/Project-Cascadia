from pathlib import Path
import importlib.util,sys,re,tempfile,subprocess,os,resource,hashlib
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
sys.dont_write_bytecode=True
root=Path(__file__).resolve().parents[1];g=root/'tests/fixtures/h2fmi-lifetime';experiment=root/'experiments/p105-h2fmi-lifetime';spec=importlib.util.spec_from_file_location('lifetime',root/'tests/test-h2fmi-lifetime.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
for name,expected in mod.HISTORICAL_HASHES.items():
 assert hashlib.sha256((g/name).read_bytes()).hexdigest()==expected,name
template=(g/'fixture.c.in').read_text()
LIST=r'''
struct list_head {struct list_head *next,*prev;};
static void INIT_LIST_HEAD(struct list_head*h){h->next=h->prev=h;}
static bool list_empty(struct list_head*h){return h->next==h;}
static void list_add_tail(struct list_head*n,struct list_head*h){n->prev=h->prev;n->next=h;h->prev->next=n;h->prev=n;}
static void list_del(struct list_head*n){n->prev->next=n->next;n->next->prev=n->prev;INIT_LIST_HEAD(n);}
'''
template=template.replace('struct mtd_info {',LIST+'\nstruct mtd_info {')
template=template.replace('struct kref refcnt;int index;','struct {struct mutex partitions_lock;}master;struct list_head partitions;struct{struct list_head node;u64 offset,size;}part;struct kref refcnt;int index;')
template=template.replace('struct mtd_partition {int unused;};','struct mtd_partition {const char*name;u64 offset,size;const char *const*types;};\n#define MTDPART_SIZ_FULL (0)\n#define MTDPART_OFS_APPEND (-1)\n#define MTDPART_OFS_NXTBLK (-2)\n#define KERN_NOTICE ""\n#define pr_err(...) ((void)0)\nstatic unsigned attrs;\nstatic void mtd_add_partition_attrs(struct mtd_info*m){assert(record(m)->live);attrs++;}')
template=template.replace('static void mtd_set_dev_defaults(struct mtd_info*m){(void)m;}','static void mtd_set_dev_defaults(struct mtd_info*m){INIT_LIST_HEAD(&m->partitions);mutex_init(&m->master.partitions_lock);}')
template=template.replace('static void free_partition(struct mtd_info*m){kfree(m->name);kfree(m);}','')
template=template.replace('static unsigned partition_releases;','static unsigned partition_releases;\nstatic unsigned duplicate_retirements;\nstatic bool quarantine;')
template=template.replace('#define WARN_ON(c) (c)','#define WARN_ON(c) fake_warn(c)')
# Preserve owned allocation storage in the historical-only countercontrols;
# never dereference genuinely freed memory or perform a repeated real free.
old='static void kfree(const void*p) {if(!p)return;struct allocation*r=record(p);assert(r->live);r->live=r->logical_live=false;free((void*)p);}'
new='static unsigned duplicate_retirements;static bool quarantine;\nstatic void kfree(const void*p) {if(!p)return;struct allocation*r=record(p);if(!r->live){assert(quarantine);duplicate_retirements++;return;}r->live=r->logical_live=false;if(!quarantine)free((void*)p);}'
assert old in template;template=template.replace(old,new)
template=template.replace('static int add_mtd_partitions(struct mtd_info*m,const struct mtd_partition*p,int n){(void)m;(void)p;(void)n;assert(!"no fallback partitions in H2FMI");return -EINVAL;}','int add_mtd_partitions(struct mtd_info*,const struct mtd_partition*,int);')
allocator=r'''
static struct mtd_info *allocate_partition(struct mtd_info*p,const struct mtd_partition*part,int number,u64 offset){
 (void)number;(void)offset;struct mtd_info*m=kzalloc(sizeof(*m),0);assert(m);
 m->parent=p;m->name=kasprintf(0,"part%d",0);m->type=p->type;m->flags=p->flags;m->size=part->size;m->part.size=part->size;m->part.offset=part->offset;
 m->writesize=p->writesize;m->erasesize=p->erasesize;m->owner=p->owner;m->_read_oob=p->_read_oob;
 INIT_LIST_HEAD(&m->part.node);INIT_LIST_HEAD(&m->partitions);return m;
}
'''
template=template.replace('static void idr_remove(void*p,int i){(void)p;', 'static void idr_remove(void*p,int i){if(!HISTORICAL_PARTITION)assert(fake_table_owned);(void)p;')

old_del='static int del_mtd_partitions(struct mtd_info*m){if(m->index==fail_unregister)return -EIO;if(partition_child&&partition_child->parent==m){struct mtd_info *child=partition_child;partition_child=NULL;return del_mtd_device(child);}return 0;}'
new_del='static int del_mtd_partitions(struct mtd_info*m){if(m->index==fail_unregister)return -EIO;while(!list_empty(&m->partitions)){struct mtd_info*child=container_of(m->partitions.next,struct mtd_info,part.node);list_del(&child->part.node);int r=del_mtd_device(child);if(r)return r;}return 0;}'
assert old_del in template;template=template.replace(old_del,new_del)
old_parse='static int parse_mtd_partitions(struct mtd_info*m,const char *const*t,struct mtd_part_parser_data*d){(void)t;(void)d;if(open_during_probe&&m->index==open_die&&m->dev.registered&&!retained){assert(!__get_mtd_device(m));retained=m;}return m->index==fail_parse?-EPROBE_DEFER:0;}'
new_parse='static bool remove_during_parse,fail_after_parse;\nstatic int parse_mtd_partitions(struct mtd_info*m,const char *const*t,struct mtd_part_parser_data*d){(void)t;(void)d;if(mtd_is_partition(m)){if(remove_during_parse){list_del(&m->part.node);assert(!del_mtd_device(m));return 0;}if(fail_after_parse)return-EPROBE_DEFER;}return m->index==fail_parse?-EPROBE_DEFER:0;}'
assert old_parse in template;template=template.replace(old_parse,new_parse)

template=template.replace('/* ACTUAL_FUNCTIONS */',allocator+'\n/* ACTUAL_FUNCTIONS */')
def function(s,n):
 match=re.search(r'\n(?:static(?: inline)? )?(?:int|void)[^\n]*\b'+re.escape(n)+r'\(',s);assert match,n;a=match.start()+1;b=s.index('{',a);level=1;e=b+1
 while level:level+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
MAIN=r'''
int main(int argc,char**argv){
 assert(argc==2);fresh();quarantine=HISTORICAL_PARTITION;duplicate_retirements=attrs=0;
 struct platform_device dev={0};assert(!h2fmi_probe(&dev));struct h2fmi*f=dev.data;struct mtd_info*parent=&f->mtd[0];

#if !HISTORICAL_PARTITION

 if(!strcmp(argv[1],"parse-remove")){
  remove_during_parse=true;struct mtd_partition p={.name="parser",.size=32768};assert(!add_mtd_partitions(parent,&p,1));assert(attrs==1&&list_empty(&parent->partitions)&&!table[4]&&partition_releases==1&&!duplicate_retirements);h2fmi_remove(&dev);cleanup_fixture();puts("PASS constructor retains child through actual last offset access after parser removes device");return 0;
 }
 if(!strcmp(argv[1],"parse-error")){
  fail_after_parse=true;struct mtd_partition p={.name="parser-error",.size=32768};assert(add_mtd_partitions(parent,&p,1)==-EPROBE_DEFER);assert(list_empty(&parent->partitions)&&!table[4]&&partition_releases==1&&!duplicate_retirements);h2fmi_remove(&dev);cleanup_fixture();puts("PASS post-publication parse error drops constructor then rolls back registered child once");return 0;
 }
 if(!strcmp(argv[1],"default")){
  h2fmi_remove(&dev);fresh();quarantine=false;struct mtd_info*m=kzalloc(sizeof(*m),0);m->name=kasprintf(0,"default%d",0);m->writesize=16384;m->erasesize=1048576;m->size=2097152;m->flags=MTD_NO_ERASE;m->no_nvmem=true;
  assert(!add_mtd_device(m));assert(!m->driver_device_ref&&!m->partition_constructor_ref&&!m->driver_parent_ref&&!fake_work_count);assert(!__get_mtd_device(m));__put_mtd_device(m);assert(!del_mtd_device(m));assert(!m->dev.type&&!table[0]&&!fake_work_count&&record(m)->live);kfree(m->name);kfree(m);cleanup_fixture();puts("PASS non-opt-in successful get/put/unregister retains original caller ownership and cleared dev");return 0;
 }
 if(!strcmp(argv[1],"nested")){
  struct mtd_partition p={.name="parent",.size=32768};assert(!add_mtd_partitions(parent,&p,1));struct mtd_info*mid=table[4];assert(mid);
  assert(!add_mtd_partitions(mid,&p,1));struct mtd_info*leaf=table[5];assert(leaf);get_device(&leaf->dev);
  list_del(&leaf->part.node);assert(!del_mtd_device(leaf));list_del(&mid->part.node);assert(!del_mtd_device(mid));
  h2fmi_remove(&dev);devres_cleanup();assert(record(f)->live&&record(mid)->live&&record(leaf)->live);assert(mtd_get_master(leaf)==&f->mtd[0]);assert(__get_mtd_device(leaf)==-ENODEV);
  put_device(&leaf->dev);assert(!record(leaf)->live&&!record(mid)->live&&!record(f)->live&&!table[4]&&!table[5]);cleanup_fixture();puts("PASS nested external device retains complete parent chain; final deferred cleanup serialized");return 0;
 }
 if(!strcmp(argv[1],"retired-parent")){
  struct mtd_partition p={.name="parent",.size=32768};assert(!add_mtd_partitions(parent,&p,1));struct mtd_info*mid=table[4];get_device(&mid->dev);list_del(&mid->part.node);assert(!del_mtd_device(mid));
  assert(add_mtd_partitions(mid,&p,1)==-ENODEV);assert(list_empty(&mid->partitions)&&!table[5]&&record(mid)->live);put_device(&mid->dev);h2fmi_remove(&dev);cleanup_fixture();puts("PASS new child of unregistered retained parent refused with exact unwind");return 0;
 }
 if(!strcmp(argv[1],"early-idr")){
  fail_registration=4;struct mtd_partition p={.name="early",.size=32768};assert(add_mtd_partitions(parent,&p,1)==-EIO);assert(list_empty(&parent->partitions)&&!table[4]&&!partition_releases&&!duplicate_retirements);h2fmi_remove(&dev);cleanup_fixture();puts("PASS pre-initialize failure retains original caller allocation cleanup");return 0;
 }
 if(!strcmp(argv[1],"failed-get")){
  WRITE_ONCE(f->removed,true);struct mtd_partition p={.name="no-get",.size=32768};assert(add_mtd_partitions(parent,&p,1)==-ENODEV);assert(!table[4]&&list_empty(&parent->partitions)&&partition_releases==1&&!duplicate_retirements);h2fmi_remove(&dev);cleanup_fixture();puts("PASS pre-publication get rejection drops constructor under actual table-lock boundary");return 0;
 }
#endif
 fail_add=4;hold_failed_device=strstr(argv[1],"held")!=NULL;
 struct mtd_partition part={.name="fake-child",.size=32768};
 int ret=strstr(argv[1],"single")?mtd_add_partition(parent,"single",0,32768):add_mtd_partitions(parent,&part,1);assert(ret==-EIO);assert(list_empty(&parent->partitions));
 if(hold_failed_device){
  assert(failed_device);struct mtd_info*child=failed_device->data;
#if HISTORICAL_PARTITION
  assert(!record(child)->live&&failed_device->refs==1);
  puts("PASS historical caller logically retires allocation while one device reference remains; no invalid access executed");
#else
  assert(record(child)->live&&failed_device->refs==1);assert(__get_mtd_device(child)==-ENODEV);
  put_device(failed_device);assert(!table[4]&&!record(child)->live&&partition_releases==1);
  puts("PASS failed child publication retains allocation until final held device ref; unregistered get denied");
#endif
 }else{
#if HISTORICAL_PARTITION
  assert(duplicate_retirements==2);
  puts("PASS historical synchronous error unwind attempts repeated logical retirement; owned storage quarantined");
#else
  assert(!table[4]&&partition_releases==1&&!duplicate_retirements);
  puts("PASS synchronous failed child allocation destroyed once after caller unlink");
#endif
 }
 if(quarantine){for(unsigned i=0;i<allocation_count;i++)free(allocations[i].p);}
 else {h2fmi_remove(&dev);cleanup_fixture();}
 return 0;
}
'''
flags=['-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-Wno-sign-compare','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'];env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
# Historical source supplied from the caller-mounted existing frozen proposal.
oldcore=(g/'prior-lifetime-mtdcore.c').read_text();oldpart=(g/'original-mtdpart.c').read_text()
for partitioned in ['0','1']:
 with tempfile.TemporaryDirectory()as td:
  td=Path(td)
  for name,relative in [('original-mtdcore.c','drivers/mtd/mtdcore.c'),('original-mtdpart.c','drivers/mtd/mtdpart.c'),('original-mtd.h','include/linux/mtd/mtd.h')]:
   destination=td/relative;destination.parent.mkdir(parents=True,exist_ok=True);destination.write_bytes((g/name).read_bytes())
  subprocess.run(['git','apply','--whitespace=error',str(experiment/'mtd-context-lifetime.patch')],cwd=td,check=True,timeout=20)
  assert (td/'drivers/mtd/mtdpart.c').read_bytes()==(experiment/'mtdpart.c').read_bytes()
  for label,core,part in [('historical',oldcore,oldpart),('fixed',(td/'drivers/mtd/mtdcore.c').read_text(),(experiment/'mtdpart.c').read_text())]:
   if os.environ.get('ONLY_CASE') and label=='historical':continue
   t=template
   geometry=(experiment/'mtd-apple-h2fmi.c').read_text();t=t.replace('/* GEOMETRY */',geometry[geometry.index('#define H2FMI_BUSES'):geometry.index('/* The ADT')]+geometry[geometry.index('#define H2FMI_CE'):geometry.index('/* The allowlist is')]);t=t.replace('/* DRIVER_STRUCTS */',geometry[geometry.index('struct h2fmi_bus {'):geometry.index('/*\n * h2fmi_wait')].replace('__iomem',''))
   names=mod.CORE_FUNCTIONS if label=='historical'else['mtd_release_final','mtd_release_work',*mod.CORE_FUNCTIONS]
   bodies=[function(part,'free_partition')]
   if label=='fixed':bodies +=[function(part,'put_partition_constructor'),function(part,'free_unpublished_partition')]
   bodies +=[function(core,n)for n in names]
   bodies +=[function(geometry,n)for n in ['h2fmi_split_page','h2fmi_read_page_at','h2fmi_bitflips','h2fmi_account_ecc','h2fmi_mtd_read_oob_locked','h2fmi_mtd_block_isbad_locked','h2fmi_release','h2fmi_get_device','h2fmi_put_device','h2fmi_mtd_read_oob','h2fmi_mtd_block_isbad','h2fmi_unregister','h2fmi_setup_mtd','h2fmi_probe','h2fmi_remove']]
   bodies +=[function(part,'mtd_add_partition'),function(part,'add_mtd_partitions')]
   t=t.replace('/* FORWARD_CORE */',mod.FORWARD_CORE).replace('/* ACTUAL_FUNCTIONS */','\n\n'.join(bodies)+mod.REGISTER_WRAPPER)
   t=t.replace('int main(void){','int old_fixture_main(void){');t += MAIN
   if label=='historical':t=t.replace(' put_partition_constructor(child);',' /* absent old constructor ownership */')
   src=td/(label+'.c');src.write_text(t);exe=td/label
   subprocess.run(['cc',*flags,'-DPARTITIONED_MASTER='+partitioned,'-DHISTORICAL_PARTITION='+str(int(label=='historical')),str(src),'-o',str(exe)],check=True,timeout=60)
   for case in ([os.environ['ONLY_CASE']] if os.environ.get('ONLY_CASE') else ['sync','held'] if label=='historical'else['sync','held','single-sync','single-held','nested','retired-parent','early-idr','failed-get','parse-remove','parse-error','default']):subprocess.run([str(exe),case],check=True,timeout=20,env=env)
