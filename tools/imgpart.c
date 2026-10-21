/* imgpart — 分区表创建/查看工具
 *   imgpart show   <image.qcow2>
 *   imgpart mk     <out.qcow2> <bytes> mbr|gpt   ← 创建新盘 + 默认分区方案
 *   imgpart add    <image.qcow2> <sectors> <type> [name]   ← 尾部加分区并写回
 */
#include "img_partition.h"
#include "img_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static img_qcow2 *open_rw(const char *p){
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(p,&io); if(!f) return NULL;
    img_qcow2_opts_t qo={0}; qo.read_only=false;
    img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q){ img_io_close(f); return NULL; }
    if(img_qcow2_write_init(q)!=IMG_OK){ img_qcow2_close(q); return NULL; }
    return q;
}
static void show(img_qcow2 *q){
    img_parttable_t t;
    int rc=img_parttable_read(q,&t,0);
    if(rc){ printf("  ❌ %s\n", img_strerror(rc)); return; }
    char *j=malloc(65536);
    if(j){ int k=img_parttable_json(&t,j,65536); if(k>0) fwrite(j,1,(size_t)k,stdout); printf("\n"); free(j); }
}

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgpart");
    if(argc<3){ fprintf(stderr,
      "用法:\n  %s show <image.qcow2>\n  %s mk <out.qcow2> <bytes> mbr|gpt\n"
      "  %s add <image.qcow2> <sectors> <mbrTypeHex|gptGuid> [name]\n"
      "  %s attr <image.qcow2> <index> <hex64>\n",
      argv[0],argv[0],argv[0],argv[0]); return 2; }

    if(!strcmp(argv[1],"show")){
        img_qcow2 *q=open_rw(argv[2]);
        if(!q){ fprintf(stderr,"❌ 无法打开\n"); return 1; }
        show(q); img_qcow2_close(q); return 0;
    }

    if(!strcmp(argv[1],"mk")){
        uint64_t sz=strtoull(argv[3],NULL,0);
        bool gpt = argc>4 && !strcmp(argv[4],"gpt");
        img_io_opts_t io={0}; io.writable=true;
        img_io *f=img_io_open_path(argv[2],&io);
        if(!f){ fprintf(stderr,"❌ 无法打开输出\n"); return 1; }
        img_qcow2_create_opts_t co={0}; co.size=sz; co.cluster_bits=16;
        int rc=img_qcow2_create(f,&co);
        if(rc){ fprintf(stderr,"❌ create: %s\n",img_strerror(rc)); return 1; }
        img_io_close(f);

        img_qcow2 *q=open_rw(argv[2]);
        if(!q){ fprintf(stderr,"❌ 重开失败\n"); return 1; }
        img_parttable_t t; memset(&t,0,sizeof t);
        t.disk_sectors = sz/512;
        t.disk_signature = 0x4ccd932b;
        snprintf(t.disk_guid,sizeof t.disk_guid,"3fc5b999-63f7-41b0-8379-efdbba81a0bd");
        t.part_entry_count=128; t.part_entry_size=128;
        t.table = gpt?IMG_TABLE_GPT:IMG_TABLE_MBR;

        int idx=-1;
        if(gpt){
            /* 模拟 Windows 官方 UEFI 布局：ESP 100MB + MSR 16MB + 数据分区 */
            uint64_t off=2048;
            img_partition_add(&t, off, 204800, IMG_MBR_ESP, IMG_GUID_ESP, "EFI system partition", false, &idx);
            off+=204800;
            img_partition_add(&t, off, 32768, IMG_MBR_EMPTY, IMG_GUID_MSR, "Microsoft reserved partition", false, &idx);
            off+=32768;
            img_partition_add(&t, off, t.disk_sectors-off-2048, 0x07, IMG_GUID_BASIC, "Basic data partition", false, &idx);
        } else {
            /* 模拟 Windows 官方 BIOS 布局：50MB 活动系统分区 + 系统分区 */
            img_partition_add(&t, 2048, 102400, IMG_MBR_NTFS_EXFAT, NULL, "System", true, &idx);
            img_partition_add(&t, 104448, t.disk_sectors-104448-2048, IMG_MBR_NTFS_EXFAT, NULL, "Windows", false, &idx);
        }
        rc=img_parttable_write(q,&t);
        if(rc){ fprintf(stderr,"❌ write: %s\n",img_strerror(rc)); return 1; }
        printf("✅ 已创建 %s 盘（%.1f GiB）并写入分区表\n", gpt?"GPT":"MBR", (double)sz/(1ULL<<30));
        show(q);
        img_qcow2_close(q);
        return 0;
    }

    if(!strcmp(argv[1],"add")){
        img_qcow2 *q=open_rw(argv[2]);
        if(!q){ fprintf(stderr,"❌ 无法打开\n"); return 1; }
        img_parttable_t t;
        int rc=img_parttable_read(q,&t,0);
        if(rc){ fprintf(stderr,"❌ 读分区表: %s\n",img_strerror(rc)); return 1; }
        uint64_t sec=strtoull(argv[3],NULL,0);
        const char *ty=argv[4];
        const char *name=(argc>5)?argv[5]:"";
        int idx=-1;
        if(t.table==IMG_TABLE_GPT){
            img_mbr_type_t mt = (strstr(ty,"c12a7328"))?IMG_MBR_ESP:
                                (strstr(ty,"e3c9e316"))?IMG_MBR_EMPTY:IMG_MBR_NTFS_EXFAT;
            rc=img_partition_add(&t, 0, sec, mt, ty, name, false, &idx);
        } else {
            uint8_t v=(uint8_t)strtoul(ty,NULL,16);
            rc=img_partition_add(&t, 0, sec, (img_mbr_type_t)v, NULL, name, false, &idx);
        }
        if(rc){ fprintf(stderr,"❌ add: %s\n",img_strerror(rc)); img_qcow2_close(q); return 1; }
        rc=img_parttable_write(q,&t);
        if(rc){ fprintf(stderr,"❌ write: %s\n",img_strerror(rc)); return 1; }
        printf("✅ 新增分区 #%d\n", idx);
        show(q);
        img_qcow2_close(q);
        return 0;
    }
    if(!strcmp(argv[1],"attr")){
        if(argc<5){ fprintf(stderr,"用法: %s attr <img> <index> <hex64>\n",argv[0]); return 2; }
        img_qcow2 *q=open_rw(argv[2]);
        if(!q){ fprintf(stderr,"❌ 无法打开\n"); return 1; }
        img_parttable_t t;
        int rc=img_parttable_read(q,&t,0);
        if(rc){ fprintf(stderr,"❌ 读分区表: %s\n",img_strerror(rc)); return 1; }
        int idx=atoi(argv[3]);
        uint64_t a=strtoull(argv[4],NULL,0);
        rc=img_partition_set_attributes(&t,idx,a);
        if(rc){ fprintf(stderr,"❌ %s\n",img_strerror(rc)); img_qcow2_close(q); return 1; }
        rc=img_parttable_write(q,&t);
        printf("%s 分区[%d].attributes = 0x%016llx\n",
               rc==IMG_OK?"✅":"❌", idx, (unsigned long long)a);
        img_qcow2_close(q);
        return rc==IMG_OK?0:1;
    }
    fprintf(stderr,"未知命令\n"); return 2;
}
