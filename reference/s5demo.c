#include "img_wim_apply.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char**argv){
    uint64_t lba=strtoull(argv[2],NULL,0);
    img_io_opts_t io={0};io.writable=true;
    img_io*f=img_io_open_path(argv[1],&io); if(!f)return 1;
    img_qcow2_opts_t qo={0}; img_qcow2*q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK)return 1;
    char buf[2048];
    if(img_wim_list_images(argv[3],buf,sizeof buf)==IMG_OK) printf("  镜像: %s\n",buf);
    clock_t t0=clock();
    int rc=img_wim_apply(argv[3],1,q,lba,NULL,NULL);
    double s=(double)(clock()-t0)/CLOCKS_PER_SEC;
    printf("  结果: %s  耗时 %.1f 秒\n", img_strerror(rc), s);
    return rc==IMG_OK?0:1;
}
