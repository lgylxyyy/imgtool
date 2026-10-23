/* img_cmds.c — 命令表（加新功能就在这里加一行 + 写一个 handler）
 *
 * 每个命令 = {名字, 版本, 帮助, 参数规格[], handler}
 * 参数规格里的类型/必填/默认/校验，CLI 与 App 共用 —— 不再两处各写一遍。
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>   /* img.create/ntfs.put 要 stat 判存在与大小 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>   /* qc_open_fail 复查文件要 open/read/close */
#include "img_cmd.h"
#include "img_fixboot.h"
#include "img_bcd.h"
#include "img_constraints.h"   /* img_layout_build：img.create 用 */
#include "img_bootcheck.h"
#include "img_iso.h"
#include "img_recovery.h"
#include "img_identify.h"      /* boot.check 用 */
#include "img_bootplan.h"      /* boot.plan / bcd.create 的分区表判定 */
#include "img_text.h"        /* 进信封的人话：中英双份 */
#include "img_io.h"
#include "img_fat32.h"
#include "img_fat32_fs.h"
#include "img_event.h"
#include "img_wim_apply.h"
#include "img_wim_fs.h"
#include "img_win_finish.h"
/* ---- 功能开关（CMake 降级判定；缺库时对应 =0，命令仍列在表里但明确报不可用）---- */
#ifndef IMG_WITH_WIM
#define IMG_WITH_WIM 0
#endif
#ifndef IMG_WITH_NTFS3G
#define IMG_WITH_NTFS3G 0
#endif
/* ★ 硬缺口 #3 的落点：库缺失【不允许悄悄失败/不允许构建失败】，必须给出稳定错误码 + 可执行建议 */
/* ★ 排查用：打开 qcow2 失败时，把"再打开一次实际读到什么"一起报出来。
 *   真机上遇到过"同一张盘、老进程读不出、冷进程读得出"的情况，光有 QC-OPEN-FAIL
 *   根本分不清是文件坏了、还是这次读本身出了问题。 */
static int qc_open_fail(img_result *out, img_ctx *ctx){
    const char *path = img_ctx_path(ctx);
    unsigned char h[16]; int nh = 0, e = 0; long long sz = -1;
    memset(h, 0, sizeof h);
    if(path && *path){
        int fd = open(path, O_RDONLY);
        if(fd >= 0){ ssize_t r = read(fd, h, sizeof h); nh = (r > 0) ? (int)r : 0; e = (r <= 0) ? errno : 0; close(fd); }
        else e = errno;
        struct stat st; if(stat(path, &st) == 0) sz = (long long)st.st_size;
    }
    char hex[64]; hex[0] = 0;
    for(int i = 0; i < nh && i < 16; i++) snprintf(hex + i*2, sizeof(hex) - i*2, "%02x", h[i]);
    char msg[320];
    if(img_io_timed_out()){
        /* I/O 看门狗已判超时 → 后续读全部 fail-fast，看起来就像"文件坏了"。
         * 说清楚，否则排查方向直接被带偏（真机上就绕了一大圈）。 */
        snprintf(msg, sizeof msg,
                 "I/O 看门狗已判超时，本进程的读写被短路；复查文件本身：前 %d 字节=%s 大小=%lld",
                 nh, hex[0] ? hex : "(空)", sz);
        img_result_fail(out, IMG_ETIMEDOUT, "IO-TIMEOUT", path, msg,
                        "重跑一次命令即可（新命令会清掉超时判定）");
        return IMG_ETIMEDOUT;
    }
    snprintf(msg, sizeof msg, "不是合法 qcow2；复查：前 %d 字节=%s 文件大小=%lld errno=%d %s",
             nh, hex[0] ? hex : "(空)", sz, e, e ? strerror(e) : "无");
    img_result_fail(out, IMG_EFORMAT, "QC-OPEN-FAIL", path, msg, NULL);
    return IMG_EFORMAT;
}

static int feat_unavail(img_result *out, const char *feat, const char *howto){
    img_result_fail(out, IMG_ENOTSUP, "FEATURE-UNAVAILABLE", feat,
                    "这个构建缺少该功能依赖的系统库", howto);
    return IMG_ENOTSUP;
}

#include "img_ntfs_fmt.h"
 #include "img_clone.h"
#include "img_ntfs_fs.h"
#include "img_xcopy.h"   /* part.scan / ntfs.list / ntfs.rename */
typedef struct { img_io *io; } rd_io_t;
static int rd_io(void *ud, uint64_t off, void *buf, size_t len){
    rd_io_t *r=(rd_io_t*)ud;
    return img_io_read_full(r->io,off,buf,len);
}
typedef struct { img_iso *iso; const char *path; } rd_iso_t;
static int rd_iso(void *ud, uint64_t off, void *buf, size_t len){
    rd_iso_t *r=(rd_iso_t*)ud;
    return img_iso_read_range(r->iso,r->path,off,buf,len);
}

#include "img_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
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
     *   查不到时允许 App 把宿主剩余传进来（拿 fd 查不到的云端提供者就是此类情况）。*/
    {"host_free", IMG_A_U64,  false, NULL, "宿主剩余可用字节（一般不用传，工具自己查；查不到时才传）", NULL},
    {"reserve",   IMG_A_BOOL, false, "false","写前用 fallocate 真预留宿主空间（立即占用，保证不会中途 ENOSPC）", NULL},
    {"preserve_times", IMG_A_BOOL, false, "false",
     "保留【宿主文件】的修改/访问时间（FAT 精度 2 秒）", NULL},
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
                        "源文件无法打开（不存在/无权限）","检查路径与权限");
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
        /* 小于 1MB 时按 KB 显示，避免"剩余 0 MB"此类看不出量级的提示 */
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
    if(rc==IMG_OK && img_args_bool(in,"preserve_times",false)){
        struct stat st2; if(stat(src,&st2)==0){
            uint16_t tm=0,dt=0; img_fat32_epoch_to_dos((int64_t)st2.st_mtime,&tm,&dt);
            uint16_t t5[5]={tm,dt,dt,tm,dt};
            (void)img_fat32_file_set_times(fs,dst,t5);
        }
    }
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
    /* 分页：与 ntfs.list 同一套参数与语义（App 只需要学一次）。
     * 以前是 64 KB 缓冲写满就悄悄截断（约 900 项封顶）—— ESP 一般够用，
     * 但"够用"不是"不炸"的理由。 */
    {"offset", IMG_A_U64, false, "0",   "从第几项开始（分页；默认 0）", NULL},
    {"limit",  IMG_A_U64, false, "500", "本页最多多少项（1~2000；默认 500）", NULL},
    {"times",  IMG_A_BOOL, false, "false", "顺带取创建/修改时间（unix 秒 UTC）", NULL},
};
#define FAT_LIST_MAX 2000
#define FAT_LIST_DEFAULT 500
typedef struct { char *p, *end;
                 uint64_t total, offset, limit;
                 int n; bool truncated;
                 img_fat32_fs *fs; const char *dir; bool want_times; } lsbuf;
static void ls_cb(void *ud, const char *name, bool isdir, uint64_t size){
    lsbuf *b=(lsbuf*)ud;
    uint64_t idx=b->total++;                              /* 先计数，再决定要不要写进本页 */
    if(idx < b->offset) return;
    if((uint64_t)b->n >= b->limit) return;
    char item[768];
    int n;
    if (b->want_times && b->fs) {
        char full[1200]; uint16_t t5[5] = {0,0,0,0,0};
        snprintf(full, sizeof full, "%s%s%s", b->dir,
                 (b->dir[0] && b->dir[strlen(b->dir)-1] == '/') ? "" : "/", name ? name : "");
        int64_t ft = 0, mt = 0;
        if (img_fat32_file_times(b->fs, full, t5) == IMG_OK) {
            ft = img_fat32_dos_to_epoch(t5[0], t5[1]);     /* 创建 */
            mt = img_fat32_dos_to_epoch(t5[3], t5[4]);     /* 最后写 */
        }
        n = snprintf(item, sizeof item,
                     "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu,\"ctime\":%lld,\"mtime\":%lld}",
                     b->n?",":"", name?name:"", isdir?"true":"false",
                     (unsigned long long)size, (long long)ft, (long long)mt);
    } else {
        n = snprintf(item, sizeof item, "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu}",
                     b->n?",":"", name?name:"", isdir?"true":"false",(unsigned long long)size);
    }
    if(n<=0) return;
    size_t need=(size_t)n+3;                              /* 还要留得下 "]\0" */
    if(b->p+need>=b->end){ b->truncated=true; return; }
    memcpy(b->p,item,(size_t)n); b->p+=n; b->n++;
}
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
    uint64_t offset=img_args_u64(in,"offset",0);
    uint64_t limit =img_args_u64(in,"limit",FAT_LIST_DEFAULT);
    if(limit==0) limit=FAT_LIST_DEFAULT;
    if(limit>FAT_LIST_MAX) limit=FAT_LIST_MAX;
    /* ★ TLS：fat.ls 带 ctx，但不同 ctx 可以并行；进程级缓冲会互相覆盖
     *   （与 iso.ls / ntfs.list 同一类缺陷，这次一并补上）*/
    IMG_THREAD_LOCAL static char g_ls_json[65536];
    g_ls_json[0]='[';
    lsbuf b={g_ls_json+1, g_ls_json+sizeof g_ls_json-4, 0, offset, limit, 0, false,
             fs, path, img_args_bool(in,"times",false)};
    int rc=img_fat32_list_dir(fs,path,ls_cb,&b);
    b.p[0]=']'; b.p[1]=0;
    img_fat32_unmount(fs);
    if(rc==IMG_ENOTSUP){
        img_result_fail(out,IMG_ENOENT,"FAT-DIR-NOTFOUND",path,"目录不存在或不是目录","核对路径");
        return IMG_ENOENT;
    }
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,img_strerror(rc),"镜像可能损坏"); return rc; }
    img_result_u64(out,"total",b.total);
    img_result_u64(out,"offset",offset);
    img_result_u64(out,"limit",limit);
    img_result_u64(out,"count",(uint64_t)b.n);
    img_result_bool(out,"has_more", offset+(uint64_t)b.n < b.total);
    img_result_bool(out,"truncated",b.truncated);
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
                        rc==IMG_ENOENT?"WIM/ESD 无法打开（不存在或不是 WIM 格式）"
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

    /* 表驱动没有 enum 值域校验 → 这里显式判（严格式，不允许 --format=maybe 被静默当 off）*/
    const char *fmt=img_args_get(in,"format");
    if(fmt && strcmp(fmt,"on") && strcmp(fmt,"off")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","format","只接受 on|off",NULL);
        return IMG_EINVAL;
    }
    bool do_fmt = !fmt || !strcmp(fmt,"on");

    /* 分区大小（格式化与灌入都要） */
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }

    /* ★ 不格式化时先问"卷脏不脏"：中断/掉电会把 NTFS 标脏，ntfs-3g 对脏卷只给
     *   只读挂载 → wimlib 直写模式【秒失败】，只报 WIM-APPLY-FAIL，没人猜得到
     *   该加 --format=on。实测：中断后重灌 103ms 失败就是这个原因。*/
    /* ★ 灌入前的只读预检：只拦【卷脏】。
     *   曾经还想在这里拦"根目录非空"，实测是错的：中断后 --format=off 重跑
     *   【能正常覆盖续灌】（12GiB 盘灌 8GB 系统 218 秒成功）—— 拦非空会误伤这个合法用法。
     *   非空的项数只作为结果字段回给 App（root_entries），由 App 决定要不要提示。*/
    int root_entries=-1;
    if(IMG_WITH_NTFS3G){
        bool dirty=false; int nroot=0;
        if(img_ntfs_precheck(q,lba,pb,&dirty,&nroot)==IMG_OK){
            root_entries=nroot;
            if(dirty && !do_fmt){
                img_result_fail(out,IMG_ESTATE,"NTFS-VOLUME-DIRTY",img_ctx_path(ctx),
                    "目标分区被标脏（上次灌入被中断/掉电）：ntfs-3g 对脏卷只给只读挂载，写不进去。"
                    "加 --format=on 重新格式化再灌（会清空该分区）",NULL);
                return IMG_ESTATE;
            }
        }
    }
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
        if(rc==IMG_ENOTSUP) return feat_unavail(out,"NTFS 格式化模块（mkntfs）",
                        "重新编译时开启 -DIMG_WITH_NTFS=ON（会引入 GPL-2.0+ 代码）");
        if(rc){ img_result_fail(out,rc,"NTFS-FORMAT-FAIL",NULL,NULL,NULL); return rc; }
    }

    /* ② 里程碑：灌入 */
    img_wim_apply_opts_t wo; memset(&wo,0,sizeof wo);
    wo.no_acls  =img_args_bool(in,"no_acls",false);
    wo.no_attrs =img_args_bool(in,"no_attrs",false);
    wo.wimboot  =img_args_bool(in,"wimboot",false);
    rc=img_wim_apply(wim,idx,q,lba,&wo,img_cmd_prog());
    if(rc){
        if(rc==IMG_ENOTSUP) return feat_unavail(out,"可写临时目录",
                                    "先调 Native.setTmpdir(context.getCacheDir() 的路径)");
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"WIM-APPLY-FAIL";
        /* ★ hint 里带上 wimlib 的原始码 + 最常见的出路：中断过就别想"接着灌"，
         *   必须 --format=on 重来（实测中断后 --format=off 重跑 103ms 就失败，
         *   而当时我们只报 WIM-APPLY-FAIL，谁也猜不到原因）。*/
        static char hint[220];
        int wr=img_wim_last_rc();
        snprintf(hint,sizeof hint,
                 "wimlib 返回 %d；若上次灌入被中断/掉电过，目标分区已经半满，"
                 "请加 --format=on 重新格式化后再灌（会清空该分区）", wr);
        img_result_fail(out,rc,code,wim,hint,NULL);
        return rc;
    }
    img_result_u64(out,"index",(uint64_t)idx);
    img_result_bool(out,"formatted",do_fmt);
    if(root_entries>=0) img_result_u64(out,"root_entries",(uint64_t)root_entries);   /* 灌入前根目录项数（App 可据此判断"这是覆盖灌"）*/
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
    /* ★ 分页（2026-10-23）：System32 有 4000+ 项，整页序列化必然溢出 128KB 输出缓冲，
     *   于是 App 只拿到 RESULT-TOO-BIG、看不见任何名字。分页把「看不见」变成「翻页」。*/
    {"offset", IMG_A_U64, false, "0",   "从第几项开始（分页；默认 0）", NULL},
    {"limit",  IMG_A_U64, false, "500", "本页最多多少项（1~2000；默认 500）", NULL},
    {"sizes",  IMG_A_BOOL, false, "false","顺带取真实文件大小（逐条开 inode，大目录明显变慢；默认关）", NULL},
    {"times",  IMG_A_BOOL, false, "false","顺带取创建/修改时间（unix 秒 UTC；逐条开 inode，大目录变慢）", NULL},
    {"ino",   IMG_A_BOOL, false, "false", "每项带 ino（App 做 stable id；逐条查索引，略慢）", NULL},
    {"after", IMG_A_U64, false, "0", "游标：只返回【这一条之后】的项（传上一页最后一项的 ino）", NULL},
};
/* 按 LBA 从分区表取分区字节数（与文件系统无关，FAT/NTFS 都用 ⇒ 不能放在 NTFS 守卫里） */
static uint64_t ntfs_part_bytes(img_parttable_t *t,uint64_t lba){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba)
            return (t->parts[i].last_lba - t->parts[i].first_lba + 1)*512;
    return 0;
}

#if IMG_WITH_NTFS3G
/* 单页硬上限：条目 JSON 约 70~90 字节，2000 项 ≈ 180KB，仍在 256KB 缓冲内。
 * 真正的上限是【输出信封】的大小（CLI 128KB / JNI 侧同样有上限），
 * 所以默认 500 给 App 留足余量。*/
#define NTFS_LIST_MAX 2000
#define NTFS_LIST_DEFAULT 500
typedef struct { char *b; size_t cap, len;
                 uint64_t total;            /* 目录里一共多少项（不受分页影响）*/
                 uint64_t offset, limit;
                 int n;                     /* 本页已序列化项数 */
                 bool truncated;
                 img_ntfs_fs *fs; const char *dir; bool want_times;
                 /* 用 dir 路径 + 库函数拿 ino（命令层不碰 ntfschar/MFT_REF 这些内部类型）*/
                 bool want_ino;           /* 返回 ino（App 做 stable id） */
                 uint64_t after;          /* --after=<ino> 游标 */
                 bool after_seen; } list_buf_t;
static void list_json_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    list_buf_t *a=(list_buf_t*)ud;
    uint64_t ino = 0;
    if(a->want_ino || a->after)
        (void)img_ntfs_dir_lookup_ino(a->fs, a->dir, name, &ino);
    /* --after 游标：跳过"到游标为止"的项（游标那条本身属于上一页） */
    if(a->after && !a->after_seen){
        if(ino != a->after) return;
        a->after_seen = true; return;
    }
    uint64_t idx = a->total++;                 /* 先计数，再决定要不要写进本页 */
    if(idx < a->offset) return;
    if((uint64_t)a->n >= a->limit) return;     /* 后面的项只计数，不再序列化 */
    char item[640];
    /* name 里可能有引号/反斜杠 → 手写转义（与日志同一套规则）*/
    int k=snprintf(item,sizeof item,"%s{\"name\":\"", a->n?",":"");
    for(const unsigned char *p=(const unsigned char*)name; *p && k<(int)sizeof item-32; p++){
        if(*p=='"'||*p=='\\') item[k++]='\\', item[k++]=(char)*p;
        else if(*p<0x20) k+=snprintf(item+k,sizeof item-(size_t)k,"\\u%04x",*p);
        else item[k++]=(char)*p;
    }
    if(a->want_times && a->fs){
        char full[1200]; uint64_t t4[4]={0,0,0,0};
        snprintf(full,sizeof full,"%s%s%s",a->dir,
                 (a->dir[0]&&a->dir[strlen(a->dir)-1]=='/')?"":"/", name);
        if(img_ntfs_file_times(a->fs,full,t4)==IMG_OK)
            k+=snprintf(item+k,sizeof item-(size_t)k,
                        "\",\"dir\":%s,\"size\":%llu,\"ino\":%llu,\"ctime\":%llu,\"mtime\":%llu}",
                        is_dir?"true":"false",(unsigned long long)size,
                        (unsigned long long)ino,
                        (unsigned long long)t4[0],(unsigned long long)t4[1]);
        else
            k+=snprintf(item+k,sizeof item-(size_t)k,"\",\"dir\":%s,\"size\":%llu}",
                        is_dir?"true":"false",(unsigned long long)size);
    } else {
        k+=snprintf(item+k,sizeof item-(size_t)k,"\",\"dir\":%s,\"size\":%llu,\"ino\":%llu}",
                    is_dir?"true":"false",(unsigned long long)size,(unsigned long long)ino);
    }
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL);
             return IMG_ESTATE; }
    uint64_t offset=img_args_u64(in,"offset",0);
    uint64_t limit =img_args_u64(in,"limit",NTFS_LIST_DEFAULT);
    if(limit==0) limit=NTFS_LIST_DEFAULT;                 /* 0 当默认，避免"要 0 项"这种无意义输入 */
    if(limit>NTFS_LIST_MAX) limit=NTFS_LIST_MAX;
    /* ★ TLS：ntfs.list 走的是 ctx（同一 ctx 内部串行），但不同 ctx 可并行；
     *   jbuf 原是进程级 static → 两盘并发列目录会互相覆盖（与 iso.ls 同一类缺陷）。*/
    IMG_THREAD_LOCAL static char jbuf[256*1024];
    uint64_t after_ino = img_args_u64(in,"after",0);
    bool want_ino = img_args_bool(in,"ino",false) || after_ino != 0;
    /* rev：目录 $SI 的"改动时间"（秒）。App 用它判断自己那份缓存过期没（§22）。*/
    uint64_t rev = 0;
    (void)img_ntfs_dir_rev(fs, path, &rev);
    bool want_sizes = img_args_bool(in,"sizes",false);
    list_buf_t acc={jbuf,sizeof jbuf,0,0,offset,limit,0,false,
                    fs,path,img_args_bool(in,"times",false),
                    want_ino,after_ino,false};
    rc = want_sizes ? img_ntfs_list_dir_sizes(fs,path,list_json_cb,&acc)
                    : img_ntfs_list_dir(fs,path,list_json_cb,&acc);
    img_ntfs_umount(fs);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,"目录无法读取（路径不对或卷损坏）",NULL); return rc; }
    img_result_u64(out,"total",acc.total);                /* 目录里总共多少项（翻页依据）*/
    img_result_u64(out,"offset",offset);
    img_result_u64(out,"limit",limit);
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"has_more", offset+(uint64_t)acc.n < acc.total);
    img_result_bool(out,"truncated",acc.truncated);
    img_result_bool(out,"sizes",want_sizes);
    img_result_bool(out,"ino",want_ino);
    img_result_u64(out,"rev",rev);
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
    if(rc!=IMG_OK){
        const char *code = (rc==IMG_ECORRUPT)?"NTFS-DIR-PHANTOM"
                         : (rc==IMG_ENOENT)?"NTFS-PATH-NOTFOUND"
                         : (rc==IMG_ENOTSUP)?"NTFS-SRC-IS-DIR":"NTFS-MOVE-FAIL";
        img_result_fail(out,rc,code,final,
            rc==IMG_ECORRUPT?"搬完从盘上复查没过：目录或名字没真落盘（卷已不一致，停止继续写）":
                             "搬移失败（名字不存在/只读/卷损坏）",
            rc==IMG_ECORRUPT?"立刻停止写这张盘；换备份镜像；用 tools/ntfs-inventory.py 或 chkdsk /scan 定位":NULL);
        return rc; }
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
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
#endif /* IMG_WITH_NTFS3G */
static int cmd_winre_auto(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（WinRE 自启用脚本）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G

    uint64_t lba=img_args_u64(in,"lba",0);
    bool enable = img_args_bool(in,"enable",true);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","/Windows","系统分区不是合法 NTFS",NULL); return IMG_ESTATE; }
    const char *path="/Windows/Setup/Scripts/SetupComplete.cmd";
    img_ntfs_umount(fs);            /* 交给库函数自己挂载/卸载（同一份脚本内容）*/
    fs=NULL;
    if(enable){
        img_span sp;
        img_span_prep(&sp,"winre","write","写入 WinRE 自启用脚本");
        img_span_kv_str(&sp,"path",path);
        img_span_begin(&sp);
        size_t nb=0;
        rc=img_winre_write_enable_script(q,lba,pb,&nb);
        img_span_end(&sp,rc);
        if(rc){ img_result_fail(out,rc,"FAT-PUT-FAIL",path,"脚本写入失败（卷只读/空间不足）",NULL); return rc; }
        img_result_bool(out,"written",true);
        img_result_u64(out,"bytes",(uint64_t)nb);
    } else {
        img_ntfs_fs *fs2=img_ntfs_mount_rw(q,lba,pb);
        if(!fs2){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","/Windows","系统分区不是合法 NTFS",NULL); return IMG_ESTATE; }
        fs=fs2;
        if(img_ntfs_path_exists(fs,path)){
            rc=img_ntfs_rm(fs,path);
            if(rc){ img_ntfs_umount(fs);
                    img_result_fail(out,rc,rc==IMG_ECORRUPT?"NTFS-DIR-PHANTOM":"NTFS-RM-FAIL",
                                    path,"删除脚本失败",NULL); return rc; }
        }
        img_result_bool(out,"removed",true);
    }
    if(fs) img_ntfs_umount(fs);
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
 *   ntoskrnl.exe 可能还未写入到）。要更强保证用 img_bootcheck 自检 或 看日志最后一行。*/
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
    if(!q){ return qc_open_fail(out,ctx); }
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
    if(!dq){ return qc_open_fail(out,ctx); }
    img_parttable_t *dt=NULL; int rc=img_ctx_parttable(ctx,&dt);
    if(rc||!dt) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",NULL,NULL,NULL), rc?rc:IMG_ESTATE;
    img_parttable_t st; {   /* 源：只读打开（★ 全程不写源，是这类迁移安全性的根）*/
        img_io_opts_t o={0};
        img_io *sfi=img_io_open_path(src_path,&o);
        if(!sfi){ img_result_fail(out,IMG_ENOENT,"IMG-OPEN-FAIL",src_path,"源镜像无法打开（路径/权限）",NULL); return IMG_ENOENT; }
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
                        img_prog *pg=img_cmd_prog();
                        if(pg){ img_prog_phase(pg,IMG_PH_COPY,"跨镜像拷贝分区");
                                img_prog_op(pg,IMG_OP_WRITE); img_prog_bytes(pg,0,n); }
                        uint64_t done=0, written=0;
                        while(done<n && rc==IMG_OK){
                            size_t k=(size_t)((n-done)<(1024*1024)?(n-done):(1024*1024));
                            rc=img_qcow2_read_full(sq, slba*512u+done, buf, k);
                            if(rc==IMG_OK && memcmp(buf,zero,k)!=0){   /* ★ 全零跳过（保持稀疏）*/
                                rc=img_qcow2_write(dq, dlba*512u+done, buf, k);
                                if(rc==IMG_OK) written+=k;
                            }
                            done+=k;
                            if(pg) img_prog_bytes(pg,done,n);   /* 节流在 img_prog 内部（200ms）*/
                            img_hb("part","copy","part.copy",done>>20);   /* 心跳（内部 2→60s 退避）*/
                            if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
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
    if(!f){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",code_path,"引导码文件无法打开",NULL); return IMG_ENOENT; }
    unsigned char code[512]={0};
    size_t got=fread(code,1,sizeof code,f); fclose(f);
    if(got<440){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","code","引导码文件至少要 440 字节",NULL); return IMG_EINVAL; }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
    if(!q){ return qc_open_fail(out,ctx); }
    rc=img_parttable_write(q,t);
    if(rc){ img_result_fail(out,rc,"PART-WRITE-FAIL",NULL,"写回分区表失败",NULL); return rc; }
    img_result_u64(out,"disk_signature",sig);
    img_result_bool(out,"written",true);
    return IMG_OK;
}

/* ================= 命令 6j：part.layout（BIOS/MBR 标准布局生成器）====================
 * 产出：系统保留(活动,0x27) → 系统(0x07) → 恢复(0x27) → 扩展分区(0x0F, 含一个逻辑分区)
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
    if(!q){ return qc_open_fail(out,ctx); }
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
    /* ★ 本命令产出的是 MBR 表，没有 GUID 这个概念：以前"校验通过但什么也未写入"，
     *   传了等于白传还不报错。现在直接说清楚（要用 GUID 请 img.create --table=gpt）。*/
    if(img_args_get(in,"part-guid") || img_args_get(in,"disk-guid")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","part-guid/disk-guid",
            "part.layout 产出 MBR 表，没有分区/磁盘 GUID；要 GUID 请用 img.create --table=gpt",NULL);
        return IMG_EINVAL;
    }
    if(apply_custom_guids(in,&t)!=IMG_OK){ img_result_fail(out,IMG_EINVAL,"GUID-INVALID-BAD","part-guid/disk-guid","GUID 非法或序号越界",NULL); return IMG_EINVAL; }
        rc=img_parttable_write(q,&t);
        if(rc){ img_result_fail(out,rc,"PART-WRITE-FAIL",NULL,"写分区表失败",NULL); return rc; }
        img_result_u64(out,"parts_total",1);
        img_result_str(out,"mode","single");
        return IMG_OK;
    }
    /* 系统保留用 0x27（Windows 安装程序同款）：0x07 会被分配盘符，抢走 C: */
    if((rc=img_partition_add(&t,off,sr_sec,IMG_MBR_RECOVERY,NULL,"System Reserved",true,&idx))!=IMG_OK)
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
 *   NTFS/wimlib 直灌路径不支持回滚 → 卡片会明确告知"该分区需重做"。*/
static int cmd_ctl_cancel(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx; (void)in;
    img_cancel();
    img_result_bool(out,"cancelled",true);
    img_result_str(out,"note",img_t("已置取消标志；正在跑的操作会在下一个检查点退出"));
    return IMG_OK;
}

/* ================= 命令 7：cmd.list（自描述：App 可据此自动生成界面） ================= */
static int cmd_cmd_list(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx; (void)in;
    /* ★ 线程局部而非进程级 static：cmd.list 无 ctx、允许并行，进程级缓冲会被
     *   另一条并发命令覆盖。不能改成 per-call malloc —— img_result_raw() 只存指针
     *   不拷贝，缓冲必须活到调用方渲染完，命令函数里 free 就是悬垂指针。*/
    IMG_THREAD_LOCAL static char buf[96*1024];
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
    {"vbr-tpl", IMG_A_STR, false, NULL, "VBR 模板 镜像:lba（缺省用内置模板，不需要外部盘）", NULL},
    {"no-vbr",  IMG_A_BOOL,false, "false","跳过 VBR 引导码修复（UEFI 盘不需要；默认按需补）", NULL},
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
    /* ★ 同 img_ctx_open：boot.repair 自己开的镜像也必须"已存在"，不允许创建空文件 */
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
    /* ★ 只认 "NTFS    " 标签不够：mkntfs 也写标签，但扇区 1..9 的引导代码是空的，
     *   BIOS 照样报 "A disk read error occurred"。必须看后续扇区有没有代码。*/
    int vbr_code_ok=0;
    if(vbr_ok){
        unsigned char s1[512];
        if(img_qcow2_read(q,(vbr_lba+1)*512u,s1,512)==IMG_OK)
            for(int i=0;i<512;i++) if(s1[i]){ vbr_code_ok=1; break; }
    }
    img_result_bool(out,"diag.vbr_code_ok",vbr_code_ok);
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
    /* ②-2 VBR：模板引导码（保留自己的 BPB）
     * ★ 缺省用【内置模板】（src/img_vbr_tpl.h），不再要求外部模板盘；
     *   --vbr-tpl 可覆盖成你自己提取的；--no-vbr=true 跳过（UEFI 不依赖这段）。*/
    bool no_vbr = img_args_bool(in,"no-vbr",false);
    /* ★ "只诊断"必须真的不动手：只有调用方【表达了要修】（传了 mbr-code / vbr-tpl / bcd-ref
     *   中任意一个）时，才把 VBR 一起补上（用内置模板）。否则 --img + --sys-lba 这种
     *   纯诊断调用也会改盘 —— boot-repair-test 第 1 组就是守这条（fixed_count=0）。*/
    bool any_fix = (mcode!=NULL) || (vtpl!=NULL) || (bref!=NULL);
    if(!no_vbr && any_fix && (vtpl || !vbr_code_ok)){
      if(!vtpl){
        int gp=0;
        if(img_ntfs_fix_geometry(q,sys_lba)==IMG_OK) gp++;
        if(vbr_lba!=sys_lba && img_ntfs_fix_geometry(q,vbr_lba)==IMG_OK) gp++;
        img_result_u64(out,"geometry_patched",(uint64_t)gp);
        int rc0=img_fixboot_builtin(q,vbr_lba,img_cmd_prog());
        if(rc0!=IMG_OK){ img_result_fail(out,rc0,"BOOT-VBR-WRITE-FAIL","builtin",NULL,NULL); return rc0; }
        fixed++;
      } else {
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
        /* 参考件里的旧值（改之前先记下：改完就无法读取了） */
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
    /* 参数级错误要在建文件之前报：msr-mb=0 以前会走到布局生成，
     * 拿回一个 PART-WRITE-FAIL（看起来像"盘坏了"），实际只是参数非法。*/
    if(gpt && img_args_u64(in,"msr-mb",16)==0){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","msr-mb",
                        "GPT 布局必须有 MSR 保留分区（Windows 安装器依赖它）",
                        "要尽量小就给 msr-mb=16；不要给 0");
        return IMG_EINVAL;
    }
    if(gpt && img_args_u64(in,"esp-mb",200)==0){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","esp-mb",
                        "GPT/UEFI 布局必须有 ESP（引导文件放这里）",
                        "ESP 至少给 64 MiB；通常 100~300");
        return IMG_EINVAL;
    }
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
        img_result_fail(out,IMG_EFORMAT,"IMG-FORMAT",outp,"qcow2 头无法写入去",NULL); return IMG_EFORMAT; }
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
        img_result_fail(out,rc,"PART-WRITE-FAIL",outp,"分区表无法写入去",NULL); return rc; }
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
    if(fat && !img_args_bool(in,"quick",true)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","quick","FAT32 没有【整盘零填】模式（quick 只对 NTFS 有意义）",NULL);
        return IMG_EINVAL;
    }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
        if(rc==IMG_ENOTSUP) return feat_unavail(out,"NTFS 格式化模块（mkntfs）",
                        "重新编译时开启 -DIMG_WITH_NTFS=ON（会引入 GPL-2.0+ 代码）");
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
    {"preserve_times", IMG_A_BOOL,false,"false",
     "保留【宿主文件】的修改/访问时间（创建时间按修改时间填；精确到 100ns）", NULL},
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
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
    if(rc==IMG_OK && img_args_bool(in,"preserve_times",false)){
        uint64_t t4[4]={ (uint64_t)st.st_mtime, (uint64_t)st.st_mtime,
                         (uint64_t)st.st_atime, 0 };
        (void)img_ntfs_set_times(fs,dst,t4);
    }
    img_span_end(&sp,rc);
    img_ntfs_umount(fs);
    if(rc){ img_result_fail(out,rc,rc==IMG_ECORRUPT?"NTFS-DIR-PHANTOM":"FAT-PUT-FAIL",dst,
                rc==IMG_ECORRUPT?"建父目录后从盘上复查没过（幽灵目录：记录没落盘）":"写入失败（空间/只读/路径）",
                rc==IMG_ECORRUPT?"停止继续写这张盘；换备份；跑 tools/ntfs-inventory.py 体检":NULL); return rc; }
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    /* ★ 流式导出（1 MiB 一块）：以前把整文件读进内存，install.wim 4 GB 会 OOM，
     *   而且中间没有进度可报。*/
    img_span sp; img_span_prep(&sp,"ntfs","export","导出 NTFS 文件");
    img_span_kv_str(&sp,"path",path); img_span_begin(&sp);
    uint64_t got=0;
    rc=img_ntfs_copy_out(fs,path,dst,img_cmd_prog(),&got);
    img_ntfs_umount(fs);
    img_span_kv_u64(&sp,"written",got);
    img_span_end(&sp,rc);
    if(rc!=IMG_OK){
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"FAT-ENTRY-NOTFOUND";
        img_result_fail(out,rc,code,path,(rc==IMG_ECANCELLED)?"已取消":"文件不存在或无法读取",NULL);
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_u64(out,"bytes",got);
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
    if(!b){ img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD",ref,"无法打开或不是合法 hive",NULL); return IMG_EFORMAT; }
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
    if(!q){ return qc_open_fail(out,ctx); }
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
/* 递归列目录（2026-10-23）
 * 背景：以前只能一层层 ls —— 用户问"这张 ISO 里有没有 install.wim / 哪个版本"，
 *   得点几十次。递归 + find 一次给全（只读，不改 ISO）。
 * 防环/防爆：① 深度上限（默认 32）② 条目上限（默认 1500）③ 目录数上限 4096。
 *   UDF 规范里目录是树，但坏镜像/构造镜像可以有回指 —— 三条上限一起兜底。*/
typedef struct { char *b; size_t cap, len; int n; unsigned max; bool trunc;
                 int dirs; const char *find; } isolist_t;
typedef struct { char **v; int n, cap; bool bad; } isolist_subs;

static bool istr_has(const char *hay, const char *needle){
    if(!needle||!*needle) return true;
    size_t nl=strlen(needle);
    for(const char *p=hay; *p; p++)
        if(!strncasecmp(p,needle,nl)) return true;
    return false;
}
static void subs_push(isolist_subs *s, const char *nm){
    if(s->n>=s->cap){
        int nc=s->cap? s->cap*2 : 16;
        if(nc>8192) nc=8192;
        char **nv=realloc(s->v,(size_t)nc*sizeof *nv);
        if(!nv){ s->bad=true; return; }
        s->v=nv; s->cap=nc;
    }
    if(s->n>=s->cap){ s->bad=true; return; }
    s->v[s->n]=strdup(nm);
    if(!s->v[s->n]){ s->bad=true; return; }
    s->n++;
}
/* JSON 字符串转义（与 wim/iso 其它地方同一套规矩）*/
static int iso_jesc(char *dst, size_t cap, const char *s){
    int k=0;
    for(const unsigned char *p=(const unsigned char*)s; p&&*p && k<(int)cap-8; p++){
        if(*p=='"'||*p=='\\'){ dst[k++]='\\'; dst[k++]=(char)*p; }
        else if(*p<0x20) k+=snprintf(dst+k,cap-(size_t)k,"\\u%04x",*p);
        else dst[k++]=(char)*p;
    }
    dst[k]=0;
    return k;
}
static void iso_scpy(char *d, size_t cap, const char *s){
    if(!cap) return;
    size_t n=strlen(s?s:"");
    if(n>=cap) n=cap-1;
    memcpy(d,s,n); d[n]=0;
}
/* dir + "/" + name，超长截断（截断只会让路径显示不全，不会越界）*/
static void iso_join(char *out, size_t cap, const char *dir, const char *name){
    iso_scpy(out,cap,dir);
    size_t n=strlen(out);
    if(n+1<cap && strcmp(dir,"/")) out[n++]='/';
    iso_scpy(out+n,cap-n,name);
}
typedef struct { isolist_t *a; isolist_subs subs; char dir[1400]; } iso_dir_ctx;
static void iso_list_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    iso_dir_ctx *c=(iso_dir_ctx*)ud;
    isolist_t *a=c?c->a:NULL;
    if(!a || !name) return;
    char full[1400];
    iso_join(full,sizeof full,c->dir,name);
    if(is_dir) subs_push(&c->subs,name);
    if(a->n>=(int)a->max){ a->trunc=true; return; }
    if(a->find && !istr_has(full,a->find)) return;
    char nm[600], fp[1400], item[2200];
    iso_jesc(nm,sizeof nm,name);
    iso_jesc(fp,sizeof fp,full);
    int k=snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"path\":\"%s\",\"dir\":%s,\"size\":%llu}",
                   a->n?",":"",nm,fp,is_dir?"true":"false",(unsigned long long)size);
    if(k>0 && a->len+(size_t)k+1 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=(size_t)k; a->n++; }
    else a->trunc=true;
}
static int iso_ls_rec(img_iso *iso, const char *dir, int depth, int maxdepth, isolist_t *a){
    if(depth>maxdepth || a->n>=(int)a->max || a->dirs>=4096) return IMG_OK;
    if(strlen(dir)>=sizeof(((iso_dir_ctx*)0)->dir)-300) return IMG_OK;  /* 路径过长：不再下钻 */
    iso_dir_ctx *c=calloc(1,sizeof *c);
    if(!c) return IMG_ENOMEM;
    iso_scpy(c->dir,sizeof c->dir,dir);
    c->a=a;
    a->dirs++;
    int cnt=img_iso_list(iso,dir,iso_list_cb,c);
    int rc=(cnt<0)?cnt:IMG_OK;
    for(int i=0;i<c->subs.n && rc==IMG_OK;i++){
        if(a->n>=(int)a->max){ a->trunc=true; break; }
        char sub[1400];
        iso_join(sub,sizeof sub,dir,c->subs.v[i]);
        rc=iso_ls_rec(iso,sub,depth+1,maxdepth,a);
    }
    for(int i=0;i<c->subs.n;i++) free(c->subs.v[i]);
    free(c->subs.v);
    free(c);
    return rc;
}
static const img_arg A_ISOLS[] = {
    {"iso",  IMG_A_STR, true,  NULL, "ISO 文件路径（UDF bridge；不修改它）", NULL},
    {"path", IMG_A_STR, false, "/",  "ISO 内的目录（大小写不敏感）", NULL},
    {"recursive", IMG_A_BOOL, false, "false", "递归列出所有子目录（默认只列一层）", NULL},
    {"find", IMG_A_STR, false, NULL, "按路径子串过滤（不区分大小写；给了它就自动递归）", NULL},
    {"max",  IMG_A_U64, false, "1500", "最多返回多少项（1~4000）", NULL},
    {"depth",IMG_A_U64, false, "32",   "递归最大深度（1~64；防病态镜像）", NULL},
};
/* img_iso_open_* 的 rc → 错误码：以前一律 HOST-OPEN-FAIL，把“不是 ISO”也说成“打不开”，
   真机排查时被带偏过一次，这里按语义分开。 */
static const char *iso_open_code(int rc){
    switch(rc){
    case IMG_ENOENT:  return "HOST-OPEN-FAIL";   /* 文件根本不存在/不是普通文件 */
    case IMG_EIO:     return "ISO-READ-FAIL";    /* 能打开但读不出内容 */
    case IMG_ENOMEM:  return "ISO-READ-FAIL";    /* 内存不足，也归读不出 */
    default:          return "ISO-FORMAT-BAD";   /* 认不出 UDF/ISO9660 */
    }
}

static int cmd_iso_ls(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *isop=img_args_get(in,"iso");
    const char *path=img_args_get(in,"path"); if(!path||!*path) path="/";
    const char *find=img_args_get(in,"find"); if(find && !*find) find=NULL;
    bool rec=img_args_bool(in,"recursive",false) || (find!=NULL);
    uint64_t max=img_args_u64(in,"max",1500); if(max<1) max=1; if(max>4000) max=4000;
    uint64_t depth=img_args_u64(in,"depth",32); if(depth<1) depth=1; if(depth>64) depth=64;
    char err[256]="";
    img_iso *iso=NULL;
    int rc=img_iso_open_path(&iso,isop,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,iso_open_code(rc),isop,err[0]?err:"无法打开 ISO",NULL); return rc; }
    /* ★ 线程局部（原来是不带 TLS 的进程级 static）：iso.ls 无 ctx、允许并行，
     *   进程级缓冲会被并发命令互相覆盖 —— 症状是 data.count 与 entries 元素数不符，
     *   或失败信封里混进别人的条目（race_test 场景③ 就是抓这个的）。
     *   不能换 per-call malloc：img_result_raw() 只存指针，命令函数里不能 free。
     *   raw 类型必须是【一个完整的 JSON 值】：条目之间只有逗号不够，得包成数组。*/
    IMG_THREAD_LOCAL static char jb[256*1024];
    jb[0]='['; isolist_t acc={jb,sizeof jb-2,1,0,(unsigned)max,false,0,find};
    img_span sp; img_span_prep(&sp,"iso","ls","列 ISO 目录");
    img_span_kv_str(&sp,"path",path); img_span_kv_str(&sp,"find",find?find:"");
    img_span_begin(&sp);
    if(rec){
        rc=iso_ls_rec(iso,path,0,(int)depth,&acc);
        if(rc==IMG_OK && acc.n==0 && !find){ rc=IMG_ENOENT; }   /* 目录不存在：与单层语义一致 */
    } else {
        iso_dir_ctx *c=calloc(1,sizeof *c);
        if(!c){ img_iso_close(iso); img_result_fail(out,IMG_ENOMEM,"ISO-READ-FAIL",path,"内存不足",NULL); return IMG_ENOMEM; }
        snprintf(c->dir,sizeof c->dir,"%s",path);
        c->a=&acc;
        int n=img_iso_list(iso,path,iso_list_cb,c);
        for(int i=0;i<c->subs.n;i++) free(c->subs.v[i]);
        free(c->subs.v); free(c);
        rc=(n<0)?IMG_ENOENT:IMG_OK;
    }
    img_span_end(&sp, rc);
    const char *vol=img_iso_volume_id(iso);
    img_iso_close(iso);
    if(rc!=IMG_OK){ img_result_fail(out,IMG_ENOENT,"FAT-DIR-NOTFOUND",path,"ISO 里没有这个目录",NULL); return IMG_ENOENT; }
    jb[acc.len++]=']'; jb[acc.len]=0;
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"truncated",acc.trunc);
    img_result_bool(out,"recursive",rec);
    img_result_u64(out,"dirs_scanned",(uint64_t)acc.dirs);
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
    if(rc!=IMG_OK){ img_result_fail(out,rc,iso_open_code(rc),isop,err[0]?err:"无法打开 ISO",NULL); return rc; }
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
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,false);      /* 只读：导出不该改盘 */
    if(!fs){ img_result_fail(out,IMG_ESTATE,"FAT-MOUNT-FAIL",path,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    img_span sp; img_span_prep(&sp,"fat32","export","导出 FAT32 文件");
    img_span_kv_str(&sp,"path",path); img_span_begin(&sp);
    uint64_t got=0;
    rc=img_fat32_copy_out(fs,path,dst,img_cmd_prog(),&got);
    img_fat32_unmount(fs);
    img_span_kv_u64(&sp,"written",got);
    img_span_end(&sp,rc);
    if(rc!=IMG_OK){
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"FAT-ENTRY-NOTFOUND";
        img_result_fail(out,rc,code,path,(rc==IMG_ECANCELLED)?"已取消":"文件不存在或无法读取",NULL);
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_u64(out,"bytes",got);
    return IMG_OK;
}

/* ---------- fat.rm（删文件 / 空目录；不递归 —— 递归删是政策，不是机制）----------
 * 与 ntfs.rm 对齐：目录非空一律拒绝（IMG_ENOTSUP），让 App 明确"先删子项"，
 * 而不是工具替它猜。底层 img_fat32_rm 已存在多年，这条命令只是把它暴露出来。*/
static const img_arg A_FATRM[] = {
    {"img",  IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true, NULL, "FAT32 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true, NULL, "分区内要删除的绝对路径（/ 开头）", NULL},
    {"recursive", IMG_A_BOOL, false, "false",
     "★默认 false：目录只有【空】的时候才删得掉。true=连子项一起递归删（危险）", NULL},
};
static int cmd_fat_rm(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","需要 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_fat32_fs *fs=img_fat32_mount(q,lba,true);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"FAT-MOUNT-FAIL",path,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    bool rec=img_args_bool(in,"recursive",false);
    int removed=0;
    int rc = rec ? img_fat32_rm_tree(fs,path,&removed) : img_fat32_rm(fs,path);
    if(rc==IMG_OK && !rec) removed=1;
    img_fat32_unmount(fs);
    if(rc==IMG_ENOENT){
        img_result_fail(out,rc,"FAT-ENTRY-NOTFOUND",path,"路径不存在（大小写不敏感，但路径必须完整）","先用 fat.ls 核对父目录");
        return rc;
    }
    if(rc==IMG_ENOTSUP||rc==IMG_ESTATE){
        img_result_fail(out,rc,"FAT-DIR-NOT-EMPTY",path,
            "目录非空（默认不递归）","要连子项一起删请显式传 recursive=true");
        return rc;
    }
    if(rc!=IMG_OK){
        img_result_fail(out,rc,rc==IMG_ECORRUPT?"PART-FS-BAD":"FAT-RM-FAIL",path,"删除失败（卷损坏或簇链成环）","跑 qc.verify / 换备份镜像");
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_u64(out,"removed",(uint64_t)removed);
    return IMG_OK;
}

/* ---------- fat.move（盘内改名 / 移动，单条路径）----------
 * 目标已存在 → 明确报 FAT-ENTRY-EXISTS，不默认覆盖（覆盖=丢数据，必须 App 显式决定：
 * 先 fat.rm 再 fat.move，两步各自可见）。*/
static const img_arg A_FATMOVE[] = {
    {"img", IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true, NULL, "FAT32 分区起始 LBA", chk_lba},
    {"src", IMG_A_STR, true, NULL, "源绝对路径（/ 开头；可指向文件或目录）", NULL},
    {"dst", IMG_A_STR, true, NULL, "目标绝对路径（父目录必须已存在）", NULL},
};
static int cmd_fat_move(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *src=img_args_get(in,"src"), *dst=img_args_get(in,"dst");
    if(!src||src[0]!='/'||!dst||dst[0]!='/'){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src/dst","两个都得是 / 开头的绝对路径",NULL); return IMG_EINVAL;
    }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_fat32_fs *fs=img_fat32_mount(q,lba,true);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"FAT-MOUNT-FAIL",src,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    /* ★ 先分清楚"源不存在"和"目标父目录不存在"：底层统一返回 ENOENT，
     *   混在一起报会让 App 提示错方向（实测就踩过：以为源丢了，其实是目标目录没建）。*/
    if(!img_fat32_path_exists(fs,src)){
        img_fat32_unmount(fs);
        img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",src,"源路径不存在","先用 fat.ls 核对大小写与整条路径");
        return IMG_ENOENT;
    }
    {   char parent[512]; snprintf(parent,sizeof parent,"%s",dst);
        char *slash=strrchr(parent,'/');
        if(slash){ if(slash==parent) slash[1]=0; else *slash=0; }
        if(!img_fat32_dir_exists(fs,parent)){
            img_fat32_unmount(fs);
            img_result_fail(out,IMG_ENOENT,"FAT-DEST-DIR-NOTFOUND",dst,"目标的父目录不存在（本命令不自动建目录）","先在该位置 fat.put 一个文件把目录建出来，或改目标路径");
            return IMG_ENOENT;
        }
    }
    int rc=img_fat32_rename(fs,src,dst);
    img_fat32_unmount(fs);
    if(rc==IMG_ENOENT){
        img_result_fail(out,rc,"FAT-ENTRY-NOTFOUND",src,"源路径不存在，或目标的父目录不存在","先用 fat.ls 核对两边的父目录");
        return rc;
    }
    if(rc==IMG_EEXISTS){
        img_result_fail(out,rc,"FAT-ENTRY-EXISTS",dst,"目标已经存在（本命令不覆盖）","先 fat.rm 删掉目标，或换一个名字");
        return rc;
    }
    if(rc==IMG_EINVAL){
        img_result_fail(out,rc,"FAT-MOVE-BAD",dst,"源与目标相同，或把目录移进了它自己的子树",NULL);
        return rc;
    }
    if(rc!=IMG_OK){
        img_result_fail(out,rc,rc==IMG_ECORRUPT?"PART-FS-BAD":"FAT-MOVE-FAIL",dst,"移动失败（卷损坏或簇链成环）","跑 qc.verify / 换备份镜像");
        return rc;
    }
    img_result_str(out,"src",src);
    img_result_str(out,"dst",dst);
    return IMG_OK;
}
/* ---------- ntfs.parent（只读：目录的 ".." 父引用自洽检查）----------
 * 为什么需要：NTFS 的"我爹是谁"记在子目录自己的 $I30 ".." 项里，搬目录时忘了同步
 *   就会出现"路径能打开、但 cd .. 跳回旧父"的错乱 —— 光看 ntfs.list 是看不出来的。
 *   上游 libntfs-3g 也没有这个能力（dir.c 里 ntfs_link 的 FIXME 就是这件事），
 *   所以我们自己补了同步，就必须自己补一个能证明它生效的检查。
 * 三项：parent_ino（.. 指向谁）、parent_by_path（按路径算的父是谁）、
 *   backlink_ok（父索引里这个名字是否指回本目录）；consistent = 前两者相等且 backlink_ok。*/
static const img_arg A_NTFSRPARENT[] = {
    {"img",  IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true, NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true, NULL, "要检查的目录（/ 开头）", NULL},
};
static int cmd_ntfs_parent(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（目录父引用检查）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是 / 开头的盘内绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    uint64_t ino=0,pino=0,pby=0; bool back=false,haddd=false;
    int pr=img_ntfs_dir_parent_check(fs,path,&ino,&pino,&pby,&back,&haddd);
    img_ntfs_umount(fs);
    if(pr==IMG_ENOENT){ img_result_fail(out,pr,"NTFS-PATH-NOTFOUND",path,"路径不存在",NULL); return pr; }
    if(pr==IMG_ENOTSUP){ img_result_fail(out,pr,"NTFS-NOT-A-DIR",path,"这是个文件，不是目录（文件没有父引用这回事）",NULL); return pr; }
    if(pr!=IMG_OK){ img_result_fail(out,pr,"NTFS-PARENT-FAIL",path,"读不到 .. 项（索引可能已损坏）",NULL); return pr; }
    bool consistent = (pino==pby) && back;
    img_result_str(out,"path",path);
    img_result_u64(out,"ino",ino);
    img_result_u64(out,"parent_ino",pino);
    img_result_u64(out,"parent_by_path",pby);
    img_result_bool(out,"backlink_ok",back);
    img_result_bool(out,"dotdot_present",haddd);
    img_result_bool(out,"consistent",consistent);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}

/* ================= 恢复分区 / WinRE 四件套（2026-10-20，A2）=================
 * 政策仍是"默认不建"：这些命令只在 App 明确勾选后调用（DESIGN-OUTPUT §十一）。*/

/* ---------- part.attr（设 GPT 分区属性；恢复分区要 0x8000000000000001）---------- */
static const img_arg A_PARTATTR[] = {
    {"img",  IMG_A_STR, true, NULL, "镜像路径（qcow2，必须是 GPT）", NULL},
    {"lba",  IMG_A_U64, true, NULL, "目标分区起始 LBA", chk_lba},
    {"attr", IMG_A_STR, true, NULL, "属性值（十六进制，如 0x8000000000000001）", NULL},
};
static int cmd_part_attr(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *as=img_args_get(in,"attr");
    if(!as||!*as){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","attr","需要属性值（0x... 或纯十六进制）",NULL); return IMG_EINVAL; }
    char *end=NULL;
    uint64_t attr=strtoull(as,&end,0);
    if(end && *end){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","attr","不是合法十六进制数",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    int rc=img_part_set_attributes(q,lba,attr);
    if(rc==IMG_ENOTSUP){ img_result_fail(out,rc,"CMD-ARG-BAD","img","MBR 分区没有 GPT attributes（只有 GPT 才有）",NULL); return rc; }
    if(rc!=IMG_OK){ img_result_fail(out,rc,"PART-WRITE-FAIL",NULL,"设属性失败",NULL); return rc; }
    img_result_u64(out,"lba",lba);
    img_result_str(out,"attributes",as);
    return IMG_OK;
}

/* ---------- winre.find（只读：在 WIM/ESD 里定位 Winre.wim）---------- */
static const img_arg A_WINREFIND[] = {
    {"src",   IMG_A_STR, true,  NULL, "WIM/ESD 路径（ISO 由 iso.extract 先抽出来）", NULL},
    {"index", IMG_A_U64, false, "0",  "映像索引（0=扫全部）", NULL},
};
static int cmd_winre_find(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 解析）","重新编译时开启该功能并安装对应系统库");
    const char *src=img_args_get(in,"src");
    int idx=(int)img_args_u64(in,"index",0);
    img_winre_info_t info;
    int rc=img_winre_find(src,idx,&info);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"WIM-NOTFOUND",src,"WIM/ESD 无法打开",NULL); return rc; }
    if(!info.found){
        img_result_fail(out,IMG_ENOENT,"WINRE-NOTFOUND",src,
            "这个文件里没有 Winre.wim（请让用户换一个包含 WinRE 的 ISO/ESD/WIM）",NULL);
        return IMG_ENOENT;
    }
    img_result_bool(out,"found",true);
    img_result_u64(out,"index",(uint64_t)info.index);
    img_result_str(out,"container",info.container);
    img_result_str(out,"internal_path",info.internal_path);
    img_result_u64(out,"size_bytes",info.size_bytes);
    return IMG_OK;
}

/* ---------- recovery.populate / recovery.populate-system ---------- */
static const img_arg A_RECPOP[] = {
    {"img",        IMG_A_STR, true,  NULL,    "镜像路径（qcow2）", NULL},
    {"rec-lba",    IMG_A_U64, true,  NULL,    "恢复分区起始 LBA（UEFI：独立恢复分区；BIOS：填系统分区）", chk_lba},
    {"winre-src",  IMG_A_STR, true,  NULL,    "WinRE 来源 WIM/ESD", NULL},
    {"winre-index",IMG_A_U64, false, "1",     "WIM/ESD 里的映像索引", NULL},
    {"winre-path", IMG_A_STR, false, NULL,    "WIM 内路径（缺省按标准两处找）", NULL},
    {"bootsdi-src",IMG_A_STR, false, NULL,    "boot.sdi 本地路径（可选）", NULL},
    {"format",     IMG_A_BOOL,false, "false", "先格式化目标分区（★会毁数据）", NULL},
    {"label",      IMG_A_STR, false, "Recovery","（format=true）卷标", NULL},
    {"set-gpt-attr",IMG_A_BOOL,false,"true",  "设 GPT 属性 0x8000000000000001", NULL},
    {"enable-script",IMG_A_BOOL,false,"false","再写【首次开机 reagentc /enable】脚本", NULL},
    {"os-lba",     IMG_A_U64, false, "0",     "系统分区 LBA（enable-script 用）", NULL},
};
static int rec_populate_common(img_ctx *ctx, const img_args *in, img_result *out, bool bios_layout){
    if(!IMG_WITH_NTFS3G || !IMG_WITH_WIM)
        return feat_unavail(out,"libntfs-3g + wimlib（恢复分区填充）","重新编译时开启 IMG_WITH_NTFS3G/IMG_WITH_WIM");
    uint64_t lba=img_args_u64(in,"rec-lba",0);
    const char *src=img_args_get(in,"winre-src");
    int idx=(int)img_args_u64(in,"winre-index",1);
    const char *wpath=img_args_get(in,"winre-path");
    const char *sdi=img_args_get(in,"bootsdi-src");
    bool do_fmt=img_args_bool(in,"format",false);
    const char *label=img_args_get(in,"label");
    bool set_attr=img_args_bool(in,"set-gpt-attr",true);
    bool ena=img_args_bool(in,"enable-script",false);
    uint64_t os_lba=img_args_u64(in,"os-lba",0);

    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","rec-lba","该 LBA 不在分区表里（先 img.create --rec-mb 或 part.layout --rec-gb）",NULL); return IMG_EINVAL; }

    char err[256]=""; uint64_t wb=0;
    rc=img_recovery_populate(q,lba,pb,src,idx,wpath,sdi,do_fmt,label,bios_layout,
                             img_cmd_prog(),&wb,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"REC-VERIFY-FAIL",src,err[0]?err:"填充失败",NULL); return rc; }

    bool attr_ok=false;
    if(set_attr){
        uint64_t want = 0x8000000000000001ULL;
        int ar=img_part_set_attributes(q,lba,want);
        if(ar==IMG_ENOTSUP) attr_ok=false;          /* MBR：没有属性，不算错 */
        else if(ar==IMG_OK) attr_ok=true;
    }
    if(ena){
        if(!os_lba){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","os-lba","enable-script=true 时必须给系统分区 LBA",NULL); return IMG_EINVAL; }
        uint64_t osb=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==os_lba){ osb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
        size_t nb=0;
        int sr=img_winre_write_enable_script(q,os_lba,osb,&nb);
        if(sr!=IMG_OK){ img_result_fail(out,sr,"FAT-PUT-FAIL","/Windows/Setup/Scripts","脚本写入失败",NULL); return sr; }
        img_result_u64(out,"script_bytes",(uint64_t)nb);
    }
    img_result_u64(out,"rec_lba",lba);
    img_result_u64(out,"winre_bytes",wb);
    img_result_bool(out,"gpt_attr_set",attr_ok);
    img_result_str(out,"layout",bios_layout?"system":"dedicated");
    return IMG_OK;
}
static int cmd_recovery_populate(img_ctx *ctx, const img_args *in, img_result *out){
    return rec_populate_common(ctx,in,out,false);     /* 独立恢复分区（UEFI）*/
}
static int cmd_recovery_populate_system(img_ctx *ctx, const img_args *in, img_result *out){
    return rec_populate_common(ctx,in,out,true);      /* 系统分区内（BIOS）*/
}


/* ---------- bcd.build（从模板直接构建；Win7 的 BCD-Template 没有绑定，只能这么来）---------- */
static const img_arg A_BCDBUILD[] = {
    {"template", IMG_A_STR, true,  NULL,   "模板 BCD（如 Win7 的 \\Windows\\System32\\config\\BCD-Template）", NULL},
    {"out",      IMG_A_STR, true,  NULL,   "输出 BCD 文件路径", NULL},
    {"table",    IMG_A_ENUM,false, "mbr",  "引导方式：mbr(BIOS) | gpt(UEFI)", NULL},
    {"desc",     IMG_A_STR, false, "Windows","菜单里显示的名字", NULL},
    {"sig",      IMG_A_STR, false, NULL,   "（mbr）磁盘签名 8 位十六进制", NULL},
    {"sys-lba",  IMG_A_U64, false, "0",    "（mbr）系统分区起始 LBA", NULL},
    {"sr-lba",   IMG_A_U64, false, "0",    "（mbr）系统保留/活动分区 LBA", NULL},
    {"sys-guid", IMG_A_STR, false, NULL,   "（gpt）系统分区 partGuid", NULL},
    {"disk-guid",IMG_A_STR, false, NULL,   "（gpt）磁盘 GUID", NULL},
    {"esp-guid", IMG_A_STR, false, NULL,   "（gpt）ESP partGuid", NULL},
};
static int cmd_bcd_build(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *tpl=img_args_get(in,"template"), *outp=img_args_get(in,"out");
    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="mbr";
    bool uefi=!strcasecmp(tb,"gpt"), mbr=!strcasecmp(tb,"mbr");
    if(!tpl||!outp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","template/out","两个都必填",NULL); return IMG_EINVAL; }
    if(!uefi && !mbr){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","table","只接受 mbr | gpt",NULL); return IMG_EINVAL; }

    img_bcd_build_req_t rq; memset(&rq,0,sizeof rq);
    rq.template_path=tpl; rq.out_path=outp; rq.uefi=uefi; rq.desc=img_args_get(in,"desc");
    rq.sys_off=img_args_u64(in,"sys-lba",0)*512ull;
    rq.sr_off =img_args_u64(in,"sr-lba",0)*512ull;
    rq.sys_guid=img_args_get(in,"sys-guid");
    rq.disk_guid=img_args_get(in,"disk-guid");
    rq.esp_guid=img_args_get(in,"esp-guid");
    const char *sig=img_args_get(in,"sig");
    if(sig && *sig) rq.mbr_sig=(uint32_t)strtoul(sig,NULL,16);
    if(!uefi && (!rq.sys_off || !rq.mbr_sig)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sig/sys-lba","BIOS 构建需要磁盘签名与系统分区 LBA",NULL);
        return IMG_EINVAL;
    }
    char err[256]="";
    int rc=img_bcd_build(&rq,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-PATCH-FAIL",tpl,err[0]?err:"构建失败",NULL); return rc; }
    img_result_str(out,"out",outp);
    img_result_str(out,"mode",uefi?"gpt":"mbr");
    return IMG_OK;
}

/* ---------- bcd.create：内置模板直接建 BCD（不需要任何参考盘）----------
 * 用户要的："兼容 Vista 到 Windows 11 全部版本，32/64 位，MBR 与 GPT，
 *            全量保证能启动；最好一个统一模板，不行就多套几个"。 */
static const img_arg A_BCDCREATE[] = {
    {"img",         IMG_A_STR,  true,  NULL,    "镜像路径（qcow2）", NULL},
    {"table",       IMG_A_ENUM, false, "auto",  "引导方式：auto(按分区表) | mbr | gpt", NULL},
    {"sys-lba",     IMG_A_U64,  false, NULL,    "系统分区 LBA（缺省=自动找最大的 NTFS/Windows 分区）", NULL},
    {"sr-lba",      IMG_A_U64,  false, NULL,    "（BIOS）系统保留/活动分区 LBA（缺省=活动分区）", NULL},
    {"esp-lba",     IMG_A_U64,  false, NULL,    "（UEFI）ESP LBA（缺省=EFI 类型分区）", NULL},
    {"desc",        IMG_A_STR,  false, "Windows", "启动菜单里显示的名字", NULL},
    {"locale",      IMG_A_STR,  false, "zh-CN", "BCD locale", NULL},
    {"timeout",     IMG_A_U64,  false, "5",     "菜单等待秒数", NULL},
    {"family",      IMG_A_ENUM, false, "auto",  "代：auto(按盘里内核版本) | legacy(6.x) | modern(10.x)", NULL},
    {"keep-recovery", IMG_A_BOOL, false, "false", "保留模板里的 WinRE/ramdisk 对象（默认删，因为我们不建恢复分区）", NULL},
    {"force",       IMG_A_BOOL, false, "false", "跳过分区表兼容性拦截（App 弹框确认后才传 true）", NULL},
    {"out",         IMG_A_STR,  false, NULL,    "另外存一份到本地文件（可选，便于对比/排查）", NULL},
};
static uint64_t pb_of(const img_parttable_t *t, uint64_t lba){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba)
            return (t->parts[i].last_lba-t->parts[i].first_lba+1)*512ull;
    return 0;
}

/* 位数判定：① 首选内核 PE 的 Machine 字段（最硬，改不了）；
 *           ② 其次 SysWOW64 是否存在（精简过的盘可能不准确，只当兜底）。*/
static img_plan_arch arch_from_ver(const img_winver_t *v, img_qcow2 *q,
                                   uint64_t lba, uint64_t bytes){
    switch(v->pe_machine){
        case 0x14c:  return IMG_PLAN_ARCH_X86;
        case 0x8664: return IMG_PLAN_ARCH_X64;
        case 0xAA64: return IMG_PLAN_ARCH_ARM64;
        default: break;
    }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,bytes);
    img_plan_arch a=IMG_PLAN_ARCH_UNKNOWN;
    if(fs){
        a = (img_ntfs_path_exists(fs,"/Windows/SysWOW64/ntdll.dll")==true)
            ? IMG_PLAN_ARCH_X64 : IMG_PLAN_ARCH_X86;
        img_ntfs_umount(fs);
    }
    return a;
}

static int cmd_bcd_create(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    if(t->table==IMG_TABLE_NONE){ img_result_fail(out,IMG_EINVAL,"PART-READ-FAIL",NULL,"该镜像还没有分区表","先 imglayout / img.create"); return IMG_EINVAL; }

    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="auto";
    bool uefi = (t->table==IMG_TABLE_GPT);
    if(!strcasecmp(tb,"mbr")) uefi=false;
    else if(!strcasecmp(tb,"gpt")) uefi=true;
    else if(strcasecmp(tb,"auto")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","table","只接受 auto|mbr|gpt",NULL); return IMG_EINVAL; }

    /* ---- 选系统分区 / 引导分区 ---- */
    uint64_t sys_lba=img_args_u64(in,"sys-lba",0);
    if(!sys_lba){
        /* ★ 不能只挑"最大的 NTFS 分区"：实测 Win11 盘上数据分区 78 GiB 比系统分区 50 GiB 大，
         *   按大小挑会挑到数据盘，写出来的 BCD 指着空分区 → 无法启动（而且 family 也判成 fallback）。
         *   正确做法：按大小逐个试，**能读到 \Windows\System32\winload.* 的才是系统分区**。*/
        uint64_t cand[IMG_MAX_PARTITIONS]; int nc=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            const img_partition_t *p=&t->parts[i];
            if(!p->used || p->logical) continue;
            bool isfs = uefi ? (!strncasecmp(p->type_guid,"ebd0a0a2",8)||!p->type_guid[0]) : (p->type==0x07);
            if(!isfs) continue;
            cand[nc++]=(uint64_t)i;
        }
        /* 按大小从大到小 */
        for(int a=0;a<nc;a++) for(int b=a+1;b<nc;b++){
            const img_partition_t *pa=&t->parts[cand[a]], *pb=&t->parts[cand[b]];
            if((pb->last_lba-pb->first_lba) > (pa->last_lba-pa->first_lba)){ uint64_t tmp=cand[a]; cand[a]=cand[b]; cand[b]=tmp; }
        }
        for(int a=0;a<nc && !sys_lba;a++){
            const img_partition_t *p=&t->parts[cand[a]];
            uint64_t pb=(p->last_lba-p->first_lba+1)*512ull;
            img_ntfs_fs *f=img_ntfs_mount_rw(q,p->first_lba,pb);
            if(!f) continue;
            bool iswin = img_ntfs_path_exists(f,"/Windows/System32/winload.exe")==true
                      || img_ntfs_path_exists(f,"/Windows/System32/winload.efi")==true;
            img_ntfs_umount(f);
            if(iswin) sys_lba=p->first_lba;
        }
        if(!sys_lba && nc) sys_lba=t->parts[cand[0]].first_lba;   /* 兜底：最大那个 */
    }
    if(!sys_lba || !pb_of(t,sys_lba)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sys-lba",
                       "自动未找到系统分区，请显式给 --sys-lba（先 part.show 看）",NULL);
        return IMG_EINVAL;
    }

    uint64_t boot_lba=0;
    if(uefi){
        boot_lba=img_args_u64(in,"esp-lba",0);
        if(!boot_lba)
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t->parts[i].used && !strncasecmp(t->parts[i].type_guid,"c12a7328",8))
                    boot_lba=t->parts[i].first_lba;
        if(!boot_lba){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","esp-lba","没有 ESP 分区：UEFI 盘必须先把 ESP 建好",NULL); return IMG_EINVAL; }
    } else {
        boot_lba=img_args_u64(in,"sr-lba",0);
        if(!boot_lba){
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t->parts[i].used && !t->parts[i].logical && (t->parts[i].status&0x80))
                    boot_lba=t->parts[i].first_lba;
        }
        if(!boot_lba) boot_lba=sys_lba;          /* 单分区布局：BCD 和系统同区 */
    }

    /* ---- 代：auto 时读盘里的内核版本（不信文件名）---- */
    int fam=0; const char *fs=img_args_get(in,"family");
    if(fs && !strcasecmp(fs,"legacy")) fam=(int)IMG_BCD_FAM_LEGACY;
    else if(fs && !strcasecmp(fs,"modern")) fam=(int)IMG_BCD_FAM_MODERN;
    else if(fs && strcasecmp(fs,"auto")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","family","只接受 auto|legacy|modern",NULL); return IMG_EINVAL; }

    const char *fam_why="given";

    /* ★ 读一次盘里的"真相"（版本 + 位数），既用来定 BCD 代，也用来做分区表拦截 */
    int omaj=0, omin=0, obld=0; img_plan_arch oarch=IMG_PLAN_ARCH_UNKNOWN;
    { img_winver_t v; char e2[128]="";
      if(img_win_version(q,sys_lba,pb_of(t,sys_lba),&v,e2,sizeof e2)==IMG_OK){
          omaj = v.kernel_major ? v.kernel_major : atoi(v.current_version);
          omin = v.kernel_major ? v.kernel_minor : 0;
          obld = v.consensus_build ? v.consensus_build : v.current_build;
          oarch = arch_from_ver(&v,q,sys_lba,pb_of(t,sys_lba));
          if(fam==0){
              /* 分代不是"Win10 以上"：Vista=6.0、7=6.1 用 legacy；
               * 8=6.2、8.1=6.3、10/11=10.0 都用 modern（BCD 元素相容，bootmgr 是 6.2+）。*/
              bool legacy = (omaj < 6) || (omaj==6 && omin < 2);
              fam = legacy ? (int)IMG_BCD_FAM_LEGACY : (int)IMG_BCD_FAM_MODERN;
              fam_why="win.version";
          }
      } else if(fam==0){ fam=(int)IMG_BCD_FAM_MODERN; fam_why="fallback(无法读取版本，按 modern)"; }
    }

    /* ★ 分区表拦截（三层里的前两层）：
     *    不支持 → 直接拒；能做到但危险 → 要 --force（App 弹框后才给）。
     *    无法读取版本时不拦（新盘还没灌系统，用户自己选的格式，工具不该猜）。*/
    if(obld>0){
        img_boot_plan_in_t pin; memset(&pin,0,sizeof pin);
        pin.os_major=omaj; pin.os_minor=omin; pin.os_build=obld;
        pin.arch=oarch; pin.want_gpt=uefi;
        img_boot_plan_t pl; memset(&pl,0,sizeof pl);
        img_boot_plan(&pin,&pl);
        if(pl.verdict==IMG_PLAN_DENIED){
            img_result_fail(out,IMG_EINVAL,"BOOT-FORMAT-UNSUPPORTED",uefi?"table=gpt":"table=mbr",
                            pl.detail,"换分区表重来（img.create --table=...）");
            return IMG_EINVAL;
        }
        if(pl.verdict==IMG_PLAN_NEEDS_CONFIRM && !img_args_bool(in,"force",false)){
            img_result_fail(out,IMG_ENOTSUP,"BOOT-FORMAT-NEEDS-CONFIRM",uefi?"table=gpt":"table=mbr",
                            pl.detail,"用户确认后带 --force=true 再调一次");
            return IMG_ENOTSUP;
        }
    }

    img_bcd_create_req_t rq; memset(&rq,0,sizeof rq);
    rq.uefi=uefi;
    rq.family=(img_bcd_family)fam;
    rq.desc=img_args_get(in,"desc");
    rq.locale=img_args_get(in,"locale");
    rq.timeout=(int)img_args_u64(in,"timeout",5);
    rq.keep_recovery=img_args_bool(in,"keep-recovery",false);
    if(uefi){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!t->parts[i].used) continue;
            if(t->parts[i].first_lba==sys_lba) rq.sys_guid=t->parts[i].part_guid;
            if(t->parts[i].first_lba==boot_lba) rq.esp_guid=t->parts[i].part_guid;
        }
        rq.disk_guid=t->disk_guid;
    } else {
        rq.mbr_sig=t->disk_signature;
        rq.sys_off=sys_lba*512ull;
        rq.sr_off=(boot_lba!=sys_lba)?boot_lba*512ull:0;
    }

    const char *td=img_tmpdir();
    if(!td){ img_result_fail(out,IMG_ENOTSUP,"TMPDIR-UNAVAILABLE",NULL,
                        "没有可写的临时目录，请设置 IMGTOOL_TMPDIR",NULL); return IMG_ENOTSUP; }
    char bcdtmp[528]; snprintf(bcdtmp,sizeof bcdtmp,"%s/imgbcdcreateXXXXXX",td);
    int fd=mkstemp(bcdtmp); if(fd<0){ img_result_fail(out,IMG_EIO,"BCD-BUILD-FAIL",NULL,"建不了临时文件",NULL); return IMG_EIO; }
    close(fd);
    char err[256]="";
    rc=img_bcd_create_file(&rq,bcdtmp,err,sizeof err);
    if(rc!=IMG_OK){ unlink(bcdtmp); img_result_fail(out,rc,"BCD-BUILD-FAIL","internal-template",err[0]?err:"生成失败",NULL); return rc; }
    struct stat st; uint64_t bsz=0;
    if(stat(bcdtmp,&st)==0) bsz=(uint64_t)st.st_size;

    /* ---- 写进盘：BIOS → SR:\Boot\BCD；UEFI → ESP:\EFI\Microsoft\Boot\BCD ---- */
    if(uefi){
        img_fat32_fs *f=img_fat32_mount(q,boot_lba,true);
        if(!f){ unlink(bcdtmp); img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","esp","ESP 不是合法 FAT32",NULL); return IMG_ESTATE; }
        img_fat32_mkdir_p(f,"/EFI");
        img_fat32_mkdir_p(f,"/EFI/Microsoft");
        img_fat32_mkdir_p(f,"/EFI/Microsoft/Boot");
        rc=img_fat32_put_file(f,bcdtmp,"/EFI/Microsoft/Boot/BCD",img_cmd_prog());
        img_fat32_unmount(f);
    } else {
        img_ntfs_fs *f=img_ntfs_mount_rw(q,boot_lba,pb_of(t,boot_lba));
        if(!f){ unlink(bcdtmp); img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","sr","引导分区不是合法 NTFS",NULL); return IMG_ESTATE; }
        img_ntfs_mkdir_p(f,"/Boot");
        rc=img_ntfs_put_file(f,bcdtmp,"/Boot/BCD",img_cmd_prog());
        img_ntfs_umount(f);
    }
    const char *outp=img_args_get(in,"out");
    if(outp && *outp){
        FILE *fp=fopen(outp,"wb");
        if(fp){ FILE *s2=fopen(bcdtmp,"rb"); if(s2){ char b[8192]; size_t r; while((r=fread(b,1,sizeof b,s2))>0) fwrite(b,1,r,fp); fclose(s2);} fclose(fp); }
    }
    unlink(bcdtmp);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",uefi?"/EFI/Microsoft/Boot/BCD":"/Boot/BCD","无法写入引导分区",NULL); return rc; }

    img_result_str(out,"mode",uefi?"gpt/uefi":"mbr/bios");
    img_result_str(out,"family",fam==(int)IMG_BCD_FAM_LEGACY?"legacy(6.x)":"modern(10.x)");
    img_result_str(out,"family_basis",fam_why);
    img_result_u64(out,"sys_lba",sys_lba);
    img_result_u64(out,"boot_lba",boot_lba);
    img_result_str(out,"path",uefi?"/EFI/Microsoft/Boot/BCD":"/Boot/BCD");
    img_result_u64(out,"bcd_bytes",bsz);
    img_result_str(out,"note",img_t("BCD 只解决『引导配置』；bootmgr/bootmgfw.efi 与 MBR 引导码仍需 boot.repair 补"));
    return IMG_OK;
}




/* ================= boot.plan：装前/装后问一句"这套组合行不行" =================
 * 设计（用户 2026-10-21 拍板）：
 *   ① 不支持的（32 位 + UEFI、XP 没有 BCD）→ 工具直接拒，给稳定错误码；
 *   ② 能做到但有风险的（Vista/7 的 UEFI）→ 回 needs_confirm，让 App 弹框，
 *      用户确认后 App 再带 --force 调 bcd.create；
 *   ③ 政策与文案在 App 层，库里零文案（只出 reason 码 + 一句中文 detail 便于调试）。
 */
static const img_arg A_BOOTPLAN[] = {
    {"iso",   IMG_A_STR,  false, NULL,   "ISO / WIM / ESD 路径（装之前问）", NULL},
    {"index", IMG_A_U64,  false, "0",    "只看第 N 个映像（0=第一个）", NULL},
    {"img",   IMG_A_STR,  false, NULL,   "已装好的镜像（装之后问；与 --lba 一起给）", NULL},
    {"lba",   IMG_A_U64,  false, NULL,   "系统分区起始 LBA", NULL},
    {"table", IMG_A_ENUM, false, "auto", "要判定的分区表：gpt | mbr | auto(两种都答)", NULL},
};

/* 从 ISO/WIM/ESD 里读第一个（或第 N 个）映像的版本与架构 */
static int plan_probe_src(const char *src, int want_index,
                          int *maj, int *min, int *bld, img_plan_arch *arch,
                          char *err, size_t errcap){
    *maj=*min=*bld=0; *arch=IMG_PLAN_ARCH_UNKNOWN;
    img_wim_image_t imgs[32]; int n=0;
    size_t sl=strlen(src);
    bool is_iso = (sl>4 && !strcasecmp(src+sl-4,".iso"));
    int rc;
    if(is_iso){
        img_iso *iso=NULL;
        rc=img_iso_open_path(&iso,src,err,errcap);
        if(rc!=IMG_OK) return rc;
        const char *cands[2]={"/sources/install.wim","/sources/install.esd"};
        const char *hit=NULL; uint64_t fsz=0; bool d=false;
        for(int i=0;i<2;i++) if(img_iso_stat(iso,cands[i],&fsz,&d)==IMG_OK && !d){ hit=cands[i]; break; }
        if(!hit){ img_iso_close(iso); snprintf(err,errcap,"ISO 里没有 sources\\install.wim"); return IMG_ENOENT; }
        rd_iso_t r={iso,hit};
        rc=img_wim_identify(rd_iso,&r,imgs,32,&n,err,errcap);
        img_iso_close(iso);
    } else {
        img_io *io=img_io_open_existing(src,false);
        if(!io){ snprintf(err,errcap,"文件无法打开：%s",src); return IMG_ENOENT; }
        rd_io_t r={io};
        rc=img_wim_identify(rd_io,&r,imgs,32,&n,err,errcap);
        img_io_close(io);
    }
    if(rc!=IMG_OK || n<=0) return rc!=IMG_OK?rc:IMG_ENOENT;
    const img_wim_image_t *pick=&imgs[0];
    if(want_index>0){
        for(int i=0;i<n;i++) if(imgs[i].index==want_index){ pick=&imgs[i]; break; }
    }
    *maj=pick->major; *min=pick->minor; *bld=pick->build;
    *arch=img_boot_plan_arch_from_wim(pick->arch);
    return IMG_OK;
}

static int cmd_boot_plan(img_ctx *ctx, const img_args *in, img_result *out){
    const char *iso=img_args_get(in,"iso"), *imgp=img_args_get(in,"img");
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="auto";
    if((!iso||!*iso) && (!imgp||!*imgp)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","iso/img","要么给 --iso（装前问），要么给 --img+--lba（装后问）",NULL);
        return IMG_EINVAL;
    }
    int maj=0,min=0,bld=0; img_plan_arch arch=IMG_PLAN_ARCH_UNKNOWN;
    char err[256]="";
    if(iso && *iso){
        if(plan_probe_src(iso,(int)img_args_u64(in,"index",0),&maj,&min,&bld,&arch,err,sizeof err)!=IMG_OK){
            img_result_fail(out,IMG_ENOENT,"WIM-NOTFOUND",iso,err[0]?err:"无法读取映像版本",NULL); return IMG_ENOENT;
        }
    } else {
        if(!lba){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","--img 要配 --lba",NULL); return IMG_EINVAL; }
        img_qcow2 *q=img_ctx_qcow2(ctx);
        if(!q){ return qc_open_fail(out,ctx); }
        img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
        if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
        uint64_t pb=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==lba) pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512;
        if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }
        img_winver_t v;
        if(img_win_version(q,lba,pb,&v,err,sizeof err)!=IMG_OK){
            img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",NULL,img_t("无法读取版本（不是 NTFS 或没有系统）"),NULL);
            return IMG_ESTATE;
        }
        maj = v.kernel_major ? v.kernel_major : atoi(v.current_version);
        min = v.kernel_minor;
        bld = v.consensus_build ? v.consensus_build : v.current_build;
        arch = arch_from_ver(&v,q,lba,pb);
    }

    const char *archname = arch==IMG_PLAN_ARCH_X86?"x86" : arch==IMG_PLAN_ARCH_X64?"x64"
                        : arch==IMG_PLAN_ARCH_ARM64?"arm64" : "unknown";
    char osver[32]; snprintf(osver,sizeof osver,"%d.%d.%d",maj,min,bld);
    img_boot_plan_in_t pin; memset(&pin,0,sizeof pin);
    pin.os_major=maj; pin.os_minor=min; pin.os_build=bld; pin.arch=arch;

    img_boot_plan_t pm, pg; memset(&pm,0,sizeof pm); memset(&pg,0,sizeof pg);
    pin.want_gpt=false; img_boot_plan(&pin,&pm);
    pin.want_gpt=true;  img_boot_plan(&pin,&pg);

    img_result_str(out,"os_version",osver);
    img_result_str(out,"os_arch",archname);
    if(bld>0){
        img_boot_plan_t tmp; memset(&tmp,0,sizeof tmp);
        pin.want_gpt=(tb && !strcasecmp(tb,"gpt")); img_boot_plan(&pin,&tmp);
        if(tmp.os_name) img_result_str(out,"os_name",tmp.os_name);
    }
    if(tb && !strcasecmp(tb,"gpt")){
        img_result_str(out,"asked","gpt");
        img_result_str(out,"verdict",img_boot_plan_verdict_name(pg.verdict));
        img_result_str(out,"reason",pg.reason);
        img_result_u64(out,"secure_boot",(uint64_t)pg.secure_boot);
    } else if(tb && !strcasecmp(tb,"mbr")){
        img_result_str(out,"asked","mbr");
        img_result_str(out,"verdict",img_boot_plan_verdict_name(pm.verdict));
        img_result_str(out,"reason",pm.reason);
        img_result_u64(out,"secure_boot",(uint64_t)pm.secure_boot);
    } else {
        img_result_str(out,"asked","auto");
        /* auto：两种都答，App 照着灰按钮 */
        img_result_str(out,"mbr_verdict",img_boot_plan_verdict_name(pm.verdict));
        img_result_str(out,"mbr_reason",pm.reason);
        img_result_str(out,"gpt_verdict",img_boot_plan_verdict_name(pg.verdict));
        img_result_str(out,"gpt_reason",pg.reason);
        img_result_u64(out,"gpt_secure_boot",(uint64_t)pg.secure_boot);
        img_result_str(out,"mbr_detail",img_t(pm.detail));
        img_result_str(out,"gpt_detail",img_t(pg.detail));
        img_result_bool(out,"recommend_gpt",pg.recommend_gpt);
    }
    return IMG_OK;
}


/* ---------- qc.verify：qcow2 结构自检（别名/越界/坏 L2）---------- */
static const img_arg A_QCVERIFY[] = {
    {"img", IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
};
static int cmd_qc_verify(img_ctx *ctx, const img_args *in, img_result *out){
    (void)in;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_qcow2_scan_t sc; int rc=img_qcow2_scan(q,&sc);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"QC-OPEN-FAIL",img_ctx_path(ctx),"扫描失败",NULL); return rc; }
    img_result_u64(out,"guest_bytes",sc.guest_bytes);
    img_result_u64(out,"guest_clusters",sc.guest_clusters);
    img_result_u64(out,"l2_tables",sc.l2_tables);
    img_result_u64(out,"referenced",sc.referenced);
    img_result_u64(out,"duplicate_refs",sc.duplicate_refs);
    img_result_u64(out,"out_of_range",sc.out_of_range);
    img_result_u64(out,"bad_l2",sc.bad_l2);
    bool bad = sc.duplicate_refs || sc.out_of_range || sc.bad_l2;
    img_result_bool(out,"clean",!bad);
    if(bad)
        img_result_str(out,"note",img_t("镜像里有别名/越界簇：能读，但请勿写入（会导致数据错乱）。"
                                  "重建或用 qemu-img check 看细节；我们写盘前已自动抬高分配水位。"));
    return IMG_OK;
}

/* ================= 版本识别：从【内容】读，不信文件名 =================
 * 用户点名的需求：挂载 ESD/WIM/ISO 时能直接看到是哪个 Windows 版本，
 * 从而判断"该镜像/该镜像到底是不是我以为的那个"。
 * ⚠ WIM 的 <NAME>/<VERSION> 是文本，第三方可以改；所以 win.version 还会去读
 *   盘里 ntoskrnl.exe 的 PE 版本资源（改了就开不了机）做交叉验证。*/


static const img_arg A_WIMID[] = {
    {"src",   IMG_A_STR, true,  NULL, "WIM / ESD / ISO 路径（ISO 会自动找 sources\\install.wim|esd）", NULL},
    {"index", IMG_A_U64, false, "0",  "只要第 N 个映像（0=全部）", NULL},
};
static int cmd_wim_identify(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *src=img_args_get(in,"src");
    int want=(int)img_args_u64(in,"index",0);
    if(!src||!*src){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","需要镜像路径",NULL); return IMG_EINVAL; }

    img_wim_image_t imgs[32]; int n=0; char err[256]="";
    char container[8]="wim", inner[64]="";
    const char *inner_kind=NULL;

    size_t sl=strlen(src);
    bool is_iso = (sl>4 && !strcasecmp(src+sl-4,".iso"));
    if(is_iso){
        img_iso *iso=NULL;
        int rc=img_iso_open_path(&iso,src,err,sizeof err);
        if(rc!=IMG_OK){ img_result_fail(out,rc,iso_open_code(rc),src,err[0]?err:"ISO 无法打开",NULL); return rc; }
        const char *cands[2]={"/sources/install.wim","/sources/install.esd"};
        const char *hit=NULL; uint64_t fsz=0; bool d=false;
        for(int i=0;i<2;i++){
            if(img_iso_stat(iso,cands[i],&fsz,&d)==IMG_OK && !d){ hit=cands[i]; break; }
        }
        if(!hit){
            img_iso_close(iso);
            img_result_fail(out,IMG_ENOENT,"WIM-NOTFOUND",src,
                "ISO 里没有 sources\\install.wim / install.esd",NULL);
            return IMG_ENOENT;
        }
        /* container 说的是【外层容器】：ISO 就报 iso，别把里面的 install.wim 当容器
         * （App 的文案是"容器 = 光盘镜像 / ESD / WIM"）*/
        snprintf(container,sizeof container,"iso");
        snprintf(inner,sizeof inner,"%s",hit);
        inner_kind = (strstr(hit,".esd")||strstr(hit,"ESD")) ? "esd" : "wim";
        rd_iso_t r={iso,hit};
        rc=img_wim_identify(rd_iso,&r,imgs,32,&n,err,sizeof err);
        const char *vol=img_iso_volume_id(iso);
        if(vol) img_result_str(out,"volume",vol);
        img_iso_close(iso);
        if(rc!=IMG_OK){ img_result_fail(out,rc,"WIM-NOTFOUND",hit,err[0]?err:"解析失败",NULL); return rc; }
    } else {
        img_io *io=img_io_open_existing(src,false);
        if(!io){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",src,"文件无法打开",NULL); return IMG_ENOENT; }
        { const char *dot=strrchr(src,'.'); if(dot && !strcasecmp(dot,".esd")) snprintf(container,sizeof container,"esd"); }
        rd_io_t r={io};
        int rc=img_wim_identify(rd_io,&r,imgs,32,&n,err,sizeof err);
        img_io_close(io);
        if(rc!=IMG_OK){ img_result_fail(out,rc,"WIM-NOTFOUND",src,err[0]?err:"不是 WIM/ESD",NULL); return rc; }
    }

    IMG_THREAD_LOCAL static char jb[64*1024]; size_t k=0;   /* ★ TLS：wim.identify 无 ctx、可并行 */
    k+=(size_t)snprintf(jb+k,sizeof jb-k,"[");
    int emitted=0;
    for(int i=0;i<n;i++){
        if(want>0 && imgs[i].index!=want) continue;
        k+=(size_t)snprintf(jb+k,sizeof jb-k,
            "%s{\"index\":%d,\"name\":\"%s\",\"display_name\":\"%s\",\"edition_id\":\"%s\","
            "\"arch\":\"%s\",\"version\":\"%d.%d.%d\",\"build\":%d,\"languages\":\"%s\","
            "\"dirs\":%d,\"files\":%d,\"total_bytes\":%llu}",
            emitted?",":"", imgs[i].index, imgs[i].name, imgs[i].display_name, imgs[i].edition_id,
            imgs[i].arch, imgs[i].major, imgs[i].minor, imgs[i].build, imgs[i].build,
            imgs[i].languages, imgs[i].dir_count, imgs[i].file_count,
            (unsigned long long)imgs[i].total_bytes);
        emitted++;
        if(k>sizeof jb-2048) break;
    }
    k+=(size_t)snprintf(jb+k,sizeof jb-k,"]");
    img_result_str(out,"container",container);
    img_result_str(out,"source",src);
    if(inner[0]) img_result_str(out,"inner_path",inner);
    if(inner_kind) img_result_str(out,"inner_kind",inner_kind);
    img_result_u64(out,"count",(uint64_t)emitted);
    img_result_raw(out,"images",jb);
    return IMG_OK;
}

/* ---------- win.version：读已装盘的"真相"（内核 PE + 注册表 + SKU）---------- */
static const img_arg A_WINVER[] = {
    {"img", IMG_A_STR, true, NULL, "镜像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true, NULL, "Windows 系统分区起始 LBA", chk_lba},
};
static int cmd_win_version(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（读系统盘里的注册表/内核）","重新编译时开启该功能并安装对应系统库");
    uint64_t lba=img_args_u64(in,"lba",0);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    img_winver_t v; char err[256]="";
    rc=img_win_version(q,lba,pb,&v,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"PART-FS-BAD",NULL,err[0]?err:"无法读取版本",NULL); return rc; }

    img_result_str(out,"registry.product_name",v.product_name);
    img_result_str(out,"registry.edition_id",v.edition_id);
    img_result_str(out,"registry.current_version",v.current_version);
    img_result_u64(out,"registry.current_build",(uint64_t)v.current_build);
    if(v.display_version[0]) img_result_str(out,"registry.display_version",v.display_version);
    if(v.build_lab[0]) img_result_str(out,"registry.build_lab",v.build_lab);

    if(v.kernel_file_version[0]){
        img_result_str(out,"kernel.file_version",v.kernel_file_version);
        img_result_str(out,"kernel.product_version",v.kernel_product_version);
        img_result_u64(out,"kernel.build",(uint64_t)v.kernel_build);
        if(v.kernel_company[0]) img_result_str(out,"kernel.company",v.kernel_company);
        if(v.kernel_description[0]) img_result_str(out,"kernel.description",v.kernel_description);
    }
    { const char *am = v.pe_machine==0x14c ? "x86" : v.pe_machine==0x8664 ? "x64"
                     : v.pe_machine==0xAA64 ? "arm64" : NULL;
      if(am){ img_result_str(out,"arch",am);
              img_result_u64(out,"pe_machine",(uint64_t)v.pe_machine); } }
    if(v.skus[0]) img_result_str(out,"skus",v.skus);
    if(v.servicing_version[0]) img_result_str(out,"servicing_version",v.servicing_version);
    if(v.skipped[0]) img_result_str(out,"unreadable",v.skipped);   /* 存在但无法读取版本（WOF）*/

    /* 多方法投票结果：App 可以直接把这张表显示给用户 */
    { static char ev[2048]; size_t k=0; k+=(size_t)snprintf(ev+k,sizeof ev-k,"[");
      for(int i=0;i<v.n_vote && k<sizeof ev-256;i++)
          k+=(size_t)snprintf(ev+k,sizeof ev-k,"%s{\"source\":\"%s\",\"build\":%d,\"version\":\"%s\"}",
                              i?",":"", v.vote[i].src, v.vote[i].build, v.vote[i].ver);
      k+=(size_t)snprintf(ev+k,sizeof ev-k,"]");
      img_result_u64(out,"evidence_count",(uint64_t)v.n_vote);
      img_result_raw(out,"evidence",ev); }

    if(v.consensus_build) img_result_u64(out,"consensus_build",(uint64_t)v.consensus_build);
    img_result_bool(out,"mismatch",v.mismatch==1);
    if(v.mismatch)
        img_result_str(out,"note",img_t("多个证据源互相矛盾 —— 该镜像被改过（精简/换版本），请勿依据文件名判断"));
    else if(v.n_vote<=1)
        img_result_str(out,"note",img_t("只有 1 个证据源可用（核心文件疑似被 WOF/CompactOS 压缩），结论可信度低"));
    return IMG_OK;
}


/* ================= 命令 6f：ntfs.move（盘内改名 / 移动，任意单条路径）====================
 * 一次覆盖三种诉求：a.txt→b.txt（同目录改名）、/a.txt→/Temp/a.txt（移动）、
 * /Temp/a.txt→/Temp/b.txt（既换目录又换名）。dst 的末段就是新名字。
 * ★ 政策：默认【不覆盖】—— 覆盖会吃掉目标文件，必须 App 显式 overwrite=true。 */
static const img_arg A_NTFSMOVE[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"src",  IMG_A_STR, true,  NULL, "盘内现有路径（/ 开头）", NULL},
    {"dst",  IMG_A_STR, true,  NULL, "盘内新路径（/ 开头；末段不同=改名，父目录不同=移动）", NULL},
    {"overwrite", IMG_A_BOOL, false, "false", "目标已存在时是否覆盖（默认 false=直接报错）", NULL},
    {"allow_dir_move", IMG_A_BOOL, false, "true",
     "目录能否搬到别的父目录（默认 true —— 2026-10-23 真 Windows 验收通过后解禁；传 false 可恢复旧政策）", NULL},
};
#if IMG_WITH_NTFS3G   /* 只在开启 NTFS 时用到；关掉时不该留下 unused 警告 */
/* 把 img_ntfs_move_path 的返回码翻成【能分开病因】的错误码：
 * "源不存在"/"目标已存在"/"目标是目录"/"空间不够"/"底层失败" 各一条 ——
 * 之前踩过"所有失败共用一个码"的坑（排查时分不出打错字还是卷坏了）。 */
static const char *ntfs_move_code(int rc){
    switch(rc){
        case IMG_ENOENT:   return "NTFS-PATH-NOTFOUND";
        case IMG_EEXISTS:  return "NTFS-DEST-EXISTS";
        case IMG_ENOTSUP:  return "NTFS-SRC-IS-DIR";   /* 只在 allow_dir_move=false 或"搬进自己子树"时出现 */
        case IMG_ESTATE:   return "NTFS-DEST-IS-DIR";  /* 目标是目录：不许拿文件顶掉它 */
        case IMG_EFULL:    return "FAT-ENOSPC";
        case IMG_ECORRUPT: return "NTFS-DIR-PHANTOM";  /* 写后自证没过：目录/名字没真落盘 */
        case IMG_ECYCLE:   return "NTFS-MOVE-INTO-SELF"; /* 搬进自己/自己的子树 */
        case IMG_EINVAL:   return "CMD-ARG-BAD";
        default:           return "NTFS-MOVE-FAIL";
    }
}
/* 共用：按 lba 从分区表取分区字节数；取不到返回 0 */
#endif /* IMG_WITH_NTFS3G */
static int cmd_ntfs_move(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 改名/移动）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *src=img_args_get(in,"src"), *dst=img_args_get(in,"dst");
    bool over=img_args_bool(in,"overwrite",false);
    if(!src||src[0]!='/'||!dst||dst[0]!='/'){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src/dst","两个都必须是 / 开头的盘内绝对路径",NULL); return IMG_EINVAL;
    }
    if(!strcmp(src,dst)){ img_result_bool(out,"noop",true); return IMG_OK; }   /* 原地改名给自己：无事可做 */
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=ntfs_part_bytes(t,lba);
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",dst,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    img_span sp; img_span_prep(&sp,"ntfs","move","盘内改名/移动");
    img_span_kv_str(&sp,"src",src); img_span_kv_str(&sp,"dst",dst); img_span_begin(&sp);
    int mr=img_ntfs_move_path(fs,src,dst,over,img_args_bool(in,"allow_dir_move",true));
    img_span_end(&sp,mr);
    /* ★ 校验必须在 umount 之前问（卸载后就问不到了）
     * ★ 只改大小写（a.txt → A.TXT）时"源还在"这个判断没有意义：
     *   NTFS 是大小写不敏感的，源名字查到的就是同一个文件（真机实测：
     *   改名成功了，却被后面的核对判成失败 IMG_ESTATE）。*/
    bool case_only = (strcasecmp(src,dst)==0);
    bool gone  = (mr==IMG_OK) && (case_only || !img_ntfs_path_exists(fs,src));
    bool arrived=(mr==IMG_OK) &&  img_ntfs_path_exists(fs,dst);
    img_ntfs_umount(fs);
    if(mr!=IMG_OK){
        const char *hint = (mr==IMG_ENOENT)?"源路径不存在（先用 ntfs.list 核对大小写与整条路径）":
                           (mr==IMG_EEXISTS)?"目标已存在：换个名字，或明确传 overwrite=true":
                           (mr==IMG_ENOTSUP)?"目录只能在【同一个父目录下改名】，不能搬到别的父目录：真 Windows 实测那样会被 chkdsk 判成孤儿、塞进 found.000（要挪目录请复制+删除，或在 Windows 里做）":
                           (mr==IMG_ESTATE)?"目标是个目录：工具不会用文件顶掉目录":
                           "改名/移动失败（空间不足或卷状态异常）";
        img_result_fail(out,mr,ntfs_move_code(mr),src,hint,dst); return mr;
    }
    if(!(gone&&arrived)){   /* 报成功但状态不符 → 如实报失败，绝不含糊 */
        img_result_fail(out,IMG_ESTATE,"NTFS-MOVE-FAIL",src,"改完核对不上（源还在或目标不在）：建议 qc.verify",dst);
        return IMG_ESTATE;
    }
    img_result_str(out,"src",src);
    img_result_str(out,"dst",dst);
    img_result_bool(out,"moved",true);
    img_result_bool(out,"case_only",case_only);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}

/* ================= 命令 6g：ntfs.rm（删盘内文件）====================
 * ★ 政策：只删【文件】。目录一律拒绝（img_ntfs_rm 内部就拒）——
 *   递归删的误操作比"删不掉"可怕得多。 */
static const img_arg A_NTFSSRM[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true,  NULL, "要删除的盘内路径（文件或目录；/ 开头）", NULL},
    {"recursive", IMG_A_BOOL, false, "false",
     "★默认 false：目录只有【空】的时候才删得掉。true=连子项一起递归删（危险，App 要二次确认）", NULL},
};
static const img_arg A_NTFSCOPY[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"src",  IMG_A_STR, false, NULL, "盘内源文件（/ 开头）；用 list_file 时可省", NULL},
    {"dst",  IMG_A_STR, false, NULL, "盘内目标（/ 开头；给目录或以 / 结尾 ⇒ 沿用源文件名）；用 list_file 时可省", NULL},
    {"conflict", IMG_A_ENUM, false, "fail",
     "同名怎么办：fail=报错（默认） | overwrite=覆盖 | rename=自动改名为 \"名 (1).扩展名\"", NULL},
    {"preserve_times", IMG_A_BOOL, false, "true", "保留源文件的四个时间戳（$SI/$FN）", NULL},
    {"create_parents", IMG_A_BOOL, false, "true", "目标父目录不存在就建", NULL},
    {"list_file", IMG_A_STR, false, NULL,
     "★批量：本地清单文件，每行 `盘内源>盘内目标`（# 开头注释、空行忽略）。"
     "一次挂载全做完，比逐条调用快得多（省掉 N 次 mount）", NULL},
    {"on_fail", IMG_A_ENUM, false, "continue",
     "批量时某项失败怎么办：continue=继续（默认） | stop=立刻停", NULL},
};
static const img_arg A_FATCOPY[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "FAT32 分区起始 LBA", chk_lba},
    {"src",  IMG_A_STR, true,  NULL, "分区内源文件（/ 开头）", NULL},
    {"dst",  IMG_A_STR, true,  NULL, "分区内目标（/ 开头；给目录或以 / 结尾 ⇒ 沿用源文件名）", NULL},
    {"conflict", IMG_A_ENUM, false, "fail",
     "同名怎么办：fail=报错（默认） | overwrite=覆盖 | rename=自动改名为 \"名 (1).扩展名\"", NULL},
};
/* ---------- 盘内复制（NTFS） ---------- */
#if IMG_WITH_NTFS3G
static void count_entry_cb(void *ud, const char *n, bool d, uint64_t sz){
    (void)n; (void)d; (void)sz; (*(int*)ud)++;
}
/* 取某路径的父目录 + 目录内条目数（App 局部刷新用：changed[] 的最小信息） */
static void copy_parent_info(img_ntfs_fs *fs, const char *abs, char *pout, size_t pcap, int *total){
    snprintf(pout, pcap, "%s", abs);
    char *sl = strrchr(pout, '/');
    if (sl && sl != pout) *sl = 0; else snprintf(pout, pcap, "/");
    int n = 0;
    if (img_ntfs_list_dir(fs, pout, count_entry_cb, &n) == IMG_OK && total) *total = n;
}
#endif

/* ================= 时间戳设置（ntfs.touch / fat.touch） =================
 * 语义照 edk2 那套（用户给的资料）：**传 0 = 该项保持原值**，不更新。
 * 三种取值来源（三选一）：--now（现在）/ --time=<unix 秒> / --ref=<盘内另一路径，抄它的时间）。
 * --set=all|create|modify|access 选要动哪几个（默认 all）。*/
static const img_arg A_NTFSTOUCH[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true,  NULL, "盘内目标路径（/ 开头）", NULL},
    {"now",  IMG_A_BOOL, false, "false", "把时间设成【现在】", NULL},
    {"time", IMG_A_U64, false, "0",     "把时间设成这个 unix 秒（0=不改）", NULL},
    {"ref",  IMG_A_STR, false, NULL,    "抄这个盘内路径的时间（clone 时间戳）", NULL},
    {"set",  IMG_A_ENUM, false, "all",  "改哪些：all | create | modify | access", NULL},
};
static const img_arg A_FATTOUCH[] = {
    {"img",  IMG_A_STR, true,  NULL, "镜像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "FAT32 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true,  NULL, "分区内目标路径（/ 开头）", NULL},
    {"now",  IMG_A_BOOL, false, "false", "把时间设成【现在】", NULL},
    {"time", IMG_A_U64, false, "0",     "把时间设成这个 unix 秒（0=不改）", NULL},
    {"ref",  IMG_A_STR, false, NULL,    "抄这个分区内路径的时间", NULL},
    {"set",  IMG_A_ENUM, false, "all",  "改哪些：all | create | modify | access", NULL},
};

#if IMG_WITH_NTFS3G
static void touch_pick(const char *kind, uint64_t v[4]){
    int mode = 0;                             /* 0=all 1=create 2=modify 3=access */
    if (kind) {
        if (!strcmp(kind,"create")) mode = 1;
        else if (!strcmp(kind,"modify")) mode = 2;
        else if (!strcmp(kind,"access")) mode = 3;
    }
    if (mode == 0) return;
    /* v = [create, modify, access, mft_change]；把不想动的清零（0=不改） */
    for (int i = 0; i < 3; i++)
        if (i != mode - 1) v[i] = 0;
}
#endif

static int cmd_ntfs_touch(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 时间戳）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","需要 / 开头的盘内绝对路径",NULL); return IMG_EINVAL; }
    bool want_now=img_args_bool(in,"now",false);
    uint64_t t=img_args_u64(in,"time",0);
    const char *ref=img_args_get(in,"ref");
    img_qcow2 *q=img_ctx_qcow2(ctx); if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *tb=NULL; int rc=img_ctx_parttable(ctx,&tb);
    if(rc||!tb) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=ntfs_part_bytes(tb,lba);
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }

    uint64_t v[4]={0,0,0,0};
    if(ref){
        uint64_t rt[4]={0,0,0,0};
        if(img_ntfs_file_times(fs,ref,rt)!=IMG_OK){
            img_ntfs_umount(fs);
            img_result_fail(out,IMG_ENOENT,"NTFS-PATH-NOTFOUND",ref,"--ref 指向的路径不存在",NULL);
            return IMG_ENOENT;
        }
        memcpy(v,rt,sizeof v);
    } else if(want_now || t){
        uint64_t x = want_now ? (uint64_t)time(NULL) : t;
        v[0]=v[1]=v[2]=x;                      /* 创建/修改/访问都设；mft_change 不碰 */
    } else {
        img_ntfs_umount(fs);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","now","要给 --now / --time= / --ref= 之一（都不给就不知道设成什么）",NULL);
        return IMG_EINVAL;
    }
    touch_pick(img_args_get(in,"set"), v);
    rc = img_ntfs_set_times(fs,path,v);
    uint64_t after[4]={0,0,0,0};
    if(rc==IMG_OK) (void)img_ntfs_file_times(fs,path,after);
    img_ntfs_umount(fs);
    if(rc){
        img_result_fail(out,rc,rc==IMG_ENOENT?"NTFS-PATH-NOTFOUND":"NTFS-TOUCH-FAIL",path,"设置时间失败",NULL);
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_u64(out,"ctime",after[0]);
    img_result_u64(out,"mtime",after[1]);
    img_result_u64(out,"atime",after[2]);
    img_result_u64(out,"mft_change",after[3]);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}

static int cmd_fat_touch(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","需要 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    bool want_now=img_args_bool(in,"now",false);
    uint64_t t=img_args_u64(in,"time",0);
    const char *ref=img_args_get(in,"ref");
    img_qcow2 *q=img_ctx_qcow2(ctx); if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *tb=NULL; int rc=img_ctx_parttable(ctx,&tb);
    if(rc||!tb) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=ntfs_part_bytes(tb,lba);
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,true);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    uint16_t cur[5]={0,0,0,0,0};
    if(img_fat32_file_times(fs,path,cur)!=IMG_OK){
        img_fat32_unmount(fs);
        img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",path,"路径不存在",NULL);
        return IMG_ENOENT;
    }
    int64_t x = 0;
    if(ref){
        uint16_t r5[5]={0,0,0,0,0};
        if(img_fat32_file_times(fs,ref,r5)!=IMG_OK){ img_fat32_unmount(fs);
            img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",ref,"--ref 指向的路径不存在",NULL); return IMG_ENOENT; }
        memcpy(cur,r5,sizeof cur);             /* 整体抄过来，不走下面的"只改一项" */
        x = -1;
    } else if(want_now || t){
        x = want_now ? (int64_t)time(NULL) : (int64_t)t;
    } else {
        img_fat32_unmount(fs);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","now","要给 --now / --time= / --ref= 之一",NULL);
        return IMG_EINVAL;
    }
    if(x >= 0){
        uint16_t tm=0,dt=0; img_fat32_epoch_to_dos(x,&tm,&dt);
        const char *kind=img_args_get(in,"set");
        bool all = !kind || !strcmp(kind,"all");
        if(all || !strcmp(kind,"create")){ cur[0]=tm; cur[1]=dt; }
        if(all || !strcmp(kind,"modify")){ cur[3]=tm; cur[4]=dt; }
        if(all || !strcmp(kind,"access")){ cur[2]=dt; }
    }
    rc = img_fat32_file_set_times(fs,path,cur);
    img_fat32_unmount(fs);
    if(rc){ img_result_fail(out,rc,"FAT-TOUCH-FAIL",path,"设置时间失败",NULL); return rc; }
    img_result_str(out,"path",path);
    img_result_u64(out,"ctime",(uint64_t)img_fat32_dos_to_epoch(cur[0],cur[1]));
    img_result_u64(out,"mtime",(uint64_t)img_fat32_dos_to_epoch(cur[3],cur[4]));
    img_result_u64(out,"atime",(uint64_t)img_fat32_dos_to_epoch(0,cur[2]));
    return IMG_OK;
}

static const char *batch_code(int rc){
    switch(rc){
        case IMG_ENOENT:    return "NTFS-PATH-NOTFOUND\"";
        case IMG_EEXISTS:   return "NTFS-DEST-EXISTS\"";
        case IMG_ESTATE:    return "NTFS-DEST-IS-DIR\"";
        case IMG_ENOTSUP:   return "NTFS-SRC-IS-DIR\"";
        case IMG_EFULL:     return "FAT-ENOSPC\"";
        case IMG_ECORRUPT:  return "NTFS-DIR-PHANTOM\"";
        case IMG_ECYCLE:    return "NTFS-MOVE-INTO-SELF\"";
        case IMG_ECANCELLED:return "CTL-CANCELLED\"";
        default:            return "NTFS-COPY-FAIL\"";
    }
}

static int cmd_ntfs_copy(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 复制）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *src=img_args_get(in,"src"), *dst=img_args_get(in,"dst");
    /* 单条复制要求 src/dst 都是绝对路径；批量（list_file）时这两个可以为空 */
    if(!img_args_get(in,"list_file") && (!src||src[0]!='/'||!dst||dst[0]!='/')){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst","src/dst 都必须是 / 开头的盘内绝对路径",NULL);
        return IMG_EINVAL;
    }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=ntfs_part_bytes(t,lba);
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",dst,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }

    img_copy_opts o = { IMG_COPY_FAIL, true, true };
    const char *cf = img_args_get(in,"conflict");
    if(cf){
        if(!strcmp(cf,"overwrite")) o.conflict = IMG_COPY_OVERWRITE;
        else if(!strcmp(cf,"rename")) o.conflict = IMG_COPY_RENAME;
    }
    o.preserve_times  = img_args_bool(in,"preserve_times",true);
    o.create_parents  = img_args_bool(in,"create_parents",true);

    const char *lst = img_args_get(in,"list_file");
    if (!lst && (!src || !dst)) {
        img_ntfs_umount(fs);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","要么给 src+dst，要么给 list_file（二选一）",NULL);
        return IMG_EINVAL;
    }
    if (lst) {
        /* ---- 批量：每行 源>目标，一次挂载全做完 ---- */
        FILE *fp = fopen(lst, "r");
        if (!fp) {
            img_ntfs_umount(fs);
            img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",lst,"清单文件打不开",NULL);
            return IMG_ENOENT;
        }
        const char *ofail = img_args_get(in,"on_fail");
        bool stop_on_fail = ofail && !strcmp(ofail,"stop");
        IMG_THREAD_LOCAL static char items[96*1024];
        size_t il = 1; items[0] = '['; items[1] = 0;
        int okn = 0, badn = 0; uint64_t bytes = 0; bool truncated = false;
        char line[1400];
        img_span sp; img_span_prep(&sp,"ntfs","copy-batch","批量盘内复制");
        img_span_kv_str(&sp,"list",lst); img_span_begin(&sp);
        while (fgets(line, sizeof line, fp)) {
            char *nl = strchr(line,'\n'); if (nl) *nl = 0;
            char *p0 = line;
            while (*p0 == ' ' || *p0 == '\t') p0++;
            if (!*p0 || *p0 == '#') continue;
            char *sep = strchr(p0, '>');
            if (!sep) { badn++; continue; }
            *sep = 0;
            char *src = p0;
            char *dst = sep + 1;
            while (*src == ' ' || *src == '\t') src++;
            char *e = src + strlen(src); while (e > src && (e[-1]==' '||e[-1]=='\t')) *--e = 0;
            while (*dst == ' ' || *dst == '\t') dst++;
            e = dst + strlen(dst); while (e > dst && (e[-1]==' '||e[-1]=='\t')) *--e = 0;
            if (!*src || !*dst) { badn++; continue; }
            img_copy_result r; memset(&r, 0, sizeof r);
            int rc2 = img_ntfs_copy(fs, src, dst, &o, img_cmd_prog(), &r);
            int need = snprintf(items + il, sizeof items - il,
                "%s{\"src\":\"%s\",\"dst\":\"%s\",\"ok\":%s,\"ino\":%llu,\"size\":%llu%s%s}",
                okn + badn ? "," : "", src, dst, rc2 == IMG_OK ? "true" : "false",
                (unsigned long long)r.ino, (unsigned long long)r.size,
                rc2 == IMG_OK ? "" : ",\"code\":\"",
                rc2 == IMG_OK ? "" : batch_code(rc2));
            if (need > 0 && il + (size_t)need < sizeof items) il += (size_t)need;
            else truncated = true;
            if (rc2 == IMG_OK) { okn++; bytes += r.size; }
            else { badn++; if (stop_on_fail) break; }
        }
        fclose(fp);
        if (il + 2 < sizeof items) { items[il++] = ']'; items[il] = 0; }
        img_span_kv_u64(&sp,"copied",(uint64_t)okn);
        img_span_kv_u64(&sp,"failed",(uint64_t)badn);
        /* ★ 批量语义：命令本身成功（App 能拿到明细）；逐项成败在 failed/items 里。
         *   否则一旦有一项失败就丢失全部明细（v1 就是这么错的）。*/
        img_span_end(&sp, IMG_OK);
        img_ntfs_umount(fs);
        img_result_u64(out,"copied",(uint64_t)okn);
        img_result_u64(out,"failed",(uint64_t)badn);
        img_result_u64(out,"bytes",bytes);
        img_result_bool(out,"truncated",truncated);
        img_result_raw(out,"items", items[1] ? items : "[]");
        return IMG_OK;
    }

    img_span sp; img_span_prep(&sp,"ntfs","copy","盘内复制");
    img_span_kv_str(&sp,"src",src); img_span_kv_str(&sp,"dst",dst);
    img_span_kv_u64(&sp,"conflict",(uint64_t)o.conflict);
    img_span_begin(&sp);
    img_copy_result r; memset(&r,0,sizeof r);
    rc = img_ntfs_copy(fs, src, dst, &o, img_cmd_prog(), &r);
    img_span_end(&sp, rc);
    char parent[512]=""; int total=0;
    if(rc==IMG_OK) copy_parent_info(fs, r.path, parent, sizeof parent, &total);
    img_ntfs_umount(fs);
    if(rc){
        const char *code = (rc==IMG_ENOENT)  ? "NTFS-PATH-NOTFOUND"
                         : (rc==IMG_EINVAL) ? "CMD-ARG-BAD"
                         : (rc==IMG_EEXISTS) ? "NTFS-DEST-EXISTS"
                         : (rc==IMG_ESTATE)  ? "NTFS-DEST-IS-DIR"
                         : (rc==IMG_ENOTSUP) ? "NTFS-SRC-IS-DIR"
                         : (rc==IMG_EFULL)   ? "FAT-ENOSPC"
                         : (rc==IMG_ECORRUPT)? "NTFS-DIR-PHANTOM"
                         : (rc==IMG_ECANCELLED)?"CTL-CANCELLED":"NTFS-COPY-FAIL";
        img_result_fail(out,rc,code,src,
            (rc==IMG_EEXISTS)?"目标已存在（改用 conflict=overwrite 或 rename）":
            (rc==IMG_ESTATE) ?"目标是目录：不能拿文件顶掉它":
            (rc==IMG_ENOTSUP)?"源是目录：目录递归复制还没做（下一步）":
            (rc==IMG_ECORRUPT)?"写完从盘上复查没过（卷已不一致，停止继续写）":
            (rc==IMG_ECANCELLED)?"已取消（临时文件已清理）":"复制失败（空间/只读/卷状态）",NULL);
        return rc;
    }
    img_result_str(out,"path", r.path);
    img_result_u64(out,"ino", r.ino);
    img_result_u64(out,"size", r.size);
    img_result_bool(out,"replaced", r.replaced);
    img_result_str(out,"parent", parent);
    img_result_u64(out,"parent_total", (uint64_t)total);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}

static int cmd_fat_copy(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *src=img_args_get(in,"src"), *dst=img_args_get(in,"dst");
    if(!src||src[0]!='/'||!dst||dst[0]!='/'){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst","src/dst 都必须是 / 开头的分区内绝对路径",NULL);
        return IMG_EINVAL;
    }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=ntfs_part_bytes(t,lba);
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_fat32_fs *fs=img_fat32_mount(q,lba,true);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",dst,"不是合法 FAT32（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    int conflict=0;
    const char *cf=img_args_get(in,"conflict");
    if(cf){ if(!strcmp(cf,"overwrite")) conflict=1; else if(!strcmp(cf,"rename")) conflict=2; }
    char got[600]=""; uint64_t sz=0; bool replaced=false;
    img_span sp; img_span_prep(&sp,"fat","copy","盘内复制");
    img_span_kv_str(&sp,"src",src); img_span_kv_str(&sp,"dst",dst); img_span_begin(&sp);
    rc = img_fat32_copy(fs, src, dst, conflict, img_cmd_prog(), got, sizeof got, &sz, &replaced);
    img_span_end(&sp, rc);
    img_fat32_unmount(fs);
    if(rc){
        const char *code = (rc==IMG_EEXISTS)?"FAT-ENTRY-EXISTS"
                         : (rc==IMG_ESTATE) ?"FAT-DIR-NOT-EMPTY"
                         : (rc==IMG_EFULL)  ?"FAT-ENOSPC"
                         : (rc==IMG_ECANCELLED)?"CTL-CANCELLED":"FAT-COPY-FAIL";
        img_result_fail(out,rc,code,src,
            (rc==IMG_EEXISTS)?"目标已存在（改用 conflict=overwrite 或 rename）":
            (rc==IMG_EFULL)  ?"文件大于 64 MiB：FAT 侧暂只支持小文件，大文件请 export 再 put":
            (rc==IMG_ECANCELLED)?"已取消":"复制失败",NULL);
        return rc;
    }
    img_result_str(out,"path", got);
    img_result_u64(out,"size", sz);
    img_result_bool(out,"replaced", replaced);
    return IMG_OK;
}

static int cmd_ntfs_rm(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 删除）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是 / 开头的盘内绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=ntfs_part_bytes(t,lba);
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    if(!img_ntfs_path_exists(fs,path)){
        img_ntfs_umount(fs);
        img_result_fail(out,IMG_ENOENT,"NTFS-PATH-NOTFOUND",path,"要删的路径不存在（先用 ntfs.list 核对）",NULL);
        return IMG_ENOENT;
    }
    bool rec=img_args_bool(in,"recursive",false);
    int removed=0;
    img_span sp; img_span_prep(&sp,"ntfs", rec?"rmtree":"rm", rec?"递归删除":"删盘内条目");
    img_span_kv_str(&sp,"path",path); img_span_kv_u64(&sp,"recursive",rec?1u:0u); img_span_begin(&sp);
    int rr = rec ? img_ntfs_rm_tree(fs,path,&removed) : img_ntfs_rm(fs,path);
    if(rr==IMG_OK && !rec) removed=1;
    img_span_end(&sp,rr);
    bool gone=(rr==IMG_OK) && !img_ntfs_path_exists(fs,path);
    img_ntfs_umount(fs);
    if(rr!=IMG_OK){
        const char *code=(rr==IMG_ESTATE)?"NTFS-DIR-NOT-EMPTY"
                        :(rr==IMG_EINVAL)?"CMD-ARG-BAD"
                        :(rr==IMG_ECORRUPT)?"NTFS-DIR-PHANTOM":"NTFS-RM-FAIL";
        const char *hint=(rr==IMG_ESTATE)?"目录不空：要连子项一起删请显式传 recursive=true":
                                          "删除失败（卷状态或权限异常）";
        img_result_fail(out,rr,code,path,hint,NULL); return rr;
    }
    if(!gone){ img_result_fail(out,IMG_ESTATE,"FAT-RM-FAIL",path,"报成功但路径还在（卷状态异常，建议 qc.verify）",NULL); return IMG_ESTATE; }
    img_result_str(out,"path",path);
    img_result_u64(out,"removed",(uint64_t)removed);
    img_result_bool(out,"deleted",true);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}


/* ================= WIM/ESD 内部只读（wim.ls / wim.extract）=================
 * 层归属同 iso.*：App 只下命令，不需要自己装 WIM 库、不需要文件操作权限。
 * ★ 为什么 --wim 可以指 ISO：wimlib 只接受【真实文件路径】（没有块设备回调），
 *   所以 ISO 内的 install.wim/.esd 必须先复制成本地文件才能浏览/抽取。
 *   这个代价不该对用户隐身 —— 工具把它做掉，同时把"抽了多少字节"如实回给 App
 *   （source_note），并在使用完删掉临时文件（unlink；进程被杀会留孤儿，
 *   App 启动时清 img_tmpdir() 里 imgtool-src-* 是既定契约，见手册）。
 * ★ 过滤状态一律放在累加器里，不用进程级全局：iso.ls 那种 static 缓冲在
 *   【无 ctx 的命令】上并发跑会互相踩（这两条命令就不需要 ctx）。 */
#if IMG_WITH_WIM       /* wim.ls / wim.extract 的私有助手 */
#define WIM_LS_MAX 4096
typedef struct {
    char *b; size_t cap, len; int n; bool trunc;
    char contains[128]; size_t limit;      /* limit=0 → 用 WIM_LS_MAX */
} wimlist_t;
/* 手写 JSON 字符串转义（与 iso 同一套规矩：机器可读永远 ASCII/转义安全）*/
static int wj_esc(char *dst,size_t cap,const char *s){
    int k=0;
    for(const unsigned char *p=(const unsigned char*)s; p&&*p && k<(int)cap-8; p++){
        if(*p=='"'||*p=='\\'){ dst[k++]='\\'; dst[k++]=(char)*p; }
        else if(*p<0x20) k+=snprintf(dst+k,cap-(size_t)k,"\\u%04x",*p);
        else dst[k++]=(char)*p;
    }
    dst[k]=0;
    return k;
}
static void wim_ls_cb(void *ud,const char *name,const char *full,bool isdir,
                      uint64_t size,uint32_t attrs,size_t depth){
    wimlist_t *a=(wimlist_t*)ud;
    if(!a) return;
    size_t lim=a->limit?a->limit:WIM_LS_MAX;
    if((size_t)a->n>=lim){ a->trunc=true; return; }
    if(a->contains[0] && !(full&&strstr(full,a->contains))) return;    /* 按整条路径匹配 */
    char nm[512], fp[1024];
    wj_esc(nm,sizeof nm,name?name:"");
    wj_esc(fp,sizeof fp,full?full:"");
    char item[1600];
    int k=snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"path\":\"%s\",\"dir\":%s,"
                       "\"size\":%llu,\"attrs\":%u,\"depth\":%zu}",
                   a->n?",":"",nm,fp,isdir?"true":"false",
                   (unsigned long long)size,(unsigned)attrs,depth);
    if(a->len+(size_t)k+1 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=(size_t)k; a->n++; }
    else a->trunc=true;
}
/* --wim 可以指 WIM/ESD 本体，也可以指含它的 ISO。返回本地可用路径；
 * is_tmp=true 时调用方用完必须 unlink。note/nsecap 回填"替你做了什么"。*/
static int wim_source_local(const char *in,char *out,size_t cap,bool *is_tmp,
                            char *note,size_t notecap,uint64_t *out_bytes,img_prog *prog){
    if(is_tmp) *is_tmp=false;
    if(out_bytes) *out_bytes=0;
    size_t n=in?strlen(in):0;
    bool looks_iso = n>4 && !strcasecmp(in+n-4,".iso");
    if(!looks_iso){ snprintf(out,cap,"%s",in); return IMG_OK; }
    img_iso *iso=NULL; char err[256]="";
    int rc=img_iso_open_path(&iso,in,err,sizeof err);
    if(rc!=IMG_OK) return rc;
    const char *cands[2]={"/sources/install.wim","/sources/install.esd"};
    const char *hit=NULL; uint64_t fsz=0; bool d=false;
    for(int i=0;i<2;i++) if(img_iso_stat(iso,cands[i],&fsz,&d)==IMG_OK && !d){ hit=cands[i]; break; }
    if(!hit){ img_iso_close(iso); return IMG_ENOENT; }
    const char *tmp=img_tmpdir();
    if(!tmp||!tmp[0]){ img_iso_close(iso); return IMG_ESTATE; }
    snprintf(out,cap,"%s/imgtool-src-%d%s",tmp,(int)getpid(),
             (strstr(hit,".esd")||strstr(hit,"ESD"))?".esd":".wim");
    uint64_t got=0;
    rc=img_iso_extract(iso,hit,out,0,prog,&got);
    img_iso_close(iso);
    if(rc!=IMG_OK){ unlink(out); out[0]=0; return rc; }
    if(is_tmp) *is_tmp=true;
    if(out_bytes) *out_bytes=got;
    if(note) snprintf(note,notecap,"已从 ISO 抽出 %s（%llu 字节）到临时文件供 wimlib 读取",
                     hit,(unsigned long long)got);
    return IMG_OK;
}
/* wim.ls / wim.extract 共用的失败码翻译（把"没这个路径"和"库读不动"分开）*/
static const char *wim_fs_code(int rc,int kind){
    switch(rc){
    case IMG_ENOENT:  return kind==1?"WIM-PATH-NOTFOUND":"HOST-OPEN-FAIL";
    case IMG_ERANGE:  return "WIM-IDX-INVALID";
    case IMG_EINVAL:  return "CMD-ARG-BAD";
    case IMG_EFORMAT: return "WIM-NOTFOUND";
    case IMG_ENOTSUP: return "WIM-IS-DIR";
    case IMG_EFULL:   return "HOST-ENOSPC";
    case IMG_ECANCELLED: return "CMD-CANCELLED";
    case IMG_ESTATE:  return "HOST-ENOSPC";
    default:          return kind==1?"WIM-LIST-FAIL":"WIM-EXTRACT-FAIL";
    }
}
#endif /* IMG_WITH_WIM */
static const img_arg A_WIMLS[] = {
    {"wim",       IMG_A_STR, true,  NULL,   "WIM/ESD 路径；也可以直接给含它的 ISO（工具自动定位 sources\\install.wim|.esd）", NULL},
    {"index",     IMG_A_U64, false, "1",    "映像索引（先用 wim.identify 拿清单）", NULL},
    {"path",      IMG_A_STR, false, "/",    "WIM 内的目录（/ 开头，大小写不敏感）", NULL},
    {"recursive", IMG_A_BOOL,false, "false","true=整棵子树（大映像可达几十万条，务必配 limit）", NULL},
    {"contains",  IMG_A_STR, false, NULL,   "只回路径里含这个子串的条目（App 省字节用）", NULL},
    {"limit",     IMG_A_U64, false, "2000", "最多回多少条（0=用硬上限 4096）", NULL},
};
static int cmd_wim_ls(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 浏览）","重新编译时开启 IMG_WITH_WIM 并安装 libwim");
#if IMG_WITH_WIM
    const char *srcp=img_args_get(in,"wim");
    if(!srcp||!srcp[0]){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","wim","要一个 WIM/ESD 或 ISO 路径",NULL); return IMG_EINVAL; }
    int index=(int)img_args_u64(in,"index",1);
    const char *dir=img_args_get(in,"path"); if(!dir||!dir[0]) dir="/";
    if(dir[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","WIM 内路径必须以 / 开头",NULL); return IMG_EINVAL; }
    bool rec=img_args_bool(in,"recursive",false);
    uint64_t lim64=img_args_u64(in,"limit",2000);
    const char *contains=img_args_get(in,"contains");

    char local[1024]; bool is_tmp=false; char note[256]=""; uint64_t extracted=0;
    int rc=wim_source_local(srcp,local,sizeof local,&is_tmp,note,sizeof note,&extracted,img_cmd_prog());
    if(rc!=IMG_OK){
        img_result_fail(out,rc,wim_fs_code(rc,1),srcp,
                        rc==IMG_ENOENT?"不是 ISO 里的 install.wim/.esd，或这个文件不是 WIM":"无法准备 WIM 源",NULL);
        return rc;
    }
    /* ★ 线程局部，理由同 iso.ls（无 ctx 命令可并行 + raw 只存指针不能就地 free）*/
    IMG_THREAD_LOCAL static char jb[512*1024];
    jb[0]=(char)0x5b; wimlist_t acc={jb,sizeof jb-2,1,0,false,{0},0};   /* ★ len 从 1 起：第 0 格是 '['，别覆盖掉 */
    if(contains&&contains[0]) snprintf(acc.contains,sizeof acc.contains,"%s",contains);
    acc.limit=(size_t)lim64;
    char err[256]="";
    /* 先辨一下目标到底是目录还是文件：wimlib 对文件路径列"子项"会安静返回 0 条，
     * App 看到 count=0 会以为目录是空的 —— 那是误导，必须显式报错。*/
    {   img_wim_stat_t est; memset(&est,0,sizeof est);
        if(strcmp(dir,"/")!=0 && img_wim_stat(local,index,dir,&est,err,sizeof err)==IMG_OK && !est.is_dir){
            if(is_tmp) unlink(local);
            img_result_fail(out,IMG_EINVAL,"WIM-NOT-A-DIR",dir,"这是文件不是目录；抽文件用 wim.extract",NULL);
            return IMG_EINVAL;
        }
    }
    img_span sp; img_span_prep(&sp,"wim","ls","列 WIM/ESD 目录");
    img_span_kv_str(&sp,"path",dir); img_span_begin(&sp);
    rc=img_wim_list(local,index,dir,rec,wim_ls_cb,&acc,err,sizeof err);
    img_span_end(&sp,rc);
    if(is_tmp) unlink(local);                     /* ★ 用完就删，不把 4~8 GB 垃圾留在用户机上 */
    if(rc!=IMG_OK){
        img_result_fail(out,rc,wim_fs_code(rc,1),dir,err[0]?err:"列目录失败",NULL);
        return rc;
    }
    jb[acc.len++]=']'; jb[acc.len]=0;
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"truncated",acc.trunc);
    img_result_bool(out,"recursive",rec);
    img_result_str(out,"path",dir);
    if(note[0]) img_result_str(out,"source_note",note);
    img_result_raw(out,"entries",jb);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}
static const img_arg A_WIMEXTRACT[] = {
    {"wim",   IMG_A_STR, true,  NULL, "WIM/ESD 路径；也可以直接给含它的 ISO", NULL},
    {"index", IMG_A_U64, false, "1",  "映像索引", NULL},
    {"path",  IMG_A_STR, true,  NULL, "WIM 内的文件路径（/ 开头；目录会被拒）", NULL},
    {"dst",   IMG_A_STR, true,  NULL, "本地输出文件（会覆盖）", NULL},
};
static int cmd_wim_extract(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 取文件）","重新编译时开启 IMG_WITH_WIM 并安装 libwim");
#if IMG_WITH_WIM
    const char *srcp=img_args_get(in,"wim"), *ip=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    int index=(int)img_args_u64(in,"index",1);
    if(!srcp||!ip||!dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","wim/path/dst","三个都必填",NULL); return IMG_EINVAL; }
    if(ip[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","WIM 内路径必须以 / 开头",NULL); return IMG_EINVAL; }
    char local[1024]; bool is_tmp=false; char note[256]=""; uint64_t extracted=0;
    img_prog *prog=img_cmd_prog();
    int rc=wim_source_local(srcp,local,sizeof local,&is_tmp,note,sizeof note,&extracted,prog);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,wim_fs_code(rc,2),srcp,"无法准备 WIM 源（不是 WIM/ESD，或 ISO 里没有 install.wim/.esd）",NULL);
        return rc;
    }
    char err[256]=""; uint64_t got=0;
    /* ★ 错误码分级：源准备失败 = 文件层面的问题（HOST-OPEN-FAIL 那一族），
     *   抽取失败 = 映像【内部路径】的问题（WIM-PATH-NOTFOUND）——
     *   之前一律按 kind=2 翻，把"里面没这个文件"报成"文件打不开"，方向完全带偏。*/
    img_span sp; img_span_prep(&sp,"wim","extract","从 WIM/ESD 抽单个文件");
    img_span_kv_str(&sp,"path",ip); img_span_begin(&sp);
    rc=img_wim_extract_file(local,index,ip,dst,prog,&got,err,sizeof err);
    img_span_kv_u64(&sp,"written",got);
    img_span_end(&sp,rc);
    if(is_tmp) unlink(local);
    if(rc!=IMG_OK){
        if(rc==IMG_ECANCELLED) unlink(dst);       /* 取消不留半成品 */
        img_result_fail(out,rc,wim_fs_code(rc,1),ip,err[0]?err:"抽取失败",NULL);
        return rc;
    }
    img_result_str(out,"path",ip);
    img_result_str(out,"dst",dst);
    img_result_u64(out,"bytes",got);
    if(note[0]) img_result_str(out,"source_note",note);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}


/* ================= 命令 6k：win.finish（裸灌后首次启动收尾）====================
 * 依据：微软 Learn《OS start-up – Computer restarted unexpectedly or encountered
 * an unexpected error》—— 裸灌系统首次启动弹"计算机意外地重新启动或遇到错误"，
 * 官方修复是把 HKLM\SYSTEM\Setup\Status\ChildCompletion\setup.exe 由 1 改成 3。
 * 我们离线改 qcow2 里的 SYSTEM hive（libhivex），因为没有 Windows 运行环境。 */
static const img_arg A_WINFINISH[] = {
    {"img",     IMG_A_STR,  true,  NULL,   "镜像路径（qcow2）", NULL},
    {"lba",     IMG_A_U64,  true,  NULL,   "Windows 系统分区起始 LBA", chk_lba},
    {"dry-run", IMG_A_BOOL, false, "false","只看不改（诊断 SetupType/SystemSetupInProgress/ChildCompletion）", NULL},
};
static int cmd_win_finish(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba=img_args_u64(in,"lba",0);
    bool dry=img_args_bool(in,"dry-run",false);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    img_win_finish_t fi; char err[256]="";
    img_span sp; img_span_prep(&sp,"win","finish",dry?"诊断首次启动状态":"收尾首次启动");
    img_span_kv_u64(&sp,"lba",lba); img_span_begin(&sp);
    rc=img_win_finish(q,lba,pb,dry,&fi,err,sizeof err);
    img_span_end(&sp,rc);
    if(rc!=IMG_OK){
        /* 错误码要能分开"盘不像 Windows"和"卷坏了"：前者是用户选错分区，
         * 后者才是真故障。以前一律 WIN-FINISH-FAIL，真机上只能靠猜。*/
        const char *code = (rc==IMG_ENOENT)?"WIN-SYSTEM-HIVE-NOTFOUND"
                         : (rc==IMG_ESTATE) ?"PART-FS-BAD"
                                            :"WIN-FINISH-FAIL";
        const char *hint = (rc==IMG_ENOENT)
                           ? "这个分区里没有 \\Windows\\System32\\config\\SYSTEM（不像 Windows 系统分区）"
                           : NULL;
        img_result_fail(out,rc,code,img_ctx_path(ctx),err[0]?err:"收尾失败",hint);
        return rc;
    }
    img_result_bool(out,"dry_run",dry);
    img_result_bool(out,"found_child_completion",fi.found_node);
    if(fi.old_setup_exe>=0) img_result_u64(out,"old_setup_exe",(uint64_t)fi.old_setup_exe);
    img_result_u64(out,"new_setup_exe",(uint64_t)fi.new_setup_exe);
    img_result_bool(out,"changed",fi.changed);
    if(fi.setup_type>=0)              img_result_u64(out,"setup_type",(uint64_t)fi.setup_type);
    if(fi.system_setup_in_progress>=0) img_result_u64(out,"system_setup_in_progress",(uint64_t)fi.system_setup_in_progress);
    img_result_bool(out,"cmdline_present",fi.cmdline_present);
    return IMG_OK;
}

/* ---------------- file.copy：跨分区 / 跨镜像 / 宿主 / ISO / WIM ----------------
 * 与 ntfs.copy（同卷）的分工：同卷仍然走 ntfs.copy（更快、只碰索引），
 * 这条覆盖剩下所有组合，也是 App 文件管理器"复制/粘贴"的唯一入口。*/
static const img_arg A_FILECOPY[] = {
    {"img",         IMG_A_STR,  false, NULL,  "默认镜像（两侧没显式给 *_img 时用它）", NULL},
    {"src",         IMG_A_STR,  false, NULL,  "源：ntfs:/路径 | fat:/路径 | host:/路径 | iso:/路径 | wim:[索引]:/路径（批量时可省）", NULL},
    {"dst",         IMG_A_STR,  false, NULL,  "目标：ntfs:/路径 | fat:/路径 | host:/路径（批量时可省）", NULL},
    {"src_img",     IMG_A_STR,  false, NULL,  "源镜像（qcow2 / iso / wim-esd）", NULL},
    {"dst_img",     IMG_A_STR,  false, NULL,  "目标镜像（qcow2）", NULL},
    {"src_lba",     IMG_A_U64,  false, "0",   "源分区起始 LBA（0=自动；该文件系统只有唯一分区才挑）", NULL},
    {"dst_lba",     IMG_A_U64,  false, "0",   "目标分区起始 LBA（0=自动）", NULL},
    {"src_index",   IMG_A_U64,  false, "1",   "WIM/ESD 映像索引", NULL},
    {"conflict",    IMG_A_ENUM, false, "fail","同名策略：fail（默认）/ overwrite / rename（名 (1).扩展名）", NULL},
    {"recursive",   IMG_A_BOOL, false, "true","源是目录时递归复制（false=目录一律拒绝，零副作用）", NULL},
    {"preserve_times", IMG_A_BOOL, false, "true", "照抄源时间（NTFS 四时间 / FAT 5 字段 / 宿主 atime+mtime）", NULL},
    {"create_parents", IMG_A_BOOL, false, "true", "目标父目录不存在就建", NULL},
    {"follow_symlinks", IMG_A_BOOL, false, "false", "宿主源：是否跟随符号链接（默认否，防止复制出环）", NULL},
    {"list_file", IMG_A_STR, false, NULL, "批量粘贴清单（每行 源>目标，两侧都是位置串）", NULL},
    {"on_fail", IMG_A_STR, false, "continue", "批量时某项失败：continue（默认）或 stop", NULL},
};

static const char *xcopy_code(int rc){
    switch(rc){
        case IMG_ENOENT:    return "COPY-SRC-NOTFOUND";
        case IMG_EAGAIN:    return "COPY-LBA-AMBIGUOUS";
        case IMG_EEXISTS:   return "COPY-DEST-EXISTS";
        case IMG_ESTATE:    return "COPY-DEST-CONFLICT";
        case IMG_ENOTSUP:   return "COPY-UNSUPPORTED";
        case IMG_EFULL:     return "COPY-NO-SPACE";
        case IMG_ECORRUPT:  return "COPY-VERIFY-FAIL";
        case IMG_ERANGE:    return "COPY-FILE-TOO-BIG";
        case IMG_ECANCELLED:return "CTL-CANCELLED";
        case IMG_ECYCLE:    return "COPY-INTO-SELF";
        case IMG_ESAMEFILE: return "COPY-SAME-FILE";
        case IMG_ENAMETOOLONG:return "COPY-NAME-TOO-LONG";
        case IMG_EFORMAT:   return "PART-FS-BAD";
        case IMG_EINVAL:    return "CMD-ARG-BAD";
        default:            return "COPY-FAIL";
    }
}
static const char *xcopy_hint(int rc){
    switch(rc){
        case IMG_ENOENT:    return "源路径或镜像不存在：先用 ntfs.list / fat.ls / host 路径核对";
        case IMG_EAGAIN:    return "镜像里该文件系统有多个分区：显式传 --src_lba/--dst_lba（先 part.show 看）";
        case IMG_EEXISTS:   return "目标已存在：改用 conflict=overwrite 或 conflict=rename";
        case IMG_ESTATE:    return "目标类型冲突（文件/目录互顶）：换一个目标名或先删掉旧对象";
        case IMG_ENOTSUP:   return "该组合不支持：目标不能是 ISO/WIM；源是目录时 recursive 必须为 true";
        case IMG_EFULL:     return "空间不足（镜像内分区或宿主磁盘）：清理后重试";
        case IMG_ECORRUPT:  return "写完从盘上复查没过：卷可能已不一致，停止继续写并考虑重建该分区";
        case IMG_ERANGE:    return "FAT32 单文件上限 4 GiB-1：改用 NTFS 分区或拆文件";
        case IMG_ECANCELLED:return "已取消（已写入的部分保留，未完成的临时文件已清理）";
        case IMG_ECYCLE:    return "目标在源目录的子树里：会无限递归，已拒绝（换目标位置）";
        case IMG_ESAMEFILE: return "源和目标指向同一个文件：要复制一份副本请用 conflict=rename";
        case IMG_ENAMETOOLONG:return "名字超过 255 字符上限：先改名再复制（工具不会静默截断）";
        default:            return "复制失败：看 err.at 定位是哪一侧；需要时导出诊断日志";
    }
}

/* 单条复制：解析两侧位置 → 调引擎。错误码由调用方决定怎么包装。*/
static int file_copy_one(img_ctx *ctx, const img_args *in,
                         const char *ss, const char *ds,
                         img_xcopy_stats *stp){
    img_xloc sl, dl; char why[256]={0};
    int rc=img_xcopy_parse(ss,&sl,why,sizeof why);
    if(rc!=IMG_OK) return rc;
    rc=img_xcopy_parse(ds,&dl,why,sizeof why);
    if(rc!=IMG_OK) return rc;
    if(dl.kind==IMG_XK_ISO || dl.kind==IMG_XK_WIM) return IMG_EINVAL;

    const char *self = ctx ? img_ctx_path(ctx) : NULL;
    const char *arg_img = img_args_get(in,"img");
    const char *si = img_args_get(in,"src_img");
    const char *di = img_args_get(in,"dst_img");
    const char *defimg = arg_img ? arg_img : self;

    if(sl.kind==IMG_XK_NTFS || sl.kind==IMG_XK_FAT){
        const char *p = si ? si : defimg;
        if(!p || !*p) return IMG_EINVAL;
        snprintf(sl.img,sizeof sl.img,"%s",p);
    } else if(sl.kind==IMG_XK_ISO || sl.kind==IMG_XK_WIM){
        if(!si || !*si) return IMG_EINVAL;
        snprintf(sl.img,sizeof sl.img,"%s",si);
    }
    if(dl.kind==IMG_XK_NTFS || dl.kind==IMG_XK_FAT){
        const char *p = di ? di : defimg;
        if(!p || !*p) return IMG_EINVAL;
        snprintf(dl.img,sizeof dl.img,"%s",p);
    }
    if(!sl.lba) sl.lba=img_args_u64(in,"src_lba",0);
    if(!dl.lba) dl.lba=img_args_u64(in,"dst_lba",0);
    sl.index=(int)img_args_u64(in,"src_index",1);

    img_xcopy_opts o={ IMG_COPY_FAIL, true, true, true, false, 32 };
    const char *cf=img_args_get(in,"conflict");
    if(cf){
        if(!strcmp(cf,"overwrite")) o.conflict=IMG_COPY_OVERWRITE;
        else if(!strcmp(cf,"rename")) o.conflict=IMG_COPY_RENAME;
        else if(strcmp(cf,"fail")) return IMG_EINVAL;
    }
    o.recursive      = img_args_bool(in,"recursive",true);
    o.preserve_times = img_args_bool(in,"preserve_times",true);
    o.create_parents = img_args_bool(in,"create_parents",true);
    o.follow_symlinks= img_args_bool(in,"follow_symlinks",false);

    memset(stp,0,sizeof *stp);
    return img_xcopy_run(&sl,&dl,&o,img_cmd_prog(),stp);
}

static void file_copy_fill(img_result *out, const char *ss, const char *ds,
                           const img_xcopy_stats *st){
    img_result_u64(out,"copied_files",st->files);
    img_result_u64(out,"copied_dirs",st->dirs);
    img_result_u64(out,"bytes",st->bytes);
    img_result_u64(out,"skipped",st->skipped);
    img_result_str(out,"src",ss);
    img_result_str(out,"dst",ds);
    if(st->final_path[0]){
        img_result_str(out,"dst_path",st->final_path);
        char par[2048]; snprintf(par,sizeof par,"%s",st->final_path);
        char *slash=strrchr(par,'/');
        if(slash){ if(slash==par) par[1]=0; else *slash=0; img_result_str(out,"parent",par); }
    }
}

static int cmd_file_copy(img_ctx *ctx, const img_args *in, img_result *out){
    const char *ss=img_args_get(in,"src"), *ds=img_args_get(in,"dst");
    const char *lst=img_args_get(in,"list_file");
    if(!lst && (!ss || !ds)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","要么给 src+dst，要么给 list_file（二选一）",NULL);
        return IMG_EINVAL;
    }
    if(lst && (ss || ds)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","list_file","list_file 与 src/dst 不能同时给",NULL);
        return IMG_EINVAL;
    }

    if(lst){
        /* ---- 批量粘贴：每行 "源>目标"（两侧都是位置串），一次调用全做完 ----
         * 语义与 ntfs.copy 的批量一致：命令本身成功（App 能拿到逐项明细），
         * 单项失败只记在 items/failed 里。*/
        FILE *fp=fopen(lst,"r");
        if(!fp){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",lst,"清单文件打不开",NULL); return IMG_ENOENT; }
        const char *ofail=img_args_get(in,"on_fail");
        bool stop_on_fail = ofail && !strcmp(ofail,"stop");
        IMG_THREAD_LOCAL static char items[96*1024];
        size_t il=1; items[0]='['; items[1]=0;
        int okn=0,badn=0; uint64_t bytes=0; bool truncated=false;
        char line[1600];
        img_span sp; img_span_prep(&sp,"file","copy-batch","批量跨位置复制");
        img_span_kv_str(&sp,"list",lst); img_span_begin(&sp);
        while(fgets(line,sizeof line,fp)){
            char *nl=strchr(line,'\n'); if(nl) *nl=0;
            char *p0=line;
            while(*p0==' '||*p0=='\t') p0++;
            if(!*p0 || *p0=='#') continue;
            char *sep=strchr(p0,'>');
            if(!sep){ badn++; continue; }
            *sep=0;
            char *a=p0, *b=sep+1;
            char *e=a+strlen(a); while(e>a && (e[-1]==' '||e[-1]=='\t')) *--e=0;
            while(*b==' '||*b=='\t') b++;
            e=b+strlen(b); while(e>b && (e[-1]==' '||e[-1]=='\t')) *--e=0;
            if(!*a||!*b){ badn++; continue; }
            img_xcopy_stats st;
            int rc2=file_copy_one(ctx,in,a,b,&st);
            int need=snprintf(items+il,sizeof items-il,
                "%s{\"src\":\"%s\",\"dst\":\"%s\",\"ok\":%s,\"files\":%llu,\"dirs\":%llu,\"bytes\":%llu%s%s%s}",
                (okn+badn)?",":"", a,b, rc2==IMG_OK?"true":"false",
                (unsigned long long)st.files,(unsigned long long)st.dirs,
                (unsigned long long)st.bytes,
                rc2==IMG_OK?"":",\"code\":\"", rc2==IMG_OK?"":xcopy_code(rc2),
                rc2==IMG_OK?"":"\"");
            if(need>0 && il+(size_t)need<sizeof items) il+=(size_t)need; else truncated=true;
            if(rc2==IMG_OK){ okn++; bytes+=st.bytes; }
            else { badn++; if(stop_on_fail) break; }
        }
        fclose(fp);
        if(il+2<sizeof items){ items[il++]=']'; items[il]=0; }
        img_span_kv_u64(&sp,"copied",(uint64_t)okn);
        img_span_kv_u64(&sp,"failed",(uint64_t)badn);
        img_span_end(&sp,IMG_OK);
        img_result_u64(out,"copied",(uint64_t)okn);
        img_result_u64(out,"failed",(uint64_t)badn);
        img_result_u64(out,"bytes",bytes);
        img_result_bool(out,"truncated",truncated);
        img_result_raw(out,"items",items[1]?items:"[]");
        return IMG_OK;
    }

    /* 镜像文件先做一次可读性检查：报错要能区分"源镜像没了"和"盘内路径没了" */
    img_xloc sl0, dl0; char why[256]={0};
    if(img_xcopy_parse(ss,&sl0,why,sizeof why)!=IMG_OK){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src",why,
                        "源写法：ntfs:/a/b、fat:/a、host:/sdcard/x、iso:/sources/x.wim、wim:1:/Windows");
        return IMG_EINVAL;
    }
    if(img_xcopy_parse(ds,&dl0,why,sizeof why)!=IMG_OK){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst",why,"目标写法：ntfs:/a/b、fat:/a、host:/sdcard/x");
        return IMG_EINVAL;
    }
    if(dl0.kind==IMG_XK_ISO || dl0.kind==IMG_XK_WIM){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst","目标只能是 ntfs/fat/host（ISO/WIM 是只读安装源）",NULL);
        return IMG_EINVAL;
    }
    const char *self=ctx?img_ctx_path(ctx):NULL;
    const char *arg_img=img_args_get(in,"img");
    const char *si=img_args_get(in,"src_img"), *di=img_args_get(in,"dst_img");
    const char *defimg=arg_img?arg_img:self;
    const char *simg = (sl0.kind==IMG_XK_NTFS||sl0.kind==IMG_XK_FAT) ? (si?si:defimg) : si;
    const char *dimg = (dl0.kind==IMG_XK_NTFS||dl0.kind==IMG_XK_FAT) ? (di?di:defimg) : NULL;
    if((sl0.kind==IMG_XK_NTFS||sl0.kind==IMG_XK_FAT||sl0.kind==IMG_XK_ISO||sl0.kind==IMG_XK_WIM)
       && (!simg||!*simg)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src_img","源在镜像/ISO/WIM 里，但没给镜像路径",NULL);
        return IMG_EINVAL;
    }
    if((dl0.kind==IMG_XK_NTFS||dl0.kind==IMG_XK_FAT) && (!dimg||!*dimg)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst_img","目标在镜像里，但没给镜像路径",NULL);
        return IMG_EINVAL;
    }
    if(simg && *simg && access(simg,R_OK)!=0){
        img_result_fail(out,IMG_ENOENT,"COPY-SRC-NOTFOUND",simg,"源镜像打不开（不存在/无权限）",NULL);
        return IMG_ENOENT;
    }
    if(dimg && *dimg && access(dimg,R_OK)!=0){
        img_result_fail(out,IMG_ENOENT,"IMG-OPEN-FAIL",dimg,"目标镜像打不开（不存在/无权限）",NULL);
        return IMG_ENOENT;
    }

    img_span sp; img_span_prep(&sp,"file","copy","跨位置复制");
    img_span_kv_str(&sp,"src",ss); img_span_kv_str(&sp,"dst",ds); img_span_begin(&sp);
    img_xcopy_stats st;
    int rc=file_copy_one(ctx,in,ss,ds,&st);
    img_span_kv_u64(&sp,"files",st.files);
    img_span_kv_u64(&sp,"bytes",st.bytes);
    img_span_end(&sp,rc);
    if(rc!=IMG_OK){
        char why2[256];
        snprintf(why2,sizeof why2,"已完成 %llu 个文件 / %.1f MiB 后失败（%s侧）",
                 (unsigned long long)st.files,(double)st.bytes/1048576.0,
                 st.side==1?"源":(st.side==2?"目标":"写入"));
        const char *cde=xcopy_code(rc);
        /* 同一错误码在两侧含义不同：ENOENT 在目标侧是"目标分区/路径不存在"，
         * 在源侧才是"源不存在"。不给 App 区分会把它引到错的方向去查。*/
        if(rc==IMG_ENOENT && st.side==2) cde="COPY-DST-NOTFOUND";
        if(rc==IMG_ESTATE && st.side==2) cde="COPY-DST-FS-BAD";
        if(rc==IMG_EAGAIN) cde="COPY-LBA-AMBIGUOUS";
        img_result_fail(out,rc,cde,st.side==2?ds:ss,why2,xcopy_hint(rc));
        return rc;
    }
    file_copy_fill(out,ss,ds,&st);
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
    {"part.mbrboot","1.0", "写 MBR 引导码（参考件）+ 随机磁盘签名（没有它 BIOS 会报「未找到操作系统」）",
        A_MBRBOOT,(int)(sizeof A_MBRBOOT/sizeof A_MBRBOOT[0]), cmd_mbrboot},
    {"part.layout","1.0", "BIOS/MBR 标准布局生成器（系统保留+系统+恢复0x27+扩展分区含逻辑分区）",
        A_PARTLAYOUT,(int)(sizeof A_PARTLAYOUT/sizeof A_PARTLAYOUT[0]), cmd_part_layout},
    {"boot.repair","1.0","BIOS 引导一键修复（诊断→按需重写→复验→JSON）",
        A_BOOTREPAIR,(int)(sizeof A_BOOTREPAIR/sizeof A_BOOTREPAIR[0]), cmd_boot_repair},
    {"fat.export","1.0", "把 FAT32 分区里的文件导出到本地（UEFI 的 BCD 在 ESP 上）",
        A_FATEXPORT,(int)(sizeof A_FATEXPORT/sizeof A_FATEXPORT[0]), cmd_fat_export},
    {"fat.exists", "1.0", "查 FAT32 路径是否存在（断点续跑探测原语）",
        A_FATEXISTS,(int)(sizeof A_FATEXISTS/sizeof A_FATEXISTS[0]), cmd_fat_exists},
    {"fat.rm",    "1.0", "删除 FAT32 里的文件；目录默认只删空的（recursive=true 才递归）",
        A_FATRM,  (int)(sizeof A_FATRM  /sizeof A_FATRM[0]),  cmd_fat_rm},
    {"fat.move",  "1.0", "FAT32 分区内改名/移动【单条路径】（目标已存在则拒绝）",
        A_FATMOVE,(int)(sizeof A_FATMOVE/sizeof A_FATMOVE[0]), cmd_fat_move},
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
    {"ntfs.move",  "1.0", "盘内改名/移动【单条路径】（末段不同=改名，父目录不同=移动；目录跨父默认允许）",
        A_NTFSMOVE,(int)(sizeof A_NTFSMOVE/sizeof A_NTFSMOVE[0]), cmd_ntfs_move},
    {"win.finish", "1.0", "裸灌后首次启动收尾：离线把 SYSTEM\\Setup\\Status\\ChildCompletion\\setup.exe 改成 3（微软官方修复）",
        A_WINFINISH,(int)(sizeof A_WINFINISH/sizeof A_WINFINISH[0]), cmd_win_finish},
    {"ntfs.rm",    "1.0", "删除盘内文件；目录默认只删空的（recursive=true 才递归）",
        A_NTFSSRM,(int)(sizeof A_NTFSSRM/sizeof A_NTFSSRM[0]), cmd_ntfs_rm},
    {"ntfs.copy",  "1.0", "盘内复制（同卷）：临时名+原子改名，可选保留时间戳/同名策略",
        A_NTFSCOPY,(int)(sizeof A_NTFSCOPY/sizeof A_NTFSCOPY[0]), cmd_ntfs_copy},
    {"fat.copy",   "1.0", "FAT32 盘内复制（同卷）：可选同名策略",
        A_FATCOPY,(int)(sizeof A_FATCOPY/sizeof A_FATCOPY[0]), cmd_fat_copy},
    {"file.copy",  "1.0", "跨位置复制：NTFS/FAT 同镜像跨分区、跨 qcow2、宿主↔镜像、ISO/WIM→镜像（支持目录递归）",
        A_FILECOPY,(int)(sizeof A_FILECOPY/sizeof A_FILECOPY[0]), cmd_file_copy},
    {"ntfs.touch", "1.0", "设置盘内路径的时间（--now / --time= / --ref= 三选一；0=该项不改）",
        A_NTFSTOUCH,(int)(sizeof A_NTFSTOUCH/sizeof A_NTFSTOUCH[0]), cmd_ntfs_touch},
    {"fat.touch",  "1.0", "设置 FAT 上路径的时间（同上；精度 2 秒）",
        A_FATTOUCH,(int)(sizeof A_FATTOUCH/sizeof A_FATTOUCH[0]), cmd_fat_touch},
    {"ntfs.parent","1.0", "只读检查目录的 \"..\" 父引用是否自洽（搬过目录之后必查）",
        A_NTFSRPARENT,(int)(sizeof A_NTFSRPARENT/sizeof A_NTFSRPARENT[0]), cmd_ntfs_parent},
    {"wim.identify","1.0", "从内容识别 WIM/ESD/ISO 里的 Windows 版本（不信文件名）",
        A_WIMID,(int)(sizeof A_WIMID/sizeof A_WIMID[0]), cmd_wim_identify},
    {"wim.ls",     "1.0", "浏览 WIM/ESD 内部目录树（只读；--wim 可直接给 ISO，工具自己找出 install.wim/.esd）",
        A_WIMLS,(int)(sizeof A_WIMLS/sizeof A_WIMLS[0]), cmd_wim_ls},
    {"wim.extract","1.0", "从 WIM/ESD 里只读抽出【单个文件】到本地（--wim 可直接给 ISO；取消不留半成品）",
        A_WIMEXTRACT,(int)(sizeof A_WIMEXTRACT/sizeof A_WIMEXTRACT[0]), cmd_wim_extract},
    {"win.version","1.0", "读已装盘的真实版本：ntoskrnl 的 PE 版本 + 注册表 + SKU 交叉验证",
        A_WINVER,(int)(sizeof A_WINVER/sizeof A_WINVER[0]), cmd_win_version},
    {"bcd.build","1.0", "从模板直接构建 BCD（Win7 的 BCD-Template 没有绑定，必须这条）",
        A_BCDBUILD,(int)(sizeof A_BCDBUILD/sizeof A_BCDBUILD[0]), cmd_bcd_build},
    {"bcd.create","1.0", "用内置模板建一张能启动的 BCD（Vista~11 / BIOS+UEFI / 32+64 位；不要参考盘）",
        A_BCDCREATE,(int)(sizeof A_BCDCREATE/sizeof A_BCDCREATE[0]), cmd_bcd_create},
    {"qc.verify","1.0", "qcow2 结构自检（别名/越界/坏 L2）—— 进程被杀后怀疑盘坏了就跑它",
        A_QCVERIFY,(int)(sizeof A_QCVERIFY/sizeof A_QCVERIFY[0]), cmd_qc_verify},
    {"bcd.gen-uefi","1.0", "UEFI 换盘：按新盘 GUID 重写 BCD（= imggenbcd，同一份库实现）",
        A_BCDGEN,(int)(sizeof A_BCDGEN/sizeof A_BCDGEN[0]), cmd_bcd_gen_uefi},
    {"boot.plan","1.0", "判定『这个系统能不能用 GPT/MBR 启动』（只读，不写盘；App 灰按钮/弹框的依据）",
        A_BOOTPLAN,(int)(sizeof A_BOOTPLAN/sizeof A_BOOTPLAN[0]), cmd_boot_plan},
    {"boot.check","1.0", "引导链自检（= imgbc）：返回逐项结果 + 必需项失败数",
        A_BOOTCHECK,(int)(sizeof A_BOOTCHECK/sizeof A_BOOTCHECK[0]), cmd_boot_check},
    {"part.attr","1.0", "设 GPT 分区属性（恢复分区官方要求 0x8000000000000001）",
        A_PARTATTR,(int)(sizeof A_PARTATTR/sizeof A_PARTATTR[0]), cmd_part_attr},
    {"winre.find","1.0", "在 WIM/ESD 里定位 Winre.wim（只读；未找到报 WINRE-NOTFOUND）",
        A_WINREFIND,(int)(sizeof A_WINREFIND/sizeof A_WINREFIND[0]), cmd_winre_find},
    {"recovery.populate","1.0", "把 WinRE 填进【独立恢复分区】（UEFI）",
        A_RECPOP,(int)(sizeof A_RECPOP/sizeof A_RECPOP[0]), cmd_recovery_populate},
    {"recovery.populate-system","1.0", "把 WinRE 填进系统分区的 Windows\\System32\\Recovery（BIOS）",
        A_RECPOP,(int)(sizeof A_RECPOP/sizeof A_RECPOP[0]), cmd_recovery_populate_system},
    {"iso.ls","1.0", "列 ISO 里的目录（UDF；Windows 安装盘的 install.wim 只在 UDF 里）",
        A_ISOLS,(int)(sizeof A_ISOLS/sizeof A_ISOLS[0]), cmd_iso_ls},
    {"iso.extract","1.0", "从 ISO 里抽文件到本地（流式 1 MiB；App 不必自己装 ISO 库）",
        A_ISOEXTRACT,(int)(sizeof A_ISOEXTRACT/sizeof A_ISOEXTRACT[0]), cmd_iso_extract},
    {"cmd.list",  "1.0", "导出命令表（名字/参数规格/帮助）——App 可据此自动生成界面",
        NULL, 0, cmd_cmd_list},
};
const int IMG_CMD_COUNT = (int)(sizeof IMG_CMD_TABLE/sizeof IMG_CMD_TABLE[0]);
