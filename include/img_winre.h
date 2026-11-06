/* img_winre.h — WinRE（恢复环境）状态检测（只读）
 *
 * ★ 2026-11-04 与用户定稿：WinRE 与【引导修复】彻底解耦。
 *   boot.check / boot.repair 不再碰恢复分区属性、不再查 Winre.wim；
 *   WinRE 的"有没有、在哪、什么版本"全部走这个模块（命令 `recovery.status`）。
 *
 * 检测口径（App 的 WinRE 卡片就按这个显示）：
 *   三处找 Winre.wim ——
 *     ① 恢复分区  \Recovery\WindowsRE\Winre.wim
 *     ② 系统分区  \Windows\System32\Recovery\Winre.wim
 *     ③ 系统分区  \Recovery\WindowsRE\Winre.wim
 *   命中任一 → winre_present=true（App 显示 WinRE 卡片）
 *   三处全无 → winre_present=false（App 隐藏卡片）
 *
 *   恢复分区存在但里面没有 Winre.wim → recovery_empty=true。
 *   ★ 政策（用户拍板）：工具【不自动创建】，只给字段；App 在修复确认卡片里
 *     提示"恢复分区是空的，建议手动创建"，创建走独立的 WinRE 流程。
 */
#ifndef IMG_WINRE_H
#define IMG_WINRE_H
#include "img_types.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* ---- 恢复分区本体 ---- */
    bool     recovery_partition_present;
    uint64_t rec_lba, rec_bytes;
    bool     rec_is_gpt;
    uint64_t rec_attr;              /* GPT 属性（MBR 恒 0）*/
    bool     rec_attr_ok;           /* 官方要求 0x8000000000000001；MBR 恒 false 且不算错 */
    bool     recovery_empty;        /* 分区在，但里面没有 Winre.wim */

    /* ---- WinRE 文件 ---- */
    bool     winre_present;
    char     winre_location[16];    /* "recovery" | "system32" | "os_recovery" | "" */
    char     wim_path[300];         /* 命中的分区内绝对路径 */
    uint64_t wim_part_lba;          /* Winre.wim 在哪个分区 */
    uint64_t wim_bytes;
    bool     winre_version_known;
    int      winre_major, winre_minor, winre_build;
    char     winre_arch[16];        /* x86|x64|arm64|""（尽力而为，取 WIM XML 的 ARCH）*/
    bool     reagent_present;       /* ReAgent.xml（reagentc 认不认它）*/
    bool     bootsdi_present;       /* boot.sdi（ramdisk 加载用）*/

    /* ---- 系统分区（WinRE 的"宿主"）---- */
    bool     os_found;
    uint64_t os_lba, os_bytes;
    char     os_version[32];        /* 10.0.22621（读得到才有）*/

    /* ---- 诊断 ---- */
    char     note[200];             /* 一句人话（给日志，不给 UI 文案）*/
} img_winre_status_t;

/* 只读检测。返回 IMG_OK（结果看 out 的 bool 字段）/ IMG_E*。
 * os_lba 传 0 = 自动找（找 \Windows\System32\winload.*）。
 * prog 可为 NULL。*/
int img_winre_status(img_qcow2 *q, const img_parttable_t *t,
                     uint64_t os_lba, img_winre_status_t *out,
                     char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif
