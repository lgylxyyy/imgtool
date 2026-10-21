/* bootplan_test — boot.plan 的判定规则（纯逻辑，不需要盘）
 *
 * 这些用例钉的都是"判错就会坑用户"的地方：
 *   · Vista 的 build 是 6000，比 Win7 的 7600 小 —— 拿 build 号当门槛会把
 *     Vista 误判成"不支持"（我第一版就是这么错的，实测抓到）
 *   · 32 位 Windows + UEFI 是【做不到】，必须拒（不是"建议别用"）
 *   · Vista/7 的 x64 + UEFI 是【能做到但危险】，要 needs_confirm 而不是拒
 *   · Win8(9200) 起 UEFI + Secure Boot 才是正常路径
 */
#include "img_bootplan.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define OK(c, m) do{ if(c) printf("  ✅ %s\n", m); else { printf("  ❌ %s\n", m); fails++; } }while(0)

static img_boot_plan_t plan(int maj, int min, int bld, img_plan_arch arch, int gpt){
    img_boot_plan_in_t in; memset(&in, 0, sizeof in);
    img_boot_plan_t out; memset(&out, 0, sizeof out);
    in.os_major = maj; in.os_minor = min; in.os_build = bld;
    in.arch = arch; in.want_gpt = (gpt != 0);
    img_boot_plan(&in, &out);
    return out;
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("bootplan_test — 分区表/固件兼容判定\n");

    /* --- 硬事实：做不到 --- */
    OK(plan(6,1,7601,IMG_PLAN_ARCH_X86,1).verdict == IMG_PLAN_DENIED, "Win7 x86 + GPT → 拒（32 位没有 UEFI 启动路径）");
    OK(!strcmp(plan(6,1,7601,IMG_PLAN_ARCH_X86,1).reason, "BOOT-ARCH-NO-UEFI"), "  原因码 BOOT-ARCH-NO-UEFI");
    OK(plan(6,0,6000,IMG_PLAN_ARCH_X86,1).verdict == IMG_PLAN_DENIED, "Vista x86 + GPT → 拒");
    OK(plan(10,0,26100,IMG_PLAN_ARCH_ARM64,0).verdict == IMG_PLAN_DENIED, "ARM64 + MBR/BIOS → 拒（只能 UEFI）");
    OK(plan(5,1,2600,IMG_PLAN_ARCH_X86,0).verdict == IMG_PLAN_DENIED, "XP(5.1) → 拒（NTLDR，没有 BCD）");
    OK(!strcmp(plan(5,1,2600,IMG_PLAN_ARCH_X86,0).reason, "BOOT-OS-NO-BCD"), "  原因码 BOOT-OS-NO-BCD");

    /* --- ★ 我踩过的坑：Vista 的 build 6000 < Win7 的 7600 --- */
    OK(plan(6,0,6000,IMG_PLAN_ARCH_X86,0).verdict == IMG_PLAN_YES, "★ Vista x86 + MBR → 放行（别拿 build<7600 当 XP 判据）");
    OK(plan(6,0,6000,IMG_PLAN_ARCH_X64,0).verdict == IMG_PLAN_YES, "★ Vista x64 + MBR → 放行");

    /* --- 能做到但危险：要确认 --- */
    { img_boot_plan_t p = plan(6,1,7601,IMG_PLAN_ARCH_X64,1);
      OK(p.verdict == IMG_PLAN_NEEDS_CONFIRM, "Win7 x64 + GPT → needs_confirm（实测能起，SB 会被拒）");
      OK(p.secure_boot == 2, "  并告知 Secure Boot 不可用");
      OK(!strcmp(p.reason, "BOOT-GPT-LEGACY-NEEDS-CONFIRM"), "  原因码 BOOT-GPT-LEGACY-NEEDS-CONFIRM"); }
    OK(plan(6,0,6000,IMG_PLAN_ARCH_X64,1).verdict == IMG_PLAN_NEEDS_CONFIRM, "Vista x64 + GPT → needs_confirm（未实测，更要问用户）");

    /* --- 正常路径 --- */
    { img_boot_plan_t p = plan(6,3,9600,IMG_PLAN_ARCH_X64,1);
      OK(p.verdict == IMG_PLAN_YES && p.secure_boot == 1, "Win8.1 x64 + GPT → 放行且 Secure Boot 可用"); }
    { img_boot_plan_t p = plan(10,0,26100,IMG_PLAN_ARCH_X64,1);
      OK(p.verdict == IMG_PLAN_YES && p.recommend_gpt, "Win11 x64 + GPT → 放行且推荐 GPT"); }
    OK(plan(6,2,9200,IMG_PLAN_ARCH_X86,0).verdict == IMG_PLAN_YES, "Win8 x86 + MBR → 放行");
    OK(plan(6,2,9200,IMG_PLAN_ARCH_X86,1).verdict == IMG_PLAN_DENIED, "Win8 x86 + GPT → 仍拒（32 位就是不行）");

    /* --- 信息不足：不拦 --- */
    OK(plan(0,0,0,IMG_PLAN_ARCH_UNKNOWN,1).verdict == IMG_PLAN_UNKNOWN, "读不出版本 → 不判定、不拦（新盘还没灌系统）");

    /* --- 名字/工具函数 --- */
    OK(!strcmp(img_boot_plan_verdict_name(IMG_PLAN_NEEDS_CONFIRM), "needs_confirm"), "verdict 名字用于 JSON");
    OK(img_boot_plan_arch_from_wim("0") == IMG_PLAN_ARCH_X86, "WIM arch 0 → x86");
    OK(img_boot_plan_arch_from_wim("9") == IMG_PLAN_ARCH_X64, "WIM arch 9 → x64");
    OK(img_boot_plan_arch_from_wim("12") == IMG_PLAN_ARCH_ARM64, "WIM arch 12 → arm64");
    { img_boot_plan_t p = plan(10,0,22631,IMG_PLAN_ARCH_X64,1);
      OK(p.os_name && strstr(p.os_name, "11") != NULL, "os_name 认得 22631 是 Windows 11"); }

    printf(fails ? "\n❌ bootplan_test 失败 %d 项\n" : "\n✅ bootplan_test 全过\n", fails);
    return fails ? 1 : 0;
}