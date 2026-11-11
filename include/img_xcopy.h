/* img_xcopy.h — 跨分区/跨镜像/宿主↔镜像/ISO/WIM → 镜像 的统一复制引擎
 *
 * 为什么单独一层（2026-10-24）：
 *   同卷复制（ntfs.copy / fat.copy）是"读一个 inode 写另一个 inode"，
 *   而用户真正要的是文件管理器的四件套 —— 跨分区、跨 qcow2、和手机目录互拷、
 *   从 ISO/WIM/ESD 里往外拷。这些组合如果各写一遍就是 5×3=15 份代码，
 *   每份都要处理目录递归/冲突/时间戳/取消 —— 必然分叉。
 *
 * 设计：把"源"抽象成 img_stream（见 img_stream.h），把"目标"抽象成
 *   3 个写入口（NTFS 流式写 / FAT32 流式写 / 宿主文件写）。
 *   本模块只负责：解析两侧位置 → 打开端点 → 递归遍历 → 冲突策略 → 元信息。
 *
 * 位置语法（一个参数说清"哪张盘、哪个分区、哪个文件"）：
 *   ntfs:/Windows/System32/x.dll   + --src_img=xxx.qcow2 [--src_lba=N]
 *   fat:/EFI/BOOT/BOOTX64.EFI      + --src_img=xxx.qcow2 [--src_lba=N]
 *   host:/sdcard/Download/a.bin    （宿主机真实路径；Android 上要 App 有权限/走真实路径）
 *   iso:/sources/install.wim       + --src_img=xxx.iso
 *   wim:/Windows/System32/x.dll    + --src_img=install.esd --src_index=1
 * 目标只允许 ntfs / fat / host。
 *
 * lba 省略（0）时自动挑：镜像里该文件系统恰好只有一个分区才挑，多个就报
 * COPY-LBA-AMBIGUOUS —— 绝不猜着写。
 */
#ifndef IMG_XCOPY_H
#define IMG_XCOPY_H
#include "img_types.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_XK_NONE = 0,
    IMG_XK_NTFS,
    IMG_XK_FAT,
    IMG_XK_HOST,
    IMG_XK_ISO,
    IMG_XK_WIM,
    IMG_XK_ARCHIVE      /* 压缩包（只读；REQ-31）*/
} img_xkind;

typedef struct {
    img_xkind kind;
    char      img[2048];    /* qcow2 / iso / wim 路径；host 为空 */
    uint64_t  lba;         /* ntfs/fat 分区起始 LBA；0=自动 */
    int       index;       /* wim 映像索引（默认 1） */
    char      path[2048];  /* 文件系统内绝对路径 / 宿主绝对路径 */
} img_xloc;

#define IMG_XCOPY_EXCL_MAX 24      /* --exclude 名单上限（默认 3 条 + 用户 16 条够用）*/

typedef struct {
    int  conflict;         /* IMG_COPY_FAIL / OVERWRITE / RENAME（见 img_ntfs_fs.h） */
    bool recursive;        /* 源是目录时必须为 true */
    bool preserve_times;   /* 照抄源时间（NTFS 4 个；FAT 5 字段；宿主 atime/mtime） */
    bool create_parents;   /* 目标父目录不存在就建（默认 true） */
    bool follow_symlinks;  /* 宿主源：false=跳过符号链接并计数（默认，最安全） */
    int  max_depth;        /* 递归深度上限；0=默认 32 */
    /* ---- REQ-32（Android 系统回收站）----
     * 只作用于【递归进入的子项】的 basename；顶层 --src 自身永远不参与排除
     * （用户手动选中 .trashed-* 导出时，必须能复制出去）。大小写敏感。*/
    bool skip_trashed;                 /* .trashed-* 前缀跳过（copy 默认 true；move 恒 false）*/
    char exclude[IMG_XCOPY_EXCL_MAX][128];   /* basename 通配（fnmatch：* ? []）*/
    int  n_exclude;
    /* ★ REQ-68：跨文件累计口径（wim.capture 的抽取阶段用）。
     *   0 = 关闭，保持 copy 命令"按单文件报"的现有行为。*/
    uint64_t agg_bytes_total;
    uint64_t agg_files_total;
    /* ★ T4（2026-11-11）：宿主 → 宿主复制时要不要保住文件的洞。
     *   raw 映像的 st_size 是容量、st_blocks 才是占用：逐字节复制会把 64 GiB
     *   的稀疏 raw 变成 64 GiB 实体占用（手机直接爆盘）。IMG_SPARSE_AUTO 时
     *   只有源真的有洞才分段复制；没有洞的文件行为完全不变。*/
    int sparse;            /* img_sparse_mode；0=AUTO */
} img_xcopy_opts;

/* copy --sparse 的取值 */
typedef enum {
    IMG_SPARSE_AUTO   = 0,   /* 默认：源有洞就保持，没洞就普通复制 */
    IMG_SPARSE_ALWAYS = 1,   /* 总是按洞复制（等同 auto，但明确表态） */
    IMG_SPARSE_NEVER  = 2    /* 逐字节完整写：目标一定实体化（用户明确要"完整副本"） */
} img_sparse_mode;

typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t bytes;
    uint64_t skipped;      /* 跳过的符号链接/不支持项 */
    uint64_t ads;          /* 复制的命名数据流条数（仅 NTFS→NTFS） */
    uint64_t skipped_trashed;   /* 被 .trashed-* 规则跳过的条目数（REQ-32）*/
    uint64_t skipped_excluded;  /* 被 --exclude 规则跳过的条目数 */
    char     final_path[2048];   /* 顶层落点（改名后的真实路径；给 UI 刷新用） */
    uint64_t parent_total;       /* 落点所在目录的条目数（UI 打补丁用） */
    uint64_t top_size;           /* 顶层文件大小（目录为 0） */
    uint64_t top_ino;            /* 顶层文件 ino（NTFS 才有；FAT/宿主为 0） */
    uint64_t archive_bytes;      /* 目标是压缩包时：重写后的包大小（其余情况 0）*/
    bool     replaced;           /* 顶层文件是否覆盖了旧文件 */
    int      side;               /* 失败发生在哪一侧：0=复制中 / 1=源 / 2=目标 */
    char     first_error[256];
    char     bad_name[1024];     /* 被拒绝的目标名字（NAME-INVALID 时给 err.at 用）*/
    /* ---- T4：稀疏口径（只对宿主 → 宿主复制有意义）---- */
    bool     sparse_source;      /* 源文件有洞 */
    uint64_t hole_bytes;         /* 源的洞总字节（SEEK_DATA/SEEK_HOLE 量出来的） */
    bool     sparse_kept;        /* 目标是否保住了洞（false ⇒ 按完整容量写入了） */
    uint64_t out_alloc_bytes;    /* 目标实际占用（st_blocks×512） */
} img_xcopy_stats;

/* 解析 "ntfs:/a/b" 这类位置串。失败返回 IMG_EINVAL 并填 why。 */
int img_xcopy_parse(const char *spec, img_xloc *out, char *why, size_t whycap);

/* 执行复制。返回 IMG_OK / IMG_E*（错误码由命令层翻译成稳定 error_code）。
 * 失败时 stats 里仍有"已经复制了多少"的诚实计数（不做整体回滚 —— 已落盘的就是落了）。*/
int img_xcopy_run(const img_xloc *src, const img_xloc *dst,
                  const img_xcopy_opts *opt, img_prog *prog,
                  img_xcopy_stats *st);

/* ---------------- 统一移动 / 删除（2026-10-24） ----------------
 * move 的判定：
 *   · 同容器 + 同分区 + 同文件系统 → 纯索引移动/改名（毫秒级）
 *   · 其余 → 复制 → 校验 → 删源（跨盘/跨镜像/宿主↔镜像）
 * 跨盘删源是不可逆动作：
 *   · 镜像内的源：复制成功即删；
 *   · 宿主源：必须 allow_host_delete=true（App 弹窗后再传）；
 *   · ISO/WIM 源：直接拒绝（只读链）。
 * 删除失败返回 IMG_EPARTIAL，stats.deleted=false，两个路径都在。*/
typedef struct {
    int  conflict;
    bool recursive;
    bool preserve_times;
    bool create_parents;
    bool allow_host_delete;   /* 宿主源删除许可 */
    bool verify;              /* 单文件复制后校验大小 */
    int  max_depth;
    int  sparse;              /* img_sparse_mode；跨卷移动时透传给复制引擎（T4） */
} img_xmove_opts;

typedef struct {
    char     mode[24];        /* rename / move / cross_volume */
    uint64_t files, dirs, bytes, ads;
    bool     deleted;         /* 源是否已删 */
    bool     noop;            /* mv a a：什么都没做 */
    bool     case_only;       /* 只改大小写 */
    bool     by_rename;       /* 真的没搬数据：走的是 rename(2)（同卷/同分区/同镜像）*/
    /* ---- T4：稀疏口径（只对宿主 → 宿主有意义）---- */
    bool     sparse_source;      /* 源文件有洞（st_blocks 明显小于 st_size） */
    uint64_t hole_bytes;         /* 源的洞总字节（SEEK_DATA/SEEK_HOLE 量出来的） */
    bool     sparse_kept;        /* 目标是否保住了洞（false ⇒ 按完整容量写入了） */
    uint64_t out_alloc_bytes;    /* 目标实际占用（st_blocks×512） */
    char     final_path[2048];
    int      side;
    char     first_error[256];   /* 失败原因的说明（如非法名字具体哪个字符） */
    char     bad_name[1024];     /* 被拒绝的目标名字（NAME-INVALID 时给 err.at 用）*/
} img_xmove_stats;

int img_xmove_run(const img_xloc *src, const img_xloc *dst,
                  const img_xmove_opts *opt, img_prog *prog,
                  img_xmove_stats *st);

/* 统一删除。loc 可以是 HOST / NTFS / FAT；ISO/WIM 拒绝（IMG_EREADONLY）。
 * 宿主删除必须 allow_host_delete=true；根目录/系统顶层一律拒绝。*/
int img_xrm_run(const img_xloc *loc, bool recursive, bool allow_host_delete,
                uint64_t *removed);

/* 给命令层：kind 的稳定名字 */
const char *img_xkind_name(img_xkind k);

#ifdef __cplusplus
}
#endif
#endif
