/* imgmkfs <image.qcow2> --mkfs <lba> [label] [--spc N] [--fats N]   格式化指定分区
 *        <image.qcow2> --check <lba>                                校验
 *        <image.qcow2> --mkfs-latest                                格式化最后一个分区 */
#include "img_fat32.h"
#include "img_event.h"
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
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgmkfs");
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
    /* ★ 参数错位修复：用法写的是 `--mkfs <lba>`，但原代码只判 --check/--mkfs-latest，
     *   把 "--mkfs" 当 LBA 解析（strtoull 得 0）→ 一直报"找不到该分区"。*/
    if(!strcmp(argv[2],"--mkfs")){
        if(argc<4){ fprintf(stderr,"❌ --mkfs 需要 <lba>\n"); return 2; }
        lba = strtoull(argv[3],NULL,0);
        /* label 顺延到 argv[4]，后面的 --spc/--fats 扫描从 argv[4] 起 */
    } else if(!strcmp(argv[2],"--mkfs-latest")){
        img_parttable_t t;
        if(img_parttable_read(q,&t,0)!=IMG_OK){ fprintf(stderr,"❌ 读分区表\n"); return 1; }
        lba=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t.parts[i].used && t.parts[i].first_lba>lba) lba=t.parts[i].first_lba;
        if(!lba){ fprintf(stderr,"❌ 没有分区\n"); return 1; }
    } else lba=strtoull(argv[2],NULL,0);

    const char *label;
    if(!strcmp(argv[2],"--mkfs"))
        label = (argc>4 && argv[4][0]!='-') ? argv[4] : "NO NAME";
    else
        label = (argc>3 && argv[3][0]!='-') ? argv[3] : "NO NAME";
    img_fat32_opts_t o; memset(&o,0,sizeof o);
    o.partition_lba = lba;
    o.label = label;
    o.hidden_sectors = (uint32_t)lba;
    o.num_fats = 2; o.drive_number=0x80;
    int scan_from = !strcmp(argv[2],"--mkfs") ? 4 : 3;
    for(int i=scan_from;i+1<argc;i++){
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
    img_span sp;                      /* 里程碑：格式化（日志要能说清"卡在哪一步"）*/
    img_span_prep(&sp,"fat32","format","格式化 FAT32 分区");
    img_span_kv_u64(&sp,"lba",lba);
    img_span_kv_u64(&sp,"bytes",o.partition_bytes);
    img_span_begin(&sp);
    int rc=img_fat32_format(q,&o,&L,pr);
    img_span_end(&sp, rc);
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
