/* imgmkfs <image.qcow2> --mkfs <lba> [label] [--spc N] [--fats N]   格式化指定分区
 *        <image.qcow2> --check <lba>                                校验
 *        <image.qcow2> --mkfs-latest                                格式化最后一个分区 */
#include "img_fat32.h"
#include "img_partition.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void onprog(void*u,const img_progress_t*p){(void)u;char b[2048];if(img_prog_json(p,b,sizeof b)>0)fprintf(stderr,"%s\n",b);}

static img_qcow2 *openq(const char*p){
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(p,&io); if(!f) return NULL;
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q){ img_io_close(f); return NULL; }
    if(img_qcow2_write_init(q)!=IMG_OK){ img_qcow2_close(q); return NULL; }
    return q;
}
int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"用法: %s <image> --mkfs <lba> [label] | --check <lba> | --mkfs-latest\n",argv[0]); return 2; }
    img_qcow2 *q = openq(argv[1]);
    if(!q){ fprintf(stderr,"❌ 打不开\n"); return 1; }

    if(!strcmp(argv[2],"--check")){
        uint64_t lba=strtoull(argv[3],NULL,0);
        img_fat32_layout_t L;
        int rc=img_fat32_check(q,lba,&L);
        if(rc==IMG_OK){
            printf("  ✅ FAT32 结构自洽\n");
            printf("     512B/扇区 · spc=%u(%u字节簇) · reserved=%u · %u 个FAT\n",
                L.sectors_per_cluster,L.sectors_per_cluster*512,L.reserved_sectors,L.num_fats);
            printf("     FAT大小=%u扇区 · 总扇区=%u · 根目录=cluster%u · 集群数=%u\n",
                L.fat_size_sectors,L.total_sectors,L.root_cluster,L.cluster_count);
            printf("     容量 %.1f MiB\n", (double)L.cluster_count*L.sectors_per_cluster*512/1048576.0);
        } else printf("  ❌ 校验失败: %s\n", img_strerror(rc));
        img_qcow2_close(q);
        return rc==IMG_OK?0:1;
    }

    uint64_t lba;
    if(!strcmp(argv[2],"--mkfs-latest")){
        img_parttable_t t;
        if(img_parttable_read(q,&t,0)!=IMG_OK){ fprintf(stderr,"❌ 读分区表\n"); return 1; }
        lba=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t.parts[i].used && t.parts[i].first_lba>lba) lba=t.parts[i].first_lba;
        if(!lba){ fprintf(stderr,"❌ 没有分区\n"); return 1; }
    } else lba=strtoull(argv[2],NULL,0);

    const char *label = (argc>3 && argv[3][0]!='-') ? argv[3] : "NO NAME";
    img_fat32_opts_t o; memset(&o,0,sizeof o);
    o.partition_lba = lba;
    o.label = label;
    o.hidden_sectors = (uint32_t)lba;
    o.num_fats = 2; o.drive_number=0x80;
    for(int i=3;i+1<argc;i++){
        if(!strcmp(argv[i],"--spc"))  o.sectors_per_cluster=(uint8_t)atoi(argv[i+1]);
        if(!strcmp(argv[i],"--fats")) o.num_fats=(uint8_t)atoi(argv[i+1]);
    }
    /* 分区大小从分区表取 */
    img_parttable_t t;
    if(img_parttable_read(q,&t,0)==IMG_OK){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t.parts[i].used && t.parts[i].first_lba==lba)
                o.partition_bytes=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512;
    }
    if(!o.partition_bytes){ fprintf(stderr,"❌ 找不到该分区\n"); return 1; }

    img_fat32_layout_t L;
    img_prog *pr=img_prog_new(onprog,NULL,200);
    int rc=img_fat32_format(q,&o,&L,pr);
    img_prog_free(pr);
    if(rc){ fprintf(stderr,"❌ 格式化失败: %s\n",img_strerror(rc)); return 1; }
    printf("  ✅ 已格式化 LBA %llu 起 %.1f MiB 分区\n",(unsigned long long)lba,
           (double)o.partition_bytes/1048576);
    printf("     spc=%u  FAT=%u扇区  集群=%u  根=cluster%u\n",
           L.sectors_per_cluster,L.fat_size_sectors,L.cluster_count,L.root_cluster);
    /* 立即自校验 */
    rc=img_fat32_check(q,lba,&L);
    printf("     自校验: %s\n", rc==IMG_OK?"✅ 通过":"❌ 失败");
    img_qcow2_close(q);
    return rc==IMG_OK?0:1;
}
