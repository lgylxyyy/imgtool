/* img_fat32_fs.h — FAT32 文件系统读写（阶段 2b.4）
 *
 * 为什么要它：ESP 是 FAT32。此前往 ESP 放文件只能「整块抽出 512MB →
 * mtools 改 → 整块写回」，慢且浪费（ESP 通常是 100~512MB）。
 * 有了本模块，App 可以【直接】在 qcow2 的 ESP 分区里建目录、放文件、读文件。
 *
 * 实现说明：全自研（FAT32 结构简单、Microsoft 规范公开且完整），
 *   不走 libntfs-3g 那类上游库（FAT32 没有对应的成熟 C 库值得接）。
 *   核心对象：VBR/FSInfo · FAT 表（32bit，掩码 0x0FFFFFFF）·
 *            目录项（8.3 + LFN 长名，0x0F 属性）· 簇链
 *
 * 注意：ESP 上的文件名大小写不敏感；本层统一用大写 8.3 匹配 + LFN 完整名匹配。
 */
#ifndef IMG_FAT32_FS_H
#define IMG_FAT32_FS_H
#include "img_qcow2.h"
#include "img_fat32.h"
#include "img_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_fat32_fs img_fat32_fs;   /* 挂载句柄 */

/* 挂载（只读或读写）。返回 NULL 表示不是合法 FAT32 */
img_fat32_fs *img_fat32_mount(img_qcow2 *q, uint64_t part_lba, bool writable);
void img_fat32_unmount(img_fat32_fs *fs);

/* 取布局（供 App 显示容量等） */
const img_fat32_layout_t *img_fat32_fs_layout(const img_fat32_fs *fs);

bool img_fat32_path_exists(img_fat32_fs *fs, const char *abs_path);
int  img_fat32_file_size(img_fat32_fs *fs, const char *abs_path, uint64_t *out);

/* 递归建目录；已存在则成功 */
int img_fat32_mkdir_p(img_fat32_fs *fs, const char *abs_path);

/* 写文件（自动建父目录，覆盖已存在）。prog 可为 NULL */
int img_fat32_put_file(img_fat32_fs *fs, const char *local_path,
                       const char *dst_abs_path, img_prog *prog);

/* 写内存里的数据为文件（ESP 里常有几个 KB 的小文件） */
int img_fat32_put_data(img_fat32_fs *fs, const void *data, size_t len,
                       const char *dst_abs_path);

/* 读文件（调用方 free） */
int img_fat32_read_file(img_fat32_fs *fs, const char *abs_path,
                        void **out, size_t *out_len);

/* 列目录：每项回调 name/is_dir/size。返回 IMG_OK */
typedef void (*img_fat32_list_cb)(void *ud, const char *name, bool is_dir, uint64_t size);
int img_fat32_list_dir(img_fat32_fs *fs, const char *abs_path,
                       img_fat32_list_cb cb, void *ud);

/* 删除文件或（空）目录 */
int img_fat32_rm(img_fat32_fs *fs, const char *abs_path);

#ifdef __cplusplus
}
#endif
#endif
