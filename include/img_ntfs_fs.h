/* img_ntfs_fs.h — NTFS 文件系统写入接口（Stage B）
 *
 * ★ 决策记录（2026-10，按"哪个不容易出问题"的判据评估）：
 *   · 自研 NTFS 写入：要处理 MFT 记录复用、$Index 平衡、$Bitmap 一致性、
 *     $LogFile 重放、MFT/MFTMirr 校验和 —— 出错是【静默数据损坏】，
 *     且我方无法在 Windows 上逐条路径验收 → 风险【高】
 *   · 用 libntfs-3g（LGPL-2.1+，已链接）：ntfs_create / ntfs_attr_pwrite /
 *     ntfs_index_add_filename 等为公开 API，20 年生产代码全球在用 → 风险【低】
 *   → 选 libntfs-3g 做【内部实现】，但接口层是【本项目自有】的，
 *     上游只出现在这里，App 侧无法查看。
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

/* 读整个文件到内存（调用方 free）。*out_len 返回字节数。
 * ⚠ 只适合小文件（≤几十 MB）。大文件（install.wim 4 GB）会 OOM，用下面的流式版。*/
int img_ntfs_read_file(img_ntfs_fs *fs, const char *abs_path, void **out, size_t *out_len);

/* 流式导出到本地文件（1 MiB 一块，边读边写；prog 可为 NULL）。
 * 与 read_file 的区别：内存峰值恒定 1 MiB，可对 GB 级文件用，且能报进度。*/
int img_ntfs_copy_out(img_ntfs_fs *fs, const char *abs_path, const char *dst_path,
                      img_prog *prog, uint64_t *out_bytes);

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

/* ---------- part.scan / 搬移 用的底层（第2、3件）---------- */
typedef void (*img_ntfs_list_cb)(void *ud, const char *name, bool is_dir, uint64_t size);
/* 列目录（size 恒为 0：NTFS 取大小要再开 inode，调用方要大小用 img_ntfs_file_size）
 * 契约与 img_fat32_list_dir 一致；不含 "."/".."；名字为 UTF-8 */
int img_ntfs_list_dir(img_ntfs_fs *fs, const char *abs_path,
                      img_ntfs_list_cb cb, void *ud);
/* 空闲/已用字节（数 $Bitmap 位 × 簇大小；不含簇以下的零碎尾部） */
int img_ntfs_free_bytes(img_ntfs_fs *fs, uint64_t *out_free, uint64_t *out_used);
/* 卷标（无卷标 → 空串；失败返回 <0） */
int img_ntfs_label(img_ntfs_fs *fs, char *out, size_t cap);
/* 删除单个文件或【空】目录（复用 libntfs-3g 的 ntfs_delete；
 * ★ 先 sync 父目录再删 —— 否则 ntfs_delete 收尾会读到未落盘的目录项（move_into 踩过）。
 * 非空目录返回 IMG_ESTATE（不递归删，避免误删整棵树）。 */
int img_ntfs_rm(img_ntfs_fs *fs, const char *abs_path);
/* 搬移原语：把【给定名字】移进 dst_dir（dst_dir 不存在则先建）
 * ★ 层归属：这是机制不是政策 —— 搬谁由调用方（App）给名单；
 *   搬完每个名字保持不变（/Windows → /Windows.old/Windows）。
 * moved_out 返回实际搬动个数。返回 IMG_OK/IMG_E* */
int img_ntfs_move_into(img_ntfs_fs *fs, const char *dst_dir,
                       const char **names, int nnames, int *moved_out);
/* 改名/移动【任意单条路径】（src_abs/dst_abs 都以 / 开头；同目录改名的 dst 就换末段名）。
 * 机制：新位置 ntfs_link → sync 新父 → 旧位置 ntfs_delete（先加后减，失败最坏是两处都在）。
 * 返回 IMG_OK / IMG_ENOENT(源或源父不存在) / IMG_EEXISTS(目标已存在且 overwrite=false)
 *      / IMG_ENOTSUP(overwrite 时目标是目录) / IMG_EFULL / IMG_EIO。
 * ★ 目录：见 img_ntfs_fs.c 里 move_path 的注释与 tests/ntfs-move-test.sh 的实测结论。*/
int img_ntfs_move_path(img_ntfs_fs *fs, const char *src_abs, const char *dst_abs, bool overwrite);
/* 卷是否被标脏（掉电/进程被杀 → NTFS $Volume flags bit0）。
 * ★ 单独做的理由：脏卷 ntfs-3g 只给【只读挂载】→ wimlib 直写秒失败，
 *   调用方只看到 WIM-APPLY-FAIL，猜不到"加 --format=on 就行"。*/
int img_ntfs_dirty_check(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes, bool *out_dirty);
/* 灌入前的只读预检：一次挂载同时回答"卷脏吗""根目录有几项"。
 * ★ 为什么必须查"非空"：实测中断后重跑 wim.apply --format=off 会【挂住】——
 *   100 秒零进展、盘一个字节都不长，既不报错也不继续，用户只能干等。
 *   主动拒绝（NTFS-NOT-EMPTY）比挂住好得多。*/
int img_ntfs_precheck(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                      bool *out_dirty, int *out_root_entries);


#ifdef __cplusplus
}
#endif
#endif
