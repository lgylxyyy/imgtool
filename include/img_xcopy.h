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
    IMG_XK_WIM
} img_xkind;

typedef struct {
    img_xkind kind;
    char      img[512];    /* qcow2 / iso / wim 路径；host 为空 */
    uint64_t  lba;         /* ntfs/fat 分区起始 LBA；0=自动 */
    int       index;       /* wim 映像索引（默认 1） */
    char      path[1024];  /* 文件系统内绝对路径 / 宿主绝对路径 */
} img_xloc;

typedef struct {
    int  conflict;         /* IMG_COPY_FAIL / OVERWRITE / RENAME（见 img_ntfs_fs.h） */
    bool recursive;        /* 源是目录时必须为 true */
    bool preserve_times;   /* 照抄源时间（NTFS 4 个；FAT 5 字段；宿主 atime/mtime） */
    bool create_parents;   /* 目标父目录不存在就建（默认 true） */
    bool follow_symlinks;  /* 宿主源：false=跳过符号链接并计数（默认，最安全） */
    int  max_depth;        /* 递归深度上限；0=默认 32 */
} img_xcopy_opts;

typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t bytes;
    uint64_t skipped;      /* 跳过的符号链接/不支持项 */
    char     final_path[1024];   /* 顶层落点（改名后的真实路径；给 UI 刷新用） */
    int      side;               /* 失败发生在哪一侧：0=复制中 / 1=源 / 2=目标 */
    char     first_error[256];
} img_xcopy_stats;

/* 解析 "ntfs:/a/b" 这类位置串。失败返回 IMG_EINVAL 并填 why。 */
int img_xcopy_parse(const char *spec, img_xloc *out, char *why, size_t whycap);

/* 执行复制。返回 IMG_OK / IMG_E*（错误码由命令层翻译成稳定 error_code）。
 * 失败时 stats 里仍有"已经复制了多少"的诚实计数（不做整体回滚 —— 已落盘的就是落了）。*/
int img_xcopy_run(const img_xloc *src, const img_xloc *dst,
                  const img_xcopy_opts *opt, img_prog *prog,
                  img_xcopy_stats *st);

/* 给命令层：kind 的稳定名字 */
const char *img_xkind_name(img_xkind k);

#ifdef __cplusplus
}
#endif
#endif