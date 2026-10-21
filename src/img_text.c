/* img_text.c — 见 img_text.h 的边界说明。中文原文即 key。 */
#include "img_text.h"
#include <stdlib.h>
#include <string.h>

static int g_en = 0;
static int g_inited = 0;

void img_lang_set(const char *code){
    if(!code){
        const char *e = getenv("IMGTOOL_LANG");     /* 与 CLI --lang 同一个变量名 */
        if(!e || !*e) e = getenv("IMG_LANG");
        code = (e && *e) ? e : "zh";
    }
    g_en = (code[0]=='e' || code[0]=='E') ? 1 : 0;
    g_inited = 1;
}
int img_lang_en(void){
    if(!g_inited) img_lang_set(NULL);
    return g_en;
}

/* zh → en。只收录【会进 JSON 信封 / 日志】的人话；注释和 CLI 专属文案不在这里。 */
static const char *const T[][2] = {
    /* ---- 命令表 note 字段 ---- */
    {"已置取消标志；正在跑的操作会在下一个检查点退出",
     "Cancellation requested; the running operation stops at the next checkpoint"},
    {"BCD 只解决『引导配置』；bootmgr/bootmgfw.efi 与 MBR 引导码仍需 boot.repair 补",
     "BCD only covers the boot configuration; bootmgr/bootmgfw.efi and the MBR boot code still need boot.repair"},
    {"镜像里有别名/越界簇：能读，但别往里写（会串数据）。"
     "重建或用 qemu-img check 看细节；我们写盘前已自动抬高分配水位。",
     "The image has aliased/out-of-range clusters: readable, but do not write (data would overlap). "
     "Rebuild it or run qemu-img check; the allocator watermark was already raised before writing."},
    {"多个证据源互相矛盾 —— 这个镜像被改过（精简/换版本），别按文件名判断",
     "Evidence sources disagree — this image was modified (stripped/relabelled); do not trust its file name"},
    {"只有 1 个证据源可用（核心文件疑似被 WOF/CompactOS 压缩），结论可信度低",
     "Only one evidence source was readable (core files likely WOF/CompactOS-compressed); low confidence"},

    /* ---- boot.plan 的 detail（整句一个 key，%s 填系统名） ---- */
    {"没读到系统版本，不做判定（不拦你，但请自己确认）",
     "Windows version unknown; no judgement made (nothing is blocked - please verify yourself)"},
    {"%s 用 NTLDR 引导，没有 BCD/bootmgr，本工具不支持",
     "%s boots via NTLDR; there is no BCD/bootmgr - this tool does not support it"},
    {"32 位 %s 没有 UEFI 启动路径（固件位数必须匹配），只能用 MBR/BIOS",
     "32-bit %s has no UEFI boot path (firmware bitness must match); use MBR/BIOS only"},
    {"ARM64 Windows 只能 UEFI/GPT 启动，不能用 MBR/BIOS",
     "ARM64 Windows boots only via UEFI/GPT, not MBR/BIOS"},
    {"%s 的 UEFI/GPT 启动微软支持有限：实测能起，但主板开 Secure Boot 会被固件验签拒绝；老机器固件也可能不认。确认要继续吗？",
     "%s on UEFI/GPT boots in our tests, but firmware with Secure Boot on rejects it (signature check), "
     "and older firmware may not accept it. Continue anyway?"},
    {"%s 的 UEFI/GPT 启动可用","%s boots via UEFI/GPT"},
    {"%s 的 BIOS/MBR 启动可用","%s boots via BIOS/MBR"},

    /* ---- boot.check：检查项名字 ---- */
    {"读分区表","read the partition table"},
    {"引导模式","boot mode"},
    {"GPT 结构有效","GPT structure valid"},
    {"存在 ESP (FAT32)","ESP (FAT32) present"},
    {"有活动分区标记","an active partition is flagged"},
    {"存在 NTFS 分区(类型0x07)","an NTFS partition (type 0x07) exists"},
    {"NTFS $Boot 扇区","NTFS $Boot sector"},
    {"NTFS $Boot 扇区 0..9 有引导器代码","NTFS $Boot sectors 0..9 carry bootstrap code"},
    {"系统文件 winload.efi","system file winload.efi"},
    {"ESP 可被 FAT32 解析","ESP parses as FAT32"},
    {"bootmgr 在活动分区","bootmgr present on the active partition"},
    {"存在恢复分区","a recovery partition exists"},
    {"winre.wim 存在","winre.wim present"},
    {"ReAgent.xml 存在","ReAgent.xml present"},
    {"boot.sdi 存在","boot.sdi present"},
    {"引导链完整","boot chain complete"},
    {"MBR 结构有效","MBR structure valid"},
    {"MBR 分区项 CHS 合法性(Windows:扇区号1-based)","MBR CHS sanity (Windows: sector numbers are 1-based)"},
    {"ESP 类型 GUID c12a7328","ESP type GUID c12a7328"},
    {"恢复分区类型 GUID de94bba4","recovery partition type GUID de94bba4"},
    {"GPT 属性 0x8000000000000001","GPT attribute 0x8000000000000001"},
    {"系统文件 \\Windows\\system32\\winload","system file \\Windows\\system32\\winload"},
    {"★ 有必需项未通过 — 很可能无法启动","★ required checks failed — most likely not bootable"},

    /* ---- 其它常见短语 ---- */
    {"镜像路径（qcow2）","image path (qcow2)"},
    {"分区不存在","partition not found"},
    {"不是合法 NTFS","not a valid NTFS volume"},
    {"读不出版本","cannot read the Windows version"},
    {NULL,NULL}
};

const char *img_t(const char *zh){
    if(!zh || !*zh) return zh ? zh : "";
    if(!img_lang_en()) return zh;
    for(int i=0; T[i][0]; i++) if(!strcmp(T[i][0], zh)) return T[i][1];
    return zh;                       /* 没翻译：原样返回（不崩、不丢字段） */
}
