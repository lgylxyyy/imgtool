/* img_sparsify.h — 原地回收映像未使用的块（T7）
 *
 * 与 img.compact 的区别（这是两件不同的事，别混）：
 *   img.compact  另存一份更小的映像（换文件、要额外空间、读写速度不变或变慢）
 *   img.sparsify 原地把「客户机已经不用」的块还给宿主（文件长度不变、不换文件）
 *
 * 两种"未使用"的来源：
 *   fs   按客户机文件系统的空闲位图（NTFS $Bitmap / FAT 表）——权威，但要求卷干净
 *   zero 按内容全零扫描 —— 不依赖文件系统，能吃掉格式化/写零留下的零块
 *
 * 各格式能做多少：
 *   raw        两种都行（只能打洞，长度不可截断 —— 长度 = 容量是 raw 的语义）
 *   qcow2      fs 模式复用 img.trim（清 L2 + refcount + 打洞）；zero 无意义
 *   vhd/vmdk   P1（要改 BAT/GTE + 打洞）
 *
 * 安全纪律：
 *   · dry_run=true 时一个字节都不改，但要给出准确的"能释放多少"
 *   · 不回收分区之间的空隙（那里可能是引导代码/厂商数据）
 *   · 执行前后校验分区表 + 每个分区头 64 KiB + 16 处抽样；不一致就报 SPARSIFY-VERIFY-FAIL
 */
#ifndef IMG_SPARSIFY_H
#define IMG_SPARSIFY_H
#include "img_types.h"
#include "img_qcow2.h"
#include "img_progress.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_SPARSIFY_FS   = 0,   /* 按客户机空闲位图 */
    IMG_SPARSIFY_ZERO = 1,   /* 按内容全零 */
    IMG_SPARSIFY_ALL  = 2    /* 先 fs 再 zero（fs 不支持时自动只用 zero） */
} img_sparsify_mode;

typedef struct {
    bool dry_run;            /* true = 只统计 */
    int  mode;               /* img_sparsify_mode；默认 ALL */
} img_sparsify_opts_t;

typedef struct {
    char     code[40];              /* 失败码（空=成功） */
    char     format[12];            /* raw / qcow2 / vhd / vmdk */
    char     mode_effective[8];     /* 实际用了哪种：fs / zero / fs+zero */
    char     fs[8];                 /* 分区文件系统（多个分区时取第一个认出的） */

    uint64_t bytes_before;          /* 宿主占用（st_blocks*512） */
    uint64_t bytes_after;
    uint64_t freed_bytes;           /* 真正还给宿主的字节 */
    uint64_t would_free_bytes;      /* dry_run 时的预估（= freed_bytes 的干跑值） */
    uint64_t scanned_bytes;         /* 扫描过的客户机字节 */
    uint64_t zero_bytes;            /* 判定为全零的字节 */
    uint64_t free_bytes_by_fs;      /* 位图说空闲的字节 */

    uint64_t punched_segments;      /* 打洞段数（相邻合并后） */
    uint64_t punched_bytes;         /* 实际打洞的字节（与 freed_bytes 口径不同：这是"打洞量"，
                                      freed_bytes 是"宿主占用前后差"） */
    uint64_t punch_failed;          /* 打洞调用失败次数 */
    bool     punch_supported;       /* 宿主文件系统支持打洞 */
    char     punch_note[160];       /* 给用户看的一句话 */

    uint64_t parts_scanned;
    uint64_t parts_skipped;         /* 认不出的分区（不是 NTFS/FAT32） */
    uint64_t skipped_bytes;         /* 分区外/受保护区间跳过的字节 */
    bool     verified;              /* 执行前后抽样一致 */
} img_sparsify_result_t;

int img_sparsify(img_qcow2 *q, const img_sparsify_opts_t *opts,
                 img_sparsify_result_t *out, img_prog *prog,
                 char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif