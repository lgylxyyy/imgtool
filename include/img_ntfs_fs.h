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
#include "img_stream.h"
#include <stdbool.h>
#include <stdint.h>

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
/* 带真实大小的列目录：逐条开 inode 取 data_size（慢，只在用户明确要大小时用）。
 * 契约：目录项的 size 记 0；拿不到大小的条目也记 0。*/
int img_ntfs_list_dir_sizes(img_ntfs_fs *fs, const char *abs_path,
                            img_ntfs_list_cb cb, void *ud);
/* 空闲/已用字节（数 $Bitmap 位 × 簇大小；不含簇以下的零碎尾部） */
int img_ntfs_free_bytes(img_ntfs_fs *fs, uint64_t *out_free, uint64_t *out_used);
/* 卷标（无卷标 → 空串；失败返回 <0） */
int img_ntfs_label(img_ntfs_fs *fs, char *out, size_t cap);
/* 删除单个文件或【空】目录（复用 libntfs-3g 的 ntfs_delete；
 * ★ 先 sync 父目录再删 —— 否则 ntfs_delete 收尾会读到未落盘的目录项（move_into 踩过）。
 * 非空目录返回 IMG_ESTATE（不递归删，避免误删整棵树）。 */
int img_ntfs_rm(img_ntfs_fs *fs, const char *abs_path);
/* 递归删除（目录树）。默认的 img_ntfs_rm 只删文件/空目录；递归必须显式调用本函数。
 * removed=删掉的条目总数（含子项）；返回 IMG_ESTATE 表示非空且没开递归/或超深。 */
int img_ntfs_rm_tree(img_ntfs_fs *fs, const char *abs_path, int *removed);
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
/* allow_dir_move=false（默认，现政策）：目录只允许【同父改名】，跨父搬回 IMG_ENOTSUP。
 * =true 是"真 Windows 验收/专家"用的显式开关（App 不要传），见 SPEC-NTFS-MOVE.md §8。 */
int img_ntfs_move_path(img_ntfs_fs *fs, const char *src_abs, const char *dst_abs,
                       bool overwrite, bool allow_dir_move);
/* 目录父引用自洽检查（只读）：父引用指向谁、按路径算的父是谁、父索引是否指回本目录。
 * 搬过目录之后（ntfs.move / move_into）必须全真。
 * ★ 实测：Windows 卷的 $I30 里【没有 "."/".." 索引项】（dotdot_present=false），
 *   父引用的权威来源是该目录自己的 $FILE_NAME.parent_directory；
 *   dotdot_present=true 只在少数写了 ".." 项的卷上出现，那时它才是必须同步的地方。
 * 出参都可以传 NULL。返回 IMG_OK / IMG_ENOENT / IMG_ENOTSUP（路径不是目录）。*/
int img_ntfs_dir_parent_check(img_ntfs_fs *fs, const char *abs_path,
                              uint64_t *out_ino, uint64_t *out_parent_ino,
                              uint64_t *out_parent_by_path, bool *out_backlink_ok,
                              bool *out_dotdot_present);
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

/* 写后自证（2026-10-23 加，防"幽灵目录/幽灵项"）：
 * img_ntfs_verify_dir(abs)  —— 这个目录必须【从盘上】查得到，且它的 MFT 记录真的是目录，
 *                              且记录里有一条"同名 + 父引用指回上一层"的 $FILE_NAME。
 * img_ntfs_verify_path(abs) —— 从盘上按路径重新解析 ino/是否目录（只读，不改任何东西）。
 * 两者失败都返回 IMG_ECORRUPT/IMG_ENOENT，绝不"看起来成功"。 */

/* ---------- 盘内复制（2026-10-23 新增；契约见 docs/…/工具能力与App集成手册.md §22） ----------
 * 语义对齐 Windows「同卷复制」：新 inode、内容逐字节、继承父目录 ACL（不是 everyone）、
 * 可选保留 $SI/$FN 四个时间。写的时候先落到同目录临时名 `.imgtmp-*`（隐藏），
 * 全部写完再【一次原子改名】到目标名 ⇒ 别人永远看不到半截文件，崩溃只留临时文件。
 * dst 是目录（或路径以 / 结尾）时，目标名取 src 的末段。
 * conflict: fail=已存在就报错 / overwrite=覆盖（目标若是目录则拒） / rename=自动 .001 编号
 * 返回：IMG_OK / IMG_EEXISTS / IMG_ESTATE(目标目录) / IMG_EFULL / IMG_EIO / IMG_ENOTSUP(源是目录) */
typedef enum {
    IMG_COPY_FAIL      = 0,   /* 已存在 ⇒ EEXISTS（默认，最安全） */
    IMG_COPY_OVERWRITE = 1,   /* 覆盖已存在的文件 */
    IMG_COPY_RENAME    = 2    /* 自动改名 xxx.001（MT/资源管理器行为） */
} img_copy_conflict_t;

typedef struct {
    int  conflict;          /* img_copy_conflict_t；默认 IMG_COPY_FAIL */
    bool preserve_times;    /* 抄源的 $SI/$FN 四时间 */
    bool create_parents;    /* 目标父目录不存在就建（默认 true） */
} img_copy_opts;

typedef struct {
    char     path[512];     /* 实际写入的盘内绝对路径（改名后可能不是用户给的） */
    uint64_t ino;           /* 新文件 inode（App 用来做 stable id / 局部刷新） */
    uint64_t size;
    bool     replaced;      /* 是否覆盖了原有文件 */
} img_copy_result;

int img_ntfs_copy(img_ntfs_fs *fs, const char *src_abs, const char *dst_abs,
                  const img_copy_opts *opt, img_prog *prog, img_copy_result *out);

/* ---------- 命名数据流（ADS / alternate data streams） ----------
 * NTFS 允许一个文件挂多条 $DATA：无名的就是"文件内容"，
 * 命名的（如 Zone.Identifier）是附加数据。Windows 的复制会保留它们；
 * 我们的 file.copy（NTFS→NTFS）也保留，NTFS→FAT/宿主 会丢弃（FAT 没有这个概念）。
 * 这些接口同时给 UI 浏览/调试用。*/
typedef void (*img_ntfs_xattr_cb)(void *ud, const char *name, uint64_t size);
int  img_ntfs_list_xattrs(img_ntfs_fs *fs, const char *abs_path,
                          img_ntfs_xattr_cb cb, void *ud);      /* 返回条数或 IMG_E* */
int  img_ntfs_get_xattr(img_ntfs_fs *fs, const char *abs_path, const char *name,
                        void **out, size_t *out_len);           /* 调用方 free */
int  img_ntfs_set_xattr(img_ntfs_fs *fs, const char *abs_path, const char *name,
                        const void *data, size_t len);
/* 把 src 上所有命名流复制到 dst（两个路径可以在不同卷/不同镜像）。
 * count 返回成功复制的流数。目标已有同名流会被覆盖。*/
int  img_ntfs_copy_xattrs(img_ntfs_fs *src_fs, const char *src_abs,
                          img_ntfs_fs *dst_fs, const char *dst_abs, int *count);

/* ---------- 流式读写（跨分区/跨镜像/宿主复制的地基，2026-10-24） ----------
 * rd_open 打开一条"只读文件句柄"：拿到大小/时间/属性，之后按 off 随机读。
 * 失败：IMG_ENOENT 路径不存在 / IMG_ENOTSUP 是目录 / IMG_EIO 打不开属性。*/
typedef struct img_ntfs_rd img_ntfs_rd;
int  img_ntfs_rd_open(img_ntfs_fs *fs, const char *abs_path, img_ntfs_rd **out,
                      img_src_meta *meta);
int  img_ntfs_rd_read(img_ntfs_rd *rd, uint64_t off, void *buf, size_t want, size_t *got);
void img_ntfs_rd_close(img_ntfs_rd *rd);

/* 把任意 img_stream 写成盘内文件。语义与 img_ntfs_copy 完全一致：
 * 同目录临时名 → 写完一次性原子改名；失败/取消清理临时文件；写后自证。
 * meta 里的 times（且 preserve_times=true 时）/flags 会照抄。
 * dst 是目录（或以 / 结尾）时目标名怎么定由调用方决定（这里按字面路径写）。*/
/* 在 dir_abs 里找一个不冲突的名字（"名 (1).扩展名"）。返回 IMG_OK/IMG_EEXISTS。*/
int img_ntfs_unique_name(img_ntfs_fs *fs, const char *dir_abs, const char *base,
                         char *out, size_t cap);

int img_ntfs_write_stream(img_ntfs_fs *fs, const char *dst_abs,
                          const img_stream *src, const img_src_meta *meta,
                          const img_copy_opts *opt, img_prog *prog,
                          img_copy_result *out);

int img_ntfs_verify_dir (img_ntfs_fs *fs, const char *abs_path);
int img_ntfs_verify_path(img_ntfs_fs *fs, const char *abs_path,
                         uint64_t *ino_out, bool *isdir_out);
/* 读 $SI 四个时间（unix 秒 UTC）：out = [create, modify, access, mft_change] */
int img_ntfs_file_times(img_ntfs_fs *fs, const char *abs_path, uint64_t out[4]);
/* 按"目录 + 名字"取 ino（只查索引；给 list --ino / --after 游标用） */
int img_ntfs_dir_lookup_ino(img_ntfs_fs *fs, const char *dir_abs, const char *name,
                            uint64_t *out_ino);
/* 目录 rev（$SI 改动时间，unix 秒）；App 用它判断列表缓存是否过期 */
int img_ntfs_dir_rev(img_ntfs_fs *fs, const char *dir_abs, uint64_t *out_rev);
/* 按 unix 秒设置 $SI 四时间（in=[create,modify,access,mft_change]；0=该项不动） */
int img_ntfs_set_times(img_ntfs_fs *fs, const char *abs_path, const uint64_t in[4]);

#endif
