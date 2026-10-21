/* imgdump <image> <offset> <len> [--info]  → 把 guest 区域写到 stdout 或打印头信息 */
#include "img_qcow2.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgdump");
    if(argc<2){ fprintf(stderr,"用法: %s <image.qcow2> [offset len] [--info]\n",argv[0]); return 2; }
    bool info=false; uint64_t off=0; size_t len=0; bool have_range=false;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--info")) info=true;
        else if(!strcmp(argv[i],"--json")) { /* 忽略 */ }
        else if(!have_range){ off=strtoull(argv[i],NULL,0); have_range=true; }
        else len=strtoull(argv[i],NULL,0);
    }
    img_io_opts_t o={0}; o.writable=false;
    img_io *io = img_io_open_path(argv[1],&o);
    if(!io){ fprintf(stderr,"无法打开: %s\n",argv[1]); return 1; }

    img_qcow2_opts_t qo={0}; qo.read_only=true; qo.cb=NULL;
    img_qcow2 *q = img_qcow2_open(io,&qo);
    if(!q){ fprintf(stderr,"不是 qcow2 或不支持的特性\n"); img_io_close(io); return 1; }

    if(info){
        char b[2048];
        int n=img_qcow2_info_json(q,b,sizeof b);
        if(n>0){ fwrite(b,1,(size_t)n,stdout); fputc('\n',stdout); }
        img_qcow2_close(q); img_io_close(io); return 0;
    }
    if(!len){ len = 4096; }
    uint8_t *buf = malloc(len);
    if(!buf){ fprintf(stderr,"内存不足\n"); return 1; }
    int rc = img_qcow2_read_full(q, off, buf, len);
    if(rc!=IMG_OK){ fprintf(stderr,"读取失败: %s\n",img_strerror(rc)); free(buf); img_qcow2_close(q); img_io_close(io); return 1; }
    fwrite(buf,1,len,stdout);
    free(buf); img_qcow2_close(q); img_io_close(io);
    return 0;
}
