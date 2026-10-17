/* Every process/filesystem syscall is a strict fake: never control host PIDs. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>
static int f_open(const char *, int, ...), f_openat(int,const char *,int,...);
static int f_close(int), f_fstat(int,struct stat *), f_fstatat(int,const char *,struct stat *,int);
static ssize_t f_read(int,void *,size_t), f_write(int,const void *,size_t);
static int f_poll(struct pollfd *,nfds_t,int), f_flock(int,int), f_fsync(int), f_fchmod(int,mode_t);
static int f_linkat(int,const char *,int,const char *,int), f_unlinkat(int,const char *,int);
static long f_syscall(long,...);
static uid_t f_geteuid(void); static pid_t f_getpid(void);
static pid_t forbidden_fork(void) { abort(); }
static int forbidden_ioctl(int fd, unsigned long op, ...) { (void)fd; (void)op; abort(); }
static int forbidden_socketpair(int a,int b,int c,int d[2]) { (void)a;(void)b;(void)c;(void)d;abort(); }
static pid_t forbidden_waitpid(pid_t p,int *s,int o) { (void)p;(void)s;(void)o;abort(); }
static void (*forbidden_signal(int s,void (*fn)(int)))(int) { (void)s;(void)fn;abort(); }
#define fork forbidden_fork
#define ioctl forbidden_ioctl
#define socketpair forbidden_socketpair
#define waitpid forbidden_waitpid
#define signal forbidden_signal
#define open f_open
#define openat f_openat
#define close f_close
#define fstat f_fstat
#define fstatat f_fstatat
#define read f_read
#define write f_write
#define poll f_poll
#define flock f_flock
#define fsync f_fsync
#define fchmod f_fchmod
#define linkat f_linkat
#define unlinkat f_unlinkat
#define syscall f_syscall
#define geteuid f_geteuid
#define getpid f_getpid
#include "../tools/nand/iosnand-process.h"
#define main iosnand_device_main
#include "../tools/nand/iosnand.c"
#undef main
/* No production NAND backend can be reached even after a test regression. */
int ppn_open(void) { abort(); }
int ppn_claim(int b) { (void)b; abort(); }
int ppn_lock(int b) { (void)b; abort(); }
int ppn_read_page(int b, int c, uint32_t r, uint8_t *p, uint32_t n, uint8_t *st)
{ (void)b; (void)c; (void)r; (void)p; (void)n; (void)st; abort(); }

static char record[512], stattext[512], command[128], written[512];
static size_t cmdlen, offset[32], nwrite;
static int kernel_pid, workerchange, ready_missing, pollerror, recordtype, recordopenerror, kernel_size, kernel_ro;
static char worker_record[256], worker_stat[512], kernel_text[64];
static int uid, directory_mode, record_mode, record_links, record_uid;
static int dead_at, polls, pidcalls, signalcalls, signalerrno, syscallerrno;
static int exechange, startchange, cmdwrong, procerror, readerror, closeerror;
static int linkerror, unlinkerror, syncerror, lockerror, chmoderror, writeerror, tempcollision;
static int links, unlinks, templinks, locked, procopens, closes, binarychange;
static void reset(void)
{
    strcpy(record,"IOSNANDPID1 42 1234 7 8\n");
    strcpy(worker_record,"IOSNANDREADY1 43 1235 7 8 32768\n");
    strcpy(worker_stat,"43 (worker) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 1235 0\n");
    kernel_pid=43; kernel_size=32768; kernel_ro=1; workerchange=ready_missing=pollerror=recordopenerror=0; recordtype=S_IFREG;
    strcpy(stattext,"42 (name has (parens)) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 1234 0\n");
    const char cmd[]="/sbin/iosnand\0--pid-record\0/run/ios/pid\0serve\0System\0";
    memcpy(command,cmd,sizeof(cmd)); cmdlen=sizeof(cmd);
    memset(offset,0,sizeof(offset)); nwrite=0; written[0]=0;
    uid=record_uid=0; directory_mode=0700; record_mode=0600; record_links=1;
    dead_at=polls=pidcalls=signalcalls=signalerrno=syscallerrno=0;
    exechange=startchange=cmdwrong=procerror=readerror=closeerror=0;
    linkerror=unlinkerror=syncerror=lockerror=chmoderror=writeerror=tempcollision=0;
    links=unlinks=templinks=locked=procopens=closes=binarychange=0;
}
/* fd10 directory,11 record,12 procdir,13 stat,14 cmdline,15 binary,
 * 16 private temporary file,17 pidfd. Anything else is forbidden. */
static int f_open(const char *p,int flags,...)
{
    assert(flags&O_CLOEXEC); assert(flags&O_NOFOLLOW);
    if(!strcmp(p,"/run/ios")) {assert(flags&O_DIRECTORY); return 10;}
    if(!strcmp(p,"/proc/42")) {assert(pidcalls>=1); procopens++; if(procerror){errno=procerror;return -1;} return 12;}
    if(!strcmp(p,"/sbin/iosnand")) return 15;
    if(!strcmp(p,"/proc/43")) {procopens++;return 19;}
    if(!strcmp(p,"/sys/devices/virtual/block/nbd0")) return 23;
    fprintf(stderr,"unexpected open %s\n",p); abort();
}
static int f_openat(int d,const char *p,int flags,...)
{
    assert(flags&O_NOFOLLOW);
    int fd;
    if(d==12&&!strcmp(p,"stat")) fd=13;
    else if(d==12&&!strcmp(p,"cmdline")) fd=14;
    else if(d==10&&!strcmp(p,"pid")) {if(recordopenerror){errno=recordopenerror;return -1;}fd=11;}
    else if(d==19&&!strcmp(p,"stat")) fd=20;
    else if(d==19&&!strcmp(p,"cmdline")) fd=21;
    else if(d==23&&!strcmp(p,"pid")) fd=24;
    else if(d==23&&!strcmp(p,"size")) fd=25;
    else if(d==23&&!strcmp(p,"ro")) fd=26;
    else if(d==10&&!strcmp(p,"pid.ready.42.1234")) {if(ready_missing){errno=ENOENT;return -1;}fd=22;}
    else if(d==10&&!strncmp(p,"pid.ready.",10)) {errno=ENOENT;return -1;}
    else if(d==10&&!strncmp(p,".iosnand.",9)) {
        assert(locked && (flags&O_EXCL)&&(flags&O_CREAT));
        if(tempcollision) {errno=EEXIST;return -1;} fd=16;
    } else {fprintf(stderr,"unexpected openat %s\n",p);abort();}
    offset[fd-10]=0; return fd;
}
static int f_close(int fd)
{ assert(fd>=10&&fd<=26); closes++; if(fd==10)locked=0; if(closeerror==fd){errno=EIO;return -1;}return 0; }
static int f_fstat(int fd,struct stat *s)
{
    memset(s,0,sizeof(*s)); s->st_nlink=1; s->st_dev=7;s->st_ino=8;
    if(fd==10){s->st_mode=S_IFDIR|directory_mode;s->st_uid=uid;}
    else if(fd==11){s->st_mode=recordtype|record_mode;s->st_uid=record_uid;s->st_nlink=record_links;}
    else if(fd==12||fd==19||fd==23){s->st_mode=S_IFDIR|0500;s->st_uid=uid;}
    else if(fd==22){s->st_mode=recordtype|record_mode;s->st_uid=record_uid;s->st_nlink=record_links;}
    else if(fd==13||fd==14||fd==15||fd==20||fd==21||fd==24||fd==25||fd==26){s->st_mode=S_IFREG|0500;if(fd==15&&binarychange)s->st_ino++;}
    else abort();
    return 0;
}
static int f_fstatat(int d,const char *p,struct stat *s,int flags)
{assert((d==12||d==19)&&!strcmp(p,"exe")&&!flags); memset(s,0,sizeof(*s));s->st_mode=S_IFREG|0500;s->st_dev=7;s->st_ino=8+exechange+(d==19?workerchange:0);return 0;}
static ssize_t f_read(int fd,void *b,size_t n)
{
    if(readerror){errno=readerror;return -1;}
    const char *s; size_t len;
    if(fd==11){s=record;len=strlen(record);}
    else if(fd==13){s=stattext;len=strlen(stattext);}
    else if(fd==14||fd==21){s=command;len=cmdlen;}
    else if(fd==20){s=worker_stat;len=strlen(s);}
    else if(fd==22){s=worker_record;len=strlen(s);}
    else if(fd==24||fd==25||fd==26){snprintf(kernel_text,sizeof(kernel_text),"%d\n",fd==24?kernel_pid:(fd==25?kernel_size:kernel_ro));s=kernel_text;len=strlen(s);}
    else abort();
    size_t *off=&offset[fd-10];if(*off>=len)return 0;
    if(n>len-*off)n=len-*off;
    memcpy(b,s+*off,n);*off+=n;return n;
}
static ssize_t f_write(int fd,const void *b,size_t n)
{assert(fd==16&&locked);if(writeerror){errno=writeerror;return -1;}assert(nwrite+n<sizeof(written));memcpy(written+nwrite,b,n);nwrite+=n;written[nwrite]=0;return n;}
static int f_poll(struct pollfd *p,nfds_t n,int timeout)
{assert(n==1&&(p->fd==17||p->fd==18)&&!timeout);polls++;if(pollerror){p->revents=pollerror;return 1;}if(dead_at&&polls>=dead_at){p->revents=POLLIN;return 1;}return 0;}
static long f_syscall(long op,...)
{
    va_list ap;va_start(ap,op);
    if(op==SYS_pidfd_open){pid_t pid=va_arg(ap,pid_t);assert(pid==42||pid==43);assert(va_arg(ap,unsigned)==0);pidcalls++;va_end(ap);if(syscallerrno){errno=syscallerrno;return -1;}return pid==42?17:18;}
    assert(op==SYS_pidfd_send_signal);assert(va_arg(ap,int)==17);assert(va_arg(ap,int)==SIGTERM);assert(va_arg(ap,void*)==NULL);assert(!va_arg(ap,unsigned));va_end(ap);signalcalls++;if(signalerrno){errno=signalerrno;return -1;}return 0;
}
static uid_t f_geteuid(void){return uid;}
static pid_t f_getpid(void){return 42;}
static int f_flock(int fd,int op){assert(fd==10&&op==LOCK_EX);if(lockerror){errno=lockerror;return -1;}locked=1;return 0;}
static int f_fsync(int fd){assert(fd==10||fd==16);if(syncerror){errno=syncerror;return -1;}return 0;}
static int f_fchmod(int fd,mode_t mode){assert(fd==16&&mode==0600);if(chmoderror){errno=chmoderror;return -1;}return 0;}
static int f_linkat(int d,const char *p,int d2,const char *p2,int flags)
{assert(d==10&&d2==10&&!flags&&locked&&(!strcmp(p2,"pid")||!strcmp(p2,"pid.ready.42.1234")));assert(!strncmp(p,".iosnand.",9));links++;if(linkerror){errno=linkerror;return -1;}templinks++;return 0;}
static int f_unlinkat(int d,const char *p,int flags)
{assert(d==10&&!flags&&locked);unlinks++;if(unlinkerror){errno=unlinkerror;return -1;}assert(!strcmp(p,"pid")||!strncmp(p,".iosnand.",9));return 0;}
static unsigned cases;
#define CHECK(x) do{assert(x);cases++;}while(0)
int main(void)
{
    struct process_record r;uint64_t start;
    reset();CHECK(!process_parse_record(record,&r)&&r.pid==42&&r.start==1234);
    const char *bad[]={"42\n","0\n","-1\n","IOSNANDPID1 1 1 1 1\n","IOSNANDPID1 2147483648 1 1 1\n","IOSNANDPID1 42 1 1 1","IOSNANDPID1 42 1 1 1\nextra","IOSNANDPID1 42 18446744073709551616 1 1\n","IOSNANDPID1 42  1 1 1\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(process_parse_record(bad[i],&r)<0);
    CHECK(!process_parse_stat(stattext,42,&start)&&start==1234);
    CHECK(process_parse_stat(stattext,43,&start)<0);CHECK(process_parse_stat("42 (bad) S 1",42,&start)<0);
    CHECK(!process_command(command,cmdlen));CHECK(process_command(command,5)<0);
    reset();CHECK(!process_control("/run/ios/pid",0)&&pidcalls==1&&!signalcalls&&polls==2);
    reset();CHECK(!process_control("/run/ios/pid",1)&&signalcalls==1);
    reset();exechange=1;CHECK(process_control("/run/ios/pid",1)<0&&errno==ESTALE&&!signalcalls);
    reset();strcpy(stattext,"42 (reused) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 5678 0\n");CHECK(process_control("/run/ios/pid",1)<0&&errno==ESTALE&&!signalcalls);
    reset();dead_at=1;CHECK(process_control("/run/ios/pid",1)<0&&errno==ESRCH&&!procopens&&!signalcalls);
    reset();dead_at=2;CHECK(process_control("/run/ios/pid",1)<0&&errno==ESRCH&&!signalcalls);
    reset();signalerrno=ESRCH;CHECK(process_control("/run/ios/pid",1)<0&&errno==ESRCH&&signalcalls==1);
    reset();syscallerrno=ENOSYS;CHECK(process_control("/run/ios/pid",1)<0&&errno==ENOSYS&&!procopens&&!signalcalls);
    reset();directory_mode=0755;CHECK(process_control("/run/ios/pid",1)<0&&!pidcalls);
    reset();record_mode=0644;CHECK(process_control("/run/ios/pid",1)<0&&!pidcalls);
    reset();record_links=2;CHECK(process_control("/run/ios/pid",1)<0&&!pidcalls);
    reset();record_uid=1000;CHECK(process_control("/run/ios/pid",1)<0&&!pidcalls);
    reset();uid=1000;CHECK(process_control("/run/ios/pid",1)<0&&errno==EPERM&&!pidcalls);
    reset();memset(record,'x',300);record[300]=0;CHECK(process_control("/run/ios/pid",1)<0&&errno==EOVERFLOW&&!pidcalls);
    reset();const char wrong[]="iosnand\0scan\0";memcpy(command,wrong,sizeof(wrong));cmdlen=sizeof(wrong);CHECK(process_control("/run/ios/pid",1)<0&&!signalcalls);
    reset();readerror=EIO;CHECK(process_control("/run/ios/pid",1)<0&&errno==EIO&&!pidcalls);
    reset();CHECK(!process_record_self("/run/ios/pid")&&links==1&&unlinks==1&&!strcmp(written,record)&&!signalcalls);
    reset();binarychange=1;CHECK(process_record_self("/run/ios/pid")<0&&errno==ESTALE&&!links&&!signalcalls);
    reset();linkerror=EEXIST;CHECK(process_record_self("/run/ios/pid")<0&&errno==EEXIST&&unlinks==1&&!signalcalls);
    reset();writeerror=ENOSPC;CHECK(process_record_self("/run/ios/pid")<0&&errno==ENOSPC&&!links&&unlinks==1);
    reset();chmoderror=EPERM;CHECK(process_record_self("/run/ios/pid")<0&&!links&&unlinks==1);
    reset();syncerror=EIO;CHECK(process_record_self("/run/ios/pid")<0&&!links&&unlinks==1);
    reset();lockerror=EIO;CHECK(process_record_self("/run/ios/pid")<0&&!links&&!unlinks);
    reset();tempcollision=1;CHECK(process_record_self("/run/ios/pid")<0&&errno==EEXIST&&!links&&!unlinks);
    reset();closeerror=15;CHECK(process_record_self("/run/ios/pid")<0&&errno==EIO&&links==1);
    reset();CHECK(process_clean("/run/ios/pid")<0&&errno==EBUSY&&!unlinks&&!signalcalls);
    reset();syscallerrno=ESRCH;CHECK(!process_clean("/run/ios/pid")&&unlinks==1&&!signalcalls);
    reset();exechange=1;CHECK(!process_clean("/run/ios/pid")&&unlinks==1&&!signalcalls);
    reset();syscallerrno=ENOSYS;CHECK(process_clean("/run/ios/pid")<0&&!unlinks&&!signalcalls);
    reset();strcpy(record,"42\n");CHECK(process_clean("/run/ios/pid")<0&&!unlinks&&!pidcalls);
    reset();procerror=EACCES;CHECK(process_clean("/run/ios/pid")<0&&!unlinks&&!signalcalls);
    reset();lockerror=EIO;CHECK(process_clean("/run/ios/pid")<0&&!unlinks&&!pidcalls);
    reset();CHECK(!process_check_directory("/run/ios/pid")&&!pidcalls);
    reset();CHECK(process_check_directory("relative/pid")<0&&!pidcalls);
    reset();CHECK(!process_ready_control("/run/ios/pid")&&pidcalls==2&&!signalcalls);
    reset();kernel_pid=99;CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESTALE&&!signalcalls);
    reset();ready_missing=1;CHECK(process_ready_control("/run/ios/pid")<0&&errno==ENOENT&&!signalcalls);
    reset();strcpy(record,"IOSNANDPID1 42 5678 7 8\n");strcpy(stattext,"42 (newdaemon) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 5678 0\n");CHECK(process_ready_control("/run/ios/pid")<0&&errno==ENOENT&&!signalcalls);
    reset();workerchange=1;CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESTALE&&!signalcalls);
    reset();dead_at=3;CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESRCH&&!signalcalls);
    reset();dead_at=5;CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESRCH&&!signalcalls);
    reset();strcpy(worker_record,record);CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESTALE&&!signalcalls);
    reset();CHECK(!process_publish_ready("/run/ios/pid",43,32768)&&links==1&&!strcmp(written,worker_record));
    reset();kernel_pid=99;CHECK(process_publish_ready("/run/ios/pid",43,32768)<0&&!links);
    reset();linkerror=EEXIST;CHECK(process_publish_ready("/run/ios/pid",43,32768)<0&&errno==EEXIST);
    reset();pollerror=POLLNVAL;CHECK(process_clean("/run/ios/pid")<0&&errno==EBADF&&!unlinks);
    reset();pollerror=POLLERR;CHECK(process_clean("/run/ios/pid")<0&&errno==EIO&&!unlinks);
    reset();recordtype=S_IFIFO;CHECK(process_control("/run/ios/pid",1)<0&&!pidcalls&&!signalcalls);
    reset();recordopenerror=ELOOP;CHECK(process_control("/run/ios/pid",1)<0&&errno==ELOOP&&!pidcalls&&!signalcalls);
    reset();closeerror=17;CHECK(process_control("/run/ios/pid",0)<0&&errno==EIO&&!signalcalls);
    reset();kernel_pid=0;CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESTALE&&!signalcalls);
    reset();strcpy(worker_record,"IOSNANDREADY1 43 9000 7 8 32768\n");CHECK(process_ready_control("/run/ios/pid")<0&&errno==ESTALE&&!signalcalls);
    reset();kernel_size=0;CHECK(process_ready_control("/run/ios/pid")<0&&errno==EAGAIN&&!signalcalls);
    reset();kernel_size=65536;CHECK(process_ready_control("/run/ios/pid")<0&&errno==EAGAIN&&!signalcalls);
    reset();kernel_ro=0;CHECK(process_ready_control("/run/ios/pid")<0&&errno==EAGAIN&&!signalcalls);
    reset();kernel_size=0;CHECK(process_publish_ready("/run/ios/pid",43,32768)<0&&!links);
    reset();strcpy(worker_record,"IOSNANDREADY1 43 1235 7 8 0\n");CHECK(process_ready_control("/run/ios/pid")<0&&!signalcalls);
    struct part part = { .size = 32768ull * 512 };
    struct export export = { .p = &part, .child = 43 };
    unsigned attempts;
    reset();attempts=0;kernel_size=0;
    CHECK(exports_ready(&export,1,&attempts)==1&&attempts==1&&errno==EAGAIN);
    reset();attempts=0;kernel_ro=0;
    CHECK(exports_ready(&export,1,&attempts)==1&&attempts==1&&errno==EAGAIN);
    reset();attempts=0;kernel_pid=99;
    CHECK(exports_ready(&export,1,&attempts)==-1&&!attempts&&errno==ESTALE);
    reset();attempts=9;kernel_size=0;
    CHECK(exports_ready(&export,1,&attempts)==-1&&attempts==10&&errno==ETIMEDOUT);
    reset();attempts=5;
    CHECK(exports_ready(&export,1,&attempts)==0&&attempts==5);
    unsetenv("IOSNAND_BACKEND");
    char *registered[] = { "iosnand", "--pid-record", "/run/ios/pid", "serve", "System", NULL };
    reset();syscallerrno=ENOSYS;CHECK(iosnand_device_main(5,registered)==1&&pidcalls==1&&!pages);
    reset();directory_mode=0755;CHECK(iosnand_device_main(5,registered)==1&&!links&&!pages);
    reset();linkerror=EEXIST;CHECK(iosnand_device_main(5,registered)==1&&!pages&&links==1);
    char *badcli[] = { "iosnand", "--pid-record", "/run/ios/pid", "scan", NULL };
    reset();CHECK(iosnand_device_main(4,badcli)==1&&!pidcalls&&!pages);
    reset();CHECK(iosnand_device_main(2,registered)==1&&!pidcalls&&!pages);
    char *emptycli[] = { "iosnand", "--pid-record", "", "serve", NULL };
    reset();CHECK(iosnand_device_main(4,emptycli)==1&&!pidcalls&&!pages);
    setenv("IOSNAND_BACKEND","unknown",1);
    reset();CHECK(iosnand_device_main(5,registered)==1&&!pidcalls&&!pages);
    unsetenv("IOSNAND_BACKEND");
    printf("iosnand process: %u strict-fake cases PASS (no host process/device APIs)\n",cases);
}
