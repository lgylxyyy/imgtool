/* img_bootplan.h — “这个系统能不能用这种分区表/固件启动”的判定（纯逻辑，零文案依赖）
 *
 * 为什么单独一个文件：
 *   这是【事实 + 微软的准入规则】，不是 App 的界面政策。
 *   工具必须自己拦住“做不到”的组合（否则用户以为成功了），
 *   但“能做到却不推荐”的组合不能替用户做主 —— 只回一个机器可读的
 *   NEEDS_CONFIRM，让 App 去弹框；用户确认后 App 再带 force 调一次。
 *
 * 规则来源（都是本项目实测/官方结论，见 app/docs 手册 §6.24/§6.26）：
 *   · 32 位 Windows（x86）没有 UEFI 启动路径：固件位数必须匹配 → 直接拒
 *   · Vista/7（6.0/6.1）x64 + GPT/UEFI：Win7 x64 实测能起，但 Secure Boot 必被拒
 *     （bootmgfw 没有现代微软签名）→ 允许但必须用户确认
 *   · Win8（6.2 / build 9200）起：BIOS+UEFI、Secure Boot 都支持 → 放行
 *   · build < 7600（XP/2003）根本没有 BCD（NTLDR 引导）→ 我们做不了 → 拒
 */
#ifndef IMG_BOOTPLAN_H
#define IMG_BOOTPLAN_H
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_PLAN_UNKNOWN = 0,     /* 信息不足，不拦 */
    IMG_PLAN_YES     = 1,     /* 可以 */
    IMG_PLAN_NEEDS_CONFIRM = 2, /* 能做但有风险，要用户确认 */
    IMG_PLAN_DENIED  = 3      /* 做不到，工具必须拒 */
} img_plan_verdict;

typedef enum { IMG_PLAN_ARCH_UNKNOWN = 0, IMG_PLAN_ARCH_X86 = 1,
               IMG_PLAN_ARCH_X64 = 2, IMG_PLAN_ARCH_ARM64 = 3 } img_plan_arch;

typedef struct {
    int  os_major, os_minor, os_build;   /* 0 = 不知道 */
    img_plan_arch arch;
    bool want_gpt;                       /* 请求/实际的分区表：true=GPT(UEFI) */
} img_boot_plan_in_t;

typedef struct {
    img_plan_verdict verdict;
    char reason[48];        /* 机器码：BOOT-ARCH-NO-UEFI / BOOT-GPT-LEGACY-NEEDS-CONFIRM ... */
    char detail[220];       /* 中文说明（App 可以不显示，走自己的文案表） */
    int  secure_boot;       /* 0=未知 1=可用 2=不可用（会被固件验签拒） */
    bool recommend_gpt;     /* 微软推荐用 GPT 的最低门槛是 Win8 */
    const char *os_name;    /* "Windows 7" / "Windows 8.1" ... 便于 App 直接显示 */
} img_boot_plan_t;

/* 判定（纯函数，不改任何状态；单测直接喂参数） */
void img_boot_plan(const img_boot_plan_in_t *in, img_boot_plan_t *out);

/* "yes" / "needs_confirm" / "denied" / "unknown" —— 给 JSON 用 */
const char *img_boot_plan_verdict_name(img_plan_verdict v);
/* WIM 的 arch 字段（"0"/"9"/"12"）→ img_plan_arch */
img_plan_arch img_boot_plan_arch_from_wim(const char *arch_text);

#ifdef __cplusplus
}
#endif
#endif
