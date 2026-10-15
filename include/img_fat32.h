/* img_fat32.h — FAT32 格式化与基础读写
 *
 * 依据 Microsoft FAT 规范（fatgen103）：
 *   sector 0      : VBR（BPB + 引导代码 + 55AA）
 *   sector 1      : FSInfo（空闲簇计数，可选但推荐）
 *   sector 2..5   : 保留（清零）
 *   sector 6      : VBR 备份
 *   sector 7      : FSInfo 备份
 *   reserved_sectors : 保留区结束
 *   FAT1 / FAT2   : 文件分配表（默认 2 份）
 *   data region   : 数据区（根目录占用 cluster 2）
 *
 * ★ 关键：根目录 = cluster 2（FAT32 特有，FAT16/12 根目录是固定区域）
 * ★ cluster 大小必须是 512 的 2 次方倍，且 FAT32 集群数 < 0x0FFFFFF5
 */
#ifndef IMG_FAT32_H
#define IMG_FAT32_H
#include "img_types.h"
#include "img_qcow2.h"
#include "img_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t partition_lba;      /* 分区起始（字节偏移 = lba*512） */
    uint64_t partition_bytes;    /* 分区大小（字节） */
    uint8_t  sectors_per_cluster;/* 0 = 自动按容量选 */
    const char *label;           /* 卷标（11 字节，最多 11 个 ASCII） */
    uint32_t volume_id;          /* 0 = 自动生成 */
    uint32_t hidden_sectors;     /* BPB hidden_sectors = 分区起始 LBA */
    uint8_t  drive_number;       /* 0x80 硬盘；ESP 通常 0x80 或 0 */
    uint8_t  num_fats;           /* 默认 2；ESP 常用 1（省空间）*/
    bool     active_boot;        /* 是否写引导跳转代码（空引导致无引导能力） */
} img_fat32_opts_t;

typedef struct {
    uint32_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t reserved_sectors;
    uint8_t  num_fats;
    uint32_t fat_size_sectors;
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t cluster_count;
} img_fat32_layout_t;

/* 自动按容量给出合法的 sectors_per_cluster（FAT32 集群数 < 0x0FFFFFF5） */
uint8_t img_fat32_pick_spc(uint64_t partition_bytes, uint32_t num_fats, uint32_t reserved);

/* 计算布局（不写盘），供校验/日志 */
int img_fat32_layout(const img_fat32_opts_t *o, img_fat32_layout_t *out);

/* 格式化分区（写 VBR + FSInfo + 备份 + 两份 FAT + 根目录） */
int img_fat32_format(img_qcow2 *q, const img_fat32_opts_t *o,
                     img_fat32_layout_t *out_layout, img_prog *prog);

/* 只做【结构解析】（宽松）：校验 BPB 自洽，不做主/备份 VBR 比对。
 * 真实镜像里 Windows 常常只更新主 VBR，备份不一致属常见情况，
 * 若因此判为"不是 FAT32"会误伤。bccheck 用这个。 */
int img_fat32_probe(img_qcow2 *q, uint64_t partition_lba, img_fat32_layout_t *out);

/* 严格校验（结构自洽 + 主/备份 VBR 一致 + FAT[0/1] 媒体描述符） */
int img_fat32_check(img_qcow2 *q, uint64_t partition_lba, img_fat32_layout_t *out);

#ifdef __cplusplus
}
#endif
#endif
