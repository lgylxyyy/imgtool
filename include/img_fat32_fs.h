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
/* 是否是【目录】（路径不存在/是文件 → false）。移动前查目标父目录用。*/
bool img_fat32_dir_exists(img_fat32_fs *fs, const char *abs_path);
/* 剩余空间（字节）。扫描 FAT 数空簇，结果缓存到 FAT 下次被修改 */
uint64_t img_fat32_free_bytes(img_fat32_fs *fs);
int  img_fat32_file_size(img_fat32_fs *fs, const char *abs_path, uint64_t *out);

/* 递归建目录；已存在则成功 */
int img_fat32_mkdir_p(img_fat32_fs *fs, const char *abs_path);

/* 写文件（自动建父目录，覆盖已存在）。prog 可为 NULL */
int img_fat32_put_file(img_fat32_fs *fs, const char *local_path,
                       const char *dst_abs_path, img_prog *prog);

/* 写内存里的数据为文件（ESP 里常有几个 KB 的小文件） */
int img_fat32_put_data(img_fat32_fs *fs, const void *data, size_t len,
                       const char *dst_abs_path);

/* 读文件（调用方 free）。⚠ 整文件进内存，只适合小文件 */
int img_fat32_read_file(img_fat32_fs *fs, const char *abs_path,
                        void **out, size_t *out_len);
/* 流式导出到本地文件（1 MiB 缓冲；prog 可为 NULL）*/
int img_fat32_copy_out(img_fat32_fs *fs, const char *abs_path, const char *dst_path,
                       img_prog *prog, uint64_t *out_bytes);

/* 列目录：每项回调 name/is_dir/size。返回 IMG_OK */
typedef void (*img_fat32_list_cb)(void *ud, const char *name, bool is_dir, uint64_t size);
int img_fat32_list_dir(img_fat32_fs *fs, const char *abs_path,
                       img_fat32_list_cb cb, void *ud);

/* 删除文件或（空）目录 */
int img_fat32_rm(img_fat32_fs *fs, const char *abs_path);
/* 递归删除目录树（默认 rm 只删文件/空目录）。removed=删掉的条目总数。 */
int img_fat32_rm_tree(img_fat32_fs *fs, const char *abs_path, int *removed);

/* 改名 / 移动（同一分区内；目录也可以，会同步子目录里的 ".." 项）。
 * 目标已存在 → IMG_EEXIST；把目录移进自己的子树 → IMG_EINVAL。*/
int img_fat32_rename(img_fat32_fs *fs, const char *src_abs_path, const char *dst_abs_path);
/* 盘内复制（FAT32）：src/dst 都是分区内绝对路径。dst 是目录（或以 / 结尾）时
 * 目标名取 src 末段。conflict: 0=fail(EEXISTS) 1=overwrite 2=rename(.001)。
 * ★FAT 没有"原子改名"以外的保护：实现是"写临时名 → 删除旧项 → 改名"，
 *   中途掉电会留下 .imgtmp 残件（不会留下半截目标）。时间戳按 FAT 惯例盖当前时间。 */
int img_fat32_copy(img_fat32_fs *fs, const char *src_abs, const char *dst_abs,
                   int conflict, img_prog *prog,
                   char *out_path, size_t out_cap, uint64_t *out_size, bool *out_replaced);
/* 盘内复制（FAT32）：src/dst 都是分区内绝对路径。dst 是目录（或以 / 结尾）时
 * 目标名取 src 末段。conflict: 0=fail(EEXISTS) 1=overwrite 2=rename(.001)。
 * ★FAT 没有"原子改名"以外的保护：实现是"写临时名 → 删除旧项 → 改名"，
 *   中途掉电会留下 .imgtmp 残件（不会留下半截目标）。时间戳按 FAT 惯例盖当前时间。 */
int img_fat32_copy(img_fat32_fs *fs, const char *src_abs, const char *dst_abs,
                   int conflict, img_prog *prog,
                   char *out_path, size_t out_cap, uint64_t *out_size, bool *out_replaced);

#ifdef __cplusplus
}
#endif
#endif
