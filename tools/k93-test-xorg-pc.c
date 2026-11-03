/* Actual sampler with fake process boundaries; never ptrace a real process. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
static int failure,requests,detaches;
static long fake_ptrace(enum __ptrace_request request,...)
{
    requests++;
    if(request==PTRACE_SEIZE)return failure==1?-1:0;
    if(request==PTRACE_INTERRUPT)return failure==2?-1:0;
    if(request==PTRACE_DETACH){detaches++;return 0;}
    assert(request==PTRACE_GETREGS);
    if(failure==3)return -1;
    va_list args;va_start(args,request);
    (void)va_arg(args,pid_t);(void)va_arg(args,void *);
    unsigned *regs=va_arg(args,unsigned *);va_end(args);
    regs[15]=0x101234;regs[14]=0x102345;regs[16]=0x60000010;
    return 0;
}
static uid_t fake_getuid(void){return 0;}
static int fake_usleep(useconds_t usec){assert(usec<=100000);return 0;}
static pid_t fake_waitpid(pid_t pid,int *status,int flags)
{
    assert(pid==123 && flags==(__WALL|WNOHANG));
    *status=(SIGTRAP<<8)|0x7f;return pid;
}
static ssize_t fake_readlink(const char *path,char *buf,size_t size)
{
    assert(!strcmp(path,"/proc/123/exe") && size>18);
    const char *name="/usr/libexec/Xorg";memcpy(buf,name,strlen(name));return strlen(name);
}
static FILE *fake_fopen(const char *path,const char *mode)
{
    assert(!strcmp(mode,"r"));
    const char *data;
    if(!strcmp(path,"/proc/device-tree/compatible"))data="apple,k93ap\0";
    else if(!strcmp(path,"/proc/123/comm"))data=failure==4?"other\n":"Xorg\n";
    else {assert(!strcmp(path,"/proc/123/maps"));data="00100000-00110000 r-xp 00001000 00:00 1 /usr/libexec/Xorg\n";}
    return fmemopen((void *)data,!strcmp(path,"/proc/device-tree/compatible")?12:strlen(data),"r");
}
#define ptrace fake_ptrace
#define getuid fake_getuid
#define usleep fake_usleep
#define waitpid fake_waitpid
#define readlink fake_readlink
#define fopen fake_fopen
#define main sampled_main
#include "k93-xorg-pc.c"
#undef main
int main(void)
{
    char *args[]={"sampler","123",NULL};
    assert(sampled_main(1,args)==2 && requests==0);
    for(failure=0;failure<=4;failure++) {
        requests=detaches=0;
        int result=sampled_main(2,args);
        if(failure==0)assert(result==0 && detaches==4 && requests==16);
        else if(failure==1)assert(result==5 && !detaches && requests==1);
        else if(failure==4)assert(result==4 && !requests);
        else assert(result==6 && detaches==1);
    }
    puts("PASS actual sampler: bounded samples, target guard and detach on post-seize failures; no hardware");
}
