/* img_bootplan.c — 分区表/固件与 Windows 版本的兼容判定（见头文件的规则来源） */
#include "img_bootplan.h"
#include <stdio.h>
#include <string.h>

const char *img_boot_plan_verdict_name(img_plan_verdict v){
    switch(v){
        case IMG_PLAN_YES: return "yes";
        case IMG_PLAN_NEEDS_CONFIRM: return "needs_confirm";
        case IMG_PLAN_DENIED: return "denied";
        default: return "unknown";
    }
}

img_plan_arch img_boot_plan_arch_from_wim(const char *t){
    if(!t) return IMG_PLAN_ARCH_UNKNOWN;
    if(!strcmp(t,"0")) return IMG_PLAN_ARCH_X86;
    if(!strcmp(t,"9")) return IMG_PLAN_ARCH_X64;
    if(!strcmp(t,"12")) return IMG_PLAN_ARCH_ARM64;
    return IMG_PLAN_ARCH_UNKNOWN;
}

/* 版本号 → 人话系统名（只用于 detail/os_name，App 想自己显示也可以） */
static const char *os_name_of(int major,int minor,int build){
    if(major==6 && minor==0) return "Windows Vista";
    if(major==6 && minor==1) return build>=7601 ? "Windows 7 SP1" : "Windows 7";
    if(major==6 && minor==2) return "Windows 8";
    if(major==6 && minor==3) return "Windows 8.1";
    if(major==10){
        if(build>=22000) return "Windows 11";
        return "Windows 10";
    }
    return "Windows";
}

void img_boot_plan(const img_boot_plan_in_t *in, img_boot_plan_t *out){
    if(!in || !out) return;
    memset(out, 0, sizeof *out);
    out->verdict = IMG_PLAN_UNKNOWN;
    out->secure_boot = 0;
    snprintf(out->reason, sizeof out->reason, "%s", "BOOT-PLAN-NO-OS-INFO");
    snprintf(out->detail, sizeof out->detail, "%s",
             "没读到系统版本，不做判定（不拦你，但请自己确认）");

    int b = in->os_build;
    if(b <= 0){ return; }                       /* 信息不足：不拦 */
    out->os_name = os_name_of(in->os_major, in->os_minor, b);

    /* ---- 硬事实：做不到 ---- */
    /* NT 5.x（XP=5.1 build 2600 / 2003=5.2 build 3790）用 NTLDR，没有 BCD。
     * ★ 别拿 build 号当门槛：Vista 的 build 是 6000，比 Win7 的 7600 小，
     *   用 "b<7600" 会把 Vista 误判成"不支持"（实测就错过一次）。看主版本号。 */
    if(in->os_major > 0 && in->os_major < 6){
        out->verdict = IMG_PLAN_DENIED;
        snprintf(out->reason, sizeof out->reason, "%s", "BOOT-OS-NO-BCD");
        snprintf(out->detail, sizeof out->detail,
                 "%s 用 NTLDR 引导，没有 BCD/bootmgr，本工具不支持", out->os_name);
        return;
    }
    if(in->arch == IMG_PLAN_ARCH_X86 && in->want_gpt){
        out->verdict = IMG_PLAN_DENIED;
        out->secure_boot = 2;
        snprintf(out->reason, sizeof out->reason, "%s", "BOOT-ARCH-NO-UEFI");
        snprintf(out->detail, sizeof out->detail,
                 "32 位 %s 没有 UEFI 启动路径（固件位数必须匹配），只能用 MBR/BIOS", out->os_name);
        return;
    }
    if(in->arch == IMG_PLAN_ARCH_ARM64 && !in->want_gpt){
        out->verdict = IMG_PLAN_DENIED;
        snprintf(out->reason, sizeof out->reason, "%s", "BOOT-ARCH-NO-BIOS");
        snprintf(out->detail, sizeof out->detail,
                 "ARM64 Windows 只能 UEFI/GPT 启动，不能用 MBR/BIOS");
        return;
    }

    /* ---- 能做但有风险：要用户确认 ---- */
    if(in->want_gpt && b < 9200){               /* Vista / 7 的 UEFI */
        out->verdict = IMG_PLAN_NEEDS_CONFIRM;
        out->secure_boot = 2;                   /* 实测：Win7 x64 UEFI 能起，但 SB 开就 Access Denied */
        snprintf(out->reason, sizeof out->reason, "%s", "BOOT-GPT-LEGACY-NEEDS-CONFIRM");
        snprintf(out->detail, sizeof out->detail,
                 "%s 的 UEFI/GPT 启动微软支持有限：实测能起，但主板开 Secure Boot 会被固件"
                 "验签拒绝；老机器固件也可能不认。确认要继续吗？", out->os_name);
        return;
    }

    /* ---- 可以 ---- */
    out->verdict = IMG_PLAN_YES;
    out->secure_boot = in->want_gpt ? 1 : 0;    /* BIOS 没有 Secure Boot 一说 */
    out->recommend_gpt = (b >= 9200);
    snprintf(out->reason, sizeof out->reason, "%s", in->want_gpt ? "BOOT-PLAN-OK-GPT" : "BOOT-PLAN-OK-MBR");
    snprintf(out->detail, sizeof out->detail, "%s %s 可用",
             out->os_name, in->want_gpt ? "的 UEFI/GPT 启动" : "的 BIOS/MBR 启动");
}
