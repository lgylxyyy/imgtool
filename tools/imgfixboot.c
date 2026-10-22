/* imgfixboot <dst.qcow2> <dst_lba> [--tpl <image> <lba>]
 *  从模板取 NTFS 引导代码写到目标分区；不给模板则做校验 */
#include "img_fixboot.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static img_qcow2 *op(const char *p){
    img_io_opts_t io={0}; io.writable=true;
    img_io *f = img_io_open_path(p,&io);
    if(!f) return NULL;
    img_qcow2_opts_t qo={0};
    img_qcow2 *q = img_qcow2_open(f,&qo);
    if(!q) return NULL;
    if(img_qcow2_write_init(q)!=IMG_OK){ img_qcow2_close(q); return NULL; }
    return q;
}
int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgfixboot");
    if(argc<3){ fprintf(stderr,"用法: %s <dst> <lba> [--tpl <img> <lba>] [--verify]\n",argv[0]); return 2; }
    uint64_t dlba=strtoull(argv[2],NULL,0);
    img_qcow2 *d=op(argv[1]); if(!d){printf("❌ 无法打开 dst\n");return 1;}
    const char *tp=NULL; uint64_t tlba=0; bool verify=false;
    for(int i=3;i<argc;i++){
        if(!strcmp(argv[i],"--tpl") && i+2<argc){ tp=argv[i+1]; tlba=strtoull(argv[i+2],NULL,0); i+=2; }
        else if(!strcmp(argv[i],"--verify")) verify=true;
    }
    if(verify){
        char det[256];
        int rc;
        if(tp){ img_qcow2 *t=op(tp); if(!t){printf("❌ 无法打开 tpl\n");return 1;}
                rc=img_fixboot_verify(d,dlba,t,tlba,det,sizeof det); }
        else  { rc=img_fixboot_verify_builtin(d,dlba,det,sizeof det); }
        printf("%s  %s\n", rc==IMG_OK?"✅ 引导代码与模板一致":"❌ 不一致", det);
        return rc==IMG_OK?0:1;
    }
    img_span sp;
    img_span_prep(&sp,"fixboot","write","写入 NTFS 引导代码（$Boot 0..9）");
    img_span_kv_u64(&sp,"dst_lba",dlba);
    img_span_begin(&sp);
    int rc;
    if(tp){ img_qcow2 *t=op(tp); if(!t){printf("❌ 无法打开 tpl\n");return 1;}
            rc=img_fixboot(d,dlba,t,tlba,NULL); }
    else  { rc=img_fixboot_builtin(d,dlba,NULL); }   /* 内置模板：不需要外部模板盘 */
    img_span_end(&sp, rc);
    printf("%s img_fixboot → %s\n", rc==IMG_OK?"✅":"❌", img_strerror(rc));
    return rc==IMG_OK?0:1;
}
