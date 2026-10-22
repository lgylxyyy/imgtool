/* imgntfscp <img.qcow2> <part_lba> <src_file> <dst_in_ntfs>
 *  经 ntfs_device 前插把文件写进 qcow2 里的 NTFS 分区（无 loop/无 FUSE/无 root） */
#include "img_qcow2.h"
#include "img_event.h"
#include "img_partition.h"
#include "img_ntfs_fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
extern int ntfscp_main(int argc,char**argv);
int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgntfscp");
    if(argc<5){ fprintf(stderr,"用法: %s <img> <part_lba> <src> <dst>\n",argv[0]); return 2; }
    uint64_t lba=strtoull(argv[2],NULL,0);
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(argv[1],&io); if(!f){printf("❌ 无法打开\n");return 1;}
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK){printf("❌ qcow2\n");return 1;}
    img_parttable_t t; img_parttable_read(q,&t,0);
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==lba)
            pb=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512;
    if(!pb){printf("❌ 未找到该分区\n");return 1;}

    img_ntfs_ctx_activate(q,lba,pb);
    /* ntfs_mount() 内部对设备名调 realpath() → 造同名空文件 */
    const char *dn=img_dev_placeholder(); if(!dn) dn="imgtool-dev";
    optind=1; opterr=1; optarg=NULL; optopt=0;
    char *a[]={(char*)"ntfscp",(char*)"-f",(char*)dn,(char*)argv[3],(char*)argv[4],NULL};
    int rc=ntfscp_main(5,a);
    img_ntfs_ctx_deactivate();
    if(rc!=0){ printf("❌ ntfscp rc=%d\n", rc); return 1; }
    printf("  ✅ %s → %s\n", argv[3], argv[4]);
    return 0;
}
