/* ebr_test — 扩展分区（EBR 链）往返验证
 *   建：3 个主分区 + 1 个扩展(0x0F) + 2 个逻辑分区 → 写盘 → 读回 → 逐项比对
 *   另用 imgdump 把每个 EBR 扇区抠出来验 55AA 与相对偏移（不只信自己的读函数） */
#define _DEFAULT_SOURCE
#include "img_partition.h"
#include "img_qcow2.h"
#include "img_io.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
static int fails=0;
#define OK(c,m) do{ if(c) printf("  ✅ %s\n", m); else { printf("  ❌ %s\n", m); fails++; } }while(0)
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    char path[]="/tmp/ebr_test_XXXXXX";
    int fd=mkstemp(path); if(fd>=0) close(fd);
    img_io_opts_t io={0}; io.writable=true;
    img_io *fh=img_io_open_path(path,&io);
    img_qcow2_create_opts_t co={0}; co.size=80ULL*1024*1024*1024; co.cluster_bits=16;
    OK(fh && img_qcow2_create(fh,&co)==IMG_OK, "建 80GB qcow2");
    img_io_close(fh);
    io.writable=true; fh=img_io_open_path(path,&io);
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(fh,&qo);
    img_qcow2_write_init(q);
    img_parttable_t t; memset(&t,0,sizeof t);
    t.table=IMG_TABLE_MBR; t.disk_sectors=80ULL*1024*1024*1024/512;
    int s1,s2,s3,se,sl1,sl2;
    s1=img_partition_add(&t,2048,102400,IMG_MBR_NTFS_EXFAT,NULL,"sys-res",true,&s1);   /* 50MB 活动 */
    s2=img_partition_add(&t,0,134217728,IMG_MBR_NTFS_EXFAT,NULL,"system",false,&s2);  /* 64GB */
    s3=img_partition_add(&t,0,2097152,IMG_MBR_RECOVERY,NULL,"recovery",false,&s3);    /* 1GB 0x27 */
    /* 扩展分区：起点 = 第一个逻辑起点-2048；大小覆盖到盘尾 */
    OK(s1>=0 && s2>=0 && s3>=0, "建 3 个主分区（50MB活动/64GB/1GB恢复）");
    uint64_t ext_first = t.parts[s3].last_lba + 1 + 2048;                  /* 恢复分区之后 + 1MiB */
    (void)s1;(void)s2;(void)s3;
    se=img_partition_add(&t,ext_first,(t.disk_sectors-ext_first-2048),IMG_MBR_EXTENDED_L,NULL,"extended",false,&se);
    OK(se>=0, "建扩展主分区(0x0F)");
    sl1=img_partition_add_logical(&t, ext_first+2048, 4194304, IMG_MBR_NTFS_EXFAT, "逻辑1");
    OK(sl1>0, "加逻辑分区 1（2GB）");
    sl2=img_partition_add_logical(&t, ext_first+2048+4194304+2048, 4194304, IMG_MBR_FAT32, "逻辑2");
    OK(sl2>0, "加逻辑分区 2（2GB）");
    OK(img_parttable_write(q,&t)==IMG_OK, "写盘（含 EBR 链）");
    /* 读回 */
    img_parttable_t r; memset(&r,0,sizeof r);
    OK(img_parttable_read(q,&r,0)==IMG_OK, "读回分区表");
    int nprim=0,nlog=0; uint64_t log1_first=0,log2_first=0; int types[8]={0};
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(!r.parts[i].used) continue;
        if(r.parts[i].logical){ nlog++; if(nlog==1) log1_first=r.parts[i].first_lba; if(nlog==2) log2_first=r.parts[i].first_lba; }
        else { nprim++; types[nprim-1]=r.parts[i].type; }
    }
    OK(nprim==4, "读回 4 个主分区");
    OK(nlog==2,  "读回 2 个逻辑分区（EBR 链跟出来了）");
    OK(log1_first==ext_first+2048, "逻辑1 起点正确（EBR+2048）");
    OK(log2_first==ext_first+2048+4194304+2048, "逻辑2 起点正确");
        { int has_ext=0, has_rec=0, has_sys=0;
      for(int i=0;i<4;i++){ if(types[i]==IMG_MBR_EXTENDED_L) has_ext=1;
                            if(types[i]==IMG_MBR_RECOVERY) has_rec=1;
                            if(types[i]==IMG_MBR_NTFS_EXFAT) has_sys=1; }
      OK(has_ext&&has_rec&&has_sys, "主分区含：扩展0x0F + 恢复0x27 + 数据0x07"); }
    /* 直接抠 EBR 扇区验（不只信读函数） */
    uint8_t eb[512];
    OK(img_qcow2_read_full(q,(ext_first)*512ULL,eb,512)==IMG_OK &&
       eb[510]==0x55 && eb[511]==0xaa, "EBR0 有 55AA");
    OK(eb[446+4]==IMG_MBR_NTFS_EXFAT && eb[446+8]==0x00 && eb[446+9]==0x08, "EBR0 entry0：相对偏移=2048（小端 00 08）");
    OK(eb[446+16+4]==IMG_MBR_EXTENDED_L, "EBR0 entry1：下一段 EBR 类型 0x0F");

    /* ★ 链尾规范：最后一个 EBR 的第二个条目必须【全零】（否则读端会去跟一个不存在的下一段）
     *   —— 2026-10-20 修（手册 §13.9）。旧写法在这里填 0x0F + 把段大小写进起始偏移。*/
    uint8_t el[512];
    OK(img_qcow2_read_full(q,(log2_first-2048)*512ULL,el,512)==IMG_OK &&
       el[510]==0x55 && el[511]==0xaa, "链尾 EBR 有 55AA");
    { int allz=1; for(int i=446+16;i<446+32;i++) if(el[i]){ allz=0; break; }
      OK(allz, "链尾 EBR 的 entry1 全零（规范要求：最后一段不再指向下一段）"); }

    /* ★ 逻辑分区【绝不】出现在 MBR 的 4 个主槽里（旧写法塞进去 → part.show 重复列、
     *   imglayout check 误报「最多 4 个主分区，当前 5 个」、Windows 看到幽灵主分区）*/
    { uint8_t mbr[512]; int ghost=0;
      if(img_qcow2_read_full(q,0,mbr,512)==IMG_OK){
        for(int e=0;e<4;e++){
            const uint8_t *en=mbr+446+e*16;
            if(en[4]==0) continue;
            uint64_t abs=(uint64_t)(en[8]|en[9]<<8|en[10]<<16|(uint32_t)en[11]<<24);
            if(abs==log1_first || abs==log2_first) ghost=1;
        }
      }
      OK(!ghost, "MBR 主槽里没有逻辑分区（只有 3 主分区 + 1 扩展容器）"); }

    /* ★ part.show 用的 JSON 必须导出 logical —— App 靠它区分主表项/逻辑分区 */
    { char jb[64*1024]; int n=img_parttable_json(&r,jb,sizeof jb);
      OK(n>0 && strstr(jb,"\"logical\":true") && strstr(jb,"\"logical\":false"),
         "分区表 JSON 导出 logical 字段（App 不必再靠猜）"); }

    img_qcow2_close(q); img_io_close(fh);
    unlink(path);
    if(fails){ printf("\n════ 结果：❌ 有失败（%d）════\n",fails); return 1; }
    printf("\n════ 结果：✅ EBR 往返通过（失败 0）════\n");
    return 0;
}
