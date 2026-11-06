/* img_bootcheck.h — 引导链自检（替代"挂虚拟机看能不能起"）
 *
 * 分层检查，每项给出 id / 名称 / 级别 / 结果 / 详情。
 * 通过条件按模式不同：
 *   · UEFI/GPT：ESP 完整性 + bootmgfw.efi 存在 + BCD 可解析 + 跨引用闭合
 *   · BIOS/MBR：活动标记 + 分区类型0x07 + $Boot 0..9 + bootmgr 存在
 *   · 恢复分区：默认【不查】；只有 IMG_BOOTCHK_EXPECT_RECOVERY 才查缺没缺
 *   · 共同：   NTFS 结构、BCD 元素齐全、Windows 系统文件存在
 *
 * 用法：bccheck 把结果打成 JSON 数组，每项一行 → App 直接渲染。
 */
#ifndef IMG_BOOTCHECK_H
#define IMG_BOOTCHECK_H
#include "img_qcow2.h"
#include "img_partition.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_CK_INFO = 0,   /* 仅提示，不影响判定 */
    IMG_CK_REQUIRED,   /* 不过 = 镜像大概率无法启动 */
    IMG_CK_WARN,       /* 不过 = 能起但功能缺失 */
} img_ck_level_t;

typedef struct {
    const char    *id;        /* 稳定 ID，如 "esp.fat32" */
    const char    *name;      /* 中文名 */
    img_ck_level_t level;
    bool           pass;
    char           detail[256];
    /* ★ REQ-38：可一键修复时给出【能直接跑的命令原文】。
     *   App 不显示给用户，只在自己的修复流程里取来执行；空串 = 没有自动修复。*/
    char           suggest[320];
} img_check_t;

typedef struct {
    img_check_t items[64];
    int         count;
    int         n_required_fail;   /* REQUIRED 未通过数 */
    int         n_warn_fail;
    bool        is_uefi;
    char        img[1024];        /* 调用方填（JSON 里 suggest 要带 --img=/path）；空则用 <img> */
} img_bootcheck_result_t;

/* 模式：自动探测 / 强制某一种 */
typedef enum { IMG_BOOT_AUTO = 0, IMG_BOOT_UEFI, IMG_BOOT_BIOS } img_boot_mode_t;

/* 检查项开关（默认 0）*/
/* ★ 默认【不把"没有恢复分区"当失败/警告】—— 恢复分区是用户显式勾选才做的
 *   （DESIGN-OUTPUT.md §十一、手册 §15）。只有调用方明确"该镜像应当带恢复分区"时
 *   才传这个位，此时缺恢复分区 = REQUIRED 未通过（rec.present）。*/
#define IMG_BOOTCHK_EXPECT_RECOVERY  1u

/* 运行完成整检查。prog 可为 NULL。等价于 img_bootcheck_ex(q,mode,0,out,prog)。*/
int img_bootcheck(img_qcow2 *q, img_boot_mode_t mode, img_bootcheck_result_t *out, img_prog *prog);
int img_bootcheck_ex(img_qcow2 *q, img_boot_mode_t mode, unsigned flags,
                     img_bootcheck_result_t *out, img_prog *prog);

/* 把结果打成 NDJSON（每项一行，方便流式输出）；返回写入字节数 */
int img_bootcheck_json(const img_bootcheck_result_t *r, char *buf, int cap);
/* ★ 打成【单个合法 JSON 数组】"[{...},{...}]"（无换行）。
 *   命令表（boot.check）必须用它：把 NDJSON 塞进结果信封的 data 会产出非法 JSON
 *   （换行在 JSON 值里非法）—— 2026-10-20 实测踩到。*/
int img_bootcheck_json_array(const img_bootcheck_result_t *r, char *buf, int cap);

/* 一行汇总："6/8 必需项通过 · 2 项警告" */
int img_bootcheck_summary(const img_bootcheck_result_t *r, char *buf, int cap);

#ifdef __cplusplus
}
#endif
#endif
