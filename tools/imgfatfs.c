/* imgfatfs — FAT32 文件系统读写 CLI
 *  imgfatfs <qcow2> <part_lba> <mkdir|put|get|ls|rm|exists> ... */
#include "img_fat32_fs.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static img_qcow2 *op(const char*p){
    img_io_opts_t io={0}; io.writable=true;
    img_io*f=img_io_open_path(p,&io); if(!f) return NULL;
    img_qcow2_opts_t qo={0}; img_qcow2*q=img_qcow2_open(f,&qo);
    if(!q) return NULL;
    if(img_qcow2_write_init(q)!=IMG_OK){ img_qcow2_close(q); return NULL; }
    return q;
}
static void show(void *ud,const char*nm,bool d,uint64_t sz){
    (void)ud; printf("  %-40s %s %llu\n", nm, d?"<DIR>":"    ", (unsigned long long)sz);
}
int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"用法: %s <qcow2> <part_lba> mkdir|put|get|ls|rm|exists ...\n",argv[0]); return 2; }
    uint64_t lba=strtoull(argv[2],NULL,0);
    img_qcow2 *q=op(argv[1]);
    if(!q){ printf("❌ 打不开\n"); return 1; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,true);
    if(!fs){ printf("❌ 不是合法 FAT32\n"); return 1; }
    const char *cmd=argv[3]; int rc=0;
    if(!strcmp(cmd,"mkdir")) rc=img_fat32_mkdir_p(fs,argv[4]);
    else if(!strcmp(cmd,"put")) rc=img_fat32_put_file(fs,argv[4],argv[5],NULL);
    else if(!strcmp(cmd,"ls")){ printf("  %s:\n",argv[4]); rc=img_fat32_list_dir(fs,argv[4],show,NULL); }
    else if(!strcmp(cmd,"exists")){ printf("  %s: %s\n",argv[4], img_fat32_path_exists(fs,argv[4])?"是":"否"); rc=0; }
    else if(!strcmp(cmd,"rm")) rc=img_fat32_rm(fs,argv[4]);
    else if(!strcmp(cmd,"get")){
        void*b=NULL; size_t n=0;
        rc=img_fat32_read_file(fs,argv[4],&b,&n);
        if(rc==IMG_OK){ printf("  %llu 字节\n",(unsigned long long)n); free(b); }
    } else printf("未知命令\n");
    printf("  → %s\n", img_strerror(rc));
    img_fat32_unmount(fs);
    return rc==IMG_OK?0:1;
}
