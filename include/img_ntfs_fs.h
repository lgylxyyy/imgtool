/* img_ntfs_fs.h — NTFS 文件系统写入接口（Stage B）
 *
 * ★ 决策记录（2026-10，按"哪个不容易出问题"的判据评估）：
 *   · 自研 NTFS 写入：要处理 MFT 记录复用、$Index 平衡、$Bitmap 一致性、
 *     $LogFile 重放、MFT/MFTMirr 校验和 —— 出错是【静默数据损坏】，
 *     且我方无法在 Windows 上逐条路径验收 → 风险【高】
 *   · 用 libntfs-3g（LGPL-2.1+，已链接）：ntfs_create / ntfs_attr_pwrite /
 *     ntfs_index_add_filename 等为公开 API，20 年生产代码全球在用 → 风险【低】
 *   → 选 libntfs-3g 做【内部实现】，但接口层是【本项目自有】的，
 *     上游只出现在这里，App 侧看不到。
 *
 * 设备后端仍是我们 qcow2 的 10 个回调（无 loop / 无 FUSE / 无 root / 无临时文件）。
 */
#ifndef IMG_NTFS_FS_H
#define IMG_NTFS_FS_H
#include "img_qcow2.h"
#include "img_progress.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_ntfs_fs img_ntfs_fs;   /* 挂载句柄（可读写） */

/* 读写挂载；失败返回 NULL（errno 留在系统里） */
img_ntfs_fs *img_ntfs_mount_rw(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes);
/* 卸载并落盘（内部会 ntfs_umount → 自动释放设备与我们的上下文）*/
void img_ntfs_umount(img_ntfs_fs *fs);

/* 递归建目录 "/Recovery/WindowsRE"（已存在则成功）。返回 IMG_OK */
int img_ntfs_mkdir_p(img_ntfs_fs *fs, const char *abs_path);

/* 路径存在性（文件或目录） */
bool img_ntfs_path_exists(img_ntfs_fs *fs, const char *abs_path);

/* 读整个文件到内存（调用方 free）。*out_len 返回字节数 */
int img_ntfs_read_file(img_ntfs_fs *fs, const char *abs_path, void **out, size_t *out_len);

/* 把内存数据写成文件（ESP 里常有几个 KB 的小文件） */
int img_ntfs_put_file_mem(img_ntfs_fs *fs, const void *data, size_t len,
                          const char *dst_abs_path);
/* 生成并写入 ReAgent.xml（WinRE 注册文件）
 * ★ 格式必须与 Windows 自己写的【完全一致】；真值来自官方 Windows 11 28000
 *   镜像里的 \Windows\System32\Recovery\ReAgent.xml（见函数实现注释）。
 * winre_dir_abs : 分区内目录，如 "/Recovery/WindowsRE"
 * winre_loc     : 写进 XML 的 WinreLocation path（Windows 惯例反斜杠，如 "\Recovery\WindowsRE"）
 * bcd_guid      : BCD 里 WinRE 加载器对象 GUID（reagentc /info 的 BCD identifier）
 * part_off      : WinRE 所在分区的【字节偏移】（Windows 写的就是分区起始偏移）
 * 返回 IMG_OK / IMG_E* */
int img_ntfs_write_reagent(img_ntfs_fs *fs, const char *winre_dir_abs,
                           const char *winre_loc, const char *bcd_guid,
                           uint64_t part_off);
/* 写文件：自动创建父目录，覆盖已存在文件。prog 可为 NULL */
int img_ntfs_put_file(img_ntfs_fs *fs, const char *local_path,
                      const char *dst_abs_path, img_prog *prog);

/* 取文件大小，不存在返回 IMG_ENOENT */
int img_ntfs_file_size(img_ntfs_fs *fs, const char *abs_path, uint64_t *out_size);

#ifdef __cplusplus
}
#endif
#endif
