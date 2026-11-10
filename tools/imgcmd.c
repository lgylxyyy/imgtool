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
 *       stderr 说明行 + 失败卡片（人看；--progress=bar 或 tty 时）
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
    {"check","校验","Check"},     {"write","写入","Write"}, {"apply","写入入","Apply"},
    {"ls","列出","List"},         {"open","打开","Open"},   {"scan","扫描","Scan"},
    {"mk","创建","Create"},       {"resize","调整大小","Resize"},
    {"convert","转换","Convert"},{"export","导出","Export"},
    {"cancel","取消","Cancel"},   {"reserve","预留","Reserve"},
    {NULL,NULL,NULL}
};
typedef struct { const char *k,*czh,*cen,*szh,*sen; } ETX;
static const ETX E_TEXT[] = {                      /* 错误码 → 原因 / 建议（专业表述，中英双语） */
    {"FAT-ENOSPC","目标 FAT32 分区可用空间不足，写入未能完成。","The target FAT32 partition has insufficient free space; the write could not be completed.",
               "请缩减待写入数据体积，或扩大该分区容量后重试。","Reduce the size of the data to be written, or enlarge the partition, then retry."},
    {"HOST-ENOSPC","宿主文件系统可用空间不足，映像无法继续写入。","The host file system has insufficient free space; the image cannot be written further.",
               "请释放宿主磁盘空间后重试。","Free host disk space, then retry."},
    {"FAT-MOUNT-FAIL","指定 LBA 处未检测到有效的 FAT32 卷（未格式化或 LBA 有误）。","No valid FAT32 volume was found at the given LBA (unformatted, or incorrect LBA).",
               "请确认分区表与 --lba 一致；必要时先执行 fs.format。","Confirm the partition table and --lba; run fs.format first if required."},
    {"FAT-PUT-FAIL","FAT32 写入过程中发生底层 I/O 错误。","A low-level I/O error occurred while writing to FAT32.",
               "请检查日志中的失败记录（含偏移与错误码），并确认卷状态。","Inspect the failure record in the log (offset and error code) and verify the volume state."},
    {"FAT-LS-FAIL","FAT32 目录内容无法读取，卷结构可能已损坏。","The FAT32 directory contents could not be read; the volume structure may be damaged.",
               "请对映像执行 qc.verify，并考虑改用备份映像。","Run qc.verify on the image and consider using a backup image."},
    {"FAT-DIR-NOTFOUND","目标目录不存在。","The target directory does not exist.",
               "请核对完整路径（FAT 不区分大小写，但路径须完整）。","Verify the complete path (FAT is case-insensitive, but the full path is required)."},
    {"FAT-ENTRY-NOTFOUND","目标目录中不存在指定名称的条目。","No entry with the given name exists in the target directory.",
               "请先列出该目录确认名称，再重试。","List the directory to confirm the name, then retry."},
    {"FAT-RM-FAIL","删除操作失败；非空目录不支持直接删除。","The delete operation failed; non-empty directories cannot be removed directly.",
               "请先删除目录内的条目，或指定 recursive=true。","Remove the entries inside the directory first, or specify recursive=true."},
    {"SRC-NOTFOUND","源文件不存在或不可读。","The source file does not exist or cannot be read.",
               "请检查路径与访问权限。","Check the path and access permissions."},
    {"IMG-OPEN-FAIL","映像文件无法打开。","The image file cannot be opened.",
               "请确认路径存在、目标为普通文件且具备读写权限。","Ensure the path exists, points to a regular file, and is accessible."},
    {"QC-OPEN-FAIL","文件不是有效的 qcow2 映像（魔数或版本不符）。","The file is not a valid qcow2 image (invalid magic or unsupported version).",
               "请确认映像来源，或使用 qc.verify 检查。","Confirm the image origin, or check it with qc.verify."},
    {"PART-READ-FAIL","分区表读取失败。","The partition table could not be read.",
               "请使用 qc.verify 复核映像，必要时改用备份。","Verify the image with qc.verify; use a backup if necessary."},
    {"CMD-NOTFOUND","请求的命令不存在。","The requested command does not exist.",
               "请通过 --list 查看可用命令。","Use --list to list the available commands."},
    {"CMD-ARG-BAD","命令参数不合法。","One or more command arguments are invalid.",
               "请通过 --help=<命令> 查看参数名称与类型。","Use --help=<command> to check argument names and types."},
    {"CMD-FAILED","命令执行失败。","The command failed.",
               "请检查日志中的失败记录。","Inspect the failure record in the log."},
    {"FAT-ENTRY-EXISTS","目标位置已存在同名条目，未执行覆盖。","An entry with the same name already exists and was not overwritten.",
               "请先删除该条目、改用其他名称，或指定 --conflict=overwrite。","Delete the entry, choose another name, or specify --conflict=overwrite."},
    {"FAT-DIR-NOT-EMPTY","目录非空，操作已中止。","The directory is not empty; the operation was aborted.",
               "请先删除目录内条目，或指定 recursive=true。","Remove the entries inside the directory, or specify recursive=true."},
    {"FAT-MOVE-BAD","移动请求被拒绝：源与目标相同，或目标位于源目录的子树内。","The move was rejected: the source and target are identical, or the target lies inside the source subtree.",
               "请指定位于源目录之外的目标路径。","Specify a target path outside the source directory."},
    {"FAT-DEST-DIR-NOTFOUND","目标父目录不存在，该命令不会自动创建目录。","The target parent directory does not exist and this command does not create directories.",
               "请先创建目标目录，或改用其他目标路径。","Create the target directory first, or choose another target path."},
    {"NTFS-NOT-A-DIR","指定路径不是目录。","The specified path is not a directory.",
               "该操作仅对目录有效。","This operation applies to directories only."},
    {"NTFS-PARENT-FAIL","无法读取该目录的父引用，索引结构可能已损坏。","The parent reference of the directory could not be read; the index may be damaged.",
               "请运行 qc.verify，并停止对该卷的进一步写入。","Run qc.verify and stop further writes to this volume."},
    {"NTFS-MOVE-FAIL","盘内改名或移动失败。","The in-volume rename or move failed.",
               "请检查日志中 move_link_fail / move_sync_fail / move_delete_fail 对应的失败阶段。","Inspect the log for move_link_fail, move_sync_fail or move_delete_fail to identify the failing stage."},
    {"NTFS-MOVE-INTO-SELF","目标位于源目录自身或其子树内，操作将造成目录环。","The target lies in the source directory or its subtree, which would create a directory cycle.",
               "请指定位于源目录之外的目标父目录。","Specify a target parent outside the source directory."},
    {"NTFS-DIR-PHANTOM","写入后的磁盘复核未通过：目录或名称未实际落盘，卷已处于不一致状态。","Post-write verification failed: the directory or name is not present on disk and the volume is inconsistent.",
               "请立即停止对该映像的写入，改用备份映像，并使用 tools/ntfs-inventory.py 或 chkdsk 定位。","Stop writing to this image immediately, switch to a backup, and locate the damage with tools/ntfs-inventory.py or chkdsk."},
    {"NTFS-SRC-IS-DIR","源对象是目录，但该操作仅接受文件，或已禁用目录跨父搬移。","The source is a directory, but this operation accepts files only, or cross-parent directory moves are disabled.",
               "请在搬移目录时不要指定 allow_dir_move=false；其余场景请改用文件。","Do not pass allow_dir_move=false when moving directories; otherwise use a file."},
    {"NTFS-PATH-NOTFOUND","盘内路径不存在。","The in-volume path does not exist.",
               "请先以 ntfs.list 列出父目录核对名称；路径须完整（大小写不敏感）。","List the parent directory with ntfs.list to confirm the name; the full path is required (case-insensitive)."},
    {"NTFS-VOLUME-DIRTY","卷已被标记为脏，上一次写入未正常结束。","The volume is marked dirty; the previous write did not finish cleanly.",
               "请重新格式化该分区，或先执行 boot.check 评估影响。","Reformat the partition, or run boot.check to assess the impact."},
    {"BCD-BUILD-FAIL","由模板构建 BCD 失败。","Building the BCD from the template failed.",
               "请确认 --table 与分区表类型一致（MBR 不应使用 GPT 模板）。","Ensure --table matches the partition table type (do not use a GPT template for MBR)."},
    {"WIM-NOT-A-DIR","WIM 内指定路径为文件而非目录。","The specified path inside the WIM is a file, not a directory.",
               "列目录请使用其父路径；取文件请使用 wim.extract。","List the parent path, or use wim.extract to obtain the file."},
    {"ISO-READ-FAIL","ISO 可打开但内容读取失败。","The ISO can be opened but its contents cannot be read.",
               "请确认文件完整、路径有效；只读描述符不可用于写入。","Verify file integrity and path validity; a read-only descriptor cannot be used for writing."},
    {"TMPDIR-UNAVAILABLE","没有可写的临时目录。","No writable temporary directory is available.",
               "请设置 IMGTOOL_TMPDIR 或 TMPDIR 指向可写目录。","Set IMGTOOL_TMPDIR or TMPDIR to a writable directory."},
    {"WIM-NOTFOUND","WIM/ESD 文件无法打开。","The WIM/ESD file cannot be opened.",
               "请确认路径有效，并先用 wim.identify 核对映像索引。","Verify the path and use wim.identify to check the image index."},
    {"WIM-IDX-INVALID","指定的映像索引不存在。","The specified image index does not exist.",
               "请先用 wim.identify 列出索引，再通过 --index 指定。","List the indexes with wim.identify, then pass --index."},
    {"NTFS-FORMAT-FAIL","NTFS 格式化失败，写入入前的准备步骤未完成。","NTFS formatting failed; the preparation step before applying the image did not complete.",
               "请缩减分区容量后重试，或单独执行 fs.format 排查，并检查日志。","Reduce the partition size and retry, or run fs.format separately; check the log."},
    {"WIM-APPLY-FAIL","WIM 写入入失败；格式化已完成，本步骤未完成。","Applying the WIM failed; formatting completed but this step did not.",
               "请检查日志中的失败记录，并对映像执行 qemu-img check。","Inspect the failure record in the log and run qemu-img check on the image."},
    {"CLONE-COPY-FAIL","克隆拷贝失败；旧映像已恢复为原名称。","The clone copy failed; the original image was restored to its former name.",
               "请检查旧映像、宿主可用空间与访问权限（qemu-img check）。","Check the original image, host free space and permissions (qemu-img check)."},
    {"CLONE-VERIFY-FAIL","克隆后校验未通过；旧映像已恢复为原名称。","Post-clone verification failed; the original image was restored to its former name.",
               "请先单独校验旧映像，确认无损坏后重试。","Verify the original image separately before retrying."},
    {"IO-TIMEOUT","磁盘 I/O 长时间无进展，操作已中止。","Disk I/O made no progress for an extended period; the operation was aborted.",
               "请检查介质与访问权限；慢速介质可增大 --io-timeout-ms（0 表示关闭）。","Check the medium and permissions; for slow media increase --io-timeout-ms (0 disables the timeout)."},
    {"FEATURE-UNAVAILABLE","当前构建未包含该功能（缺少依赖库）。","This build does not include the feature (missing dependency).",
               "请重新构建并启用对应选项；可用 --list 的 features 字段确认。","Rebuild with the corresponding option enabled; check the features field of --list."},
    {"CMD-CANCELLED","操作已被取消。","The operation was cancelled.",
               "目标分区或文件可能处于中间状态，请重新执行该步骤。","The target partition or file may be in an intermediate state; re-run the step."},
    {"HOST-OPEN-FAIL","宿主文件无法打开。","The host file cannot be opened.",
               "请检查路径与权限；Android 上须先将 assets 中的参考件释放到 cacheDir。","Check the path and permissions; on Android, extract assets to cacheDir first."},
    {"IMG-FORMAT","映像格式不合法或已损坏，可打开但无法正常读写。","The image format is invalid or corrupted; it can be opened but not read or written normally.",
               "请使用 qemu-img check 或 qc.verify 复核，必要时改用备份。","Verify with qemu-img check or qc.verify; use a backup if necessary."},
    {"PART-WRITE-FAIL","分区表写入失败。","Writing the partition table failed.",
               "请确认映像可写、宿主空间充足且文件未被占用。","Ensure the image is writable, host space is sufficient and the file is not in use."},
    {"PART-FS-BAD","目标分区的文件系统类型不符（未格式化或类型错误）。","The target partition has an unexpected file system (unformatted or wrong type).",
               "请先以 fs.format 格式化该分区，再重试。","Format the partition with fs.format, then retry."},
    {"PART-JSON-FAIL","分区表序列化失败，分区数量超出内部上限。","Serialising the partition table failed; the partition count exceeds the internal limit.",
               "请用 part.show 检查分区表，并提交日志。","Inspect the table with part.show and attach the log."},
    {"PART-COPY-FAIL","分区拷贝失败。","The partition copy failed.",
               "请核对 --src-lba/--dst-lba 与两侧分区容量，并检查宿主空间。","Verify --src-lba/--dst-lba and both partition sizes; check host free space."},
    {"BCD-REF-BAD","参考 BCD 类型不符（BIOS/UEFI 混用）或文件已损坏。","The reference BCD has the wrong type (BIOS/UEFI mismatch) or is damaged.",
               "请使用对应引导方式的参考件：BIOS 用 bcdboot 产物，UEFI 用 ESP 上的副本。","Use a reference of the matching boot mode: bcdboot output for BIOS, the ESP copy for UEFI."},
    {"BCD-PATCH-FAIL","改写 BCD 设备对象失败。","Patching the BCD device objects failed.",
               "请确认参考件完整（reference/bcd-reference-bios-win11.bin）并提交日志。","Verify the reference is intact (reference/bcd-reference-bios-win11.bin) and attach the log."},
    {"BCD-WRITE-FAIL","写入 Boot/BCD 失败。","Writing Boot/BCD failed.",
               "请确认系统保留分区为 NTFS 且空间充足。","Ensure the system reserved partition is NTFS with sufficient space."},
    {"BOOT-FORMAT-UNSUPPORTED","该 Windows 版本不支持以当前分区表方式启动。","This Windows version cannot boot with the selected partition scheme.",
               "32 位 Windows 没有 UEFI 启动路径，请改用 MBR/BIOS。","32-bit Windows has no UEFI boot path; use MBR/BIOS instead."},
    {"BOOT-FORMAT-NEEDS-CONFIRM","该组合可以启动，但存在兼容性风险，需要用户确认。","The combination is bootable but carries a compatibility risk and requires user confirmation.",
               "请向用户确认后以 --force=true 重新调用。","Confirm with the user, then re-run with --force=true."},
    {"BOOT-VBR-WRITE-FAIL","写入 VBR 引导码失败。","Writing the VBR boot code failed.",
               "请核对 --vbr-tpl=映像:lba 与目标分区一致，且目标已格式化为 NTFS。","Ensure --vbr-tpl=image:lba matches the target partition and that it is formatted as NTFS."},
    {"GUID-INVALID-BAD","GUID 格式不合法。","The GUID format is invalid.",
               "请使用 8-4-4-4-12 形式的十六进制 GUID。","Use a hexadecimal GUID in 8-4-4-4-12 form."},
    {"LIST-FAIL","列表或命令表渲染失败（内部错误）。","Rendering the list or command table failed (internal error).",
               "请重试；若持续失败请提交日志。","Retry; attach the log if the failure persists."},
    {"WINRE-NOTFOUND","该 WIM/ESD 中不包含 Winre.wim。","The WIM/ESD does not contain Winre.wim.",
               "请改用包含 WinRE 的映像；精简版通常已移除该组件。","Use an image that includes WinRE; trimmed editions usually remove it."},
    {"REC-VERIFY-FAIL","恢复分区填充失败。","Populating the recovery partition failed.",
               "请确认分区已格式化且空间充足，并检查日志中的 at/error_code。","Ensure the partition is formatted with sufficient space; check at/error_code in the log."},
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
static void wr_all(int fd, const char *b, size_t n){   /* 忽略写失败（卡片/说明行丢了也不能拖垮命令本身）*/
    size_t off=0;
    while(off<n){ ssize_t w=write(fd,b+off,n-off); if(w<=0) return; off+=(size_t)w; }
}

/* ================= 说明行（Linux 风格） ================= */
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
    n+=snprintf(card+n,sizeof card-(size_t)n,"%s: %s\n", en?"Image":"映像  ", ei->img[0]?ei->img:"?");
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

/* ★ 2026-10-26（REQ-16 §3E）：--progress=json 的进度行。
 *   iso.extract 抽 4~5 GB 时 App 的进度条要能看到在干活（以前只有 bar 模式有回调，
 *   json 模式等于静默）。每条一行，和结果信封同走 stdout —— App 按 "ev" 过滤即可。*/
static void json_esc(const char *s, char *out, size_t cap){
    size_t k=0;
    for(const unsigned char *p=(const unsigned char*)s; p&&*p && k+8<cap; p++){
        if(*p=='"'||*p=='\\'){ out[k++]='\\'; out[k++]=(char)*p; }
        else if(*p<0x20) k+=(size_t)snprintf(out+k,cap-k,"\\u%04x",*p);
        else out[k++]=(char)*p;
    }
    out[k]=0;
}
static void on_progress_json(void *ud, const img_progress_t *p){
    (void)ud;
    if(!p) return;
    const char *ph=img_phase_name(p->phase), *op=img_op_name(p->op);
    char line[768], it[256];
    json_esc(p->item?p->item:"",it,sizeof it);
    int n=snprintf(line,sizeof line,
        "{\"ev\":\"progress\",\"phase\":\"%s\",\"op\":\"%s\","
        "\"cur\":%llu,\"total\":%llu,\"cur_bytes\":%llu,\"total_bytes\":%llu,\"pct\":%d,"
        "\"item\":\"%s\"",
        ph?ph:"", op?op:"",
        (unsigned long long)p->cur,(unsigned long long)p->total,
        (unsigned long long)p->cur_bytes,(unsigned long long)p->total_bytes,
        p->pct, it);
    if(p->eta_s>=0 && n>0 && n<(int)sizeof line-16)
        n+=snprintf(line+n,sizeof line-(size_t)n,",\"eta_s\":%d",p->eta_s);
    if(n>0 && n<(int)sizeof line-4) n+=snprintf(line+n,sizeof line-(size_t)n,"}\n");
    if(n>0) wr_all(1,line,(size_t)n);
}

/* ================= 用法帮助 ================= */
static void print_help(const img_cmd *c){
    if(!c){ printf("没有这个命令\n"); return; }
    printf("%s  (since %s)\n  %s\n", c->name, c->since?c->since:"", c->help?c->help:"");
    if(c->usage && c->usage[0])
        printf("  %s: imgcmd %s\n", lang_en()?"Usage":"用法", c->usage);
    if(c->nargs){
        printf("  %s：\n", lang_en()?"Arguments":"参数");
        for(int i=0;i<c->nargs;i++){
            const img_arg *a=&c->args[i];
            const char *ty=lang_en()?"string":"字符串";
            switch(a->ty){ case IMG_A_U64: ty=lang_en()?"number":"数字"; break;
                           case IMG_A_I64: ty=lang_en()?"integer":"整数"; break;
                           case IMG_A_BOOL:ty=lang_en()?"boolean":"布尔"; break;
                           case IMG_A_ENUM: ty=lang_en()?"enum":"枚举"; break; default: break; }
            /* ★ 默认值要和帮助文本隔开：以前直接 printf("%s%s") 拼出
             *   "falsetrue=整棵子树" 这种鬼东西（--help=wim.ls 实测）。*/
            const char *req = a->required ? (lang_en()?"required":"必填")
                                          : (lang_en()?"optional":"可选");
            if(!a->required && a->def && a->def[0])
                printf("    --%-12s %-6s %s  （默认 %s）%s\n",
                       a->name, ty, req, a->def, a->help? a->help :"");
            else
                printf("    --%-12s %-6s %s  %s\n",
                       a->name, ty, req, a->help? a->help :"");
        }
    }
}

int main(int argc, char **argv){
    const char *cmd=NULL,*log=NULL,*lvl=NULL,*helpcmd=NULL,*logmax=NULL,*progress=NULL,*iot=NULL;
    bool list=false, ro=false;
    const char *posv[32]; int npos=0;      /* 位置参数：copy <src> <dst> */
    ui_t ui={false};
    img_args *args=img_args_new();
    if(!args) return 1;

    { const char *el=getenv("IMGTOOL_LANG"); if(el&&*el){ snprintf(g_lang,sizeof g_lang,"%s",el); img_lang_set(el); } }

    for(int i=1;i<argc;i++){
        const char *a=argv[i];
        if(a[0]!='-'){
            if(!cmd){ cmd=a; continue; }
            if(npos>=(int)(sizeof posv/sizeof posv[0])){
                fprintf(stderr,"位置参数过多：%s\n",a); img_args_free(args); return 2;
            }
            posv[npos++]=a;
            continue;
        }
        const char *eq=strchr(a,'=');
        char key[64]; const char *val=eq?eq+1:"";
        if(eq){ size_t n=(size_t)(eq-a-2); if(n>=sizeof key) n=sizeof key-1; memcpy(key,a+2,n); key[n]=0; }
        else   snprintf(key,sizeof key,"%s",a+2);

        if(!strcmp(key,"version")){
            /* REQ-64：App 侧一直以为有 --version（其实没有），补上。
             * 输出与 `imgcmd version` 同构，方便脚本直接 jq。*/
            printf("{\"ok\":true,\"cmd\":\"version\",\"version\":1,\"data\":{"
                   "\"tool\":\"imgtool\",\"version\":\"%s\",\"build_date\":\"%s\","
                   "\"commit\":\"%s\",\"platform\":\"%s\",\"abi\":\"%s\"}}\n",
                   IMGTOOL_BUILD_DATE, IMGTOOL_BUILD_DATE, IMGTOOL_BUILD_COMMIT,
                   img_platform(), img_abi());
            img_args_free(args);
            return 0;
        }
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
        if(img_args_set(args,key,val)!=IMG_OK){
            fprintf(stderr,"参数 %s 过长（单个值上限 2047 字节）或名称非法\n",key);
            img_args_free(args); return 2;
        }
    }

    /* 位置参数：按命令表顺序填，已被 --name=value 占用的跳过。
     * 例：imgcmd copy "/a.qcow2!p3:/x" "/b.qcow2!p3:/y" --conflict=rename */
    if(cmd && npos){
        const img_cmd *cc=img_cmd_find(cmd);
        if(!cc || cc->npos<=0){
            fprintf(stderr,"命令 %s 不支持位置参数；请用 --参数=值（--help=%s 看用法）\n",cmd,cmd);
            img_args_free(args); return 2;
        }
        int pi=0;
        for(int k=0;k<npos;k++){
            while(pi<cc->npos && img_args_get(args,cc->args[pi].name)) pi++;
            if(pi>=cc->npos){
                fprintf(stderr,"位置参数过多：%s（%s 最多 %d 个）\n",posv[k],cmd,cc->npos);
                img_args_free(args); return 2;
            }
            if(img_args_set(args,cc->args[pi].name,posv[k])!=IMG_OK){
                fprintf(stderr,"参数 %s 过长或非法\n",cc->args[pi].name);
                img_args_free(args); return 2;
            }
            pi++;
        }
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
    /* 屏幕形态：tty → 说明行；非 tty → JSON；--progress=bar|json|none 强制 */
    ui.human = progress ? (!strcmp(progress,"bar")) : (isatty(2)!=0);
    if(progress && !strcmp(progress,"none")) ui.human=false;
    /* ★ 说明模式 → 百分比行；--progress=json → 每条一行 {"ev":"progress",…}；
     *   --progress=none / 默认非 tty → 不注册（零开销，只有一行信封）。*/
    if(progress && !strcmp(progress,"json")) img_cmd_set_progress(on_progress_json, NULL);
    else if(ui.human) img_cmd_set_progress(on_progress, &ui);
    img_out_open_fd(2, ui.human);      /* human=true → 库不再往 stderr 吐机器行 */
    img_out_set_text(on_event, &ui);   /* ★ 库发码 → 这里出中文/英文说明行 */

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

    /* ★ 先判命令存在，再打开 --img：否则旧命令名 + 不存在的镜像会报
     *   IMG-OPEN-FAIL，看起来像"镜像坏了"，而不是"命令没了"。
     *   放在 ui/卡片上下文初始化之后，保证卡片也有建议文案。*/
    if(!img_cmd_find(cmd)){
        img_result r; img_result_begin(&r,cmd);
        img_result_fail(&r,IMG_ENOENT,"CMD-NOTFOUND",cmd,"没有这个命令",
                        "用 --list 查看可用命令；文件操作已合并为 copy/move/rename/rm");
        char b[1024]; int n=img_result_json(&r,b,sizeof b);
        if(n>0) fwrite(b,1,(size_t)n,stdout);
        img_error_info_t ei; img_error_info(&ei);
        /* 命令不存在是"用法级"致命错：卡片直接出到 stderr（非 tty 也出），
         * 否则机器模式下只剩一行码，用户不知道旧命令已经合并。*/
        if(ei.code[0]) render_card(&ei, true);
        img_args_free(args); return 1;
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
    /* ★ REQ-29：--dst=- / --dst-fd=1 时 stdout 是【数据流】，结果 JSON 必须改走 stderr，
     *   否则数据末尾会多出一行 JSON（文件多 154 字节，校验和全废）。*/
    FILE *resf=stdout;
    const char *dstv=img_args_get(args,"dst");
    if(dstv && !strcmp(dstv,"-")) resf=stderr;
    else if(img_args_u64(args,"dst-fd",0)==1) resf=stderr;
    if(n>0) fwrite(out,1,(size_t)n,resf);
    if(rc!=IMG_OK){
        img_error_info_t ei; img_error_info(&ei);
        if(ei.code[0]) render_card(&ei, ui.human);
    }
    if(ctx) img_ctx_close(ctx);
    img_log_close();
    img_args_free(args);
    return rc==IMG_OK?0:1;
}
