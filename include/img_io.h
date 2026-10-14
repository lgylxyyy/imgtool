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
img_io *img_io_open_path(const char *path, const img_io_opts_t *o);
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

/* 稀疏指示：告诉系统“这段是空的，别占真实空间”。不支持时静默忽略。 */
int img_io_punch(img_io *io, uint64_t off, uint64_t len);

#ifdef __cplusplus
}
#endif
#endif
