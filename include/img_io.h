/* img_io.h — 文件读写抽象层
 *
 * ★ 全库唯一允许直接碰“路径”的地方。Android 上 SAF 只能给 fd，
 *   所以把 img_io 的后端换掉即可，其余所有代码不用改。
 *
 * 模式：
 *   IMG_IO_PATH   — 用路径（PC/开发期）
 *   IMG_IO_FD     — 用调用者传入的 fd（Android SAF）
 */
#ifndef IMG_IO_H
#define IMG_IO_H
#include "img_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_io img_io;

typedef struct {
    bool    writable;    /* 需要写权限 */
    bool    truncate;    /* 打开时截断 */
    uint64_t initial_size;/* IMG_IO_CREATE 时的初始大小（字节），0=不预分配 */
} img_io_opts_t;

/* —— 打开/关闭 —— */
/* 打开（可写时允许创建）。★ 只有"建盘"这类真的要产出新文件的调用者该用它。*/
img_io *img_io_open_path(const char *path, const img_io_opts_t *o);
/* ★ 打开【已存在】的文件，绝不创建（对已存在性有强要求时的唯一正确入口）。
 *   背景（手册 §4.5，实测）：命令表/JNI 的镜像入口若走 img_io_open_path(writable=true)，
 *   用户在 App 里选错或手输一个不存在的路径时，会在盘上留下一个 0 字节的 *.qcow2 垃圾，
 *   并且报的是 QC-OPEN-FAIL（看起来像"镜像损坏"）而不是"文件不存在"。
 *   本函数先 stat（必须是普通文件或块设备）再不带 O_CREAT 打开：
 *     · 路径不存在/是目录/fd 无法获取 → 返回 NULL（调用方报 IMG-OPEN-FAIL）
 *     · 绝不产生新文件 */
img_io *img_io_open_existing(const char *path, bool writable);
img_io *img_io_open_fd(int fd, bool take_ownership, const img_io_opts_t *o);
void    img_io_close(img_io *io);

/* —— 基础读写（off = 镜像内偏移，与 qcow2 的“文件偏移”是同一个坐标系）—— */
int img_io_read (img_io *io, uint64_t off, void *buf, size_t len, size_t *done);
int img_io_write(img_io *io, uint64_t off, const void *buf, size_t len, size_t *done);
int img_io_sync (img_io *io);
int img_io_truncate(img_io *io, uint64_t new_size);

/* —— 便捷：读满/写满（内部循环直到完成或出错）—— */
int img_io_read_full (img_io *io, uint64_t off, void *buf, size_t len);
int img_io_write_full(img_io *io, uint64_t off, const void *buf, size_t len);

uint64_t img_io_size(img_io *io);
bool     img_io_writable(const img_io *io);
bool     img_io_is_fd(const img_io *io);   /* Android 判定用 */
int      img_io_fd(img_io *io);            /* 不是 fd 模式返回 -1 */
const char *img_io_path(const img_io *io); /* 路径模式返回路径；fd 模式返回 NULL */

/* 稀疏指示：告诉系统“这段是空的，别占真实空间”。不支持时静默忽略。 */
int img_io_punch(img_io *io, uint64_t off, uint64_t len);

#ifdef __cplusplus
}
#endif
#endif
