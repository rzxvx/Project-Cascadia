/* Bounded ARM Xorg PC/LR sampler. Brief ptrace stops, no process-memory reads,
 * writes, remote listener or MMIO access. Always detach before reporting. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

static void mapping(pid_t pid, uint32_t address, const char *label)
{
    char path[64], line[1024], perms[5], name[768];
    unsigned long start,end,offset;
    snprintf(path,sizeof(path),"/proc/%d/maps",pid);
    FILE *stream=fopen(path,"r");
    if(!stream)return;
    while(fgets(line,sizeof(line),stream)) {
        name[0]=0;
        if(sscanf(line,"%lx-%lx %4s %lx %*s %*s %767[^\n]",&start,&end,perms,&offset,name)<4)continue;
        if(address>=start && address<end && strchr(perms,'x')) {
            printf("%s=%08x file_offset=%08lx executable=%s\n",label,address,offset+address-start,name);
            break;
        }
    }
    fclose(stream);
}

int main(int argc,char **argv)
{
    char board[32],path[64],name[32],exe[256],*end;
    if(argc!=2 || getuid()!=0)return 2;
    errno=0;long value=strtol(argv[1],&end,10);
    if(errno || *end || value<2 || value>4194304)return 2;
    pid_t pid=(pid_t)value;
    FILE *stream=fopen("/proc/device-tree/compatible","r");
    if(!stream)return 3;
    size_t n=fread(board,1,sizeof(board),stream);fclose(stream);
    if(n<sizeof("apple,k93ap") || memcmp(board,"apple,k93ap",sizeof("apple,k93ap")))return 3;
    snprintf(path,sizeof(path),"/proc/%d/comm",pid);
    stream=fopen(path,"r");if(!stream)return 4;
    char *got=fgets(name,sizeof(name),stream);fclose(stream);
    if(!got || strcmp(name,"Xorg\n"))return 4;
    snprintf(path,sizeof(path),"/proc/%d/exe",pid);
    ssize_t length=readlink(path,exe,sizeof(exe)-1);
    if(length<0)return 4;
    exe[length]=0;
    if(strcmp(exe,"/usr/libexec/Xorg"))return 4;
    for(int sample=0;sample<4;sample++) {
        if(ptrace(PTRACE_SEIZE,pid,0,0)<0)return 5;
        int status=0,stopped=0;
        if(ptrace(PTRACE_INTERRUPT,pid,0,0)==0) {
            for(int attempt=0;attempt<50;attempt++) {
                pid_t result=waitpid(pid,&status,__WALL|WNOHANG);
                if(result==pid){stopped=WIFSTOPPED(status);break;}
                if(result<0 && errno!=EINTR)break;
                usleep(10000);
            }
        }
        struct {uint32_t r[18];} regs={{0}};
        int okay=stopped && ptrace(PTRACE_GETREGS,pid,0,&regs)==0;
        int detached=ptrace(PTRACE_DETACH,pid,0,0)==0;
        if(!okay || !detached)return 6;
        printf("K93_XORG_PC sample=%d pid=%d cpsr=%08x\n",sample,pid,regs.r[16]);
        printf("syscall_nr=%u r0=%08x r2=%08x orig_r0=%08x\n",regs.r[7],regs.r[0],regs.r[2],regs.r[17]);
        mapping(pid,regs.r[15],"pc");mapping(pid,regs.r[14]&~1u,"lr");
        fflush(stdout);usleep(100000);
    }
    return 0;
}
