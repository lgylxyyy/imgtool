/* img_cmds.c — 命令表（加新功能就在这里加一行 + 写一个 handler）
 *
 * 每个命令 = {名字, 版本, 帮助, 参数规格[], handler}
 * 参数规格里的类型/必填/默认/校验，CLI 与 App 共用 —— 不再两处各写一遍。
 */
#define _POSIX_C_SOURCE 200809L
#include "img_cmd.h"
#include "img_io.h"
#include "img_fat32.h"
#include "img_fat32_fs.h"
#include "img_event.h"
#include "img_wim_apply.h"
#include "img_ntfs_fmt.h"
#include "img_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static char g_pt_json[8192];        /* 单线程；handler 产出后立即渲染 */
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
    int rc=img_fat32_put_file(fs,src,dst,NULL);
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
    {"format", IMG_A_ENUM, false, "off",      "灌入前是否格式化目标分区（on|off）", NULL},
    {"label",  IMG_A_STR,  false, "Windows", "（format=on）卷标", NULL},
    {"no_acls", IMG_A_BOOL,false, "false",   "不要 ACL（跨平台/不需要时开）", NULL},
    {"no_attrs",IMG_A_BOOL,false, "false",   "不要扩展属性", NULL},
    {"wimboot", IMG_A_BOOL,false, "false",   "wimboot 模式（文件指针化）", NULL},
};
static int cmd_wim_apply_cmd(img_ctx *ctx, const img_args *in, img_result *out){
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
        rc=img_ntfs_format(q,&o,NULL);
        img_span_end(&sp,rc);
        if(rc){ img_result_fail(out,rc,"NTFS-FORMAT-FAIL",NULL,NULL,NULL); return rc; }
    }

    /* ② 里程碑：灌入 */
    img_wim_apply_opts_t wo; memset(&wo,0,sizeof wo);
    wo.no_acls  =img_args_bool(in,"no_acls",false);
    wo.no_attrs =img_args_bool(in,"no_attrs",false);
    wo.wimboot  =img_args_bool(in,"wimboot",false);
    rc=img_wim_apply(wim,idx,q,lba,&wo,NULL);
    if(rc){
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"WIM-APPLY-FAIL";
        img_result_fail(out,rc,code,NULL,NULL,NULL);
        return rc;
    }
    img_result_u64(out,"index",(uint64_t)idx);
    img_result_bool(out,"formatted",do_fmt);
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
    {"ctl.cancel","1.0", "请求取消正在执行的操作（另一线程调用；协作式，下一个检查点生效）",
        NULL, 0, cmd_ctl_cancel},
    {"cmd.list",  "1.0", "导出命令表（名字/参数规格/帮助）——App 可据此自动生成界面",
        NULL, 0, cmd_cmd_list},
};
const int IMG_CMD_COUNT = (int)(sizeof IMG_CMD_TABLE/sizeof IMG_CMD_TABLE[0]);
