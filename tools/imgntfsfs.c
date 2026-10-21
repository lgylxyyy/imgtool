/* imgntfsfs — NTFS 写入能力演示与校验
 *  用法: imgntfsfs <img> <part_lba> <mkdir <path>> | <put <local> <dst>>
 *                     | <ls <path>> | <stat <path>>
 *  ★ 校验策略：写用我们的 img_ntfs_fs，读用【上游 ntfsls/ntfscat】（同一进程内前插），
 *    两边都通过才算数 —— 避免"自己写自己读"的自证陷阱。 */
#include "img_ntfs_fs.h"
#include "img_event.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_ntfs_fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
extern int ntfsls_main(int argc,char**argv);
extern int ntfscat_main(int argc,char**argv);

static img_qcow2 *open_q(const char*p){
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(p,&io); if(!f) return NULL;
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q) return NULL;
    if(img_qcow2_write_init(q)!=IMG_OK){ img_qcow2_close(q); return NULL; }
    return q;
}
/* 用上游工具读（前插） */
static void upstream_ls(img_qcow2 *q, uint64_t lba, uint64_t sz, const char *path){
    img_ntfs_ctx_activate(q,lba,sz);
    int d=open("imgtool-dev",O_RDWR|O_CREAT|O_TRUNC,0666); if(d>=0)close(d);
    optind=1;
    const char *a[]={"ntfsls","-a","-p",path,"imgtool-dev"};
    ntfsls_main(5,(char**)a);
    img_ntfs_ctx_deactivate();
}
int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgntfsfs");
    if(argc<4){ fprintf(stderr,"用法见文件头\n"); return 2; }
    uint64_t lba=strtoull(argv[2],NULL,0);
    img_qcow2 *q=open_q(argv[1]);
    if(!q){ printf("❌ 无法打开\n"); return 1; }
    img_parttable_t t; img_parttable_read(q,&t,0);
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==lba)
            pb=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512;
    if(!pb){ printf("❌ 分区不存在\n"); return 1; }
    const char *cmd=argv[3];
    int rc=0;
    if(!strcmp(cmd,"format")){
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,lba,pb); o.label="WINRE";
        img_span sp;
        img_span_prep(&sp,"ntfs","format","格式化 NTFS 分区");
        img_span_kv_u64(&sp,"lba",lba);
        img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        rc=img_ntfs_format(q,&o,NULL);
        img_span_end(&sp, rc);
        printf("  格式化: %s\n", img_strerror(rc));
    } else {
        img_ntfs_fs *fs = img_ntfs_mount_rw(q,lba,pb);
        if(!fs){ printf("❌ 挂载失败（errno=%d %s）\n", errno, strerror(errno)); return 1; }
        if(!strcmp(cmd,"mkdir")){
            rc=img_ntfs_mkdir_p(fs, argv[4]);
            printf("  mkdir %s → %s\n", argv[4], img_strerror(rc));
        } else if(!strcmp(cmd,"put")){
            rc=img_ntfs_put_file(fs, argv[4], argv[5], NULL);
            printf("  put %s → %s → %s\n", argv[4], argv[5], img_strerror(rc));
            uint64_t sz=0;
            if(rc==IMG_OK && img_ntfs_file_size(fs,argv[5],&sz)==IMG_OK)
                printf("    写入后大小: %llu 字节\n",(unsigned long long)sz);
        } else if(!strcmp(cmd,"reagent")){
        /* reagent <目录> <WinreLocation路径> <BCD恢复GUID> [分区字节偏移]
         *   例: reagent /Recovery/WindowsRE '\Recovery\WindowsRE' 7619dcc9-... 
         *   分区偏移缺省用本分区的 lba*512；写 OS 分区那份副本时要显式给恢复分区的偏移 */
        if(argc<7){ printf("用法: reagent <目录> <WinreLocation路径> <BCD GUID> [分区字节偏移]\n"); rc=IMG_EINVAL; }
        else {
        uint64_t off = (argc>=8) ? strtoull(argv[7],NULL,0) : lba*512;
        rc=img_ntfs_write_reagent(fs, argv[4], argv[5], argv[6], off);
        if(rc==IMG_OK){
            /* argv[4] 就是目录本身（不是 wim 路径），不要再往上剥一层 */
            char full[512];
            snprintf(full,sizeof full,"%s/ReAgent.xml", argv[4]);
            void*b=NULL; size_t n=0;
            if(img_ntfs_read_file(fs, full, &b, &n)==IMG_OK){
                printf("  ── %s (%zu 字节) ──\n", full, n);
                printf("%s", (char*)b);
                free(b);
            }
        }
        }
    } else if(!strcmp(cmd,"export")){
        /* export <路径在NTFS里> <宿主文件>  —— 读出到宿主 */
        void *b=NULL; size_t n=0;
        rc=img_ntfs_read_file(fs, argv[4], &b, &n);
        if(rc==IMG_OK){
            FILE *fp=fopen(argv[5], "wb");
            if(!fp){ printf("❌ 宿主文件写不开\n"); rc=IMG_EIO; }
            else { if(n) fwrite(b,1,n,fp); fclose(fp);
                   printf("  ✅ %s → %s  %llu 字节\n", argv[4], argv[5], (unsigned long long)n); }
            free(b);
        }
    } else if(!strcmp(cmd,"exists")){
            printf("  exists %s: %s\n", argv[4], img_ntfs_path_exists(fs,argv[4])?"是":"否");
        } else if(!strcmp(cmd,"ls")){        /* ls <目录> —— 用【上游 ntfsls】列（独立实现对拍用） */
        if(argc<5){ printf("用法: ls <目录>\n"); rc=IMG_EINVAL; }
        else { printf("  ── 上游 ntfsls 列目录 %s ──\n", argv[4]);
               upstream_ls(q,lba,pb, argv[4]); }
    } else { printf("未知命令\n"); rc=1; }
        img_ntfs_umount(fs);
    }
    /* 用上游工具复核 */
    if(rc==0){
        /* put 的复核路径应是 NTFS 目标(argv[5])，不是本地源(argv[4]) */
        const char *vpath = "/";
        if(!strcmp(cmd,"mkdir") && argc>4) vpath = argv[4];
        else if(!strcmp(cmd,"put") && argc>5) vpath = argv[5];
        printf("  ── 上游 ntfsls 复核 %s ──\n", vpath);
        upstream_ls(q,lba,pb, vpath);
    }
    return rc==IMG_OK?0:1;
}
