/* Production open: strict fake sysfs/device/ioctl API, memory streams only. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <dirent.h>
#include <mtd/mtd-abi.h>
#include "../tools/nand/ppn.h"
static unsigned cursor, open_count, close_count;
static int swapped, wrong_major, missing_dev, nonchar, writable, duplicate;
static int staterror, ioctlerror, ioctlpositive, geometryerror, poisonclose;
static unsigned reported_type, extra_flags;
struct memory_cookie { char text[64]; size_t length, offset; };
static unsigned stream; static struct memory_cookie cookies[32];
static ssize_t memory_read(void *data, char *buffer, size_t length)
{
    struct memory_cookie *cookie = data;
    size_t remaining = cookie->length - cookie->offset;
    if (length > remaining) length = remaining;
    memcpy(buffer, cookie->text + cookie->offset, length);
    cookie->offset += length;
    return length; /* Empty text is actual EOF, not a synthetic NUL byte. */
}
static const char *dev_text, *size_text;
static unsigned inventory_count;
static int streamreaderror, streamcloseerror;
static struct dirent entry;
static int token;
static DIR *fake_opendir(const char *path) {assert(!strcmp(path,"/sys/class/mtd"));cursor=0;return (DIR *)&token;}
static struct dirent *fake_readdir(DIR *dir) {assert(dir==(DIR *)&token);if(cursor==inventory_count)return NULL;snprintf(entry.d_name,sizeof(entry.d_name),"mtd%u",cursor++);return &entry;}
static int fake_closedir(DIR *dir){assert(dir==(DIR *)&token);return 0;}
static FILE *fake_fopen(const char *path,const char *mode)
{
    assert(!strcmp(mode,"r"));unsigned index;char attribute[32];
    assert(sscanf(path,"/sys/class/mtd/mtd%u/%31s",&index,attribute)==2&&index<4);
    char text[64];
    if(!strcmp(attribute,"name"))snprintf(text,sizeof(text),"apple-nand%u\n",duplicate&&index==1?0:index);
    else if(!strcmp(attribute,"size"))snprintf(text,sizeof(text),"%s",size_text?size_text:"4462739456\n");
    else if(!strcmp(attribute,"dev")) {
        if(missing_dev) {errno=ENOENT;return NULL;}
        if(dev_text)snprintf(text,sizeof(text),"%s",dev_text);
        else snprintf(text,sizeof(text),"90:%u\n",index*2);
    }
    else abort();
    /* An in-process memory stream, never a real sysfs file. */
    assert(stream<32);struct memory_cookie *cookie=&cookies[stream++];
    strcpy(cookie->text,text);cookie->length=strlen(text);cookie->offset=0;
    cookie_io_functions_t callbacks={.read=memory_read};
    FILE *f=fopencookie(cookie,"r",callbacks);assert(f);return f;
}
static int fake_open(const char *path,int flags,...)
{ unsigned index;assert(sscanf(path,"/dev/mtd%u",&index)==1&&index<4);assert(flags== (O_RDONLY|O_CLOEXEC|O_NOFOLLOW));open_count++;return 100+index; }
static int fake_fstat(int fd,struct stat *st)
{ assert(fd>=100&&fd<104);if(staterror){errno=EIO;return -1;}memset(st,0,sizeof(*st));st->st_mode=(nonchar?S_IFREG:S_IFCHR)|0600;unsigned index=fd-100;if(swapped&&index<2)index^=1;st->st_rdev=makedev(wrong_major?89:90,index*2);return 0; }
static int fake_close(int fd){assert(fd>=100&&fd<104);close_count++;if(poisonclose)errno=EINTR;return 0;}
static int fake_ioctl(int fd,unsigned long op,void *arg)
{ assert(fd>=100&&fd<104&&op==MEMGETINFO);if(ioctlerror){errno=EACCES;return -1;}if(ioctlpositive)return 1;struct mtd_info_user *i=arg;memset(i,0,sizeof(*i));i->type=reported_type;i->flags=MTD_NO_ERASE|(writable?MTD_WRITEABLE:0)|extra_flags;i->size=(uint32_t)4462739456ull;i->erasesize=4194304;i->writesize=geometryerror?8192:16384;i->oobsize=64;return 0; }
static char *fake_fgets(char *text, int len, FILE *file)
{ if(streamreaderror){errno=EIO;return NULL;}return fgets(text,len,file); }
static int fake_ferror(FILE *file) {return streamreaderror?1:ferror(file);}
static int fake_fclose(FILE *file)
{int r=fclose(file);if(streamcloseerror){errno=ENOSPC;return -1;}return r;}
#define fgets fake_fgets
#define ferror fake_ferror
#define fclose fake_fclose
#define opendir fake_opendir
#define readdir fake_readdir
#define closedir fake_closedir
#define fopen fake_fopen
#define open fake_open
#define fstat fake_fstat
#define close fake_close
#define IOSNAND_IOCTL fake_ioctl
#include "../tools/nand/iosnand-mtd.h"
static unsigned checks;
static void reset(void)
{
    cursor=open_count=close_count=stream=0;
    swapped=wrong_major=missing_dev=nonchar=writable=duplicate=0;
    staterror=ioctlerror=ioctlpositive=geometryerror=poisonclose=0;
    reported_type=MTD_MLCNANDFLASH;extra_flags=0;
    dev_text=size_text=NULL; inventory_count=4; streamreaderror=streamcloseerror=0;
    for(int i=0;i<PPN_DIES;i++)assert(mtd_fds[i]==-1);
}
static void rejected(int expected_error)
{
    assert(mtd_backend_open()==-1);
    if(errno!=expected_error)fprintf(stderr,"case %u errno %d expected %d devtext=%s\n",checks,errno,expected_error,dev_text?dev_text:"<normal>");
    assert(errno==expected_error);
    assert(close_count==open_count);
    for(int i=0;i<PPN_DIES;i++)assert(mtd_fds[i]==-1);
    checks++;
}
int main(void)
{
    (void)mtd_backend_read;
    reset();assert(!mtd_backend_open()&&open_count==4);mtd_backend_close();assert(close_count==4);checks++;
    reset();swapped=1;rejected(ENODEV); /* baseline silently accepted all four */
    reset();wrong_major=1;rejected(ENODEV);
    reset();missing_dev=1;rejected(ENOENT);assert(!open_count);
    reset();nonchar=1;rejected(ENODEV);
    reset();writable=1;rejected(EINVAL);
    reset();reported_type=MTD_NANDFLASH;rejected(EINVAL); /* Old SLC-mislabelled driver. */
    reset();reported_type=MTD_NORFLASH;rejected(EINVAL);
    reset();extra_flags=MTD_SLC_ON_MLC_EMULATION;rejected(EINVAL);
    reset();duplicate=1;rejected(EINVAL);
    const char *bad[]={"", "90", "90:", "90:-1\n", "-90:0\n", "90:+0\n", " 90:0\n", "90:0 junk\n",
                       "90:0:2\n", "90:18446744073709551616\n", "18446744073709551616:0\n", "91:0\n"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){reset();dev_text=bad[i];rejected(strstr(bad[i],"18446744073709551616")?ERANGE:EINVAL);assert(!open_count);}
    reset();staterror=1;poisonclose=1;rejected(EIO);
    reset();ioctlerror=1;poisonclose=1;rejected(EACCES);
    reset();ioctlpositive=1;poisonclose=1;rejected(EIO);
    reset();geometryerror=1;poisonclose=1;rejected(EINVAL);
    reset();swapped=1;poisonclose=1;rejected(ENODEV);
    reset();size_text="123junk\n";rejected(EINVAL);
    reset();size_text="-1\n";rejected(EINVAL);
    reset();inventory_count=3;rejected(ENODEV);
    reset();char text[64];streamreaderror=1;
    assert(mtd_sysfs_text("mtd0","dev",text,sizeof(text))==-1&&errno==EIO);checks++;
    reset();streamcloseerror=1;
    assert(mtd_sysfs_text("mtd0","dev",text,sizeof(text))==-1&&errno==ENOSPC);checks++;
    reset();dev_text="thisdevattributetextislongerthanthesixtyfourbytebufferanddoesnotendinline90:0";
    assert(mtd_sysfs_text("mtd0","dev",text,8)==-1&&errno==EINVAL);checks++;
    reset();assert(mtd_sysfs_text("mtd0","dev",text,0)==-1&&errno==EINVAL);checks++;
    printf("iosnand MTD open: %u actual fake-FS/device binding cases PASS\n",checks);
}
