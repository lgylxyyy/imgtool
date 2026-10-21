/* img_host.h — 宿主（宿主磁盘/存储）空间：估算 + 查询 + 预留
 *
 * 为什么需要：写 qcow2 是【往宿主文件里写】。内部空间够 ≠ 能写完：
 *   · 内部分区不够 → 半成品分区（可重做）
 *   · 宿主存储不够 → ★ 整个镜像文件损坏（报废），比崩更糟
 * 因此宿主检查【默认开、不可关】（DESIGN-OUTPUT.md 第五节）：
 *   写前估算 → 不够就拒（卡片给"剩余 X / 需要 Y"）
 *   写中底线 → 低于 IMG_HOST_MIN_FREE 就停止写并报错（不静默半成品）
 *   可选 reserve → posix_fallocate 真预留，才叫"保证"（默认关：会失去稀疏）
 *
 * 平台（DESIGN-OUTPUT.md 第五节末尾的契约）：
 *   有路径（Windows / PC）      → statvfs / GetDiskFreeSpaceExW
 *   只有 fd（Android SAF）      → fstatfs(fd) ；fstatfs 失败才需要 App 传 host_free_bytes
 */
#ifndef IMG_HOST_H
#define IMG_HOST_H
#include "img_types.h"
#include "img_io.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_HOST_MIN_FREE  (32ull*1024*1024)   /* 写中底线：低于它就停止写 */

/* 宿主剩余可用字节（无法获取时返回 0 = 未知，调用方要用 App 传入的值兜底） */
uint64_t img_host_free_bytes_fd(int fd);
uint64_t img_host_free_bytes_path(const char *path);
uint64_t img_host_free_bytes(const img_io *io);   /* 自动选 fd / 路径 */

/* 本次写操作预计会占多少【宿主】字节（qcow2 头 + L1/L2/refcount + 数据）。
 * compressed=false → 写多少占多少（我们默认不压缩），约 +3% 元数据余量 */
uint64_t img_host_estimate_write(uint64_t internal_bytes, bool compressed);

/* 真预留（posix_fallocate）。失败时返回 IMG_ENOTSUP 或底层错误码 ——
 * 不支持不代表"不能写"，只代表"无法保证"（调用方据此决定是否继续）*/
int img_host_reserve(int fd, uint64_t bytes);

#ifdef __cplusplus
}
#endif
#endif
