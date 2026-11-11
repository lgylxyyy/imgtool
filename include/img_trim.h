/* img_trim.h — 按客户机空闲位图原地释放 qcow2 簇（REQ-66） */
#ifndef IMG_TRIM_H
#define IMG_TRIM_H
#include "img_types.h"
#include "img_qcow2.h"
#include "img_progress.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool dry_run;      /* true = 只统计，一个字节都不改 */
    bool punch;        /* true（默认）= 释放后再对宿主文件打洞 */
} img_trim_opts_t;

typedef struct {
    char     code[40];                /* 失败时的稳定错误码（空=成功）；见 REQ-66 §6 */
    char     fs[8];                   /* "ntfs" / "fat32" */
    uint64_t fs_cluster_bytes;        /* 客户机分配单元（NTFS/FAT 簇） */
    uint64_t host_cluster_bytes;      /* qcow2 簇 */
    uint64_t host_clusters_checked;   /* 扫过的 qcow2 簇 */
    uint64_t clusters_freed;          /* 真正释放的 qcow2 簇 */
    uint64_t guest_bytes_freed;       /* 这些簇对应的客户机字节 */
    uint64_t host_bytes_punched;      /* 宿主文件真正回收的字节（打洞成功部分） */
    uint64_t punch_failed;            /* 打洞失败的簇数（文件没变小，但不是数据问题） */
    uint64_t shared_skipped;          /* refcount != 1 而跳过的簇（快照/别名） */
    uint64_t compressed_skipped;      /* 压缩簇（没有独立 refcount 语义，跳过） */
    uint64_t alloc_before;            /* 宿主已占块（st_blocks*512），失败给 0 */
    uint64_t alloc_after;
} img_trim_result_t;

/* 在 [part_lba, part_lba+part_bytes) 这个分区上，按卷的空闲位图释放 qcow2 簇。
 *
 * ★ 一期只做这些（见 docs/工作计划-2026-11-10-img-trim.md）：
 *   · 只认 NTFS / FAT32；卷被标记为脏（上次没正常卸载）→ 拒绝
 *   · 有快照（nb_snapshots>0）或 backing 文件 → 拒绝（我们从来不做 L2 COW，
 *     清 L2 等于改快照的历史；backing 我们根本不读，位图会读成零）
 *   · 只释放 refcount==1 的簇；其余一律跳过并计数（shared_skipped）
 *   · 只释放【完全落在分区内】且【所覆盖的客户机簇全部空闲】的 qcow2 簇
 *   · 默认打洞；不支持打洞就只计数，**不写零**（写零是假回收）
 *   · 不做 ftruncate（中断会让文件短一截、L2 指向文件外）
 *
 * 崩溃顺序（不要改）：L2=0 → fsync → refcount=0 → fsync → punch。
 * 反序会产生"L2 指向 refcount=0 的簇"= 真丢数据；本序中断只留泄漏簇（qc.verify 可查）。
 */
/* 读一个分区的客户机空闲位图（NTFS $Bitmap / FAT 表）。trim 与 sparsify 共用。
 * *bits 由调用方 free；bits[i]=1 表示分区内第 i 个卷簇空闲。
 * 返回 IMG_ENOTSUP：分区不是 NTFS/FAT32。*/
int img_fs_free_map(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                    uint8_t **bits, uint64_t *nbits, uint64_t *gcs,
                    char *fsname, size_t fscap, char *code, size_t codecap,
                    char *err, size_t errcap);

int img_trim(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
             const img_trim_opts_t *opts, img_trim_result_t *out,
             img_prog *prog, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif