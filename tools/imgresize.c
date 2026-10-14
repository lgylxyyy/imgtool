/* imgresize <image.qcow2> resize <idx> <sectors>
            <image.qcow2> fill   <idx>
            <image.qcow2> show */
#include "img_partition.h"
#include "img_constraints.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static img_qcow2 *open_rw(const char *p){
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(p,&io); if(!f) return NULL;
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q){ img_io_close(f); return NULL; }
    if(img_qcow2_write_init(q)!=IMG_OK){ img_qcow2_close(q); return NULL; }
    return q;
}
static void show(img_qcow2 *q){
    img_parttable_t t;
    if(img_parttable_read(q,&t,0)!=IMG_OK){ printf("  读失败\n"); return; }
    uint64_t ul=img_parttable_usable_last(&t);
    printf("  可用末尾 LBA = %llu   磁盘末扇区 = %llu\n",
        (unsigned long long)ul,(unsigned long long)(t.disk_sectors-1));
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t.parts[i];
        if(!p->used) continue;
        double mb=(double)(p->last_lba-p->first_lba+1)*512.0/(1024*1024);
        printf("   #%d LBA %8llu..%8llu  %9.2f MiB  %-14s %s%s\n", i,
            (unsigned long long)p->first_lba,(unsigned long long)p->last_lba, mb,
            (t.table==IMG_TABLE_GPT?p->type_guid:img_mbr_type_name(p->type)),
            p->status==0x80?"★活动":"", p->name);
    }
}
int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"用法: %s <image> show|resize|fill ...\n",argv[0]); return 2; }
    img_qcow2 *q=open_rw(argv[1]);
    if(!q){ fprintf(stderr,"❌ 打不开\n"); return 1; }
    img_parttable_t t;
    if(img_parttable_read(q,&t,0)!=IMG_OK){ fprintf(stderr,"❌ 读分区表\n"); return 1; }

    img_check_result_t e={0};
    if(!strcmp(argv[2],"resize") && argc>=5){
        int idx=atoi(argv[3]); uint64_t sec=strtoull(argv[4],NULL,0);
        int rc=img_partition_resize(&t,idx,sec,&e);
        if(rc==IMG_E_NEED_PREV_SHRINK) printf("  ⚠ 需要压缩前一个分区: %s\n  → %s\n",e.reason,e.suggestion);
        else if(rc!=IMG_OK){ printf("  ❌ %s\n  → %s\n",e.reason,e.suggestion); img_qcow2_close(q); return 1; }
        else printf("  ✅ %s\n", e.reason);
    } else if(!strcmp(argv[2],"fill") && argc>=4){
        int idx=atoi(argv[3]);
        int rc=img_partition_fill_remaining(&t,idx,&e);
        if(rc!=IMG_OK){ printf("  ❌ %s\n  → %s\n",e.reason,e.suggestion); img_qcow2_close(q); return 1; }
        printf("  ✅ %s\n", e.reason);
    }
    /* 技术正确性复核（工具层硬检查） */
    int rc=img_constraints_check_table(&t,&e);
    if(rc!=IMG_OK){ printf("  🔴 技术校验失败: %s\n  → %s\n",e.reason,e.suggestion); img_qcow2_close(q); return 1; }
    printf("  技术校验: ✅ 通过（不溢出 / 不重叠 / 结构合法）\n");
    rc=img_parttable_write(q,&t);
    if(rc){ printf("  ❌ 写回: %s\n",img_strerror(rc)); return 1; }
    printf("  已写回\n\n");
    show(q);
    img_qcow2_close(q);
    return 0;
}
