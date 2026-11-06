/* img_recovery.h — 恢复分区 / WinRE 的"查、填、验"机制
 *
 * 层归属（DESIGN-OUTPUT.md §十一）：
 *   工具只提供"在 WIM/ESD 里找 WinRE、往分区里填、设 GPT 属性"这些机制；
 *   "建不建、多大、未找到怎么办"全归 App（默认不建 —— 用户 2026-10-19 定的政策）。
 */
#ifndef IMG_RECOVERY_H
#define IMG_RECOVERY_H
#include "img_types.h"
#include "img_qcow2.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     found;
    int      index;              /* 命中的映像索引（从 1 起）*/
    char     container[8];       /* "wim" / "esd" */
    char     internal_path[256]; /* 命中路径（WIM 内绝对路径）*/
    uint64_t size_bytes;
} img_winre_info_t;

/* 在 WIM/ESD 里找 WinRE。
 *   index = 0 → 扫全部映像；>0 → 只看该索引。
 * 搜索顺序：\Windows\System32\Recovery\Winre.wim → \Recovery\WindowsRE\Winre.wim
 * 返回：IMG_OK（info->found 可能为 false）/ IMG_ENOENT（WIM 无法打开）/ IMG_EINVAL。
 * ★ 只读：不落盘、不改源文件。*/
int img_winre_find(const char *wim_path, int index, img_winre_info_t *info);

/* 把 WIM/ESD 里的某个文件抽到本地（内部用 wimlib_extract_paths）。
 * 供 recovery.populate 把 Winre.wim 落到临时文件，再写进 NTFS 分区。*/
int img_winre_extract_file(const char *wim_path, int index, const char *internal_path,
                           const char *dst_path, char *err, size_t errcap);

/* 往【NTFS 分区】里放 WinRE 三件套。
 *   os_lba != 0 → 落点用 \Windows\System32\Recovery（BIOS 约定，系统分区内）
 *   os_lba == 0 → 落点用 \Recovery\WindowsRE（UEFI 独立恢复分区）
 *   format=true 会先格式化该分区（★会毁数据，App 必须确认过）
 *   bootsdi_src 可为 NULL（那就只放 Winre.wim）
 * 返回 IMG_OK / 具体错误；written_bytes 返回 Winre.wim 的字节数。*/
int img_recovery_populate(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                          const char *winre_src, int winre_index,
                          const char *winre_internal_path,
                          const char *bootsdi_src,
                          bool do_format, const char *label,
                          bool bios_layout,
                          img_prog *prog, uint64_t *written_bytes,
                          char *err, size_t errcap);

/* 把 WIM/ESD/ISO 里的一个文件直接写进 qcow2 里的 NTFS 分区（REQ-29，0 临时文件）。
 * dst_abs 是盘内绝对路径（父目录会按需创建）。*/
int img_put_wim_file(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                     const char *wim_path, int index, const char *internal_path,
                     const char *dst_abs, img_prog *prog, uint64_t *written,
                     char *err, size_t errcap);

/* 设/清一个 GPT 分区的 attributes（恢复分区官方要求 0x8000000000000001）。
 * 只对 GPT 有效；MBR 返回 IMG_ENOTSUP。*/
int img_part_set_attributes(img_qcow2 *q, uint64_t part_lba, uint64_t attributes);

/* 写"首次开机自动启用 WinRE"的 SetupComplete.cmd（与 winre.auto 同一份内容）。
 * 返回写入字节数。*/
int img_winre_write_enable_script(img_qcow2 *q, uint64_t os_lba, uint64_t os_bytes,
                                  size_t *out_bytes);

#ifdef __cplusplus
}
#endif
#endif
