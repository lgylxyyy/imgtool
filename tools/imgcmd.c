/* imgcmd — 命令表的统一入口（CLI 外壳）
 *
 *   imgcmd --list                     列出所有命令（含参数规格/帮助）
 *   imgcmd --help=<命令>              单个命令用法（从表自动生成）
 *   imgcmd <命令> [--参数=值 ...]      执行
 *   全局：--lang=zh|en（或 IMGTOOL_LANG） --log=路径|--log=- --log-level=... --log-max=N --ro
 *
 * ★ 语言归属（DESIGN-OUTPUT.md §八）：**库只发语义码 + 数值，文案在这个文件里**。
 *   这里的文案表是【CLI 专用】的临时渲染（中文默认 / --lang=en）。
 *   App 用同一份码 + 自己的语言表渲染 —— 库里没有中文，加语言不用改库。
 *
 * 结果：stdout 一行 JSON 信封（App/脚本解析）
 *       stderr 人话行 + 失败卡片（人看；--progress=bar 或 tty 时）
 */
#include "img_cmd.h"
#include "img_text.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* isatty / write */
#include <fcntl.h>    /* open */
#include <sys/stat.h> /* stat：判断日志到底落尚未写入介质 */

/* ================= CLI 专用文案表（中/英） ================= */
static char g_lang[8] = "zh";
static bool lang_en(void){ return g_lang[0]=='e'||g_lang[0]=='E'; }

typedef struct { const char *k; const char *zh; const char *en; } TXT;
static const TXT A_TEXT[] = {                      /* 动作名 */
    {"format","格式化","Format"}, {"put","写入","Write"}, {"save","保存","Save"},
    {"check","校验","Check"},     {"write","写入","Write"}, {"apply","灌入","Apply"},
    {"ls","列出","List"},         {"open","打开","Open"},   {"scan","扫描","Scan"},
    {"mk","创建","Create"},       {"resize","调整大小","Resize"},
    {"convert","转换","Convert"},{"export","导出","Export"},
    {"cancel","取消","Cancel"},   {"reserve","预留","Reserve"},
    {NULL,NULL,NULL}
};
typedef struct { const char *k,*czh,*cen,*szh,*sen; } ETX;
static const ETX E_TEXT[] = {                      /* 错误码 → 原因 / 建议 */
    {"FAT-ENOSPC","目标分区空间不足","Not enough space in the target partition",
                 "换更小的文件，或先扩容该分区（imgresize）","Use a smaller file, or grow the partition first (imgresize)"},
    {"HOST-ENOSPC","宿主磁盘空间不足","Not enough free space on the host disk",
                 "清理宿主磁盘，或换更小的源文件/镜像","Free up host disk space, or use a smaller source/image"},
    {"FAT-MOUNT-FAIL","目标 LBA 上不是合法 FAT32（未格式化或 LBA 不对）","Not a valid FAT32 at this LBA (not formatted, or wrong LBA)",
                 "先用 imgmkfs 格式化该分区，或核对 --lba","Format the partition with imgmkfs, or check the LBA"},
    {"FAT-PUT-FAIL","写入 FAT32 失败","Failed to write into FAT32",
                 "看日志里的失败行（含偏移与错误码）","See the fail line in the log (offset + code)"},
    {"FAT-LS-FAIL","列目录失败","Failed to list directory",
                 "镜像可能损坏，用 imgq2test 校验","Image may be corrupt; verify with imgq2test"},
    {"FAT-DIR-NOTFOUND","目录不存在","Directory does not exist",
                 "核对路径（FAT 不区分大小写，但要写全名）","Check the path (case-insensitive, but needs the full name)"},
    {"FAT-ENTRY-NOTFOUND","要操作的名字不在目录里（可能已被移动或拼写有误）","The entry does not exist (moved already, or a typo)",
                 "先用 ntfs.list 查看目录里有什么","List the directory first (ntfs.list) to see what's there"},
    {"FAT-RM-FAIL","删除失败","Failed to delete",
                 "只允许删空目录；非空请先删掉里面的文件","Only empty directories can be removed"},
    {"SRC-NOTFOUND","源文件无法打开（不存在或无权限）","Cannot open the source file",
                 "检查路径与权限","Check the path and permissions"},
    {"IMG-OPEN-FAIL","镜像无法打开","Cannot open the image",
                 "检查路径/权限，确认文件存在","Check path/permission, make sure the file exists"},
    {"QC-OPEN-FAIL","不是合法的 qcow2（魔数或版本不符）","Not a valid qcow2",
                 "确认是本工具创建的镜像；或先用 imgq2test 校验","Created by this tool? Or verify with imgq2test"},
    {"PART-READ-FAIL","分区表读取失败","Partition table read failed",
                 "镜像可能损坏，用 imglayout check 复核","Image may be corrupt; re-check with imglayout"},
    {"CMD-NOTFOUND","没有这个命令","Unknown command",
                 "用 --list 查看可用命令","Use --list to see available commands"},
    {"CMD-ARG-BAD","参数不合法","Invalid argument",
                 "检查参数名/类型；用 --help=<命令> 看用法","Check names/types; see --help=<command>"},
    {"CMD-FAILED","命令执行失败","Command failed",
                 "看日志里的失败行","See the fail line in the log"},
    {"WIM-NOTFOUND","WIM/ESD 无法打开","Cannot open the WIM/ESD",
                 "先用 imgwimls 列出索引","List indexes first with imgwimls"},
    {"WIM-IDX-INVALID","镜像索引不存在","Image index does not exist",
                 "先用 imgwimls 列出索引，再带 --index=N","List indexes first, then pass --index=N"},
    {"NTFS-FORMAT-FAIL","NTFS 格式化失败（灌 WIM 前的准备步骤）","NTFS format failed (the step before applying)",
                 "换更小的分区或用 imgntfsfs format 单独重试；看日志里的失败行","Use a smaller partition or retry imgntfsfs format; see the fail line"},
    {"WIM-APPLY-FAIL","灌入 WIM 失败（格式化成功，但这步没完成）","Applying the WIM failed (format succeeded, this step didn't)",
                 "看日志里的失败行与 error_code；镜像文件可用 qemu-img check 复核","See the fail line + error_code; verify the image with qemu-img check"},
    {"CLONE-COPY-FAIL","克隆拷贝失败（已回滚：旧镜像恢复原名）","Clone copy failed (rolled back; the old image was renamed back)",
                 "检查旧镜像与宿主磁盘（qemu-img check / 空间 / 权限）","Check the old image and host disk (qemu-img check / space / permission)"},
    {"CLONE-VERIFY-FAIL","克隆后校验不通过（已回滚：旧镜像恢复原名）","Clone verification failed (rolled back; the old image was renamed back)",
                 "旧镜像可能损坏；先单独检查它再重试","The old image may be corrupt; check it first, then retry"},
    {"IO-TIMEOUT","磁盘 I/O 无进展超时（连续一段时间没有任何读写完成）","Disk I/O stalled: nothing completed for the timeout window",
                 "检查介质/线缆/权限；慢介质可调大 --io-timeout-ms（0=关闭）","Check the media/cable/permission; raise --io-timeout-ms for slow media (0=off)"},
    {"FEATURE-UNAVAILABLE","这个功能在这个构建里不可用（缺一个系统库）","This feature is not available in this build (missing a system library)",
                 "重新编译时开启对应开关并安装库（见 --list 的 features 字段）","Rebuild with the feature switch on and the library installed (see features in --list)"},
    {"CMD-CANCELLED","操作已取消（用户点了停止）","Operation cancelled by the user",
                 "目标分区/文件可能停在半写状态，请重做该步骤","The target may be half-written; redo this step"},
    /* ↓ 2026-10-20 补齐：这些码由命令表返回，之前 CLI 卡片只会显示"操作失败/重试" */
    {"HOST-OPEN-FAIL","宿主文件无法打开（不存在或无权限）","Cannot open the host file (missing or no permission)",
                 "检查路径与权限；Android 上 assets 里的参考件要先解到 cacheDir","Check the path/permission; extract asset references to cacheDir first"},
    {"IMG-FORMAT","镜像格式不对或已损坏（能打开但无法读取/写不了）","Invalid or corrupt image (opens but won't read/write)",
                 "用 qemu-img check / imgq2test 校验镜像，必要时换备份","Verify with qemu-img check / imgq2test; fall back to a backup"},
    {"PART-WRITE-FAIL","分区表写入失败","Failed to write the partition table",
                 "确认镜像可写（缺少 --ro）、宿主空间足够、文件没被占用","Make sure the image is writable (no --ro), has free space, and isn't in use"},
    {"PART-FS-BAD","目标分区的文件系统不对（未格式化或不是预期类型）","The target partition has the wrong filesystem",
                 "先格式化该分区（NTFS 用 imgntfsfs format / FAT32 用 imgmkfs），再重试","Format the partition first (imgntfsfs format / imgmkfs), then retry"},
    {"PART-JSON-FAIL","分区表序列化失败（分区太多装不下，内部错误）","Failed to render the partition table JSON (too many entries; internal error)",
                 "用 imglayout check 看分区表，并把日志一并发回来","Use imglayout check to inspect the table and report it with the log"},
    {"PART-COPY-FAIL","分区拷贝失败","Partition copy failed",
                 "核对 --src-lba/--dst-lba 与两侧分区大小，检查宿主空间","Check --src-lba/--dst-lba and both partition sizes; check host free space"},
    {"BCD-REF-BAD","参考 BCD 不是所需的那一种（BIOS/UEFI 混用）或已损坏","The reference BCD is the wrong kind (BIOS vs UEFI) or is corrupt",
                 "BIOS 用 bcdboot 生成的 BIOS BCD；UEFI 用 ESP 上那份；别互相顶","Use a BIOS BCD for boot.repair and the ESP one for UEFI; don't substitute"},
    {"BCD-PATCH-FAIL","改写 BCD 里的设备对象失败","Failed to patch device objects in the BCD",
                 "确认参考件完整未损坏（reference/bcd-reference-bios-win11.bin），并把日志发回来","Make sure the reference is intact (reference/bcd-reference-bios-win11.bin) and report the log"},
    {"BCD-WRITE-FAIL","写入 \\Boot\\BCD 失败","Failed to write \\Boot\\BCD",
                 "确认系统保留分区是 NTFS 且有空间","Make sure the system reserved partition is NTFS with free space"},
    {"BOOT-FORMAT-UNSUPPORTED","该 Windows 版本不支持使用此分区表启动","This Windows version does not support booting from the requested partition table",
                 "32 位 Windows 没有 UEFI 启动路径；改用 MBR/BIOS（img.create --table=mbr）","32-bit Windows has no UEFI boot path; use MBR/BIOS (img.create --table=mbr)"},
    {"BOOT-FORMAT-NEEDS-CONFIRM","技术上能启动，但风险要用户确认（老系统的 UEFI/GPT）","Technically bootable, but the risk needs the user's confirmation (legacy OS on UEFI/GPT)",
                 "弹框让用户确认；确认后带 --force=true 再调一次","Ask the user to confirm, then retry with --force=true"},
    {"BOOT-VBR-WRITE-FAIL","写入 VBR 引导码失败","Failed to write the VBR boot code",
                 "核对 --vbr-tpl=镜像:lba 与目标分区一致；目标必须是已格式化的 NTFS","Check that --vbr-tpl=image:lba matches the target; the target must be formatted NTFS"},
    {"GUID-INVALID-BAD","GUID 格式非法","Invalid GUID format",
                 "用 8-4-4-4-12 十六进制（如 1a233c35-12f4-6047-9d59-eafe3bb17577）","Use an 8-4-4-4-12 hex GUID (e.g. 1a233c35-12f4-6047-9d59-eafe3bb17577)"},
    {"LIST-FAIL","列表/命令表渲染失败（内部错误）","Failed to render the listing/command table (internal error)",
                 "重试；仍然失败请把日志发回来","Retry; if it persists, report it with the log"},
    {"WINRE-NOTFOUND","这个 WIM/ESD 里没有 Winre.wim","No Winre.wim inside this WIM/ESD",
                 "换一个包含 WinRE 的 ISO/ESD/WIM（精简版常把 WinRE 删掉）","Use an ISO/ESD/WIM that contains WinRE (slimmed builds remove it)"},
    {"REC-VERIFY-FAIL","恢复分区填充失败","Filling the recovery partition failed",
                 "确认分区已格式化、空间够；看日志的 at/error_code","Make sure it's formatted with enough space; check at/error_code in the log"},
    {NULL,NULL,NULL,NULL,NULL}
};
static const char *txt_act(const char *a){        /* 动作名 */
    if(a) for(int i=0;A_TEXT[i].k;i++) if(!strcmp(a,A_TEXT[i].k)) return lang_en()?A_TEXT[i].en:A_TEXT[i].zh;
    return a?a:"";
}
static const char *txt_cause(const char *code){
    if(code) for(int i=0;E_TEXT[i].k;i++) if(!strcmp(code,E_TEXT[i].k)) return lang_en()?E_TEXT[i].cen:E_TEXT[i].czh;
    return lang_en()?"Operation failed":"操作失败";
}
static const char *txt_suggest(const char *code){
    if(code) for(int i=0;E_TEXT[i].k;i++) if(!strcmp(code,E_TEXT[i].k)) return lang_en()?E_TEXT[i].sen:E_TEXT[i].szh;
    return lang_en()?"Retry; if reproducible, send this report with the log"
                    :"重试；若复现请把本段与日志文件一起反馈";
}

/* ---------- 事件字段小读取器（库发码，这里出文案） ---------- */
static const char *ev_str_fld(const img_ev *e,const char *k){
    for(int i=0;i<e->nf;i++) if(!strcmp(e->f[i].k,k)&&e->f[i].kind==1) return e->f[i].s;
    return NULL;
}
static long long ev_num_fld(const img_ev *e,const char *k,long long def){
    for(int i=0;i<e->nf;i++) if(!strcmp(e->f[i].k,k)&&e->f[i].kind==2) return e->f[i].n;
    return def;
}
typedef struct { bool human; } ui_t;
static void wr_all(int fd, const char *b, size_t n){   /* 忽略写失败（卡片/人话行丢了也不能拖垮命令本身）*/
    size_t off=0;
    while(off<n){ ssize_t w=write(fd,b+off,n-off); if(w<=0) return; off+=(size_t)w; }
}

/* ================= 人话行（Linux 风格） ================= */
static void on_event(const img_ev *e, void *ud){
    ui_t *ui=(ui_t*)ud;
    if(!ui||!ui->human) return;
    if(e->lv>IMG_LV_INFO) return;                 /* 屏幕只显示 info 及以上 */
    const char *act=txt_act(ev_str_fld(e,"action"));
    char b[512]; int n=0;
    if(!strcmp(e->ev,"begin")){
        n=snprintf(b,sizeof b,"%s %s", lang_en()?"->":"▶", act);
        const char *p=ev_str_fld(e,"path");
        if(p) n+=snprintf(b+n,sizeof b-(size_t)n,"  %s",p);
        long long sz=ev_num_fld(e,"size_bytes",-1);
        if(sz>=0) n+=snprintf(b+n,sizeof b-(size_t)n, lang_en()?"  %lld bytes":"  %lld 字节",sz);
        long long lb=ev_num_fld(e,"lba",-1);
        if(lb>=0) n+=snprintf(b+n,sizeof b-(size_t)n,"  LBA %lld",lb);
    } else if(!strcmp(e->ev,"done")){
        long long ms=ev_num_fld(e,"duration_ms",-1);
        if(e->rc) n=snprintf(b,sizeof b,"%s %s %s (%s)", lang_en()?"x":"✖", act,
                             lang_en()?"failed":"失败", img_strerror(e->rc));
        else      n=snprintf(b,sizeof b,"%s %s %s", lang_en()?"ok":"✔", act,
                             lang_en()?"done":"完成");
        if(ms>=0) n+=snprintf(b+n,sizeof b-(size_t)n," (%lld ms)",ms);
    } else if(!strcmp(e->ev,"cancel")){
        long long ms=ev_num_fld(e,"duration_ms",-1);
        n=snprintf(b,sizeof b,"%s %s %s", lang_en()?"stop":"⏹", act,
                   lang_en()?"cancelled":"已取消");
        if(ms>=0) n+=snprintf(b+n,sizeof b-(size_t)n," (%lld ms)",ms);
    } else if(!strcmp(e->ev,"alive")){
        if(ev_num_fld(e,"stuck_ms",-1)>=0) return;   /* 疑似无响应：不刷屏 */
        n=snprintf(b,sizeof b,"%s %s %s (iterations=%llu)",
                   lang_en()?"...":"…", lang_en()?"still running":"仍在进行", act,
                   (unsigned long long)ev_num_fld(e,"iterations",0));
    } else return;
    if(n>0 && n<(int)sizeof b-1) b[n++]='\n';
    wr_all(2,b,(size_t)n);                            /* stderr */
}

/* ================= 失败卡片（CLI 渲染；App 用自己的表） ================= */
static void render_card(const img_error_info_t *ei, bool to_screen){
    bool en=lang_en();
    char card[8192]; int n=0;
    n+=snprintf(card+n,sizeof card-(size_t)n,"\n%s\n",
        en?"===== Icdqcow2 error report (copy the whole block) ====="
          :"════════ Icdqcow2 错误报告（整段复制即可）════════");
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s / %s / %s\n",
        en?"Tool":"工具  ", IMGTOOL_VERSION, img_platform(), img_abi());
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Command":"命令  ", ei->ctx[0]?ei->ctx:"?");
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Image":"镜像  ", ei->img[0]?ei->img:"?");
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Where":"失败位置", ei->at[0]?ei->at:"-");
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s (rc=%d)\n", en?"Error":"错误码",
                ei->code[0]?ei->code:"?", ei->rc);
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Cause":"原因    ", txt_cause(ei->code));
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Suggest":"建议    ", txt_suggest(ei->code));
    if(!en && ei->status[0]) n+=snprintf(card+n,sizeof card-(size_t)n,"当前状态: %s\n", ei->status);
    if(ei->log[0]){
        /* ★ 卡片是"用户拿去反馈"的东西，路径必须是事实：只印真正打开着的日志，
         *   并且确认它确实有内容（没开 --log 时磁盘上根本没有这个文件）。*/
        struct stat st;
        bool has = (stat(ei->log,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0);
        n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s%s\n", en?"Log":"日志  ", ei->log,
            has ? "" : (en?"  (NOT written - rerun with --log)"
                           :"  （未落盘：重跑时加 --log 才有）"));
    } else {
        n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Log":"日志  ",
            en?"not enabled (rerun with --log to capture one)"
               :"未开启（重跑时加 --log 才会写日志）");
    }
    n+=snprintf(card+n,sizeof card-(size_t)n,
        en?"--- event tail (%d bytes, machine format) ---\n":"──────── 事件尾部（内存最近 %d 字节，机器格式）────────\n",
        ei->events_len);
    /* 事件尾部：直接取快照里的同源机器格式行 */
    size_t used=strlen(ei->events);
    size_t room=(sizeof card - (size_t)n - 64);
    if(used>room) used=room;
    memcpy(card+n,ei->events,used); n+=(int)used;
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s\n",
        en?"===== end of error report =====":"════════ 错误报告结束 ════════");
    if(n<0) n=0;
    if(n>(int)sizeof card) n=(int)sizeof card;
    /* ★ 屏幕与文件分开控制：--progress=none/json 时不刷屏，但**仍然落 .error.txt**
     *   —— 用户要拿它反馈，跟终端是人是机器无关。*/
    if(to_screen) wr_all(2,card,(size_t)n);
    if(ei->img[0]){                                  /* 落一份：用户直接发这个 */
        char path[600]; snprintf(path,sizeof path,"%s.error.txt",ei->img);
        int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd>=0){ wr_all(fd,card,(size_t)n); close(fd); }
    }
}

/* ================= 进度（P2）：--progress=bar 时的百分比行 =================
 * 库内部只在 phase/op/item 变化或跨过 200 ms 节流时回调，不会刷屏。
 * 只在 human 模式下渲染；--progress=none/json 时完全不注册（零开销）。*/
static void on_progress(void *ud, const img_progress_t *p){
    (void)ud;
    if(!p) return;
    const char *ph=img_phase_name(p->phase), *op=img_op_name(p->op);
    char line[512]; int n=0;
    if(p->pct>=0){
        n=snprintf(line,sizeof line,"\r▶ %s·%s  %3d%%", ph?ph:"", op?op:"", p->pct);
        if(p->total_bytes)
            n+=snprintf(line+n,sizeof line-(size_t)n,"  %llu/%llu MB",
                (unsigned long long)(p->cur_bytes/1048576),(unsigned long long)(p->total_bytes/1048576));
        else if(p->total)
            n+=snprintf(line+n,sizeof line-(size_t)n,"  %llu/%llu",(unsigned long long)p->cur,(unsigned long long)p->total);
        if(p->eta_s>=0) n+=snprintf(line+n,sizeof line-(size_t)n,"  剩余 %ds",p->eta_s);
    } else {
        n=snprintf(line,sizeof line,"\r▶ %s·%s  %s", ph?ph:"", op?op:"", p->item?p->item:"");
    }
    if(p->item && p->pct>=0) n+=snprintf(line+n,sizeof line-(size_t)n,"  %s", p->item);
    if(n>0) wr_all(2,line,(size_t)n);
    if(p->pct>=100) wr_all(2,"\n",1);
}

/* ================= 用法帮助 ================= */
static void print_help(const img_cmd *c){
    if(!c){ printf("没有这个命令\n"); return; }
    printf("%s  (since %s)\n  %s\n", c->name, c->since?c->since:"", c->help?c->help:"");
    if(c->nargs){
        printf("  %s：\n", lang_en()?"Arguments":"参数");
        for(int i=0;i<c->nargs;i++){
            const img_arg *a=&c->args[i];
            const char *ty=lang_en()?"string":"字符串";
            switch(a->ty){ case IMG_A_U64: ty=lang_en()?"number":"数字"; break;
                           case IMG_A_I64: ty=lang_en()?"integer":"整数"; break;
                           case IMG_A_BOOL:ty=lang_en()?"boolean":"布尔"; break;
                           case IMG_A_ENUM: ty=lang_en()?"enum":"枚举"; break; default: break; }
            printf("    --%-12s %-6s %s%s%s\n", a->name, ty,
                   a->required?(lang_en()?"required ":"必填  "):(lang_en()?"optional ":"可选  "),
                   a->def?a->def:"", a->help? a->help :"");
        }
    }
}

int main(int argc, char **argv){
    const char *cmd=NULL,*log=NULL,*lvl=NULL,*helpcmd=NULL,*logmax=NULL,*progress=NULL,*iot=NULL;
    bool list=false, ro=false;
    ui_t ui={false};
    img_args *args=img_args_new();
    if(!args) return 1;

    { const char *el=getenv("IMGTOOL_LANG"); if(el&&*el){ snprintf(g_lang,sizeof g_lang,"%s",el); img_lang_set(el); } }

    for(int i=1;i<argc;i++){
        const char *a=argv[i];
        if(a[0]!='-'){ if(!cmd){ cmd=a; continue; }
            fprintf(stderr,"多余的位置参数：%s\n",a); img_args_free(args); return 2; }
        const char *eq=strchr(a,'=');
        char key[64]; const char *val=eq?eq+1:"";
        if(eq){ size_t n=(size_t)(eq-a-2); if(n>=sizeof key) n=sizeof key-1; memcpy(key,a+2,n); key[n]=0; }
        else   snprintf(key,sizeof key,"%s",a+2);

        if(!strcmp(key,"list")){ list=true; continue; }
        if(!strcmp(key,"help")){ helpcmd=eq?val:NULL; if(!eq) list=true; continue; }
        if(!strcmp(key,"ro")){ ro=true; continue; }
        if(!strcmp(key,"log")){ log=val; continue; }
        if(!strcmp(key,"log-level")){ lvl=val; continue; }
        if(!strcmp(key,"log-max")){ logmax=val; continue; }
        if(!strcmp(key,"io-timeout-ms")){ iot=val; continue; }
        if(!strcmp(key,"progress")){ progress=val; continue; }
        if(!strcmp(key,"lang")){ snprintf(g_lang,sizeof g_lang,"%s",val);
            img_lang_set(val);   /* ★ 库里的 note/summary/检查项名字跟着换语言 */
            continue; }
        if(!eq){
            /* ★ 布尔参数允许裸写：--recursive 等价于 --recursive=true。
             *   用户和 App 都会自然这么写（实测：--recursive 报"缺少 =值"，
             *   第一反应是"参数名写错了"，而不是"要补 =1"）。*/
            bool isbool=false;
            const img_cmd *cc = cmd ? img_cmd_find(cmd) : NULL;
            for(int k=0; cc && k<cc->nargs; k++)
                if(!strcmp(cc->args[k].name,key) && cc->args[k].ty==IMG_A_BOOL){ isbool=true; break; }
            if(!isbool){ fprintf(stderr,"参数 %s 缺少 =值（例如 --lba=2048）\n",a); img_args_free(args); return 2; }
            val="true";
        }
        img_args_set(args,key,val);
    }

    if(list){ static char buf[96*1024]; if(img_cmd_list_json(buf,sizeof buf)>0) fputs(buf,stdout);
              img_args_free(args); return 0; }
    if(helpcmd){ print_help(img_cmd_find(helpcmd)); img_args_free(args); return 0; }   /* 先于"缺命令名"判定 */
    if(!cmd){ fprintf(stderr,"用法: imgcmd <命令> [--参数=值 ...] | --list | --help=<命令>\n");
              img_args_free(args); return 2; }

    {   /* 卡片里的"命令"一行：命令名 + 参数（没开日志也要有） */
        char cl[512]; int k=snprintf(cl,sizeof cl,"%s",cmd?cmd:"?");
        for(int i=0;i<img_args_count(args) && k<(int)sizeof cl-64;i++){
            /* 简单起见只带前 6 个参数 */
        }
        img_card_set_context(cl);
    }
    /* I/O 看门狗（无进展超时）：默认 30s；--io-timeout-ms=0 关闭；env IMGTOOL_IO_TIMEOUT_MS 优先级低于参数 */
    { int tmo=30000;
      if(!iot){ const char *e=getenv("IMGTOOL_IO_TIMEOUT_MS"); if(e&&*e) tmo=atoi(e); }
      else tmo=atoi(iot);
      img_io_watchdog_start(tmo);
      atexit(img_io_watchdog_stop); }
    /* 屏幕形态：tty → 人话行；非 tty → JSON；--progress=bar|json|none 强制 */
    ui.human = progress ? (!strcmp(progress,"bar")) : (isatty(2)!=0);
    if(progress && !strcmp(progress,"none")) ui.human=false;
    /* ★ 只有人话模式才注册进度回调：其余两种模式保持"只出一行信封"的旧行为 */
    if(ui.human) img_cmd_set_progress(on_progress, &ui);
    img_out_open_fd(2, ui.human);      /* human=true → 库不再往 stderr 吐机器行 */
    img_out_set_text(on_event, &ui);   /* ★ 库发码 → 这里出中文/英文人话行 */

    if(!lvl) lvl=getenv("IMGTOOL_LOG_LEVEL");      /* 环境变量同样适用 */
    if(lvl){
        img_lv_t l=IMG_LV_INFO;
        if(!strcmp(lvl,"error")) l=IMG_LV_ERROR; else if(!strcmp(lvl,"warn")) l=IMG_LV_WARN;
        else if(!strcmp(lvl,"debug")) l=IMG_LV_DEBUG; else if(!strcmp(lvl,"trace")) l=IMG_LV_TRACE;
        img_log_set_level(l);
    }
    if(!log){                                       /* ★ 环境变量 IMGTOOL_LOG（shell 流水线一行 export 全生效）*/
        const char *el=getenv("IMGTOOL_LOG"); if(el&&*el) log=el;
    }
    if(log){
        size_t mb=4u*1024*1024;
        if(logmax){ uint64_t v=strtoull(logmax,NULL,0); if(v>0) mb=(size_t)v; }
        int rc = !strcmp(log,"-") ? img_log_open_fd(2,img_log_level(),mb,1)
                                  : img_log_open(log,img_log_level(),mb,1);
        if(rc!=IMG_OK) fprintf(stderr,"警告：日志无法打开（%s），继续执行\n",log);
        else img_log_session("imgcmd", cmd, img_args_get(args,"img"), 0);   /* 会话头（工具/命令/镜像）*/
    }

    const char *img=img_args_get(args,"img");
    char err[256]="";
    img_ctx *ctx=NULL;
    if(img){
        ctx=img_ctx_open(img, !ro, img_args_bool(args,"check_size",false), err, sizeof err);
        if(!ctx){
            img_result r; img_result_begin(&r,cmd);
            img_result_fail(&r,IMG_ENOENT,"IMG-OPEN-FAIL",img,err,"检查路径/权限，确认文件存在");
            char b[1024]; int n=img_result_json(&r,b,sizeof b);
            if(n>0) fwrite(b,1,(size_t)n,stdout);
            img_error_info_t ei; img_error_info(&ei);
            if(ei.code[0]) render_card(&ei, ui.human);
            img_args_free(args); return 1;
        }
    }

    img_result res;
    int rc=img_cmd_exec(cmd, ctx, args, &res);
    static char out[128*1024];
    int n=img_result_json(&res,out,sizeof out);
    if(n>0) fwrite(out,1,(size_t)n,stdout);
    if(rc!=IMG_OK){
        img_error_info_t ei; img_error_info(&ei);
        if(ei.code[0]) render_card(&ei, ui.human);
    }
    if(ctx) img_ctx_close(ctx);
    img_log_close();
    img_args_free(args);
    return rc==IMG_OK?0:1;
}
