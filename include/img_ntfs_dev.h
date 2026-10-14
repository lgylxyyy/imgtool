/* img_ntfs_dev.h — 把 qcow2 分区接入 libntfs-3g 的块设备抽象
 *
 * libntfs-3g 通过 struct ntfs_device_operations 访问底层设备。
 * 我们实现这 10 个回调 → mkntfs / ntfs-3g 就把 qcow2 里的分区
 * 当成一块裸块设备直接读写。
 *
 * 好处（相对"落临时文件再拷贝"）：
 *   · 无临时文件（安卓分区空间有限，且省掉一次全量拷贝）
 *   · 无 fork / 无进程二进制
 *   · 无 loop 设备 / 无 root / 无 FUSE
 *
 * 许可：本文件是【我们自己的代码】（只 include libntfs-3g 的头），
 *       libntfs-3g 为 LGPL-2.1+，动态/静态链接都可。
 *       格式化器 mkntfs(GPL) 是否链入由 IMG_WITH_NTFS 开关决定。
 */
#ifndef IMG_NTFS_DEV_H
#define IMG_NTFS_DEV_H
#include "img_qcow2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 将来要与 ntfs_device_operations 搭配时，由调用者 include
 *   <ntfs-3g/device.h>  后再用本头。这里不强依赖 ntfs 头，
 *   以便在未装 libntfs-3g-dev 的环境也能编译其余部分。 */

typedef struct {
    img_qcow2 *q;
    img_io    *io;         /* 用于 sync；可为 NULL */
    uint64_t   base;       /* 分区起始（字节）= partition_lba * 512 */
    uint64_t   size;       /* 分区大小（字节） */
    uint64_t   pos;        /* seek 游标（相对分区起点） */
    int        last_errno;
} img_ntfs_ctx;

/* 建一个上下文（不分配设备结构，那由 ntfs_device_alloc 负责） */
img_ntfs_ctx *img_ntfs_ctx_new(img_qcow2 *q, uint64_t partition_lba, uint64_t partition_bytes);
void          img_ntfs_ctx_free(img_ntfs_ctx *c);

/* 以下 10 个是回调实现。签名必须与 ntfs_device_operations 一致，
 * 但为避免强依赖 ntfs 头，这里用 (void*) 形参；由桥接层强转。
 * 真正的桥接在 src/img_ntfs_bridge.c（仅在 IMG_WITH_NTFS=ON 时编译）。 */
int   img_ntfs_cb_open  (void *dev, int flags);
int   img_ntfs_cb_close (void *dev);
long long img_ntfs_cb_seek(void *dev, long long offset, int whence);
long long img_ntfs_cb_read (void *dev, void *buf, long long count);
long long img_ntfs_cb_write(void *dev, const void *buf, long long count);
long long img_ntfs_cb_pread (void *dev, void *buf, long long count, long long off);
long long img_ntfs_cb_pwrite(void *dev, const void *buf, long long count, long long off);
int   img_ntfs_cb_sync  (void *dev);
int   img_ntfs_cb_stat  (void *dev, void *st);
int   img_ntfs_cb_ioctl (void *dev, unsigned long request, void *argp);

/* 便捷：直接在分区上做一次读（供测试与校验复用） */
int img_ntfs_partition_read(img_qcow2 *q, uint64_t part_lba, uint64_t off, void *buf, size_t n);

#ifdef __cplusplus
}
#endif
#endif
