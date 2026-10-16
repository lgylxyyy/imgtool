/* imgbc <image.qcow2> [--uefi|--bios|--auto]   引导链自检
 * 输出：每项一行 JSON + 一行汇总；退出码 = 未通过的必需项数 */
#include "img_bootcheck.h"
#ifndef IMG_WITH_NTFS3G
#define IMG_WITH_NTFS3G 0
#endif
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

#if !IMG_WITH_NTFS3G
    (void)mode;
    fprintf(stderr,"⚠ 本构建没带 libntfs-3g → 17 项启动自检不可用（重编译开启 IMG_WITH_NTFS3G）\n");
    return 1;
#else
    img_bootcheck_result_t r;
    img_span sp;
    img_span_prep(&sp,"bc","check","引导链自检（17 项）");
    img_span_begin(&sp);
    int rc = img_bootcheck(q, mode, &r, NULL);
    img_span_end(&sp, rc);
    if(rc && r.count==0){
        fprintf(stderr,"❌ 检查失败: %s\n", img_strerror(rc));
        return 1;
    }
    char buf[65536];
    img_bootcheck_json(&r, buf, sizeof buf);
    fputs(buf, stdout);
    char sum[256];
    img_bootcheck_summary(&r, sum, sizeof sum);
    fprintf(stderr, "%s\n", sum);   /* ★ stdout 留给 JSON 行，中文汇总去 stderr（否则污染机器输出）*/
    return r.n_required_fail;   /* 退出码 = 必需项未通过数 */
#endif /* IMG_WITH_NTFS3G */
}
