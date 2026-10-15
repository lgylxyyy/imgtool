/* img_ntfs_fmt.h — 进程内 NTFS 格式化（经 qcow2 设备后端） */
#ifndef IMG_NTFS_FMT_H
#define IMG_NTFS_FMT_H
#include "img_qcow2.h"
#include "img_progress.h"
#include "img_ntfs_dev.h"   /* 取 img_ntfs_ctx 的真实类型 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t partition_lba;    /* 分区起始扇区 */
    uint64_t partition_bytes;  /* 分区大小（字节） */
    const char *label;         /* 卷标（<=11 ASCII），NULL/空 → "NO NAME" */
    uint32_t sector_size;      /* 默认 512 */
    uint8_t  sectors_per_cluster; /* 0=自动 */
    bool     force;            /* -F 覆盖已有 */
    bool     quick;            /* -Q 快速（不零填全盘）*/
} img_ntfs_fmt_opts_t;

/* 用默认值填充 */
void img_ntfs_fmt_defaults(img_ntfs_fmt_opts_t *o, uint64_t lba, uint64_t bytes);

/* 格式化 qcow2 中的一个分区为 NTFS。
 * 内部：构造 argv → 调用 ntfsprogs 的 mkntfs main()，
 *       其设备 I/O 经由【我们提供的 ntfs_device_unix_io_ops】落到 qcow2。 */
int img_ntfs_format(img_qcow2 *q, const img_ntfs_fmt_opts_t *o, img_prog *prog);

/* 手工激活/停用设备上下文。
 * mkntfs 由 img_ntfs_format 内部自动处理；
 * ntfscp / ntfsls 等同样走 ntfs_device_default_io_ops 的工具，需要调用方
 * 在调用它们的 main() 之前激活、之后停用。 */
void img_ntfs_ctx_activate(img_qcow2 *q, uint64_t partition_lba, uint64_t partition_bytes);
void img_ntfs_ctx_deactivate(void);
img_ntfs_ctx *img_ntfs_ctx_current(void);

/* 取当前激活的设备上下文（内部设备后端与 bootcheck 桥接共用） */
img_ntfs_ctx *img_ntfs_get_active_ctx(void);

/* 校验分区是否为合法 NTFS（结构自洽）；out 可为 NULL */
typedef struct {
    uint32_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint64_t total_sectors;
    uint64_t mft_cluster;
    uint64_t mftmirr_cluster;
} img_ntfs_info_t;
int img_ntfs_probe(img_qcow2 *q, uint64_t partition_lba, img_ntfs_info_t *out);

#ifdef __cplusplus
}
#endif
#endif
