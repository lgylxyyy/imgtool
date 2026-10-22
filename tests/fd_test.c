/* fd_test — fd 生命周期契约验证（App 通过 SAF 传 fd 最容易踩的地方）
 *   ① 坏 fd（负数/已关闭/目录/管道）必须立刻得到明确失败，而不是后面莫名出错
 *   ② take_ownership=true  → img_io_close() 关掉 fd
 *   ③ take_ownership=false → 调用方保留 fd，img_io_close() 不许动它
 *   ④ 调用方提前关掉 fd    → 后续读写返回错误码，不崩（错误里带 errno）
 * 用法: fd_test <qcow2>
 */
#define _POSIX_C_SOURCE 200809L
#include "img_io.h"
#include "img_qcow2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails=0;
static void chk(const char *what,int cond){
    printf("  %s %s\n",cond?"✅":"❌",what);
    if(!cond) fails++;
}
static int fd_alive(int fd){ return fcntl(fd,F_GETFD)!=-1; }

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"用法: %s <qcow2>\n",argv[0]); return 2; }
    img_io_opts_t wo={0}; wo.writable=true;
    img_io_opts_t ro={0};

    printf("════ ① 坏 fd 必须立刻失败 ════\n");
    chk("open_fd(-1) = NULL",            img_io_open_fd(-1,false,&ro)==NULL);
    {
        int fd=open(argv[1],O_RDONLY); close(fd);     /* 已关闭的 fd */
        chk("open_fd(已关闭的 fd) = NULL", img_io_open_fd(fd,false,&ro)==NULL);
    }
    {
        const char *td=img_tmpdir();                  /* 拿一个真实存在的目录当反例 */
        int fd=open(td?td:".",O_RDONLY);              /* 目录 */
        chk("open_fd(目录 fd) = NULL",     img_io_open_fd(fd,false,&ro)==NULL);
        close(fd);
    }
    {
        int p[2]; if(pipe(p)==0){                     /* 管道不可 seek */
            chk("open_fd(管道 fd) = NULL", img_io_open_fd(p[0],false,&ro)==NULL);
            close(p[0]); close(p[1]);
        }
    }
    {
        int fd=open(argv[1],O_RDONLY);
        chk("open_fd(只读 fd) + 要求可写 = NULL", img_io_open_fd(fd,false,&wo)==NULL);
        close(fd);
    }

    printf("════ ② 合法 fd 可正常用 ════\n");
    {
        int fd=open(argv[1],O_RDWR);
        img_io *io=img_io_open_fd(fd,false,&wo);
        chk("open_fd(合法) 成功", io!=NULL);
        if(io){
            unsigned char magic[4]={0};
            int rc=img_io_read_full(io,0,magic,4);
            chk("能读到 qcow2 魔数 QFI\\xfb", rc==IMG_OK && magic[0]=='Q' && magic[1]=='F' && magic[2]=='I' && magic[3]==0xfb);
            chk("img_io_is_fd() = true", img_io_is_fd(io));
            chk("img_io_fd() 返回原 fd",  img_io_fd(io)==fd);
            img_io_close(io);
            chk("take_ownership=false → close 后 fd 仍活着", fd_alive(fd));
            close(fd);
        }
    }

    printf("════ ③ take_ownership=true 时才关 fd ════\n");
    {
        int fd=open(argv[1],O_RDWR);
        img_io *io=img_io_open_fd(fd,true,&wo);
        chk("open_fd(own=true) 成功", io!=NULL);
        if(io){
            img_io_close(io);
            chk("take_ownership=true → close 后 fd 已关闭", !fd_alive(fd));
        }
    }

    printf("════ ④ 调用方提前关 fd → 报错不崩 ════\n");
    {
        int fd=open(argv[1],O_RDWR);
        img_io *io=img_io_open_fd(fd,false,&wo);
        img_qcow2_opts_t qo={0};
        img_qcow2 *q=img_qcow2_open(io,&qo);
        chk("qcow2 打开成功", q!=NULL);
        close(fd);                       /* App 手滑提前关了 */
        char buf[512];
        int rc=img_qcow2_read_full(q,0,buf,sizeof buf);
        chk("提前关 fd 后读 → 返回错误码（不崩）", rc!=IMG_OK);
        if(q) img_qcow2_close(q);
        if(io) img_io_close(io);
    }

    printf("\n════ 结果：%s（失败 %d）════\n", fails?"❌ 有问题":"✅ fd 契约通过", fails);
    return fails?1:0;
}
