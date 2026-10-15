/* imgparts <image.qcow2>  → 打印分区表 JSON + 人读版 */
#include "img_partition.h"
#include "img_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgparts");
    if(argc<2){ fprintf(stderr,"用法: %s <image.qcow2>\n",argv[0]); return 2; }
    img_io_opts_t io={0};
    img_io *f=img_io_open_path(argv[1],&io);
    if(!f){ fprintf(stderr,"❌ 打不开\n"); return 1; }
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q){ fprintf(stderr,"❌ 不是 qcow2\n"); img_io_close(f); return 1; }

    img_parttable_t t; int rc=img_parttable_read(q,&t,0);
    if(rc){ fprintf(stderr,"❌ 读分区表失败: %s\n", img_strerror(rc)); return 1; }

    printf("表类型: %s   磁盘扇区数: %llu   分区GUID: %s\n",
        t.table==IMG_TABLE_GPT?"GPT":(t.table==IMG_TABLE_MBR?"MBR":"NONE"),
        (unsigned long long)t.disk_sectors, t.disk_guid[0]?t.disk_guid:"(—)");
    if(t.table==IMG_TABLE_MBR) printf("磁盘签名: 0x%08x\n", t.disk_signature);
    printf("%-4s %-8s %-24s %12s  %-12s %s\n","#","类型","GUID","扇区数","范围","名称");
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t.parts[i];
        if(!p->used) continue;
        printf("#%-3d 0x%-6x %-24s %12llu  %llu..%-12llu %s %s\n",
            i, p->type, p->type_guid[0]?p->type_guid:"(—)",
            (unsigned long long)(p->last_lba-p->first_lba+1),
            (unsigned long long)p->first_lba,(unsigned long long)p->last_lba,
            p->status==0x80?"★活动":"", p->name);
    }
    /* 空白区（你的需求：尾部找空位） */
    img_range_t rs[6];
    int n = img_parttable_free_regions(&t, t.disk_sectors, rs, 6, 2048, 1);
    printf("\n可用空白区（对齐2048）:\n");
    for(int i=0;i<n;i++)
        printf("  LBA %llu..%llu  %.1f GiB\n",
            (unsigned long long)rs[i].first_lba,(unsigned long long)rs[i].last_lba,
            (double)rs[i].sectors*512.0/(1ULL<<30));
    /* JSON */
    char *j=malloc(65536);
    if(j){ printf("\nJSON: ");
        int k=img_parttable_json(&t,j,65536); if(k>0) fwrite(j,1,(size_t)k,stdout); printf("\n"); free(j); }
    img_qcow2_close(q); img_io_close(f);
    return 0;
}
