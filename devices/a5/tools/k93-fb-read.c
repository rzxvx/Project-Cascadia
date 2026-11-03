/* Read-only, exact K93 framebuffer -> RGB PPM. No MMIO or state-changing ioctl. */
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int valid_mode(const struct fb_fix_screeninfo *f,const struct fb_var_screeninfo *v)
{
    return f->type==FB_TYPE_PACKED_PIXELS && f->visual==FB_VISUAL_TRUECOLOR &&
        f->line_length==4096 && f->smem_len>=4096*768 &&
        v->xres==1024 && v->yres==768 && !v->xoffset && !v->yoffset &&
        v->bits_per_pixel==32 && v->red.offset==16 && v->red.length==8 &&
        v->green.offset==8 && v->green.length==8 && v->blue.offset==0 && v->blue.length==8 &&
        !v->red.msb_right && !v->green.msb_right && !v->blue.msb_right;
}
static void rgb_row(const unsigned char in[4096],unsigned char out[3072])
{
    for(unsigned x=0;x<1024;x++) {
        out[x*3]=in[x*4+2];out[x*3+1]=in[x*4+1];out[x*3+2]=in[x*4];
    }
}
int main(void)
{
    char board[128];int b=open("/proc/device-tree/compatible",O_RDONLY);
    if(b<0)return 1;
    ssize_t n=read(b,board,sizeof(board));close(b);
    if(n<(ssize_t)sizeof("apple,k93ap") || memcmp(board,"apple,k93ap",sizeof("apple,k93ap")))return 1;
    int fd=open("/dev/fb0",O_RDONLY);
    if(fd<0){perror("fb0 open");return 1;}
    struct fb_fix_screeninfo f;struct fb_var_screeninfo v;
    if(ioctl(fd,FBIOGET_FSCREENINFO,&f)<0 || ioctl(fd,FBIOGET_VSCREENINFO,&v)<0 || !valid_mode(&f,&v)) {
        fputs("K93 framebuffer mode not verified\n",stderr);close(fd);return 1;
    }
    fprintf(stderr,"K93_FB_READ 1024x768 bpp32 stride4096 RGB16/8/0\n");
    if(fputs("P6\n1024 768\n255\n",stdout)==EOF){close(fd);return 1;}
    unsigned char raw[4096],rgb[3072];
    for(unsigned y=0;y<768;y++) {
        size_t got=0;
        while(got<sizeof(raw)) {
            ssize_t r=pread(fd,raw+got,sizeof(raw)-got,(off_t)y*4096+(off_t)got);
            if(r<0 && errno==EINTR)continue;
            if(r<=0){fputs("Short framebuffer read\n",stderr);close(fd);return 1;}
            got+=(size_t)r;
        }
        rgb_row(raw,rgb);
        if(fwrite(rgb,1,sizeof(rgb),stdout)!=sizeof(rgb)){close(fd);return 1;}
    }
    close(fd);return fflush(stdout)==0?0:1;
}
