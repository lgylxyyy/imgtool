/* img_wim_apply.h — Stage 5：把 WIM/ESD 直接灌进 qcow2 的 NTFS 分区
 *
 * ★ 走法（联网查证后选定，2026-10）：
 *   wimlib 的 NTFS 卷模式（WIMLIB_EXTRACT_FLAG_NTFS）会调 ntfs_mount()，
 *   而 ntfs_mount 用宏 ntfs_device_default_io_ops = ntfs_device_unix_io_ops
 *   建设备 —— 我们在 img_ntfs_fmt.c 里【定义了同名符号】把 I/O 前插到 qcow2。
 *   于是 wimlib 解压出来的数据直接落进 qcow2 分区。
 *
 *   结果：
 *     ✅ 零临时目录（干掉 legacy 的 11GB tempdir）
 *     ✅ 零 loop 设备 / 零 FUSE / 零 root / 零挂载
 *     ✅ NTFS 元数据全保留（ACL、硬链接、ADS、8.3 名、时间戳）
 *     ✅ 流式解压，磁盘只多写目标分区的空间
 *
 * 实测（8GB qcow2 + 6.6GB OS 分区 + Winre.wim）：
 *   22,682 文件 / 1,106 MiB / 14.7 秒
 *   上游 ntfsls 复核到 winload.efi(1,805,176B)、ntoskrnl.exe(10,218,296B)
 *
 * 依赖：libwim (wimlib) + libntfs-3g
 * 注意：wimlib 的目标路径只是个「探测用占位」，真正 I/O 全在我们的回调里；
 *       目标必须是【普通文件】（不能是目录），wimlib 先 mkdir 探测，
 *       EEXIST 且非目录时才会走 NTFS 卷模式 —— 所以要先建一个占位文件。
 */
#ifndef IMG_WIM_APPLY_H
#define IMG_WIM_APPLY_H
#include "img_qcow2.h"
#include "img_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 打开 WIM/ESD 并列出镜像索引/名称/大小（用于 App 选镜像）
 * buf 输出类似 JSON 的一行一个镜像 */
int img_wim_list_images(const char *wim_path, char *buf, int cap);

/* ---------- 尺寸估算（App 拿数字自己决定分区大小；政策不在工具里） ----------
 * 依据：WIM/ESD 的 XML 里就记着该镜像的实际数据量与文件/目录数，
 *      再按经验加"落到 NTFS 后的元数据开销"和安全余量。
 * ★ 这是【估算】：真正写入时若空间不够，写中错误照样会报清楚
 *   （见 DESIGN-OUTPUT.md 第五节：默认不校验，App 显式请求才校验）。*/
typedef struct {
    int      index;
    char     name[128];
    char     compression[24];
    uint64_t data_bytes;        /* 镜像里文件数据的总字节（未压缩） */
    uint64_t file_count;
    uint64_t dir_count;
    uint64_t est_ntfs_bytes;    /* 估算：落到 NTFS 后的占用（含元数据 + 余量） */
} img_wim_size_t;
int img_wim_estimate(const char *wim_path, int image_idx, img_wim_size_t *out);

/* 把 WIM 的第 image_idx 个镜像（1 起）灌进 qcow2 的 part_lba 分区
 *   opts 可为 NULL：
 *     keep_acls  默认 true（NTFS 模式下才有意义）
 *   分区必须【已用 NTFS 格式化且为空】。prog 可为 NULL */
typedef struct {
    bool no_acls;      /* 不保留 ACL（Windows 权限），默认 false */
    bool no_attrs;     /* 不保留 NTFS 属性位，默认 false */
    bool wimboot;      /* WIMBoot 模式（稀疏/占位），默认 false */
} img_wim_apply_opts_t;
void img_wim_apply_defaults(img_wim_apply_opts_t *o);

/* 上一次 img_wim_apply 里 wimlib 的原始错误码（0=成功）。
 * 我们对外统一为 IMG_EIO，但诊断时没这个数字就只能瞎猜（实测中断后重跑就吃过这个亏）。*/
int img_wim_last_rc(void);
int img_wim_apply(const char *wim_path, int image_idx,
                  img_qcow2 *q, uint64_t part_lba,
                  const img_wim_apply_opts_t *opts, img_prog *prog);

#ifdef __cplusplus
}
#endif
#endif
