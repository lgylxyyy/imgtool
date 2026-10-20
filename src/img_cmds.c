/* img_cmds.c — 命令表（加新功能就在这里加一行 + 写一个 handler）
 *
 * 每个命令 = {名字, 版本, 帮助, 参数规格[], handler}
 * 参数规格里的类型/必填/默认/校验，CLI 与 App 共用 —— 不再两处各写一遍。
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>   /* img.create/ntfs.put 要 stat 判存在与大小 */
#include "img_cmd.h"
#include "img_fixboot.h"
#include "img_bcd.h"
#include "img_constraints.h"   /* img_layout_build：img.create 用 */
#include "img_bootcheck.h"
#include "img_iso.h"      /* boot.check 用 */
#include "img_io.h"
#include "img_fat32.h"
#include "img_fat32_fs.h"
#include "img_event.h"
#include "img_wim_apply.h"
/* ---- 功能开关（CMake 降级判定；缺库时对应 =0，命令仍列在表里但明确报不可用）---- */
#ifndef IMG_WITH_WIM
#define IMG_WITH_WIM 0
#endif
#ifndef IMG_WITH_NTFS3G
#define IMG_WITH_NTFS3G 0
#endif
/* ★ 硬缺口 #3 的落点：库缺失【不许悄悄失败/不许构建失败】，必须给出稳定错误码 + 可执行建议 */
static int feat_unavail(img_result *out, const char *feat, const char *howto){
    img_result_fail(out, IMG_ENOTSUP, "FEATURE-UNAVAILABLE", feat,
                    "这个构建没带该功能依赖的系统库", howto);
    return IMG_ENOTSUP;
}

#include "img_ntfs_fmt.h"
 #include "img_clone.h"
#include "img_ntfs_fs.h"   /* part.scan / ntfs.list / ntfs.rename */
#include "img_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
 #include <strings.h>
#include <unistd.h>   /* rename/unlink/access（克隆的改名与回滚护栏）*/  /* strcasecmp（Windows/Android 都可用）*/
#include <time.h>

static long long now_ms_(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* ---------- 参数级校验（只写一处，CLI/App 都走这里） ---------- */
static int chk_lba(const char *v, char *why, size_t cap){
    uint64_t x=strtoull(v,NULL,0);
    if(x<1){ snprintf(why,cap,"LBA 必须 >=1（0 是 MBR 区，绝不允许当分区用）"); return IMG_EINVAL; }
    if(x>0xFFFFFFFFull){ snprintf(why,cap,"LBA 过大（>2^32）"); return IMG_ERANGE; }
    return IMG_OK;
}

/* ================= 命令 1：img.info ================= */
static const img_arg A_INFO[] = {
    {"img", IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
};
static int cmd_img_info(img_ctx *ctx, const img_args *in, img_result *out){
    (void)in;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){
        img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),
                        "不是合法的 qcow2（魔数或版本不符）",
                        "确认是本工具创建的镜像；或先用 imgq2test 校验");
        return IMG_EFORMAT;
    }
    img_result_u64(out,"size_bytes",img_qcow2_size(q));
    img_parttable_t *pt=NULL;
    if(img_ctx_parttable(ctx,&pt)==IMG_OK){
        img_result_str(out,"table", pt->table==IMG_TABLE_GPT?"GPT":(pt->table==IMG_TABLE_MBR?"MBR":"NONE"));
        img_result_u64(out,"disk_sectors",pt->disk_sectors);
        if(pt->table==IMG_TABLE_GPT) img_result_str(out,"disk_guid",pt->disk_guid);
        int used=0; for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(pt->parts[i].used) used++;
        img_result_u64(out,"partitions",(uint64_t)used);
    }
    return IMG_OK;
}

/* ================= 命令 2：part.show ================= */
static const img_arg A_PTSHOW[] = {
    {"img", IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
};
/* GPT 最多 128 个分区项，一条上限约 300 字节 → 48 KiB；取 64 KiB 留余量。
 * img_parttable_json() 装不下会返回 0（不再静默截断），这里据此报 PART-JSON-FAIL。 */
static char g_pt_json[64*1024];
static int cmd_part_show(img_ctx *ctx, const img_args *in, img_result *out){
    (void)in;
    img_parttable_t *pt=NULL;
    int rc=img_ctx_parttable(ctx,&pt);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),
                        "镜像可能损坏；用 imglayout check 复核");
        return rc;
    }
    int n=img_parttable_json(pt,g_pt_json,sizeof g_pt_json);
    if(n<=0){ img_result_fail(out,IMG_EIO,"PART-JSON-FAIL",img_ctx_path(ctx),"分区表序列化失败",NULL); return IMG_EIO; }
    img_result_raw(out,"pt",g_pt_json);
    return IMG_OK;
}

/* ================= 命令 3：fat.put（★ 演示 check_size 开关） ================= */
static const img_arg A_FATPUT[] = {
    {"img",        IMG_A_STR,  true,  NULL,   "镜像路径（qcow2）",        NULL},
    {"lba",        IMG_A_U64,  true,  NULL,   "目标分区起始 LBA",         chk_lba},
    {"src",        IMG_A_STR,  true,  NULL,   "本地源文件路径",           NULL},
    {"dst",        IMG_A_STR,  true,  NULL,   "分区内目标路径（/ 开头）", NULL},
    {"check_size", IMG_A_BOOL, false, "false","写入前校验【内部分区】空间（默认关：App 显式请求才校验）", NULL},
    /* ★ 宿主空间：默认开、不可关（它保护的是镜像文件本身 —— 宿主满会把整个
     *   qcow2 写坏）。工具优先自己查（fstatfs(fd)，路径/SAF fd 都行）；
     *   查不到时允许 App 把宿主剩余传进来（拿 fd 查不到的云端提供者就是这种情况）。*/
    {"host_free", IMG_A_U64,  false, NULL, "宿主剩余可用字节（一般不用传，工具自己查；查不到时才传）", NULL},
    {"reserve",   IMG_A_BOOL, false, "false","写前用 fallocate 真预留宿主空间（立即占用，保证不会中途 ENOSPC）", NULL},
};
static int cmd_fat_put(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *src=img_args_get(in,"src"), *dst=img_args_get(in,"dst");
    bool check = img_args_bool(in,"check_size",false) || img_ctx_check_size(ctx);

    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),"不是合法 qcow2",NULL); return IMG_EFORMAT; }

    FILE *f=fopen(src,"rb");
    if(!f){
        img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",src,
                        "源文件打不开（不存在/无权限）","检查路径与权限");
        return IMG_ENOENT;
    }
    fseek(f,0,SEEK_END); long long fsz=ftell(f); fclose(f);

    img_fat32_fs *fs=img_fat32_mount(q,lba,true);
    if(!fs){
        img_result_fail(out,IMG_EFORMAT,"FAT-MOUNT-FAIL",dst,
                        "目标 LBA 上不是合法 FAT32（未格式化或 LBA 不对）",
                        "先用 imgmkfs 格式化该分区，或核对 --lba");
        return IMG_EFORMAT;
    }

    /* ★ 尺寸校验开关：默认【不校验】（App 显式请求才校验）。
     *   不校验也必须把底层失败报清楚（ENOSPC 会转成同一张失败信封）。*/
    if(check){
        uint64_t freeb=img_fat32_free_bytes(fs);
        if(freeb < (uint64_t)fsz){
            img_fat32_unmount(fs);
            char why[256];
            snprintf(why,sizeof why,"目标分区剩余 %llu MB，要写入 %lld MB",
                     (unsigned long long)(freeb/1048576), fsz/1048576);
            img_result_fail(out,IMG_EFULL,"FAT-ENOSPC",dst,why,
                            "换更小的文件，或先扩容该分区（imgresize）");
            return IMG_EFULL;
        }
    }

    /* ---- 宿主账（默认开、不可关）----
     *   需求 = 源文件大小 → qcow2 增长估算（+3% 元数据 + 4MiB，实测保守 15% 过剩）
     *   剩余 = fstatfs(fd).f_bavail × f_bsize（fd 模式同样可用，Android 也 OK）*/
    uint64_t est_host = img_host_estimate_write((uint64_t)fsz, false);
    uint64_t host_free = 0;
    const char *hf = img_args_get(in,"host_free");
    if(hf && *hf) host_free = img_args_u64(in,"host_free",0);
    else          host_free = img_host_free_bytes(img_ctx_io(ctx));
    if(host_free==0){
        /* 查不到（如管道类 fd）：留痕但不拦 —— 卡片里会写明"宿主剩余未知" */
        img_ev e;
        if(img_ev_init(&e,IMG_LV_WARN,"host","free_unknown",IMG_OK,"宿主剩余空间未知，跳过宿主校验")){
            img_ev_u64(&e,"need_bytes",est_host);
            img_ev_emit(&e);
        }
    } else if(host_free < est_host){
        img_fat32_unmount(fs);
        char why[256];
        /* 小于 1MB 时按 KB 显示，避免"剩余 0 MB"这种看不出量级的提示 */
        char a[48], b[48];
        if(host_free>=1048576) snprintf(a,sizeof a,"%llu MB",(unsigned long long)(host_free/1048576));
        else                   snprintf(a,sizeof a,"%llu KB",(unsigned long long)(host_free/1024));
        if(est_host>=1048576)  snprintf(b,sizeof b,"%llu MB",(unsigned long long)(est_host/1048576));
        else                   snprintf(b,sizeof b,"%llu KB",(unsigned long long)(est_host/1024));
        snprintf(why,sizeof why,"宿主剩余 %s，本次预计需要 %s（qcow2 文件会长大）",a,b);
        img_result_fail(out,IMG_EFULL,"HOST-ENOSPC",dst,why,
                        "清理宿主磁盘，或换更小的源文件/更小的镜像");
        return IMG_EFULL;
    }

    /* ---- 可选真预留（稀疏文件的 ftruncate 是假承诺，fallocate 才是真占位）---- */
    if(img_args_bool(in,"reserve",false)){
        int fdh = img_io_fd(img_ctx_io(ctx));
        int rrc = img_host_reserve(fdh, est_host);
        if(rrc!=IMG_OK){
            img_ev e;                     /* 占不到 ≠ 不能写，只是"无法保证"（部分 Android 存储不支持 fallocate）*/
            if(img_ev_init(&e,IMG_LV_WARN,"host","reserve_fail",rrc,"fallocate 不支持，无法保证宿主空间")){
                img_ev_u64(&e,"need_bytes",est_host);
                img_ev_emit(&e);
            }
        }
    }

    long long t0=now_ms_();
    int rc=img_fat32_put_file(fs,src,dst,img_cmd_prog());
    long long ms=now_ms_()-t0;
    img_fat32_unmount(fs);

    if(rc!=IMG_OK){
        img_result_fail(out,rc, rc==IMG_EFULL?"FAT-ENOSPC":"FAT-PUT-FAIL", dst,
                        rc==IMG_EFULL?"目标分区空间不足":"写入失败（详见日志/事件）",
                        "空间不足→扩容分区或换小文件；镜像损坏→先 imgq2test 校验");
        return rc;
    }
    img_result_u64(out,"written_bytes",(uint64_t)fsz);
    img_result_i64(out,"ms",ms);
    return IMG_OK;
}

/* ================= 命令 4：fat.ls ================= */
static const img_arg A_FATLS[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）",  NULL},
    {"lba",  IMG_A_U64, true,  NULL, "分区起始 LBA",        chk_lba},
    {"path", IMG_A_STR, false, "/",  "分区内目录（/ 开头）", NULL},
};
typedef struct { char *p, *end; int n; } lsbuf;
static void ls_cb(void *ud, const char *name, bool isdir, uint64_t size){
    lsbuf *b=(lsbuf*)ud;
    if(b->p>=b->end) return;
    if(b->n) *b->p++=',';
    b->n++;
    char item[512];
    int n=snprintf(item,sizeof item,"{\"name\":\"%s\",\"dir\":%s,\"size\":%llu}",
                   name?name:"", isdir?"true":"false",(unsigned long long)size);
    if(n>0 && b->p+n<b->end){ memcpy(b->p,item,(size_t)n); b->p+=n; }
}
static char g_ls_json[65536];
static int cmd_fat_ls(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path"); if(!path||!*path) path="/";
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),"不是合法 qcow2",NULL); return IMG_EFORMAT; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,false);
    if(!fs){
        img_result_fail(out,IMG_EFORMAT,"FAT-MOUNT-FAIL",path,
                        "目标 LBA 上不是合法 FAT32","核对 --lba，或先用 imgmkfs 格式化");
        return IMG_EFORMAT;
    }
    g_ls_json[0]='[';
    lsbuf b={g_ls_json+1, g_ls_json+sizeof g_ls_json-4, 0};
    int rc=img_fat32_list_dir(fs,path,ls_cb,&b);
    b.p[0]=']'; b.p[1]=0;
    img_fat32_unmount(fs);
    if(rc==IMG_ENOTSUP){
        img_result_fail(out,IMG_ENOENT,"FAT-DIR-NOTFOUND",path,"目录不存在或不是目录","核对路径");
        return IMG_ENOENT;
    }
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,img_strerror(rc),"镜像可能损坏"); return rc; }
    img_result_u64(out,"count",(uint64_t)b.n);
    img_result_raw(out,"entries",g_ls_json);
    return IMG_OK;
}

/* ================= 命令 5：wim.size（尺寸估算，政策留给 App） ================= */
static const img_arg A_WIMSIZE[] = {
    {"wim",   IMG_A_STR, true,  NULL, "WIM/ESD 路径",        NULL},
    {"index", IMG_A_U64, false, "1",  "镜像索引（见 --list 用 imgwimls 查）", NULL},
};
static int cmd_wim_size(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 尺寸估算）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_WIM

    (void)ctx;
    const char *wim=img_args_get(in,"wim");
    int idx=(int)img_args_u64(in,"index",1);
    img_wim_size_t sz;
    int rc=img_wim_estimate(wim,idx,&sz);
    if(rc!=IMG_OK){
        img_result_fail(out,rc, rc==IMG_ENOENT?"WIM-NOTFOUND":"WIM-IDX-INVALID", wim,
                        rc==IMG_ENOENT?"WIM/ESD 打不开（不存在或不是 WIM 格式）"
                                      :"镜像索引不存在",
                        "先用 imgwimls <wim> 列出索引，再带 --index=N");
        return rc;
    }
    img_result_str(out,"name",sz.name);
    img_result_str(out,"compression",sz.compression);
    img_result_u64(out,"data_bytes",sz.data_bytes);
    img_result_u64(out,"file_count",sz.file_count);
    img_result_u64(out,"dir_count",sz.dir_count);
    img_result_u64(out,"est_ntfs_bytes",sz.est_ntfs_bytes);
    img_result_u64(out,"est_ntfs_mb",sz.est_ntfs_bytes/1048576);
    /* ★ 工具只给数字；"够不够 / 分区开多大"由 App 决定（政策不在工具里）*/
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}

/* ================= 命令 5b：wim.apply（灌入 WIM/ESD，可先格式化目标分区）====================
 * ★ 默认【不格式化】（--format=off，用户拍板：有数据先问 App）；显式 --format=on 才格式化：
 *   1) 防残留 —— 目标分区若是旧系统/别的数据，wimlib 是覆盖式写入，不会清掉没被覆盖的旧文件；
 *   2) 阶段分离 —— 格式化与灌入是两个里程碑，日志能定位卡在哪一步（规范第七节要求）；
 *   3) 参数可控 —— 簇大小/卷标由我们定，不随 wimlib 实现版本漂移。
 * 实测：先格式化再灌 → rc=0，抽回 ntoskrnl.exe 与"直接灌"逐字节一致（md5 相同），
 *       分区 $Boot 完好、qemu-img check 无错。
 * 保留原分区用 --format=off（如"只重灌数据不重建卷"的场景）。*/
static const img_arg A_WIMAPPLY[] = {
    {"img",    IMG_A_STR,  true,  NULL,      "镜像路径（qcow2）", NULL},
    {"lba",    IMG_A_U64,  true,  NULL,      "目标分区起始 LBA",  chk_lba},
    {"wim",    IMG_A_STR,  true,  NULL,      "WIM/ESD 文件路径",  NULL},
    {"index",  IMG_A_U64,  false, "1",       "WIM 内的映像索引",  NULL},
    {"format", IMG_A_ENUM, false, "on",       "灌入前是否格式化目标分区（on|off）；★不传=on（会毁目标分区数据）", NULL},
    {"label",  IMG_A_STR,  false, "Windows", "（format=on）卷标", NULL},
    {"no_acls", IMG_A_BOOL,false, "false",   "不要 ACL（跨平台/不需要时开）", NULL},
    {"no_attrs",IMG_A_BOOL,false, "false",   "不要扩展属性", NULL},
    {"wimboot", IMG_A_BOOL,false, "false",   "wimboot 模式（文件指针化）", NULL},
};
static int cmd_wim_apply_cmd(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 直灌）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_WIM

    uint64_t lba=img_args_u64(in,"lba",0);
    const char *wim=img_args_get(in,"wim");
    int idx=(int)img_args_u64(in,"index",1);

    /* 表驱动没有 enum 值域校验 → 这里显式判（严格式，不许 --format=maybe 被静默当 off）*/
    const char *fmt=img_args_get(in,"format");
    if(fmt && strcmp(fmt,"on") && strcmp(fmt,"off")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","format","只接受 on|off",NULL);
        return IMG_EINVAL;
    }
    bool do_fmt = !fmt || !strcmp(fmt,"on");

    /* 分区大小（格式化与灌入都要） */
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }

    /* ① 里程碑：格式化（默认开） */
    if(do_fmt){
        img_span sp;
        img_span_prep(&sp,"ntfs","format","格式化系统分区（灌 WIM 前）");
        img_span_kv_u64(&sp,"lba",lba);
        img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,lba,pb);
        o.label=img_args_get(in,"label");
        rc=img_ntfs_format(q,&o,img_cmd_prog());
        img_span_end(&sp,rc);
        if(rc){ img_result_fail(out,rc,"NTFS-FORMAT-FAIL",NULL,NULL,NULL); return rc; }
    }

    /* ② 里程碑：灌入 */
    img_wim_apply_opts_t wo; memset(&wo,0,sizeof wo);
    wo.no_acls  =img_args_bool(in,"no_acls",false);
    wo.no_attrs =img_args_bool(in,"no_attrs",false);
    wo.wimboot  =img_args_bool(in,"wimboot",false);
    rc=img_wim_apply(wim,idx,q,lba,&wo,img_cmd_prog());
    if(rc){
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"WIM-APPLY-FAIL";
        img_result_fail(out,rc,code,NULL,NULL,NULL);
        return rc;
    }
    img_result_u64(out,"index",(uint64_t)idx);
    img_result_bool(out,"formatted",do_fmt);
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}

/* ================= 命令 6b：part.scan（分区扫描 —— App 判断"有没有数据"的依据）====================
 * ★ 层归属（PARTITION-POLICY #11/#13）：这里只出【事实】，弹窗/按钮/文案全归 App。
 *   三级口径（用户点名要对拍验证）：
 *     level 0  无文件系统（空白/未格式化）        → App：直接装
 *     level 1  有文件系统，但根目录是空的         → App：直接装（别打扰用户）
 *     level 2  有文件系统 + 根目录非空            → App：弹三选
 *   附加 looks_like_windows（根目录有 \Windows）→ App 决定提示文案用不用。
 * 只读语义：FAT 用只读挂载；NTFS 走 libntfs-3g 挂载但不写任何数据。 */
static const img_arg A_PARTSCAN[] = {
    {"img", IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true, NULL, "要扫描的分区起始 LBA", chk_lba},
};
typedef struct { int n; bool win; } scan_acc_t;
static void scan_count_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    (void)is_dir; (void)size;
    scan_acc_t *a=(scan_acc_t*)ud;
    if(a->n < 1000000) a->n++;
    if(name && !strcasecmp(name,"Windows")) a->win = true;
}
static int cmd_part_scan(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_result_u64(out,"lba",lba);
    img_result_u64(out,"bytes",pb);

    /* 读引导扇区判文件系统（NTFS 在 +3 的 "NTFS    "；FAT 系看 +0x52 / +3）*/
    unsigned char bs[512]={0};
    rc=img_qcow2_read_full(q, lba*512, bs, sizeof bs);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"PART-READ-FAIL",NULL,NULL,NULL); return rc; }
    bool is_ntfs = !memcmp(bs+3,"NTFS    ",8);
    bool is_fat  = !memcmp(bs+0x52,"FAT32   ",8) || (!memcmp(bs+3,"FAT",3) && !is_ntfs);
    uint64_t spc = bs[0x0D];
    if(spc) img_result_u64(out,"cluster_bytes", (uint64_t)spc*512);
    if(!is_ntfs && !is_fat){
        img_result_str(out,"fs","none");                    /* level 0：空白/未格式化 */
        img_result_i64(out,"level",0);
        img_result_bool(out,"has_data",false);
        return IMG_OK;
    }
    scan_acc_t acc={0,false};
    if(is_ntfs && !IMG_WITH_NTFS3G){
        return feat_unavail(out,"libntfs-3g（该分区是 NTFS，需要它才能判口径）",
                            "重新编译时开启 IMG_WITH_NTFS3G 并安装 libntfs-3g-dev");
    }
    if(is_ntfs){
        img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
        if(!fs){ img_result_str(out,"fs","unknown"); img_result_i64(out,"level",-1);
                 img_result_bool(out,"has_data",false); return IMG_OK; }
        int lr=img_ntfs_list_dir(fs,"/",scan_count_cb,&acc);
        uint64_t free_b=0, used_b=0;
        if(img_ntfs_free_bytes(fs,&free_b,&used_b)!=IMG_OK){ free_b=0; used_b=0; }
        char lab[64]=""; if(img_ntfs_label(fs,lab,sizeof lab)!=IMG_OK) lab[0]=0;
        img_ntfs_umount(fs);
        if(lr!=IMG_OK){ img_result_str(out,"fs","unknown"); img_result_i64(out,"level",-1);
                        img_result_bool(out,"has_data",false); return IMG_OK; }
        img_result_str(out,"fs","NTFS");
        if(lab[0]) img_result_str(out,"label",lab);
        img_result_u64(out,"free_bytes",free_b);
        img_result_u64(out,"used_bytes",used_b);
    } else {
        img_fat32_fs *fs=img_fat32_mount(q,lba,false);       /* 只读：扫描不该改盘 */
        if(!fs){ img_result_str(out,"fs","unknown"); img_result_i64(out,"level",-1);
                 img_result_bool(out,"has_data",false); return IMG_OK; }
        img_fat32_list_dir(fs,"/",scan_count_cb,&acc);
        uint64_t free_b=img_fat32_free_bytes(fs);
        char lab[12]={0}; memcpy(lab,bs+0x47,11);            /* BPB 卷标（11 字节，可能 "NO NAME"）*/
        for(int i=0;i<11;i++) if(lab[i]==' ') lab[i]=0;
        img_fat32_unmount(fs);
        img_result_str(out,"fs","FAT32");
        if(lab[0] && strcmp(lab,"NO NAME")) img_result_str(out,"label",lab);
        img_result_u64(out,"free_bytes",free_b);
        img_result_u64(out,"used_bytes", pb>free_b?pb-free_b:0);
    }
    int level = (acc.n==0) ? 1 : 2;                          /* 有卷但空 / 有文件 */
    img_result_i64(out,"level",level);
    img_result_i64(out,"entries_root",acc.n);
    img_result_bool(out,"has_data", level==2);
    img_result_bool(out,"looks_like_windows", acc.win);
    return IMG_OK;
}

/* ================= 命令 6c：ntfs.list（列目录 —— App 拿名单自己决定搬谁/显示什么）====================
 * 层归属：给名单的"事实"能力；不带任何"搬谁"的政策（PARTITION-POLICY §四.2）。*/
static const img_arg A_NTFSLIST[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, false, "/",  "要列的目录（绝对路径，默认根目录）", NULL},
};
#if IMG_WITH_NTFS3G
#define NTFS_LIST_MAX 4096
typedef struct { char *b; size_t cap, len; int n; bool truncated; } list_buf_t;
static void list_json_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    list_buf_t *a=(list_buf_t*)ud;
    if(a->n >= NTFS_LIST_MAX){ a->truncated=true; return; }
    char item[640];
    /* name 里可能有引号/反斜杠 → 手写转义（与日志同一套规则）*/
    int k=snprintf(item,sizeof item,"%s{\"name\":\"", a->n?",":"");
    for(const unsigned char *p=(const unsigned char*)name; *p && k<(int)sizeof item-32; p++){
        if(*p=='"'||*p=='\\') item[k++]='\\', item[k++]=(char)*p;
        else if(*p<0x20) k+=snprintf(item+k,sizeof item-(size_t)k,"\\u%04x",*p);
        else item[k++]=(char)*p;
    }
    k+=snprintf(item+k,sizeof item-(size_t)k,"\",\"dir\":%s,\"size\":%llu}",
                is_dir?"true":"false",(unsigned long long)size);
    if(a->len+(size_t)k+2 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=(size_t)k; a->n++; }
    else a->truncated=true;
}
#endif /* IMG_WITH_NTFS3G */
static int cmd_ntfs_list(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 目录枚举）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G

    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path"); if(!path||!*path) path="/";
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL);
             return IMG_ESTATE; }
    static char jbuf[256*1024];
    list_buf_t acc={jbuf,sizeof jbuf,0,0,false};
    rc=img_ntfs_list_dir(fs,path,list_json_cb,&acc);
    img_ntfs_umount(fs);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,"目录读不出（路径不对或卷损坏）",NULL); return rc; }
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"truncated",acc.truncated);
    if(!acc.len){ memcpy(jbuf,"[]",2); acc.len=2; }
    else{                                             /* ★ 补方括号（裸项拼接不是合法 JSON —— 实测踩过）*/
        if(acc.len+2 >= (int)sizeof jbuf) acc.len=(int)sizeof jbuf-3;
        memmove(jbuf+1,jbuf,acc.len);
        jbuf[0]='['; jbuf[acc.len+1]=']'; acc.len+=2; jbuf[acc.len]=0;
    }
    img_result_raw(out,"names",jbuf);
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}

/* ================= 命令 6d：ntfs.rename（搬移原语 —— 把【给定名单】移进目标目录）====================
 * ★ 层归属：机制不是政策。搬谁 = App 传名单（通常来自 ntfs.list / part.scan）；
 *   目标目录名必填、工具不设默认（默认名归 App，PARTITION-POLICY #4）。
 * 机制细节：目标名冲突自动编号 .001/.002…（与 Windows 的 Windows.old.001 行为一致）。*/
static const img_arg A_NTFSREN[] = {
    {"img",   IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba",   IMG_A_U64, true, NULL, "NTFS 分区起始 LBA", chk_lba},
    {"dst",   IMG_A_STR, true, NULL, "目标目录名（如 Windows.old；冲突自动编号）", NULL},
    {"names", IMG_A_STR, true, NULL, "要搬动的顶层名，逗号分隔（如 Windows,Program Files,Users）", NULL},
};
static int cmd_ntfs_rename(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 搬移）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G

    uint64_t lba=img_args_u64(in,"lba",0);
    const char *dst=img_args_get(in,"dst"), *names=img_args_get(in,"names");
    if(!dst||!dst[0]||dst[0]!='/'){ /* 允许给裸名，补成根路径 */
        static char dstp[256];
        if(!dst||!dst[0]||strchr(dst,'/')==NULL){
            snprintf(dstp,sizeof dstp,"/%s",(dst&&dst[0])?dst:"");
            dst=dstp;
        } else { img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst","路径必须以 / 开头且不带尾斜杠",NULL); return IMG_EINVAL; }
    }
    if(!names||!names[0]){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","names","名单不能为空",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",dst,"不是合法 NTFS（未格式化或 LBA 不对）",NULL);
             return IMG_ESTATE; }
    /* 预检：名单里的顶层名必须存在 —— 否则给专属错误码（"名字不存在"≠"搬移失败"，
     * 之前混在一起，排查时分不出"打错字"和"卷坏了"）*/
    { static char nbuf0[64*208]; snprintf(nbuf0,sizeof nbuf0,"%s",names);
      for(char *tok=strtok(nbuf0,","); tok; tok=strtok(NULL,",")){
        while(*tok==' ') tok++;
        if(!*tok) continue;
        char full[256]; snprintf(full,sizeof full,"/%s",tok);
        if(!img_ntfs_path_exists(fs,full)){
            img_ntfs_umount(fs);
            img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",full,"这个顶层名不在根目录里（可能已被搬走或拼错）",NULL);
            return IMG_ENOENT;
        }
      } }
    /* 目标名冲突 → 自动编号 .001/.002…（机制，不是政策）*/
    static char final[256]; snprintf(final,sizeof final,"%s",dst);
    if(img_ntfs_path_exists(fs,final)){
        bool found=false;
        for(int i=1;i<=999;i++){
            snprintf(final,sizeof final,"%.250s.%03d",dst,i);
            if(!img_ntfs_path_exists(fs,final)){ found=true; break; }
        }
        if(!found){ img_ntfs_umount(fs);
                    img_result_fail(out,IMG_EFULL,"CMD-ARG-BAD","dst","编号到 .999 仍冲突",NULL); return IMG_EFULL; }
    }
    /* 名单：逗号分隔 → 数组（上限 64 项，每项 ≤200 字节）*/
    static char nbuf[64*208]; const char *list[64]; int nn=0;
    snprintf(nbuf,sizeof nbuf,"%s",names);
    for(char *tok=strtok(nbuf,","); tok && nn<64; tok=strtok(NULL,",")){
        while(*tok==' ') tok++;
        if(!*tok) continue;
        list[nn++]=tok;
    }
    if(!nn){ img_ntfs_umount(fs);
             img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","names","名单解析后为空",NULL); return IMG_EINVAL; }
    int moved=0;
    rc=img_ntfs_move_into(fs,final,list,nn,&moved);
    img_ntfs_umount(fs);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-RM-FAIL",final,"搬移失败（名字不存在/只读/卷损坏）",NULL); return rc; }
    img_result_u64(out,"moved",(uint64_t)moved);
    img_result_u64(out,"requested",(uint64_t)nn);
    img_result_str(out,"dir_used",final);
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}

/* ================= 命令 6e：img.clone（克隆保留分区到新盘 —— 换系统的主路径）====================
 * 文件名流程（PARTITION-POLICY #10 + 两条硬护栏）：
 *   X.qcow2 ─①改名─▶ X.old.qcow2（App 可传 old_name） ─②写新─▶ 新镜像占原名 X.qcow2
 *            ─③校验─▶ 成功：按 keep_old 决定旧副本删/留（默认删 = #9）
 *                    失败：新盘删掉、旧镜像【改回原名】（护栏 2：用户文件永远在原路径）
 *   ★ 校验通过前绝不删旧的（护栏 1）；宿主空间不够 → 零操作 + HOST-ENOSPC（#12）*/
static const img_arg A_IMGCLONE[] = {
    {"img",      IMG_A_STR,  true,  NULL,     "要克隆的镜像路径（qcow2）", NULL},
    {"keep",     IMG_A_STR,  true,  NULL,     "要保留的分区起始 LBA，逗号分隔（不列的分区在新盘上留空）", NULL},
    {"guid",     IMG_A_ENUM, false, "random", "GUID：random=重新生成（默认）|keep=逐字节相同", NULL},
    {"keep_old", IMG_A_BOOL, false, "false",  "校验通过后是否保留旧镜像副本（默认不保留）", NULL},
    {"old_name", IMG_A_STR,  false, NULL,     "旧镜像改名后的名字（默认 <名字>.old.qcow2）", NULL},
};
static int verify_clone(const char *oldp, const char *newp,
                        const uint64_t *keep, int nkeep){
    /* 校验：新盘分区表可读 + 每个保留分区的【头 64KiB】与旧盘逐字节一致 */
    img_io_opts_t o={0};
    img_io *a=img_io_open_path(oldp,&o), *b=img_io_open_path(newp,&o);
    if(!a||!b){ if(a) img_io_close(a); if(b) img_io_close(b); return -1; }
    img_qcow2_opts_t qo={0};
    img_qcow2 *qa=img_qcow2_open(a,&qo), *qb=img_qcow2_open(b,&qo);
    if(!qa||!qb){ if(qa) img_qcow2_close(qa); if(qb) img_qcow2_close(qb);
                  img_io_close(a); img_io_close(b); return -1; }
    img_parttable_t ta,tb;
    int bad=0;
    if(img_parttable_read(qa,&ta,0)!=IMG_OK || img_parttable_read(qb,&tb,0)!=IMG_OK) bad=1;
    enum { VCHK = 64*1024 };
    static unsigned char vb[VCHK], vc[VCHK];
    for(int k=0; k<nkeep && !bad; k++){
        uint64_t lba=keep[k];
        const img_partition_t *pa=NULL;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(ta.parts[i].used && ta.parts[i].first_lba==lba){ pa=&ta.parts[i]; break; }
        if(!pa){ bad=1; break; }
        uint64_t off=pa->first_lba*512u;
        if(img_qcow2_read_full(qa,off,vb,VCHK)!=IMG_OK ||
           img_qcow2_read_full(qb,off,vc,VCHK)!=IMG_OK ||
           memcmp(vb,vc,VCHK)!=0) bad=1;
    }
    img_qcow2_close(qa); img_io_close(a);
    img_qcow2_close(qb); img_io_close(b);
    return bad?-1:0;
}
static int cmd_img_clone(img_ctx *ctx, const img_args *in, img_result *out){
    const char *src=img_ctx_path(ctx);
    const char *guidp=img_args_get(in,"guid");
    bool guid_random = !guidp || !strcmp(guidp,"random");
    bool keep_old = img_args_bool(in,"keep_old",false);

    /* 名单（上限 32，严格数字） */
    const char *klist=img_args_get(in,"keep");
    if(!klist||!klist[0]){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","keep","保留分区名单不能为空",NULL); return IMG_EINVAL; }
    static char kbuf[512]; snprintf(kbuf,sizeof kbuf,"%s",klist);
    uint64_t keep[32]; int nkeep=0;
    for(char *tok=strtok(kbuf,","); tok && nkeep<32; tok=strtok(NULL,",")){
        while(*tok==' ') tok++;
        if(!*tok) continue;
        char *end=NULL; unsigned long long v=strtoull(tok,&end,0);
        if(!end||*end||v==0){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","keep","LBA 必须是纯数字",NULL); return IMG_EINVAL; }
        keep[nkeep++]=(uint64_t)v;
    }
    if(!nkeep){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","keep","名单解析后为空",NULL); return IMG_EINVAL; }

    /* 宿主空间预检（不足 → 零操作 + 稳定码，#12） */
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    /* ★ 名单必须【在改名之前】校验：参数错误不该惊动文件系统
     *   （之前坏 LBA 走了"改名→失败→回滚"整条重流程，回滚是对的但纯属折腾） */
    for(int k=0;k<nkeep;k++){
        bool found=false;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==keep[k]){ found=true; break; }
        if(!found){
            char atbuf[32]; snprintf(atbuf,sizeof atbuf,"keep=%llu",(unsigned long long)keep[k]);
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD",atbuf,"这个 LBA 不在分区表里",NULL);
            return IMG_EINVAL;
        }
    }
    uint64_t need=4u*1024*1024;
    for(int k=0;k<nkeep;k++){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==keep[k]){
                need += (t->parts[i].last_lba - t->parts[i].first_lba + 1)*512u; break; }
    }
    need += need/32 + 4u*1024*1024;              /* 约 +3%：元数据/对齐/GPT */
    { char dir[600]; snprintf(dir,sizeof dir,"%s",src);
      char *sl=strrchr(dir,'/'); if(sl) *sl=0; else snprintf(dir,sizeof dir,".");
      uint64_t free_b=img_host_free_bytes_path(dir);
      if(free_b && free_b < need){
          img_result_fail(out,IMG_EFULL,"HOST-ENOSPC",src,
                          "宿主剩余空间不足以克隆（未写入任何字节）","清理宿主磁盘后重试");
          return IMG_EFULL;
      } }

    /* 旧镜像改名目标（默认 X.old.qcow2；存在则 .001/.002…） */
    static char oldp[600];
    const char *on=img_args_get(in,"old_name");
    if(on&&on[0]){
        if(on[0]=='/') snprintf(oldp,sizeof oldp,"%s",on);
        else { const char *sl=strrchr(src,'/'); snprintf(oldp,sizeof oldp,"%.*s/%s",
                  sl?(int)(sl-src):0, sl?src:"", on); if(!sl) snprintf(oldp,sizeof oldp,"./%s",on); }
    } else {
        const char *dot=strrchr(src,'.'); const char *sl=strrchr(src,'/');
        if(dot && (!sl || dot>sl)) snprintf(oldp,sizeof oldp,"%.*s.old%s",(int)(dot-src),src,dot);
        else                       snprintf(oldp,sizeof oldp,"%s.old",src);
    }
    if( access(oldp,F_OK)==0 ){
        char base[600]; snprintf(base,sizeof base,"%s",oldp);   /* 先存基准名再编号（自写自读会重叠）*/
        bool found=false;
        for(int i=1;i<=999;i++){
            snprintf(oldp,sizeof oldp,"%.570s.%03d", base, i);
            if(access(oldp,F_OK)!=0){ found=true; break; }
        }
        if(!found){ img_result_fail(out,IMG_EFULL,"CMD-ARG-BAD","old_name","旧文件编号到 .999 仍冲突",NULL); return IMG_EFULL; }
    }

    /* ① 改名（源全程只读的前提） ② 写新 ③ 校验 ④ keep_old */
    if(rename(src,oldp)!=0){ img_result_fail(out,IMG_EIO,"IMG-OPEN-FAIL",src,"旧镜像改名失败（权限/占用）",NULL); return IMG_EIO; }
    img_clone_stats_t st;
    rc=img_clone(oldp, src, keep, nkeep, guid_random, &st, img_cmd_prog());
    if(rc!=IMG_OK){
        unlink(src); rename(oldp,src);              /* ★ 回滚：用户文件回到原路径 */
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":
                         (rc==IMG_EFULL)?"HOST-ENOSPC":"CLONE-COPY-FAIL";
        img_result_fail(out,rc,code,src,"克隆失败（已回滚：旧镜像恢复原名）",NULL);
        return rc;
    }
    if(verify_clone(oldp,src,keep,nkeep)!=0){
        unlink(src); rename(oldp,src);              /* ★ 校验不过 → 回滚 */
        img_result_fail(out,IMG_EIO,"CLONE-VERIFY-FAIL",src,
                        "克隆后校验不通过（已回滚：旧镜像恢复原名）","检查旧镜像是否损坏（qemu-img check）");
        return IMG_EIO;
    }
    /* 校验通过 → 才允许动旧文件（护栏 1） */
    if(!keep_old) unlink(oldp);
    img_result_u64(out,"bytes_written",st.bytes_written);
    img_result_u64(out,"parts_copied",(uint64_t)st.parts_copied);
    img_result_bool(out,"old_kept",keep_old);
    img_result_str(out,"old_path",keep_old?oldp:"");
    return IMG_OK;
}

/* ================= 命令 6f：winre.auto（WinRE 出厂自动启用脚本）====================
 * 背景（用户实测 + 官方查证）：离线拼的盘里 ReAgent.xml 是禁用态、OS 条目缺
 * recoverysequence → reagentc /info 显示 Disabled，要用户手动 reagentc /enable。
 * ★ 官方机制：reagentc /enable 会写 ReAgent.xml + BCD 恢复序列，且支持 offline；
 *   但它会按【目标机器实际分区号】写 GLOBALROOT 路径 —— 我们在镜像里写死分区号会换机失效。
 *   所以走"首次开机自动启用"：往系统分区放 SetupComplete.cmd（Windows 部署的官方脚本位），
 *   首次启动时以 SYSTEM 权限自动跑 reagentc /enable —— 官方工具自己探测+自己写，跨机器安全。
 * ★ 层归属：机制不是政策 —— 默认写（enable=true），App 可传 false 删除脚本。*/
static const img_arg A_WINRE_AUTO[] = {
    {"img",    IMG_A_STR, true,  NULL,   "镜像路径（qcow2）", NULL},
    {"lba",    IMG_A_U64, true,  NULL,   "Windows 系统分区（NTFS）起始 LBA", chk_lba},
    {"enable", IMG_A_BOOL,false, "true", "true=写开机自动启用脚本；false=删除它", NULL},
};
#if IMG_WITH_NTFS3G
/* 内容必须是 ASCII + CRLF（cmd.exe 按 OEM/ANSI 读取；带 BOM 或 UTF-8 中文会乱） */
static const char WINRE_SCRIPT[] =
    "@echo off\r\n"
    "rem imgtool: auto-enable Windows RE on first boot (official mechanism: reagentc writes\r\n"
    "rem          ReAgent.xml + BCD recoverysequence for THIS machine's partition layout)\r\n"
    "reagentc /enable\r\n"
    "reagentc /info > \"%SystemRoot%\\Temp\\winre-enable.log\" 2>&1\r\n"
    "rem one-shot: delete this script after it runs (goto aborts the batch first)\r\n"
    "(goto) 2>nul & del /f /q \"%~f0\"\r\n"
    "exit /b 0\r\n";
#endif /* IMG_WITH_NTFS3G */
static int cmd_winre_auto(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（WinRE 自启用脚本）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G

    uint64_t lba=img_args_u64(in,"lba",0);
    bool enable = img_args_bool(in,"enable",true);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","/Windows","系统分区不是合法 NTFS",NULL); return IMG_ESTATE; }
    const char *path="/Windows/Setup/Scripts/SetupComplete.cmd";
    if(enable){
        img_span sp;
        img_span_prep(&sp,"winre","write","写入 WinRE 自启用脚本");
        img_span_kv_str(&sp,"path",path);
        img_span_begin(&sp);
        img_ntfs_mkdir_p(fs,"/Windows/Setup/Scripts");   /* 目录不存在则建（默认不存在）*/
        rc=img_ntfs_put_file_mem(fs,WINRE_SCRIPT,sizeof WINRE_SCRIPT-1,path);
        img_span_end(&sp,rc);
        if(rc){ img_ntfs_umount(fs);
                img_result_fail(out,rc,"FAT-PUT-FAIL",path,"脚本写入失败（卷只读/空间不足）",NULL); return rc; }
        img_result_bool(out,"written",true);
        img_result_u64(out,"bytes",sizeof WINRE_SCRIPT-1);
    } else {
        if(img_ntfs_path_exists(fs,path)){
            rc=img_ntfs_rm(fs,path);
            if(rc){ img_ntfs_umount(fs);
                    img_result_fail(out,rc,"FAT-RM-FAIL",path,"删除脚本失败",NULL); return rc; }
        }
        img_result_bool(out,"removed",true);
    }
    img_ntfs_umount(fs);
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}

/* ================= 命令 6g：ntfs.exists / fat.exists（断点续跑的探测原语）====================
 * 背景（用户选了"步骤级续跑"）：App 断电重启后要判断"这一步做完没有"——
 *   例：WIM 灌完了吗？→ 看 \Windows\System32\ntoskrnl.exe 在不在
 *       ESP 写完了吗？→ 看 \EFI\Boot\bootx64.efi 在不在
 * ★ 层归属：只给"在不在"的事实；判定规则（哪一步看哪个路径）是 App 的编排知识。
 * ★ 提醒：exists=true 只说明"入口在"，不代表该步骤 100% 完整（如 WIM 灌到一半断电，
 *   ntoskrnl.exe 可能还没写到）。要更强保证用 img_bootcheck 自检 或 看日志最后一行。*/
static const img_arg A_NTFSEXISTS[] = {
    {"img", IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path",IMG_A_STR, true,  NULL, "要查的绝对路径（如 /Windows/System32/ntoskrnl.exe）", NULL},
};
static int cmd_ntfs_exists(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 存在性探测）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G

    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是以 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    bool ex=img_ntfs_path_exists(fs,path);
    uint64_t sz=0; bool has_sz = ex && img_ntfs_file_size(fs,path,&sz)==IMG_OK;
    img_ntfs_umount(fs);
    img_result_str(out,"path",path);
    img_result_bool(out,"exists",ex);
    if(has_sz) img_result_u64(out,"size_bytes",sz);   /* 文件大小：比"在不在"更强一步的判据 */
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}
static const img_arg A_FATEXISTS[] = {
    {"img", IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true,  NULL, "FAT32 分区起始 LBA", chk_lba},
    {"path",IMG_A_STR, true,  NULL, "要查的绝对路径（如 /EFI/Boot/bootx64.efi）", NULL},
};
static int cmd_fat_exists(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是以 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,false);      /* 只读：探测不该改盘 */
    if(!fs){ img_result_fail(out,IMG_ESTATE,"FAT-MOUNT-FAIL",path,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    bool ex=img_fat32_path_exists(fs,path);
    uint64_t sz=0;
    bool has_sz = ex && img_fat32_file_size(fs,path,&sz)==IMG_OK;
    img_fat32_unmount(fs);
    img_result_str(out,"path",path);
    img_result_bool(out,"exists",ex);
    if(has_sz) img_result_u64(out,"size_bytes",sz);
    return IMG_OK;
}

/* ================= 命令 6h：part.copy（跨镜像扇区级拷贝分区 —— GPT→MBR 迁移的搬运工）====================
 * 场景：用户给一块能启动的 GPT 盘，要把它的【系统分区内容】搬到新 MBR 盘的某个分区里。
 * ★ 源只读、目标稀疏（全零块跳过）、1MiB 分块（不整分区进内存）。
 * ★ 层归属：机制不是政策 —— 拷哪个分区、放哪，是 App/流水线的事。*/
static const img_arg A_PARTCOPY[] = {
    {"img",       IMG_A_STR, true,  NULL, "目标镜像路径（qcow2，已建好布局）", NULL},
    {"src",       IMG_A_STR, true,  NULL, "源镜像路径（只读打开，绝不修改）", NULL},
    {"src-lba",   IMG_A_U64, true,  NULL, "源分区起始 LBA", chk_lba},
    {"dst-lba",   IMG_A_U64, true,  NULL, "目标分区起始 LBA", chk_lba},
    {"size-bytes",IMG_A_U64, false, NULL, "拷贝字节数（缺省=min(源分区,目标分区)大小）", NULL},
};
static int cmd_part_copy(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t slba=img_args_u64(in,"src-lba",0), dlba=img_args_u64(in,"dst-lba",0);
    const char *src_path=img_args_get(in,"src");
    img_qcow2 *dq=img_ctx_qcow2(ctx);
    if(!dq){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *dt=NULL; int rc=img_ctx_parttable(ctx,&dt);
    if(rc||!dt) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    img_parttable_t st; {   /* 源：只读打开（★ 全程不写源，是这类迁移安全性的根）*/
        img_io_opts_t o={0};
        img_io *sfi=img_io_open_path(src_path,&o);
        if(!sfi){ img_result_fail(out,IMG_ENOENT,"IMG-OPEN-FAIL",src_path,"源镜像打不开（路径/权限）",NULL); return IMG_ENOENT; }
        img_qcow2_opts_t qo={0};
        img_qcow2 *sq=img_qcow2_open(sfi,&qo);
        if(!sq){ img_io_close(sfi); img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",src_path,"源不是合法 qcow2",NULL); return IMG_EFORMAT; }
        rc=img_parttable_read(sq,&st,0);
        if(rc==IMG_OK){
            img_partition_t *sp=NULL;
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(st.parts[i].used && st.parts[i].first_lba==slba){ sp=&st.parts[i]; break; }
            img_partition_t *dp=NULL;
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(dt->parts[i].used && dt->parts[i].first_lba==dlba){ dp=&dt->parts[i]; break; }
            if(!sp||!dp){
                rc=IMG_EINVAL;
                img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD", !sp?"src-lba":"dst-lba",
                                 "该 LBA 不在对应盘的分区表里",NULL);
            } else {
                uint64_t sbytes=(sp->last_lba - sp->first_lba + 1)*512u;
                uint64_t dbytes=(dp->last_lba - dp->first_lba + 1)*512u;
                uint64_t n=img_args_u64(in,"size-bytes",0);
                if(!n || n>sbytes) n=sbytes;
                if(n>dbytes){
                    rc=IMG_EINVAL;
                    img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","size-bytes",
                                     "目标分区装不下这么大的内容（要扩分区/重排布局）",NULL);
                }
                if(rc==IMG_OK){
                    unsigned char *buf=malloc(1024*1024), *zero=calloc(1,1024*1024);
                    if(!buf||!zero){ rc=IMG_ENOMEM; }
                    else{
                        img_span sp2;
                        img_span_prep(&sp2,"part","copy","跨镜像拷贝分区");
                        img_span_kv_u64(&sp2,"src_lba",slba);
                        img_span_kv_u64(&sp2,"dst_lba",dlba);
                        img_span_kv_u64(&sp2,"size_bytes",n);
                        img_span_begin(&sp2);
                        uint64_t done=0, written=0;
                        while(done<n && rc==IMG_OK){
                            size_t k=(size_t)((n-done)<(1024*1024)?(n-done):(1024*1024));
                            rc=img_qcow2_read_full(sq, slba*512u+done, buf, k);
                            if(rc==IMG_OK && memcmp(buf,zero,k)!=0){   /* ★ 全零跳过（保持稀疏）*/
                                rc=img_qcow2_write(dq, dlba*512u+done, buf, k);
                                if(rc==IMG_OK) written+=k;
                            }
                            done+=k;
                        }
                        img_span_end(&sp2,rc);
                        if(rc==IMG_OK){
                            img_result_u64(out,"bytes_copied",done);
                            img_result_u64(out,"bytes_written",written);  /* 跳零后实际落盘 */
                        }
                    }
                    free(buf); free(zero);
                }
            }
        }
        img_qcow2_close(sq); img_io_close(sfi);
    }
    if(rc!=IMG_OK && out->ok) img_result_fail(out,rc,"PART-COPY-FAIL",NULL,"跨镜像拷贝失败",NULL);
    return rc;
}

/* ================= 命令 6i：part.mbrboot（写 MBR 引导码 + 磁盘签名）====================
 * MBR 的前 440 字节是 BIOS 要执行的引导码；空码 = "Operating System not found"。
 * 引导码内容来自【参考件】（如 reference/mbr-code-bios.bin，从真装好 Windows 的 MBR 盘提取），
 * 与 BCD 参考件同一套"工具不硬编码二进制内容"的规矩。*/
static const img_arg A_MBRBOOT[] = {
    {"img",   IMG_A_STR, true,  NULL, "镜像（MBR 分区表）", NULL},
    {"code",  IMG_A_STR, true,  NULL, "引导码文件（440 字节；如 reference/mbr-code-bios.bin）", NULL},
    {"sig",   IMG_A_STR, false, NULL, "磁盘签名（8 位十六进制；缺省随机生成）", NULL},
};
static int cmd_mbrboot(img_ctx *ctx, const img_args *in, img_result *out){
    const char *code_path=img_args_get(in,"code");
    FILE *f=fopen(code_path,"rb");
    if(!f){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",code_path,"引导码文件打不开",NULL); return IMG_ENOENT; }
    unsigned char code[512]={0};
    size_t got=fread(code,1,sizeof code,f); fclose(f);
    if(got<440){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","code","引导码文件至少要 440 字节",NULL); return IMG_EINVAL; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    if(t->table!=IMG_TABLE_MBR){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","img","MBR 引导码只能写在 MBR 盘上（当前是 GPT）",NULL);
        return IMG_EINVAL;
    }
    memcpy(t->mbr_code, code, 446);          /* 前 446 字节整体（引导码 + 签名位留给我们填）*/
    const char *sig_s=img_args_get(in,"sig");
    uint32_t sig;
    if(sig_s&&sig_s[0]) sig=(uint32_t)strtoul(sig_s,NULL,16);
    else { FILE *ur=fopen("/dev/urandom","rb"); sig=0; if(ur){ if(fread(&sig,1,4,ur)!=4) sig=0; fclose(ur);} if(!sig) sig=0x11223344; }
    t->disk_signature = sig;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    rc=img_parttable_write(q,t);
    if(rc){ img_result_fail(out,rc,"PART-WRITE-FAIL",NULL,"写回分区表失败",NULL); return rc; }
    img_result_u64(out,"disk_signature",sig);
    img_result_bool(out,"written",true);
    return IMG_OK;
}

/* ================= 命令 6j：part.layout（BIOS/MBR 标准布局生成器）====================
 * 产出：系统保留(活动,0x07) → 系统(0x07) → 恢复(0x27) → 扩展分区(0x0F, 含一个逻辑分区)
 * ★ 层归属：这是"机制"——尺寸/名字由参数给；默认值只是 CLI 兜底（App 显式传）。
 * ★ EBR：img_parttable_write 负责写 EBR 链；读表时自动跟链补全逻辑分区。*/
static const img_arg A_PARTLAYOUT[] = {
    {"img",       IMG_A_STR,  true,  NULL,  "镜像路径（qcow2，会被建/重写分区表）", NULL},
    {"sr-mb",     IMG_A_U64,  false, "100", "系统保留分区大小（MiB，活动）", NULL},
    {"sys-gb",    IMG_A_U64,  true,  NULL,  "系统分区大小（GiB）", NULL},
    {"rec-gb",    IMG_A_U64,  false, "0",   "恢复分区大小（GiB，0=最小布局：只建 系统保留+系统（实验用）", NULL},
    {"logical-name", IMG_A_STR, false, "Data", "扩展分区内逻辑分区的卷标", NULL},
    {"extended", IMG_A_BOOL, false, "false", "没有恢复分区时也建扩展+逻辑分区（单变量实验用）", NULL},
    {"single", IMG_A_BOOL, false, "false", "单分区模式：整盘一个活动系统分区（bootmgr/BCD 放它里面）", NULL},
{"part-guid", IMG_A_STR, false, NULL, "自定义分区唯一GUID：序号:GUID[,序号:GUID]（1起；非法→GUID-INVALID-BAD）", NULL},
    {"disk-guid", IMG_A_STR, false, NULL, "自定义 GPT 盘 GUID（非法→GUID-INVALID-BAD）", NULL},
};
static int apply_custom_guids(const img_args *in, img_parttable_t *t);
static int cmd_part_layout(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    uint64_t disk_sectors = img_qcow2_size(q)/512u;
    uint64_t sr_mb=img_args_u64(in,"sr-mb",100), sys_gb=img_args_u64(in,"sys-gb",0),
             rec_gb=img_args_u64(in,"rec-gb",0);
    if(!sys_gb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sys-gb","系统分区大小必填",NULL); return IMG_EINVAL; }
    const uint64_t MiB=1024u*1024u, GiB=1024u*1024u*1024u, align=2048;
    uint64_t sr_sec=sr_mb*MiB/512, sys_sec=sys_gb*GiB/512, rec_sec=rec_gb*GiB/512;
    img_parttable_t t; memset(&t,0,sizeof t);
    t.table=IMG_TABLE_MBR; t.disk_sectors=disk_sectors;
    uint64_t off=align; int idx, rc;
    if(img_args_bool(in,"single",false)){
        /* ★ 单分区（最小化实验）：整个盘一个活动 NTFS 分区，bootmgr/BCD 放它里面 */
        if(disk_sectors<=align*2) return img_result_fail(out,IMG_EFULL,"CMD-ARG-BAD","img","盘太小",NULL), IMG_EFULL;
        if((rc=img_partition_add(&t,align,disk_sectors-align,IMG_MBR_NTFS_EXFAT,NULL,"Windows",true,&idx))!=IMG_OK)
            return img_result_fail(out,rc,"CMD-ARG-BAD","img","单分区建不了",NULL), rc;
        { uint32_t sig=0; FILE *ur=fopen("/dev/urandom","rb"); if(ur){ if(fread(&sig,1,4,ur)!=4) sig=0; fclose(ur);} if(!sig) sig=0x22334455;
          t.disk_signature=sig; }
    if(apply_custom_guids(in,&t)!=IMG_OK){ img_result_fail(out,IMG_EINVAL,"GUID-INVALID-BAD","part-guid/disk-guid","GUID 非法或序号越界",NULL); return IMG_EINVAL; }
        rc=img_parttable_write(q,&t);
        if(rc){ img_result_fail(out,rc,"PART-WRITE-FAIL",NULL,"写分区表失败",NULL); return rc; }
        img_result_u64(out,"parts_total",1);
        img_result_str(out,"mode","single");
        return IMG_OK;
    }
    if((rc=img_partition_add(&t,off,sr_sec,IMG_MBR_NTFS_EXFAT,NULL,"System Reserved",true,&idx))!=IMG_OK)
        return img_result_fail(out,rc,"CMD-ARG-BAD","sr-mb","系统保留分区建不了（太小/越界）",NULL), rc;
    off+=sr_sec;
    if((rc=img_partition_add(&t,off,sys_sec,IMG_MBR_NTFS_EXFAT,NULL,"Windows",false,&idx))!=IMG_OK)
        return img_result_fail(out,rc,"CMD-ARG-BAD","sys-gb","系统分区建不了",NULL), rc;
    off+=sys_sec;
    if(rec_gb){
        if((rc=img_partition_add(&t,off,rec_sec,IMG_MBR_RECOVERY,NULL,"Recovery",false,&idx))!=IMG_OK)
            return img_result_fail(out,rc,"CMD-ARG-BAD","rec-gb","恢复分区建不了",NULL), rc;
        off+=rec_sec;
    }
    /* ★ 最小模式（rec-gb=0）：到此为止 = 只有 系统保留+系统（用户要求"一个一个试"的第 0 步）*/
    if(rec_gb || img_args_bool(in,"extended",false)){   /* 有恢复 或 显式 --extended=true（单变量实验第2步）*/
        uint64_t ext_start=(off+align-1)/align*align;
        if(disk_sectors<=ext_start+align*2) return img_result_fail(out,IMG_EFULL,"CMD-ARG-BAD","img","剩余空间不够建扩展分区",NULL), IMG_EFULL;
        if((rc=img_partition_add(&t,ext_start,disk_sectors-ext_start,IMG_MBR_EXTENDED_L,NULL,"Extended",false,&idx))!=IMG_OK)
            return img_result_fail(out,rc,"CMD-ARG-BAD","img","扩展分区建不了",NULL), rc;
        const char *lname=img_args_get(in,"logical-name"); if(!lname||!lname[0]) lname="Data";
        int lidx=img_partition_add_logical(&t, ext_start+align, disk_sectors-ext_start-align,
                                           IMG_MBR_NTFS_EXFAT, lname);
        if(lidx<0){ img_result_fail(out,(img_err_t)lidx,"CMD-ARG-BAD","img","逻辑分区建不了",NULL); return lidx; }
    }
    /* 随机磁盘签名（★ 换新盘不该沿用源盘签名）*/
    uint32_t sig=0; { FILE *ur=fopen("/dev/urandom","rb"); if(ur){ if(fread(&sig,1,4,ur)!=4) sig=0; fclose(ur);} if(!sig) sig=0x22334455; }
    t.disk_signature=sig;
    if(apply_custom_guids(in,&t)!=IMG_OK){ img_result_fail(out,IMG_EINVAL,"GUID-INVALID-BAD","part-guid/disk-guid","GUID 非法或序号越界",NULL); return IMG_EINVAL; }
    rc=img_parttable_write(q,&t);
    if(rc){ img_result_fail(out,rc,"PART-WRITE-FAIL",NULL,"写分区表失败",NULL); return rc; }
    /* ★ parts_total 从表里真数（旧版硬编码 3/5：rec-gb=0 + extended 时会谎报 5 个分区）*/
    uint64_t parts_total=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(t.parts[i].used) parts_total++;
    img_result_u64(out,"disk_signature",sig);
    img_result_u64(out,"parts_total",parts_total);
    return IMG_OK;
}

/* ================= 命令 6：ctl.cancel（请求取消当前操作） =================
 * ★ 用法：另一个线程调用它（或 App 直接调 img_cancel()）。取消是【协作式】的：
 *   正在跑的操作在自己的检查点（每 1MiB/每个目录项前）看到标志就退出，
 *   并且【不写目录项 + 释放已分配簇链】→ FAT 路径不留半成品。
 *   NTFS/wimlib 直灌路径做不到回滚 → 卡片会明确告知"该分区需重做"。*/
static int cmd_ctl_cancel(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx; (void)in;
    img_cancel();
    img_result_bool(out,"cancelled",true);
    img_result_str(out,"note","已置取消标志；正在跑的操作会在下一个检查点退出");
    return IMG_OK;
}

/* ================= 命令 7：cmd.list（自描述：App 可据此自动生成界面） ================= */
static int cmd_cmd_list(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx; (void)in;
    static char buf[16384];
    int n=img_cmd_list_json(buf,sizeof buf);
    if(n<=0){ img_result_fail(out,IMG_EIO,"LIST-FAIL",NULL,"命令表导出失败",NULL); return IMG_EIO; }
    img_result_raw(out,"table",buf);
    return IMG_OK;
}

/* ================= 表 ================= */
/* boot.repair —— BIOS 引导一键修复（诊断→按需重写→复验） */
static const img_arg A_BOOTREPAIR[]={
    {"img", IMG_A_STR, true, NULL, "目标镜像", NULL},
    {"sys-lba", IMG_A_U64, true, NULL, "系统分区起始 LBA", NULL},
    {"sr-lba", IMG_A_U64, false, NULL, "系统保留分区 LBA（缺省=活动分区）", NULL},
    {"sig", IMG_A_STR, false, NULL, "磁盘签名 HEX（缺省=保持现值）", NULL},
    {"mbr-code", IMG_A_STR, false, NULL, "MBR 引导码文件（≥440字节；缺省不动 MBR）", NULL},
    {"vbr-tpl", IMG_A_STR, false, NULL, "VBR 模板 镜像:lba（缺省不动 VBR）", NULL},
    {"vbr-lba", IMG_A_U64, false, NULL, "要写 VBR 的分区 LBA（缺省=活动/SR 分区；BIOS 引导码在活动分区）", NULL},
    {"bcd-ref", IMG_A_STR, false, NULL, "参考 BCD 文件（缺省不动 BCD）", NULL},
};
static int apply_custom_guids(const img_args *in, img_parttable_t *t){
    const char *dg=img_args_get(in,"disk-guid");
    if(dg&&dg[0]){
        if(img_partition_guid_valid(dg)!=IMG_OK) return IMG_EINVAL;
        snprintf(t->disk_guid,sizeof t->disk_guid,"%s",dg);
    }
    const char *pg=img_args_get(in,"part-guid");
    if(pg&&pg[0]){
        char buf[512]; snprintf(buf,sizeof buf,"%s",pg);
        for(char *tok=strtok(buf,",");tok;tok=strtok(NULL,",")){
            char *cl=strchr(tok,':'); if(!cl) return IMG_EINVAL;
            *cl=0; int idx=atoi(tok); const char *g=cl+1;
            if(idx<1||idx>IMG_MAX_PARTITIONS||!t->parts[idx-1].used) return IMG_EINVAL;
            if(img_partition_guid_valid(g)!=IMG_OK) return IMG_EINVAL;
            snprintf(t->parts[idx-1].part_guid,sizeof t->parts[idx-1].part_guid,"%s",g);
        }
    }
    return IMG_OK;
}
static int cmd_boot_repair(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *imgp=img_args_get(in,"img");
    uint64_t sys_lba=img_args_u64(in,"sys-lba",0);
    uint64_t sr_lba =img_args_u64(in,"sr-lba",0);
    uint64_t vbr_lba=img_args_u64(in,"vbr-lba",0);
    const char *sig_s=img_args_get(in,"sig");
    const char *mcode=img_args_get(in,"mbr-code");
    const char *vtpl =img_args_get(in,"vbr-tpl");
    const char *bref =img_args_get(in,"bcd-ref");
    if(!imgp||!sys_lba){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","img/sys-lba","需要 --img 与 --sys-lba",NULL); return IMG_EINVAL; }
    /* ★ 同 img_ctx_open：boot.repair 自己开的镜像也必须"已存在"，不许创建空文件 */
    img_io*f=img_io_open_existing(imgp,true);
    if(!f){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",imgp,"镜像不存在或不是普通文件（本命令不会创建镜像）",NULL); return IMG_ENOENT; }
    img_qcow2_opts_t qo={0}; img_qcow2*q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK){ img_result_fail(out,IMG_EFORMAT,"IMG-FORMAT",imgp,NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t t; memset(&t,0,sizeof t);
    if(img_parttable_read(q,&t,0)!=IMG_OK){ img_result_fail(out,IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL); return IMG_ESTATE; }
    uint64_t sys_b=0,sr_b=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(!t.parts[i].used) continue;
        uint64_t b=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512u;
        if(t.parts[i].first_lba==sys_lba) sys_b=b;
        if(t.parts[i].first_lba==sr_lba)  sr_b=b;
        if(!sr_lba && (t.parts[i].status&0x80)){ sr_lba=t.parts[i].first_lba; sr_b=b; }
    }
    if(!sr_lba && t.parts[0].used){ sr_lba=t.parts[0].first_lba; sr_b=(t.parts[0].last_lba-t.parts[0].first_lba+1)*512u; }
    if(!sys_b){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sys-lba","LBA 不在分区表里",NULL); return IMG_EINVAL; }
    /* BIOS 的 VBR 引导码在【活动/系统保留】分区（bootmgr 所在），不是 OS 分区。
     * 缺省用 sr_lba；显式 --vbr-lba 可覆盖。*/
    if(!vbr_lba) vbr_lba = sr_lba ? sr_lba : sys_lba;
    /* ① 诊断 */
    unsigned char z[512]; int mbr_sig_ok=0,mbr_code_ok=0,vbr_ok=0,bcd_ok=0;
    if(img_qcow2_read(q,0,z,512)==IMG_OK){
        mbr_sig_ok=(z[510]==0x55&&z[511]==0xAA);
        for(int i=0;i<440;i++) if(z[i]){ mbr_code_ok=1; break; }
    }
    if(img_qcow2_read(q,vbr_lba*512u,z,512)==IMG_OK) vbr_ok=(!memcmp(z+3,"NTFS    ",8));
    if(sr_lba&&sr_b){
        img_ntfs_fs *fs=img_ntfs_mount_rw(q,sr_lba,sr_b);
        if(fs){ void *b=NULL; size_t n=0;
                if(img_ntfs_read_file(fs,"/Boot/BCD",&b,&n)==IMG_OK&&n){ bcd_ok=1; free(b); }
                img_ntfs_umount(fs); }
    }
    img_result_bool(out,"diag.mbr_sig_ok",mbr_sig_ok);
    img_result_bool(out,"diag.mbr_code_ok",mbr_code_ok);
    img_result_bool(out,"diag.vbr_ntfs_ok",vbr_ok);
    img_result_bool(out,"diag.bcd_ok",bcd_ok);
    int fixed=0;
    /* ②-1 MBR：写码+55AA+签名（保持表项不动） */
    if(mcode){
        FILE*mf=fopen(mcode,"rb");
        if(!mf){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",mcode,NULL,NULL); return IMG_ENOENT; }
        unsigned char sec[512]={0}; size_t got=fread(sec,1,440,mf); fclose(mf);
        if(got<440){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","mbr-code","码文件不足440字节",NULL); return IMG_EINVAL; }
        if(img_qcow2_read(q,0,z,512)!=IMG_OK){ img_result_fail(out,IMG_EIO,"PART-READ-FAIL",NULL,NULL,NULL); return IMG_EIO; }
        if(sig_s){ uint32_t sg=(uint32_t)strtoul(sig_s,NULL,16); memcpy(z+440,&sg,4); }
        memcpy(z,sec,440); z[510]=0x55; z[511]=0xAA;
        if(img_qcow2_write(q,0,z,512)!=IMG_OK){ img_result_fail(out,IMG_EIO,"PART-WRITE-FAIL",NULL,NULL,NULL); return IMG_EIO; }
        fixed++;
    }
    /* ②-2 VBR：模板引导码（保留自己的 BPB） */
    if(vtpl){
        char buf[512]; snprintf(buf,sizeof buf,"%s",vtpl);
        char *cl=strrchr(buf,':'); if(!cl){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","vbr-tpl","格式 镜像:lba",NULL); return IMG_EINVAL; }
        *cl=0; uint64_t tl=strtoull(cl+1,NULL,0);
        img_io_opts_t io2={0}; img_io*f2=img_io_open_path(buf,&io2);
        img_qcow2_opts_t qo2={0}; img_qcow2*q2=f2?img_qcow2_open(f2,&qo2):NULL;
        if(!q2){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",buf,NULL,NULL); return IMG_ENOENT; }
        /* ★ mkntfs 的 BPB 几何字段是 0；不补，Windows 引导代码算 CHS 会失败
         *   → SeaBIOS "A disk read error occurred"（实测 2026-10-20）。
         *   VBR 分区和 OS 分区都要补（bootmgr 读 OS 也用它）。*/
        int gp=0;
        if(img_ntfs_fix_geometry(q,sys_lba)==IMG_OK) gp++;
        if(vbr_lba!=sys_lba && img_ntfs_fix_geometry(q,vbr_lba)==IMG_OK) gp++;
        img_result_u64(out,"geometry_patched",(uint64_t)gp);
        int rc=img_fixboot(q,vbr_lba,q2,tl,img_cmd_prog());
        if(rc!=IMG_OK){ img_result_fail(out,rc,"BOOT-VBR-WRITE-FAIL",NULL,NULL,NULL); return rc; }
        fixed++;
    }
    /* ②-3 BCD：参考件 → 改命门（签名+分区字节偏移）→ 写回 SR */
    if(bref){
        FILE*bf=fopen(bref,"rb"); if(!bf){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",bref,NULL,NULL); return IMG_ENOENT; }
        fseek(bf,0,SEEK_END); long bn=ftell(bf); fseek(bf,0,SEEK_SET);
        void *bb=malloc(bn); if(!bb||fread(bb,1,bn,bf)!=(size_t)bn){ fclose(bf); free(bb); return IMG_EIO; }
        fclose(bf);
        img_bcd *b2=img_bcd_open_mem(bb,(size_t)bn); free(bb);
        if(!b2){ img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD",bref,NULL,NULL); return IMG_EFORMAT; }
        char ld[96]=""; char g[96]="";
        if(img_bcd_get_display_order_first(b2,ld,sizeof ld)==IMG_OK){
            char *p=ld; if(p[0]=='{') p++;
            snprintf(g,sizeof g,"%s",p); char *c=strchr(g,'}'); if(c)*c=0;
        }
        /* 参考件里的旧值（改之前先记下：改完就读不到了） */
        uint32_t oldsig=0; uint64_t old_os_off=0;
        if(!g[0] || (img_bcd_get_mbr_binding(b2,g,"21000001",&oldsig,&old_os_off)!=IMG_OK &&
                     img_bcd_get_mbr_binding(b2,g,"11000001",&oldsig,&old_os_off)!=IMG_OK)){
            img_bcd_close(b2);
            img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD",bref,"不是 BIOS BCD",NULL); return IMG_EFORMAT; }
        uint32_t old_sig_bm=0; uint64_t old_sr_off=0;
        img_bcd_get_mbr_binding(b2,IMG_BCD_GUID_BOOTMGR,"11000001",&old_sig_bm,&old_sr_off);
        uint32_t newsig=oldsig; if(sig_s) newsig=(uint32_t)strtoul(sig_s,NULL,16);
        uint64_t sys_off=sys_lba*512u, sr_off=sr_lba?sr_lba*512u:0;
        /* ① 全对象扫描：bootmgr/memtest→SR，resume/OSLoader/OSDevice→系统分区，
         *    按【旧偏移→新偏移 + 旧签名→新签名】一次改完（不再文件级裸替换字节） */
        img_bcd_off_map maps[2]; int nmaps=0;
        if(old_os_off && old_os_off!=sys_off){
            maps[nmaps].old_off=old_os_off; maps[nmaps].new_off=sys_off; nmaps++; }
        if(old_sr_off && old_sr_off!=old_os_off && old_sr_off!=sys_off && sr_off){
            maps[nmaps].old_off=old_sr_off; maps[nmaps].new_off=sr_off; nmaps++; }
        int swept=0;
        int prc=img_bcd_patch_all_mbr_bindings(b2,oldsig,newsig,maps,(size_t)nmaps,&swept);
        if(prc!=IMG_OK){ img_bcd_close(b2); img_result_fail(out,prc,"BCD-PATCH-FAIL",bref,NULL,NULL); return prc; }
        /* ② 兜底：即使参考件缺 bootmgr 等元素，语义已知的三处也确保指对（幂等） */
        img_bcd_set_mbr_binding(b2,g,"21000001",newsig,sys_off);
        img_bcd_set_mbr_binding(b2,g,"11000001",newsig,sys_off);
        if(sr_off) img_bcd_set_mbr_binding(b2,IMG_BCD_GUID_BOOTMGR,"11000001",newsig,sr_off);
        img_result_u64(out,"bcd.bindings_patched",(uint64_t)swept);
        void *outb=NULL; size_t outn=0;
        if(img_bcd_save_mem(b2,&outb,&outn)!=IMG_OK){ img_bcd_close(b2); return IMG_EIO; }
        img_bcd_close(b2);
        if(!sr_lba||!sr_b){ free(outb); img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sr-lba","BCD 需要 SR 分区",NULL); return IMG_EINVAL; }
        img_ntfs_fs *fs=img_ntfs_mount_rw(q,sr_lba,sr_b);
        if(!fs){ free(outb); img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",NULL,"SR 不是 NTFS",NULL); return IMG_ESTATE; }
        img_ntfs_mkdir_p(fs,"/Boot");
        int rc=img_ntfs_put_file_mem(fs,outb,outn,"/Boot/BCD");
        img_ntfs_umount(fs); free(outb);
        if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",NULL,NULL,NULL); return rc; }
        fixed++;
    }
    /* ③ 复验 */
    int v_sig=0,v_code=0,v_vbr=0,v_bcd=0;
    if(img_qcow2_read(q,0,z,512)==IMG_OK){ v_sig=(z[510]==0x55&&z[511]==0xAA);
        for(int i=0;i<440;i++) if(z[i]){ v_code=1; break; } }
    if(img_qcow2_read(q,vbr_lba*512u,z,512)==IMG_OK) v_vbr=(!memcmp(z+3,"NTFS    ",8));
    if(sr_lba&&sr_b){ img_ntfs_fs *fs=img_ntfs_mount_rw(q,sr_lba,sr_b);
        if(fs){ void *b=NULL; size_t n=0;
                if(img_ntfs_read_file(fs,"/Boot/BCD",&b,&n)==IMG_OK&&n){ v_bcd=1; free(b); }
                img_ntfs_umount(fs); } }
    img_result_bool(out,"verify.mbr_sig_ok",v_sig);
    img_result_bool(out,"verify.mbr_code_ok",v_code);
    img_result_bool(out,"verify.vbr_ntfs_ok",v_vbr);
    img_result_bool(out,"verify.bcd_ok",v_bcd);
    img_result_u64(out,"fixed_count",(uint64_t)fixed);
    img_result_u64(out,"vbr_lba",vbr_lba);
    img_result_bool(out,"ok", v_sig&&v_code&&v_vbr&&v_bcd);
    return IMG_OK;
}


/* ================= 7. 补齐 App 建盘流水线缺的 5 个命令（2026-10-20）=================
 * 背景（手册 §11.0）：命令表原来只有"零件"，建盘/格式化/写 NTFS 文件/UEFI BCD/自检
 * 都只能调表外 CLI，App 通过 JNI 走不通整条流水线。这 5 条把它们搬进表里。
 * ★ 命名规矩：凡是"产出新镜像"的命令，路径参数叫 out（不叫 img）——
 *   img 会被 img_cmd_exec 当作"已存在的镜像"去打开（img_ctx_open 要求存在）。*/

/* ---------- img.create ---------- */
static const img_arg A_IMGCREATE[] = {
    {"out",     IMG_A_STR,  true,  NULL,   "要创建的 qcow2 路径（已存在时需 force=true）", NULL},
    {"size-gb", IMG_A_U64,  true,  NULL,   "虚拟磁盘大小（GiB）", NULL},
    {"table",   IMG_A_ENUM, false, "gpt",  "分区表：gpt(UEFI) | mbr(BIOS)", NULL},
    {"esp-mb",  IMG_A_U64,  false, "200",  "（gpt）ESP 大小 MiB", NULL},
    {"msr-mb",  IMG_A_U64,  false, "16",   "（gpt）MSR 大小 MiB", NULL},
    {"rec-mb",  IMG_A_U64,  false, "0",    "恢复分区 MiB（0=不建；政策默认关）", NULL},
    {"sys-mb",  IMG_A_U64,  false, NULL,   "系统分区 MiB：mbr=活动系统分区（缺省 512）；gpt=Windows 分区大小（缺省=吃满剩余）", NULL},
    {"force",   IMG_A_BOOL, false, "false","已存在则覆盖（★会删掉原文件）", NULL},
};
static int cmd_img_create(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *outp=img_args_get(in,"out");
    uint64_t gb=img_args_u64(in,"size-gb",0);
    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="gpt";
    if(!outp||!*outp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","out","缺少输出路径",NULL); return IMG_EINVAL; }
    if(!gb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","size-gb","必须是正整数（GiB）",NULL); return IMG_EINVAL; }
    bool gpt, mbr;
    gpt=!strcasecmp(tb,"gpt"); mbr=!strcasecmp(tb,"mbr");
    if(!gpt && !mbr){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","table","只接受 gpt | mbr",NULL); return IMG_EINVAL; }
    bool force=img_args_bool(in,"force",false);
    struct stat st;
    if(!force && stat(outp,&st)==0){
        img_result_fail(out,IMG_EEXISTS,"CMD-ARG-BAD","out","输出文件已存在（force=true 才覆盖）",NULL);
        return IMG_EEXISTS;
    }
    const uint64_t MiB=1024u*1024u, GiB=1024u*1024u*1024u;
    uint64_t bytes=gb*GiB;
    if(bytes < 1024ull*MiB){ img_result_fail(out,IMG_ERANGE,"CMD-ARG-BAD","size-gb","盘太小（≥1 GiB）",NULL); return IMG_ERANGE; }

    /* ① 建 qcow2 本体（稀疏：只写头，不占实际空间）*/
    img_io_opts_t io={0}; io.writable=true; io.truncate=true;
    img_io *f=img_io_open_path(outp,&io);
    if(!f){ img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",outp,"创建文件失败（权限/目录不存在）",NULL); return IMG_EIO; }
    img_qcow2_create_opts_t co={0}; co.size=bytes; co.cluster_bits=16;
    int rc=img_qcow2_create(f,&co);
    if(rc!=IMG_OK){ img_io_close(f); unlink(outp);
        img_result_fail(out,rc,"IMG-FORMAT",outp,"qcow2 创建失败",NULL); return rc; }

    /* ② 分区表（复用 img_layout_build，与 imglayout mk 同一份实现）*/
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK){ img_io_close(f); unlink(outp);
        img_result_fail(out,IMG_EFORMAT,"IMG-FORMAT",outp,"qcow2 头写不进去",NULL); return IMG_EFORMAT; }
    img_layout_opts_t lo; img_layout_defaults(&lo, gpt?IMG_TABLE_GPT:IMG_TABLE_MBR);
    /* ★ sys_bytes 的语义【按表不同】（img_layout_build 里写死的）：
     *   MBR = 活动系统分区大小；GPT = Windows 分区大小，0 表示吃满剩余。
     *   默认值交给 img_layout_defaults()，这里只在用户显式给了才覆盖。*/
    lo.esp_bytes      = img_args_u64(in,"esp-mb",200)*MiB;
    lo.msr_bytes      = img_args_u64(in,"msr-mb",16)*MiB;
    lo.recovery_bytes = img_args_u64(in,"rec-mb",0)*MiB;
    { uint64_t sysmb=img_args_u64(in,"sys-mb",0);
      if(sysmb) lo.sys_bytes = sysmb*MiB; }
    img_parttable_t t; img_check_result_t err; memset(&t,0,sizeof t); memset(&err,0,sizeof err);
    rc=img_layout_build(&t, bytes/512u, gpt?IMG_TABLE_GPT:IMG_TABLE_MBR, &lo, &err);
    if(rc!=IMG_OK){
        img_qcow2_close(q); img_io_close(f); unlink(outp);
        img_result_fail(out,rc,"PART-WRITE-FAIL",outp,err.reason[0]?err.reason:"布局生成失败",NULL);
        return rc;
    }
    rc=img_parttable_write(q,&t);
    if(rc!=IMG_OK){ img_qcow2_close(q); img_io_close(f); unlink(outp);
        img_result_fail(out,rc,"PART-WRITE-FAIL",outp,"分区表写不进去",NULL); return rc; }
    img_qcow2_close(q); img_io_close(f);

    /* ③ 结果：把新表原样回给 App（省一次 part.show）*/
    static char pj[64*1024];
    int pn=img_parttable_json(&t,pj,sizeof pj);
    img_result_u64(out,"size_bytes",bytes);
    img_result_str(out,"table",gpt?"GPT":"MBR");
    img_result_u64(out,"parts_total",(uint64_t)t.count);
    if(pn>0) img_result_raw(out,"pt",pj);
    return IMG_OK;
}

/* ---------- fs.format ---------- */
static const img_arg A_FSFORMAT[] = {
    {"img",   IMG_A_STR,  true,  NULL,  "镜像路径（qcow2）", NULL},
    {"lba",   IMG_A_U64,  true,  NULL,  "分区起始 LBA", chk_lba},
    {"fs",    IMG_A_ENUM, false, "ntfs","文件系统：ntfs | fat32", NULL},
    {"label", IMG_A_STR,  false, NULL,  "卷标（NTFS ≤11 ASCII；FAT32 ≤11）", NULL},
    {"quick", IMG_A_BOOL, false, "true","NTFS 快速格式化（false=整盘零填，很慢）", NULL},
};
static int cmd_fs_format(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *fsn=img_args_get(in,"fs"); if(!fsn||!*fsn) fsn="ntfs";
    const char *label=img_args_get(in,"label");
    bool ntfs=!strcasecmp(fsn,"ntfs"), fat=!strcasecmp(fsn,"fat32");
    if(!ntfs && !fat){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","fs","只接受 ntfs | fat32",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    if(ntfs){
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,lba,pb);
        o.label=label; o.force=true;
        o.quick=img_args_bool(in,"quick",true);   /* 默认快速：整盘零填在慢介质上要几十分钟 */
        img_span sp; img_span_prep(&sp,"ntfs","format","格式化 NTFS");
        img_span_kv_u64(&sp,"lba",lba); img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        rc=img_ntfs_format(q,&o,img_cmd_prog());
        img_span_end(&sp,rc);
        if(rc){ img_result_fail(out,rc,"NTFS-FORMAT-FAIL",NULL,NULL,NULL); return rc; }
        img_result_str(out,"fs","NTFS");
    } else {
        img_fat32_opts_t o={0};
        o.partition_lba=lba; o.partition_bytes=pb; o.label=label;
        o.hidden_sectors=(uint32_t)lba; o.num_fats=2; o.active_boot=true;
        img_fat32_layout_t L; memset(&L,0,sizeof L);
        img_span sp; img_span_prep(&sp,"fat32","format","格式化 FAT32");
        img_span_kv_u64(&sp,"lba",lba); img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        rc=img_fat32_format(q,&o,&L,img_cmd_prog());
        img_span_end(&sp,rc);
        if(rc){ img_result_fail(out,rc,"FAT-PUT-FAIL",NULL,"FAT32 格式化失败",NULL); return rc; }
        img_result_str(out,"fs","FAT32");
        img_result_u64(out,"cluster_bytes",(uint64_t)L.sectors_per_cluster*512u);
        img_result_u64(out,"cluster_count",L.cluster_count);
    }
    img_result_u64(out,"lba",lba);
    img_result_u64(out,"bytes",pb);
    if(label) img_result_str(out,"label",label);
    return IMG_OK;
}

/* ---------- ntfs.put / ntfs.export（往 NTFS 读写本地文件）---------- */
static const img_arg A_NTFSPUT[] = {
    {"img", IMG_A_STR, true,  NULL,   "镜像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true,  NULL,   "NTFS 分区起始 LBA", chk_lba},
    {"src", IMG_A_STR, true,  NULL,   "本地源文件路径", NULL},
    {"dst", IMG_A_STR, true,  NULL,   "分区内目标绝对路径（/ 开头）", NULL},
    {"mkdir", IMG_A_BOOL,false,"true","父目录不存在时自动创建", NULL},
};
static int cmd_ntfs_put(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 写入）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *src=img_args_get(in,"src"), *dst=img_args_get(in,"dst");
    bool mk=img_args_bool(in,"mkdir",true);
    if(!src||!dst||dst[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst","需要 src 与以 / 开头的 dst",NULL); return IMG_EINVAL; }
    struct stat st;
    if(stat(src,&st)!=0 || !S_ISREG(st.st_mode)){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",src,NULL,NULL); return IMG_ENOENT; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",dst,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    if(mk){
        char parent[512]; snprintf(parent,sizeof parent,"%s",dst);
        char *sl=strrchr(parent,'/');
        if(sl && sl!=parent){ *sl=0; img_ntfs_mkdir_p(fs,parent); }
    }
    img_span sp; img_span_prep(&sp,"ntfs","put","写入文件");
    img_span_kv_str(&sp,"path",dst); img_span_kv_u64(&sp,"size_bytes",(uint64_t)st.st_size);
    img_span_begin(&sp);
    rc=img_ntfs_put_file(fs,src,dst,img_cmd_prog());
    img_span_end(&sp,rc);
    img_ntfs_umount(fs);
    if(rc){ img_result_fail(out,rc,"FAT-PUT-FAIL",dst,"写入失败（空间/只读/路径）",NULL); return rc; }
    img_result_str(out,"path",dst);
    img_result_u64(out,"written_bytes",(uint64_t)st.st_size);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}
static const img_arg A_NTFSEXPORT[] = {
    {"img",  IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true, NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true, NULL, "分区内要导出的绝对路径（/ 开头）", NULL},
    {"dst",  IMG_A_STR, true, NULL, "本地输出文件路径（会覆盖）", NULL},
};
static int cmd_ntfs_export(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 读取）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    if(!path||path[0]!='/'||!dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","需要 / 开头的 path 与 dst",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    void *buf=NULL; size_t n=0;
    rc=img_ntfs_read_file(fs,path,&buf,&n);
    img_ntfs_umount(fs);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-ENTRY-NOTFOUND",path,"文件不存在或读不出",NULL); return rc; }
    FILE *o=fopen(dst,"wb");
    if(!o){ free(buf); img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",dst,"本地文件写不了",NULL); return IMG_EIO; }
    size_t wr=fwrite(buf,1,n,o); fclose(o); free(buf);
    if(wr!=n){ img_result_fail(out,IMG_EIO,"HOST-ENOSPC",dst,"本地写入不完整",NULL); return IMG_EIO; }
    img_result_str(out,"path",path);
    img_result_u64(out,"bytes",(uint64_t)n);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}

/* ---------- bcd.gen-uefi（= imggenbcd，同一份库实现）---------- */
static const img_arg A_BCDGEN[] = {
    {"ref",       IMG_A_STR, true,  NULL, "参考 BCD（取自一张能启动的同代 UEFI 盘）", NULL},
    {"out",       IMG_A_STR, true,  NULL, "输出 BCD 文件路径", NULL},
    {"sys-guid",  IMG_A_STR, true,  NULL, "新盘系统分区 partGuid", NULL},
    {"disk-guid", IMG_A_STR, true,  NULL, "新盘 diskGuid", NULL},
    {"esp-guid",  IMG_A_STR, false, NULL, "新盘 ESP partGuid（不给=只映射系统分区）", NULL},
    {"old-rec-guid", IMG_A_STR, false, NULL, "参考件里的恢复分区 GUID（可选）", NULL},
    {"new-rec-guid", IMG_A_STR, false, NULL, "新盘的恢复分区 GUID（可选）", NULL},
};
static int cmd_bcd_gen_uefi(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *ref=img_args_get(in,"ref"), *outp=img_args_get(in,"out");
    if(!ref||!outp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","ref/out","需要参考 BCD 与输出路径",NULL); return IMG_EINVAL; }
    img_bcd *b=img_bcd_open_file(ref);
    if(!b){ img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD",ref,"打不开或不是合法 hive",NULL); return IMG_EFORMAT; }
    img_bcd_remap_req_t rq;
    rq.sys_guid=img_args_get(in,"sys-guid");
    rq.disk_guid=img_args_get(in,"disk-guid");
    rq.esp_guid=img_args_get(in,"esp-guid");
    rq.old_rec_guid=img_args_get(in,"old-rec-guid");
    rq.new_rec_guid=img_args_get(in,"new-rec-guid");
    char err[256]=""; int hits=0;
    int rc=img_bcd_remap(b,&rq,&hits,err,sizeof err);
    if(rc!=IMG_OK){ img_bcd_close(b);
        img_result_fail(out,rc,"BCD-PATCH-FAIL",ref,err[0]?err:"换盘映射失败",NULL); return rc; }
    rc=img_bcd_save(b,outp);
    img_bcd_close(b);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",outp,"输出文件写不了",NULL); return rc; }
    img_result_u64(out,"bindings_patched",(uint64_t)hits);
    img_result_str(out,"out",outp);
    return IMG_OK;
}

/* ---------- boot.check（= imgbc，但走命令表/JSON）---------- */
static const img_arg A_BOOTCHECK[] = {
    {"img",  IMG_A_STR,  true,  NULL,    "镜像路径（qcow2）", NULL},
    {"mode", IMG_A_ENUM, false, "auto",  "引导模式：auto | uefi | bios", NULL},
    {"expect-recovery", IMG_A_BOOL, false, "false", "要求有恢复分区（默认不要求）", NULL},
};
static int cmd_boot_check(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（引导链自检）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    const char *m=img_args_get(in,"mode"); if(!m||!*m) m="auto";
    img_boot_mode_t mode=IMG_BOOT_AUTO;
    if(!strcasecmp(m,"uefi")) mode=IMG_BOOT_UEFI;
    else if(!strcasecmp(m,"bios")) mode=IMG_BOOT_BIOS;
    else if(strcasecmp(m,"auto")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","mode","只接受 auto | uefi | bios",NULL); return IMG_EINVAL; }
    unsigned flags = img_args_bool(in,"expect-recovery",false) ? IMG_BOOTCHK_EXPECT_RECOVERY : 0;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_bootcheck_result_t r; memset(&r,0,sizeof r);
    img_span sp; img_span_prep(&sp,"bc","check","引导链自检");
    img_span_begin(&sp);
    int rc=img_bootcheck_ex(q,mode,flags,&r,img_cmd_prog());
    img_span_end(&sp,rc);
    if(rc!=IMG_OK && r.count==0){
        img_result_fail(out,rc,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL); return rc;
    }
    static char jb[65536];
    int n=img_bootcheck_json_array(&r,jb,sizeof jb);   /* ★ 必须是数组，不是 NDJSON */
    char sum[256]; img_bootcheck_summary(&r,sum,sizeof sum);
    img_result_str(out,"mode", r.is_uefi?"uefi":"bios");
    img_result_u64(out,"count",(uint64_t)r.count);
    img_result_u64(out,"required_fail",(uint64_t)r.n_required_fail);
    img_result_u64(out,"warn_fail",(uint64_t)r.n_warn_fail);
    img_result_str(out,"summary",sum);
    if(n>0) img_result_raw(out,"items",jb);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}


/* ================= ISO/UDF 抽取（App 只下命令，不自己动文件）=================
 * 背景（DESIGN-OUTPUT.md §十一 层归属）：ISO → sources/install.wim 原来归 App 做，
 * 意味着 App 必须自己找 ISO 库、自己写文件。改成工具做：
 *   iso.ls / iso.extract → App 只剩下"命令权"，读写出问题时排查面收敛到工具日志。*/
typedef struct { char *b; size_t cap, len; int n; bool trunc; } isolist_t;
static void iso_list_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    isolist_t *a=(isolist_t*)ud;
    if(!a || a->n>=4096){ if(a) a->trunc=true; return; }
    char item[640]; int k=snprintf(item,sizeof item,"%s{\"name\":\"",a->n?",":"");
    for(const unsigned char *p=(const unsigned char*)name; p&&*p && k<(int)sizeof item-64; p++){
        if(*p=='"'||*p=='\\') item[k++]='\\', item[k++]=(char)*p;
        else if(*p<0x20) k+=snprintf(item+k,sizeof item-(size_t)k,"\\u%04x",*p);
        else item[k++]=(char)*p;
    }
    k+=snprintf(item+k,sizeof item-(size_t)k,"\",\"dir\":%s,\"size\":%llu}",
                is_dir?"true":"false",(unsigned long long)size);
    if(a->len+(size_t)k+1 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=(size_t)k; a->n++; }
    else a->trunc=true;
}
static const img_arg A_ISOLS[] = {
    {"iso",  IMG_A_STR, true,  NULL, "ISO 文件路径（UDF bridge；不修改它）", NULL},
    {"path", IMG_A_STR, false, "/",  "ISO 内的目录（大小写不敏感）", NULL},
};
static int cmd_iso_ls(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *isop=img_args_get(in,"iso");
    const char *path=img_args_get(in,"path"); if(!path||!*path) path="/";
    char err[256]="";
    img_iso *iso=NULL;
    int rc=img_iso_open_path(&iso,isop,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"HOST-OPEN-FAIL",isop,err[0]?err:"打不开 ISO",NULL); return rc; }
    static char jb[256*1024];
    /* ★ raw 类型必须是【一个完整的 JSON 值】：条目之间只有逗号是不够的，得包成数组 */
    jb[0]='['; isolist_t acc={jb,sizeof jb-2,1,0,false};
    img_span sp; img_span_prep(&sp,"iso","ls","列 ISO 目录");
    img_span_kv_str(&sp,"path",path); img_span_begin(&sp);
    int n=img_iso_list(iso,path,iso_list_cb,&acc);
    img_span_end(&sp, n<0?IMG_ENOENT:IMG_OK);
    const char *vol=img_iso_volume_id(iso);
    img_iso_close(iso);
    if(n<0){ img_result_fail(out,IMG_ENOENT,"FAT-DIR-NOTFOUND",path,"ISO 里没有这个目录",NULL); return IMG_ENOENT; }
    jb[acc.len++]=']'; jb[acc.len]=0;
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"truncated",acc.trunc);
    if(vol) img_result_str(out,"volume",vol);
    img_result_raw(out,"entries",jb);
    return IMG_OK;
}
static const img_arg A_ISOEXTRACT[] = {
    {"iso",  IMG_A_STR, true,  NULL, "ISO 文件路径", NULL},
    {"path", IMG_A_STR, true,  NULL, "ISO 内的文件路径（如 /sources/install.wim）", NULL},
    {"dst",  IMG_A_STR, true,  NULL, "本地输出文件（会覆盖）", NULL},
    {"max-bytes", IMG_A_U64, false, "0", "只抽前 N 字节（0=整份；自测/预览用）", NULL},
};
static int cmd_iso_extract(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *isop=img_args_get(in,"iso");
    const char *path=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    uint64_t maxb=img_args_u64(in,"max-bytes",0);
    if(!isop||!path||!dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","iso/path/dst","三个都必填",NULL); return IMG_EINVAL; }
    char err[256]="";
    img_iso *iso=NULL;
    int rc=img_iso_open_path(&iso,isop,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"HOST-OPEN-FAIL",isop,err[0]?err:"打不开 ISO",NULL); return rc; }
    uint64_t fsz=0; bool isdir=false;
    rc=img_iso_stat(iso,path,&fsz,&isdir);
    if(rc!=IMG_OK){ img_iso_close(iso);
        img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",path,"ISO 里没有这个文件",NULL); return IMG_ENOENT; }
    if(isdir){ img_iso_close(iso);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD",path,"这是个目录，不是文件",NULL); return IMG_EINVAL; }
    img_span sp; img_span_prep(&sp,"iso","extract","从 ISO 抽取文件");
    img_span_kv_str(&sp,"path",path); img_span_kv_u64(&sp,"size_bytes",fsz);
    img_span_begin(&sp);
    uint64_t got=0;
    rc=img_iso_extract(iso,path,dst,maxb,img_cmd_prog(),&got);
    img_span_kv_u64(&sp,"written",got);
    img_span_end(&sp,rc);
    img_iso_close(iso);
    if(rc!=IMG_OK){
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"HOST-ENOSPC";
        img_result_fail(out,rc,code,path,(rc==IMG_ECANCELLED)?"已取消（目标文件已删除）":"抽取失败",NULL);
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_str(out,"dst",dst);
    img_result_u64(out,"bytes",got);
    img_result_u64(out,"size_bytes",fsz);
    return IMG_OK;
}
/* 未编 libudfread 时也要有符号：给表一个明确报不可用的实现 */


/* ---------- fat.export（与 ntfs.export 对称；UEFI 的 BCD 在 ESP 上，是 FAT32）---------- */
static const img_arg A_FATEXPORT[] = {
    {"img",  IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true, NULL, "FAT32 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true, NULL, "分区内要导出的绝对路径（/ 开头）", NULL},
    {"dst",  IMG_A_STR, true, NULL, "本地输出文件路径（会覆盖）", NULL},
};
static int cmd_fat_export(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    if(!path||path[0]!='/'||!dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","需要 / 开头的 path 与 dst",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),NULL,NULL); return IMG_EFORMAT; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,false);      /* 只读：导出不该改盘 */
    if(!fs){ img_result_fail(out,IMG_ESTATE,"FAT-MOUNT-FAIL",path,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    void *buf=NULL; size_t n=0;
    rc=img_fat32_read_file(fs,path,&buf,&n);
    img_fat32_unmount(fs);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-ENTRY-NOTFOUND",path,"文件不存在或读不出",NULL); return rc; }
    FILE *o=fopen(dst,"wb");
    if(!o){ free(buf); img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",dst,"本地文件写不了",NULL); return IMG_EIO; }
    size_t wr=fwrite(buf,1,n,o); fclose(o); free(buf);
    if(wr!=n){ img_result_fail(out,IMG_EIO,"HOST-ENOSPC",dst,"本地写入不完整",NULL); return IMG_EIO; }
    img_result_str(out,"path",path);
    img_result_u64(out,"bytes",(uint64_t)n);
    return IMG_OK;
}

const img_cmd IMG_CMD_TABLE[] = {
    {"img.info",  "1.0", "显示镜像基本信息（大小/分区表/分区数）",
        A_INFO,   (int)(sizeof A_INFO  /sizeof A_INFO[0]),   cmd_img_info},
    {"part.show", "1.0", "打印分区表（JSON：GPT/MBR + 每个分区）",
        A_PTSHOW, (int)(sizeof A_PTSHOW/sizeof A_PTSHOW[0]), cmd_part_show},
    {"fat.put",   "1.0", "把本地文件写入某个 FAT32 分区（带可选空间校验）",
        A_FATPUT, (int)(sizeof A_FATPUT/sizeof A_FATPUT[0]), cmd_fat_put},
    {"fat.ls",    "1.0", "列某个 FAT32 分区里的目录",
        A_FATLS,  (int)(sizeof A_FATLS /sizeof A_FATLS[0]),  cmd_fat_ls},
    {"wim.size",  "1.0", "估算某个 WIM/ESD 镜像展开后需要多少空间（工具给数字，App 定政策）",
        A_WIMSIZE,(int)(sizeof A_WIMSIZE/sizeof A_WIMSIZE[0]), cmd_wim_size},
    {"wim.apply","1.0", "灌入 WIM/ESD 到分区（默认先格式化目标分区防残留；--format=off 保留原分区）",
        A_WIMAPPLY,(int)(sizeof A_WIMAPPLY/sizeof A_WIMAPPLY[0]), cmd_wim_apply_cmd},
    {"part.scan", "1.0", "扫描分区：文件系统/容量/根目录有没有文件（App 弹不弹提示的依据，三级口径）",
        A_PARTSCAN,(int)(sizeof A_PARTSCAN/sizeof A_PARTSCAN[0]), cmd_part_scan},
    {"ntfs.list", "1.0", "列 NTFS 目录（给 App 名单：显示什么/搬谁由 App 决定）",
        A_NTFSLIST,(int)(sizeof A_NTFSLIST/sizeof A_NTFSLIST[0]), cmd_ntfs_list},
    {"ntfs.rename","1.0", "把【给定名单】的顶层项移进目标目录（冲突自动编号 .001；工具不设默认名）",
        A_NTFSREN,(int)(sizeof A_NTFSREN/sizeof A_NTFSREN[0]), cmd_ntfs_rename},
    {"img.clone", "1.0", "克隆保留分区到新盘（不列的分区留空 → 换系统主路径；旧镜像自动改名/校验/回滚）",
        A_IMGCLONE,(int)(sizeof A_IMGCLONE/sizeof A_IMGCLONE[0]), cmd_img_clone},
    {"winre.auto","1.0", "写入/删除【首次开机自动启用 WinRE】脚本（SetupComplete.cmd，官方 reagentc 机制）",
        A_WINRE_AUTO,(int)(sizeof A_WINRE_AUTO/sizeof A_WINRE_AUTO[0]), cmd_winre_auto},
    {"ntfs.exists","1.0", "查 NTFS 路径是否存在（断点续跑探测原语：App 判定步骤做完没）",
        A_NTFSEXISTS,(int)(sizeof A_NTFSEXISTS/sizeof A_NTFSEXISTS[0]), cmd_ntfs_exists},
    {"part.copy", "1.0", "跨镜像扇区级拷贝分区（GPT→MBR 迁移的搬运工；源只读、目标稀疏）",
        A_PARTCOPY,(int)(sizeof A_PARTCOPY/sizeof A_PARTCOPY[0]), cmd_part_copy},
    {"part.mbrboot","1.0", "写 MBR 引导码（参考件）+ 随机磁盘签名（没有它 BIOS 会报「找不到操作系统」）",
        A_MBRBOOT,(int)(sizeof A_MBRBOOT/sizeof A_MBRBOOT[0]), cmd_mbrboot},
    {"part.layout","1.0", "BIOS/MBR 标准布局生成器（系统保留+系统+恢复0x27+扩展分区含逻辑分区）",
        A_PARTLAYOUT,(int)(sizeof A_PARTLAYOUT/sizeof A_PARTLAYOUT[0]), cmd_part_layout},
    {"boot.repair","1.0","BIOS 引导一键修复（诊断→按需重写→复验→JSON）",
        A_BOOTREPAIR,(int)(sizeof A_BOOTREPAIR/sizeof A_BOOTREPAIR[0]), cmd_boot_repair},
    {"fat.export","1.0", "把 FAT32 分区里的文件导出到本地（UEFI 的 BCD 在 ESP 上）",
        A_FATEXPORT,(int)(sizeof A_FATEXPORT/sizeof A_FATEXPORT[0]), cmd_fat_export},
    {"fat.exists", "1.0", "查 FAT32 路径是否存在（断点续跑探测原语）",
        A_FATEXISTS,(int)(sizeof A_FATEXISTS/sizeof A_FATEXISTS[0]), cmd_fat_exists},
    {"ctl.cancel","1.0", "请求取消正在执行的操作（另一线程调用；协作式，下一个检查点生效）",
        NULL, 0, cmd_ctl_cancel},
    /* ---- 2026-10-20：补齐 App 建盘流水线（手册 §11.0 的 7 个缺口）---- */
    {"img.create","1.0", "建 qcow2 本体 + 分区表（★路径参数叫 out：img 会被当成已存在镜像打开）",
        A_IMGCREATE,(int)(sizeof A_IMGCREATE/sizeof A_IMGCREATE[0]), cmd_img_create},
    {"fs.format","1.0", "把一个分区格式化成 NTFS / FAT32（不用再调 imgntfsfs/imgmkfs）",
        A_FSFORMAT,(int)(sizeof A_FSFORMAT/sizeof A_FSFORMAT[0]), cmd_fs_format},
    {"ntfs.put","1.0", "把本地文件写进 NTFS 分区（自动建父目录）",
        A_NTFSPUT,(int)(sizeof A_NTFSPUT/sizeof A_NTFSPUT[0]), cmd_ntfs_put},
    {"ntfs.export","1.0", "把 NTFS 分区里的文件导出到本地（抽 bootmgr/boot.sdi/winre.wim）",
        A_NTFSEXPORT,(int)(sizeof A_NTFSEXPORT/sizeof A_NTFSEXPORT[0]), cmd_ntfs_export},
    {"bcd.gen-uefi","1.0", "UEFI 换盘：按新盘 GUID 重写 BCD（= imggenbcd，同一份库实现）",
        A_BCDGEN,(int)(sizeof A_BCDGEN/sizeof A_BCDGEN[0]), cmd_bcd_gen_uefi},
    {"boot.check","1.0", "引导链自检（= imgbc）：返回逐项结果 + 必需项失败数",
        A_BOOTCHECK,(int)(sizeof A_BOOTCHECK/sizeof A_BOOTCHECK[0]), cmd_boot_check},
    {"iso.ls","1.0", "列 ISO 里的目录（UDF；Windows 安装盘的 install.wim 只在 UDF 里）",
        A_ISOLS,(int)(sizeof A_ISOLS/sizeof A_ISOLS[0]), cmd_iso_ls},
    {"iso.extract","1.0", "从 ISO 里抽文件到本地（流式 1 MiB；App 不必自己装 ISO 库）",
        A_ISOEXTRACT,(int)(sizeof A_ISOEXTRACT/sizeof A_ISOEXTRACT[0]), cmd_iso_extract},
    {"cmd.list",  "1.0", "导出命令表（名字/参数规格/帮助）——App 可据此自动生成界面",
        NULL, 0, cmd_cmd_list},
};
const int IMG_CMD_COUNT = (int)(sizeof IMG_CMD_TABLE/sizeof IMG_CMD_TABLE[0]);
