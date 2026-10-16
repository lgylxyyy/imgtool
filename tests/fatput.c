/* fatput — 测试用小工具：把宿主文件写进 FAT32 分区（corrupt-chain-test 等脚本用）
 *
 * 为什么要有它：以前脚本依赖临时编译的 /tmp/fatput，/tmp 一清测试就挂（实测踩过）。
 * 放进仓库 + CMake，回归才是自洽的。
 *
 * 用法: fatput <qcow2> <part_lba> <宿主源文件> <分区内路径>
 */
#include "img_cmd.h"
#include "img_fat32_fs.h"
#include "img_io.h"
#include "img_qcow2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv){
    if(argc<4){
        fprintf(stderr,"用法: %s <qcow2> <part_lba> <源文件> <分区内路径>\n",argv[0]);
        fprintf(stderr,"  或: %s <qcow2> <part_lba> <清单文件>   (每行 源<TAB>分区内路径)\n",argv[0]);
        return 2;
    }
    img_io_opts_t io={0}; io.writable=true;
    img_io *fh=img_io_open_path(argv[1],&io);
    if(!fh){ fprintf(stderr,"打不开镜像\n"); return 1; }
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(fh,&qo);
    if(!q){ fprintf(stderr,"不是 qcow2\n"); img_io_close(fh); return 1; }
    if(img_qcow2_write_init(q)!=IMG_OK){ fprintf(stderr,"write_init 失败\n"); return 1; }
    img_fat32_fs *fs=img_fat32_mount(q,strtoull(argv[2],NULL,0),true);
    if(!fs){ fprintf(stderr,"不是 FAT32/打不开\n"); return 1; }
    int rc=IMG_OK;
    if(argc>=5){                                  /* 单文件：src dst */
        rc=img_fat32_put_file(fs,argv[3],argv[4],NULL);
    } else {                                      /* 清单：每行 "源<TAB>分区内路径" */
        FILE *fp=fopen(argv[3],"r");
        if(!fp){ fprintf(stderr,"清单打不开: %s\n",argv[3]); img_fat32_unmount(fs); return 1; }
        char line[1024];
        while(fgets(line,sizeof line,fp)){
            char *tab=strchr(line,'\t'); if(!tab) continue;
            *tab=0;
            char *src=line, *dst=tab+1;
            while(*dst==' '||*dst=='\t') dst++;
            size_t dl=strlen(dst); while(dl && (dst[dl-1]=='\n'||dst[dl-1]=='\r')) dst[--dl]=0;
            if(!*src||!*dst) continue;
            rc=img_fat32_put_file(fs,src,dst,NULL);
            if(rc!=IMG_OK){ fprintf(stderr,"写入失败 %s → %s: %s\n",src,dst,img_strerror(rc)); break; }
        }
        fclose(fp);
    }
    img_fat32_unmount(fs);
    img_qcow2_close(q); img_io_close(fh);
    if(rc!=IMG_OK){ fprintf(stderr,"写入失败: %s\n",img_strerror(rc)); return 1; }
    printf("  → 成功\n");
    return 0;
}
