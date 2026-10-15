/* imgbc <image.qcow2> [--uefi|--bios|--auto]   引导链自检
 * 输出：每项一行 JSON + 一行汇总；退出码 = 未通过的必需项数 */
#include "img_bootcheck.h"
#include "img_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgbc");
    if(argc<2){ fprintf(stderr,"用法: %s <image.qcow2> [--uefi|--bios|--auto]\n",argv[0]); return 2; }
    img_boot_mode_t mode = IMG_BOOT_AUTO;
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--uefi")) mode=IMG_BOOT_UEFI;
        else if(!strcmp(argv[i],"--bios")) mode=IMG_BOOT_BIOS;
        else if(!strcmp(argv[i],"--auto")) mode=IMG_BOOT_AUTO;
    }
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(argv[1],&io);
    if(!f){ fprintf(stderr,"❌ 打不开 %s\n",argv[1]); return 1; }
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q){ fprintf(stderr,"❌ 不是 qcow2\n"); return 1; }
    if(img_qcow2_write_init(q)!=IMG_OK){ fprintf(stderr,"❌ init\n"); return 1; }

    img_bootcheck_result_t r;
    int rc = img_bootcheck(q, mode, &r, NULL);
    if(rc && r.count==0){
        fprintf(stderr,"❌ 检查失败: %s\n", img_strerror(rc));
        return 1;
    }
    char buf[65536];
    img_bootcheck_json(&r, buf, sizeof buf);
    fputs(buf, stdout);
    char sum[256];
    img_bootcheck_summary(&r, sum, sizeof sum);
    printf("%s\n", sum);
    return r.n_required_fail;   /* 退出码 = 必需项未通过数 */
}
