/* img_cmds.c — 命令表（加新功能就在这里加一行 + 写一个 handler）
 *
 * 每个命令 = {名字, 版本, 帮助, 参数规格[], handler}
 * 参数规格里的类型/必填/默认/校验，CLI 与 App 共用 —— 不再两处各写一遍。
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>   /* img.create/ntfs.put 要 stat 判存在与大小 */
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>   /* qc_open_fail 复查文件要 open/read/close */
#include <ctype.h>    /* toupper：FAT32 卷标规范化 */
#include <fnmatch.h>  /* archive.create 的 --exclude（与 copy 同一套语义）*/
#include <dirent.h>   /* archive.create 扫描源目录 */
#include "img_cmd.h"
#include "img_mbr_tpl.h"
#include "img_fixboot.h"
#include "img_bcd.h"
#include "img_constraints.h"   /* img_layout_build：img.create 用 */
#include "img_ptfile.h"        /* part.export / part.apply：分区表文件（REQ-14） */
#include "img_bootcheck.h"
#include "img_iso.h"
#include "img_vfd.h"      /* 虚拟 fd：ISO 直读（REQ-16） */
#include "img_recovery.h"
#include "img_winre.h"
#include "img_identify.h"      /* boot.check 用 */
#include "img_bootplan.h"      /* boot.plan / bcd.create 的分区表判定 */
#include "img_text.h"        /* 进信封的说明：中英双份 */
#include "img_io.h"
#include "img_fat32.h"
#include "img_fat32_fs.h"
#include "img_event.h"
#include "img_wim_apply.h"
#include "img_wim_fs.h"
#include "img_archive.h"
#include "img_archive_w.h"
#include "img_du.h"
#include "img_trim.h"
#include "img_win_finish.h"
#include "img_reg.h"
#include "img_wim_capture.h"
#include "img_qcow2_int.h"   /* P3：img_vhd_create / img_vmdk_create */
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
#include "img_xcopy.h"
#include "img_path.h"   /* part.scan / ntfs.list / ntfs.rename */
#include "img_name.h"   /* rename/copy/move 的 Windows 文件名规则 */
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
#include "img_tbuf.h"   /* 线程级缓冲（堆上，替代 1.5MB 静态 TLS，REQ-21）*/

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
/* 线程级缓冲分配失败（只可能是内存不足）：统一信封（REQ-21）*/
static int tb_nomem(img_result *out, const char *what){
    img_result_fail(out,IMG_ENOMEM,"HOST-NO-MEM",what,
                    "临时缓冲分配失败（内存不足）","关掉别的 App 再来；或减少一次要列的条目数");
    return IMG_ENOMEM;
}

static const img_arg A_INFO[] = {
    {"img", IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
};
static int cmd_img_info(img_ctx *ctx, const img_args *in, img_result *out){
    (void)in;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){
        img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),
                        "不是合法的 qcow2（魔数或版本不符）",
                        "确认是本工具创建的映像；或先用 imgq2test 校验");
        return IMG_EFORMAT;
    }
    img_result_u64(out,"size_bytes",img_qcow2_size(q));
    /* format：App 按扩展名猜格式并不可靠（实际按魔数判定）。
     *   建盘后要显示实际格式，这里直接报探测结果。*/
    img_result_str(out,"format",img_diskfmt_name(q->fmt));
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
    {"img", IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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
                        "映像可能损坏；用 imglayout check 复核");
        return rc;
    }
    int n=img_parttable_json(pt,g_pt_json,sizeof g_pt_json);
    if(n<=0){ img_result_fail(out,IMG_EIO,"PART-JSON-FAIL",img_ctx_path(ctx),"分区表序列化失败",NULL); return IMG_EIO; }
    img_result_raw(out,"pt",g_pt_json);
    return IMG_OK;
}

/* ================= 命令 3：fat.put（★ 演示 check_size 开关） ================= */
static const img_arg A_FATPUT[] = {
    {"img",        IMG_A_STR,  true,  NULL,   "映像路径（qcow2）",        NULL},
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
                        "清理宿主磁盘，或换更小的源文件/更小的映像");
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
                        "空间不足→扩容分区或换小文件；映像损坏→先 imgq2test 校验");
        return rc;
    }
    img_result_u64(out,"written_bytes",(uint64_t)fsz);
    img_result_i64(out,"ms",ms);
    return IMG_OK;
}

/* ================= 命令 4：fat.ls ================= */
static const img_arg A_FATLS[] = {
    {"img",  IMG_A_STR, true,  NULL, "映像路径（qcow2）",  NULL},
    {"lba",  IMG_A_U64, true,  NULL, "分区起始 LBA",        chk_lba},
    {"path", IMG_A_STR, false, "/",  "分区内目录（/ 开头）", NULL},
    /* 分页：与 ntfs.list 同一套参数与语义（App 只需要学一次）。
     * 以前是 64 KB 缓冲写满就悄悄截断（约 900 项封顶）—— ESP 一般够用，
     * 但"够用"不是"不炸"的理由。 */
    {"offset", IMG_A_U64, false, "0",   "从第几项开始（分页；默认 0）", NULL},
    {"limit",  IMG_A_U64, false, "500", "本页最多多少项（1~2000；默认 500）", NULL},
    {"times",  IMG_A_BOOL, false, "false", "顺带取创建/修改时间（unix 秒 UTC）", NULL},
    {"sort",   IMG_A_ENUM, false, NULL,   "整目录排序：name|size|mtime|dir（与 --after 互斥）", NULL},
    {"desc",   IMG_A_BOOL, false, "false","--sort 反向", NULL},
    {"sort-max",IMG_A_U64, false, "100000","排序最多收多少条；超了报 LIST-TOO-BIG（0=不限制）", NULL},
};
#define FAT_LIST_MAX 2000
#define FAT_LIST_DEFAULT 500
typedef struct { char *p, *end;
                 uint64_t total, offset, limit;
                 int n; bool truncated;
                 img_fat32_fs *fs; const char *dir; bool want_times; } lsbuf;
/* 统一的条目发射器：times 由调用方给（--times 时来自同一次遍历，REQ-30.4）*/
static void ls_emit(lsbuf *b, const char *name, bool isdir, uint64_t size,
                    uint64_t ct, uint64_t mt){
    uint64_t idx=b->total++;                              /* 先计数，再决定要不要写进本页 */
    if(idx < b->offset) return;
    if((uint64_t)b->n >= b->limit) return;
    char item[768];
    int n;
    if(b->want_times)
        n=snprintf(item,sizeof item,
                   "%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu,\"ctime\":%llu,\"mtime\":%llu}",
                   b->n?",":"", name?name:"", isdir?"true":"false",
                   (unsigned long long)size,(unsigned long long)ct,(unsigned long long)mt);
    else
        n=snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu}",
                   b->n?",":"", name?name:"", isdir?"true":"false",(unsigned long long)size);
    if(n<=0) return;
    size_t need=(size_t)n+3;                              /* 还要留得下 "]\0" */
    if(b->p+need>=b->end){ b->truncated=true; return; }
    memcpy(b->p,item,(size_t)n); b->p+=n; b->n++;
}
static void ls_cb(void *ud, const char *name, bool isdir, uint64_t size){
    lsbuf *b=(lsbuf*)ud;
    uint64_t ct=0, mt=0;
    if(b->want_times && b->fs){                 /* 兜底：老调用方没有 times 时仍可查（慢路）*/
        char full[1200]; uint16_t t5[5]={0,0,0,0,0};
        snprintf(full,sizeof full,"%s%s%s",b->dir,
                 (b->dir[0]&&b->dir[strlen(b->dir)-1]=='/')?"":"/",name?name:"");
        if(img_fat32_file_times(b->fs,full,t5)==IMG_OK){
            ct=(uint64_t)img_fat32_dos_to_epoch(t5[0],t5[1]);
            mt=(uint64_t)img_fat32_dos_to_epoch(t5[3],t5[4]);
        }
    }
    ls_emit(b,name,isdir,size,ct,mt);
}
static void ls_full_cb(void *ud, const img_fat32_ent_t *e){
    ls_emit((lsbuf*)ud,e->name,e->is_dir,e->size,e->ctime,e->mtime);
}

#define NTFS_SORT_MAX_DEFAULT 100000ULL   /* REQ-30.1：排序最多收这么多条（App 确认的默认值）*/

/* ---------- FAT 侧的整目录排序（与 NTFS 同一套语义） ---------- */
typedef struct { char name[256]; bool dir; uint64_t size, mtime, ctime; } fatent;
typedef struct { fatent *v; size_t n, cap; } fatvec;
typedef struct { fatvec *v; uint64_t total, lim; bool over; int rc; img_fat32_fs *fs; const char *dir; } fatcollctx;
static void fat_free_vec(fatvec *v){
    if(!v) return;
    free(v->v); v->v=NULL; v->n=v->cap=0;
}
static int fat_ev_push(fatvec *vv, const fatent *e){
    if(vv->n==vv->cap){
        size_t nc=vv->cap?vv->cap*2:256;
        fatent *nv=(fatent*)realloc(vv->v,nc*sizeof *nv);
        if(!nv) return IMG_ENOMEM;
        vv->v=nv; vv->cap=nc;
    }
    vv->v[vv->n++]=*e;
    return IMG_OK;
}
static void fatcoll_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    fatcollctx *c=(fatcollctx*)ud;
    c->total++;
    if(c->over) return;
    if(c->lim && c->total>c->lim){ c->over=true; return; }
    fatent e; memset(&e,0,sizeof e);
    snprintf(e.name,sizeof e.name,"%s",name?name:"");
    e.dir=is_dir; e.size=size;
    if(c->fs){                       /* mtime 排序要时间：收集时顺手取 */
        char full[1200]; uint16_t t5[5]={0,0,0,0,0};
        snprintf(full,sizeof full,"%s%s%s",c->dir,(c->dir[0]&&c->dir[strlen(c->dir)-1]=='/')?"":"/",e.name);
        if(img_fat32_file_times(c->fs,full,t5)==IMG_OK){
            e.mtime=(uint64_t)img_fat32_dos_to_epoch(t5[3],t5[4]);
            e.ctime=(uint64_t)img_fat32_dos_to_epoch(t5[0],t5[1]);
        }
    }
    int r=fat_ev_push(c->v,&e);
    if(r!=IMG_OK){ c->rc=r; c->over=true; }
}
typedef struct { const char *field; bool desc; } fatcmp_ctx;
static fatcmp_ctx fat_SORT;
static int fat_entcmp(const void *pa, const void *pb){
    const fatent *a=(const fatent*)pa, *b=(const fatent*)pb;
    int r=0;
    if(!strcmp(fat_SORT.field,"size")){ r=(a->size<b->size)?-1:(a->size>b->size)?1:0; }
    else if(!strcmp(fat_SORT.field,"mtime")){ r=(a->mtime<b->mtime)?-1:(a->mtime>b->mtime)?1:0; }
    else if(!strcmp(fat_SORT.field,"dir")){ if(a->dir!=b->dir) r=a->dir?-1:1; }
    if(r==0){ r=strcasecmp(a->name,b->name); if(r==0) r=strcmp(a->name,b->name); }
    return fat_SORT.desc?-r:r;
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
    enum { G_LS_JSON_CAP = 65536 };
    char *g_ls_json=img_tbuf(IMG_TB_LS,G_LS_JSON_CAP);
    if(!g_ls_json) return tb_nomem(out,"fat.ls 条目缓冲");
    g_ls_json[0]='[';
    bool want_times=img_args_bool(in,"times",false);
    lsbuf b={g_ls_json+1, g_ls_json+G_LS_JSON_CAP-4, 0, offset, limit, 0, false,
             fs, path, want_times};
    const char *sortk=img_args_get(in,"sort");
    if(sortk && !*sortk) sortk=NULL;
    if(sortk && strcmp(sortk,"name") && strcmp(sortk,"size") && strcmp(sortk,"mtime") && strcmp(sortk,"dir")){
        img_fat32_unmount(fs);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sort","sort 只能是 name|size|mtime|dir",NULL);
        return IMG_EINVAL;
    }
    uint64_t sort_max=img_args_u64(in,"sort-max",NTFS_SORT_MAX_DEFAULT);
    int rc;
    if(sortk){
        /* REQ-30.1 同 NTFS：整目录收集 → 排序 → 只序列化本页 */
        fatvec vv; memset(&vv,0,sizeof vv);
        fatcollctx cc={&vv,0,sort_max,false,IMG_OK,fs,path};
        rc=img_fat32_list_dir(fs,path,fatcoll_cb,&cc);
        if(rc==IMG_OK && cc.rc!=IMG_OK) rc=cc.rc;
        if(rc==IMG_OK && cc.over){
            fat_free_vec(&vv);
            img_fat32_unmount(fs);
            img_result_u64(out,"total",cc.total);
            img_result_u64(out,"sort_max",sort_max);
            img_result_fail(out,IMG_ESTATE,"LIST-TOO-BIG","sort","目录太大，排序前先缩小范围",
                            "调大 --sort-max（0=不限制），或退回客户端排序");
            return IMG_ESTATE;
        }
        if(rc!=IMG_OK){
            fat_free_vec(&vv);
            img_fat32_unmount(fs);
            img_result_fail(out,rc,"FAT-LS-FAIL",path,img_strerror(rc),"映像可能损坏");
            return rc;
        }
        fat_SORT.field=sortk; fat_SORT.desc=img_args_bool(in,"desc",false);
        if(vv.n>1) qsort(vv.v,vv.n,sizeof vv.v[0],fat_entcmp);
        for(size_t i=0;i<vv.n && !b.truncated;i++)
            ls_emit(&b,vv.v[i].name,vv.v[i].dir,vv.v[i].size,vv.v[i].ctime,vv.v[i].mtime);
        fat_free_vec(&vv);
        img_result_str(out,"sort",sortk);
        img_result_bool(out,"desc",fat_SORT.desc);
    } else if(want_times){
        /* ★ REQ-30.4：--times 走一次遍历（以前每条再扫一遍目录，O(n²)）*/
        rc=img_fat32_list_dir_full(fs,path,ls_full_cb,&b);
    } else {
        rc=img_fat32_list_dir(fs,path,ls_cb,&b);
    }
    b.p[0]=']'; b.p[1]=0;
    img_fat32_unmount(fs);
    if(rc==IMG_ENOTSUP){
        img_result_fail(out,IMG_ENOENT,"FAT-DIR-NOTFOUND",path,"目录不存在或不是目录","核对路径");
        return IMG_ENOENT;
    }
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,img_strerror(rc),"映像可能损坏"); return rc; }
    img_result_u64(out,"total",b.total);
    img_result_u64(out,"offset",offset);
    img_result_u64(out,"limit",limit);
    img_result_u64(out,"count",(uint64_t)b.n);
    img_result_bool(out,"has_more", offset+(uint64_t)b.n < b.total);
    img_result_bool(out,"truncated",b.truncated);
    img_result_raw(out,"entries",g_ls_json);
    return IMG_OK;
}

/* ---------- ISO 里找 WIM/ESD + 直读（REQ-20261026-16）----------
 * 以前只认两条硬编码路径 /sources/install.wim|esd —— "Win10 合一" 那种
 *   /x86/sources/install.wim
 *   /x64/sources/install.wim
 * 一条都找不到。现在：递归扫（只往 sources/ 和根的一级目录里走，避免在大 ISO 上乱翻），
 * 并把每条里的映像条目一起回给 App。
 * ★ 直读：wimlib 只吃真实文件路径，所以走 img_vfd（虚拟 fd，见 img_vfd.h）——
 *   一个字节都不落盘。本构建的 wimlib 若是共享库，自动退回"先抽到临时文件"。*/
#define WIMSCAN_MAX 16

typedef struct {
    char     path[WIMSCAN_MAX][256];
    uint64_t size[WIMSCAN_MAX];
    int      n;
    bool     trunc;
} wimscan_t;

typedef struct {
    wimscan_t *a;
    const char *dir;
    char subs[8][256];
    int nsub;
    int depth;
} wimscan_ctx;

static const char *join_dir(const char *dir, const char *name, char *out, size_t cap){
    if(!dir || !*dir || !strcmp(dir,"/")) snprintf(out,cap,"/%s",name);
    else snprintf(out,cap,"%s/%s",dir,name);
    return out;
}

static void wimscan_cb(void *ud, const char *name, bool is_dir, uint64_t size, uint64_t mtime){
    (void)mtime;
    wimscan_ctx *c=(wimscan_ctx*)ud;
    if(!c || !name) return;
    char full[256]; join_dir(c->dir,name,full,sizeof full);
    if(is_dir){
        /* 只往两层里走：根（x86/x64/arm64/sources…）和名叫 sources 的目录 */
        if(c->depth==0 || !strcasecmp(name,"sources")){
            if(c->nsub<8) snprintf(c->subs[c->nsub++],256,"%s",full);
        }
        return;
    }
    size_t l=strlen(name);
    if(l>4 && (!strcasecmp(name+l-4,".wim") || !strcasecmp(name+l-4,".esd"))){
        if(c->a->n>=WIMSCAN_MAX){ c->a->trunc=true; return; }
        snprintf(c->a->path[c->a->n],256,"%s",full);
        c->a->size[c->a->n]=size;
        c->a->n++;
    }
}

/* 递归扫（广度优先，层数 ≤3：/ → /x64 → /x64/sources）*/
static void wim_scan_iso(img_iso *iso, wimscan_t *a){
    memset(a,0,sizeof *a);
    char q[24][256]; int qn=0;
    snprintf(q[qn++],256,"/");
    for(int qi=0; qi<qn && qi<24; qi++){
        wimscan_ctx c; memset(&c,0,sizeof c);
        c.a=a; c.dir=q[qi];
        c.depth = (qi==0)?0:1;
        img_iso_list(iso,q[qi],wimscan_cb,&c);
        for(int i=0;i<c.nsub && qn<24;i++) snprintf(q[qn++],256,"%s",c.subs[i]);
    }
}

/* 选一个内部 WIM：--inner 优先；否则老的两条硬编码；再否则扫出来的唯一一条。
 * 返回 IMG_OK 时 out 是 ISO 内部路径。多个候选且没指定 → IMG_EINVAL（把候选列在 why 里）。*/
static int wim_pick_inner(img_iso *iso, const char *want, char *out, size_t cap,
                          char *why, size_t whycap){
    if(why && whycap) why[0]=0;
    if(want && *want){
        uint64_t sz=0; bool d=false;
        if(img_iso_stat(iso,want,&sz,&d)!=IMG_OK || d){
            if(why && whycap) snprintf(why,whycap,"ISO 里没有这个文件：%s",want);
            return IMG_ENOENT;
        }
        snprintf(out,cap,"%s",want);
        return IMG_OK;
    }
    const char *cands[2]={"/sources/install.wim","/sources/install.esd"};
    for(int i=0;i<2;i++){
        uint64_t sz=0; bool d=false;
        if(img_iso_stat(iso,cands[i],&sz,&d)==IMG_OK && !d){ snprintf(out,cap,"%s",cands[i]); return IMG_OK; }
    }
    wimscan_t sc; wim_scan_iso(iso,&sc);
    if(sc.n==1){ snprintf(out,cap,"%s",sc.path[0]); return IMG_OK; }
    if(sc.n==0){
        if(why && whycap) snprintf(why,whycap,"这个 ISO 里没有 sources\\install.wim / install.esd");
        return IMG_ENOENT;
    }
    if(why && whycap){
        size_t k=(size_t)snprintf(why,whycap,"ISO 里有 %d 个 install.wim/esd，要用 --inner 指定：",sc.n);
        for(int i=0;i<sc.n && k<whycap-60;i++)
            k+=(size_t)snprintf(why+k,whycap-k,"%s%s",i?"，":"",sc.path[i]);
    }
    return IMG_EINVAL;
}

/* 把 --wim 解析成"wimlib 能打开的路径"。
 *   plain   普通 .wim/.esd（原样）
 *   vfd     ISO + 虚拟 fd（用完 img_vfd_unmount(path)）
 *   extract ISO + 抽到临时文件（用完 unlink(path)）
 * extracted=true 表示用了退路；out_bytes 是抽出来的字节数。*/
typedef enum { WIMSRC_PLAIN=0, WIMSRC_VFD=1, WIMSRC_EXTRACT=2 } wimsrc_kind;

static int wim_resolve_source(const char *wim, const char *inner_opt,
                              char *path, size_t cap,
                              char *used_inner, size_t icap,
                              wimsrc_kind *kind, uint64_t *out_bytes,
                              char *why, size_t whycap, img_prog *prog){
    if(kind) *kind=WIMSRC_PLAIN;
    if(out_bytes) *out_bytes=0;
    if(used_inner && icap) used_inner[0]=0;
    if(why && whycap) why[0]=0;
    if(!wim || !*wim) return IMG_EINVAL;
    size_t n=strlen(wim);
    bool looks_iso = n>4 && !strcasecmp(wim+n-4,".iso");
    if(!looks_iso){ snprintf(path,cap,"%s",wim); return IMG_OK; }

    img_iso *iso=NULL;
    char err[256]="";
    int rc=img_iso_open_path(&iso,wim,err,sizeof err);
    if(rc!=IMG_OK){ if(why&&whycap) snprintf(why,whycap,"%s",err[0]?err:"ISO 无法打开"); return rc; }
    char inner[256]="";
    rc=wim_pick_inner(iso,inner_opt,inner,sizeof inner,why,whycap);
    if(rc!=IMG_OK){ img_iso_close(iso); return rc; }
    if(used_inner && icap) snprintf(used_inner,icap,"%s",inner);

    if(img_vfd_ready()){
        img_iso_close(iso);                       /* VFD 自己持有一份句柄 */
        uint64_t sz=0;
        rc=img_vfd_mount(wim,inner,path,cap,&sz,err,sizeof err);
        if(rc==IMG_OK){ if(kind)*kind=WIMSRC_VFD; return IMG_OK; }
        /* VFD 挂了就退回复制，别让用户卡住 */
        if(why&&whycap) snprintf(why,whycap,"%s",err[0]?err:"虚拟直读失败");
        rc=img_iso_open_path(&iso,wim,err,sizeof err);
        if(rc!=IMG_OK) return rc;
        if(why&&whycap) why[0]=0;
    }
    const char *tmp=img_tmpdir();
    if(!tmp || !tmp[0]){ img_iso_close(iso); return IMG_ESTATE; }
    /* ★ P1-6：统一临时命名 */
    {
        const char *purpose=(strstr(inner,".esd")||strstr(inner,".ESD"))?"src-esd":"src-wim";
        if(img_tmpfile(purpose,path,cap)==NULL){ img_iso_close(iso); return IMG_ESTATE; }
    }
    uint64_t got=0;
    rc=img_iso_extract(iso,inner,path,0,prog,&got);
    img_iso_close(iso);
    if(rc!=IMG_OK){ unlink(path); path[0]=0; return rc; }
    if(kind) *kind=WIMSRC_EXTRACT;
    if(out_bytes) *out_bytes=got;
    return IMG_OK;
}

/* 把 wim_resolve_source 的失败翻译成统一的错误信封（4 条命令共用一个口径）：
 *   EINVAL  → CMD-ARG-BAD@inner（ISO 里有多条，要 --inner 指定）
 *   ENOTSUP → WIM-ISO-UNSUPPORTED（本构建的 wimlib 拦不住 IO）
 *   其它    → WIM-NOTFOUND（找不到/打不开）*/
static int wim_src_fail(img_result *out, int rc, const char *path, const char *why){
    if(rc==IMG_EINVAL){
        img_result_fail(out,rc,"CMD-ARG-BAD","inner",
            why[0]?why:"ISO 里有多个 install.wim/esd，请用 --inner 指定",
            "用 wim.identify --scan=true 列出候选，再带 --inner=/x64/sources/install.wim");
    } else if(rc==IMG_ENOTSUP){
        img_result_fail(out,rc,"WIM-ISO-UNSUPPORTED",path,
            why[0]?why:"本构建不支持从 ISO 里直读",
            "换完整版 so；或先用 iso.extract 把 install.wim 提取来");
    } else {
        img_result_fail(out,rc,"WIM-NOTFOUND",path,
            why[0]?why:"WIM/ESD 无法打开（不存在或不是 WIM 格式）",
            "用 wim.identify --scan=true 列出 ISO 里的候选，再带 --inner=…");
    }
    return rc;
}

static void wim_src_cleanup(const char *path, wimsrc_kind kind){
    if(!path || !path[0]) return;
    if(kind==WIMSRC_VFD) img_vfd_unmount(path);
    else if(kind==WIMSRC_EXTRACT) unlink(path);
}

/* ISO 被抽出来当退路时，结果里说实话（App 靠它决定要不要提示"多占了 X GB"）*/
static void wim_src_note(img_result *out, wimsrc_kind kind, const char *inner, uint64_t bytes){
    if(kind==WIMSRC_PLAIN) return;
    img_result_str(out,"container","iso");
    if(inner && *inner) img_result_str(out,"inner_path",inner);
    img_result_bool(out,"direct", kind==WIMSRC_VFD);
    if(kind==WIMSRC_EXTRACT){
        img_result_bool(out,"extracted",true);
        img_result_u64(out,"extracted_bytes",bytes);
    }
}
/* ================= 命令 5：wim.size（尺寸估算，政策留给 App） ================= */
static const img_arg A_WIMSIZE[] = {
    {"wim",   IMG_A_STR, true,  NULL, "WIM/ESD 路径（也可以直接给 ISO，内部自动找 install.wim）", NULL},
    {"index", IMG_A_U64, false, "1",  "映像索引（见 --list 用 imgwimls 查）", NULL},
    {"inner", IMG_A_STR, false, NULL, "（ISO）内部路径，如 /x64/sources/install.wim", NULL},
};
static int cmd_wim_size(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 尺寸估算）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_WIM

    (void)ctx;
    const char *wim=img_args_get(in,"wim");
    int idx=(int)img_args_u64(in,"index",1);
    /* ★ REQ-16：--wim 可以直接是 ISO（虚拟 fd 直读，不抽 4~5 GB 临时文件）*/
    char src[1024], inner[256]="", why[400]="";
    wimsrc_kind sk=WIMSRC_PLAIN; uint64_t sbytes=0;
    int rc0=wim_resolve_source(wim,img_args_get(in,"inner"),src,sizeof src,
                               inner,sizeof inner,&sk,&sbytes,why,sizeof why,img_cmd_prog());
    if(rc0!=IMG_OK) return wim_src_fail(out,rc0,wim,why);
    img_wim_size_t sz;
    int rc=img_wim_estimate(src,idx,&sz);
    wim_src_cleanup(src,sk);
    if(rc!=IMG_OK){
        img_result_fail(out,rc, rc==IMG_ENOENT?"WIM-NOTFOUND":"WIM-IDX-INVALID", wim,
                        rc==IMG_ENOENT?"WIM/ESD 无法打开（不存在或不是 WIM 格式）"
                                      :"映像索引不存在",
                        "先用 imgwimls <wim> 列出索引，再带 --index=N");
        return rc;
    }
    wim_src_note(out,sk,inner,sbytes);
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
    {"img",    IMG_A_STR,  true,  NULL,      "映像路径（qcow2）", NULL},
    {"lba",    IMG_A_U64,  true,  NULL,      "目标分区起始 LBA",  chk_lba},
    {"wim",    IMG_A_STR,  true,  NULL,      "WIM/ESD 文件路径（也可以直接给 ISO）",  NULL},
    {"inner",  IMG_A_STR,  false, NULL,      "（ISO）内部路径，如 /x64/sources/install.wim", NULL},
    {"index",  IMG_A_U64,  false, "1",       "WIM 内的映像索引",  NULL},
    {"format", IMG_A_ENUM, false, "on",       "写入前是否格式化目标分区（on|off）；★不传=on（会毁目标分区数据）", NULL},
    {"label",  IMG_A_STR,  false, "Windows", "（format=on）卷标", NULL},
    {"no_acls", IMG_A_BOOL,false, "false",   "不要 ACL（跨平台/不需要时开）", NULL},
    {"no_attrs",IMG_A_BOOL,false, "false",   "不要扩展属性", NULL},
    {"wimboot", IMG_A_BOOL,false, "false",   "wimboot 模式（文件指针化）", NULL},
};
static int cmd_wim_apply_cmd(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 直接写入）","重新编译时开启该功能并安装对应系统库");
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

    /* ★ REQ-16：--wim 可以是 ISO。直读（虚拟 fd）→ 一个字节都不落盘；
     *   本构建的 wimlib 若是共享库，wim_resolve_source 自动退回抽取并如实上报。*/
    char src[1024], inner[256]="", why[400]="";
    wimsrc_kind sk=WIMSRC_PLAIN; uint64_t sbytes=0;
    int rc0=wim_resolve_source(wim,img_args_get(in,"inner"),src,sizeof src,
                               inner,sizeof inner,&sk,&sbytes,why,sizeof why,img_cmd_prog());
    if(rc0!=IMG_OK) return wim_src_fail(out,rc0,wim,why);

    /* ★ 空间预判（2026-10-26 真机踩到）：Win11 x64 的 install.wim index=1 展开要 28.9 GB，
     *   往 12 GB 分区里灌 → wimlib 干跑 4 分钟才失败，只报 "wimlib 返回 4"，谁也猜不到。
     *   分区大小是硬上限：估算比它还大就【必失败】，先拦掉（wim.size 给的也是这个数）。*/
    {   img_wim_size_t est; memset(&est,0,sizeof est);
        if(img_wim_estimate(src,idx,&est)==IMG_OK && est.est_ntfs_bytes && est.est_ntfs_bytes>pb){
            wim_src_cleanup(src,sk);
            img_result_u64(out,"need_bytes",est.est_ntfs_bytes);
            img_result_u64(out,"part_bytes",pb);
            img_result_fail(out,IMG_EFULL,"PART-NOSPACE","lba",
                "系统分区比映像展开后的体积还小，写到一半必然失败（空等数分钟）",
                "建盘时把 --sys-mb 加大（需求值见 wim.size 的 est_ntfs_bytes），或换更小的映像/版本");
            return IMG_EFULL;
        }
    }

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
                wim_src_cleanup(src,sk);
                img_result_fail(out,IMG_ESTATE,"NTFS-VOLUME-DIRTY",img_ctx_path(ctx),
                    "目标分区被标脏（上次写入被中断或掉电）：ntfs-3g 对脏卷只给只读挂载，写不进去。"
                    "加 --format=on 重新格式化后写入（会清空该分区）",NULL);
                return IMG_ESTATE;
            }
        }
    }
    /* ① 里程碑：格式化（默认开） */
    if(do_fmt){
        img_span sp;
        img_span_prep(&sp,"ntfs","format","格式化系统分区（写入 WIM 前）");
        img_span_kv_u64(&sp,"lba",lba);
        img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,lba,pb);
        o.label=img_args_get(in,"label");
        rc=img_ntfs_format(q,&o,img_cmd_prog());
        img_span_end(&sp,rc);
        if(rc==IMG_ENOTSUP){ wim_src_cleanup(src,sk); return feat_unavail(out,"NTFS 格式化模块（mkntfs）",
                        "重新编译时开启 -DIMG_WITH_NTFS=ON（会引入 GPL-2.0+ 代码）"); }
        if(rc){ wim_src_cleanup(src,sk);
                img_result_fail(out,rc,"NTFS-FORMAT-FAIL",NULL,NULL,NULL); return rc; }
    }

    /* ② 里程碑：灌入 */
    img_wim_apply_opts_t wo; memset(&wo,0,sizeof wo);
    wo.no_acls  =img_args_bool(in,"no_acls",false);
    wo.no_attrs =img_args_bool(in,"no_attrs",false);
    wo.wimboot  =img_args_bool(in,"wimboot",false);
    rc=img_wim_apply(src,idx,q,lba,&wo,img_cmd_prog());
    wim_src_cleanup(src,sk);
    if(rc){
        if(rc==IMG_ENOTSUP) return feat_unavail(out,"可写临时目录",
                                    "先调 Native.setTmpdir(context.getCacheDir() 的路径)");
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":"WIM-APPLY-FAIL";
        /* ★ REQ-48 §1：wimlib 原始码 → 枚举名 + 中文含义 + 按码给下一步。
         *   以前一律"分区已经写满，请 --format=on"——纯粹是猜，46 其实是
         *   libntfs-3g 底层报错（脏卷/只读/空间），41 才是"目录非空"。*/
        int wr=img_wim_last_rc();
        const char *wn=img_wimlib_err_name(wr);
        const char *wz=img_wimlib_err_zh(wr);
        static char hint[600], sug[600];
        hint[0]=sug[0]=0;
        switch(wr){
        case 41:
            snprintf(hint,sizeof hint,"目标目录非空（wimlib 目录模式）：要么把现有内容搬进 Windows.old，要么 --format=on");
            snprintf(sug,sizeof sug,"ntfs.mkdir Windows.old → 逐项 move → 再重跑 --format=off（保持可回退）");
            break;
        case 46:
            snprintf(hint,sizeof hint,"libntfs-3g 写不进目标卷：%s。常见是上次写入被中断或掉电 → 卷被标脏，ntfs-3g 只给只读挂载",wz?wz:"WIMLIB_ERR_NTFS_3G");
            snprintf(sug,sizeof sug,"加 --format=on 重新格式化后写入（会清空该分区）；若分区剩余空间不足，先看根分区可用字节");
            break;
        case 51:
            snprintf(hint,sizeof hint,"写目标卷失败：%s",wz?wz:"WIMLIB_ERR_WRITE");
            snprintf(sug,sizeof sug,"确认分区足够大（wim.size 的 est_ntfs_bytes）、卷没被标脏；必要时 --format=on");
            break;
        case 2: case 13: case 17: case 21: case 43: case 50:
            snprintf(hint,sizeof hint,"WIM/ESD 本身读不出来：%s",wz?wz:"数据损坏");
            snprintf(sug,sizeof sug,"换一份完整映像重试（wim.identify 能过才算完整）；不要在这个文件上反复重试");
            break;
        case 18:
            snprintf(hint,sizeof hint,"映像索引不存在：--index 超出范围（%s）",wz?wz:"WIMLIB_ERR_INVALID_IMAGE");
            snprintf(sug,sizeof sug,"先 wim.identify 看有几个映像，再传 1..N");
            break;
        case 47: case 49:
            snprintf(hint,sizeof hint,"WIM/ESD 无法打开：%s",wz?wz:"路径不存在或无权限");
            snprintf(sug,sizeof sug,"核对 --wim 路径与读权限（Android 上要真实路径，别传 content://）");
            break;
        case 39:
            snprintf(hint,sizeof hint,"内存不足（wimlib 返回 39）");
            snprintf(sug,sizeof sug,"关掉其它 App 再试；大映像在低内存机上可能无解");
            break;
        case 0:
            snprintf(hint,sizeof hint,"wimlib 没报错，但写入被中断（通常是取消/看门狗触发）");
            snprintf(sug,sizeof sug,"看日志里的 cancel/IO-TIMEOUT；重跑即可，已写入的部分会被覆盖");
            break;
        default:
            snprintf(hint,sizeof hint,"wimlib 返回 %d（%s）%s%s",wr,wn?wn:"?",wz?": ":"",wz?wz:"");
            snprintf(sug,sizeof sug,"把 wimlib_code 一起反馈；若是中断后重跑失败，优先试 --format=on");
            break;
        }
        img_result_fail(out,rc,code,wim,hint,sug);
        if(rc!=IMG_ECANCELLED){
            img_result_i64(out,"wimlib_code",wr);
            if(wn) img_result_str(out,"wimlib_name",wn);
            if(wz) img_result_str(out,"wimlib_msg",wz);
            /* 给 App 一个【布尔】就够决定要不要自动搬 Windows.old / 重格式化 */
            img_result_bool(out,"format_on_would_help", wr==41||wr==46||wr==51);
            img_result_bool(out,"image_looks_corrupt", wr==2||wr==13||wr==17||wr==21||wr==43||wr==50);
        }
        return rc;
    }
    img_result_u64(out,"index",(uint64_t)idx);
    img_result_bool(out,"formatted",do_fmt);
    wim_src_note(out,sk,inner,sbytes);
    if(root_entries>=0) img_result_u64(out,"root_entries",(uint64_t)root_entries);   /* 灌入前根目录项数（App 可据此判断"这是覆盖写入"）*/
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
    {"img", IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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
    {"img",  IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
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
    {"sort",   IMG_A_ENUM, false, NULL,   "整目录排序：name|size|mtime|dir（与 --after 互斥；用 offset/limit 续页）", NULL},
    {"desc",   IMG_A_BOOL, false, "false","--sort 反向", NULL},
    {"sort-max",IMG_A_U64, false, "100000","排序最多收多少条；超了报 LIST-TOO-BIG（0=不限制）", NULL},
    {"show-8.3",IMG_A_BOOL, false, "false","把 8.3 短名（PROGRA~1）也列出来（默认隐去，与资源管理器一致；REQ-49 §1）", NULL},
};
/* 按 LBA 从分区表取分区字节数（与文件系统无关，FAT/NTFS 都用 ⇒ 不能放在 NTFS 守卫里） */
static uint64_t ntfs_part_bytes(img_parttable_t *t,uint64_t lba){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba)
            return (t->parts[i].last_lba - t->parts[i].first_lba + 1)*512;
    return 0;
}

/* JSON 字符串转义（压缩包 archive.ls 与 NTFS 目录枚举共用，故放在 #if 外） */
static int json_esc(char *dst, size_t cap, const char *src){
    int k=0;
    for(const unsigned char *p=(const unsigned char*)src; *p && k<(int)cap-8; p++){
        if(*p=='"'||*p=='\\'){ dst[k++]='\\'; dst[k++]=(char)*p; }
        else if(*p<0x20) k+=snprintf(dst+k,cap-(size_t)k,"\\u%04x",*p);
        else dst[k++]=(char)*p;
    }
    dst[k]=0;
    return k;
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
/* ---------- REQ-30.3/30.4：一次遍历的发射器（times/ino/sort 共用） ---------- */
typedef struct {
    list_buf_t *a;
    bool        want_sort_key;
    const char *sort_key_field;
} ent_emit_t;
static void ent_emit(ent_emit_t *em, const img_ntfs_ent_t *e){
    list_buf_t *a=em->a;
    if(a->after && !a->after_seen){
        if(e->ino != a->after) return;
        a->after_seen = true; return;
    }
    uint64_t idx=a->total++;
    if(idx < a->offset) return;
    if((uint64_t)a->n >= a->limit) return;
    char name[600]; json_esc(name,sizeof name,e->name);
    char item[900];
    int k=snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"dir\":%s,\"size\":%llu",
                   a->n?",":"",name,e->is_dir?"true":"false",(unsigned long long)e->size);
    if(a->want_ino || a->after)
        k+=snprintf(item+k,sizeof item-(size_t)k,",\"ino\":%llu",(unsigned long long)e->ino);
    if(a->want_times)
        k+=snprintf(item+k,sizeof item-(size_t)k,
                    ",\"ctime\":%llu,\"mtime\":%llu,\"atime\":%llu,\"change_time\":%llu",
                    (unsigned long long)e->times[0],(unsigned long long)e->times[1],
                    (unsigned long long)e->times[2],(unsigned long long)e->times[3]);
    if(em->want_sort_key){
        char sk[64];
        if(!strcmp(em->sort_key_field,"size")) snprintf(sk,sizeof sk,"%020llu",(unsigned long long)e->size);
        else if(!strcmp(em->sort_key_field,"mtime")) snprintf(sk,sizeof sk,"%020llu",(unsigned long long)e->times[1]);
        else if(!strcmp(em->sort_key_field,"dir")) snprintf(sk,sizeof sk,"%d",e->is_dir?0:1);
        else snprintf(sk,sizeof sk,"%s",e->name);
        char esk[600]; json_esc(esk,sizeof esk,sk);
        k+=snprintf(item+k,sizeof item-(size_t)k,",\"sort_key\":\"%s\"",esk);
    }
    k+=snprintf(item+k,sizeof item-(size_t)k,"}");
    if(a->len+(size_t)k+2 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=k; a->n++; }
    else a->truncated=true;
}
static void list_ent_cb(void *ud, const img_ntfs_ent_t *e){
    ent_emit_t *em=(ent_emit_t*)ud;
    ent_emit(em,e);
}

/* ---------- REQ-30.1：整目录排序 ---------- */
typedef struct { img_ntfs_ent_t *v; char **names; size_t n, cap; } entvec;
static int ev_push(entvec *vv, const img_ntfs_ent_t *e){
    if(vv->n==vv->cap){
        size_t nc=vv->cap?vv->cap*2:256;
        img_ntfs_ent_t *nv=(img_ntfs_ent_t*)realloc(vv->v,nc*sizeof *nv);
        if(!nv) return IMG_ENOMEM;
        vv->v=nv;
        char **nn=(char**)realloc(vv->names,nc*sizeof *nn);
        if(!nn) return IMG_ENOMEM;
        vv->names=nn; vv->cap=nc;
    }
    vv->v[vv->n]=*e;
    char *cp=strdup(e->name?e->name:"");
    if(!cp) return IMG_ENOMEM;
    vv->names[vv->n]=cp;
    vv->v[vv->n].name=cp;
    vv->n++;
    return IMG_OK;
}
typedef struct { const char *field; bool desc; } entcmp_ctx;
static entcmp_ctx g_entcmp;              /* 只在 cmd_ntfs_list 里同步用，无并发风险 */
static int entcmp(const void *pa, const void *pb){
    const img_ntfs_ent_t *a=(const img_ntfs_ent_t*)pa, *b=(const img_ntfs_ent_t*)pb;
    int r=0;
    if(!strcmp(g_entcmp.field,"size")){
        if(a->size<b->size) r=-1; else if(a->size>b->size) r=1;
    } else if(!strcmp(g_entcmp.field,"mtime")){
        if(a->times[1]<b->times[1]) r=-1; else if(a->times[1]>b->times[1]) r=1;
    } else if(!strcmp(g_entcmp.field,"dir")){
        if(a->is_dir!=b->is_dir) r=a->is_dir?-1:1;
    }
    if(r==0){                                   /* 同名/同大小 → 名字兜底，保证稳定 */
        r=strcasecmp(a->name?a->name:"",b->name?b->name:"");
        if(r==0) r=strcmp(a->name?a->name:"",b->name?b->name:"");
    }
    return g_entcmp.desc?-r:r;
}

typedef struct { entvec *v; uint64_t total; uint64_t lim; bool over; int rc; } collctx;
static void coll_cb(void *ud, const img_ntfs_ent_t *e){
    collctx *c=(collctx*)ud;
    c->total++;
    if(c->over) return;
    if(c->lim && c->total > c->lim){ c->over=true; return; }
    int r=ev_push(c->v,e);
    if(r!=IMG_OK){ c->rc=r; c->over=true; }
}
static void free_entvec(entvec *v){
    if(!v) return;
    for(size_t i=0;i<v->n;i++) free(v->names[i]);
    free(v->names); free(v->v);
    v->v=NULL; v->names=NULL; v->n=v->cap=0;
}

#endif /* IMG_WITH_NTFS3G —— 上面是 NTFS 目录枚举辅助族（list_buf_t/ent_emit/entvec），NTFS=OFF 时全跳过 */

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
    enum { JBUF_CAP = 262144 };
    char *jbuf=img_tbuf(IMG_TB_NTFS_LIST,JBUF_CAP);
    if(!jbuf) return tb_nomem(out,"ntfs.list 条目缓冲");
    uint64_t after_ino = img_args_u64(in,"after",0);
    bool want_ino = img_args_bool(in,"ino",false) || after_ino != 0;
    /* rev：目录 $SI 的"改动时间"（秒）。App 用它判断自己那份缓存过期没（§22）。*/
    uint64_t rev = 0;
    (void)img_ntfs_dir_rev(fs, path, &rev);
    bool want_sizes = img_args_bool(in,"sizes",false);
    bool want_times = img_args_bool(in,"times",false);
    const char *sortk=img_args_get(in,"sort");
    if(sortk && !*sortk) sortk=NULL;
    bool desc=img_args_bool(in,"desc",false);
    /* ★ REQ-30.1（App 2026-10-27 确认）：--sort 与 --after 互斥，不许静默忽略其中一条 */
    if(sortk && after_ino){
        img_ntfs_umount(fs);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","after",
                        "--sort 与 --after 互斥","带 --sort 时用 --offset/--limit 续页，或看返回里的 sort_key");
        return IMG_EINVAL;
    }
    if(sortk && strcmp(sortk,"name") && strcmp(sortk,"size") && strcmp(sortk,"mtime") && strcmp(sortk,"dir")){
        img_ntfs_umount(fs);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sort",
                        "sort 只能是 name|size|mtime|dir",NULL);
        return IMG_EINVAL;
    }
    uint64_t sort_max=img_args_u64(in,"sort-max",NTFS_SORT_MAX_DEFAULT);
    bool show83=img_args_bool(in,"show-8.3",false);   /* REQ-49 §1 */
    list_buf_t acc={jbuf,JBUF_CAP,0,0,offset,limit,0,false,
                    fs,path,want_times,want_ino,after_ino,false};
    ent_emit_t em={&acc,false,NULL};
    if(sortk){
        /* 整目录收集 → 排序 → 只序列化本页（total 仍是过滤后的全量）*/
        entvec vv; memset(&vv,0,sizeof vv);
        collctx cc={&vv,0,sort_max,false,IMG_OK};
        rc=img_ntfs_list_dir_full_show83(fs,path,show83,coll_cb,&cc);
        if(rc==IMG_OK && cc.rc!=IMG_OK) rc=cc.rc;
        if(rc==IMG_OK && cc.over){
            /* ★ App 明确要求：超限就报错，绝不降级/半排序 */
            free_entvec(&vv);
            img_ntfs_umount(fs);
            img_result_u64(out,"total",cc.total);
            img_result_u64(out,"sort_max",sort_max);
            img_result_fail(out,IMG_ESTATE,"LIST-TOO-BIG","sort",
                            "目录太大，排序前先缩小范围",
                            "调大 --sort-max（0=不限制），或退回客户端排序");
            return IMG_ESTATE;
        }
        if(rc!=IMG_OK){
            free_entvec(&vv);
            img_ntfs_umount(fs);
            img_result_fail(out,rc,"FAT-LS-FAIL",path,"目录无法读取（路径不对或卷损坏）",NULL);
            return rc;
        }
        g_entcmp.field=sortk; g_entcmp.desc=desc;
        if(vv.n>1) qsort(vv.v,vv.n,sizeof vv.v[0],entcmp);
        em.want_sort_key=true; em.sort_key_field=sortk;
        for(size_t i=0;i<vv.n && !acc.truncated;i++)
            ent_emit(&em,&vv.v[i]);
        free_entvec(&vv);
        img_result_str(out,"sort",sortk);
        img_result_bool(out,"desc",desc);
    } else    if(want_times){
        /* ★ REQ-30.4：一次遍历就把时间/大小/ino 全带出来（以前 --times 每条再查一次）*/
        rc=img_ntfs_list_dir_full_show83(fs,path,show83,list_ent_cb,&em);
    } else {
        rc = want_sizes ? img_ntfs_list_dir_sizes(fs,path,list_json_cb,&acc)
                        : img_ntfs_list_dir_show83(fs,path,show83,list_json_cb,&acc);
    }
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
        if(acc.len+2 >= (int)JBUF_CAP) acc.len=(int)JBUF_CAP-3;
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

/* ================= 命令 6e：img.clone（克隆保留分区到新盘 —— 换系统的主路径）====================
 * 文件名流程（PARTITION-POLICY #10 + 两条硬护栏）：
 *   X.qcow2 ─①改名─▶ X.old.qcow2（App 可传 old_name） ─②写新─▶ 新镜像占原名 X.qcow2
 *            ─③校验─▶ 成功：按 keep_old 决定旧副本删/留（默认删 = #9）
 *                    失败：新盘删掉、旧镜像【改回原名】（护栏 2：用户文件永远在原路径）
 *   ★ 校验通过前绝不删旧的（护栏 1）；宿主空间不够 → 零操作 + HOST-ENOSPC（#12）*/
static const img_arg A_IMGCLONE[] = {
    {"img",      IMG_A_STR,  true,  NULL,     "要克隆的映像路径（qcow2）", NULL},
    {"keep",     IMG_A_STR,  true,  NULL,     "要保留的分区起始 LBA，逗号分隔（不列的分区在新盘上留空）", NULL},
    {"guid",     IMG_A_ENUM, false, "random", "GUID：random=重新生成（默认）|keep=逐字节相同", NULL},
    {"keep_old", IMG_A_BOOL, false, "false",  "校验通过后是否保留旧映像副本（默认不保留）", NULL},
    {"old_name", IMG_A_STR,  false, NULL,     "旧映像改名后的名字（默认 <名字>.old.qcow2）", NULL},
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
    if(rename(src,oldp)!=0){ img_result_fail(out,IMG_EIO,"IMG-OPEN-FAIL",src,"旧映像改名失败（权限/占用）",NULL); return IMG_EIO; }
    img_clone_stats_t st;
    rc=img_clone(oldp, src, keep, nkeep, guid_random, &st, img_cmd_prog());
    if(rc!=IMG_OK){
        unlink(src); rename(oldp,src);              /* ★ 回滚：用户文件回到原路径 */
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":
                         (rc==IMG_EFULL)?"HOST-ENOSPC":"CLONE-COPY-FAIL";
        img_result_fail(out,rc,code,src,"克隆失败（已回滚：旧映像恢复原名）",NULL);
        return rc;
    }
    if(verify_clone(oldp,src,keep,nkeep)!=0){
        unlink(src); rename(oldp,src);              /* ★ 校验不过 → 回滚 */
        img_result_fail(out,IMG_EIO,"CLONE-VERIFY-FAIL",src,
                        "克隆后校验不通过（已回滚：旧映像恢复原名）","检查旧映像是否损坏（qemu-img check）");
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
    {"img",    IMG_A_STR, true,  NULL,   "映像路径（qcow2）", NULL},
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
    {"img", IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
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
    {"img", IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
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
    {"img",       IMG_A_STR, true,  NULL, "目标映像路径（qcow2，已建好布局）", NULL},
    {"src",       IMG_A_STR, true,  NULL, "源映像路径（只读打开，绝不修改）", NULL},
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
        if(!sfi){ img_result_fail(out,IMG_ENOENT,"IMG-OPEN-FAIL",src_path,"源映像无法打开（路径/权限）",NULL); return IMG_ENOENT; }
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
                        img_span_prep(&sp2,"part","copy","跨映像拷贝分区");
                        img_span_kv_u64(&sp2,"src_lba",slba);
                        img_span_kv_u64(&sp2,"dst_lba",dlba);
                        img_span_kv_u64(&sp2,"size_bytes",n);
                        img_span_begin(&sp2);
                        img_prog *pg=img_cmd_prog();
                        if(pg){ img_prog_phase(pg,IMG_PH_COPY,"跨映像拷贝分区");
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
    if(rc!=IMG_OK && out->ok) img_result_fail(out,rc,"PART-COPY-FAIL",NULL,"跨映像拷贝失败",NULL);
    return rc;
}

/* ================= 命令 6i：part.mbrboot（写 MBR 引导码 + 磁盘签名）====================
 * MBR 的前 440 字节是 BIOS 要执行的引导码；空码 = "Operating System not found"。
 * 引导码内容来自【参考件】（如 reference/mbr-code-bios.bin，从真装好 Windows 的 MBR 盘提取），
 * 与 BCD 参考件同一套"工具不硬编码二进制内容"的规矩。*/
static const img_arg A_MBRBOOT[] = {
    {"img",   IMG_A_STR, true,  NULL, "映像（MBR 分区表）", NULL},
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
    {"img",       IMG_A_STR,  true,  NULL,  "映像路径（qcow2，会被建/重写分区表）", NULL},
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
    enum { BUF_CAP = 98304 };
    char *buf=img_tbuf(IMG_TB_CMD_LIST,BUF_CAP);
    if(!buf) return tb_nomem(out,"cmd.list 文本缓冲");
    int n=img_cmd_list_json(buf,BUF_CAP);
    if(n<=0){ img_result_fail(out,IMG_EIO,"LIST-FAIL",NULL,"命令表导出失败",NULL); return IMG_EIO; }
    /* ★ 去掉结尾换行：这段 JSON 要【嵌进】外层信封，带换行会让"按行取 JSON"的
     *   调用方（测试/脚本）把一行切成两半。直接调 img_cmd_list_json 的 CLI 不受影响。*/
    while(n>0 && (buf[n-1]=='\n' || buf[n-1]=='\r')) buf[--n]=0;
    img_result_raw(out,"table",buf);
    return IMG_OK;
}

/* ================= 表 ================= */
/* boot.repair —— BIOS 引导一键修复（诊断→按需重写→复验） */
static const img_arg A_BOOTREPAIR[]={
    {"img", IMG_A_STR, true, NULL, "目标映像", NULL},
    {"sys-lba", IMG_A_U64, true, NULL, "系统分区起始 LBA", NULL},
    {"sr-lba", IMG_A_U64, false, NULL, "系统保留分区 LBA（缺省=活动分区）", NULL},
    {"sig", IMG_A_STR, false, NULL, "磁盘签名 HEX（缺省=保持现值）", NULL},
    {"mbr-code", IMG_A_STR, false, NULL, "MBR 引导码文件（≥440字节；缺省不动 MBR）", NULL},
    {"vbr-tpl", IMG_A_STR, false, NULL, "VBR 模板 映像:lba；不传=内置模板（写 builtin 也等同不传）", NULL},
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
    /* ★ REQ-48 §3：`--vbr-tpl=builtin` 以前直接 CMD-ARG-BAD（hint 只说"格式 镜像:lba"），
     *   结果 App 按"builtin"传 → BIOS 引导修复一直是失败的。现在显式接受
     *   builtin/default/内置 = "用内置模板"，语义与不传相同，但算作"用户表达了要修"。*/
    bool vtpl_builtin=false;
    if(vtpl && (!strcasecmp(vtpl,"builtin") || !strcasecmp(vtpl,"default") || !strcmp(vtpl,"内置"))){
        vtpl=NULL; vtpl_builtin=true;
    }
    /* ★ 格式错误要在【打开镜像之前】报：以前等解析到分区表才校验，传错了却先报别的错
     *   （用户看到 IMG-OPEN-FAIL/PART-READ-FAIL，根本猜不到是 vbr-tpl 写错）。*/
    if(vtpl && !strrchr(vtpl,':')){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","vbr-tpl",
            "格式 映像:lba。★ 不传这个参数就用内置模板（不要写 builtin；builtin/default/内置 也接受，等同不传）",
            "去掉 --vbr-tpl 即可用内置模板；只有要外部提取的 VBR 时才写 路径:lba");
        return IMG_EINVAL;
    }
    const char *bref =img_args_get(in,"bcd-ref");
    if(!imgp||!sys_lba){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","img/sys-lba","需要 --img 与 --sys-lba",NULL); return IMG_EINVAL; }
    /* ★ 同 img_ctx_open：boot.repair 自己开的镜像也必须"已存在"，不允许创建空文件 */
    img_io*f=img_io_open_existing(imgp,true);
    if(!f){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",imgp,"映像不存在或不是普通文件（本命令不会创建映像）",NULL); return IMG_ENOENT; }
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
    bool any_fix = (mcode!=NULL) || (vtpl!=NULL) || vtpl_builtin || (bref!=NULL);
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
        char *cl=strrchr(buf,':');
        if(!cl){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","vbr-tpl",
                "格式 映像:lba。★ 不传这个参数就用内置模板（不要写 builtin；builtin/default 也已经接受，等价于不传）",
                "去掉 --vbr-tpl 即可用内置模板；只有要外部提取的 VBR 时才写 路径:lba");
            return IMG_EINVAL;
        }
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
    {"out",     IMG_A_STR,  true,  NULL,   "要创建的映像路径（已存在时需 force=true）", NULL},
    {"format",  IMG_A_ENUM, false, "qcow2","磁盘格式：qcow2 | raw | vhd | vmdk", NULL},
    {"size-gb", IMG_A_U64,  true,  NULL,   "虚拟磁盘大小（GiB，1~1024）", NULL},
    {"table",   IMG_A_ENUM, false, "gpt",  "分区表：gpt(UEFI) | mbr(BIOS) | none(空盘，不写任何分区表)", NULL},
    {"layout",  IMG_A_ENUM, false, "windows", "分区布局：windows(ESP+MSR+Windows) | single(整盘一个分区) | empty(有表但无分区)", NULL},
    {"esp-mb",  IMG_A_U64,  false, "200",  "（gpt）ESP 大小 MiB，128~1024", NULL},
    {"msr-mb",  IMG_A_U64,  false, "16",   "（gpt）MSR 大小 MiB，官方固定 16", NULL},
    {"rec-mb",  IMG_A_U64,  false, "0",    "恢复分区 MiB（0=不建；要建就 512~2048）", NULL},
    {"sys-mb",  IMG_A_U64,  false, NULL,   "系统分区 MiB：mbr=活动系统分区（缺省 512，128~1024）；gpt=Windows 分区大小（缺省=占满剩余；要指定则 ≥128）", NULL},
    {"win-mb",  IMG_A_U64,  false, "0",    "（mbr）Windows 分区 MiB：0=占满剩余（默认）；指定时 ≥128（GPT 用 sys-mb）", NULL},
    {"extra-mb",IMG_A_STR,  false, NULL,   "追加数据分区：逗号分隔 MiB（如 4096,8192），按顺序排在 Windows/系统分区之后；每块 ≥16", NULL},
    {"limits",  IMG_A_ENUM, false, "strict","尺寸校验：strict=越界直接 PART-SIZE-BAD（默认）| off=不校验（只有测试/实验用）", NULL},
    {"mbr-fill",IMG_A_ENUM, false, "reserve","（mbr）主分区槽怎么用：reserve=占满 4 槽时整批进 0x0F 容器、留一个槽（默认）| primary=先把 4 槽用满，装不下报 PART-NOSPACE | auto=主分区只填到 3 个、剩下进容器（永远留一条后路）", NULL},
    {"force",   IMG_A_BOOL, false, "false","已存在则覆盖（★会删掉原文件）", NULL},
};

/* 尺寸越界统一出口（REQ-20261025-03）：
 *   错误码 PART-SIZE-BAD（不复用 CMD-ARG-BAD：前者是"值超范围"，后者是"参数名/格式错"），
 *   err.at=参数名；data 里带 role/min/max/unit/got —— App 拿它高亮输入框并显示真实范围，
 *   范围数字来自 img_constraints.c 的 ARG_LIMITS（同一份，前端不用再抄常量）。 */
static int size_bad_at(img_result *out, const char *arg, const img_arg_limit_t *L, uint64_t got){
    char why[220], sug[220];
    if(!L){
        snprintf(why,sizeof why,"%s 的值超出允许范围（收到 %llu）",arg,(unsigned long long)got);
        snprintf(sug,sizeof sug,"用 cmd.list 查该参数的范围");
    } else if(L->zero_ok && L->max){
        snprintf(why,sizeof why,"%s 超出允许范围：0（=不建/占满剩余）或 %llu~%llu %s，收到 %llu",
                 arg,(unsigned long long)L->min,(unsigned long long)L->max,L->unit?L->unit:"",
                 (unsigned long long)got);
        snprintf(sug,sizeof sug,"改成 0，或 %llu~%llu %s",(unsigned long long)L->min,
                 (unsigned long long)L->max,L->unit?L->unit:"");
    } else if(L->max){
        snprintf(why,sizeof why,"%s 超出允许范围：%llu~%llu %s，收到 %llu",
                 arg,(unsigned long long)L->min,(unsigned long long)L->max,L->unit?L->unit:"",
                 (unsigned long long)got);
        snprintf(sug,sizeof sug,"改成 %llu~%llu %s",(unsigned long long)L->min,
                 (unsigned long long)L->max,L->unit?L->unit:"");
    } else {
        snprintf(why,sizeof why,"%s 太小：至少 %llu %s，收到 %llu",
                 arg,(unsigned long long)L->min,L->unit?L->unit:"",(unsigned long long)got);
        snprintf(sug,sizeof sug,"改成 ≥%llu %s",(unsigned long long)L->min,L->unit?L->unit:"");
    }
    img_result_fail(out,IMG_ERANGE,"PART-SIZE-BAD",arg,why,sug);
    if(L){
        img_result_str(out,"role",L->role?L->role:"");
        img_result_u64(out,"min",L->min);
        img_result_u64(out,"max",L->max);
        img_result_str(out,"unit",L->unit?L->unit:"");
    }
    img_result_u64(out,"got",got);
    return IMG_ERANGE;
}

/* 严格解析一串逗号分隔的 MiB 值（extra-mb）。返回个数；失败返回 -1 并填 why。 */
static int parse_mb_list(const char *s, uint64_t *out, int max, uint64_t min_mb,
                         bool strict, char *why, size_t whycap){
    if(!s||!*s) return 0;
    int n=0;
    const char *p=s;
    while(*p){
        const char *q=p;
        while(*q && *q!=',') q++;
        /* 去首尾空格（命令行手打容易带空格） */
        const char *b=p; while(b<q && (*b==' '||*b=='\t')) b++;
        const char *e=q; while(e>b && (e[-1]==' '||e[-1]=='\t')) e--;
        if(b==e){
            if(why) snprintf(why,whycap,"额外分区列表里有空元素（逗号连着写或结尾多逗号）：\"%s\"",s);
            return -1;
        }
        char tmp[32]; size_t L=(size_t)(e-b);
        if(L>=sizeof tmp){
            if(why) snprintf(why,whycap,"额外分区数值太长：\"%.*s\"",(int)L,b);
            return -1;
        }
        memcpy(tmp,b,L); tmp[L]=0;
        char *endp=NULL; unsigned long long v=strtoull(tmp,&endp,10);
        if(!endp || *endp){
            if(why) snprintf(why,whycap,"额外分区不是数字：\"%s\"（要逗号分隔的 MiB 整数，如 4096,8192）",tmp);
            return -1;
        }
        if(strict && v < min_mb){
            if(why) snprintf(why,whycap,"额外分区太小：%llu MiB（每块至少 %llu MiB）",
                             (unsigned long long)v,(unsigned long long)min_mb);
            return -1;
        }
        if(n>=max){
            if(why) snprintf(why,whycap,"额外分区最多 %d 块",max);
            return -1;
        }
        out[n++]=(uint64_t)v;
        p = (*q==',') ? q+1 : q;
    }
    return n;
}

static int cmd_img_create(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *outp=img_args_get(in,"out");
    uint64_t gb=img_args_u64(in,"size-gb",0);
    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="gpt";
    if(!outp||!*outp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","out","缺少输出路径",NULL); return IMG_EINVAL; }
    if(!gb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","size-gb","必须是正整数（GiB）",NULL); return IMG_EINVAL; }
    if(gb>1024){ img_result_fail(out,IMG_ERANGE,"CMD-ARG-BAD","size-gb",
                    "盘太大：上限 1024 GiB（再大请分卷）","改成 1~1024 GiB"); return IMG_ERANGE; }
    bool gpt, mbr, no_tbl;
    gpt=!strcasecmp(tb,"gpt"); mbr=!strcasecmp(tb,"mbr"); no_tbl=!strcasecmp(tb,"none");
    if(!gpt && !mbr && !no_tbl){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","table","只接受 gpt | mbr | none",NULL); return IMG_EINVAL; }
    const char *lay=img_args_get(in,"layout"); if(!lay||!*lay) lay="windows";
    bool l_win=!strcasecmp(lay,"windows"), l_single=!strcasecmp(lay,"single"), l_empty=!strcasecmp(lay,"empty");
    if(!l_win && !l_single && !l_empty){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","layout","只接受 windows | single | empty",NULL); return IMG_EINVAL; }

    const uint64_t MiB=1024u*1024u, GiB=1024u*1024u*1024u;
    uint64_t bytes=gb*GiB;

    /* ★ 尺寸校验（REQ-20261025-03）：全部在【建文件之前】做完 ——
     *   越界要报 PART-SIZE-BAD，且不许留下半个镜像/半张表。 */
    const char *lv=img_args_get(in,"limits");
    bool strict=true;
    if(lv && *lv){
        if(!strcasecmp(lv,"off") || !strcasecmp(lv,"0") || !strcasecmp(lv,"false")) strict=false;
        else if(!strcasecmp(lv,"strict") || !strcasecmp(lv,"on") || !strcasecmp(lv,"1")
             || !strcasecmp(lv,"true")) strict=true;
        else { img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","limits","只接受 strict | off",NULL); return IMG_EINVAL; }
    }

    uint64_t esp_mb=img_args_u64(in,"esp-mb",200);
    uint64_t msr_mb=img_args_u64(in,"msr-mb",16);
    uint64_t rec_mb=img_args_u64(in,"rec-mb",0);
    const char *sys_s=img_args_get(in,"sys-mb");
    uint64_t sys_mb = (sys_s && *sys_s) ? img_args_u64(in,"sys-mb",0) : 0;
    bool sys_given = (sys_s && *sys_s);
    /* ★ MBR 的 Windows 分区大小（REQ-20261025-05）：0 = 吃满剩余（老行为）。 */
    uint64_t win_mb=img_args_u64(in,"win-mb",0);
    /* ★ MBR 主分区槽策略（REQ-20261025-07 / -08）：reserve（默认）/ primary / auto */
    const char *mf=img_args_get(in,"mbr-fill");
    int fill_mode=0;
    if(mf && *mf){
        if(!strcasecmp(mf,"reserve")) fill_mode=0;
        else if(!strcasecmp(mf,"primary")) fill_mode=1;
        else if(!strcasecmp(mf,"auto")) fill_mode=2;
        else{
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","mbr-fill",
                "只接受 reserve | primary | auto",
                "reserve=占满 4 槽时整批进容器；primary=先填满主槽；auto=主槽填到 3 个、剩下进容器");
            return IMG_EINVAL;
        }
    }
    if(win_mb && !(mbr && l_win)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","win-mb",
            "只有 table=mbr + layout=windows 用 win-mb",
            mbr? "layout=windows 才有 Windows 分区":"GPT 的 Windows 分区大小用 sys-mb");
        return IMG_EINVAL;
    }

    if(strict && l_win){
        const img_arg_limit_t *Le;
        if(gpt){
            Le=img_arg_limit_find("img.create","esp-mb");
            if(esp_mb<Le->min || esp_mb>Le->max) return size_bad_at(out,"esp-mb",Le,esp_mb);
            Le=img_arg_limit_find("img.create","msr-mb");
            if(msr_mb<Le->min || msr_mb>Le->max) return size_bad_at(out,"msr-mb",Le,msr_mb);
            Le=img_arg_limit_find("img.create","rec-mb");
            if(rec_mb!=0 && (rec_mb<Le->min || rec_mb>Le->max)) return size_bad_at(out,"rec-mb",Le,rec_mb);
            Le=img_arg_limit_find("img.create","sys-mb-gpt");
            if(sys_given && sys_mb!=0 && sys_mb<Le->min) return size_bad_at(out,"sys-mb",Le,sys_mb);
            if(sys_mb>Le->max && Le->max) return size_bad_at(out,"sys-mb",Le,sys_mb);
        } else {
            /* MBR：sys-mb = 活动系统分区（0/缺省 → 布局默认 512 MiB） */
            Le=img_arg_limit_find("img.create","rec-mb");
            if(rec_mb!=0 && (rec_mb<Le->min || rec_mb>Le->max)) return size_bad_at(out,"rec-mb",Le,rec_mb);
            Le=img_arg_limit_find("img.create","sys-mb");
            if(sys_given && sys_mb!=0 && (sys_mb<Le->min || sys_mb>Le->max))
                return size_bad_at(out,"sys-mb",Le,sys_mb);
        }
    }

    /* ★ 追加分区（extra-mb）*/
    uint64_t extra_mb[64]; int nextra=0;
    const char *extras=img_args_get(in,"extra-mb");
    if(extras && *extras){
        if(!l_win){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","extra-mb",
                "layout=single/empty 不支持额外分区（single 已经占满整盘）",
                "要额外分区请用 layout=windows；建完也可以用 part.add 加");
            return IMG_EINVAL;
        }
        char why[200]="";
        nextra=parse_mb_list(extras,extra_mb,64,0,false,why,sizeof why);   /* 这里只管格式 */
        if(nextra<0){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","extra-mb",why,
                        "逗号分隔的 MiB 整数，例如 4096,8192"); return IMG_EINVAL; }
        if(strict){
            /* 每块 ≥16 MiB → 走 PART-SIZE-BAD（值超范围，不是格式错） */
            const img_arg_limit_t *Lx=img_arg_limit_find("img.create","extra-mb");
            for(int i=0;i<nextra;i++)
                if(extra_mb[i] < Lx->min) return size_bad_at(out,"extra-mb",Lx,extra_mb[i]);
        }
        /* ★ sys-mb=0（Windows 吃满剩余）+ extra-mb 是矛盾的：明确报错，不静默丢分区。
         *   （REQ-01 §3.4 的 A 方案）*/
        if(gpt && nextra>0 && !sys_mb){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sys-mb",
                "给了 extra-mb 就必须给 Windows 分区大小（sys-mb），否则 Windows 会吃掉全部剩余空间",
                "把 sys-mb 设成 Windows 分区大小（≥128 MiB）");
            return IMG_EINVAL;
        }
    }

    /* ★ mbr-fill=primary（REQ-20261025-07）：主分区槽不够就直接失败 ——
     *   这时"再建容器"也要占一个槽，而槽已经没了，所以没有别的出路。
     *   提前判掉，比让布局生成器返回 IMG_EFULL 再猜是哪个参数更准。*/
    if(fill_mode==1 && mbr && l_win){
        int free_slots = 4 - (2 + (rec_mb?1:0));       /* System + Windows (+Recovery) */
        if(nextra > free_slots){
            img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","extra-mb",
                "mbr-fill=primary 要求额外分区都放进主分区槽，但槽不够了",
                free_slots>0 ? "改用 mbr-fill=reserve（会建 0x0F 容器），或减少额外分区"
                             : "恢复分区已经占满了槽：去掉 rec-mb，或改用 mbr-fill=reserve");
            img_result_u64(out,"avail_primary_slots",free_slots>0?(uint64_t)free_slots:0);
            return IMG_ERANGE;
        }
    }

    /* ★ MBR Windows 分区大小（win-mb）：口径与容量都在建文件之前查完。
     *   "可用" = 盘 - 系统保留 - 恢复 - Σ额外分区 - 扩展容器的 EBR 空隙 - 对齐余量。 */
    uint64_t avail_for_win=0;
    if(win_mb){
        uint64_t sys_used = sys_mb ? sys_mb : 512;      /* MBR 缺省 512 MiB 系统保留 */
        uint64_t extra_sum=0;
        for(int i=0;i<nextra;i++) extra_sum += extra_mb[i];
        int prim = 2 + (rec_mb ? 1 : 0);
        if(prim + nextra >= 4) extra_sum += (uint64_t)nextra;   /* 每块逻辑分区 1 MiB EBR */
        uint64_t fixed_mb = sys_used + rec_mb + extra_sum;
        avail_for_win = gb*1024 > fixed_mb + 2 ? gb*1024 - fixed_mb - 2 : 0;
        if(strict){
            /* 下限从表里取（别写死数字 —— REQ-12 就是改这个值） */
            const img_arg_limit_t *Lmin=img_arg_limit_find("img.create","win-mb");
            if(win_mb < Lmin->min){
                img_arg_limit_t Lw=*Lmin;
                Lw.max=avail_for_win;            /* data.max = 当前可用（App 直接显示） */
                return size_bad_at(out,"win-mb",&Lw,win_mb);
            }
        }
        if(win_mb > avail_for_win){
            img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","win-mb",
                "Windows 分区放不下：系统保留 + Windows + 恢复 + 额外分区 > 盘容量",
                "把 win-mb 调小，或把盘建大一点，或减少额外分区");
            img_result_u64(out,"avail_mb",avail_for_win);
            return IMG_ERANGE;
        }
    }

    bool force=img_args_bool(in,"force",false);
    struct stat st;
    if(!force && stat(outp,&st)==0){
        img_result_fail(out,IMG_EEXISTS,"CMD-ARG-BAD","out","输出文件已存在（force=true 才覆盖）",NULL);
        return IMG_EEXISTS;
    }

    /* ① 建 qcow2 本体（稀疏：只写头，不占实际空间）*/
    img_io_opts_t io={0}; io.writable=true; io.truncate=true;
    img_io *f=img_io_open_path(outp,&io);
    if(!f){ img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",outp,"创建文件失败（权限/目录不存在）",NULL); return IMG_EIO; }
    /* ★ P3：按 --format 选创建方式。
     *   raw  = truncate 到目标大小（全 0）
     *   vhd  = 先按 raw 铺满，再包装成动态 VHD（footer + cxsparse 头 + BAT）
     *   vmdk = sparse 头 + 描述符 + GD/GT
     *   其它（默认）= qcow2 */
    const char *fmt_s = img_args_get(in,"format"); if(!fmt_s||!*fmt_s) fmt_s="qcow2";
    int rc = IMG_OK;
    if(!strcasecmp(fmt_s,"raw")){
        rc = img_io_truncate(f, bytes);
        if(rc != IMG_OK){ img_io_close(f); unlink(outp);
            img_result_fail(out,rc,"IMG-FORMAT",outp,"raw 创建失败",NULL); return rc; }
    } else if(!strcasecmp(fmt_s,"vhd") || !strcasecmp(fmt_s,"vpc")){
        rc = img_vhd_create(f, bytes);
        if(rc != IMG_OK){ img_io_close(f); unlink(outp);
            img_result_fail(out,rc,"IMG-FORMAT",outp,"VHD 创建失败",NULL); return rc; }
    } else if(!strcasecmp(fmt_s,"vmdk")){
        rc = img_vmdk_create(f, bytes);
        if(rc != IMG_OK){ img_io_close(f); unlink(outp);
            img_result_fail(out,rc,"IMG-FORMAT",outp,"VMDK 创建失败",NULL); return rc; }
    } else {
        img_qcow2_create_opts_t co={0}; co.size=bytes; co.cluster_bits=16;
        rc = img_qcow2_create(f,&co);
        if(rc!=IMG_OK){ img_io_close(f); unlink(outp);
            img_result_fail(out,rc,"IMG-FORMAT",outp,"qcow2 创建失败",NULL); return rc; }
    }

    /* ② 分区表（复用 img_layout_build，与 imglayout mk 同一份实现）*/
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK){ img_io_close(f); unlink(outp);
        img_result_fail(out,IMG_EFORMAT,"IMG-FORMAT",outp,"qcow2 头无法写入去",NULL); return IMG_EFORMAT; }
    /* ★ table=none：空盘。只留容器头，一个分区表都不写，
     *   之后可以用 part.layout / 外部工具自己分区。*/
    if(no_tbl){
        uint64_t empty_bytes = img_qcow2_size(q);
        img_qcow2_close(q); img_io_close(f);
        img_result_str(out,"format",fmt_s);
        img_result_u64(out,"size_bytes",empty_bytes ? empty_bytes : bytes);
        img_result_str(out,"table","none");
        img_result_u64(out,"parts_total",0);
        return IMG_OK;
    }

    img_parttable_t t; img_check_result_t err; memset(&t,0,sizeof t); memset(&err,0,sizeof err);
    /* ★ 用容器**实际**容量排分区，不用请求值。
     *   VHD 会把容量向上补齐到整柱面（与 qemu 一致），按请求值排版会让
     *   GPT 备份表落在盘中间而不是盘尾，固件按盘尾去找就找不到。 */
    uint64_t real_bytes = img_qcow2_size(q);
    uint64_t disk_sectors = (real_bytes ? real_bytes : bytes)/512u;
    uint64_t extra_bytes[64];
    for(int i=0;i<nextra;i++) extra_bytes[i]=extra_mb[i]*MiB;

    if(l_win){
        img_layout_opts_t lo; img_layout_defaults(&lo, gpt?IMG_TABLE_GPT:IMG_TABLE_MBR);
        /* ★ sys_bytes 的语义【按表不同】（img_layout_build 里写死的）：
         *   MBR = 活动系统分区大小；GPT = Windows 分区大小，0 表示吃满剩余。
         *   默认值交给 img_layout_defaults()，这里只在用户显式给了才覆盖。*/
        lo.esp_bytes      = esp_mb*MiB;
        lo.msr_bytes      = msr_mb*MiB;
        lo.recovery_bytes = rec_mb*MiB;
        if(sys_mb) lo.sys_bytes = sys_mb*MiB;
        if(win_mb) lo.win_bytes = win_mb*MiB;   /* MBR 专用：0 = 吃满剩余 */
        if(fill_mode) lo.mbr_fill = fill_mode;
        if(nextra>0){ lo.extra_bytes=extra_bytes; lo.extra_count=nextra; }
        rc=img_layout_build(&t, disk_sectors, gpt?IMG_TABLE_GPT:IMG_TABLE_MBR, &lo, &err);
        if(rc!=IMG_OK){
            img_qcow2_close(q); img_io_close(f); unlink(outp);
            /* 放不下：定位到最可能的那个参数（Windows 大小 → 追加分区 → 盘容量） */
            if(rc==IMG_EFULL && win_mb){
                img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","win-mb",
                    "Windows 分区放不下：系统保留 + Windows + 恢复 + 额外分区 > 盘容量",
                    "把 win-mb 调小，或把盘建大一点，或减少额外分区");
                img_result_u64(out,"avail_mb",avail_for_win);
                return IMG_ERANGE;
            }
            if(rc==IMG_EFULL && nextra>0){
                char why[200];
                uint64_t fixed_mb = (gpt? (esp_mb+msr_mb+rec_mb+sys_mb) : (rec_mb+sys_mb? sys_mb:512));
                uint64_t avail = gb*1024 > fixed_mb+2 ? gb*1024-fixed_mb-2 : 0;
                snprintf(why,sizeof why,"追加分区放不下：盘 %llu GiB，必要分区已占约 %llu MiB，剩余约 %llu MiB",
                         (unsigned long long)gb,(unsigned long long)fixed_mb,(unsigned long long)avail);
                img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","extra-mb",why,
                                "把 Windows 分区或追加分区调小，或把盘建大一点");
                img_result_u64(out,"avail_mb",avail);
                return IMG_ERANGE;
            }
            if(rc==IMG_EFULL){
                if(sys_given && gpt){
                    img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","sys-mb",
                        "Windows 分区 + ESP/MSR/恢复/追加分区放不下这张盘",
                        "把 sys-mb 调小，或把盘建大一点");
                    return IMG_ERANGE;
                }
                img_result_fail(out,IMG_ERANGE,"CMD-ARG-BAD","size-gb",
                    err.reason[0]?err.reason:"盘太小，放不下这个布局",
                    "把盘建大一点（Windows 布局至少要 16 GiB + ESP/MSR）");
                return IMG_ERANGE;
            }
            img_result_fail(out,rc,"PART-WRITE-FAIL",outp,err.reason[0]?err.reason:"布局生成失败",
                            err.suggestion[0]?err.suggestion:NULL);
            return rc;
        }
    } else {
        /* single / empty：自己搭表头，不走 Windows 专用布局。
         * 这是给 Linux / 数据盘 / 手动分区留的口子。*/
        t.table           = gpt?IMG_TABLE_GPT:IMG_TABLE_MBR;
        t.disk_sectors    = disk_sectors;
        t.disk_signature  = 0x4ccd932b;
        img_guid_new(t.disk_guid);
        t.part_entry_count=128; t.part_entry_size=128;
        if(l_single){
            const uint64_t first=2048, tail=2048;   /* 尾部留 1 MiB 给 GPT 备份表 */
            if(disk_sectors <= first+tail+2048){
                img_qcow2_close(q); img_io_close(f); unlink(outp);
                img_result_fail(out,IMG_ERANGE,"CMD-ARG-BAD","size-gb","盘太小，放不下一个分区",NULL);
                return IMG_ERANGE;
            }
            uint64_t sectors = disk_sectors - first - tail;
            int idx=0;
            rc = gpt
               ? img_partition_add(&t, first, sectors, IMG_MBR_EMPTY, IMG_GUID_BASIC, "Data", false, &idx)
               : img_partition_add(&t, first, sectors, IMG_MBR_NTFS_EXFAT, NULL, "Data", false, &idx);
            if(rc!=IMG_OK){
                img_qcow2_close(q); img_io_close(f); unlink(outp);
                img_result_fail(out,rc,"PART-WRITE-FAIL",outp,"单分区建不了",NULL);
                return rc;
            }
        }
    }
    rc=img_parttable_write(q,&t);
    if(rc!=IMG_OK){ img_qcow2_close(q); img_io_close(f); unlink(outp);
        img_result_fail(out,rc,"PART-WRITE-FAIL",outp,"分区表无法写入去",NULL); return rc; }
    img_qcow2_close(q); img_io_close(f);

    /* ③ 结果：把新表原样回给 App（省一次 part.show）*/
    static char pj[64*1024];
    int pn=img_parttable_json(&t,pj,sizeof pj);
    uint64_t used=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(t.parts[i].used) used++;
    /* size_bytes 报【容器实际容量】：VHD 会把容量向上补齐到整柱面，
     *   报请求值会让 App 的空间预检少算一截。*/
    img_result_str(out,"format",fmt_s);
    img_result_u64(out,"size_bytes",real_bytes ? real_bytes : bytes);
    img_result_str(out,"table",gpt?"GPT":"MBR");
    img_result_u64(out,"parts_total",used);
    if(nextra>0){
        img_result_u64(out,"extra_count",(uint64_t)nextra);
        img_result_str(out,"layout","windows+extra");
    }
    if(pn>0) img_result_raw(out,"pt",pj);
    return IMG_OK;
}

/* ---------- img.compact（2026-10-25，REQ-20261025-06） ----------
 * 另存为紧凑镜像：源【只读】、目标新建、只写非零块（qcow2 里未分配簇不写 → 体积变小）。
 * 与 img.clone 的区别：img.clone 是【原地换盘】（原名→.old→写回原路径→删 .old），
 * 这条【绝不碰源文件】—— 写完就落在 out，替换不替换由用户决定。
 * ★ GUID 默认 keep：BCD 里记着分区 GUID，重新生成会让拷过去的系统起不来。*/
static int compact_src_open(const char *src, img_parttable_t *t, uint64_t *vsize, img_result *out){
    img_io *f=img_io_open_existing(src,false);
    if(!f){
        img_result_fail(out,IMG_ENOENT,"IMG-OPEN-FAIL",src,
            "源映像无法打开（不存在 / 不是普通文件 / 没权限）",NULL);
        return IMG_ENOENT;
    }
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q){
        img_io_close(f);
        img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",src,"不是合法 qcow2（qcow2 头读不出）",NULL);
        return IMG_EFORMAT;
    }
    int rc=img_parttable_read(q,t,0);
    if(vsize) *vsize=img_qcow2_size(q);
    img_qcow2_close(q); img_io_close(f);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PART-READ-FAIL",src,
            "源映像没有可读的分区表（空盘？）","空盘用 cp 复制即可，compact 只对分区盘有意义");
        return rc;
    }
    return IMG_OK;
}

/* 抽样校验：虚拟大小 + 分区表 JSON（逐字节）+ 每个保留分区 5 处 64 KiB 块 */
static int verify_compact(const char *src, const char *dst,
                          const uint64_t *keep, int nkeep, char *why, size_t whycap){
    if(why&&whycap) why[0]=0;
    img_io *a=img_io_open_existing(src,false), *b=img_io_open_existing(dst,false);
    if(!a||!b){
        if(a) img_io_close(a);
        if(b) img_io_close(b);
        if(why) snprintf(why,whycap,"无法打开源/结果映像");
        return -1;
    }
    img_qcow2_opts_t qo={0};
    img_qcow2 *qa=img_qcow2_open(a,&qo), *qb=img_qcow2_open(b,&qo);
    if(!qa||!qb){
        if(qa) img_qcow2_close(qa); else img_io_close(a);
        if(qb) img_qcow2_close(qb); else img_io_close(b);
        if(why) snprintf(why,whycap,"结果映像无法打开");
        return -1;
    }
    int bad=0;
    if(img_qcow2_size(qa)!=img_qcow2_size(qb)){
        bad=1; if(why)snprintf(why,whycap,"虚拟大小不一致（%llu vs %llu）",
            (unsigned long long)img_qcow2_size(qa),(unsigned long long)img_qcow2_size(qb));
    }
    img_parttable_t ta,tb;
    if(!bad){
        if(img_parttable_read(qa,&ta,0)!=IMG_OK || img_parttable_read(qb,&tb,0)!=IMG_OK){
            bad=1; if(why)snprintf(why,whycap,"结果映像的分区表读不出");
        } else {
            static char ja[16*1024], jb[16*1024];
            int na=img_parttable_json(&ta,ja,sizeof ja), nb=img_parttable_json(&tb,jb,sizeof jb);
            if(na<=0||nb<=0||na!=nb||memcmp(ja,jb,(size_t)na)!=0){
                bad=1; if(why)snprintf(why,whycap,"分区表与源不一致");
            }
        }
    }
    if(!bad){
        enum { VC = 64*1024 };
        static unsigned char v1[VC], v2[VC];
        for(int k=0;k<nkeep && !bad;k++){
            const img_partition_t *pa=NULL;
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(ta.parts[i].used && ta.parts[i].first_lba==keep[k]){ pa=&ta.parts[i]; break; }
            if(!pa){ bad=1; if(why)snprintf(why,whycap,"保留分区 LBA %llu 不在源表里",
                                            (unsigned long long)keep[k]); break; }
            uint64_t poff=keep[k]*512ull;
            uint64_t bytes=(pa->last_lba - pa->first_lba + 1)*512ull;
            size_t n=(size_t)(bytes<(uint64_t)VC?bytes:(uint64_t)VC);
            for(int s=0;s<5 && !bad;s++){
                uint64_t at = (bytes>n) ? (bytes-n)*(uint64_t)s/4u : 0;
                if(img_qcow2_read_full(qa,poff+at,v1,n)!=IMG_OK ||
                   img_qcow2_read_full(qb,poff+at,v2,n)!=IMG_OK ||
                   memcmp(v1,v2,n)!=0){
                    bad=1;
                    if(why)snprintf(why,whycap,"分区 LBA %llu 的第 %d 个抽样块不一致",
                                    (unsigned long long)keep[k],s);
                }
            }
        }
    }
    img_qcow2_close(qa); img_io_close(a);
    img_qcow2_close(qb); img_io_close(b);
    return bad?-1:0;
}

static const img_arg A_IMGCOMPACT[] = {
    {"src",    IMG_A_STR,  true,  NULL,    "源映像（只读打开，全程不改）", NULL},
    {"out",    IMG_A_STR,  true,  NULL,    "输出的新 qcow2 路径（绝不会覆盖源）", NULL},
    {"keep",   IMG_A_STR,  false, NULL,    "只保留这些分区的数据（起始 LBA，逗号分隔）；空 = 全部保留", NULL},
    {"guid",   IMG_A_ENUM, false, "keep",  "GUID：keep=原样保留（默认，BCD 里记的分区 GUID 还认得）| random=重新生成", NULL},
    {"verify", IMG_A_BOOL, false, "true",  "写完回读校验（虚拟大小 + 分区表 + 每个分区 5 处 64 KiB 抽样）", NULL},
    {"force",  IMG_A_BOOL, false, "false", "out 已存在时覆盖（★会先 unlink 那个文件）", NULL},
};
static int cmd_img_compact(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *src=img_args_get(in,"src");
    const char *dst=img_args_get(in,"out");
    if(!src||!*src){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","缺少源映像路径",NULL); return IMG_EINVAL; }
    if(!dst||!*dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","out","缺少输出路径",NULL); return IMG_EINVAL; }
    if(!strcmp(src,dst)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","out","输出不能和源是同一个文件","换一个输出名");
        return IMG_EINVAL;
    }
    bool force=img_args_bool(in,"force",false);
    bool verify=img_args_bool(in,"verify",true);
    const char *gp=img_args_get(in,"guid");
    bool guid_random=false;
    if(gp && *gp){
        if(!strcasecmp(gp,"random")) guid_random=true;
        else if(strcasecmp(gp,"keep")){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","guid","只接受 keep | random",NULL);
            return IMG_EINVAL;
        }
    }
    struct stat dstat;
    if(stat(dst,&dstat)==0 && !force){
        img_result_fail(out,IMG_EEXISTS,"CMD-ARG-BAD","out",
            "输出文件已存在（force=true 才覆盖）","换个名字，或先删掉它");
        return IMG_EEXISTS;
    }
    img_parttable_t st; uint64_t vsize=0;
    int rc=compact_src_open(src,&st,&vsize,out);
    if(rc!=IMG_OK) return rc;

    /* 保留名单：默认 = 源盘全部分区 */
    static uint64_t keep[64]; int nkeep=0;
    const char *kl=img_args_get(in,"keep");
    if(kl && *kl){
        static char kbuf[512]; snprintf(kbuf,sizeof kbuf,"%s",kl);
        for(char *tok=strtok(kbuf,","); tok && nkeep<64; tok=strtok(NULL,",")){
            while(*tok==' ') tok++;
            if(!*tok) continue;
            char *end=NULL; unsigned long long v=strtoull(tok,&end,0);
            if(!end||*end||!v){
                img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","keep",
                    "保留分区名单里必须是纯数字 LBA（逗号分隔）",NULL);
                return IMG_EINVAL;
            }
            keep[nkeep++]=(uint64_t)v;
        }
        if(!nkeep){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","keep","名单解析后为空",NULL); return IMG_EINVAL; }
    } else {
        for(int i=0;i<IMG_MAX_PARTITIONS && nkeep<64;i++)
            if(st.parts[i].used) keep[nkeep++]=st.parts[i].first_lba;
    }
    if(!nkeep){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src",
            "源映像没有分区，没什么可拷的","空盘直接 cp/复制文件即可");
        return IMG_EINVAL;
    }
    uint64_t need=4u*1024*1024;
    for(int k=0;k<nkeep;k++){
        bool found=false;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(st.parts[i].used && st.parts[i].first_lba==keep[k]){
                need += (st.parts[i].last_lba - st.parts[i].first_lba + 1)*512u;
                found=true; break;
            }
        if(!found){
            char atbuf[48]; snprintf(atbuf,sizeof atbuf,"keep=%llu",(unsigned long long)keep[k]);
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD",atbuf,"这个 LBA 不在源分区表里",NULL);
            return IMG_EINVAL;
        }
    }
    need += need/32 + 4u*1024*1024;                 /* 约 +3% 元数据 + 4 MiB 余量 */
    {   char dir[600]; snprintf(dir,sizeof dir,"%s",dst);
        char *sl=strrchr(dir,'/');
        if(sl) *sl=0;
        else snprintf(dir,sizeof dir,".");
        uint64_t free_b=img_host_free_bytes_path(dir);
        if(free_b && free_b < need){
            img_result_fail(out,IMG_EFULL,"HOST-ENOSPC",dst,
                "输出目录剩余空间不够（未写入任何字节）","清理空间后重试，或减少保留分区");
            return IMG_EFULL;
        }
    }

    if(force) unlink(dst);
    img_clone_stats_t cs; memset(&cs,0,sizeof cs);
    rc=img_clone(src,dst,keep,nkeep,guid_random,&cs,img_cmd_prog());
    if(rc!=IMG_OK){
        unlink(dst);                                /* ★ 只删半成品，源一个字节没动 */
        const char *code=(rc==IMG_ECANCELLED)?"CMD-CANCELLED":
                         (rc==IMG_EFULL)?"HOST-ENOSPC":"CLONE-COPY-FAIL";
        img_result_fail(out,rc,code,dst,"紧凑另存失败（只删了半成品，源映像没动）",NULL);
        return rc;
    }
    if(verify){
        char why[220]="";
        if(verify_compact(src,dst,keep,nkeep,why,sizeof why)!=0){
            unlink(dst);
            img_result_fail(out,IMG_EIO,"CLONE-VERIFY-FAIL",dst,
                why[0]?why:"抽样校验不一致","结果已删除；源映像没动，可重试或先 qc.verify 源盘");
            return IMG_EIO;
        }
    }
    struct stat a2,b2; memset(&a2,0,sizeof a2); memset(&b2,0,sizeof b2);
    stat(src,&a2); stat(dst,&b2);
    char guid_out[64]; snprintf(guid_out,sizeof guid_out,"%s",st.disk_guid);
    if(guid_random){                                /* 重新生成过 → 从结果里读真值 */
        img_parttable_t ot; uint64_t v2=0;
        if(compact_src_open(dst,&ot,&v2,out)==IMG_OK) snprintf(guid_out,sizeof guid_out,"%s",ot.disk_guid);
    }
    img_result_str(out,"out",dst);
    img_result_u64(out,"bytes_before",(uint64_t)a2.st_size);
    img_result_u64(out,"bytes_after",(uint64_t)b2.st_size);
    img_result_u64(out,"bytes_written",cs.bytes_written);
    img_result_u64(out,"parts_copied",(uint64_t)cs.parts_copied);
    img_result_u64(out,"size_bytes",vsize);
    img_result_str(out,"disk_guid",guid_out);
    img_result_bool(out,"verified",verify);
    img_result_u64(out,"keep_total",(uint64_t)nkeep);
    return IMG_OK;
}

/* ---------- 卷标校验/归一化（唯一一套：fs.format 与 ntfs.label/fat.label 共用）----------
 * REQ-20261026-11：同一个"写卷标"的动作必须走同样的规则 ——
 *   NTFS：UTF-8 ≤32 字符（卷标本身是 UTF-16，中文没问题）；
 *   FAT32：1~11 字符，A-Z 0-9 空格 _ -（小写自动转大写），中文直接拒绝。
 * allow_empty=true 时（fs.format）空/不传 = 不写卷标（FAT 落成 "NO NAME"）。*/
static int label_check(const char *lab, bool ntfs, bool allow_empty,
                       char *norm, size_t cap, img_result *out){
    if(norm && cap) norm[0]=0;
    if(!lab || !*lab){
        if(allow_empty) return IMG_OK;
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","label",
            "卷标不能为空","至少给一个字符");
        return IMG_EINVAL;
    }
    if(ntfs){
        int n=0;
        for(const unsigned char *p=(const unsigned char*)lab; *p; p++)
            if((*p & 0xC0)!=0x80) n++;                 /* UTF-8 按字符数数 */
        if(n>32){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","label",
                "NTFS 卷标最多 32 个字符","去掉几个字再试");
            return IMG_EINVAL;
        }
        snprintf(norm,cap,"%s",lab);
        return IMG_OK;
    }
    size_t L=strlen(lab);
    if(L>11){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","label",
            "FAT32 卷标最多 11 个字符","短一点，例如 SYSTEM；中文请用 NTFS");
        return IMG_EINVAL;
    }
    for(size_t i=0;i<L;i++){
        char c=(char)toupper((unsigned char)lab[i]);
        if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==' '||c=='_'||c=='-')){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","label",
                "FAT32 卷标只能用 A-Z 0-9 空格 _ -（FAT 目录项是 11 字节 ASCII，中文写不进去）",
                "换成 ASCII；中文卷标请把分区做成 NTFS");
            return IMG_EINVAL;
        }
        norm[i]=c;
    }
    norm[L]=0;
    return IMG_OK;
}

/* ---------- ntfs.label / fat.label（2026-10-25）----------
 * 只改卷标字段：NTFS 动 $Volume 的 AT_VOLUME_NAME；FAT32 动 BPB 0x47 + 根目录那条 0x08。
 * 不重建文件系统（fs.format 会清空整卷）、不动任何文件数据。*/
static const img_arg A_FSLABEL[] = {
    {"img",   IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
    {"lba",   IMG_A_U64, true, NULL, "分区起始 LBA（part.show 里的 first）", chk_lba},
    {"label", IMG_A_STR, false, NULL, "新卷标：NTFS ≤32 字符（空=清掉卷标）；FAT32 1~11（A-Z 0-9 空格 _ -，小写自动转大写）", NULL},
};
static int cmd_fs_label(img_ctx *ctx, const img_args *in, img_result *out, bool ntfs){
    /* 校验/归一化走 label_check —— 和 fs.format --label 同一套（REQ-20261026-11）
     * ★ NTFS 允许传空（= 清掉卷标，REPLY-06 的语义）；FAT32 空卷标不合法。 */
    char norm[80];
    int lrc=label_check(img_args_get(in,"label"), ntfs, ntfs, norm, sizeof norm, out);
    if(lrc!=IMG_OK) return lrc;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t){
        img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL);
        return rc?rc:IMG_ESTATE;
    }
    uint64_t lba=img_args_u64(in,"lba",0), pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512u; break;
        }
    if(!pb){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
            "分区不存在或 LBA 不在分区表里","先用 part.show 看 first 字段");
        return IMG_EINVAL;
    }
    if(ntfs){
        img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
        if(!fs){
            img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",img_ctx_path(ctx),
                "这个分区不是 NTFS（未格式化 / LBA 不对）","用 part.scan 看它到底是什么文件系统");
            return IMG_ESTATE;
        }
        rc=img_ntfs_set_label(fs,norm);
        img_ntfs_umount(fs);                      /* 卸载才会把 MFT 记录落盘 */
        if(rc!=IMG_OK){
            img_result_fail(out,rc,"LABEL-WRITE-FAIL",img_ctx_path(ctx),
                "写 NTFS 卷标失败（$Volume 记录写不进去）","qc.verify 自检；必要时先备份再动");
            return rc;
        }
        img_result_str(out,"fs","NTFS");
    } else {
        img_fat32_fs *fs=img_fat32_mount(q,lba,true);
        if(!fs){
            img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",img_ctx_path(ctx),
                "这个分区不是 FAT32（未格式化 / LBA 不对）","用 part.scan 看它到底是什么文件系统");
            return IMG_ESTATE;
        }
        rc=img_fat32_set_label(fs,norm);
        img_fat32_unmount(fs);
        if(rc!=IMG_OK){
            img_result_fail(out,rc,"LABEL-WRITE-FAIL",img_ctx_path(ctx),
                "写 FAT32 卷标失败（根目录没有空槽 / 写盘失败）",NULL);
            return rc;
        }
        img_result_str(out,"fs","FAT32");
    }
    img_result_str(out,"label",norm);
    img_result_u64(out,"lba",lba);
    img_result_u64(out,"bytes",pb);
    return IMG_OK;
}
static int cmd_ntfs_label(img_ctx *ctx, const img_args *in, img_result *out){
    return cmd_fs_label(ctx,in,out,true);
}
static int cmd_fat_label(img_ctx *ctx, const img_args *in, img_result *out){
    return cmd_fs_label(ctx,in,out,false);
}
/* ---------- part.export / part.apply（REQ-20261026-14）----------
 * "表就是表"：只有 LBA/大小/类型 GUID/分区 GUID/名字/属性/活动位，不含任何文件数据。
 * 导出 → 应用 到同尺寸的空盘，两块表必须逐字段等价（GUID 也保住 —— BCD 记着它）。*/
static uint64_t used_count(const img_parttable_t *t);   /* 定义在 part.add 那一段 */
static const img_arg A_PTEXPORT[] = {
    {"img", IMG_A_STR, true,  NULL, "映像路径（只读打开，不改任何东西）", NULL},
    {"out", IMG_A_STR, false, NULL, "把表写成 JSON 到这个路径（不给就只在 data.pt 里回）", NULL},
    {"raw", IMG_A_STR, false, NULL, "额外把原始表区写成二进制（MBR=LBA0 512B；GPT=LBA1..33）", NULL},
};
static int cmd_part_export(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t){
        img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),
            "这张盘读不出分区表（空盘？）","空盘没有表可导出；先 part.apply 或 imglayout 写一张");
        return rc?rc:IMG_ESTATE;
    }
    static char js[64*1024];
    int n=img_ptfile_dump(t,t->mbr_code,js,sizeof js);
    if(n<=0){
        img_result_fail(out,IMG_EIO,"PT-FILE-BAD",img_ctx_path(ctx),
            "表太大，渲染不下（分区太多？）","减少分区后再导出");
        return IMG_EIO;
    }
    const char *outp=img_args_get(in,"out");
    if(outp && *outp){
        FILE *f=fopen(outp,"wb");
        if(!f){
            img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",outp,
                "写不了这个文件（目录不存在/没权限）","换一个可写的目录");
            return IMG_EIO;
        }
        size_t w=fwrite(js,1,(size_t)n,f);
        w += fwrite("\n",1,1,f);              /* 文件结尾给个换行，编辑起来舒服 */
        if(fclose(f)!=0 || w!=(size_t)n+1){
            unlink(outp);
            img_result_fail(out,IMG_EIO,"HOST-WRITE-FAIL",outp,"写文件失败（宿主满？）",NULL);
            return IMG_EIO;
        }
        img_result_str(out,"out",outp);
        img_result_u64(out,"size_bytes",(uint64_t)n);
    }
    const char *rawp=img_args_get(in,"raw");
    if(rawp && *rawp){
        /* MBR：LBA0 512B；GPT：LBA1..33（保护 MBR 不在内，按 REQ 的说法）*/
        uint64_t off = (t->table==IMG_TABLE_GPT) ? 512ull : 0ull;
        size_t   len = (t->table==IMG_TABLE_GPT) ? (33u*512u) : 512u;
        unsigned char *buf=(unsigned char*)malloc(len);
        if(!buf) return IMG_ENOMEM;
        rc=img_qcow2_read_full(q,off,buf,len);
        if(rc!=IMG_OK){ free(buf);
            img_result_fail(out,rc,"IMG-READ-FAIL",img_ctx_path(ctx),"读表区失败",NULL);
            return rc; }
        FILE *f=fopen(rawp,"wb");
        if(!f){ free(buf);
            img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",rawp,"写不了这个文件",NULL);
            return IMG_EIO; }
        size_t w=fwrite(buf,1,len,f);
        int cl=fclose(f);
        free(buf);
        if(w!=len || cl!=0){
            unlink(rawp);
            img_result_fail(out,IMG_EIO,"HOST-WRITE-FAIL",rawp,"写文件失败",NULL);
            return IMG_EIO;
        }
        img_result_str(out,"raw",rawp);
        img_result_u64(out,"raw_bytes",(uint64_t)len);
    }
    /* pt = 规范文件 JSON 原样回（它的 parts[] 字段名与 part.show 一致，App 一套解析就够）*/
    img_result_str(out,"table", t->table==IMG_TABLE_GPT?"GPT":(t->table==IMG_TABLE_MBR?"MBR":"NONE"));
    img_result_u64(out,"parts_total",used_count(t));
    img_result_bool(out,"hasBootCode", img_parttable_has_bootcode(t));
    img_result_raw(out,"pt",js);
    return IMG_OK;
}

static const img_arg A_PTAPPLY[] = {
    {"img",   IMG_A_STR,  true,  NULL,    "目标映像（可以还没有分区表：img.create --table=none 建的即可）", NULL},
    {"in",    IMG_A_STR,  true,  NULL,    "分区表文件（.json，kind=icdq.parttable）", NULL},
    {"force", IMG_A_BOOL, false, "false", "目标已有分区时是否覆盖（默认 false → PT-NOT-EMPTY）", NULL},
};
static int cmd_part_apply(img_ctx *ctx, const img_args *in, img_result *out){
    const char *path=img_args_get(in,"in");
    if(!path||!*path){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","in","缺少分区表文件路径",NULL);
        return IMG_EINVAL;
    }
    img_ptfile_t pf; memset(&pf,0,sizeof pf);
    char code[40]="", at[64]="", why[220]="";
    int rc=img_ptfile_load(path,&pf,code,sizeof code,at,sizeof at,why,sizeof why);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,code[0]?code:"PT-FILE-BAD",at[0]?at:path,
            why[0]?why:"分区表文件读不出来",
            "确认是 part.export 导出的那种 .json（kind=icdq.parttable）");
        return rc;
    }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    uint64_t disk=img_qcow2_size(q)/512u;
    if(!disk){
        img_result_fail(out,IMG_EINVAL,"QC-OPEN-FAIL",img_ctx_path(ctx),"映像虚拟大小为 0",NULL);
        return IMG_EINVAL;
    }
    bool force=img_args_bool(in,"force",false);
    /* 目标上已经有分区？→ 没 force 就拒绝（先看 55AA，空盘读表会失败，那是正常的）*/
    {
        uint8_t sig[512];
        bool has_tbl = (img_qcow2_read_full(q,0,sig,512)==IMG_OK && sig[510]==0x55 && sig[511]==0xaa);
        if(has_tbl){
            img_parttable_t *old=NULL;
            if(img_ctx_parttable(ctx,&old)==IMG_OK && old && used_count(old)>0 && !force){
                img_result_fail(out,IMG_EINVAL,"PT-NOT-EMPTY",img_ctx_path(ctx),
                    "目标盘已经有分区了，覆盖要用 force=true",
                    "确认要覆盖再传 force=true（只改分区表，不动数据）");
                img_result_u64(out,"parts_total",used_count(old));
                return IMG_EINVAL;
            }
        }
    }
    /* 容量：全部在写之前判完 */
    uint64_t need=0, need_gb=0;
    if(!img_ptfile_fits(&pf.t,disk,&need,&need_gb)){
        char wh[200];
        snprintf(wh,sizeof wh,"文件里的分区要占到 %llu 扇区，这张盘只有 %llu 扇区",
                 (unsigned long long)need,(unsigned long long)disk);
        img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","in",wh,
                        "建一张更大的盘再应用（至少 need_gb）");
        img_result_u64(out,"need_sectors",need);
        img_result_u64(out,"need_gb",need_gb);
        return IMG_ERANGE;
    }
    /* 写表（LBA 原样；GUID 原样；MBR 的 EBR 链由 write_mbr 重建）*/
    img_parttable_t t=pf.t;
    t.disk_sectors=disk;
    t.part_entry_count=128; t.part_entry_size=128;
    if(t.table==IMG_TABLE_MBR){
        memcpy(t.mbr_code, pf.mbr_code, 446);
        if(!t.disk_signature) t.disk_signature=0x4ccd932b;   /* 文件没给 → 给个稳定的 */
    }
    if(t.table==IMG_TABLE_GPT && !t.disk_guid[0]) img_guid_new(t.disk_guid);
    rc=img_parttable_write(q,&t);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PT-WRITE-FAIL",img_ctx_path(ctx),
            "写分区表失败（宿主满/映像损坏）",
            "qc.verify 自检；GPT 最坏情况可能只写了主表（备份表没写成），重跑一次本命令即可");
        return rc;
    }
    static char pj[64*1024];
    int pn=img_parttable_json(&t,pj,sizeof pj);
    static char fj[64*1024];
    int fn=img_ptfile_dump(&t,t.mbr_code,fj,sizeof fj);
    img_result_str(out,"table", t.table==IMG_TABLE_GPT?"GPT":"MBR");
    img_result_u64(out,"parts_total",used_count(&t));
    img_result_u64(out,"size_bytes",img_qcow2_size(q));
    img_result_u64(out,"sector_count",disk);
    if(pn>0) img_result_raw(out,"pt",pj);
    if(fn>0) img_result_raw(out,"file",fj);          /* 规范化之后的文件内容（App 可回存）*/
    return IMG_OK;
}

/* ================= part.add / part.remove / part.resize（2026-10-25）=================
 * REQ-20261025-02：分区管理页要能"在剩余空间里再切一块"，不必整盘重建。
 * ★ 铁律：只往【未分配空间】里加，绝不挤掉/移动已有分区；空间不够报 PART-NOSPACE
 *   （不是 PART-WRITE-FAIL：一个说"盘满了"，一个说"写坏了"，对用户完全是两件事）。
 * ★ 只改分区表，不格式化；要文件系统请接着调 fs.format（img! lba! fs=ntfs quick=true）。 */

typedef struct { uint64_t a,b; } img_rng_t;

static int slot_by_lba(const img_parttable_t *t, uint64_t lba){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba) return i;
    return -1;
}
static bool is_ext_container(const img_partition_t *p){
    return p->used && !p->logical && (p->type==IMG_MBR_EXTENDED || p->type==IMG_MBR_EXTENDED_L);
}
/* 1-based 序号：按起始 LBA 排序后的位置（= 用户看到的"第几块"） */
static uint64_t ordinal_of(const img_parttable_t *t, int slot){
    uint64_t n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(!t->parts[i].used) continue;
        if(t->parts[i].first_lba < t->parts[slot].first_lba) n++;
    }
    return n+1;
}
static uint64_t used_count(const img_parttable_t *t){
    uint64_t n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(t->parts[i].used) n++;
    return n;
}
/* 找【包含 lba】的那块未分配区（[gs,ge]）；lba 在已用区间上/盘外 → false。
 * ★ 0x0F 容器按"已用"处理：它的内部只给逻辑分区用，走 cmd_part_add 里另一条路径。*/
static bool gap_at(const img_parttable_t *t, uint64_t total, uint64_t lba,
                   uint64_t *gs, uint64_t *ge){
    if(!t || lba>=total) return false;
    img_rng_t r[IMG_MAX_PARTITIONS]; int n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        if(n<(int)(sizeof r/sizeof r[0])){ r[n].a=p->first_lba; r[n].b=p->last_lba; n++; }
    }
    for(int i=1;i<n;i++){
        img_rng_t k=r[i]; int j=i-1;
        while(j>=0 && r[j].a>k.a){ r[j+1]=r[j]; j--; }
        r[j+1]=k;
    }
    uint64_t cur = (t->table==IMG_TABLE_GPT) ? 34 : 1;   /* GPT: LBA1 头 + LBA2..33 表 */
    for(int i=0;i<=n;i++){
        uint64_t end = (i==n)? total-1 : r[i].a-1;
        if(lba>=cur && lba<=end){ if(gs) *gs=cur; if(ge) *ge=end; return true; }
        if(i<n) cur = r[i].b + 1;
    }
    return false;
}
/* 在【未分配空间】里找一段放得下 need 扇区的地方。
 * ★ 扩展分区容器本体算占用：容器里的空位只能给逻辑分区（走 part_add 的另一条路径），
 *   绝不能让主分区落在容器范围内（会与逻辑分区重叠，Windows 直接判表坏）。*/
static bool find_free_run(const img_parttable_t *t, uint64_t total, uint64_t align,
                          uint64_t need, bool max_gap, uint64_t *out_first, uint64_t *out_len){
    img_rng_t r[IMG_MAX_PARTITIONS]; int n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        if(n<(int)(sizeof r/sizeof r[0])){ r[n].a=p->first_lba; r[n].b=p->last_lba; n++; }
    }
    for(int i=1;i<n;i++){                      /* 按起点插入排序（n ≤ 128） */
        img_rng_t k=r[i]; int j=i-1;
        while(j>=0 && r[j].a>k.a){ r[j+1]=r[j]; j--; }
        r[j+1]=k;
    }
    uint64_t start = (t->table==IMG_TABLE_GPT) ? 34 : 1;   /* GPT: LBA1 头 + LBA2..33 分区表 */
    bool found=false; uint64_t bf=0, bl=0;
    for(int i=0;i<=n;i++){
        uint64_t gb = (i==0)? start : r[i-1].b+1;
        uint64_t ge = (i==n)? (total? total-1 : 0) : r[i].a-1;
        if(ge<gb) continue;
        uint64_t st=(gb+align-1)/align*align;
        if(st>ge) continue;
        uint64_t len=ge-st+1;
        if(len<need) continue;
        if(!found || (max_gap ? len>bl : st>bf)){ bf=st; bl=len; found=true; }
    }
    if(!found) return false;
    if(out_first) *out_first=bf;
    if(out_len) *out_len=bl;
    return true;
}
/* part.* 共用：打开镜像 + 读表（空盘要能和"表坏了"区分开） */
static int parts_open(img_ctx *ctx, img_qcow2 **q, img_parttable_t **t, img_result *out){
    *q=img_ctx_qcow2(ctx);
    if(!*q) return qc_open_fail(out,ctx);
    uint8_t mbr[512];
    int prc=img_qcow2_read_full(*q,0,mbr,512);
    if(prc!=IMG_OK || mbr[510]!=0x55 || mbr[511]!=0xaa){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","img",
            "该映像没有分区表（空盘）",
            "建盘时用 --table=gpt|mbr，或先 part.layout 写一张表再分区");
        return IMG_EINVAL;
    }
    int rc=img_ctx_parttable(ctx,t);
    if(rc||!*t){
        img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),
            img_strerror(rc),"qc.verify 自检一下；表坏了先备份再处理");
        return rc?rc:IMG_ESTATE;
    }
    return IMG_OK;
}

static const img_arg A_PARTADD[] = {
    {"img",     IMG_A_STR,  true,  NULL,   "映像路径（qcow2）", NULL},
    {"size-mb", IMG_A_U64,  false, NULL,   "新分区大小（MiB，≥16）；type=extended 时可省略 = 占满选中的空闲区", NULL},
    {"lba",     IMG_A_U64,  false, NULL,   "★ 就在【包含这个 LBA 的那块未分配区】里建（App 算好位置就用它；给了就忽略 where）", chk_lba},
    {"type",    IMG_A_ENUM, false, "data", "分区类型：data | efi | msr | recovery | logical（进已有扩展容器）| extended（建空扩展容器，MBR 专属）", NULL},
    {"label",   IMG_A_STR,  false, NULL,   "分区名（≤11 字符；只写进分区表，不格式化）", NULL},
    {"align-mb",IMG_A_U64,  false, "1",    "起点对齐（MiB，默认 1）", NULL},
    {"where",   IMG_A_ENUM, false, "tail", "放哪：tail=最靠后的空闲区（默认）| max-gap=最大空洞", NULL},
};
static int cmd_part_add(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=NULL; img_parttable_t *t=NULL;
    int rc=parts_open(ctx,&q,&t,out);
    if(rc!=IMG_OK) return rc;

    uint64_t align_mb=img_args_u64(in,"align-mb",1); if(!align_mb) align_mb=1;
    uint64_t align=align_mb*2048;

    const char *wh=img_args_get(in,"where"); if(!wh||!*wh) wh="tail";
    bool max_gap=!strcasecmp(wh,"max-gap");
    if(!max_gap && strcasecmp(wh,"tail")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","where","只接受 tail | max-gap",NULL);
        return IMG_EINVAL;
    }
    /* ★ 指定落点（REQ-20261026-10）：给了它就完全按它来，where 忽略。 */
    uint64_t want_lba=img_args_u64(in,"lba",0);

    const char *typ=img_args_get(in,"type"); if(!typ||!*typ) typ="data";
    img_mbr_type_t mt=IMG_MBR_NTFS_EXFAT; const char *guid=IMG_GUID_BASIC;
    bool want_logical=false, want_ext=false;
    if(!strcasecmp(typ,"data")){ mt=IMG_MBR_NTFS_EXFAT; guid=IMG_GUID_BASIC; }
    else if(!strcasecmp(typ,"efi")){ mt=IMG_MBR_ESP; guid=IMG_GUID_ESP; }
    else if(!strcasecmp(typ,"recovery")){ mt=IMG_MBR_RECOVERY; guid=IMG_GUID_RECOVERY; }
    else if(!strcasecmp(typ,"msr")){ mt=IMG_MBR_EMPTY; guid=IMG_GUID_MSR; }
    else if(!strcasecmp(typ,"logical")){ mt=IMG_MBR_NTFS_EXFAT; guid=IMG_GUID_BASIC; want_logical=true; }
    else if(!strcasecmp(typ,"extended")){ want_ext=true; }
    else { img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type",
                "只接受 data | efi | msr | recovery | logical | extended","data 就够用了"); return IMG_EINVAL; }
    if(t->table==IMG_TABLE_MBR && !strcasecmp(typ,"msr")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type","MSR 是 GPT 专用分区，MBR 磁盘不能有",NULL);
        return IMG_EINVAL;
    }
    if(want_logical && t->table!=IMG_TABLE_MBR){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type",
            "逻辑分区是 MBR 专属的（GPT 没有主分区/逻辑分区之分）","GPT 直接 type=data 即可");
        return IMG_EINVAL;
    }

    /* size-mb：只有 type=extended 可以省略（=吃满选中的空闲区） */
    uint64_t mb=img_args_u64(in,"size-mb",0);
    const img_arg_limit_t *L=img_arg_limit_find("part.add","size-mb");
    if(!mb && !want_ext){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","size-mb",
            "缺少必填参数 size-mb","只有 type=extended 可以省略（=占满空闲区）");
        return IMG_EINVAL;
    }
    if(mb && L && mb<L->min) return size_bad_at(out,"size-mb",L,mb);
    uint64_t sec=mb*2048;                       /* MiB → 512B 扇区 */
    const char *label=img_args_get(in,"label");
    if(label && strlen(label)>11){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","label",
            "分区名太长（最多 11 字符）","短一点，例如 DATA");
        return IMG_EINVAL;
    }

    uint64_t total=img_parttable_usable_last(t)+1;   /* 可用末尾（GPT 已扣掉尾部备份表） */
    uint64_t first=0; int slot=-1;

    if(want_ext){
        /* ★ 只建"空扩展容器"，绝不顺手往里塞逻辑分区（REQ-05 §7 / 2026-10-25 追加）：
         *   App 拿它做一张"扩展分区"卡片，用户自己决定要不要往里加、加多大。*/
        if(t->table!=IMG_TABLE_MBR){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type",
                "扩展分区是 MBR 专属的（GPT 没有主分区/逻辑分区之分）","GPT 直接 type=data 即可");
            return IMG_EINVAL;
        }
        int nprim=0, ext=-1;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            const img_partition_t *p=&t->parts[i];
            if(!p->used) continue;
            if(!p->logical){ nprim++; if(is_ext_container(p)) ext=i; }
        }
        if(ext>=0){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type",
                "这个盘已经有一个扩展分区了（MBR 只允许一个）",
                "往它里面加分区请用 type=logical");
            return IMG_EINVAL;
        }
        if(nprim>=4){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","img",
                "MBR 4 个主分区槽已经用满，建不了扩展分区（它也占一个槽）",
                "删掉一个主分区腾出槽位，或改用 GPT（重建时 --table=gpt）");
            return IMG_EINVAL;
        }
        uint64_t len=0;
        if(want_lba){
            /* 容器也要"点哪建哪"：给了 lba 就只在那块空闲区里放 */
            uint64_t gs=0, ge=0;
            if(!gap_at(t,total,want_lba,&gs,&ge)){
                img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
                    "这个 LBA 不在任何一块未分配区里（落在已有分区上，或超出磁盘）",
                    "用 part.show 看空闲区；或改用 where 让工具自己找");
                return IMG_EINVAL;
            }
            first = (want_lba>gs)? want_lba : gs;
            first = (first + align-1)/align*align;
            len = (first<=ge) ? (ge - first + 1) : 0;
        } else if(!find_free_run(t,total,align, sec?sec:1, max_gap, &first,&len)){
            uint64_t best=0,best_len=0;
            if(find_free_run(t,total,align,1,true,&best,&best_len))
                img_result_u64(out,"avail_mb",best_len/2048);
            else
                img_result_u64(out,"avail_mb",0);
            img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
                "没有足够的未分配空间放扩展分区","把扩展分区调小，或先删/缩小最后一块分区");
            return IMG_ERANGE;
        }
        if(!len){
            img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","lba",
                "这块空闲区对齐之后已经没有位置了","换一块空闲区");
            img_result_u64(out,"avail_mb",0);
            return IMG_ERANGE;
        }
        if(!sec) sec=len;                        /* 省略大小 = 吃满选中的空闲区 */
        else if(sec>len){
            img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
                "这段空闲区放不下这么大的扩展分区","调小一点，或换 where=max-gap 试试最大空洞");
            img_result_u64(out,"avail_mb",len/2048);
            return IMG_ERANGE;
        }
        rc=img_partition_add(t,first,sec,IMG_MBR_EXTENDED_L,NULL,
                             (label&&*label)?label:"Extended",false,&slot);
        if(rc!=IMG_OK){
            img_result_fail(out,rc,"PART-WRITE-FAIL","img","扩展分区建不了",NULL);
            return rc;
        }
    } else if(t->table==IMG_TABLE_MBR){
        int nprim=0, ext=-1;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            const img_partition_t *p=&t->parts[i];
            if(!p->used) continue;
            if(!p->logical){ nprim++; if(is_ext_container(p)) ext=i; }
        }
        if(ext>=0 && !want_lba){
            /* 已经有扩展容器 → 新分区只能进容器当逻辑分区（容器里的空位主分区用不了）
             * ★ 但给了 lba 时先不抢：那一块可能是容器外面的空闲区（REQ-20261026-10）。*/
            const img_partition_t *c=&t->parts[ext];
            uint64_t last_end=c->first_lba;
            for(int i=0;i<IMG_MAX_PARTITIONS;i++){
                const img_partition_t *p=&t->parts[i];
                if(p->used && p->logical && p->last_lba>last_end) last_end=p->last_lba;
            }
            uint64_t st=last_end+1;
            if(last_end>c->first_lba) st+=2048;
            st=(st+align-1)/align*align;
            if(st+sec-1 > c->last_lba){
                uint64_t avail_mb = (c->last_lba >= st) ? (c->last_lba - st + 1)/2048 : 0;
                img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
                    "扩展分区里放不下了（逻辑分区不能超出扩展容器）",
                    "缩小新分区，或扩展容器要重做（整盘重建）");
                img_result_u64(out,"avail_mb",avail_mb);
                return IMG_ERANGE;
            }
            slot=img_partition_add_logical(t,st,sec,mt,label);
            if(slot<0){
                img_result_fail(out,IMG_EIO,"PART-WRITE-FAIL","size-mb","逻辑分区加不进去",NULL);
                return IMG_EIO;
            }
            first=st;
        } else if(nprim>=4){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","img",
                "MBR 已经有 4 个主分区，而且没有扩展分区，加不了第 5 块",
                "删掉一个主分区，或改用 GPT（重建时 --table=gpt）");
            return IMG_EINVAL;
        }
    }

    if(slot<0 && want_logical){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type",
            "这个盘没有 0x0f 扩展分区，放不了逻辑分区",
            "建盘时用 --extra-mb 让工具把扩展容器一起建出来；或改用 type=data 建主分区");
        return IMG_EINVAL;
    }

    /* ★ 指定位置（REQ-20261026-10）：App 算好"点哪块建哪块"，工具只负责别跑偏。
     *   给了 lba 就完全跳过 where / 最大空洞那套挑选逻辑。*/
    if(slot<0 && want_lba){
        int ext=-1;
        if(t->table==IMG_TABLE_MBR)
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(is_ext_container(&t->parts[i])){ ext=i; break; }

        if(ext>=0 && want_lba>=t->parts[ext].first_lba && want_lba<=t->parts[ext].last_lba){
            /* 落点在一个扩展容器里 → 建逻辑分区；它前面还得塞得下 1 MiB 的 EBR */
            const img_partition_t *c=&t->parts[ext];
            uint64_t prev_end=c->first_lba;          /* 容器头就是第一个 EBR 的位置 */
            uint64_t next_start=c->last_lba+1;
            for(int i=0;i<IMG_MAX_PARTITIONS;i++){
                const img_partition_t *p=&t->parts[i];
                if(!p->used||!p->logical) continue;
                if(p->last_lba < want_lba && p->last_lba > prev_end) prev_end=p->last_lba;
                if(p->first_lba > want_lba && p->first_lba < next_start) next_start=p->first_lba;
            }
            uint64_t gs = prev_end + 1 + 2048;       /* 给这块逻辑分区自己的 EBR 留 1 MiB */
            uint64_t st = (want_lba>gs)? want_lba : gs;
            st = (st + align-1)/align*align;
            if(st >= next_start){
                img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
                    "这块空闲区放不下（逻辑分区前面还要留 1 MiB 给 EBR）",
                    "往后挪一点，或改用 type=logical 让工具自己找位置");
                return IMG_EINVAL;
            }
            uint64_t len = next_start - st;
            if(len < sec){
                img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
                    "这块空闲区放不下这个大小（不是全盘最大空洞）",
                    "把新分区调小，或换一块更大的空闲区");
                img_result_u64(out,"avail_mb",len/2048);
                return IMG_ERANGE;
            }
            slot=img_partition_add_logical(t,st,sec,mt,label);
            if(slot<0){
                img_result_fail(out,IMG_EIO,"PART-WRITE-FAIL","lba","逻辑分区加不进去",NULL);
                return IMG_EIO;
            }
            first=st;
        } else {
            uint64_t gs=0, ge=0;
            if(!gap_at(t,total,want_lba,&gs,&ge)){
                img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
                    "这个 LBA 不在任何一块未分配区里（落在已有分区上，或超出磁盘）",
                    "用 part.show 看空闲区；或改用 where / type 让工具自己找位置");
                return IMG_EINVAL;
            }
            uint64_t st = (want_lba>gs)? want_lba : gs;
            st = (st + align-1)/align*align;
            if(st > ge){
                img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","lba",
                    "对齐之后这块空闲区已经没位置了","换一块空闲区，或把 align-mb 调小");
                img_result_u64(out,"avail_mb",0);
                return IMG_ERANGE;
            }
            uint64_t len = ge - st + 1;
            if(len < sec){
                img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
                    "这块空闲区放不下这个大小（不是全盘最大空洞）",
                    "把新分区调小，或换一块更大的空闲区");
                img_result_u64(out,"avail_mb",len/2048);
                return IMG_ERANGE;
            }
            rc=img_partition_add(t,st,sec,mt,guid,label,false,&slot);
            if(rc!=IMG_OK){
                img_result_fail(out,rc,rc==IMG_EFULL?"PART-NOSPACE":"PART-WRITE-FAIL","lba",
                    rc==IMG_EFULL?"主分区槽位用完了（MBR 4 个 / GPT 128 个）":"分区加不进去",
                    rc==IMG_EFULL?"删掉一个不用的分区，或改用 GPT":NULL);
                return rc;
            }
            first=st;
        }
    }

    if(slot<0){
        uint64_t len=0;
        if(!find_free_run(t,total,align,sec,max_gap,&first,&len)){
            /* 报可用空间：给最大的一段空闲（App 显示"可用 3.8 GB"） */
            uint64_t best=0, best_len=0;
            if(find_free_run(t,total,align,1,true,&best,&best_len))
                img_result_u64(out,"avail_mb",best_len/2048);
            else
                img_result_u64(out,"avail_mb",0);
            img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
                "未分配空间不够放这块分区（不会去挤已有分区）",
                "把新分区调小；或先删/缩小最后一块分区腾地方；或建更大的盘");
            return IMG_ERANGE;
        }
        rc=img_partition_add(t,first,sec,mt,guid,label,false,&slot);
        if(rc!=IMG_OK){
            img_result_fail(out,rc,rc==IMG_EFULL?"PART-NOSPACE":"PART-WRITE-FAIL","size-mb",
                rc==IMG_EFULL?"分区槽位用完了（MBR 4 个主分区 / GPT 128 个）":"分区加不进去",
                rc==IMG_EFULL?"删掉一个不用的分区，或改用 GPT":NULL);
            return rc;
        }
    }

    rc=img_parttable_write(q,t);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PART-WRITE-FAIL",img_ctx_path(ctx),
            "分区表写回失败（映像损坏/宿主满）","qc.verify 自检；宿主空间确认一下");
        return rc;
    }
    static char pj[64*1024];
    int pn=img_parttable_json(t,pj,sizeof pj);
    img_result_u64(out,"lba",first);
    img_result_u64(out,"sectors",sec);
    img_result_u64(out,"size_mb",sec/2048);   /* extended 省略 size-mb 时也能算出真实值 */
    img_result_u64(out,"index",ordinal_of(t,slot));
    img_result_u64(out,"parts_total",used_count(t));
    if(label && *label) img_result_str(out,"label",label);
    img_result_str(out,"type",typ);
    if(t->parts[slot].logical) img_result_bool(out,"logical",true);
    if(pn>0) img_result_raw(out,"pt",pj);
    return IMG_OK;
}

static const img_arg A_PARTREMOVE[] = {
    {"img", IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
    {"lba", IMG_A_U64, true, NULL, "要删的分区起始 LBA（part.show 里的 first）", NULL},
};
static int cmd_part_remove(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=NULL; img_parttable_t *t=NULL;
    int rc=parts_open(ctx,&q,&t,out);
    if(rc!=IMG_OK) return rc;
    uint64_t lba=img_args_u64(in,"lba",0);
    int slot=slot_by_lba(t,lba);
    if(slot<0){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
            "分区表里没有起始 LBA 等于这个值的分区",
            "先用 part.show 看 first 字段（别拿容量/偏移顶替）");
        return IMG_EINVAL;
    }
    if(is_ext_container(&t->parts[slot])){
        int nlog=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].logical) nlog++;
        if(nlog>0){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
                "这是扩展分区容器（0x0F），里面还有逻辑分区",
                "先删它里面的逻辑分区；全删完之后容器本身才能删");
            return IMG_EINVAL;
        }
        /* ★ 空的扩展容器允许删（2026-10-25）：容器占着一个主分区槽，
         *   建错了又删不掉的话，用户只能整盘重建 —— 空容器里没有任何数据/索引，
         *   删掉只丢那一条主表项。*/
    }
    uint64_t sec=t->parts[slot].last_lba-t->parts[slot].first_lba+1;
    rc=img_partition_remove(t,slot);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PART-WRITE-FAIL","lba","删不掉这个分区",NULL);
        return rc;
    }
    rc=img_parttable_write(q,t);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PART-WRITE-FAIL",img_ctx_path(ctx),"分区表写回失败",NULL);
        return rc;
    }
    static char pj[64*1024];
    int pn=img_parttable_json(t,pj,sizeof pj);
    img_result_u64(out,"removed_lba",lba);
    img_result_u64(out,"removed_sectors",sec);
    img_result_u64(out,"parts_total",used_count(t));
    if(pn>0) img_result_raw(out,"pt",pj);
    return IMG_OK;
}

static const img_arg A_PARTRESIZE[] = {
    {"img",     IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
    {"lba",     IMG_A_U64, true, NULL, "分区起始 LBA（part.show 里的 first）", NULL},
    {"size-mb", IMG_A_U64, true, NULL, "新大小（MiB，≥16）", NULL},
};
static int cmd_part_resize(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=NULL; img_parttable_t *t=NULL;
    int rc=parts_open(ctx,&q,&t,out);
    if(rc!=IMG_OK) return rc;
    uint64_t lba=img_args_u64(in,"lba",0);
    int slot=slot_by_lba(t,lba);
    if(slot<0){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区表里没有起始 LBA 等于这个值的分区",NULL);
        return IMG_EINVAL;
    }
    /* ★ 只调【最后一块】：库里的 img_partition_resize 对非末位分区会移动后方分区，
     *   那等于改动别人的起点（数据全废）。App 目前也只需要"给最后一块扩/缩"。*/
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(p->used && i!=slot && p->first_lba>t->parts[slot].first_lba){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
                "只能调整最后一块分区（它后面还有分区，调整会挪动别人的起点）",
                "先删掉后面的分区，或整盘重建");
            return IMG_EINVAL;
        }
    }
    uint64_t mb=img_args_u64(in,"size-mb",0);
    const img_arg_limit_t *L=img_arg_limit_find("part.add","size-mb");
    if(!mb || (L && mb<L->min)) return size_bad_at(out,"size-mb",L,mb);
    uint64_t sec=mb*2048;
    /* 这块能长到多大：从它起点到可用末尾（App 用 avail_mb 显示"最多 X"） */
    uint64_t usable_last=img_parttable_usable_last(t);
    /* ★ 逻辑分区不能长出自己的扩展容器（REQ-20261025-07 §6.1）：
     *   容器末尾可能早于磁盘可用末尾（后面还有恢复分区），按容器的末尾收口。*/
    if(t->parts[slot].logical){
        int ext=-1;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(is_ext_container(&t->parts[i])){ ext=i; break; }
        if(ext<0){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba",
                "这块逻辑分区找不到所属的扩展容器（表结构异常）","qc.verify 自检一下");
            return IMG_EINVAL;
        }
        if(t->parts[ext].last_lba < usable_last) usable_last=t->parts[ext].last_lba;
    }
    uint64_t max_mb = (usable_last > t->parts[slot].first_lba)
                    ? (usable_last - t->parts[slot].first_lba + 1)/2048 : 0;
    if(!max_mb || mb > max_mb){
        img_result_fail(out,IMG_ERANGE,"PART-NOSPACE","size-mb",
            t->parts[slot].logical
              ? "逻辑分区不能超出它所在的扩展容器"
              : "这块分区最多只能长到可用末尾",
            "把目标调小，或先删/缩小后面的分区");
        img_result_u64(out,"avail_mb",max_mb);
        return IMG_ERANGE;
    }
    img_check_result_t err; memset(&err,0,sizeof err);
    rc=img_partition_resize(t,slot,sec,&err);
    if(rc!=IMG_OK){
        /* 这里的失败 99% 是空间问题（超出可用范围 / 后方挡住 / 需压缩前一块），
         * 统一给 PART-NOSPACE，并带 avail_mb = 这块【最多能长到多大】（App 显示"可用 X"）。*/
        const char *code=(rc==IMG_ENOENT)?"CMD-ARG-BAD":"PART-NOSPACE";
        img_result_fail(out,IMG_ERANGE,code,"size-mb",
            err.reason[0]?err.reason:"分区调不了这个大小",
            err.suggestion[0]?err.suggestion:"缩小目标大小，或先把后面的分区删掉");
        img_result_u64(out,"avail_mb",max_mb);
        return IMG_ERANGE;
    }
    rc=img_parttable_write(q,t);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PART-WRITE-FAIL",img_ctx_path(ctx),"分区表写回失败",NULL);
        return rc;
    }
    static char pj[64*1024];
    int pn=img_parttable_json(t,pj,sizeof pj);
    img_result_u64(out,"lba",lba);
    img_result_u64(out,"sectors",t->parts[slot].last_lba-t->parts[slot].first_lba+1);
    img_result_u64(out,"size_mb",mb);
    img_result_u64(out,"parts_total",used_count(t));
    if(pn>0) img_result_raw(out,"pt",pj);
    return IMG_OK;
}

/* ---------- fs.format ---------- */
static const img_arg A_FSFORMAT[] = {
    {"img",   IMG_A_STR,  true,  NULL,  "映像路径（qcow2）", NULL},
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

    /* ★ 卷标校验放在【任何写入之前】（REQ-20261026-11）：
     *   以前把中文卷标直接丢给 mkntfs -L（ASCII only），它写到一半失败 →
     *   盘上留个 part.scan 认不出的半截卷；FAT32 更糟：直接返回 ok 但写的是乱码。
     *   现在先按统一规则验一遍：不合格 → 一个字节都不写。*/
    char label_norm[80]; label_norm[0]=0;
    {
        int lrc=label_check(label, ntfs, true, label_norm, sizeof label_norm, out);
        if(lrc!=IMG_OK) return lrc;      /* 空 = 不写卷标（FAT 落 NO NAME） */
    }

    if(ntfs){
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,lba,pb);
        /* ★ 卷标不交给 mkntfs（它只认 ASCII，还会静默截到 11 字符）：
         *   格完盘再用 img_ntfs_set_label() 写 —— 和 ntfs.label 完全同一条路径，
         *   中文也能写（NTFS 卷标本身是 UTF-16）。*/
        o.label=NULL; o.force=true;
        o.quick=img_args_bool(in,"quick",true);   /* 默认快速：整盘零填在慢介质上要几十分钟 */
        img_span sp; img_span_prep(&sp,"ntfs","format","格式化 NTFS");
        img_span_kv_u64(&sp,"lba",lba); img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        rc=img_ntfs_format(q,&o,img_cmd_prog());
        img_span_end(&sp,rc);
        if(rc==IMG_ENOTSUP) return feat_unavail(out,"NTFS 格式化模块（mkntfs）",
                        "重新编译时开启 -DIMG_WITH_NTFS=ON（会引入 GPL-2.0+ 代码）");
        if(rc){
            img_result_fail(out,rc,"NTFS-FORMAT-FAIL",img_ctx_path(ctx),
                "NTFS 格式化失败（mkntfs 报错，卷可能不完整）",
                "确认分区 ≥128 MB、映像没被别的进程占用；再用 qc.verify 自检后重试");
            return rc;
        }
        if(label_norm[0]){
            img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
            if(!fs){
                img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",img_ctx_path(ctx),
                    "卷已经格式化好了，但挂不上来写卷标",
                    "用 ntfs.label 再补一次卷标即可（数据没问题）");
                img_result_bool(out,"formatted",true);
                return IMG_ESTATE;
            }
            int lrc=img_ntfs_set_label(fs,label_norm);
            img_ntfs_umount(fs);
            if(lrc!=IMG_OK){
                img_result_fail(out,lrc,"LABEL-WRITE-FAIL",img_ctx_path(ctx),
                    "卷已经格式化好了，但卷标没写上（不影响使用）",
                    "用 ntfs.label 再写一次卷标；或换个短一点的卷标");
                img_result_bool(out,"formatted",true);
                return lrc;
            }
        }
        img_result_str(out,"fs","NTFS");
    } else {
        img_fat32_opts_t o={0};
        o.partition_lba=lba; o.partition_bytes=pb;
        o.label=label_norm[0]? label_norm : NULL;   /* 已归一化（大写 + 纯 ASCII）*/
        o.hidden_sectors=(uint32_t)lba; o.num_fats=2; o.active_boot=true;
        img_fat32_layout_t L; memset(&L,0,sizeof L);
        img_span sp; img_span_prep(&sp,"fat32","format","格式化 FAT32");
        img_span_kv_u64(&sp,"lba",lba); img_span_kv_u64(&sp,"size_bytes",pb);
        img_span_begin(&sp);
        rc=img_fat32_format(q,&o,&L,img_cmd_prog());
        img_span_end(&sp,rc);
        if(rc){
            img_result_fail(out,rc,"FAT-PUT-FAIL",img_ctx_path(ctx),
                "FAT32 格式化失败（写 BPB/FAT/根目录时出错）",
                "确认分区 ≥32 MB、映像没被占用；再用 qc.verify 自检");
            return rc;
        }
        img_result_str(out,"fs","FAT32");
        img_result_u64(out,"cluster_bytes",(uint64_t)L.sectors_per_cluster*512u);
        img_result_u64(out,"cluster_count",L.cluster_count);
    }
    img_result_u64(out,"lba",lba);
    img_result_u64(out,"bytes",pb);
    if(label_norm[0]) img_result_str(out,"label",label_norm);
    return IMG_OK;
}

/* ---------- ntfs.put / ntfs.export（往 NTFS 读写本地文件）---------- */
static const img_arg A_NTFSPUT[] = {
    {"img", IMG_A_STR, true,  NULL,   "映像路径（qcow2）", NULL},
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
    {"img",  IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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
    {"img",  IMG_A_STR,  true,  NULL,    "映像路径（qcow2）", NULL},
    {"mode", IMG_A_ENUM, false, "auto",  "引导模式：auto | uefi | uefi32 | bios（uefi32=32 位 UEFI 盘）", NULL},
    {"expect-recovery", IMG_A_BOOL, false, "false", "要求有恢复分区（默认不要求）", NULL},
};
static int cmd_boot_check(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（引导链自检）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_NTFS3G
    const char *m=img_args_get(in,"mode"); if(!m||!*m) m="auto";
    img_boot_mode_t mode=IMG_BOOT_AUTO;
    unsigned flags=0;
    bool want32=false;
    if(!strcasecmp(m,"uefi")) mode=IMG_BOOT_UEFI;
    else if(!strcasecmp(m,"uefi32")){ mode=IMG_BOOT_UEFI; flags|=IMG_BC_FLAG_UEFI32; want32=true; }
    else if(!strcasecmp(m,"bios")) mode=IMG_BOOT_BIOS;
    else if(strcasecmp(m,"auto")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","mode","只接受 auto | uefi | uefi32 | bios",NULL); return IMG_EINVAL; }
    /* ★ --expect-recovery 已废弃（2026-11-04）：恢复分区的检查整体搬去 recovery.status。
     *   参数保留但不再起作用 —— App 旧调用传了也不会报"未知参数"。 */
    (void)img_args_bool(in,"expect-recovery",false);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_bootcheck_result_t r; memset(&r,0,sizeof r);
    img_span sp; img_span_prep(&sp,"bc","check","引导链自检");
    img_span_begin(&sp);
    int rc=img_bootcheck_ex(q,mode,flags,&r,img_cmd_prog());
    /* ★ 必须在 _ex 之后填：它开头会 memset(out) 把这里擦掉（踩过一次）*/
    if(rc==IMG_OK) snprintf(r.img,sizeof r.img,"%s",img_ctx_path(ctx));
    img_span_end(&sp,rc);
    if(rc!=IMG_OK && r.count==0){
        img_result_fail(out,rc,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL); return rc;
    }
    static char jb[65536];
    int n=img_bootcheck_json_array(&r,jb,sizeof jb);   /* ★ 必须是数组，不是 NDJSON */
    char sum[256]; img_bootcheck_summary(&r,sum,sizeof sum);
    img_result_str(out,"mode", want32?"uefi32":(r.is_uefi?"uefi":"bios"));
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
static void iso_list_cb(void *ud, const char *name, bool is_dir, uint64_t size, uint64_t mtime){
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
    int k=snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"path\":\"%s\",\"dir\":%s,"
                       "\"size\":%llu,\"mtime\":%llu}",
                   a->n?",":"",nm,fp,is_dir?"true":"false",
                   (unsigned long long)size,(unsigned long long)mtime);
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
    {"depth",IMG_A_U64, false, "32",   "递归最大深度（1~64；防病态映像）", NULL},
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
    enum { JB_CAP = 262144 };
    char *jb=img_tbuf(IMG_TB_ISO_LIST,JB_CAP);
    if(!jb) return tb_nomem(out,"iso.ls 条目缓冲");
    jb[0]='['; isolist_t acc={jb,JB_CAP-2,1,0,(unsigned)max,false,0,find};
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
    char kind[16]; snprintf(kind,sizeof kind,"%s",img_iso_kind(iso));   /* REQ-45：崩了能报哪条路 */
    uint32_t lbs=0;
    {   /* lb_size 没有公开 getter：用 root 扇区反推没意义，这里走 probe 太贵 → 用 kind 够 */
        lbs=2048;
    }
    img_iso_close(iso);
    if(rc!=IMG_OK){ img_result_fail(out,IMG_ENOENT,"FAT-DIR-NOTFOUND",path,"ISO 里没有这个目录",NULL); return IMG_ENOENT; }
    jb[acc.len++]=']'; jb[acc.len]=0;
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"truncated",acc.trunc);
    img_result_bool(out,"recursive",rec);
    img_result_u64(out,"dirs_scanned",(uint64_t)acc.dirs);
    if(vol) img_result_str(out,"volume",vol);
    if(vol) img_result_str(out,"label",vol);       /* REQ-46 §3.2：App 要的字段名 */
    img_result_str(out,"kind",kind);      /* udf/joliet/iso9660（REQ-45 §4）*/
    img_result_u64(out,"lb_size",(uint64_t)lbs);
    img_result_raw(out,"entries",jb);
    return IMG_OK;
}
/* ---------- iso.probe：只读卷描述符，不列目录（REQ-45 §3 的两段式预检）---------- */
static const img_arg A_ISOPROBE[] = {
    {"iso", IMG_A_STR, true, NULL, "ISO 文件路径（只读；不修改它）", NULL},
};
static int cmd_iso_probe(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *isop=img_args_get(in,"iso");
    if(!isop||!*isop){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","iso","要一个 ISO 路径",NULL);
        return IMG_EINVAL;
    }
    img_iso_probe_t pr; char err[256]="";
    int rc=img_iso_probe_path(isop,&pr,err,sizeof err);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,iso_open_code(rc),isop,err[0]?err:"不能识别这个 ISO",NULL);
        return rc;
    }
    img_result_str(out,"kind", pr.udf?"udf":(pr.joliet?"joliet":"iso9660"));
    img_result_bool(out,"udf",pr.udf);
    img_result_bool(out,"joliet",pr.joliet);
    img_result_bool(out,"iso9660",pr.iso9660);
    img_result_u64(out,"lb_size",(uint64_t)pr.lb_size);
    if(pr.volume[0]){ img_result_str(out,"volume",pr.volume);
                      img_result_str(out,"label",pr.volume); }
    if(pr.udf){
        img_result_u64(out,"root_lb",pr.root_lb);
    }else{
        img_result_u64(out,"iso_root_lba",(uint64_t)pr.iso_root_lba);
        img_result_u64(out,"iso_root_len",(uint64_t)pr.iso_root_len);
    }
    return IMG_OK;
}

static const img_arg A_ISOEXTRACT[] = {
    {"iso",  IMG_A_STR, true,  NULL, "ISO 文件路径", NULL},
    {"path", IMG_A_STR, true,  NULL, "ISO 内的文件路径（如 /sources/install.wim）", NULL},
    {"dst",  IMG_A_STR, true,  NULL, "本地输出文件（会覆盖）", NULL},
    {"max-bytes", IMG_A_U64, false, "0", "只抽前 N 字节（0=整份；自测/预览用）", NULL},
    {"inner", IMG_A_STR, false, NULL, "（REQ-28 B″）要进 ISO 里的 WIM/ESD 取文件时，给它的内部路径（如 /sources/install.wim）", NULL},
    {"index", IMG_A_U64, false, "1", "（配 --inner）WIM/ESD 里的映像索引", NULL},
};
/* 大小写不敏感的子串查找（strcasestr 在部分 libc/Android 上没有声明，自己写一个收口）*/
static const char *ci_str(const char *h, const char *n){
    if(!h||!n||!*n) return NULL;
    size_t nl=strlen(n);
    for(const char *p=h; *p; p++){
        size_t k=0;
        while(k<nl && p[k] && tolower((unsigned char)p[k])==tolower((unsigned char)n[k])) k++;
        if(k==nl) return p;
    }
    return NULL;
}

static const char *wim_fs_code(int rc, int kind);   /* 见 wim.extract（下面定义）*/

static int cmd_iso_extract(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *isop=img_args_get(in,"iso");
    const char *path=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    const char *inner=img_args_get(in,"inner");
    uint64_t maxb=img_args_u64(in,"max-bytes",0);
    if(!isop||!path||!dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","iso/path/dst","三个都必填",NULL); return IMG_EINVAL; }

    /* ★ REQ-28 B″：--inner 给了就走"进 ISO 里的 WIM 取文件"，App 一个命令走到底。
     *   和 wim.extract 是同一套实现（img_vfd 直读，不落 2.7 GB 整份 WIM）。*/
    if(inner && *inner){
        if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 解析）","重新编译时开启该功能");
#if IMG_WITH_WIM
        if(maxb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","max-bytes",
                    "--inner（进 WIM 取文件）不支持 --max-bytes","去掉 --max-bytes，或改用 iso.extract --path=<ISO 层文件>");
                  return IMG_EINVAL; }
        if(path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","WIM 内路径必须以 / 开头",NULL); return IMG_EINVAL; }
        int index=(int)img_args_u64(in,"index",1);
        char local[1024]="", used[256]="", why[400]="";
        wimsrc_kind wk=WIMSRC_PLAIN; uint64_t nbytes=0;
        int rrc=wim_resolve_source(isop,inner,local,sizeof local,used,sizeof used,&wk,&nbytes,
                                   why,sizeof why,img_cmd_prog());
        if(rrc!=IMG_OK) return wim_src_fail(out,rrc,isop,why);
        char werr[256]=""; uint64_t wgot=0;
        rrc=img_wim_extract_file(local,index,path,dst,img_cmd_prog(),&wgot,werr,sizeof werr);
        wim_src_cleanup(local,wk);
        if(rrc!=IMG_OK){
            if(rrc==IMG_ECANCELLED) unlink(dst);
            img_result_fail(out,rrc,wim_fs_code(rrc,1),path,werr[0]?werr:"抽取失败",NULL);
            return rrc;
        }
        img_result_str(out,"path",path);
        img_result_str(out,"dst",dst);
        img_result_u64(out,"bytes",wgot);
        img_result_str(out,"container","iso");
        img_result_str(out,"inner_path",used[0]?used:inner);
        img_result_bool(out,"direct", wk==WIMSRC_VFD);
        return IMG_OK;
#else
        return feat_unavail(out,"wimlib（WIM/ESD 解析）","重新编译时开启该功能");
#endif
    }
    char err[256]="";
    img_iso *iso=NULL;
    int rc=img_iso_open_path(&iso,isop,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,iso_open_code(rc),isop,err[0]?err:"无法打开 ISO",NULL); return rc; }
    uint64_t fsz=0; bool isdir=false;
    rc=img_iso_stat(iso,path,&fsz,&isdir);
    if(rc!=IMG_OK){
        /* ★ REQ-28 B′：App 很可能把"映像内部路径"当 ISO 路径传进来（Winre.wim 在 install.wim 里）。
         *   直接说"没有"＋"重试"等于把人带进沟里 —— 有 WIM 就指路。*/
        wimscan_t sc; wim_scan_iso(iso,&sc);
        img_iso_close(iso);
        char hint[1200];
        if(sc.n>0){
            const char *pick=sc.path[0];
            for(int i=0;i<sc.n;i++)                       /* 优先 install.*：WinRE 只可能在它里面 */
                if(ci_str(sc.path[i],"install.")) { pick=sc.path[i]; break; }
            snprintf(hint,sizeof hint,
                     "ISO 里没有这个路径；它可能在映像内部（WIM/ESD）→ 用 wim.extract --wim=… "
                     "--inner=%s --path=%s --dst=…（或 iso.extract 加 --inner=%s）",
                     pick,path,pick);
            img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",path,hint,
                            sc.n>1?"这个 ISO 有多条候选，按需换 --inner":"路径给的是 ISO 内的文件；WIM 里的文件要用 --inner");
        } else {
            img_result_fail(out,IMG_ENOENT,"FAT-ENTRY-NOTFOUND",path,"ISO 里没有这个文件",
                            "确认路径大小写/层级；用 iso.ls --path=/ 看目录");
        }
        return IMG_ENOENT;
    }
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
    /* ★ 收尾必须补一条 100%：进度是 200ms 节流的，短文件可能一条都没发出去 */
    if(img_cmd_prog() && fsz){
        img_prog_bytes(img_cmd_prog(),got,fsz);
        img_prog_done(img_cmd_prog(),"抽取完成");
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
    {"img",  IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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

/* ---------- fat.move（盘内改名 / 移动，单条路径）----------
 * 目标已存在 → 明确报 FAT-ENTRY-EXISTS，不默认覆盖（覆盖=丢数据，必须 App 显式决定：
 * 先 fat.rm 再 fat.move，两步各自可见）。*/
/* ---------- ntfs.parent（只读：目录的 ".." 父引用自洽检查）----------
 * 为什么需要：NTFS 的"我爹是谁"记在子目录自己的 $I30 ".." 项里，搬目录时忘了同步
 *   就会出现"路径能打开、但 cd .. 跳回旧父"的错乱 —— 光看 ntfs.list 是看不出来的。
 *   上游 libntfs-3g 也没有这个能力（dir.c 里 ntfs_link 的 FIXME 就是这件事），
 *   所以我们自己补了同步，就必须自己补一个能证明它生效的检查。
 * 三项：parent_ino（.. 指向谁）、parent_by_path（按路径算的父是谁）、
 *   backlink_ok（父索引里这个名字是否指回本目录）；consistent = 前两者相等且 backlink_ok。*/
static const img_arg A_NTFSRPARENT[] = {
    {"img",  IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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
    {"img",  IMG_A_STR, true, NULL, "映像路径（qcow2，必须是 GPT）", NULL},
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

/* ================= 找 WinRE 的唯一出口（REQ-24/25）=================
 * 为什么要有它：官方 ISO 的 `--scan` 会列【两条】候选 —— /sources/boot.wim（WinPE，
 * 没有 WinRE）和 /sources/install.wim（有）。App 拿 candidates[0] 去 find 就必然
 * "找不到"，于是把"有 WinRE 的官方盘"判成"不含 Windows RE"（用户实测的假拦）。
 * 所以：找 WinRE 这件事必须由工具做完 —— 按顺序把候选都试一遍，谁有就报谁。
 *
 * 三种源：
 *   ① 裸 winre.wim / 普通 WIM / ESD        → 直接 img_winre_find
 *   ② 裸 winre.wim（根下没有 \Windows）    → self=true（整个文件就是 payload）
 *   ③ ISO                                  → 逐条候选（--inner 指定的排最前）找
 */
typedef struct {
    bool     found, self;
    char     inner_path[256];    /* ISO 里的路径；非 ISO/self 时为空 */
    char     container[8];       /* wim / esd */
    char     internal[256];      /* WIM 内路径；self 时为空 */
    char     src_resolved[1024]; /* 已解析的本地/VFD 路径（self 时就是原文件） */
    wimsrc_kind kind;
    int      index;
    uint64_t size_bytes;
    char     tried[1024];        /* JSON：试过哪些候选 */
    int      n_tried;
} winre_pick_t;

static void wp_add_tried(winre_pick_t *p, const char *inner, bool found, uint64_t sz){
    if(p->n_tried>=16) return;
    size_t len=strlen(p->tried);
    if(len+300>sizeof p->tried) return;
    snprintf(p->tried+len,sizeof p->tried-len,
             "%s{\"inner_path\":\"%s\",\"found\":%s,\"size_bytes\":%llu}",
             p->n_tried?",":"",inner?inner:"",found?"true":"false",
             (unsigned long long)sz);
    p->n_tried++;
}

/* 释放 winre_pick 占的资源（VFD 槽 / 抽取出来的临时 wim）*/
static void winre_pick_free(winre_pick_t *p){
    if(!p) return;
    if(p->src_resolved[0]){
        if(p->kind==WIMSRC_VFD) img_vfd_unmount(p->src_resolved);
        else if(p->kind==WIMSRC_EXTRACT) unlink(p->src_resolved);
    }
    p->src_resolved[0]=0;
}

/* 在【一个已解析的 wim 路径】里找；顺带认 self */
static int wp_probe_one(const char *wim_path, int idx, winre_pick_t *p, char *why, size_t whycap){
    img_winre_info_t info; memset(&info,0,sizeof info);
    int rc=img_winre_find(wim_path,idx,&info);
    if(rc!=IMG_OK){
        /* 打不开 / 不是 WIM —— 记下来继续试别的候选，不急着下结论 */
        if(why && whycap) snprintf(why,whycap,"WIM 无法打开（rc=%d）",rc);
        return rc;
    }
    if(info.found){
        p->found=true; p->self=false;
        p->index=info.index; p->size_bytes=info.size_bytes;
        snprintf(p->container,sizeof p->container,"%s",info.container);
        snprintf(p->internal,sizeof p->internal,"%s",info.internal_path);
        return IMG_OK;
    }
    bool self=false; char w2[200]="";
    if(img_wim_is_winre_self(wim_path,&self,w2,sizeof w2)==IMG_OK && self){
        struct stat st;
        p->found=true; p->self=true; p->index=1; p->size_bytes=0;
        if(stat(wim_path,&st)==0) p->size_bytes=(uint64_t)st.st_size;
        snprintf(p->container,sizeof p->container,"wim");
        p->internal[0]=0;
        if(why && whycap) snprintf(why,whycap,"%s",w2);
        return IMG_OK;
    }
    if(why && whycap) snprintf(why,whycap,"这个映像里没有 Winre.wim");
    return IMG_OK;                      /* 打开了，但没有 —— 不是错误 */
}

static int winre_pick_raw(const char *src, const char *inner_opt, int index, bool try_all,
                          winre_pick_t *out, char *why, size_t whycap, img_prog *prog)
{
    memset(out,0,sizeof *out);
    out->tried[0]='['; out->tried[1]=0;
    if(why && whycap) why[0]=0;
    out->index=index>0?index:1;
    if(!src || !*src) return IMG_EINVAL;

    size_t n=strlen(src);
    bool is_iso = n>4 && !strcasecmp(src+n-4,".iso");

    if(!is_iso){
        snprintf(out->src_resolved,sizeof out->src_resolved,"%s",src);
        out->kind=WIMSRC_PLAIN;
        int rc=wp_probe_one(src,index,out,why,whycap);
        wp_add_tried(out,"",out->found,out->size_bytes);
        if(rc!=IMG_OK) return rc;       /* 打不开 = WIM-NOTFOUND 那一类 */
        return IMG_OK;                  /* found 可能为 false（真没有）*/
    }

    /* ISO：先扫候选，再按顺序试（--inner 指定的排最前）*/
    img_iso *iso=NULL; char e[256]="";
    int rc=img_iso_open_path(&iso,src,e,sizeof e);
    if(rc!=IMG_OK){ if(why&&whycap) snprintf(why,whycap,"%s",e[0]?e:"ISO 无法打开"); return rc; }
    wimscan_t sc; wim_scan_iso(iso,&sc);
    img_iso_close(iso);
    if(sc.n==0){ if(why&&whycap) snprintf(why,whycap,"ISO 里没有 install.wim/.esd"); return IMG_ENOENT; }

    int order[32], no=0;
    if(inner_opt && *inner_opt)
        for(int i=0;i<sc.n;i++) if(!strcasecmp(sc.path[i],inner_opt)) order[no++]=i;
    for(int i=0;i<sc.n && no<32;i++){
        bool dup=false;
        for(int j=0;j<no;j++) if(order[j]==i) dup=true;
        if(!dup) order[no++]=i;
    }

    int last_ok=IMG_OK;
    for(int k=0;k<no;k++){
        int i=order[k];
        char p[1024]="", used[256]="", w2[400]="";
        wimsrc_kind kind=WIMSRC_PLAIN; uint64_t nb=0;
        int rrc=wim_resolve_source(src,sc.path[i],p,sizeof p,used,sizeof used,&kind,&nb,w2,sizeof w2,prog);
        if(rrc!=IMG_OK){ wp_add_tried(out,sc.path[i],false,0); last_ok=rrc; continue; }
        int prc=wp_probe_one(p,index,out,w2,sizeof w2);
        if(prc==IMG_OK && out->found){
            snprintf(out->inner_path,sizeof out->inner_path,"%s",sc.path[i]);
            snprintf(out->src_resolved,sizeof out->src_resolved,"%s",p);
            out->kind=kind;
            if(out->self && !out->size_bytes) out->size_bytes=nb;   /* VFD 上 stat 可能拿不到 */
            wp_add_tried(out,sc.path[i],true,out->size_bytes);
            if(why&&whycap) snprintf(why,whycap,"命中 %s",sc.path[i]);
            return IMG_OK;              /* ★ 命中就不 cleanup：调用方还要用这个路径 */
        }
        wp_add_tried(out,sc.path[i],false,0);
        wim_src_cleanup(p,kind);
        if(!try_all) break;             /* 指定了 --inner 且要求严格：只看这一条 */
    }
    if(why && whycap && !why[0]) snprintf(why,whycap,"这个 ISO 里的 %d 条候选都没有 Winre.wim",no);
    return last_ok==IMG_OK ? IMG_OK : last_ok;   /* 打开了但都没有 → found=false */
}

/* 对外的入口：把 candidates_tried 收成合法 JSON 数组（img_result_raw 只存指针）*/
static int winre_pick(const char *src, const char *inner_opt, int index, bool try_all,
                      winre_pick_t *out, char *why, size_t whycap, img_prog *prog)
{
    int rc=winre_pick_raw(src,inner_opt,index,try_all,out,why,whycap,prog);
    if(out->n_tried){
        size_t L=strlen(out->tried);
        if(L+2<sizeof out->tried){ out->tried[L]=']'; out->tried[L+1]=0; }
    } else out->tried[0]=0;
    return rc;
}

/* ---------- winre.find（只读：在 WIM/ESD 里定位 Winre.wim）---------- */
static const img_arg A_WINREFIND[] = {
    {"src",   IMG_A_STR, true,  NULL, "WIM/ESD/ISO 路径；也可以直接给【裸 winre.wim】", NULL},
    {"index", IMG_A_U64, false, "0",  "映像索引（0=扫全部）", NULL},
    {"inner", IMG_A_STR, false, NULL, "（ISO）优先试这条，如 /x64/sources/install.wim；没有就接着试别的", NULL},
    {"probe-only", IMG_A_BOOL, false, "false", "只检测、不落任何东西（本命令本来就只读；给 App 一个统一写法）", NULL},
};
static int cmd_winre_find(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 解析）","重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_WIM
    const char *src=img_args_get(in,"src");
    int idx=(int)img_args_u64(in,"index",0);
    winre_pick_t pk; char why[400]="";
    int rc=winre_pick(src,img_args_get(in,"inner"),idx,true,&pk,why,sizeof why,img_cmd_prog());
    if(rc!=IMG_OK){
        winre_pick_free(&pk);
        if(rc==IMG_ENOENT||rc==IMG_EFORMAT)
            img_result_fail(out,rc,"WIM-NOTFOUND",src,why[0]?why:"不是 WIM/ESD，或文件无法打开",
                            "确认选的是映像文件（ISO/WIM/ESD/winre.wim），不是压缩包");
        else
            img_result_fail(out,rc,"IMG-READ-FAIL",src,why[0]?why:"读这个文件时工具出错了",
                            "这是工具侧问题，不是\"映像里没有 WinRE\"；重试一次，或把结果反馈给我们");
        return rc;
    }
    if(!pk.found){
        winre_pick_free(&pk);
        img_result_fail(out,IMG_ENOENT,"WINRE-NOTFOUND",src,
            why[0]?why:"这个文件里没有 Winre.wim",
            "换一份含 WinRE 的映像，或直接选裸 winre.wim");
        if(pk.n_tried){
            char *tb=img_tbuf(IMG_TB_ITEMS1,strlen(pk.tried)+1);
            if(tb){ snprintf(tb,strlen(pk.tried)+1,"%s",pk.tried); img_result_raw(out,"candidates_tried",tb); }
        }
        return IMG_ENOENT;
    }
    img_result_bool(out,"found",true);
    img_result_bool(out,"self",pk.self);
    img_result_u64(out,"index",(uint64_t)pk.index);
    img_result_str(out,"container",pk.container);
    img_result_str(out,"internal_path",pk.internal);
    img_result_u64(out,"size_bytes",pk.size_bytes);
    if(pk.inner_path[0]){
        img_result_str(out,"iso_inner_path",pk.inner_path);
        img_result_bool(out,"direct", pk.kind==WIMSRC_VFD);
    }
    if(pk.n_tried>1){
        /* ★ tried 在栈上，img_result_raw 只存指针 → 必须拷进线程缓冲，否则渲染时是野指针 */
        char *tb=img_tbuf(IMG_TB_ITEMS1,strlen(pk.tried)+1);
        if(tb){ snprintf(tb,strlen(pk.tried)+1,"%s",pk.tried); img_result_raw(out,"candidates_tried",tb); }
    }
    winre_pick_free(&pk);
    return IMG_OK;
#else
    (void)in;(void)out; return IMG_ENOTSUP;
#endif
}

/* ================= REQ-30.2/30.3：mkdir / stat ================= */
typedef struct { uint64_t lba, pb; } part_ref;
static int resolve_part(img_ctx *ctx, uint64_t lba, part_ref *out, img_result *res){
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t){ img_result_fail(res,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL);
                return rc?rc:IMG_ESTATE; }
    out->lba=lba; out->pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            out->pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!out->pb){ img_result_fail(res,IMG_EINVAL,"CMD-ARG-BAD","lba",
                                  "分区不存在或 LBA 不在分区表里",NULL); return IMG_EINVAL; }
    return IMG_OK;
}
static const img_arg A_NTFSMKDIR[] = {
    {"img",      IMG_A_STR,  true,  NULL,   "qcow2 映像", NULL},
    {"lba",      IMG_A_U64,  true,  NULL,   "NTFS 分区起始 LBA", chk_lba},
    {"path",     IMG_A_STR,  true,  NULL,   "要建的目录（绝对路径）", NULL},
    {"parents",  IMG_A_BOOL, false, "true", "父目录不存在就一起建", NULL},
};
static int cmd_ntfs_mkdir(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 目录操作）","重新编译时开启该功能");
#if IMG_WITH_NTFS3G
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    part_ref pr; int rc=resolve_part(ctx,img_args_u64(in,"lba",0),&pr,out);
    if(rc!=IMG_OK) return rc;
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,pr.lba,pr.pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    uint64_t ino=0; bool isdir=false;
    if(img_ntfs_verify_path(fs,path,&ino,&isdir)==IMG_OK){
        img_ntfs_umount(fs);
        if(isdir){ img_result_str(out,"path",path); img_result_bool(out,"created",false);
                   img_result_u64(out,"ino",ino); return IMG_OK; }
        img_result_fail(out,IMG_EEXISTS,"PATH-EXISTS-FILE",path,"同名文件已存在，不能当目录",NULL);
        return IMG_EEXISTS;
    }
    rc=img_ntfs_mkdir_p(fs,path);
    uint64_t nin=0; bool nisdir=false;
    if(rc==IMG_OK) (void)img_ntfs_verify_path(fs,path,&nin,&nisdir);
    img_ntfs_umount(fs);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"FS-WRITE-FAIL",path,img_strerror(rc),"检查父路径/权限/空间");
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_bool(out,"created",true);
    img_result_u64(out,"ino",nin);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}
static const img_arg A_NTFSSTAT[] = {
    {"img",  IMG_A_STR, true, NULL, "qcow2 映像", NULL},
    {"lba",  IMG_A_U64, true, NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true, NULL, "要查的路径（绝对路径）", NULL},
};
static int cmd_ntfs_stat(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（NTFS 单条详情）","重新编译时开启该功能");
#if IMG_WITH_NTFS3G
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    part_ref pr; int rc=resolve_part(ctx,img_args_u64(in,"lba",0),&pr,out);
    if(rc!=IMG_OK) return rc;
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,pr.lba,pr.pb);
    if(!fs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD",path,"不是合法 NTFS（未格式化或 LBA 不对）",NULL); return IMG_ESTATE; }
    img_ntfs_ent_t e; rc=img_ntfs_stat(fs,path,&e);
    img_ntfs_umount(fs);
    if(rc==IMG_ENOENT){ img_result_fail(out,IMG_ENOENT,"PATH-NOT-FOUND",path,"路径不存在",NULL); return rc; }
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,img_strerror(rc),NULL); return rc; }
    char fl[16]; snprintf(fl,sizeof fl,"0x%x",e.flags);
    img_result_str(out,"path",path);
    img_result_str(out,"type",e.is_dir?"dir":"file");
    img_result_bool(out,"dir",e.is_dir);
    img_result_u64(out,"size",e.size);
    img_result_u64(out,"alloc_bytes",e.alloc_bytes);
    img_result_u64(out,"ino",e.ino);
    img_result_u64(out,"links",e.links);
    img_result_str(out,"attr_flags",fl);
    img_result_u64(out,"ctime",e.times[0]);
    img_result_u64(out,"mtime",e.times[1]);
    img_result_u64(out,"atime",e.times[2]);
    img_result_u64(out,"change_time",e.times[3]);
    /* App 只想要个能显示的 mode：只读位 + 目录位 */
    uint64_t mode = e.is_dir ? 0755 : 0644;
    if(e.flags & 0x1) mode &= ~(uint64_t)0222;          /* FILE_ATTRIBUTE_READONLY */
    img_result_u64(out,"mode",mode);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}
static const img_arg A_FATMKDIR[] = {
    {"img",      IMG_A_STR,  true,  NULL,   "qcow2 映像", NULL},
    {"lba",      IMG_A_U64,  true,  NULL,   "FAT32 分区起始 LBA", chk_lba},
    {"path",     IMG_A_STR,  true,  NULL,   "要建的目录（绝对路径）", NULL},
    {"parents",  IMG_A_BOOL, false, "true", "父目录不存在就一起建", NULL},
};
static int cmd_fat_mkdir(img_ctx *ctx, const img_args *in, img_result *out){
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),"不是合法 qcow2",NULL); return IMG_EFORMAT; }
    img_fat32_fs *fs=img_fat32_mount(q,img_args_u64(in,"lba",0),true);
    if(!fs){ img_result_fail(out,IMG_EFORMAT,"FAT-MOUNT-FAIL",path,"目标 LBA 上不是合法 FAT32",NULL); return IMG_EFORMAT; }
    if(img_fat32_dir_exists(fs,path)){
        img_fat32_unmount(fs);
        img_result_str(out,"path",path); img_result_bool(out,"created",false);
        return IMG_OK;
    }
    if(img_fat32_path_exists(fs,path)){
        img_fat32_unmount(fs);
        img_result_fail(out,IMG_EEXISTS,"PATH-EXISTS-FILE",path,"同名文件已存在，不能当目录",NULL);
        return IMG_EEXISTS;
    }
    int rc=img_fat32_mkdir_p(fs,path);
    img_fat32_unmount(fs);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"FS-WRITE-FAIL",path,img_strerror(rc),"检查父路径/空间（FAT 根目录项有限）");
        return rc;
    }
    img_result_str(out,"path",path);
    img_result_bool(out,"created",true);
    return IMG_OK;
}
static const img_arg A_FATSTAT[] = {
    {"img",  IMG_A_STR, true, NULL, "qcow2 映像", NULL},
    {"lba",  IMG_A_U64, true, NULL, "FAT32 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true, NULL, "要查的路径（绝对路径）", NULL},
};
static int cmd_fat_stat(img_ctx *ctx, const img_args *in, img_result *out){
    const char *path=img_args_get(in,"path");
    if(!path||path[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","必须是 / 开头的绝对路径",NULL); return IMG_EINVAL; }
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ img_result_fail(out,IMG_EFORMAT,"QC-OPEN-FAIL",img_ctx_path(ctx),"不是合法 qcow2",NULL); return IMG_EFORMAT; }
    img_fat32_fs *fs=img_fat32_mount(q,img_args_u64(in,"lba",0),false);
    if(!fs){ img_result_fail(out,IMG_EFORMAT,"FAT-MOUNT-FAIL",path,"目标 LBA 上不是合法 FAT32",NULL); return IMG_EFORMAT; }
    img_fat32_stat_t st; int rc=img_fat32_stat(fs,path,&st);
    img_fat32_unmount(fs);
    if(rc==IMG_ENOENT){ img_result_fail(out,IMG_ENOENT,"PATH-NOT-FOUND",path,"路径不存在",NULL); return rc; }
    if(rc!=IMG_OK){ img_result_fail(out,rc,"FAT-LS-FAIL",path,img_strerror(rc),NULL); return rc; }
    img_result_str(out,"path",path);
    img_result_str(out,"type",st.is_dir?"dir":"file");
    img_result_bool(out,"dir",st.is_dir);
    img_result_u64(out,"size",st.size);
    img_result_u64(out,"alloc_bytes",st.alloc_bytes);
    img_result_u64(out,"ino",0);
    img_result_u64(out,"links",0);
    img_result_u64(out,"ctime",st.ctime);
    img_result_u64(out,"mtime",st.mtime);
    img_result_u64(out,"atime",st.atime);
    img_result_u64(out,"mode",st.is_dir?0755:0644);
    return IMG_OK;
}

/* ================= 压缩包只读（REQ-31）================= */
typedef struct { char *b; size_t cap, len; int n; uint64_t limit; bool trunc; } arc_emit_t;
static void arc_emit_cb(void *ud, const img_archive_ent *e){
    arc_emit_t *a=(arc_emit_t*)ud;
    if((uint64_t)a->n >= a->limit){ a->trunc=true; return; }
    char pj[700]; json_esc(pj,sizeof pj,e->path);
    const char *base=strrchr(e->path,'/'); base=base?base+1:e->path;
    char nj[300]; json_esc(nj,sizeof nj,base);
    char item[1100];
    int k=snprintf(item,sizeof item,
        "%s{\"name\":\"%s\",\"path\":\"%s\",\"dir\":%s,\"size\":%llu,\"mtime\":%llu",
        a->n?",":"",nj,pj,e->is_dir?"true":"false",
        (unsigned long long)e->size,(unsigned long long)e->mtime);
    if(e->is_dir) k+=snprintf(item+k,sizeof item-(size_t)k,",\"children_count\":%d",e->children_count);
    k+=snprintf(item+k,sizeof item-(size_t)k,"}");
    if(a->len+(size_t)k+2 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=k; a->n++; }
    else a->trunc=true;
}
static void arc_count_cb(void *ud, const img_archive_ent *e){
    (void)e; (*(uint64_t*)ud)++;
}
typedef struct { arc_emit_t *em; uint64_t skip; } arc_walk_t;
static void arc_walk_cb(void *ud, const img_archive_ent *e){
    arc_walk_t *w=(arc_walk_t*)ud;
    if(w->skip){ w->skip--; return; }
    arc_emit_cb(w->em,e);
}
static const img_arg A_ARCLS[] = {
    {"archive",    IMG_A_STR,  false, NULL,  "压缩包路径（给了 index-token 时可省略）", NULL},
    {"path",       IMG_A_STR,  false, "/",   "包内目录前缀（/ 开头；只列它下面的）", NULL},
    {"recursive",  IMG_A_BOOL, false, "false","递归列出（默认只列直接子项）", NULL},
    {"offset",     IMG_A_U64,  false, "0",   "分页起点", NULL},
    {"limit",      IMG_A_U64,  false, "500", "本页最多多少项（1~2000）", NULL},
    {"index-token",IMG_A_STR,  false, NULL,  "用上次返回的 index_token 续页（不重扫包；失效报 INDEX-EXPIRED）", NULL},
};
/* ================= archive.create（REQ-39）=================
 * 把宿主上选中的文件/目录打包成 zip / 7z / tar / tgz / iso。**rar 不能创建**（libarchive 只有读端）。
 * 三条政策：
 *   · 流式 + 中间进度（每个文件一条 item + 累计字节），大目录不会只剩转圈；
 *   · 取消/失败 → 临时文件删掉，dst 不存在（半截包最坑：用户会双击它）；
 *   · 写前先算总量（多走一遍源树），进度条的 total 才是真的。*/
/* REQ-42：--src 多源。App 的 JNI 入参是两根并行数组，同一个 key 只能出现一次，
 * 所以多源只能是"一个字符串里带分隔符"：逗号或换行，\, 转义；再加 --src-file 当逃生口。*/
typedef struct { char **v; int n, cap; } acsrc;
static void acsrc_free(acsrc *l){
    if(!l) return;
    for(int i=0;i<l->n;i++) free(l->v[i]);
    free(l->v); l->v=NULL; l->n=l->cap=0;
}
static int acsrc_add(acsrc *l, const char *s, size_t n){
    while(n && (*s==' '||*s=='\t')){ s++; n--; }
    while(n && (s[n-1]==' '||s[n-1]=='\t')) n--;
    if(!n) return IMG_OK;
    if(l->n>=1000) return IMG_EFULL;                 /* 上限 1000 条（App 建议值）*/
    if(l->n>=l->cap){
        int nc=l->cap?l->cap*2:16;
        char **nv=(char**)realloc(l->v,(size_t)nc*sizeof(char*));
        if(!nv) return IMG_ENOMEM;
        l->v=nv; l->cap=nc;
    }
    char *p=(char*)malloc(n+1);
    if(!p) return IMG_ENOMEM;
    memcpy(p,s,n); p[n]=0;
    l->v[l->n++]=p;
    return IMG_OK;
}
static int acsrc_split(acsrc *l, const char *str, char *err, size_t cap){
    if(!str || !*str) return IMG_OK;              /* 只给 --src-file 时 src 可以为空 */
    char buf[2048]; size_t o=0;
    for(const char *p=str; *p; p++){
        if(*p=='\\' && p[1]){                       /* \, → 字面逗号；\\x → x */
            if(o+1>=sizeof buf){ if(err&&cap) snprintf(err,cap,"单个路径超过 %zu 字节",sizeof buf-1); return IMG_ENAMETOOLONG; }
            buf[o++]=p[1]; p++; continue;
        }
        if(*p==',' || *p=='\n'){
            int rc=acsrc_add(l,buf,o);
            if(rc!=IMG_OK) return rc;
            o=0; continue;
        }
        if(o+1>=sizeof buf){ if(err&&cap) snprintf(err,cap,"单个路径超过 %zu 字节",sizeof buf-1); return IMG_ENAMETOOLONG; }
        buf[o++]=*p;
    }
    return acsrc_add(l,buf,o);
}
static int acsrc_from_file(acsrc *l, const char *path, char *err, size_t cap){
    FILE *f=fopen(path,"r");
    if(!f){ if(err&&cap) snprintf(err,cap,"清单文件无法打开：%s",path); return IMG_ENOENT; }
    char line[4096]; int rc=IMG_OK;
    while(fgets(line,sizeof line,f)){
        size_t n=strlen(line);
        while(n && (line[n-1]=='\n'||line[n-1]=='\r')) line[--n]=0;
        char *p=line; while(*p==' '||*p=='\t') p++;
        if(!*p || *p=='#') continue;
        rc=acsrc_add(l,p,strlen(p));
        if(rc!=IMG_OK) break;
    }
    fclose(f);
    if(rc==IMG_EFULL && err&&cap) snprintf(err,cap,"源太多（上限 1000 条）");
    return rc;
}
static void ac_srcs_free(acsrc *l, char **inn){
    if(inn){ for(int i=0;i<l->n;i++) free(inn[i]); free(inn); }
    acsrc_free(l);
}

typedef struct {
    bool     skip_trashed;
    char     exclude[16][128];
    int      n_exclude;
    int      max_depth;
    img_aw  *w;
    img_prog *prog;
    uint64_t io_done, io_total;
    uint64_t files, dirs, bytes_in;
    uint64_t skipped_trashed, skipped_excluded, skipped_links;
} accx;

/* 1=.trashed-* / 2=--exclude 命中（只对递归子项的 basename，与 copy 同口径）*/
static int acx_excluded(const accx *x, const char *name){
    if(x->skip_trashed && !strncmp(name,".trashed-",9)) return 1;
    for(int i=0;i<x->n_exclude;i++)
        if(fnmatch(x->exclude[i],name,0)==0) return 2;
    return 0;
}

static int acx_scan(accx *x, const char *host, int depth,
                    uint64_t *files, uint64_t *dirs, uint64_t *bytes, char *err, size_t cap){
    struct stat st;
    if(lstat(host,&st)!=0){ snprintf(err,cap,"读不到源：%s",host); return IMG_ENOENT; }
    if(S_ISLNK(st.st_mode)) return IMG_OK;                    /* 符号链接不跟随 */
    if(!S_ISDIR(st.st_mode)){
        (*files)++; *bytes += (uint64_t)st.st_size;
        return IMG_OK;
    }
    if(depth>x->max_depth){ snprintf(err,cap,"目录层数超过上限 %d：%s",x->max_depth,host); return IMG_ERANGE; }
    (*dirs)++;
    DIR *d=opendir(host);
    if(!d){ snprintf(err,cap,"目录无法打开：%s",host); return IMG_EIO; }
    struct dirent *de;
    int rc=IMG_OK;
    while((de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        int ex=acx_excluded(x,de->d_name);
        if(ex){ if(ex==1) x->skipped_trashed++; else x->skipped_excluded++; continue; }
        char c[4096]; snprintf(c,sizeof c,"%s/%s",host,de->d_name);
        rc=acx_scan(x,c,depth+1,files,dirs,bytes,err,cap);
        if(rc!=IMG_OK) break;
    }
    closedir(d);
    return rc;
}

static int acx_walk(accx *x, const char *host, const char *inner, int depth,
                    char *err, size_t cap){
    struct stat st;
    if(lstat(host,&st)!=0){ snprintf(err,cap,"读不到源：%s",host); return IMG_ENOENT; }
    if(S_ISLNK(st.st_mode)){ x->skipped_links++; return IMG_OK; }
    if(img_cancelled()) return IMG_ECANCELLED;
    if(!S_ISDIR(st.st_mode)){
        if(inner && *inner){
            int rc=img_aw_add_host_file(x->w,inner,host,(uint64_t)st.st_size,
                                        (uint64_t)st.st_mtime,x->prog,
                                        &x->io_done,x->io_total,err,cap);
            if(rc!=IMG_OK) return rc;
            x->files++; x->bytes_in += (uint64_t)st.st_size;
        }
        return IMG_OK;
    }
    if(depth>x->max_depth){ snprintf(err,cap,"目录层数超过上限 %d",x->max_depth); return IMG_ERANGE; }
    if(inner && *inner){
        int rc=img_aw_add_dir(x->w,inner,(uint64_t)st.st_mtime,err,cap);
        if(rc!=IMG_OK) return rc;
        x->dirs++;
    }
    DIR *d=opendir(host);
    if(!d){ snprintf(err,cap,"目录无法打开：%s",host); return IMG_EIO; }
    struct dirent *de;
    int rc=IMG_OK;
    while(rc==IMG_OK && (de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        int ex=acx_excluded(x,de->d_name);
        if(ex){ if(ex==1) x->skipped_trashed++; else x->skipped_excluded++; continue; }
        char c[4096]; snprintf(c,sizeof c,"%s/%s",host,de->d_name);
        char ei[2048];
        if(inner && *inner) snprintf(ei,sizeof ei,"%s/%s",inner,de->d_name);
        else                snprintf(ei,sizeof ei,"%s",de->d_name);
        rc=acx_walk(x,c,ei,depth+1,err,cap);
    }
    closedir(d);
    return rc;
}

/* 包内路径：给了 base 就取相对路径，否则取 basename。src==base → ""（把内容铺在根）*/
static int acx_inner_of(const char *src, const char *base, char *out, size_t cap){
    if(base && *base){
        size_t bl=strlen(base);
        while(bl>1 && base[bl-1]=='/') bl--;
        if(!strncmp(src,base,bl) && (src[bl]=='/' || !src[bl])){
            const char *rel=src+bl;
            while(*rel=='/') rel++;
            snprintf(out,cap,"%s",rel);
            return IMG_OK;
        }
        return IMG_EINVAL;
    }
    const char *b=strrchr(src,'/');
    snprintf(out,cap,"%s",b?b+1:src);
    return IMG_OK;
}

/* 扩展名判断（不区分大小写；只给错误提示用）*/
static bool ends_with_ci_ext(const char *s, const char *ext){
    size_t n=strlen(s), m=strlen(ext);
    return n>=m && !strcasecmp(s+n-m,ext);
}
static const img_arg A_ARCCREATE[] = {
    {"dst",       IMG_A_STR, true,  NULL,   "要生成的包路径（.zip/.7z/.tar/.tgz/.iso）", NULL},
    {"format",    IMG_A_ENUM,false, NULL,   "zip | 7z | tar | tgz | iso（不给则按 dst 扩展名）", NULL},
    {"src",       IMG_A_STR, false, NULL,   "要打包的宿主路径；多个用逗号分隔（路径里的逗号写 \\,）", NULL},
    {"src-file",  IMG_A_STR, false, NULL,   "清单文件：一行一条，# 开头注释，空行忽略（路径含逗号/换行时用它）", NULL},
    {"base",      IMG_A_STR, false, NULL,   "包内相对根（默认取每个 src 的父目录 → 只包名字本身）", NULL},
    {"level",     IMG_A_U64, false, "5",    "压缩等级 1~9（iso/tar 忽略）", NULL},
    {"exclude",   IMG_A_STR, false, NULL,   "逗号分隔的 basename 通配（*.tmp,.trashed-*），与 copy 同语义", NULL},
    {"skip-trashed",IMG_A_BOOL,false,"true","递归子项里跳过 .trashed-*（顶层 src 自身不跳）", NULL},
    {"conflict",  IMG_A_ENUM,false, "fail", "dst 已存在时：fail | overwrite | rename", NULL},
    {"iso-volume",IMG_A_STR, false, NULL,   "（iso）卷标", NULL},
    {"iso-joliet",IMG_A_BOOL,false, "true", "（iso）Joliet（Windows 靠它看到长名/中文名）", NULL},
    {"max-depth", IMG_A_U64, false, "32",   "目录递归深度上限", NULL},
};
static int cmd_archive_create(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_ARCHIVE) return feat_unavail(out,"libarchive（压缩包写）","重新编译时开启 IMG_WITH_ARCHIVE");
    const char *dst=img_args_get(in,"dst");
    const char *srcl=img_args_get(in,"src");
    const char *sfile0=img_args_get(in,"src-file");
    if(!dst||!*dst || ((!srcl||!*srcl) && (!sfile0||!*sfile0))){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD",(!dst||!*dst)?"dst":"src",
                        "dst 必填；src 与 src-file 至少给一个",NULL);
        return IMG_EINVAL;
    }

    /* 格式：先看 --format，再看扩展名；rar 明确拒绝（只读）*/
    img_aw_fmt f=IMG_AW_NONE;
    const char *fs=img_args_get(in,"format");
    if(fs && *fs){
        if(!strcasecmp(fs,"zip")) f=IMG_AW_ZIP;
        else if(!strcasecmp(fs,"7z")) f=IMG_AW_7Z;
        else if(!strcasecmp(fs,"tar")) f=IMG_AW_TAR;
        else if(!strcasecmp(fs,"tgz")||!strcasecmp(fs,"tar.gz")) f=IMG_AW_TGZ;
        else if(!strcasecmp(fs,"iso")) f=IMG_AW_ISO;
        else if(!strcasecmp(fs,"rar")){
            img_result_fail(out,IMG_EREADONLY,"ARCHIVE-FORMAT-READONLY","format",
                "RAR 只能读，不能创建（专有格式）","请用 --format=7z（7-Zip 能打开，压缩率同级）");
            return IMG_EREADONLY;
        }else{
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","format",
                "只支持 zip / 7z / tar / tgz / iso",
                "RAR 不能创建；需要苹果/Windows 通用就 zip，要压缩率就 7z");
            return IMG_EINVAL;
        }
    }else{
        f=img_aw_fmt_by_name(dst);
        if(f==IMG_AW_NONE && (ends_with_ci_ext(dst,".rar")||ends_with_ci_ext(dst,".cab"))){
            img_result_fail(out,IMG_EREADONLY,"ARCHIVE-FORMAT-READONLY","dst",
                "RAR/CAB 只能读，不能创建","请用 --format=7z 或 .zip");
            return IMG_EREADONLY;
        }
        if(f==IMG_AW_NONE){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst",
                "认不出格式：扩展名不是 .zip/.7z/.tar/.tgz/.iso","或者显式给 --format=");
            return IMG_EINVAL;
        }
    }
    if(!img_aw_writable(f)){
        img_result_fail(out,IMG_EREADONLY,"ARCHIVE-FORMAT-READONLY","dst","这个格式不能写",NULL);
        return IMG_EREADONLY;
    }

    /* dst 已存在：按 conflict 处理 */
    struct stat dstst;
    bool dst_exists=(stat(dst,&dstst)==0);
    const char *cfs=img_args_get(in,"conflict");
    int conflict=IMG_COPY_FAIL;
    if(cfs && *cfs){
        if(!strcasecmp(cfs,"overwrite")) conflict=IMG_COPY_OVERWRITE;
        else if(!strcasecmp(cfs,"rename")) conflict=IMG_COPY_RENAME;
        else if(!strcasecmp(cfs,"fail")) conflict=IMG_COPY_FAIL;
        else{
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","conflict","只能是 fail / overwrite / rename",NULL);
            return IMG_EINVAL;
        }
    }
    char real_dst[2048]; snprintf(real_dst,sizeof real_dst,"%s",dst);
    bool replaced=false;
    if(dst_exists){
        if(conflict==IMG_COPY_FAIL){
            img_result_fail(out,IMG_EEXISTS,"ARCHIVE-EXISTS","dst",
                "目标已存在",(f==IMG_AW_ISO)?"ISO 不能追加：换名字，或 --conflict=overwrite 整张重建"
                                            :"要替换就 --conflict=overwrite，要并存就 --conflict=rename");
            return IMG_EEXISTS;
        }
        if(conflict==IMG_COPY_RENAME){
            char dir[2048], base[256];
            const char *sl=strrchr(dst,'/');
            if(sl){ size_t n=(size_t)(sl-dst); if(n>=sizeof dir) n=sizeof dir-1; memcpy(dir,dst,n); dir[n]=0; if(!dir[0]) snprintf(dir,sizeof dir,"/"); snprintf(base,sizeof base,"%s",sl+1); }
            else { snprintf(dir,sizeof dir,"."); snprintf(base,sizeof base,"%s",dst); }
            const char *dot=strrchr(base,'.'); bool he=dot&&dot!=base&&dot[1];
            bool got=false;
            for(int i=1;i<1000 && !got;i++){
                char cand[512];
                if(he) snprintf(cand,sizeof cand,"%.*s (%d)%s",(int)(dot-base),base,i,dot);
                else   snprintf(cand,sizeof cand,"%s (%d)",base,i);
                char full[2048];
                snprintf(full,sizeof full,"%s/%s",dir,cand);
                struct stat t;
                if(stat(full,&t)!=0){ snprintf(real_dst,sizeof real_dst,"%s",full); got=true; }
            }
            if(!got){
                img_result_fail(out,IMG_EEXISTS,"ARCHIVE-EXISTS","dst","同名文件太多，想不出新名字",NULL);
                return IMG_EEXISTS;
            }
            dst_exists=false;
        }else{
            replaced=true;      /* overwrite：整包重建（iso 语义天然如此）*/
        }
    }

    /* 源列表（REQ-42）：--src 认逗号与换行，\, 转义；--src-file 一行一条（# 注释）。*/
    acsrc srcs; memset(&srcs,0,sizeof srcs);
    char err[400]="";
    int rc=acsrc_split(&srcs,srcl,err,sizeof err);
    const char *sfile=img_args_get(in,"src-file");
    if(rc==IMG_OK && sfile && *sfile) rc=acsrc_from_file(&srcs,sfile,err,sizeof err);
    if(rc==IMG_OK && srcs.n==0) rc=IMG_EINVAL;
    if(rc!=IMG_OK){
        acsrc_free(&srcs);
        img_result_fail(out,rc,(rc==IMG_ENAMETOOLONG)?"CMD-ARG-BAD":"CMD-ARG-BAD","src",
                        err[0]?err:(rc==IMG_EFULL?"源太多（上限 1000 条）":"没有有效的源路径"),NULL);
        return rc;
    }
    int nsrc=srcs.n;
    char **inn=(char**)calloc((size_t)nsrc,sizeof(char*));
    if(!inn){
        acsrc_free(&srcs);
        return tb_nomem(out,"archive.create 源列表");
    }

    accx x; memset(&x,0,sizeof x);
    x.skip_trashed=img_args_bool(in,"skip-trashed",true);
    x.max_depth=(int)img_args_u64(in,"max-depth",32);
    if(x.max_depth<=0 || x.max_depth>256) x.max_depth=32;
    const char *excl=img_args_get(in,"exclude");
    if(excl && *excl){
        char eb[2048]; snprintf(eb,sizeof eb,"%s",excl);
        char *save=NULL;
        for(char *tok=strtok_r(eb,",",&save); tok && x.n_exclude<16; tok=strtok_r(NULL,",",&save)){
            while(*tok==' ') tok++;
            if(*tok) snprintf(x.exclude[x.n_exclude++],128,"%s",tok);
        }
    }
    const char *base=img_args_get(in,"base");
    for(int i=0;i<nsrc;i++){
        struct stat st;
        /* REQ-43 §5：容器内路径（x.zip!:/a）明确拒绝，别让用户以为"源不存在"是打错字。
         * 真要支持 = 把 copy 的遍历搬进来（或落临时文件），收益不值这个分叉。*/
        if(strstr(srcs.v[i],"!:")){
            img_result_fail(out,IMG_EINVAL,"ARCHIVE-SRC-CONTAINER",srcs.v[i],
                "archive.create 的 --src 只接受宿主路径；压缩包/ISO/WIM 里的路径不在这里取",
                "先把它们 copy 到本地再打包；需要原生支持提 REQ（我按 copy 的引擎做）");
            ac_srcs_free(&srcs,inn);
            return IMG_EINVAL;
        }
        if(stat(srcs.v[i],&st)!=0){
            img_result_fail(out,IMG_ENOENT,"PATH-SRC-NOTFOUND",srcs.v[i],"源不存在",NULL);
            ac_srcs_free(&srcs,inn);
            return IMG_ENOENT;
        }
        char inner[2048];
        if(acx_inner_of(srcs.v[i],base,inner,sizeof inner)!=IMG_OK){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD",srcs.v[i],
                            "这个 src 不在 --base 下面",NULL);
            ac_srcs_free(&srcs,inn);
            return IMG_EINVAL;
        }
        for(int k=0;k<i;k++)
            if(inner[0] && !strcmp(inn[k],inner)){
                img_result_fail(out,IMG_EEXISTS,"ARCHIVE-NAME-DUP",srcs.v[i],
                                "两个源在包里会落到同一个名字（先用 --base 分开）",inner);
                ac_srcs_free(&srcs,inn);
                return IMG_EEXISTS;
            }
        inn[i]=strdup(inner[0]?inner:"");
        if(!inn[i]){
            img_result_fail(out,IMG_ENOMEM,"ARCHIVE-CREATE-FAIL",srcs.v[i],"内存不足",NULL);
            ac_srcs_free(&srcs,inn);
            return IMG_ENOMEM;
        }
    }

    /* 先量总量（进度条的 total 要是真的）*/
    uint64_t total_files=0,total_dirs=0,total_bytes=0;
    for(int i=0;i<nsrc;i++){
        int r2=acx_scan(&x,srcs.v[i],0,&total_files,&total_dirs,&total_bytes,err,sizeof err);
        if(r2!=IMG_OK){
            const char *code=(r2==IMG_ERANGE)?"CMD-ARG-BAD":"PATH-SRC-NOTFOUND";
            img_result_fail(out,r2,code,srcs.v[i],err[0]?err:"读源失败",NULL);
            ac_srcs_free(&srcs,inn);
            return r2;
        }
    }
    x.io_total=total_bytes;

    img_prog *prog=img_cmd_prog();
    if(prog){ img_prog_phase(prog,IMG_PH_COPY,"打包/压缩");
              img_prog_op(prog,IMG_OP_COMPRESS);
              img_prog_count(prog,0,total_files+total_dirs);
              img_prog_bytes(prog,0,total_bytes); }

    img_aw *w=NULL;
    rc=img_aw_begin(real_dst,f,(int)img_args_u64(in,"level",5),
                    img_args_get(in,"iso-volume"),img_args_bool(in,"iso-joliet",true),
                    false,NULL,0,false,&w,err,sizeof err);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"ARCHIVE-CREATE-FAIL",real_dst,err[0]?err:"建包失败",NULL);
        ac_srcs_free(&srcs,inn);
        return rc;
    }
    x.w=w; x.prog=prog;
    x.skipped_trashed=x.skipped_excluded=x.skipped_links=0;   /* 扫描时数过一遍，写入时重新数 */
    for(int i=0;i<nsrc && rc==IMG_OK;i++){
        rc=acx_walk(&x,srcs.v[i],inn[i],0,err,sizeof err);
        if(rc==IMG_OK && prog) img_prog_count(prog,(uint64_t)(x.files+x.dirs),total_files+total_dirs);
    }
    if(rc!=IMG_OK){
        img_aw_abort(w);                     /* ★ 半成品删掉：dst 不存在 */
        const char *code=(rc==IMG_ECANCELLED)?"CTL-CANCELLED":
                         (rc==IMG_ERANGE)?"CMD-ARG-BAD":"ARCHIVE-WRITE-FAIL";
        img_result_fail(out,rc,code,real_dst,err[0]?err:"打包失败（已删除半成品）",NULL);
        ac_srcs_free(&srcs,inn);
        return rc;
    }
    uint64_t outsz=0;
    rc=img_aw_commit(w,&outsz,err,sizeof err);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"ARCHIVE-WRITE-FAIL",real_dst,err[0]?err:"收尾失败（已删除半成品）",NULL);
        ac_srcs_free(&srcs,inn);
        return rc;
    }
    if(prog){ img_prog_bytes(prog,total_bytes,total_bytes); img_prog_done(prog,"打包完成"); }

    img_result_str(out,"dst",real_dst);
    img_result_str(out,"format",img_aw_fmt_name(f));
    img_result_u64(out,"level",img_args_u64(in,"level",5));
    img_result_u64(out,"files",x.files);
    img_result_u64(out,"dirs",x.dirs);
    img_result_u64(out,"sources",(uint64_t)nsrc);
    img_result_u64(out,"bytes_in",x.bytes_in);
    img_result_u64(out,"bytes_out",outsz);
    img_result_u64(out,"skipped_trashed",x.skipped_trashed);
    img_result_u64(out,"skipped_excluded",x.skipped_excluded);
    img_result_u64(out,"skipped_links",x.skipped_links);
    img_result_bool(out,"replaced",replaced);
    if(f==IMG_AW_ISO) img_result_bool(out,"udf",false);   /* 明说：只做 ISO9660 L3 + Joliet */
    ac_srcs_free(&srcs,inn);
    return IMG_OK;
}

static int cmd_archive_ls(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_ARCHIVE) return feat_unavail(out,"libarchive（压缩包只读）","重新编译时开启 IMG_WITH_ARCHIVE");
    const char *arp=img_args_get(in,"archive");
    const char *prefix=img_args_get(in,"path"); if(!prefix||!*prefix) prefix="/";
    const char *tok=img_args_get(in,"index-token");
    bool rec=img_args_bool(in,"recursive",false);
    uint64_t offset=img_args_u64(in,"offset",0);
    uint64_t limit=img_args_u64(in,"limit",500);
    if(limit==0) limit=500;
    if(limit>2000) limit=2000;
    if((!tok||!*tok) && (!arp||!*arp)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","archive","要么给 archive，要么给 index-token",NULL);
        return IMG_EINVAL;
    }

    img_archive_index *ix=NULL; bool fresh=false;
    char err[256]=""; int rc;
    if(tok && *tok){
        rc=img_archive_index_by_token(tok,&ix,err,sizeof err);
        if(rc!=IMG_OK){
            img_result_fail(out,IMG_ESTALE,"INDEX-EXPIRED",tok,
                            err[0]?err:"索引失效",
                            "从第 1 页重新索引（不带 --index-token 再调一次）");
            return rc;
        }
    } else {
        rc=img_archive_index_build(arp,&ix,err,sizeof err);
        if(rc!=IMG_OK){
            img_result_fail(out,rc,
                            (rc==IMG_ENOENT)?"ARCHIVE-NOTFOUND":"ARCHIVE-OPEN-FAIL",
                            arp,err[0]?err:"无法打开压缩包",NULL);
            return rc;
        }
        fresh=true;
    }

    uint64_t total=0;
    img_archive_index_walk(ix,prefix,rec,arc_count_cb,&total);

    enum { ARC_JB_CAP=262144 };
    char *jb=img_tbuf(IMG_TB_ISO_LIST,ARC_JB_CAP);
    if(!jb){ img_archive_index_release(ix); return tb_nomem(out,"archive.ls 条目缓冲"); }
    arc_emit_t em={jb,ARC_JB_CAP,0,0,limit,false};
    arc_walk_t ws={&em,offset};
    img_archive_index_walk(ix,prefix,rec,arc_walk_cb,&ws);
    if(!em.len){ memcpy(jb,"[]",2); em.len=2; jb[em.len]=0; }   /* ★ 缺 NUL 会把栈上的垃圾带进 JSON */
    else{
        if(em.len+2>=ARC_JB_CAP) em.len=ARC_JB_CAP-3;
        memmove(jb+1,jb,em.len); jb[0]='['; jb[em.len+1]=']'; em.len+=2; jb[em.len]=0;
    }

    img_result_str(out,"archive",arp?arp:"");
    img_result_str(out,"format",img_archive_index_format(ix));
    img_result_str(out,"path",prefix);
    img_result_u64(out,"total",total);
    img_result_u64(out,"offset",offset);
    img_result_u64(out,"limit",limit);
    img_result_u64(out,"count",(uint64_t)em.n);
    img_result_bool(out,"has_more", offset+(uint64_t)em.n < total);
    img_result_bool(out,"truncated",em.trunc || img_archive_index_truncated(ix));
    img_result_bool(out,"solid",img_archive_index_solid(ix));   /* REQ-50：RAR 固实包 */
    if(fresh) img_result_str(out,"index_token",img_archive_index_token(ix));
    else      img_result_str(out,"index_token",tok);
    img_result_raw(out,"entries",jb);
    img_archive_index_release(ix);
    return IMG_OK;
}

static const img_arg A_ARCEXT[] = {
    {"archive", IMG_A_STR, true, NULL, "压缩包路径", NULL},
    {"path",    IMG_A_STR, true, NULL, "包内路径（/ 开头；给目录就抽整棵子树）", NULL},
    {"dst",     IMG_A_STR, true, NULL, "本地目标（文件路径，或目录路径）", NULL},
};
static int cmd_archive_extract(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_ARCHIVE) return feat_unavail(out,"libarchive（压缩包只读）","重新编译时开启 IMG_WITH_ARCHIVE");
    const char *arp=img_args_get(in,"archive");
    const char *path=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    if(!arp||!path||!dst){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","archive/path/dst","三个都必填",NULL);
        return IMG_EINVAL;
    }
    char err[256]=""; uint64_t bytes=0; int files=0;
    img_span sp; img_span_prep(&sp,"archive","extract","从压缩包抽文件（只读）");
    img_span_kv_str(&sp,"path",path); img_span_kv_str(&sp,"dst",dst);
    img_span_begin(&sp);
    int rc=img_archive_extract(arp,path,dst,img_cmd_prog(),&bytes,&files,err,sizeof err);
    img_span_kv_u64(&sp,"written",bytes);
    img_span_end(&sp,rc);
    if(rc!=IMG_OK){
        if(rc==IMG_ENOENT)
            img_result_fail(out,rc,"ARCHIVE-ENTRY-NOTFOUND",path,err[0]?err:"包内没有这个路径",
                            "用 archive.ls --path=/ 看包内路径");
        else if(rc==IMG_ENOTSUP)
            img_result_fail(out,rc,"ARCHIVE-ENCRYPTED",path,err[0]?err:"包内条目加密",
                            "加密包暂不支持（不做密码参数）");
        else if(rc==IMG_ECANCELLED)
            img_result_fail(out,rc,"CMD-CANCELLED",path,"已取消（半成品已删）",NULL);
        else if(rc==IMG_ESTATE)
            img_result_fail(out,rc,"CMD-ARG-BAD",path,err[0]?err:"目标目录非空",NULL);
        else
            img_result_fail(out,rc,"ARCHIVE-READ-FAIL",path,err[0]?err:"抽取失败",
                            "确认包没损坏、磁盘还有空间");
        return rc;
    }
    img_result_str(out,"archive",arp);
    img_result_str(out,"path",path);
    img_result_str(out,"dst",dst);
    img_result_u64(out,"bytes",bytes);
    img_result_u64(out,"files",(uint64_t)files);
    char fmt[24]=""; img_archive_format(arp,fmt,sizeof fmt,NULL,0);
    if(fmt[0]) img_result_str(out,"format",fmt);
    return IMG_OK;
}

/* ---------- wim.put（REQ-29 C：WIM 里的文件 → 分区里的路径，0 临时）---------- */
static const img_arg A_WIMPUT[] = {
    {"img",     IMG_A_STR, true,  NULL, "目标 qcow2 映像", NULL},
    {"lba",     IMG_A_U64, true,  NULL, "目标分区起始 LBA（会按分区表算大小）", chk_lba},
    {"wim",     IMG_A_STR, true,  NULL, "WIM/ESD 路径；也可以直接给含它的 ISO", NULL},
    {"inner",   IMG_A_STR, false, NULL, "（ISO）内部路径，如 /sources/install.wim", NULL},
    {"index",   IMG_A_U64, false, "1",  "映像索引", NULL},
    {"path",    IMG_A_STR, true,  NULL, "WIM 内的文件路径（/ 开头）", NULL},
    {"dst-path",IMG_A_STR, true,  NULL, "目标盘内路径（如 /Recovery/WindowsRE/Winre.wim）", NULL},
    {"format",  IMG_A_BOOL,false, "false","先格式化目标分区为 NTFS（★会毁数据）", NULL},
};
static int cmd_wim_put(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G || !IMG_WITH_WIM)
        return feat_unavail(out,"libntfs-3g + wimlib（流式落盘）","重新编译时开启 IMG_WITH_NTFS3G/IMG_WITH_WIM");
    uint64_t lba=img_args_u64(in,"lba",0);
    const char *srcp=img_args_get(in,"wim"), *ip=img_args_get(in,"path");
    const char *dp=img_args_get(in,"dst-path");
    int index=(int)img_args_u64(in,"index",1);
    if(!srcp||!ip||!dp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","wim/path/dst-path","三个都必填",NULL); return IMG_EINVAL; }
    if(ip[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","WIM 内路径必须以 / 开头",NULL); return IMG_EINVAL; }
    if(dp[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","dst-path","盘内路径必须以 / 开头",NULL); return IMG_EINVAL; }

    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","该 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    char local[1024]="", used[256]="", why[400]="";
    wimsrc_kind wk=WIMSRC_PLAIN; uint64_t nb=0;
    rc=wim_resolve_source(srcp,img_args_get(in,"inner"),local,sizeof local,
                          used,sizeof used,&wk,&nb,why,sizeof why,img_cmd_prog());
    if(rc!=IMG_OK) return wim_src_fail(out,rc,srcp,why);
    if(img_args_bool(in,"format",false)){
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,lba,pb);
        o.label="wimport"; o.force=true; o.quick=true;
        int frc=img_ntfs_format(q,&o,img_cmd_prog());
        if(frc!=IMG_OK){
            wim_src_cleanup(local,wk);
            img_result_fail(out,frc,"FS-FORMAT-FAIL",img_ctx_path(ctx),
                            frc==IMG_ENOTSUP?"本构建未启用 NTFS 格式化":"格式化目标分区失败",NULL);
            return frc;
        }
    }
    char err[256]=""; uint64_t written=0;
    img_span sp; img_span_prep(&sp,"wim","put","WIM 里的文件直接写进分区（0 临时）");
    img_span_kv_str(&sp,"path",ip); img_span_kv_str(&sp,"dst",dp); img_span_begin(&sp);
    rc=img_put_wim_file(q,lba,pb,local,index,ip,dp,img_cmd_prog(),&written,err,sizeof err);
    img_span_kv_u64(&sp,"written",written);
    img_span_end(&sp,rc);
    wim_src_cleanup(local,wk);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,wim_fs_code(rc,1),ip,err[0]?err:"写入分区失败",NULL);
        return rc;
    }
    img_result_str(out,"path",ip);
    img_result_str(out,"dst_path",dp);
    img_result_u64(out,"bytes",written);
    img_result_u64(out,"lba",lba);
    if(wk!=WIMSRC_PLAIN){
        img_result_str(out,"container","iso");
        img_result_str(out,"inner_path",used);
        img_result_bool(out,"direct", wk==WIMSRC_VFD);
    }
    return IMG_OK;
}

/* ---------- recovery.status：WinRE 卡片的全部字段（只读；2026-11-04 解耦） ----------
 * ★ 口径（用户拍板）：三处找 Winre.wim，命中任一 → App 显示 WinRE 卡片；全无 → 隐藏。
 *   恢复分区在但没 wim → recovery_empty=true，不显示"修复"按钮，只在修复确认卡片里
 *   提示"恢复分区是空的，建议手动创建"（创建走独立 WinRE 流程，工具不自动建）。 */
static const img_arg A_RECSTATUS[] = {
    {"img",    IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
    {"os-lba", IMG_A_U64, false, NULL, "系统分区 LBA（缺省自动找 \\Windows\\System32\\winload.*）", NULL},
};
static int cmd_recovery_status(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（WinRE 检测）","重新编译时开启 IMG_WITH_NTFS3G");
    img_winre_status_t st; memset(&st,0,sizeof st);
    char err[256]="";
    rc=img_winre_status(q,t,img_args_u64(in,"os-lba",0),&st,err,sizeof err);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"PART-FS-BAD",img_ctx_path(ctx),err[0]?err:img_strerror(rc),NULL); return rc; }

    img_result_bool(out,"winre_present",st.winre_present);          /* 卡片显示与否就看它 */
    img_result_str(out,"winre_location",st.winre_location);
    img_result_str(out,"wim_path",st.wim_path);
    img_result_u64(out,"wim_partition_lba",st.wim_part_lba);
    img_result_u64(out,"wim_bytes",st.wim_bytes);
    char ver[40]="";
    if(st.winre_version_known) snprintf(ver,sizeof ver,"%d.%d.%d",st.winre_major,st.winre_minor,st.winre_build);
    img_result_str(out,"winre_version",ver);
    img_result_str(out,"winre_arch",st.winre_arch);
    img_result_bool(out,"reagent_present",st.reagent_present);
    img_result_bool(out,"bootsdi_present",st.bootsdi_present);

    img_result_bool(out,"recovery_partition_present",st.recovery_partition_present);
    img_result_u64(out,"recovery_partition_lba",st.rec_lba);
    img_result_u64(out,"recovery_partition_bytes",st.rec_bytes);
    img_result_bool(out,"recovery_partition_empty",st.recovery_empty);
    char attr[24]="";
    if(st.rec_is_gpt) snprintf(attr,sizeof attr,"0x%016llx",(unsigned long long)st.rec_attr);
    img_result_str(out,"recovery_partition_attr",attr);
    img_result_bool(out,"recovery_partition_attr_ok",st.rec_attr_ok);

    img_result_bool(out,"os_found",st.os_found);
    img_result_u64(out,"os_lba",st.os_lba);
    img_result_str(out,"os_version",st.os_version);
    if(st.note[0]) img_result_str(out,"note",st.note);
    return IMG_OK;
}

/* ---------- recovery.populate / recovery.populate-system ---------- */
static const img_arg A_RECPOP[] = {
    {"img",        IMG_A_STR, true,  NULL,    "映像路径（qcow2）", NULL},
    {"rec-lba",    IMG_A_U64, true,  NULL,    "恢复分区起始 LBA（UEFI：独立恢复分区；BIOS：填系统分区）", chk_lba},
    {"winre-src",  IMG_A_STR, true,  NULL,    "WinRE 来源 WIM/ESD（也可以直接给 ISO，内部自动找 install.wim）", NULL},
    {"inner",      IMG_A_STR, false, NULL,    "（ISO）内部路径，如 /x64/sources/install.wim", NULL},
    {"winre-index",IMG_A_U64, false, "1",     "WIM/ESD 里的映像索引", NULL},
    {"winre-path", IMG_A_STR, false, NULL,    "WIM 内路径（缺省按标准两处找；self 时不许给）", NULL},
    {"winre-self", IMG_A_ENUM,false, "auto",  "winre-src 是【裸 winre.wim】：auto(自动认) | true | false", NULL},
    {"winre-dry-run", IMG_A_BOOL,false,"false","只回答能不能写、WinRE 在哪、多大（不格式化、不写盘）", NULL},
    {"stream",      IMG_A_BOOL,false,"true", "流式解压直写（0 临时文件；false=先解到临时文件，占盘但更快）", NULL},
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

    /* ★ REQ-24/25：WinRE 的来源由工具自己认（裸 winre.wim / 普通 WIM / ESD / ISO），
     *   ISO 里还会把候选逐个试一遍 —— App 不再需要"先探再决定"。*/
    winre_pick_t pk; char pwhy[400]="";
    int prc=winre_pick(src,img_args_get(in,"inner"),idx,true,&pk,pwhy,sizeof pwhy,img_cmd_prog());
    if(prc!=IMG_OK){
        winre_pick_free(&pk);
        if(prc==IMG_ENOENT||prc==IMG_EFORMAT){
            img_result_fail(out,prc,"WIM-NOTFOUND",src,pwhy[0]?pwhy:"不是 WIM/ESD/ISO，或文件无法打开",NULL);
            return prc;
        }
        img_result_fail(out,prc,"IMG-READ-FAIL",src,pwhy[0]?pwhy:"定位 WinRE 时工具出错","这是工具侧问题，可直接重试");
        return prc;
    }
    if(!pk.found){
        winre_pick_free(&pk);
        img_result_fail(out,IMG_ENOENT,"WINRE-NOTFOUND",src,
            pwhy[0]?pwhy:"这个文件里没有 Winre.wim",
            "换一份含 WinRE 的映像，或直接选裸 winre.wim");
        if(pk.n_tried){
            char *tb=img_tbuf(IMG_TB_ITEMS2,strlen(pk.tried)+1);
            if(tb){ snprintf(tb,strlen(pk.tried)+1,"%s",pk.tried); img_result_raw(out,"candidates_tried",tb); }
        }
        return IMG_ENOENT;
    }
    /* --winre-self 与实际情况对账（互相矛盾就明确报，不猜）*/
    const char *ws=img_args_get(in,"winre-self");
    if(ws && *ws && strcasecmp(ws,"auto")){
        bool want=!strcasecmp(ws,"true") || !strcmp(ws,"1");
        if(want!=pk.self){
            winre_pick_free(&pk);
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","winre-self",
                want?"指定了 winre-self=true，但这份文件是普通映像（里面才有 Winre.wim）"
                     :"这份文件本身就是一份 winre.wim，但 winre-self=false",
                want?"去掉 --winre-self，或改成 auto":"去掉 --winre-self=false（用 auto 即可）");
            return IMG_EINVAL;
        }
    }
    if(pk.self && wpath && *wpath){
        winre_pick_free(&pk);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","winre-path",
            "winre-src 本身就是 winre.wim，此时不能再用 --winre-path 指内部路径",
            "去掉 --winre-path（self 时整个文件就是 payload）");
        return IMG_EINVAL;
    }
    /* dry-run：只回答"能不能写、在哪、多大"*/
    if(img_args_bool(in,"winre-dry-run",false)){
        img_result_bool(out,"dry_run",true);
        img_result_bool(out,"can_populate",true);
        img_result_bool(out,"self",pk.self);
        img_result_u64(out,"winre_index",(uint64_t)pk.index);
        img_result_u64(out,"winre_bytes",pk.size_bytes);
        if(pk.inner_path[0]) img_result_str(out,"iso_inner_path",pk.inner_path);
        if(pk.internal[0])   img_result_str(out,"internal_path",pk.internal);
        winre_pick_free(&pk);
        return IMG_OK;
    }
    const char *internal = pk.self ? "" : (wpath&&*wpath ? wpath : pk.internal);
    char err[256]=""; uint64_t wb=0;
    rc=img_recovery_populate(q,lba,pb,pk.src_resolved,pk.self?1:pk.index,internal,sdi,do_fmt,label,
                             bios_layout,img_cmd_prog(),&wb,err,sizeof err);
    wimsrc_kind rsk=pk.kind; uint64_t rbytes=pk.size_bytes;
    char rinner[256]; snprintf(rinner,sizeof rinner,"%s",pk.inner_path);
    bool was_self=pk.self; int pk_index=pk.index;
    winre_pick_free(&pk);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"REC-VERIFY-FAIL",src,err[0]?err:"填充失败",NULL); return rc; }

    /* ★ REQ-37 ①：把恢复分区的 GPT 属性写成微软官方值 0x8000000000000001
     *   （bit0 平台必需 + bit63 不分配盘符）。真机上缺 bit0 时 boot.check 红、
     *   reagentc 可能拒认恢复分区。
     *   · 已经是对的就不写（幂等），attr_set=false 但不算失败；
     *   · MBR 没有属性位 → attr_supported=false，不报错、不改任何东西；
     *   · 写失败不影响主流程，只回 attr_set=false + attr_error。*/
    bool attr_ok=false;
    bool attr_supported=(t->table==IMG_TABLE_GPT);
    char attr_before[24]="", attr_after[24]="", attr_err[160]="";
    uint64_t want=IMG_GPT_ATTR_RECOVERY_OFFICIAL;
    int ridx=-1;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){ ridx=i; break; }
    if(attr_supported && ridx>=0){
        snprintf(attr_before,sizeof attr_before,"0x%016llx",(unsigned long long)t->parts[ridx].attributes);
        snprintf(attr_after ,sizeof attr_after ,"%s",attr_before);
        if(set_attr && t->parts[ridx].attributes!=want){
            uint64_t old=t->parts[ridx].attributes;
            t->parts[ridx].attributes=want;
            int wr=img_parttable_write(q,t);
            if(wr==IMG_OK){
                attr_ok=true;
                snprintf(attr_after,sizeof attr_after,"0x%016llx",(unsigned long long)want);
            }else{
                t->parts[ridx].attributes=old;      /* 回滚内存里的值，别让后面的判断被带偏 */
                snprintf(attr_err,sizeof attr_err,"写回分区表失败：%s",img_strerror(wr));
            }
        }else if(set_attr){
            attr_ok=true;                            /* 已经是官方值：幂等，不重复写 */
        }
    }else if(attr_supported){
        snprintf(attr_err,sizeof attr_err,"分区表里找不到 LBA %llu",(unsigned long long)lba);
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
    img_result_bool(out,"self",was_self);
    img_result_u64(out,"winre_index",(uint64_t)pk_index);
    if(rsk!=WIMSRC_PLAIN){
        img_result_str(out,"container","iso");
        img_result_str(out,"inner_path",rinner);
        img_result_bool(out,"direct", rsk==WIMSRC_VFD);
        if(rsk==WIMSRC_EXTRACT){
            img_result_bool(out,"extracted",true);
            img_result_u64(out,"extracted_bytes",rbytes);
        }
    }
    img_result_bool(out,"gpt_attr_set",attr_ok);
    img_result_bool(out,"attr_supported",attr_supported);
    if(attr_before[0]) img_result_str(out,"attr_before",attr_before);
    if(attr_after[0])  img_result_str(out,"attr_after",attr_after);
    img_result_bool(out,"attr_set", attr_ok && strcmp(attr_before,attr_after)!=0);
    if(attr_err[0])    img_result_str(out,"attr_error",attr_err);
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
    {"img",         IMG_A_STR,  true,  NULL,    "映像路径（qcow2）", NULL},
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

/* 自动找系统分区：按大小逐个试，能读到 \\Windows\\System32\\winload.* 的才是
 * （不能只挑"最大的 NTFS 分区"：Win11 盘上数据分区常比系统分区大）。
 * bcd.create / boot.populate 共用。 */
static uint64_t auto_sys_lba(img_qcow2 *q, img_parttable_t *t, bool uefi){
    uint64_t sys_lba=0;
    {
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
    return sys_lba;
}

/* ---------- 共用：定 BCD 代 + 分区表拦截（bcd.create / boot.populate）----------
 * 返回值：IMG_OK / IMG_EINVAL（不支持，--force 也跳不过）/ IMG_ENOTSUP（要用户确认）。*/
static int boot_gate(img_qcow2 *q, img_parttable_t *t, uint64_t sys_lba, bool uefi,
                     bool force, int fam_in, int *out_fam, const char **out_why,
                     img_plan_arch *out_arch, img_result *out)
{
    int fam=fam_in; const char *why="given";
    img_plan_arch oarch=IMG_PLAN_ARCH_UNKNOWN;

    /* ★ 读一次盘里的"真相"（版本 + 位数），既用来定 BCD 代，也用来做分区表拦截 */
    int omaj=0, omin=0, obld=0;
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
              why="win.version";
          }
      } else if(fam==0){ fam=(int)IMG_BCD_FAM_MODERN; why="fallback(无法读取版本，按 modern)"; }
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
        if(pl.verdict==IMG_PLAN_NEEDS_CONFIRM && !force){
            img_result_fail(out,IMG_ENOTSUP,"BOOT-FORMAT-NEEDS-CONFIRM",uefi?"table=gpt":"table=mbr",
                            pl.detail,"用户确认后带 --force=true 再调一次");
            return IMG_ENOTSUP;
        }
    }


    if(out_fam)  *out_fam=fam;
    if(out_why)  *out_why=why;
    if(out_arch) *out_arch=oarch;
    return IMG_OK;
}

/* ---------- 共用：生成 BCD 并写进引导分区（uefi→ESP；bios→引导分区 \Boot\BCD）---------- */
static int bcd_put(img_qcow2 *q, img_parttable_t *t, bool uefi, uint64_t sys_lba, uint64_t boot_lba,
                   const char *desc, const char *locale, int timeout, int fam, bool keep_rec,
                   const char *outp, uint64_t *out_bytes, img_result *out)
{
    img_bcd_create_req_t rq; memset(&rq,0,sizeof rq);
    rq.uefi=uefi; rq.family=(img_bcd_family)fam;
    rq.desc=desc; rq.locale=locale; rq.timeout=timeout; rq.keep_recovery=keep_rec;
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
    int fd=mkstemp(bcdtmp);
    if(fd<0){ img_result_fail(out,IMG_EIO,"BCD-BUILD-FAIL",NULL,"建不了临时文件",NULL); return IMG_EIO; }
    close(fd);
    char err[256]="";
    int rc=img_bcd_create_file(&rq,bcdtmp,err,sizeof err);
    if(rc!=IMG_OK){ unlink(bcdtmp); img_result_fail(out,rc,"BCD-BUILD-FAIL","internal-template",err[0]?err:"生成失败",NULL); return rc; }
    struct stat st; uint64_t bsz=0;
    if(stat(bcdtmp,&st)==0) bsz=(uint64_t)st.st_size;

    /* ---- 写进盘：BIOS → 引导分区:\Boot\BCD；UEFI → ESP:\EFI\Microsoft\Boot\BCD ---- */
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
    if(outp && *outp){
        FILE *fp2=fopen(outp,"wb");
        if(fp2){ FILE *s2=fopen(bcdtmp,"rb"); if(s2){ char b[8192]; size_t r; while((r=fread(b,1,sizeof b,s2))>0) fwrite(b,1,r,fp2); fclose(s2);} fclose(fp2); }
    }
    unlink(bcdtmp);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",uefi?"/EFI/Microsoft/Boot/BCD":"/Boot/BCD","无法写入引导分区",NULL); return rc; }
    if(out_bytes) *out_bytes=bsz;
    return IMG_OK;
}

static int cmd_bcd_create(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    if(t->table==IMG_TABLE_NONE){ img_result_fail(out,IMG_EINVAL,"PART-READ-FAIL",NULL,"该映像还没有分区表","先 imglayout / img.create"); return IMG_EINVAL; }

    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="auto";
    bool uefi = (t->table==IMG_TABLE_GPT);
    if(!strcasecmp(tb,"mbr")) uefi=false;
    else if(!strcasecmp(tb,"gpt")) uefi=true;
    else if(strcasecmp(tb,"auto")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","table","只接受 auto|mbr|gpt",NULL); return IMG_EINVAL; }

    /* ---- 选系统分区 / 引导分区 ---- */
    uint64_t sys_lba=img_args_u64(in,"sys-lba",0);
    if(!sys_lba) sys_lba=auto_sys_lba(q,t,uefi);
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

    int fam=0; const char *fs=img_args_get(in,"family");
    if(fs && !strcasecmp(fs,"legacy")) fam=(int)IMG_BCD_FAM_LEGACY;
    else if(fs && !strcasecmp(fs,"modern")) fam=(int)IMG_BCD_FAM_MODERN;
    else if(fs && strcasecmp(fs,"auto")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","family","只接受 auto|legacy|modern",NULL); return IMG_EINVAL; }
    const char *fam_why="given"; img_plan_arch oarch=IMG_PLAN_ARCH_UNKNOWN;
    rc=boot_gate(q,t,sys_lba,uefi,img_args_bool(in,"force",false),fam,&fam,&fam_why,&oarch,out);
    if(rc!=IMG_OK) return rc;
    const char *outp=img_args_get(in,"out");
    uint64_t bsz=0;
    rc=bcd_put(q,t,uefi,sys_lba,boot_lba,img_args_get(in,"desc"),img_args_get(in,"locale"),
               (int)img_args_u64(in,"timeout",5),fam,img_args_bool(in,"keep-recovery",false),
               outp,&bsz,out);
    if(rc!=IMG_OK) return rc;
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




/* ================= boot.populate（REQ-20261026-20）=================
 * 一条命令把"系统文件以外"的引导链铺完（App 以前自己拼 ntfs.export + fat.put）：
 *   UEFI：ESP 没文件系统 → 格 FAT32 → bootmgfw.efi（+ \EFI\Boot\bootx64.efi）
 *         → \Windows\Boot\Fonts\* → boot.sdi → BCD
 *   BIOS：SR/活动分区没文件系统 → 格 NTFS → 活动标志 → bootmgr
 *         → MBR 引导码（内置模板）+ VBR 引导码（内置模板）→ BCD
 * ★ 幂等：已有文件系统的分区【一律不格】；文件覆盖写。
 * ★ 末尾跑一次 boot.check，把 required_fail 带回来（App 不用再调一次）。
 */
static const img_arg A_BOOTPOP[] = {
    {"img",     IMG_A_STR,  true,  NULL,    "映像路径（qcow2）", NULL},
    {"table",   IMG_A_ENUM, false, "auto",  "引导方式：auto(按分区表) | mbr | gpt", NULL},
    {"sys-lba", IMG_A_U64,  false, NULL,    "系统分区 LBA（缺省=自动找带 winload 的分区）", NULL},
    {"esp-lba", IMG_A_U64,  false, NULL,    "（UEFI）ESP LBA（缺省=EFI 类型分区）", NULL},
    {"sr-lba",  IMG_A_U64,  false, NULL,    "（BIOS）系统保留/活动分区 LBA（缺省=活动分区）", NULL},
    {"force",   IMG_A_BOOL, false, "false", "跳过分区表兼容性拦截（App 弹框确认后给 true）", NULL},
    {"fonts",   IMG_A_BOOL, false, "true",  "（UEFI）拷 \\Windows\\Boot\\Fonts（中文菜单是方块还是字）", NULL},
    {"desc",    IMG_A_STR,  false, "Windows", "启动菜单里显示的名字", NULL},
    {"locale",  IMG_A_STR,  false, "zh-CN", "BCD locale", NULL},
    {"timeout", IMG_A_U64,  false, "5",     "菜单等待秒数", NULL},
    {"efi-fallback", IMG_A_ENUM, false, "auto",
     "\\EFI\\Boot 兜底：auto(不存在→写；是我们的→覆盖；别人的→不覆盖并报错) | force(一律覆盖) | skip(不写)",
     NULL},
};

/* 逐项结果（files[]）拼 JSON：img_result_raw 只存指针，所以必须是静态缓冲 */
typedef struct { char *b; size_t cap, len; int n, n_missing, n_failed; } bpfiles_t;

static void bp_esc(char *d, size_t cap, const char *s){
    size_t o=0;
    for(const unsigned char *p=(const unsigned char*)(s?s:""); *p && o+8<cap; p++){
        if(*p=='"' || *p=='\\'){ d[o++]='\\'; d[o++]=(char)*p; }
        else if(*p<0x20){ o+=(size_t)snprintf(d+o,cap-o,"\\u%04x",*p); }
        else d[o++]=(char)*p;
    }
    if(cap) d[o<cap?o:cap-1]=0;
}
static void bp_add(bpfiles_t *f, const char *dst, const char *src, uint64_t bytes,
                   bool ok, const char *why){
    if(!f || f->len+800>=f->cap) return;
    char ed[512], es[512], ew[300];
    bp_esc(ed,sizeof ed,dst); bp_esc(es,sizeof es,src); bp_esc(ew,sizeof ew,why);
    f->len += (size_t)snprintf(f->b+f->len, f->cap-f->len,
        "%s{\"path\":\"%s\",\"src\":\"%s\",\"bytes\":%llu,\"ok\":%s,\"error\":\"%s\"}",
        f->n?",":"", ed, es, (unsigned long long)bytes, ok?"true":"false", ew);
    f->n++;
}
/* NTFS → FAT32 拷一个文件（可一次写两个目标路径；1.2 MB 的 bootmgfw 也走内存，不留临时文件） */
static void bp_put(img_ntfs_fs *sfs, const char *src, img_fat32_fs *efs,
                   const char *d1, const char *d2, bool required, bpfiles_t *f){
    void *buf=NULL; size_t len=0;
    int rc=img_ntfs_read_file(sfs,src,&buf,&len);
    if(rc!=IMG_OK){
        bp_add(f,d1,src,0,false,"系统分区里没有这个文件");
        if(required) f->n_missing++;
        return;
    }
    int r1=img_fat32_put_data(efs,buf,len,d1);
    bp_add(f,d1,src,len,r1==IMG_OK, r1?img_strerror(r1):NULL);
    if(r1!=IMG_OK && required) f->n_failed++;
    if(d2 && *d2){
        int r2=img_fat32_put_data(efs,buf,len,d2);
        bp_add(f,d2,src,len,r2==IMG_OK, r2?img_strerror(r2):NULL);
        if(r2!=IMG_OK && required) f->n_failed++;
    }
    free(buf);
}
typedef struct { img_ntfs_fs *sfs; img_fat32_fs *efs; bpfiles_t *f; } bp_font_ctx;
static void bp_font_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    (void)size;
    bp_font_ctx *c=(bp_font_ctx*)ud;
    if(is_dir || !name || !*name) return;
    char sp[520], dp[520];
    snprintf(sp,sizeof sp,"/Windows/Boot/Fonts/%.200s",name);
    snprintf(dp,sizeof dp,"/EFI/Microsoft/Boot/Fonts/%.200s",name);
    bp_put(c->sfs,sp,c->efs,dp,NULL,false,c->f);      /* 字体：能拷多少拷多少 */
}
/* NTFS → NTFS 拷一个文件（BIOS 的 bootmgr：系统分区 → 引导分区根；同区时拷进自己） */
static void bp_put_ntfs(img_ntfs_fs *sfs, const char *src, img_ntfs_fs *bfs, const char *dst,
                        bool required, bpfiles_t *f){
    void *buf=NULL; size_t len=0;
    int rc=img_ntfs_read_file(sfs,src,&buf,&len);
    if(rc!=IMG_OK){
        bp_add(f,dst,src,0,false,"系统分区里没有这个文件");
        if(required) f->n_missing++;
        return;
    }
    int r1=img_ntfs_put_file_mem(bfs,buf,len,dst);
    bp_add(f,dst,src,len,r1==IMG_OK, r1?img_strerror(r1):NULL);
    if(r1!=IMG_OK && required) f->n_failed++;
    free(buf);
}
static int pt_idx_of(const img_parttable_t *t, uint64_t lba){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba) return i;
    return -1;
}

static int cmd_boot_populate(img_ctx *ctx, const img_args *in, img_result *out){
    if(!IMG_WITH_NTFS3G) return feat_unavail(out,"libntfs-3g（引导文件搬运）",
                         "重新编译时开启 IMG_WITH_NTFS3G");
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    if(t->table==IMG_TABLE_NONE){
        img_result_fail(out,IMG_EINVAL,"PART-READ-FAIL",NULL,"该映像还没有分区表","用 img.create 建布局");
        return IMG_EINVAL;
    }

    const char *tb=img_args_get(in,"table"); if(!tb||!*tb) tb="auto";
    bool uefi=(t->table==IMG_TABLE_GPT);
    if(!strcasecmp(tb,"mbr")) uefi=false;
    else if(!strcasecmp(tb,"gpt")) uefi=true;
    else if(strcasecmp(tb,"auto")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","table","只接受 auto|mbr|gpt",NULL);
        return IMG_EINVAL;
    }

    /* ---- 系统分区 ---- */
    uint64_t sys_lba=img_args_u64(in,"sys-lba",0);
    if(!sys_lba) sys_lba=auto_sys_lba(q,t,uefi);
    if(!sys_lba || !pb_of(t,sys_lba)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sys-lba",
            "自动未找到系统分区，请显式给 --sys-lba（先 part.show 看）","先 wim.apply 把系统写入");
        return IMG_EINVAL;
    }

    /* ---- 引导分区：UEFI→ESP，BIOS→系统保留/活动 ---- */
    uint64_t boot_lba=0;
    if(uefi){
        boot_lba=img_args_u64(in,"esp-lba",0);
        if(!boot_lba)
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t->parts[i].used && !strncasecmp(t->parts[i].type_guid,"c12a7328",8))
                    { boot_lba=t->parts[i].first_lba; break; }
        if(!boot_lba){
            img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","esp-lba",
                "没有 ESP 分区：UEFI 盘必须先建 ESP","img.create --esp-mb=200");
            return IMG_EINVAL;
        }
    } else {
        boot_lba=img_args_u64(in,"sr-lba",0);
        if(!boot_lba)
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t->parts[i].used && !t->parts[i].logical && (t->parts[i].status&0x80))
                    { boot_lba=t->parts[i].first_lba; break; }
        if(!boot_lba) boot_lba=sys_lba;          /* 单分区布局 */
    }

    /* ---- 分区表拦截（和 bcd.create 同一套；32 位 + GPT 这里就拒）---- */
    int fam=0; const char *fam_why="given"; img_plan_arch arch=IMG_PLAN_ARCH_UNKNOWN;
    rc=boot_gate(q,t,sys_lba,uefi,img_args_bool(in,"force",false),0,&fam,&fam_why,&arch,out);
    if(rc!=IMG_OK) return rc;

    static char files_json[8192];
    bpfiles_t fl; fl.b=files_json; fl.cap=sizeof files_json; fl.len=1; fl.n=0; fl.n_missing=0; fl.n_failed=0;
    files_json[0]='['; files_json[1]=0;          /* files[] 是数组，raw 出去要自带括号 */
    bool did_format=false;
    uint64_t bcd_bytes=0;

    if(uefi){
        /* ① ESP 文件系统：有就不动，没有才格 */
        img_fat32_fs *efs=img_fat32_mount(q,boot_lba,true);
        if(!efs){
            img_fat32_opts_t o={0};
            o.partition_lba=boot_lba; o.partition_bytes=pb_of(t,boot_lba);
            o.label="ESP"; o.hidden_sectors=(uint32_t)boot_lba; o.num_fats=2; o.active_boot=true;
            img_fat32_layout_t L; memset(&L,0,sizeof L);
            int frc=img_fat32_format(q,&o,&L,img_cmd_prog());
            if(frc!=IMG_OK){
                img_result_fail(out,frc,"FAT-FORMAT-FAIL","esp",
                    "ESP 没有文件系统，自动格式化也失败了","先 fs.format --lba=<esp> --fs=fat32 手动来一次");
                return frc;
            }
            did_format=true;
            efs=img_fat32_mount(q,boot_lba,true);
            if(!efs){
                img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","esp","格式化后仍挂不上 FAT32",NULL);
                return IMG_ESTATE;
            }
        }
        img_fat32_mkdir_p(efs,"/EFI");
        img_fat32_mkdir_p(efs,"/EFI/Microsoft");
        img_fat32_mkdir_p(efs,"/EFI/Microsoft/Boot");
        img_fat32_mkdir_p(efs,"/EFI/Boot");

        /* ② 系统分区（只读就够，但 rw 挂载无害） */
        img_ntfs_fs *sfs=img_ntfs_mount_rw(q,sys_lba,pb_of(t,sys_lba));
        if(!sfs){
            img_fat32_unmount(efs);
            img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","sys","系统分区不是合法 NTFS","先 wim.apply / fs.format");
            return IMG_ESTATE;
        }

        /* ③ bootmgfw：主路径必写；\EFI\Boot 兜底按 --efi-fallback 策略。
         * ★ 2026-11-04 用户拍板：别人的 \EFI\Boot\BOOTX64.EFI（GRUB/黑苹果等）不能覆盖，
         *   不覆盖时报 efi_fallback_conflict，由 App 弹框问用户。 */
        const char *fbmode=img_args_get(in,"efi-fallback"); if(!fbmode||!*fbmode) fbmode="auto";
        const char *fb = (arch==IMG_PLAN_ARCH_ARM64) ? "/EFI/Boot/bootaa64.efi"
                        : (arch==IMG_PLAN_ARCH_X86)  ? "/EFI/Boot/bootia32.efi"
                                                     : "/EFI/Boot/bootx64.efi";
        bool fb_skip = !strcasecmp(fbmode,"skip");
        bool fb_force= !strcasecmp(fbmode,"force");
        if(!fb_skip){
            void *mine=NULL; size_t minen=0;
            bool have_mine=(img_ntfs_read_file(sfs,"/Windows/Boot/EFI/bootmgfw.efi",&mine,&minen)==IMG_OK);
            void *cur=NULL; size_t curn=0;
            bool have_cur=(img_fat32_read_file(efs,fb,&cur,&curn)==IMG_OK && curn);
            bool same = have_cur && have_mine && curn==minen && memcmp(cur,mine,minen)==0;
            free(cur);
            if(have_cur && !same && !fb_force){
                /* 别人的：不覆盖，记一笔 -->
                 * 用 bp_add 报 ok=false 但【不计 missing/failed】，走专门的 conflict 输出 */
                bp_add(&fl,fb,"\Windows\Boot\EFI\bootmgfw.efi",0,true,
                       "已存在且不是 Windows 的 bootmgr，按策略不覆盖（efi_fallback_conflict=true）");
                img_result_bool(out,"efi_fallback_conflict",true);
                img_result_bool(out,"efi_fallback_written",false);
            } else {
                bp_put(sfs,"/Windows/Boot/EFI/bootmgfw.efi",efs,
                       "/EFI/Microsoft/Boot/bootmgfw.efi",fb,true,&fl);
                img_result_bool(out,"efi_fallback_conflict",false);
                img_result_bool(out,"efi_fallback_written",true);
            }
            free(mine);
        } else {
            bp_put(sfs,"/Windows/Boot/EFI/bootmgfw.efi",efs,
                   "/EFI/Microsoft/Boot/bootmgfw.efi",NULL,true,&fl);
            img_result_bool(out,"efi_fallback_written",false);
        }

        /* ④ 字体（中文菜单不出方块）*/
        if(img_args_bool(in,"fonts",true)){
            if(img_ntfs_path_exists(sfs,"/Windows/Boot/Fonts")){
                img_fat32_mkdir_p(efs,"/EFI/Microsoft/Boot/Fonts");
                bp_font_ctx fc={sfs,efs,&fl};
                img_ntfs_list_dir(sfs,"/Windows/Boot/Fonts",bp_font_cb,&fc);
            } else {
                bp_add(&fl,"/EFI/Microsoft/Boot/Fonts","/Windows/Boot/Fonts",0,false,
                       "系统分区里没有 Fonts 目录（中文菜单会是方块）");
            }
        }

        /* ⑤ boot.sdi（WinRE 的 ramdisk 要）*/
        bp_put(sfs,"/Windows/Boot/DVD/EFI/boot.sdi",efs,"/EFI/Microsoft/Boot/boot.sdi",NULL,false,&fl);

        img_ntfs_umount(sfs);
        img_fat32_unmount(efs);

        /* ⑥ BCD */
        rc=bcd_put(q,t,true,sys_lba,boot_lba,img_args_get(in,"desc"),img_args_get(in,"locale"),
                   (int)img_args_u64(in,"timeout",5),fam,false,NULL,&bcd_bytes,out);
        if(rc!=IMG_OK) return rc;
    } else {
        /* ① 引导分区文件系统（SR 通常是 mkntfs 空卷，或者压根没格） */
        img_ntfs_fs *bfs=img_ntfs_mount_rw(q,boot_lba,pb_of(t,boot_lba));
        if(!bfs){
            img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,boot_lba,pb_of(t,boot_lba));
            o.label="System Reserved"; o.quick=true; o.force=true;
            img_span sp; img_span_prep(&sp,"ntfs","format","格式化引导分区");
            img_span_begin(&sp);
            int frc=img_ntfs_format(q,&o,img_cmd_prog());
            img_span_end(&sp,frc);
            if(frc!=IMG_OK){
                img_result_fail(out,frc,"NTFS-FORMAT-FAIL","sr",
                    "系统保留分区没有文件系统，自动格式化也失败了","先 fs.format --lba=<sr> --fs=ntfs");
                return frc;
            }
            did_format=true;
            bfs=img_ntfs_mount_rw(q,boot_lba,pb_of(t,boot_lba));
            if(!bfs){
                img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","sr","格式化后仍挂不上 NTFS",NULL);
                return IMG_ESTATE;
            }
        }

        /* ② 活动标志（MBR 必须有，否则 BIOS 不知道从哪起）*/
        int bi=pt_idx_of(t,boot_lba);
        if(bi>=0 && !(t->parts[bi].status&0x80)){
            if(img_partition_set_active(t,bi,true)==IMG_OK){
                int wrc=img_parttable_write(q,t);
                if(wrc!=IMG_OK){
                    img_ntfs_umount(bfs);
                    img_result_fail(out,wrc,"PART-WRITE-FAIL","sr","设活动分区失败",NULL);
                    return wrc;
                }
            }
        }

        /* ③ bootmgr → 引导分区根（源在系统分区；单分区布局时两者同一个 fs） */
        img_ntfs_fs *sfs=(boot_lba==sys_lba) ? bfs : img_ntfs_mount_rw(q,sys_lba,pb_of(t,sys_lba));
        if(!sfs){
            img_ntfs_umount(bfs);
            img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","sys","系统分区不是合法 NTFS","先 wim.apply / fs.format");
            return IMG_ESTATE;
        }
        bp_put_ntfs(sfs,"/Windows/Boot/PCAT/bootmgr",bfs,"/bootmgr",true,&fl);
        if(sfs!=bfs) img_ntfs_umount(sfs);

        /* ④ MBR 引导码（内置 440 字节）+ VBR 引导码（内置模板，保留 BPB）*/
        if(memcmp(t->mbr_code,IMG_MBR_TPL,440)!=0){
            memcpy(t->mbr_code,IMG_MBR_TPL,440);
            int wrc=img_parttable_write(q,t);
            if(wrc!=IMG_OK){
                img_ntfs_umount(bfs);
                img_result_fail(out,wrc,"PART-WRITE-FAIL",NULL,"写 MBR 引导码失败",NULL);
                return wrc;
            }
            bp_add(&fl,"(LBA0)","mbr-code-bios.bin",440,true,NULL);
        } else bp_add(&fl,"(LBA0)","mbr-code-bios.bin",440,true,"已有，未改动");
        /* VBR：mkntfs 不写引导码；BPB 几何也得补（否则 Windows 引导代码算 CHS 失败）*/
        if(img_ntfs_fix_geometry(q,sys_lba)==IMG_OK && boot_lba!=sys_lba)
            (void)img_ntfs_fix_geometry(q,boot_lba);
        int vrc=img_fixboot_builtin(q,boot_lba,img_cmd_prog());
        bp_add(&fl,"(VBR)",boot_lba==sys_lba?"builtin(sys)":"builtin(sr)",512,vrc==IMG_OK,
               vrc?img_strerror(vrc):NULL);

        img_ntfs_umount(bfs);

        /* ⑤ BCD */
        rc=bcd_put(q,t,false,sys_lba,boot_lba,img_args_get(in,"desc"),img_args_get(in,"locale"),
                   (int)img_args_u64(in,"timeout",5),fam,false,NULL,&bcd_bytes,out);
        if(rc!=IMG_OK) return rc;
    }

    /* ---- 自检（App 直接显示，不用再调一次 boot.check）---- */
    img_result_str(out,"mode",uefi?"gpt/uefi":"mbr/bios");
    img_result_u64(out,"sys_lba",sys_lba);
    if(uefi) img_result_u64(out,"esp_lba",boot_lba); else img_result_u64(out,"sr_lba",boot_lba);
    img_result_bool(out,"formatted",did_format);
    img_result_str(out,"family",fam==(int)IMG_BCD_FAM_LEGACY?"legacy(6.x)":"modern(10.x)");
    img_result_u64(out,"bcd_bytes",bcd_bytes);
    img_result_u64(out,"file_count",(uint64_t)fl.n);
    if(fl.n){
        if(fl.len+1<fl.cap){ files_json[fl.len++]=']'; files_json[fl.len]=0; }
        img_result_raw(out,"files",files_json);
    }

    img_bootcheck_result_t br; memset(&br,0,sizeof br);
    int brc=img_bootcheck_ex(q,uefi?IMG_BOOT_UEFI:IMG_BOOT_BIOS,0,&br,img_cmd_prog());
    if(brc==IMG_OK && br.count){
        static char bc[256];
        snprintf(bc,sizeof bc,"{\"mode\":\"%s\",\"count\":%d,\"required_fail\":%d,\"warn_fail\":%d}",
                 uefi?"uefi":"bios",br.count,br.n_required_fail,br.n_warn_fail);
        img_result_raw(out,"boot_check",bc);
        char sum[256]; img_bootcheck_summary(&br,sum,sizeof sum);
        img_result_str(out,"boot_check_summary",sum);
    }

    if(fl.n_missing || fl.n_failed){
        img_result_fail(out,IMG_ENOENT,"BOOT-SRC-MISSING",img_ctx_path(ctx),
            fl.n_missing ? "系统分区里缺引导文件（盘里是不是还没写入系统？）"
                         : "引导文件写入失败（看 files[] 里的 error）",
            "先 wim.apply 把系统写入系统分区，再重跑 boot.populate");
        return IMG_ENOENT;
    }
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
    {"iso",   IMG_A_STR,  false, NULL,   "ISO / WIM / ESD 路径（装之前问；--what=repair 时作为 image 兜底来源）", NULL},
    {"index", IMG_A_U64,  false, "0",    "只看第 N 个映像（0=第一个）", NULL},
    {"img",   IMG_A_STR,  false, NULL,   "已装好的映像（装之后问；与 --lba 一起给）", NULL},
    {"lba",   IMG_A_U64,  false, NULL,   "系统分区起始 LBA（缺省自动找 winload）", NULL},
    {"table", IMG_A_ENUM, false, "auto", "要判定的分区表：gpt | mbr | auto(两种都答)", NULL},
    {"what",  IMG_A_ENUM, false, "compat","问什么：compat(兼容判定，老行为) | repair(★ 修复计划：每样物件从哪来/缺什么)", NULL},
    {"src",   IMG_A_ENUM, false, "auto", "来源策略：auto | disk | template | image（只影响 repair 计划里的来源标注）", NULL},
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

static int cmd_boot_plan_repair(img_ctx *ctx, const img_args *in, img_result *out);
/* ★ REQ-54 §2：读 NTFS 上某 PE 文件的 machine → 架构名（x86/x64/arm64）。
 *   只读头 64KB，不解析版本资源。读不出返回 NULL（不猜）。 */
static const char *ntfs_pe_arch_of(img_ntfs_fs *fs, const char *path){
    if(!fs || !path) return NULL;
    void *data=NULL; size_t n=0;
    if(img_ntfs_read_file(fs, path, &data, &n)!=IMG_OK || n<0x40){ free(data); return NULL; }
    size_t limit = n < 65536 ? n : 65536;
    const uint8_t *b = (const uint8_t*)data;
    uint16_t mach = 0;
    #define R16x(p) ((uint16_t)((p)[0] | ((uint16_t)(p)[1]<<8)))
    #define R32x(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1]<<8) | ((uint32_t)(p)[2]<<16) | ((uint32_t)(p)[3]<<24))
    if(b[0]=='M' && b[1]=='Z'){
        uint32_t pe=R32x(b+0x3C);
        if((size_t)pe+6<=limit && !memcmp(b+pe,"PE\0\0",4)) mach=R16x(b+pe+4);
    }
    if(!mach){
        for(size_t o=0;o+0x40<limit;o++){
            if(b[o]!='M'||b[o+1]!='Z') continue;
            uint32_t pe=R32x(b+o+0x3C);
            if((size_t)o+pe+6>limit) continue;
            if(memcmp(b+o+pe,"PE\0\0",4)) continue;
            uint16_t m=R16x(b+o+pe+4);
            if(m==0x14c||m==0x8664||m==0xAA64||m==0x1C4||m==0x200){ mach=m; break; }
        }
    }
    #undef R16x
    #undef R32x
    free(data);
    switch(mach){
        case 0x14c:  return "x86";
        case 0x8664: return "x64";
        case 0xAA64: return "arm64";
        case 0x1c4:  return "arm32";
        default:     return NULL;
    }
}

static int cmd_boot_plan(img_ctx *ctx, const img_args *in, img_result *out){
    const char *wp=img_args_get(in,"what");
    if(wp && !strcasecmp(wp,"repair")) return cmd_boot_plan_repair(ctx,in,out);
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



/* ================= 模板导出 / 应用（2026-11-04） =================
 * 用户定稿：
 *   · 导出 = BCD 骨架（无本机 GUID/签名/分区号）+ 元数据 JSON（family/arch/firmware/
 *     built_from/elements/sha256/created）。**不含 bootmgr/bootmgfw**（那是微软二进制，
 *     用系统自带的）。
 *   · 应用：跨版本族/跨架构/跨固件 → 一律拒绝 TPL-NOT-APPLICABLE；
 *     同族小版本不同 → 允许，但返回双方版本号，App 弹"可能存在不兼容，是否使用？"。
 *   · 设备对象永远按当前盘现生成（模板里就算有残留绑定也不写进目标盘）。*/
static int cmd_boot_tpl_export_bcd(img_ctx *ctx, const img_args *in, img_result *out, const char *bcd_file);
static const img_arg A_TPLEXPORT[] = {
    {"img",  IMG_A_STR, false, NULL, "映像路径（qcow2；给了 --bcd 就不需要）", NULL},
    {"bcd",  IMG_A_STR, false, NULL, "BCD 本地文件（从 ISO 提取来的模板原料；给了就不读盘）", NULL},
    {"lba",  IMG_A_U64, false, NULL, "引导分区 LBA（BIOS=SR/UEFI=ESP；--bcd 模式不用）", NULL},
    {"uefi", IMG_A_BOOL,false, "false","引导分区是 ESP（FAT32）还是 SR（NTFS）", NULL},
    {"out",  IMG_A_STR, true,  NULL, "导出到本地文件（.bcdtpl）", NULL},
    {"meta", IMG_A_STR, false, NULL, "元数据 JSON 写哪（缺省 <out>.json）", NULL},
    {"desc", IMG_A_STR, false, "Windows","元数据里的来源说明（ISO 名/盘名）", NULL},
    {"family", IMG_A_STR, false, NULL, "（--bcd 模式）版本族：6.1|6.2|6.3|10.0", NULL},
    {"arch",   IMG_A_STR, false, NULL, "（--bcd 模式）架构：x86|x64|arm64", NULL},
    {"os-version", IMG_A_STR, false, NULL, "（--bcd 模式）完整版本串，如 10.0.22631.1", NULL},
};
static int cmd_boot_tpl_export(img_ctx *ctx, const img_args *in, img_result *out){
    /* ★ --bcd=<本地文件>：BCD 不在盘上（从 ISO 抽出来的模板原料）也能导出 ——
     *   阶段 2 建 12 个模板就走这条：iso.extract 出 \boot\BCD / \efi\microsoft\boot\BCD，
     *   再 boot.tpl-export --bcd=... --family=... --arch=... --out=...。 */
    const char *bcd_file=img_args_get(in,"bcd");
    if(bcd_file && *bcd_file) return cmd_boot_tpl_export_bcd(ctx,in,out,bcd_file);
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t lba=img_args_u64(in,"lba",0);
    bool uefi=img_args_bool(in,"uefi",false);
    const char *outp=img_args_get(in,"out");
    if(!lba||!outp||!*outp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba/out","需要 --lba 与 --out",NULL); return IMG_EINVAL; }
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba)
            pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","该 LBA 不在分区表里",NULL); return IMG_EINVAL; }

    /* 读 BCD
     * ★ 必须传分区字节数（第二个参数是 partition_bytes；漏传→长度 0→挂载失败
     *   报"ESP 不是合法 FAT32"——其实在骂调用方传参） */
    void *bcd=NULL; size_t bn=0;
    if(uefi){
        img_fat32_fs *efs=img_fat32_mount(q,lba,true);
        if(!efs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","esp","ESP 不是合法 FAT32",NULL); return IMG_ESTATE; }
        rc=img_fat32_read_file(efs,"/EFI/Microsoft/Boot/BCD",&bcd,&bn);
        img_fat32_unmount(efs);
    } else {
        img_ntfs_fs *bfs=img_ntfs_mount_rw(q,lba,pb);
        if(!bfs){ img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","sr","引导分区不是合法 NTFS",NULL); return IMG_ESTATE; }
        rc=img_ntfs_read_file(bfs,"/Boot/BCD",&bcd,&bn);
        img_ntfs_umount(bfs);
    }
    if(rc!=IMG_OK || !bn){
        img_result_fail(out,IMG_ENOENT,"BCD-NOT-FOUND",uefi?"/EFI/Microsoft/Boot/BCD":"/Boot/BCD",
            "引导分区上没有 BCD","先 boot.populate 或 bcd.create 生成一张");
        return IMG_ENOENT;
    }
    img_bcd *b=img_bcd_open_mem(bcd,bn);
    if(!b){ free(bcd); img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD","bcd","不是合法 hive",NULL); return IMG_EFORMAT; }
    int stripped=0;
    img_bcd_strip_devices(b,&stripped);
    img_bcd_stats_t stt; img_bcd_stats(b,&stt);
    rc=img_bcd_save(b,outp);
    img_bcd_close(b);
    free(bcd);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",outp,"导出失败",NULL); return rc; }

    /* 元数据：family / arch / firmware 从盘里读；读不到就标 unknown，不猜 */
    char fam[16]="unknown", archs[16]="unknown", osv[40]="";
    {   /* 找系统分区读版本 */
        uint64_t sl=0,sb=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!t->parts[i].used) continue;
            uint64_t b2=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
            if(t->parts[i].type_guid[0] && strncmp(t->parts[i].type_guid,IMG_GUID_BASIC,8)) continue;
            img_ntfs_fs *f=img_ntfs_mount_rw(q,t->parts[i].first_lba,b2);
            if(!f) continue;
            bool hit=img_ntfs_path_exists(f,"/Windows/System32/winload.exe")
                  || img_ntfs_path_exists(f,"/Windows/System32/winload.efi");
            img_ntfs_umount(f);
            if(hit){ sl=t->parts[i].first_lba; sb=b2; break; }
        }
        if(sl){
            img_winver_t v; char e2[128]="";
            if(img_win_version(q,sl,sb,&v,e2,sizeof e2)==IMG_OK){
                int maj=v.kernel_major?v.kernel_major:atoi(v.current_version);
                int min=v.kernel_minor;
                int bld=v.consensus_build?v.consensus_build:v.current_build;
                snprintf(fam,sizeof fam,"%d.%d",maj,min);
                snprintf(osv,sizeof osv,"%d.%d.%d",maj,min,bld);
                if(v.pe_machine==0x8664) snprintf(archs,sizeof archs,"x64");
                else if(v.pe_machine==0x14c) snprintf(archs,sizeof archs,"x86");
                else if(v.pe_machine==0xAA64) snprintf(archs,sizeof archs,"arm64");
            }
        }
    }
    char metap[600]; const char *mp=img_args_get(in,"meta");
    if(mp && *mp) snprintf(metap,sizeof metap,"%s",mp);
    else snprintf(metap,sizeof metap,"%s.json",outp);
    FILE *mf=fopen(metap,"w");
    if(!mf){ img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",metap,"元数据写不了",NULL); return IMG_EIO; }
    fprintf(mf,
        "{\n"
        "  \"format\": \"bcdtpl/1\",\n"
        "  \"family\": \"%s\",\n"
        "  \"arch\": \"%s\",\n"
        "  \"firmware\": \"%s\",\n"
        "  \"built_from\": \"%s\",\n"
        "  \"os_version\": \"%s\",\n"
        "  \"objects\": %d,\n"
        "  \"elements\": %d,\n"
        "  \"loaders\": %d,\n"
        "  \"devices_stripped\": %d,\n"
        "  \"bcd_bytes\": %llu,\n"
        "  \"created\": \"%s\"\n"
        "}\n",
        fam, archs, uefi?"uefi":"bios",
        img_args_get(in,"desc")?img_args_get(in,"desc"):"",
        osv, stt.objects, stt.elements, stt.loaders, stripped,
        (unsigned long long)bn, "2026-11-04");
    fclose(mf);

    img_result_str(out,"out",outp);
    img_result_str(out,"meta",metap);
    img_result_str(out,"family",fam);
    img_result_str(out,"arch",archs);
    img_result_str(out,"firmware",uefi?"uefi":"bios");
    img_result_u64(out,"objects",(uint64_t)stt.objects);
    img_result_u64(out,"elements",(uint64_t)stt.elements);
    img_result_u64(out,"devices_stripped",(uint64_t)stripped);
    img_result_u64(out,"bcd_bytes",(uint64_t)bn);
    return IMG_OK;
}

/* ---- 模板应用：读模板 + 校验适用性 + 按本盘现生成设备对象 ---- */
static const img_arg A_TPLAPPLY[] = {
    {"img",  IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
    {"sys-lba", IMG_A_U64, false, NULL,"系统分区 LBA（缺省自动找 winload）", NULL},
    {"lba",  IMG_A_U64, false, NULL, "引导分区 LBA（缺省按分区表找 ESP/活动分区）", NULL},
    {"tpl",  IMG_A_STR, true,  NULL, "模板 BCD 文件（.bcdtpl）", NULL},
    {"meta", IMG_A_STR, false, NULL, "模板元数据 JSON（缺省读 <tpl>.json）", NULL},
    {"force",IMG_A_BOOL,false, "false","跨族/跨架构/跨固件也强制应用（★ 默认拒绝；App 不要随便给）", NULL},
    {"desc", IMG_A_STR, false, "Windows","启动菜单显示名", NULL},
    {"locale",IMG_A_STR,false, "zh-CN","BCD locale", NULL},
    {"timeout",IMG_A_U64,false,"5",  "菜单等待秒数", NULL},
};
/* --bcd=<文件>：模板原料直接来自 ISO 抽出来的 BCD（阶段 2 建模板的主路径）。
 * family/arch 必须显式给 —— ISO 里的 BCD 不带机器信息，这正是元数据 JSON 的用途。 */
static int cmd_boot_tpl_export_bcd(img_ctx *ctx, const img_args *in, img_result *out, const char *bcd_file){
    (void)ctx;
    const char *outp=img_args_get(in,"out");
    if(!outp||!*outp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","out","需要 --out",NULL); return IMG_EINVAL; }
    const char *fam=img_args_get(in,"family"), *ar=img_args_get(in,"arch");
    const char *osv=img_args_get(in,"os-version");
    bool uefi=img_args_bool(in,"uefi",false);
    if(!fam||!*fam||!ar||!*ar){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","family/arch",
            "--bcd 模式必须给 --family 与 --arch（ISO 里的 BCD 不带机器信息）",
            "family 取 Windows 版本族（6.1/6.2/6.3/10.0）；arch 取位数（x86/x64/arm64）");
        return IMG_EINVAL;
    }
    FILE *f=fopen(bcd_file,"rb");
    if(!f){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",bcd_file,"BCD 文件无法打开",NULL); return IMG_ENOENT; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<512||n>(64<<20)){ fclose(f); img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD",bcd_file,"大小不像 BCD",NULL); return IMG_EFORMAT; }
    void *bcd=malloc((size_t)n);
    if(!bcd||fread(bcd,1,(size_t)n,f)!=(size_t)n){ fclose(f); free(bcd); return IMG_EIO; }
    fclose(f);
    img_bcd *b=img_bcd_open_mem(bcd,(size_t)n);
    if(!b){ free(bcd); img_result_fail(out,IMG_EFORMAT,"BCD-REF-BAD",bcd_file,"不是合法 hive",NULL); return IMG_EFORMAT; }
    int stripped=0;
    img_bcd_strip_devices(b,&stripped);
    img_bcd_stats_t stt; img_bcd_stats(b,&stt);
    int rc=img_bcd_save(b,outp);
    img_bcd_close(b);
    free(bcd);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",outp,"导出失败",NULL); return rc; }
    char metap[600]; const char *mp=img_args_get(in,"meta");
    if(mp&&*mp) snprintf(metap,sizeof metap,"%s",mp); else snprintf(metap,sizeof metap,"%s.json",outp);
    FILE *mf=fopen(metap,"w");
    if(!mf){ img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",metap,"元数据写不了",NULL); return IMG_EIO; }
    fprintf(mf,
        "{\n  \"format\": \"bcdtpl/1\",\n"
        "  \"family\": \"%s\",\n  \"arch\": \"%s\",\n  \"firmware\": \"%s\",\n"
        "  \"built_from\": \"%s\",\n  \"os_version\": \"%s\",\n"
        "  \"objects\": %d,\n  \"elements\": %d,\n  \"loaders\": %d,\n"
        "  \"devices_stripped\": %d,\n  \"bcd_bytes\": %llu,\n  \"created\": \"%s\"\n}\n",
        fam, ar, uefi?"uefi":"bios",
        img_args_get(in,"desc")?img_args_get(in,"desc"):"",
        osv?osv:"", stt.objects, stt.elements, stt.loaders, stripped,
        (unsigned long long)n, "2026-11-04");
    fclose(mf);
    img_result_str(out,"out",outp);
    img_result_str(out,"meta",metap);
    img_result_str(out,"family",fam);
    img_result_str(out,"arch",ar);
    img_result_str(out,"firmware",uefi?"uefi":"bios");
    img_result_u64(out,"objects",(uint64_t)stt.objects);
    img_result_u64(out,"elements",(uint64_t)stt.elements);
    img_result_u64(out,"devices_stripped",(uint64_t)stripped);
    img_result_u64(out,"bcd_bytes",(uint64_t)n);
    return IMG_OK;
}

/* ---------- boot.tpl-inspect：应用前的预览（只读；App 弹窗的数据源） ----------
 * 给三样东西：模板自己是什么（元数据）、本盘是什么（版本/架构/固件）、
 * 逐项适用判定（firmware/arch/family + 小版本差异）。
 * ★ 判定口径与 tpl-apply 完全一致 —— inspect 说能过，apply 就能过。 */
static const img_arg A_TPLINSPECT[] = {
    {"img",  IMG_A_STR, false, NULL, "目标映像（不给就只报模板自身）", NULL},
    {"sys-lba", IMG_A_U64, false, NULL, "系统分区 LBA（缺省自动找 winload）", NULL},
    {"tpl",  IMG_A_STR, true,  NULL, "模板 BCD 文件（.bcdtpl）", NULL},
    {"meta", IMG_A_STR, false, NULL, "模板元数据 JSON（缺省读 <tpl>.json）", NULL},
};
static int cmd_boot_tpl_inspect(img_ctx *ctx, const img_args *in, img_result *out){
    const char *tp=img_args_get(in,"tpl");
    if(!tp||!*tp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","tpl","需要 --tpl",NULL); return IMG_EINVAL; }
    FILE *f=fopen(tp,"rb");
    if(!f){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",tp,"模板文件无法打开",NULL); return IMG_ENOENT; }
    fseek(f,0,SEEK_END); long tn=ftell(f); fseek(f,0,SEEK_SET);
    if(tn<512||tn>(64<<20)){ fclose(f); img_result_fail(out,IMG_EFORMAT,"TPL-BAD",tp,"模板大小异常",NULL); return IMG_EFORMAT; }
    void *tb=malloc((size_t)tn);
    if(!tb||fread(tb,1,(size_t)tn,f)!=(size_t)tn){ fclose(f); free(tb); return IMG_EIO; }
    fclose(f);
    img_bcd *b=img_bcd_open_mem(tb,(size_t)tn);
    if(!b){ free(tb); img_result_fail(out,IMG_EFORMAT,"TPL-BAD",tp,"不是合法 hive",NULL); return IMG_EFORMAT; }
    img_bcd_stats_t stt; img_bcd_stats(b,&stt);
    img_bcd_close(b);
    free(tb);

    char metap[600]; const char *mp=img_args_get(in,"meta");
    if(mp&&*mp) snprintf(metap,sizeof metap,"%s",mp); else snprintf(metap,sizeof metap,"%s.json",tp);
    char tplfam[24]="", tplarch[24]="", tplfw[24]="", tplos[40]="", tplsrc[200]="";
    FILE *mf=fopen(metap,"rb");
    if(mf){
        char buf[4096]; size_t n=fread(buf,1,sizeof buf-1,mf); buf[n]=0; fclose(mf);
        const char *p;
        if((p=strstr(buf,"\"family\"")))      sscanf(p,"\"family\": \"%23[^\"]\"",tplfam);
        if((p=strstr(buf,"\"arch\"")))        sscanf(p,"\"arch\": \"%23[^\"]\"",tplarch);
        if((p=strstr(buf,"\"firmware\"")))    sscanf(p,"\"firmware\": \"%23[^\"]\"",tplfw);
        if((p=strstr(buf,"\"os_version\"")))  sscanf(p,"\"os_version\": \"%39[^\"]\"",tplos);
        if((p=strstr(buf,"\"built_from\"")))  sscanf(p,"\"built_from\": \"%199[^\"]\"",tplsrc);
    }
    img_result_str(out,"tpl_family",tplfam[0]?tplfam:"unknown");
    img_result_str(out,"tpl_arch",tplarch[0]?tplarch:"unknown");
    img_result_str(out,"tpl_firmware",tplfw[0]?tplfw:"");
    img_result_str(out,"tpl_version",tplos);
    img_result_str(out,"tpl_built_from",tplsrc);
    img_result_u64(out,"tpl_objects",(uint64_t)stt.objects);
    img_result_u64(out,"tpl_elements",(uint64_t)stt.elements);
    img_result_str(out,"tpl_meta_found",(mf||mp)?"true":"false");

    const char *imgp=img_args_get(in,"img");
    if(!imgp||!*imgp){ img_result_str(out,"verdict","no_target"); return IMG_OK; }

    /* 本盘：固件/版本/架构 */
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    bool uefi=(t->table==IMG_TABLE_GPT);
    uint64_t sys_lba=img_args_u64(in,"sys-lba",0), sys_b=0;
    if(sys_lba){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==sys_lba)
                sys_b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
    } else {
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!t->parts[i].used) continue;
            uint64_t b2=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
            img_ntfs_fs *s2=img_ntfs_mount_rw(q,t->parts[i].first_lba,b2);
            if(!s2) continue;
            bool hit=img_ntfs_path_exists(s2,"/Windows/System32/winload.exe")
                  || img_ntfs_path_exists(s2,"/Windows/System32/winload.efi");
            img_ntfs_umount(s2);
            if(hit){ sys_lba=t->parts[i].first_lba; sys_b=b2; break; }
        }
    }
    img_result_bool(out,"os_found",sys_lba!=0);
    char diskfam[24]="unknown", diskarch[24]="unknown", diskos[40]="";
    if(sys_lba&&sys_b){
        img_winver_t v; char e2[128]="";
        if(img_win_version(q,sys_lba,sys_b,&v,e2,sizeof e2)==IMG_OK){
            int maj=v.kernel_major?v.kernel_major:atoi(v.current_version);
            snprintf(diskfam,sizeof diskfam,"%d.%d",maj,v.kernel_minor);
            snprintf(diskos,sizeof diskos,"%d.%d.%d",maj,v.kernel_minor,
                     v.consensus_build?v.consensus_build:v.current_build);
            if(v.pe_machine==0x8664) snprintf(diskarch,sizeof diskarch,"x64");
            else if(v.pe_machine==0x14c) snprintf(diskarch,sizeof diskarch,"x86");
            else if(v.pe_machine==0xAA64) snprintf(diskarch,sizeof diskarch,"arm64");
        }
    }
    img_result_str(out,"disk_family",diskfam);
    img_result_str(out,"disk_arch",diskarch);
    img_result_str(out,"disk_version",diskos);
    img_result_bool(out,"disk_uefi",uefi);

    bool fw_ok   = !tplfw[0]  || !strcasecmp(tplfw, uefi?"uefi":"bios");
    bool arch_ok = !tplarch[0]|| !strcmp(tplarch,"unknown") || !strcmp(diskarch,"unknown")
                   || !strcasecmp(tplarch,diskarch);
    bool fam_ok  = !tplfam[0] || !strcmp(tplfam,"unknown") || !strcmp(diskfam,"unknown")
                   || !strcmp(tplfam,diskfam);
    bool disk_unknown = !strcmp(diskfam,"unknown");
    bool minor_diff = !disk_unknown && !fam_ok ? false
                    : (tplos[0] && diskos[0] && strcmp(tplos,diskos)!=0);
    bool ok = fw_ok && arch_ok && fam_ok && !disk_unknown;
    img_result_bool(out,"firmware_ok",fw_ok);
    img_result_bool(out,"arch_ok",arch_ok);
    img_result_bool(out,"family_ok",fam_ok);
    img_result_bool(out,"disk_version_unknown",disk_unknown);
    img_result_bool(out,"minor_version_diff",minor_diff);
    img_result_str(out,"verdict", ok ? "applicable" : "not_applicable");
    return IMG_OK;
}

static int cmd_boot_tpl_apply(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    const char *tp=img_args_get(in,"tpl");
    if(!tp||!*tp){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","tpl","需要 --tpl",NULL); return IMG_EINVAL; }
    FILE *f=fopen(tp,"rb");
    if(!f){ img_result_fail(out,IMG_ENOENT,"HOST-OPEN-FAIL",tp,"模板文件无法打开",NULL); return IMG_ENOENT; }
    fseek(f,0,SEEK_END); long tn=ftell(f); fseek(f,0,SEEK_SET);
    if(tn<512 || tn>(32<<20)){ fclose(f); img_result_fail(out,IMG_EFORMAT,"TPL-BAD",tp,"模板大小异常",NULL); return IMG_EFORMAT; }
    void *tb=malloc((size_t)tn);
    if(!tb || fread(tb,1,(size_t)tn,f)!=(size_t)tn){ fclose(f); free(tb); return IMG_EIO; }
    fclose(f);

    /* 元数据：<tpl>.json */
    char metap[600]; const char *mp=img_args_get(in,"meta");
    if(mp && *mp) snprintf(metap,sizeof metap,"%s",mp);
    else snprintf(metap,sizeof metap,"%s.json",tp);
    char tplfam[24]="", tplarch[24]="", tplfw[24]="", tplos[40]="";
    FILE *mf=fopen(metap,"rb");
    if(mf){
        char buf[4096]; size_t n=fread(buf,1,sizeof buf-1,mf); buf[n]=0; fclose(mf);
        const char *p;
        if((p=strstr(buf,"\"family\"")))      sscanf(p,"\"family\": \"%23[^\"]\"",tplfam);
        if((p=strstr(buf,"\"arch\"")))        sscanf(p,"\"arch\": \"%23[^\"]\"",tplarch);
        if((p=strstr(buf,"\"firmware\"")))    sscanf(p,"\"firmware\": \"%23[^\"]\"",tplfw);
        if((p=strstr(buf,"\"os_version\"")))  sscanf(p,"\"os_version\": \"%39[^\"]\"",tplos);
    }

    /* 本盘实际：固件由分区表定；版本/架构从系统分区读 */
    bool uefi=(t->table==IMG_TABLE_GPT);
    uint64_t sys_lba=img_args_u64(in,"sys-lba",0), sys_b=0;
    if(!sys_lba){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!t->parts[i].used) continue;
            uint64_t b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
            img_ntfs_fs *s2=img_ntfs_mount_rw(q,t->parts[i].first_lba,b);
            if(!s2) continue;
            bool hit=img_ntfs_path_exists(s2,"/Windows/System32/winload.exe")
                  || img_ntfs_path_exists(s2,"/Windows/System32/winload.efi");
            img_ntfs_umount(s2);
            if(hit){ sys_lba=t->parts[i].first_lba; sys_b=b; break; }
        }
    } else {
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==sys_lba)
                sys_b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
    }
    if(!sys_lba||!sys_b){ free(tb); img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","sys-lba","没找到系统分区",NULL); return IMG_EINVAL; }

    char diskfam[24]="unknown", diskarch[24]="unknown", diskos[40]="";
    { img_winver_t v; char e2[128]="";
      if(img_win_version(q,sys_lba,sys_b,&v,e2,sizeof e2)==IMG_OK){
          int maj=v.kernel_major?v.kernel_major:atoi(v.current_version);
          snprintf(diskfam,sizeof diskfam,"%d.%d",maj,v.kernel_minor);
          snprintf(diskos,sizeof diskos,"%d.%d.%d",maj,v.kernel_minor,
                   v.consensus_build?v.consensus_build:v.current_build);
          if(v.pe_machine==0x8664) snprintf(diskarch,sizeof diskarch,"x64");
          else if(v.pe_machine==0x14c) snprintf(diskarch,sizeof diskarch,"x86");
          else if(v.pe_machine==0xAA64) snprintf(diskarch,sizeof diskarch,"arm64");
      }
    }

    /* 适用性：固件/架构/版本族 三样都硬校验 */
    char why[400]="";
    bool not_applicable=false;
    if(tplfw[0] && strcasecmp(tplfw,uefi?"uefi":"bios")){
        not_applicable=true;
        snprintf(why,sizeof why,"模板是 %s 的，这张盘是 %s，固件不符",tplfw,uefi?"uefi":"bios");
    } else if(tplarch[0] && strcmp(tplarch,"unknown") && diskarch[0] && strcmp(diskarch,"unknown")
              && strcasecmp(tplarch,diskarch)){
        not_applicable=true;
        snprintf(why,sizeof why,"模板是 %s 的，这张盘是 %s，架构不符",tplarch,diskarch);
    } else if(tplfam[0] && strcmp(tplfam,"unknown") && diskfam[0] && strcmp(diskfam,"unknown")
              && strcmp(tplfam,diskfam)){
        not_applicable=true;
        snprintf(why,sizeof why,"模板是 %s 族，这张盘是 %s 族，跨版本族（BCD 元素不兼容）",tplfam,diskfam);
    }
    /* ★ 硬校验要"证据充分"才放行：模板三样（family/arch/firmware）里，固件、架构
     *   与本盘不一致 → 拒；版本族不一致 → 拒。
     *   本盘读不出版本（unknown）≠ 豁免：夹具/半成品盘最容易被"unknown"绕过 ——
     *   只要是【模板声明了】而【本盘读不出】的组合，一律按不适用拒绝（--force 才放行）。 */
    if(not_applicable){
        /* 已经在 why 里写明了是哪一样不符 */
    } else if(tplfam[0] && strcmp(tplfam,"unknown") && strcmp(diskfam,"unknown")){
        /* 同族：通过 */
    } else if(tplfam[0] && strcmp(tplfam,"unknown") && !strcmp(diskfam,"unknown")){
        not_applicable=true;
        snprintf(why,sizeof why,"模板是 %s 族，但这张盘读不出系统版本（unknown），不能确认适用",
                 tplfam);
    }
    if(not_applicable && !img_args_bool(in,"force",false)){
        free(tb);
        img_result_fail(out,IMG_ENOTSUP,"TPL-NOT-APPLICABLE",tp,why,
            "换一个同族模板；确实要用就带 --force=true（不保证能启动）");
        return IMG_ENOTSUP;
    }
    /* 同族小版本不同：允许，但把双方版本号给 App 去弹窗 */
    bool minor_diff = tplos[0] && diskos[0] && strcmp(tplos,diskos)!=0;

    /* 引导分区 */
    uint64_t boot_lba=img_args_u64(in,"lba",0), boot_b=0;
    if(!boot_lba){
        if(uefi){
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t->parts[i].used && !strncasecmp(t->parts[i].type_guid,"c12a7328",8))
                    { boot_lba=t->parts[i].first_lba; break; }
        } else {
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t->parts[i].used && !t->parts[i].logical && (t->parts[i].status&0x80))
                    { boot_lba=t->parts[i].first_lba; break; }
            if(!boot_lba) boot_lba=sys_lba;
        }
    }
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==boot_lba)
            boot_b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
    if(!boot_lba||!boot_b){ free(tb); img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","没有引导分区（UEFI 要先建 ESP）",NULL); return IMG_EINVAL; }

    int fam=(int)IMG_BCD_FAM_MODERN;
    { int maj=atoi(diskfam); if(maj==6 && atof(diskfam)<6.2) fam=(int)IMG_BCD_FAM_LEGACY; }

    img_bcd_create_req_t rq; memset(&rq,0,sizeof rq);
    rq.uefi=uefi; rq.family=(img_bcd_family)fam;
    rq.desc=img_args_get(in,"desc"); rq.locale=img_args_get(in,"locale");
    rq.timeout=(int)img_args_u64(in,"timeout",5);
    rq.template_bytes=tb; rq.template_len=(size_t)tn;
    if(uefi){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!t->parts[i].used) continue;
            if(t->parts[i].first_lba==sys_lba)  rq.sys_guid=t->parts[i].part_guid;
            if(t->parts[i].first_lba==boot_lba) rq.esp_guid=t->parts[i].part_guid;
        }
        rq.disk_guid=t->disk_guid;
    } else {
        rq.mbr_sig=t->disk_signature;
        rq.sys_off=sys_lba*512ull;
        rq.sr_off=(boot_lba!=sys_lba)?boot_lba*512ull:0;
    }
    void *nb=NULL; size_t nbn=0; char err[256]="";
    rc=img_bcd_create_mem(&rq,&nb,&nbn,err,sizeof err);
    free(tb);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-BUILD-FAIL",tp,err[0]?err:"模板无法实例化",NULL); return rc; }
    if(uefi){
        img_fat32_fs *efs=img_fat32_mount(q,boot_lba,true);
        if(!efs){ free(nb); img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","esp","ESP 不是合法 FAT32",NULL); return IMG_ESTATE; }
        img_fat32_mkdir_p(efs,"/EFI/Microsoft/Boot");
        rc=img_fat32_put_data(efs,nb,nbn,"/EFI/Microsoft/Boot/BCD");
        img_fat32_unmount(efs);
    } else {
        img_ntfs_fs *bfs=img_ntfs_mount_rw(q,boot_lba,boot_b);
        if(!bfs){ free(nb); img_result_fail(out,IMG_ESTATE,"PART-FS-BAD","sr","引导分区不是合法 NTFS",NULL); return IMG_ESTATE; }
        img_ntfs_mkdir_p(bfs,"/Boot");
        rc=img_ntfs_put_file_mem(bfs,nb,nbn,"/Boot/BCD");
        img_ntfs_umount(bfs);
    }
    free(nb);
    if(rc!=IMG_OK){ img_result_fail(out,rc,"BCD-WRITE-FAIL",uefi?"/EFI/Microsoft/Boot/BCD":"/Boot/BCD","写入失败",NULL); return rc; }

    img_result_str(out,"mode",uefi?"gpt/uefi":"mbr/bios");
    img_result_str(out,"tpl_family",tplfam[0]?tplfam:"unknown");
    img_result_str(out,"tpl_arch",tplarch[0]?tplarch:"unknown");
    img_result_str(out,"tpl_firmware",tplfw[0]?tplfw:"");
    img_result_str(out,"tpl_version",tplos);
    img_result_str(out,"disk_family",diskfam);
    img_result_str(out,"disk_arch",diskarch);
    img_result_str(out,"disk_version",diskos);
    img_result_bool(out,"minor_version_diff",minor_diff);
    img_result_bool(out,"forced",not_applicable);
    img_result_u64(out,"sys_lba",sys_lba);
    img_result_u64(out,"boot_lba",boot_lba);
    return IMG_OK;
}

/* ================= boot.plan --what=repair：修复计划（只读） =================
 * ★ 2026-11-04 与用户定稿的"统一修复策略"：
 *   · 真正要修的物件每样标一个来源：disk（\Windows\Boot\*，系统自带，版本必然匹配）
 *     / template（BCD 骨架、MBR 引导码、NTFS VBR 引导码——内置）/ image（ISO/WIM 兜底）。
 *   · disk 优先：bootmgr/bootmgfw 从盘里拷（版本和 winload 天然匹配）；BCD/MBR/VBR
 *     永远用内置模板。只有 \Windows\Boot 被精简/破坏、系统分区空、装新系统才要 ISO。
 *   · 硬边界：GPT+BIOS 不假装能修（Windows 本身不支持）。
 *   · \EFI\Boot\BOOTX64.EFI 兜底：不存在→写；是我们的 bootmgr→覆盖；
 *     是别人的（GRUB 等）→ 不覆盖，efi_fallback_conflict=true 报给 App 问用户。 */
typedef struct {
    const char *id;        /* bootmgr / bootmgfw / fonts / boot_sdi / bcd / mbr_code / vbr_code / efi_fallback */
    const char *what;      /* 显示名 */
    const char *from;      /* disk | template | image | none */
    const char *src_path;  /* 来源路径（disk=image 内路径；template=内置）*/
    const char *dst_path;  /* 落点（分区内路径）*/
    bool        available; /* 这个来源现在能不能拿到 */
    bool        present;   /* 落点已经是对的（不用修）*/
    char        note[180];
} bfitem_t;

/* JSON 字符串转义（反斜杠/引号/控制字符）：修复计划里全是 Windows 路径 */
static void jesc_put(const char *src, char *dst, size_t cap){
    size_t k=0;
    for(const unsigned char *p=(const unsigned char*)src; p&&*p && k+8<cap; p++){
        if(*p=='"'||*p=='\\'){ dst[k++]='\\'; dst[k++]=(char)*p; }
        else if(*p<0x20) k+=(size_t)snprintf(dst+k,cap-k,"\\u%04x",*p);
        else dst[k++]=(char)*p;
    }
    if(cap) dst[k<cap?k:cap-1]=0;
}

static int plan_repair_run(img_ctx *ctx, const img_args *in, img_result *out){
    img_qcow2 *q=img_ctx_qcow2(ctx);
    if(!q){ return qc_open_fail(out,ctx); }
    img_parttable_t *t=NULL; int rc=img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    if(t->table==IMG_TABLE_NONE){
        img_result_fail(out,IMG_EINVAL,"PART-READ-FAIL",NULL,"该映像还没有分区表","用 img.create 建布局");
        return IMG_EINVAL;
    }
    const char *src_mode=img_args_get(in,"src"); if(!src_mode||!*src_mode) src_mode="auto";
    bool uefi=(t->table==IMG_TABLE_GPT);

    /* 系统分区：显式给，或者自动找带 winload 的 */
    uint64_t sys_lba=img_args_u64(in,"lba",0);
    uint64_t sys_b=0;
    if(sys_lba){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==sys_lba)
                sys_b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
    } else {
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!t->parts[i].used) continue;
            uint64_t b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;
            img_ntfs_fs *f=img_ntfs_mount_rw(q,t->parts[i].first_lba,b);
            if(!f) continue;
            bool hit=img_ntfs_path_exists(f,"/Windows/System32/winload.exe")
                  || img_ntfs_path_exists(f,"/Windows/System32/winload.efi");
            img_ntfs_umount(f);
            if(hit){ sys_lba=t->parts[i].first_lba; sys_b=b; break; }
        }
    }

    /* 引导分区：UEFI→ESP；BIOS→活动分区，没有就用系统分区 */
    uint64_t boot_lba=0, boot_b=0; const char *boot_kind="none";
    if(uefi){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && !strncasecmp(t->parts[i].type_guid,"c12a7328",8))
                { boot_lba=t->parts[i].first_lba; boot_kind="esp"; break; }
    } else {
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && !t->parts[i].logical && (t->parts[i].status&0x80))
                { boot_lba=t->parts[i].first_lba; boot_kind="sr"; break; }
        if(!boot_lba && sys_lba){ boot_lba=sys_lba; boot_kind="os"; }
    }
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==boot_lba)
            boot_b=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u;

    img_result_str(out,"mode",uefi?"gpt/uefi":"mbr/bios");
    img_result_u64(out,"sys_lba",sys_lba); img_result_u64(out,"sys_bytes",sys_b);
    img_result_u64(out,"boot_lba",boot_lba); img_result_u64(out,"boot_bytes",boot_b);
    img_result_str(out,"boot_partition",boot_kind);
    if(!sys_lba){
        img_result_str(out,"verdict","no_os");
        img_result_str(out,"note","没找到系统分区（没有 \\Windows\\System32\\winload.*），盘是空的？装系统请走 wim.apply");
        return IMG_OK;
    }

    img_ntfs_fs *sfs=img_ntfs_mount_rw(q,sys_lba,sys_b);

    /* ---- 物件事项表 ---- */
    /* ★ REQ-54 §2：兜底启动器名字必须按【本盘真实位数】定，不能只看 --arch。
     *   优先读 \Windows\Boot\EFI\bootmgfw.efi 的 PE machine（与 boot.populate
     *   写文件时用的同一个 arch 判定源），读不出才退回 --arch，再退回 x64。 */
    const char *fbname = "/EFI/Boot/bootx64.efi";
    const char *arch_basis = "arg-default";
    {
        const char *a = img_args_get(in,"arch");
        const char *disk_arch = NULL;
        if(sfs){
            disk_arch = ntfs_pe_arch_of(sfs, "/Windows/Boot/EFI/bootmgfw.efi");
            if(!disk_arch) disk_arch = ntfs_pe_arch_of(sfs, "/Windows/system32/winload.efi");
        }
        const char *eff = disk_arch ? disk_arch : a;
        if(eff){
            if(!strcasecmp(eff,"arm64"))       fbname="/EFI/Boot/bootaa64.efi";
            else if(!strcasecmp(eff,"x86")||!strcasecmp(eff,"ia32")) fbname="/EFI/Boot/bootia32.efi";
            arch_basis = disk_arch ? "disk-pe" : "arg";
        }
        img_result_str(out,"fallback_name", fbname+10);   /* 去掉 "/EFI/Boot/" */
        img_result_str(out,"fallback_arch_basis", arch_basis);
    }
    struct { const char *id,*what,*from,*src,*dst; bool avail,present; const char *note; } it[8];
    int ni=0;
    memset(it,0,sizeof it);
    if(uefi){
        it[ni++]=(typeof(it[0])){"bootmgfw","bootmgfw.efi（UEFI 启动管理器）","disk",
            "\\Windows\\Boot\\EFI\\bootmgfw.efi","/EFI/Microsoft/Boot/bootmgfw.efi",false,false,
            "系统自带；版本与 winload 天然匹配"};
        it[ni++]=(typeof(it[0])){"boot_sdi","boot.sdi（WinRE ramdisk）","disk",
            "\\Windows\\Boot\\DVD\\EFI\\boot.sdi","/EFI/Microsoft/Boot/boot.sdi",false,false,""};
        it[ni++]=(typeof(it[0])){"fonts","Boot\\Fonts（中文菜单不出方块）","disk",
            "\\Windows\\Boot\\Fonts","/EFI/Microsoft/Boot/Fonts",false,false,""};
        it[ni++]=(typeof(it[0])){"bcd","BCD（内置模板现生成）","template",
            "builtin","/EFI/Microsoft/Boot/BCD",true,false,"设备对象按本盘 GUID 现铺，不带模板作者的机器信息"};
        it[ni++]=(typeof(it[0])){"efi_fallback","\\EFI\\Boot 固件兜底","disk",
            "\\Windows\\Boot\\EFI\\bootmgfw.efi",fbname,false,false,"别人的 BOOTX64.EFI（GRUB 等）不覆盖"};
    } else {
        it[ni++]=(typeof(it[0])){"bootmgr","bootmgr（BIOS 启动管理器）","disk",
            "\\Windows\\Boot\\PCAT\\bootmgr","/bootmgr",false,false,"系统自带；版本与 winload 天然匹配"};
        it[ni++]=(typeof(it[0])){"bcd","BCD（内置模板现生成）","template",
            "builtin","/Boot/BCD",true,false,""};
        it[ni++]=(typeof(it[0])){"mbr_code","MBR 引导码（内置模板）","template",
            "builtin","LBA0[0..439]",true,false,""};
        it[ni++]=(typeof(it[0])){"vbr_code","NTFS VBR 引导码（内置模板）","template",
            "builtin","引导分区扇区 0..9",true,false,"mkntfs 不写这段，缺了 BIOS 报 A disk read error"};
    }

    /* 来源可用性（disk）
     * ★ 展示用 Windows 风格（\Windows\Boot\EFI\bootmgfw.efi），探测用 POSIX 风格
     *   （/Windows/Boot/EFI/bootmgfw.efi）—— ntfs 查找只认正斜杠（踩过）。 */
    if(sfs){
        for(int i=0;i<ni;i++){
            if(strcmp(it[i].from,"disk")) continue;
            char probe[300];
            snprintf(probe,sizeof probe,"%s",it[i].src);
            for(char *c=probe;*c;c++) if(*c=='\\') *c='/';
            it[i].avail = img_ntfs_path_exists(sfs,probe);
        }
    }
    /* 落点现状 */
    if(boot_b){
        if(uefi){
            img_fat32_fs *efs=img_fat32_mount(q,boot_lba,true);
            if(efs){
                for(int i=0;i<ni;i++)
                    if(!strcmp(it[i].id,"bcd")||!strcmp(it[i].id,"bootmgfw")||!strcmp(it[i].id,"boot_sdi")
                       ||!strcmp(it[i].id,"efi_fallback")||!strcmp(it[i].id,"fonts"))
                        it[i].present=img_fat32_path_exists(efs,it[i].dst);
                img_fat32_unmount(efs);
            }
        } else {
            img_ntfs_fs *bfs=img_ntfs_mount_rw(q,boot_lba,boot_b);
            if(bfs){
                for(int i=0;i<ni;i++)
                    if(!strcmp(it[i].id,"bootmgr")||!strcmp(it[i].id,"bcd"))
                        it[i].present=img_ntfs_path_exists(bfs,it[i].dst);
                img_ntfs_umount(bfs);
            }
            unsigned char z[512];
            if(img_qcow2_read(q,0,z,512)==IMG_OK){
                for(int i=0;i<ni;i++) if(!strcmp(it[i].id,"mbr_code")){
                    it[i].present=false;
                    for(int k=0;k<440;k++) if(z[k]){ it[i].present=true; break; }
                }
            }
            /* VBR：看引导分区扇区 1..9 有没有代码 */
            unsigned char v[512]; int vok=0;
            if(boot_lba && img_qcow2_read(q,(boot_lba+1)*512u,v,512)==IMG_OK)
                for(int k=0;k<512;k++) if(v[k]){ vok=1; break; }
            for(int i=0;i<ni;i++) if(!strcmp(it[i].id,"vbr_code")) it[i].present=(vok!=0);
        }
    }

    /* EFI 兜底冲突判定：三态 absent / ours / foreign */
    const char *fb_state= uefi?"absent":"none"; bool fb_conflict=false;   /* ESP 没格式化也算 absent */
    if(uefi && boot_b){
        img_fat32_fs *efs=img_fat32_mount(q,boot_lba,true);
        if(efs){
            void *fb=NULL; size_t fbn=0;
            if(img_fat32_read_file(efs,fbname,&fb,&fbn)==IMG_OK && fbn){
                void *src=NULL; size_t srcn=0;
                bool same=false;
                /* ★ 又一处反斜杠陷阱：ntfs 读只认正斜杠 */
                if(sfs && img_ntfs_read_file(sfs,"/Windows/Boot/EFI/bootmgfw.efi",&src,&srcn)==IMG_OK)
                    same=(fbn==srcn && memcmp(fb,src,srcn)==0);
                free(src);
                fb_state = same?"ours":"foreign";
                fb_conflict = !same;
                free(fb);
            } else fb_state="absent";
            img_fat32_unmount(efs);
        }
    }

    /* image 兜底：只有 disk 拿不到、又不是模板能给的物件才需要 ISO */
    bool need_image=false; char missing[200]="";
    for(int i=0;i<ni;i++){
        bool tmpl = !strcmp(it[i].from,"template");
        if(!tmpl && !it[i].avail && !it[i].present){
            need_image=true;
            strncat(missing,it[i].id,sizeof missing-strlen(missing)-2);
            strncat(missing," ",sizeof missing-strlen(missing)-2);
        }
    }
    if(!strcmp(src_mode,"disk")) need_image=false;    /* 用户选了 disk：缺就报缺，不推荐 ISO */

    /* WinRE（独立模块，只读）*/
    img_winre_status_t wr; memset(&wr,0,sizeof wr); char werr[256]="";
    img_winre_status(q,t,img_args_u64(in,"lba",0),&wr,werr,sizeof werr);
    if(sfs) img_ntfs_umount(sfs);

    /* ---- 输出 ---- */
    static char jb[4096];
    size_t off=0; jb[off++]='[';
    for(int i=0;i<ni;i++){
        char line[1400];
        /* ★ src/dst 里全是 Windows 路径反斜杠，不转义整条 JSON 就废了（踩过）*/
        char esrc[300], edst[300], enote[400], ewhat[200];
        jesc_put(it[i].src?it[i].src:"",esrc,sizeof esrc);
        jesc_put(it[i].dst?it[i].dst:"",edst,sizeof edst);
        jesc_put(it[i].note?it[i].note:"",enote,sizeof enote);
        jesc_put(it[i].what?it[i].what:"",ewhat,sizeof ewhat);
        snprintf(line,sizeof line,
            "%s{\"id\":\"%s\",\"what\":\"%s\",\"from\":\"%s\",\"src\":\"%s\",\"dst\":\"%s\","
            "\"available\":%s,\"present\":%s,\"note\":\"%s\"}",
            off>1?",":"", it[i].id,ewhat,it[i].from,esrc,edst,
            it[i].avail?"true":"false", it[i].present?"true":"false", enote);
        size_t l=strlen(line);
        if(off+l+2<sizeof jb){ memcpy(jb+off,line,l); off+=l; }
    }
    jb[off]=']'; jb[off+1]=0;
    img_result_raw(out,"items",jb);
    img_result_str(out,"src_policy",src_mode);
    img_result_bool(out,"needs_image",need_image);
    if(missing[0]) img_result_str(out,"missing_ids",missing);
    img_result_str(out,"efi_fallback_state",fb_state);
    img_result_bool(out,"efi_fallback_conflict",fb_conflict);

    img_result_bool(out,"winre_present",wr.winre_present);
    img_result_str(out,"wim_path",wr.wim_path);
    img_result_str(out,"winre_location",wr.winre_location);
    img_result_bool(out,"recovery_partition_present",wr.recovery_partition_present);
    img_result_bool(out,"recovery_partition_empty",wr.recovery_empty);
    if(werr[0]) img_result_str(out,"winre_note",werr);

    img_result_str(out,"verdict", need_image?"needs_image":"fixable");
    return IMG_OK;
}

static int cmd_boot_plan_repair(img_ctx *ctx, const img_args *in, img_result *out){
    return plan_repair_run(ctx,in,out);
}

/* ---------- qc.verify：qcow2 结构自检（别名/越界/坏 L2）---------- */
static const img_arg A_QCVERIFY[] = {
    {"img", IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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
        img_result_str(out,"note",img_t("映像里有别名/越界簇：能读，但请勿写入（会导致数据错乱）。"
                                  "重建或用 qemu-img check 看细节；我们写盘前已自动抬高分配水位。"));
    return IMG_OK;
}

/* ================= 版本识别：从【内容】读，不信文件名 =================
 * 用户点名的需求：挂载 ESD/WIM/ISO 时能直接看到是哪个 Windows 版本，
 * 从而判断"该镜像/该镜像到底是不是我以为的那个"。
 * ⚠ WIM 的 <NAME>/<VERSION> 是文本，第三方可以改；所以 win.version 还会去读
 *   盘里 ntoskrnl.exe 的 PE 版本资源（改了就开不了机）做交叉验证。*/


static const img_arg A_WIMID[] = {
    {"src",   IMG_A_STR, true,  NULL, "WIM / ESD / ISO 路径（ISO 会自动找 install.wim|esd）", NULL},
    {"index", IMG_A_U64, false, "0",  "只要第 N 个映像（0=全部）", NULL},
    {"inner", IMG_A_STR, false, NULL, "（ISO）指定内部路径，如 /x64/sources/install.wim", NULL},
    {"scan",  IMG_A_BOOL,false, "false","（ISO）列出里面所有 *.wim/*.esd（合一盘的 x86/x64 那种）", NULL},
    {"probe", IMG_A_ENUM,false, "auto", "位数判定：auto(仅当 XML 的 ARCH 不可信时才抽文件看 PE) | always | never", NULL},
};
/* JSON 字符串转义（WIM 里的 display_name 可能带引号/非 ASCII）*/
static void idj_esc(char *dst, size_t cap, const char *src){
    size_t k=0;
    for(const unsigned char *p=(const unsigned char*)src; p&&*p && k+8<cap; p++){
        if(*p=='"'||*p=='\\'){ dst[k++]='\\'; dst[k++]=(char)*p; }
        else if(*p<0x20) k+=(size_t)snprintf(dst+k,cap-k,"\\u%04x",*p);
        else dst[k++]=(char)*p;
    }
    dst[k]=0;
}
/* REQ-19 §B：用【内容】判位数（SysWOW64 / ntoskrnl 的 PE Machine）。
 * XML 的 <ARCH> 不能信（微软 x64 盘里也写 0）；这里判出来的直接覆盖它。
 * 无法识别 → out 置空串，调用方沿用 XML 归一化结果（宁缺勿错）。
 * ★ 不为了判位数去抽 4~8 GB：只有走 img_vfd 直读，或本来就是本地文件时才探。*/
static void wim_arch_content(const char *container, const char *inner_opt, int index,
                             char *out, size_t cap, char *src, size_t srccap)
{
    out[0]=0;
    if(src && srccap) src[0]=0;
    char p[1024]="", used[256]="", why[400]="";
    wimsrc_kind k=WIMSRC_PLAIN; uint64_t nb=0;
    if(wim_resolve_source(container,inner_opt,p,sizeof p,used,sizeof used,&k,&nb,why,sizeof why,NULL)!=IMG_OK)
        return;
    if(k==WIMSRC_EXTRACT){ wim_src_cleanup(p,k); return; }
    char a[16]="", w2[200]="", s2[32]="";
    if(img_wim_probe_arch(p,index,a,sizeof a,s2,sizeof s2,w2,sizeof w2)==IMG_OK && a[0]){
        snprintf(out,cap,"%s",a);
        if(src && srccap) snprintf(src,srccap,"%s",s2);
    }
    wim_src_cleanup(p,k);
}

/* 介质级判位数（REQ-22）：只看 ISO 里几十~几百字节的元数据，不抽取、不进 WIM。
 *   ① \sources\idwbinfo.txt 的 BuildArch=（微软官方介质都带：x86 / amd64 / arm64）
 *   ② 没有就看 \efi\boot\bootx64.efi / bootaa64.efi / bootia32.efi 哪个在
 * 只在"这个 ISO 里只有一条 install.wim"时用（合一盘各自不同，交给内容判据）。*/
static void iso_media_arch(const char *iso_path, char *out, size_t cap,
                           char *src, size_t srccap)
{
    out[0]=0;
    if(src && srccap) src[0]=0;
    img_iso *iso=NULL; char err[200]="";
    if(img_iso_open_path(&iso,iso_path,err,sizeof err)!=IMG_OK) return;
    uint64_t sz=0; bool isdir=false;
    if(img_iso_stat(iso,"/sources/idwbinfo.txt",&sz,&isdir)==IMG_OK && !isdir && sz>0 && sz<8192){
        char buf[8192]; memset(buf,0,sizeof buf);
        if(img_iso_read_range(iso,"/sources/idwbinfo.txt",0,buf,(size_t)sz)==IMG_OK){
            const char *p=strstr(buf,"BuildArch=");
            if(p){
                p+=10;
                char v[32]=""; size_t k=0;
                while(*p && *p!='\r' && *p!='\n' && k+1<sizeof v) v[k++]=*p++;
                v[k]=0;
                const char *a=NULL;
                if(!strcasecmp(v,"x86")||!strcasecmp(v,"i386")) a="x86";
                else if(!strcasecmp(v,"amd64")||!strcasecmp(v,"x64")||!strcasecmp(v,"x86_64")) a="x64";
                else if(!strcasecmp(v,"arm64")||!strcasecmp(v,"aarch64")) a="arm64";
                else if(!strcasecmp(v,"arm")) a="arm";
                if(a){ snprintf(out,cap,"%s",a); if(src&&srccap) snprintf(src,srccap,"idwbinfo"); }
            }
        }
    }
    if(!out[0]){
        static const struct { const char *path; const char *arch; } bt[]={
            {"/efi/boot/bootx64.efi","x64"},
            {"/efi/boot/bootaa64.efi","arm64"},
            {"/efi/boot/bootia32.efi","x86"},
        };
        for(size_t i=0;i<sizeof bt/sizeof bt[0];i++)
            if(img_iso_stat(iso,bt[i].path,&sz,&isdir)==IMG_OK && !isdir){
                snprintf(out,cap,"%s",bt[i].arch);
                if(src&&srccap) snprintf(src,srccap,"iso-bootfile");
                break;
            }
    }
    img_iso_close(iso);
}

/* 一条映像的最终 arch / basis / source（REQ-22 的唯一出口，两条路共用）*/
static void id_final_arch(const img_wim_image_t *im,
                          const char *acont, const char *asrc,
                          const char *mcont, const char *msrc,
                          const char **par, const char **pbasis, const char **psrc)
{
    if(acont && acont[0]){ *par=acont; *pbasis="content"; *psrc=(asrc&&asrc[0])?asrc:"content"; return; }
    /* XML 的 <ARCH>9/5/12 是【明确声明】，优先于 media —— media 判的是 ISO 级
     * （bootmgr 的 PE 位数），x86+x64 合一盘的 bootmgr 只代表其中一边，会骗人。 */
    if(im && im->arch[0] && strcmp(im->arch_raw,"0")!=0){
        *par=im->arch; *pbasis="xml"; *psrc="xml"; return;
    }
    if(mcont && mcont[0]){ *par=mcont; *pbasis="media";   *psrc=(msrc&&msrc[0])?msrc:"media";   return; }
    /* 只剩 XML：<ARCH>0 是【占位符】不是证据（微软官方盘普遍如此），不给 x86 */
    *par=""; *pbasis="none"; *psrc="none";
}

/* ★ snprintf 截断时返回的是"本该写多长"，把返回值累加到 k 上会让 k 超过
 *   缓冲区长度：此时 cb+k 已经出界，且 CB_CAP-k 变成巨大的 size_t，
 *   下一次 snprintf 就会写到界外（CodeQL: cpp/overflowing-snprintf）。
 *   这两个函数保证位置永远落在 [0, cap-1]、剩余长度永远 >= 1。*/
static size_t cb_pos(size_t k, size_t cap){ return k < cap ? k : cap - 1; }
static size_t cb_left(size_t k, size_t cap){ return k < cap ? cap - k : 1; }

static int cmd_wim_identify(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *src=img_args_get(in,"src");
    int want=(int)img_args_u64(in,"index",0);
    if(!src||!*src){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","需要映像路径",NULL); return IMG_EINVAL; }

    /* ★ REQ-22：probe=auto 时，只有 XML 的 <ARCH> 不可信（0 / 空 / 缺失）才去抽文件看 PE。
     *   理由：ARCH=9/5/12 是【明确声明】，微软那批"缺 flag"的盘写的正是 0；
     *   每次都抽会让释放页多花 1~3 秒（Win11 盘 6 个映像时实测 3.5 s → 0.7 s）。*/
    const char *pmode=img_args_get(in,"probe");
    bool probe_never  = pmode && !strcasecmp(pmode,"never");
    bool probe_always = pmode && !strcasecmp(pmode,"always");
    if(pmode && *pmode && !probe_never && !probe_always && strcasecmp(pmode,"auto")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","probe","只接受 auto|always|never",NULL);
        return IMG_EINVAL;
    }

    img_wim_image_t imgs[32]; int n=0; char err[256]="";
    char container[8]="wim", inner[64]="";
    const char *inner_kind=NULL;

    size_t sl=strlen(src);
    bool is_iso = (sl>4 && !strcasecmp(src+sl-4,".iso"));

    /* ★ REQ-16 §3A：--scan=true 列出 ISO 里【所有】 wim/esd 文件（含 x86/x64 那种合一盘），
     *   每条带上它自己的映像条目 —— App 的"卷索引"卡片直接照着列。*/
    if(is_iso && img_args_bool(in,"scan",false)){
        img_iso *iso=NULL;
        int rc=img_iso_open_path(&iso,src,err,sizeof err);
        if(rc!=IMG_OK){ img_result_fail(out,rc,iso_open_code(rc),src,err[0]?err:"ISO 无法打开",NULL); return rc; }
        wimscan_t sc; wim_scan_iso(iso,&sc);
        enum { CB_CAP = 131072 };
        char *cb=img_tbuf(IMG_TB_ID_SCAN,CB_CAP);
        if(!cb) return tb_nomem(out,"wim.identify 候选缓冲");
        size_t k=0; int emitted=0;
        k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),"[");
        char mcont[16]="", msrc[32]="";
        if(sc.n==1) iso_media_arch(src,mcont,sizeof mcont,msrc,sizeof msrc);
        for(int i=0;i<sc.n && k<CB_CAP-2048;i++){
            img_wim_image_t im2[32]; int n2=0; char e2[200]="";
            rd_iso_t r2={iso,sc.path[i]};
            int rrc=img_wim_identify(rd_iso,&r2,im2,32,&n2,e2,sizeof e2);
            size_t pl=strlen(sc.path[i]);
            const char *kind=(pl>4 && (sc.path[i][pl-1]=='d'||sc.path[i][pl-1]=='D')
                              && sc.path[i][pl-2]=='s')?"esd":"wim";
            k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),
                "%s{\"inner_path\":\"%s\",\"kind\":\"%s\",\"size_bytes\":%llu,\"ok\":%s,\"count\":%d",
                emitted?",":"",sc.path[i],kind,(unsigned long long)sc.size[i],
                (rrc==IMG_OK)?"true":"false",(rrc==IMG_OK)?n2:0);
            if(rrc==IMG_OK){
                k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),",\"images\":[");
                /* ★ 逐映像判位数（2026-11-05 修正）：
                 *   以前只探第 1 个映像然后把结果套给全部 —— x86+x64 合一盘
                 *   （Win7/Win10）的后 4 个 64 位映像全被标成 x86。
                 *   按 arch_raw 值缓存：同 raw 只探一次（一个 ESD 里 6 个版本只花 1 次探测）。*/
                char cache_raw[16]="", cache_cont[16]="", cache_src[32]="";
                for(int j=0;j<n2;j++){
                    char nm[256], dn[512], ln[128];
                    idj_esc(nm,sizeof nm,im2[j].name);
                    idj_esc(dn,sizeof dn,im2[j].display_name);
                    idj_esc(ln,sizeof ln,im2[j].languages);
                    char acont[16]="", asrc[32]="";
                    const char *rawj=im2[j].arch_raw;
                    bool need = !probe_never && (probe_always || !rawj[0] || !strcmp(rawj,"0"));
                    if(rrc==IMG_OK && need){
                        if(cache_raw[0] && !strcmp(cache_raw,rawj)){
                            snprintf(acont,sizeof acont,"%s",cache_cont);
                            snprintf(asrc,sizeof asrc,"%s",cache_src);
                        } else {
                            wim_arch_content(src,sc.path[i],im2[j].index,
                                             acont,sizeof acont,asrc,sizeof asrc);
                            snprintf(cache_raw,sizeof cache_raw,"%s",rawj);
                            snprintf(cache_cont,sizeof cache_cont,"%s",acont);
                            snprintf(cache_src,sizeof cache_src,"%s",asrc);
                        }
                    }
                    const char *ar1=NULL,*basis1=NULL,*asrc1=NULL;
                    id_final_arch(&im2[j],acont,asrc,mcont,msrc,&ar1,&basis1,&asrc1);
                    k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),
                        "%s{\"index\":%d,\"name\":\"%s\",\"display_name\":\"%s\","
                        "\"arch\":\"%s\",\"arch_raw\":\"%s\",\"arch_basis\":\"%s\",\"arch_source\":\"%s\","
                        "\"build\":%d,\"languages\":\"%s\",\"total_bytes\":%llu}",
                        j?",":"",im2[j].index,nm,dn,
                        ar1, im2[j].arch_raw, basis1, asrc1,
                        im2[j].build,ln,
                        (unsigned long long)im2[j].total_bytes);
                }
                k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),"]");
            } else {
                char e3[220]; idj_esc(e3,sizeof e3,e2[0]?e2:"解析失败");
                k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),",\"error\":\"%s\"",e3);
            }
            k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),"}");
            emitted++;
        }
        k+=(size_t)snprintf(cb+cb_pos(k,(size_t)CB_CAP),cb_left(k,(size_t)CB_CAP),"]");
        const char *vol=img_iso_volume_id(iso);
        img_iso_close(iso);
        img_result_str(out,"container","iso");
        img_result_str(out,"source",src);
        if(vol) img_result_str(out,"volume",vol);
        img_result_str(out,"mode","scan");
        img_result_u64(out,"count",(uint64_t)emitted);
        img_result_bool(out,"truncated",sc.trunc);
        img_result_raw(out,"candidates",cb);
        return IMG_OK;
    }

    if(is_iso){
        img_iso *iso=NULL;
        int rc=img_iso_open_path(&iso,src,err,sizeof err);
        if(rc!=IMG_OK){ img_result_fail(out,rc,iso_open_code(rc),src,err[0]?err:"ISO 无法打开",NULL); return rc; }
        /* ★ REQ-16 §3B：--inner 指定优先；不给就按老规则找（找不到时扫一遍，
         *   唯一一条就直接用，多条则要求 --inner）。*/
        char hitbuf[256]=""; char wh[400]="";
        int prc=wim_pick_inner(iso,img_args_get(in,"inner"),hitbuf,sizeof hitbuf,wh,sizeof wh);
        if(prc!=IMG_OK){
            img_iso_close(iso);
            bool amb=(prc==IMG_EINVAL);
            img_result_fail(out,prc,amb?"CMD-ARG-BAD":"WIM-NOTFOUND",
                amb?"inner":src, wh[0]?wh:"ISO 里没有 install.wim / install.esd",
                amb?"用 --scan=true 列出候选，再带 --inner=/x64/sources/install.wim":NULL);
            return prc;
        }
        const char *hit=hitbuf;
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

    /* ★ REQ-19 §B：内容判位数（SysWOW64 / ntoskrnl PE）覆盖 XML 的 <ARCH>
     *   ★ 2026-11-05 修正：逐映像探测 + 按 raw 值缓存（以前只探 imgs[0] 套全部，
     *     x86+x64 合一盘的 64 位映像全被标成 x86 —— Win7 九合一实测踩中）。
     *   media（bootmgr PE）作为 ISO 级兜底保留，但 id_final_arch 里 XML 声明优先于它。 */
    char mcont[16]="", msrc[32]="";
    if(is_iso) iso_media_arch(src, mcont,sizeof mcont, msrc,sizeof msrc);

    enum { JB_CAP = 65536 };
    char *jb=img_tbuf(IMG_TB_ID_IMAGES,JB_CAP);
    if(!jb) return tb_nomem(out,"wim.identify 映像缓冲");
    size_t k=0;
    k+=(size_t)snprintf(jb+k,JB_CAP-k,"[");
    int emitted=0;
    char cache_raw2[16]="", cache_cont2[16]="", cache_src2[32]="";
    for(int i=0;i<n;i++){
        if(want>0 && imgs[i].index!=want) continue;
        char acont[16]="", asrc[32]="";
        const char *rawi=imgs[i].arch_raw;
        bool need = !probe_never && (probe_always || !rawi[0] || !strcmp(rawi,"0"));
        if(need){
            if(cache_raw2[0] && !strcmp(cache_raw2,rawi)){
                snprintf(acont,sizeof acont,"%s",cache_cont2);
                snprintf(asrc,sizeof asrc,"%s",cache_src2);
            } else {
                wim_arch_content(src, inner[0]?inner:NULL, imgs[i].index,
                                 acont,sizeof acont, asrc,sizeof asrc);
                snprintf(cache_raw2,sizeof cache_raw2,"%s",rawi);
                snprintf(cache_cont2,sizeof cache_cont2,"%s",acont);
                snprintf(cache_src2,sizeof cache_src2,"%s",asrc);
            }
        }
        const char *ar2=NULL,*basis2=NULL,*asrc2=NULL;
        id_final_arch(&imgs[i],acont,asrc,mcont,msrc,&ar2,&basis2,&asrc2);
        k+=(size_t)snprintf(jb+k,JB_CAP-k,
            "%s{\"index\":%d,\"name\":\"%s\",\"display_name\":\"%s\",\"edition_id\":\"%s\","
            "\"arch\":\"%s\",\"arch_raw\":\"%s\",\"arch_basis\":\"%s\",\"arch_source\":\"%s\","
            "\"version\":\"%d.%d.%d\",\"build\":%d,\"languages\":\"%s\","
            "\"dirs\":%d,\"files\":%d,\"total_bytes\":%llu}",
            emitted?",":"", imgs[i].index, imgs[i].name, imgs[i].display_name, imgs[i].edition_id,
            ar2, imgs[i].arch_raw, basis2, asrc2,
            imgs[i].major, imgs[i].minor, imgs[i].build, imgs[i].build,
            imgs[i].languages, imgs[i].dir_count, imgs[i].file_count,
            (unsigned long long)imgs[i].total_bytes);
        emitted++;
        if(k>JB_CAP-2048) break;
    }
    k+=(size_t)snprintf(jb+k,JB_CAP-k,"]");
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
    {"img", IMG_A_STR, true, NULL, "映像路径（qcow2）", NULL},
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
        img_result_str(out,"note",img_t("多个证据源互相矛盾，该映像被改过（精简/换版本），请勿依据文件名判断"));
    else if(v.n_vote<=1)
        img_result_str(out,"note",img_t("只有 1 个证据源可用（核心文件疑似被 WOF/CompactOS 压缩），结论可信度低"));
    return IMG_OK;
}


/* ================= 命令 6f：ntfs.move（盘内改名 / 移动，任意单条路径）====================
 * 一次覆盖三种诉求：a.txt→b.txt（同目录改名）、/a.txt→/Temp/a.txt（移动）、
 * /Temp/a.txt→/Temp/b.txt（既换目录又换名）。dst 的末段就是新名字。
 * ★ 政策：默认【不覆盖】—— 覆盖会吃掉目标文件，必须 App 显式 overwrite=true。 */
#if IMG_WITH_NTFS3G   /* 只在开启 NTFS 时用到；关掉时不该留下 unused 警告 */
/* 把 img_ntfs_move_path 的返回码翻成【能分开病因】的错误码：
 * "源不存在"/"目标已存在"/"目标是目录"/"空间不够"/"底层失败" 各一条 ——
 * 之前踩过"所有失败共用一个码"的坑（排查时分不出打错字还是卷坏了）。 */
/* 共用：按 lba 从分区表取分区字节数；取不到返回 0 */
#endif /* IMG_WITH_NTFS3G */

/* ================= 命令 6g：ntfs.rm（删盘内文件）====================
 * ★ 政策：只删【文件】。目录一律拒绝（img_ntfs_rm 内部就拒）——
 *   递归删的误操作比"删不掉"可怕得多。 */
/* ---------- 盘内复制（NTFS） ---------- */

/* ================= 时间戳设置（ntfs.touch / fat.touch） =================
 * 语义照 edk2 那套（用户给的资料）：**传 0 = 该项保持原值**，不更新。
 * 三种取值来源（三选一）：--now（现在）/ --time=<unix 秒> / --ref=<盘内另一路径，抄它的时间）。
 * --set=all|create|modify|access 选要动哪几个（默认 all）。*/
static const img_arg A_NTFSTOUCH[] = {
    {"img",  IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
    {"lba",  IMG_A_U64, true,  NULL, "NTFS 分区起始 LBA", chk_lba},
    {"path", IMG_A_STR, true,  NULL, "盘内目标路径（/ 开头）", NULL},
    {"now",  IMG_A_BOOL, false, "false", "把时间设成【现在】", NULL},
    {"time", IMG_A_U64, false, "0",     "把时间设成这个 unix 秒（0=不改）", NULL},
    {"ref",  IMG_A_STR, false, NULL,    "抄这个盘内路径的时间（clone 时间戳）", NULL},
    {"set",  IMG_A_ENUM, false, "all",  "改哪些：all | create | modify | access", NULL},
};
static const img_arg A_FATTOUCH[] = {
    {"img",  IMG_A_STR, true,  NULL, "映像路径（qcow2）", NULL},
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

/* 把任意路径转义成 JSON 字符串内容（不写外层引号）。
 * 文件名里可以有 " \ 换行 —— 直接 snprintf("%s") 会产出非法 JSON，
 * App 解析失败时只会看到"未知错误"，比复制失败更难查。 */
static void jesc(char *out, size_t cap, const char *in){
    size_t o=0;
    if(!cap) return;
    for(const unsigned char *p=(const unsigned char*)in; p && *p; p++){
        if(o+8>=cap) break;
        switch(*p){
        case '"':  out[o++]='\\'; out[o++]='"';  break;
        case '\\': out[o++]='\\'; out[o++]='\\'; break;
        case '\n': out[o++]='\\'; out[o++]='n';  break;
        case '\r': out[o++]='\\'; out[o++]='r';  break;
        case '\t': out[o++]='\\'; out[o++]='t';  break;
        default:
            if(*p<0x20) o+=(size_t)snprintf(out+o,cap-o,"\\u%04x",*p);
            else out[o++]=(char)*p;
        }
    }
    out[o]=0;
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
                      uint64_t size,uint64_t mtime,uint32_t attrs,size_t depth){
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
                       "\"size\":%llu,\"mtime\":%llu,\"attrs\":%u,\"depth\":%zu}",
                   a->n?",":"",nm,fp,isdir?"true":"false",
                   (unsigned long long)size,(unsigned long long)mtime,(unsigned)attrs,depth);
    if(a->len+(size_t)k+1 < a->cap){ memcpy(a->b+a->len,item,(size_t)k); a->len+=(size_t)k; a->n++; }
    else a->trunc=true;
}
/* --wim 可以指 WIM/ESD 本体，也可以指含它的 ISO。返回本地可用路径；
 * is_tmp=true 时调用方用完必须 unlink。note/nsecap 回填"替你做了什么"。*/
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
    {"wim",       IMG_A_STR, true,  NULL,   "WIM/ESD 路径；也可以直接给含它的 ISO（工具自动定位 install.wim/.esd）", NULL},
    {"inner",     IMG_A_STR, false, NULL,   "（ISO）内部路径，如 /x64/sources/install.wim", NULL},
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

    char local[1024]="", used_inner[256]="", why[400]="";
    wimsrc_kind wk=WIMSRC_PLAIN; uint64_t nbytes=0;
    int rc=wim_resolve_source(srcp,img_args_get(in,"inner"),local,sizeof local,
                              used_inner,sizeof used_inner,&wk,&nbytes,why,sizeof why,img_cmd_prog());
    if(rc!=IMG_OK) return wim_src_fail(out,rc,srcp,why);
    /* ★ 线程局部，理由同 iso.ls（无 ctx 命令可并行 + raw 只存指针不能就地 free）*/
    enum { JB_CAP = 524288 };
    char *jb=img_tbuf(IMG_TB_WIM_LS,JB_CAP);
    if(!jb) return tb_nomem(out,"wim.ls 条目缓冲");
    jb[0]=(char)0x5b; wimlist_t acc={jb,JB_CAP-2,1,0,false,{0},0};   /* ★ len 从 1 起：第 0 格是 '['，别覆盖掉 */
    if(contains&&contains[0]) snprintf(acc.contains,sizeof acc.contains,"%s",contains);
    acc.limit=(size_t)lim64;
    char err[256]="";
    /* 先辨一下目标到底是目录还是文件：wimlib 对文件路径列"子项"会安静返回 0 条，
     * App 看到 count=0 会以为目录是空的 —— 那是误导，必须显式报错。*/
    {   img_wim_stat_t est; memset(&est,0,sizeof est);
        if(strcmp(dir,"/")!=0 && img_wim_stat(local,index,dir,&est,err,sizeof err)==IMG_OK && !est.is_dir){
            wim_src_cleanup(local,wk);
            img_result_fail(out,IMG_EINVAL,"WIM-NOT-A-DIR",dir,"这是文件不是目录；抽文件用 wim.extract",NULL);
            return IMG_EINVAL;
        }
    }
    img_span sp; img_span_prep(&sp,"wim","ls","列 WIM/ESD 目录");
    img_span_kv_str(&sp,"path",dir); img_span_begin(&sp);
    rc=img_wim_list(local,index,dir,rec,wim_ls_cb,&acc,err,sizeof err);
    img_span_end(&sp,rc);
    wim_src_cleanup(local,wk);                    /* ★ 用完就删，不把 4~8 GB 垃圾留在用户机上 */
    if(rc!=IMG_OK){
        img_result_fail(out,rc,wim_fs_code(rc,1),dir,err[0]?err:"列目录失败",NULL);
        return rc;
    }
    jb[acc.len++]=']'; jb[acc.len]=0;
    img_result_u64(out,"count",(uint64_t)acc.n);
    img_result_bool(out,"truncated",acc.trunc);
    img_result_bool(out,"recursive",rec);
    img_result_str(out,"path",dir);
    if(wk!=WIMSRC_PLAIN){
        img_result_str(out,"container","iso");
        img_result_str(out,"inner_path",used_inner);
        img_result_bool(out,"direct", wk==WIMSRC_VFD);
        if(wk==WIMSRC_EXTRACT){ img_result_bool(out,"extracted",true); img_result_u64(out,"extracted_bytes",nbytes); }
    }
    img_result_raw(out,"entries",jb);
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}
/* --dst=- / --dst-fd=N 的消费者：把管道里的字节原样写到目标 fd（REQ-29）*/
typedef struct { int fd; uint64_t total; } stream_sink_t;
static int stream_sink_cb(void *ud, int rfd, uint64_t total, char *err, size_t errcap){
    (void)total;
    stream_sink_t *sk=(stream_sink_t*)ud;
    char *buf=(char*)malloc(1<<20);
    if(!buf){ if(err&&errcap) snprintf(err,errcap,"内存不足"); return IMG_ENOMEM; }
    int rc=IMG_OK;
    for(;;){
        ssize_t n=read(rfd,buf,1<<20);
        if(n<0){ if(errno==EINTR) continue; rc=IMG_EIO; break; }
        if(n==0) break;
        size_t done=0;
        while(done<(size_t)n){
            ssize_t w=write(sk->fd,buf+done,(size_t)n-done);
            if(w<0){ if(errno==EINTR) continue; rc=IMG_EIO; break; }
            done+=(size_t)w;
        }
        if(rc!=IMG_OK) break;
        sk->total+=(uint64_t)n;
    }
    free(buf);
    if(rc!=IMG_OK && err&&errcap&&!err[0]) snprintf(err,errcap,"写到 fd %d 失败：%s",sk->fd,strerror(errno));
    return rc;
}

static const img_arg A_WIMEXTRACT[] = {
    {"wim",   IMG_A_STR, true,  NULL, "WIM/ESD 路径；也可以直接给含它的 ISO", NULL},
    {"inner", IMG_A_STR, false, NULL, "（ISO）内部路径，如 /x64/sources/install.wim", NULL},
    {"index", IMG_A_U64, false, "1",  "映像索引", NULL},
    {"path",  IMG_A_STR, true,  NULL, "WIM 内路径（/ 开头）；给目录就抽整棵子树（REQ-28）", NULL},
    {"dst",   IMG_A_STR, true,  NULL, "本地输出文件（会覆盖；给 - 表示写 stdout，REQ-29）", NULL},
    {"dst-fd",IMG_A_U64, false, "0",  "（REQ-29）直接写到这个 fd，不落临时文件（0=用 --dst）", NULL},
};
static int cmd_wim_extract(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 取文件）","重新编译时开启 IMG_WITH_WIM 并安装 libwim");
#if IMG_WITH_WIM
    const char *srcp=img_args_get(in,"wim"), *ip=img_args_get(in,"path"), *dst=img_args_get(in,"dst");
    int index=(int)img_args_u64(in,"index",1);
    if(!srcp||!ip||!dst){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","wim/path/dst","三个都必填",NULL); return IMG_EINVAL; }
    /* ★ REQ-29：--dst=- / --dst-fd=N → 流式写出去（wimlib TO_STDOUT → pipe → 目标 fd），
     *   不落临时文件。App 可以直接 | 给别的消费者，或者配合 wim.put 直接灌盘。*/
    int sink=img_args_u64(in,"dst-fd",0)>0?(int)img_args_u64(in,"dst-fd",0):0;
    bool to_stream = (sink>0) || !strcmp(dst,"-");
    if(to_stream && sink<=0) sink=1;
    if(to_stream && img_args_u64(in,"dst-fd",0)>0 && !strcmp(dst,"-")) sink=(int)img_args_u64(in,"dst-fd",0);
    if(ip[0]!='/'){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","WIM 内路径必须以 / 开头",NULL); return IMG_EINVAL; }
    char local[1024]="", used_inner[256]="", why[400]="";
    wimsrc_kind wk=WIMSRC_PLAIN; uint64_t nbytes=0;
    img_prog *prog=img_cmd_prog();
    int rc=wim_resolve_source(srcp,img_args_get(in,"inner"),local,sizeof local,
                              used_inner,sizeof used_inner,&wk,&nbytes,why,sizeof why,prog);
    if(rc!=IMG_OK) return wim_src_fail(out,rc,srcp,why);
    char err[256]=""; uint64_t got=0;
    /* ★ 错误码分级：源准备失败 = 文件层面的问题（HOST-OPEN-FAIL 那一族），
     *   抽取失败 = 映像【内部路径】的问题（WIM-PATH-NOTFOUND）——
     *   之前一律按 kind=2 翻，把"里面没这个文件"报成"文件打不开"，方向完全带偏。*/
    if(to_stream){
        /* 日志/进度必须让路：fd 1 这时是【数据】，混进去就是坏文件 */
        int old_log=img_log_out_fd();
        bool silenced=false;
        if(sink==1 && old_log==1){ img_log_set_out_fd(-1); silenced=true; }
        /* ★ 目标 fd 必须先 dup 出来：wimlib 会把管道写端 dup2 到 fd 1，
         *   如果 sink 也是 1（--dst=-），消费者就会"从管道读、再写回管道"自己喂自己
         *   → 谁也排不干净 → 死锁（踩过：跑到一半两边都不动）。*/
        int out_fd=dup(sink);
        if(out_fd<0){
            if(silenced) img_log_set_out_fd(1);
            wim_src_cleanup(local,wk);
            img_result_fail(out,IMG_EIO,"HOST-OPEN-FAIL",dst,"目标 fd 复制失败","检查 --dst-fd 是否有效");
            return IMG_EIO;
        }
        stream_sink_t sk; memset(&sk,0,sizeof sk); sk.fd=out_fd;
        uint64_t ssz=0; char serr[240]="";
        int orc=img_wim_stream_consume(local,index,ip,stream_sink_cb,&sk,&ssz,
                                       silenced?NULL:img_cmd_prog(),serr,sizeof serr);
        close(out_fd);
        if(silenced) img_log_set_out_fd(1);
        wim_src_cleanup(local,wk);
        if(orc!=IMG_OK){
            img_result_fail(out,orc,wim_fs_code(orc,1),ip,serr[0]?serr:"流式取文件失败",NULL);
            return orc;
        }
        img_result_str(out,"path",ip);
        img_result_str(out,"dst",dst);
        img_result_u64(out,"bytes",sk.total);
        img_result_u64(out,"stream_fd",(uint64_t)sink);
        if(wk!=WIMSRC_PLAIN){
            img_result_str(out,"container","iso");
            img_result_str(out,"inner_path",used_inner);
            img_result_bool(out,"direct", wk==WIMSRC_VFD);
        }
        return IMG_OK;
    }

    /* ★ REQ-28 C″：--path 给的是目录就抽整棵子树（否则以前直接 ENOTSUP 让人干瞪眼）*/
    img_wim_stat_t wst; memset(&wst,0,sizeof wst);
    int src_rc=img_wim_stat(local,index,ip,&wst,err,sizeof err);
    if(src_rc!=IMG_OK){ wim_src_cleanup(local,wk);
        img_result_fail(out,src_rc,wim_fs_code(src_rc,1),ip,err[0]?err:"WIM 里没有这个路径",NULL);
        return src_rc; }
    int files=0;
    img_span sp; img_span_prep(&sp,"wim","extract",
                               wst.is_dir?"从 WIM/ESD 抽目录子树":"从 WIM/ESD 抽单个文件");
    img_span_kv_str(&sp,"path",ip); img_span_begin(&sp);
    if(wst.is_dir) rc=img_wim_extract_dir(local,index,ip,dst,prog,&got,&files,err,sizeof err);
    else           rc=img_wim_extract_file(local,index,ip,dst,prog,&got,err,sizeof err);
    img_span_kv_u64(&sp,"written",got);
    img_span_end(&sp,rc);
    wim_src_cleanup(local,wk);
    if(rc!=IMG_OK){
        if(rc==IMG_ECANCELLED) unlink(dst);       /* 取消不留半成品 */
        img_result_fail(out,rc,wim_fs_code(rc,1),ip,err[0]?err:"抽取失败",NULL);
        return rc;
    }
    img_result_str(out,"path",ip);
    img_result_str(out,"dst",dst);
    img_result_bool(out,"is_dir",wst.is_dir);
    if(wst.is_dir) img_result_u64(out,"files",(uint64_t)files);
    img_result_u64(out,"bytes",got);
    if(wk!=WIMSRC_PLAIN){
        img_result_str(out,"container","iso");
        img_result_str(out,"inner_path",used_inner);
        img_result_bool(out,"direct", wk==WIMSRC_VFD);
    }
    return IMG_OK;
#else
    (void)ctx;(void)in;(void)out; return IMG_ENOTSUP;
#endif
}


/* ================= wim.file-version（REQ-37 ②）=================
 * 只读一个文件的 PE 版本资源：给 App 做"跨映像取引导文件"的版本闸门 ——
 * 系统分区的 win.version 给 build，候选映像里 bootmgr 的 file_version 给 build，
 * 两边主版本+build 段一致才允许用。
 * ★ 不落临时文件：走 img_wim_stream_consume（wimlib TO_STDOUT → pipe → 内存）。
 *   版本资源只在前几百 KB，但文件可能很大 —— 超过 16 MB 只收前面，余下读掉丢弃
 *   （不读掉 wimlib 写管道会卡住，和 REQ-29 同一个坑）。*/
#define WFV_MEM_MAX (16u<<20)
typedef struct { uint8_t *b; size_t len; uint64_t total; } wfv_mem_t;
static int wfv_mem_cb(void *ud, int rfd, uint64_t total, char *err, size_t errcap){
    (void)total;
    wfv_mem_t *m=(wfv_mem_t*)ud;
    m->b=(uint8_t*)malloc(WFV_MEM_MAX);
    if(!m->b){ if(err&&errcap) snprintf(err,errcap,"内存不足"); return IMG_ENOMEM; }
    char buf[65536];
    for(;;){
        ssize_t n=read(rfd,buf,sizeof buf);
        if(n<0){
            if(errno==EINTR) continue;
            if(err&&errcap) snprintf(err,errcap,"读管道失败：%s",strerror(errno));
            return IMG_EIO;
        }
        if(n==0) break;
        m->total+=(uint64_t)n;
        size_t room=WFV_MEM_MAX-m->len;
        if(room){
            size_t cp=(size_t)n<room?(size_t)n:room;
            memcpy(m->b+m->len,buf,cp); m->len+=cp;
        }
    }
    return IMG_OK;
}
/* 映像内路径规范化：`\Windows\Boot\PCAT\bootmgr` 与 `/Windows/...` 等价 */
static void wim_path_norm(const char *in, char *out, size_t cap){
    if(!cap) return;
    if(!in || !*in){ out[0]=0; return; }
    size_t k=0;
    for(const char *p=in; *p && k+2<cap; p++) out[k++]=(*p=='\\')?'/':*p;
    out[k]=0;
    if(out[0]!='/'){ memmove(out+1,out,k+1); out[0]='/'; }
}
static const img_arg A_WIMFILEVER[] = {
    {"src",   IMG_A_STR, false, NULL, "WIM/ESD 路径；也可以直接给含 install.wim/.esd 的 ISO", NULL},
    {"wim",   IMG_A_STR, false, NULL, "--src 的别名（与 wim.ls / wim.extract 的写法一致）", NULL},
    {"inner", IMG_A_STR, false, NULL, "（ISO）内部路径，如 /x64/sources/install.wim", NULL},
    {"index", IMG_A_U64, false, "1",  "映像索引", NULL},
    {"path",  IMG_A_STR, true,  NULL, "映像里的文件路径（/ 或 \\ 都行，大小写不敏感）", NULL},
};
static int cmd_wim_file_version(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（读映像里的文件版本）","重新编译时开启 IMG_WITH_WIM 并安装 libwim");
#if IMG_WITH_WIM
    const char *srcp=img_args_get(in,"src");
    if(!srcp||!*srcp) srcp=img_args_get(in,"wim");
    const char *raw=img_args_get(in,"path");
    if(!srcp||!*srcp||!raw||!*raw){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD",(!srcp||!*srcp)?"src":"path",
                        "需要 --src（WIM/ESD/ISO）与 --path（映像内文件路径）",NULL);
        return IMG_EINVAL;
    }
    int index=(int)img_args_u64(in,"index",1);
    char ip[512]; wim_path_norm(raw,ip,sizeof ip);

    char local[1024]="", used_inner[256]="", why[400]="";
    wimsrc_kind wk=WIMSRC_PLAIN; uint64_t nbytes=0;
    img_prog *prog=img_cmd_prog();
    int rc=wim_resolve_source(srcp,img_args_get(in,"inner"),local,sizeof local,
                              used_inner,sizeof used_inner,&wk,&nbytes,why,sizeof why,prog);
    if(rc!=IMG_OK) return wim_src_fail(out,rc,srcp,why);

    wfv_mem_t m; memset(&m,0,sizeof m);
    uint64_t fsz=0; char err[256]="";
    int orc=img_wim_stream_consume(local,index,ip,wfv_mem_cb,&m,&fsz,prog,err,sizeof err);
    wim_src_cleanup(local,wk);
    if(orc!=IMG_OK){
        free(m.b);
        img_result_fail(out,orc,wim_fs_code(orc,1),ip,err[0]?err:"读映像里的文件失败",NULL);
        return orc;
    }

    img_result_bool(out,"found",true);
    img_result_str(out,"path",ip);
    img_result_u64(out,"size_bytes",fsz);
    img_result_u64(out,"image_index",(uint64_t)index);
    if(wk!=WIMSRC_PLAIN){
        img_result_str(out,"container","iso");
        img_result_str(out,"inner_path",used_inner);
        img_result_bool(out,"direct", wk==WIMSRC_VFD);
    }

    /* PE 版本资源解析（与 win.version 读 ntoskrnl 是同一份实现）*/
    img_winver_t v; memset(&v,0,sizeof v);
    int prc=img_pe_version(m.b,m.len,&v);
    if(prc==IMG_OK){
        img_result_bool(out,"pe",true);
        img_result_bool(out,"has_version",true);
        img_result_str(out,"file_version",v.kernel_file_version);
        img_result_str(out,"product_version",v.kernel_product_version);
        img_result_u64(out,"major",(uint64_t)v.kernel_major);
        img_result_u64(out,"minor",(uint64_t)v.kernel_minor);
        img_result_u64(out,"build",(uint64_t)v.kernel_build);
        unsigned r1=0,r2=0,r3=0,r4=0;
        if(sscanf(v.kernel_file_version,"%u.%u.%u.%u",&r1,&r2,&r3,&r4)==4)
            img_result_u64(out,"revision",(uint64_t)r4);
        const char *am = v.pe_machine==0x14c ? "x86" : v.pe_machine==0x8664 ? "x64"
                       : v.pe_machine==0xAA64 ? "arm64" : v.pe_machine==0x1c4 ? "arm" : NULL;
        if(am) img_result_str(out,"arch",am);
        if(v.pe_machine) img_result_u64(out,"pe_machine",(uint64_t)v.pe_machine);
        /* bootmgr 这种前面带 16 位引导壳的，PE 不在偏移 0 —— 报出来省得 App 当异常 */
        if(v.pe_offset) img_result_u64(out,"pe_offset",(uint64_t)v.pe_offset);
        if(v.kernel_company[0])     img_result_str(out,"company",v.kernel_company);
        if(v.kernel_description[0]) img_result_str(out,"description",v.kernel_description);
    }else{
        /* img_pe_version 的两种失败要分开：ENOENT = 认出了 PE 但没有版本资源，
         * EFORMAT = 根本找不到 PE 头（数据文件）。*/
        bool ispe=(prc==IMG_ENOENT);
        img_result_bool(out,"pe",ispe);
        img_result_bool(out,"has_version",false);
        img_result_str(out,"reason", ispe
            ? "这是 PE，但没有 VS_VERSIONINFO（数据文件/资源被剥掉了），不能拿它做版本比较"
            : "不是 PE 文件（没有 MZ 头），不能拿它做版本比较");
    }
    free(m.b);
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
/* ================= 注册表通用编辑（P2，REQ-P2）=================
 * 4 条命令：reg.get / reg.set / reg.list / reg.del
 *
 * 共同参数：
 *   img  qcow2 路径
 *   lba  Windows 系统分区（NTFS）起始 LBA
 *   hive SYSTEM | SOFTWARE | SAM | SECURITY | DEFAULT（大小写不敏感）
 *
 * 路径统一写成 `hive\子键\...`，由 reg_path_split 解析。
 * 事务性见 img_reg.h：抽出→改→校验→写回，任一步失败分区字节不变。
 */

/* 把 "SYSTEM\Setup\Status\ChildCompletion" 拆成 hive + key。
 * 也接受 "/" 分隔。 */
static int reg_path_split(const char *full, img_reg_hive *hive,
                          char *key, size_t keycap, const char **rest_value,
                          char *err, size_t errcap){
    if(!full || !*full){
        if(err) snprintf(err,errcap,"路径为空（形如 SYSTEM\\Setup\\Status）");
        return IMG_EINVAL;
    }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", full);
    for(char *p = buf; *p; p++) if(*p == '/') *p = '\\';

    char *sep = strchr(buf, '\\');
    char hive_name[32];
    if(sep){
        size_t n = (size_t)(sep - buf);
        if(n >= sizeof hive_name){ if(err) snprintf(err,errcap,"hive 名过长"); return IMG_EINVAL; }
        memcpy(hive_name, buf, n); hive_name[n] = 0;
    } else {
        snprintf(hive_name, sizeof hive_name, "%s", buf);
        sep = buf + strlen(buf);
    }
    if(!img_reg_hive_from_name(hive_name, hive)){
        if(err) snprintf(err,errcap,
            "未知 hive「%s」（只能是 SYSTEM/SOFTWARE/SAM/SECURITY/DEFAULT）", hive_name);
        return IMG_EINVAL;
    }
    snprintf(key, keycap, "%s", (*sep == '\\') ? sep + 1 : sep);
    if(rest_value) *rest_value = key;
    return IMG_OK;
}

/* ---- reg.get ---- */
static const img_arg A_REGGET[] = {
    {"img",   IMG_A_STR,  true,  NULL, "映像路径（qcow2）", NULL},
    {"lba",   IMG_A_U64,  true,  NULL, "Windows 系统分区起始 LBA", chk_lba},
    {"path",  IMG_A_STR,  true,  NULL, "值路径：SYSTEM\\Setup\\Status\\ChildCompletion\\setup.exe", NULL},
    {"hex",   IMG_A_BOOL, false, "false","REG_BINARY 以十六进制字符串输出（默认 base64）", NULL},
};
static int cmd_reg_get(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba = img_args_u64(in,"lba",0);
    const char *full = img_args_get(in,"path");
    bool as_hex = img_args_bool(in,"hex",false);
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);

    img_reg_hive hive; char keybuf[900]; char err[256] = "";
    int rc = reg_path_split(full, &hive, keybuf, sizeof keybuf, NULL, err, sizeof err);
    if(rc != IMG_OK){
        img_result_fail(out, rc, "REG-PATH-BAD", full, err, NULL);
        return rc;
    }
    /* 最后一段是值名 */
    char *last = strrchr(keybuf, '\\');
    char keypath[900], valname[300];
    if(last){ *last = 0; snprintf(keypath,sizeof keypath,"%s",keybuf); snprintf(valname,sizeof valname,"%s",last+1); }
    else    { keypath[0] = 0; snprintf(valname,sizeof valname,"%s",keybuf); }

    img_parttable_t *t=NULL;
    rc = img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb = 0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }

    img_reg *r = img_reg_open(img_ctx_path(ctx), lba, pb, hive, false, err, sizeof err);
    if(!r){
        img_result_fail(out, IMG_ESTATE, "REG-OPEN-FAIL", img_ctx_path(ctx), err,
            "这个分区可能不是 Windows 系统分区，或 LBA 给错");
        return IMG_ESTATE;
    }
    img_reg_value v;
    rc = img_reg_get(r, keypath, valname, &v, err, sizeof err);
    img_reg_close(r);
    if(rc != IMG_OK){
        const char *code = (rc==IMG_ENOENT)?"REG-NOTFOUND":"REG-GET-FAIL";
        img_result_fail(out, rc, code, full, err, NULL);
        return rc;
    }
    img_result_str(out, "hive", img_reg_hive_name(hive));
    img_result_str(out, "key", keypath);
    img_result_str(out, "name", v.name ? v.name : "");
    img_result_str(out, "type", img_reg_type_name(v.type));
    img_result_u64(out, "bytes", (uint64_t)v.len);

    /* 按类型给出可读值 */
    switch(v.type){
    case IMG_REG_DWORD:
        if(v.len >= 4){
            uint32_t d = (uint32_t)v.data[0] | ((uint32_t)v.data[1]<<8)
                       | ((uint32_t)v.data[2]<<16) | ((uint32_t)v.data[3]<<24);
            img_result_u64(out, "value", d);
            char sx[32]; snprintf(sx,sizeof sx,"0x%08X",d); img_result_str(out,"hex",sx);
        }
        break;
    case IMG_REG_QWORD:
        if(v.len >= 8){
            uint64_t d = 0;
            for(int i=7;i>=0;i--) d = (d<<8) | v.data[i];
            img_result_u64(out, "value", d);
        }
        break;
    case IMG_REG_SZ:
    case IMG_REG_EXPAND_SZ:
    case IMG_REG_MULTI_SZ:
        img_result_str(out, "value", (const char*)v.data);   /* hivex 已转 UTF-8 */
        break;
    default:
        if(as_hex){
            size_t cap = v.len*2 + 1;
            char *hx = (char*)malloc(cap);
            if(hx){
                for(size_t i=0;i<v.len;i++) snprintf(hx+i*2,3,"%02X",v.data[i]);
                hx[cap-1]=0;
                img_result_str(out,"value_hex",hx);
                free(hx);
            }
        }
        break;
    }
    img_reg_value_free(&v);
    return IMG_OK;
}

/* ---- reg.list ---- */
static const img_arg A_REGLIST[] = {
    {"img",        IMG_A_STR,  true,  NULL, "映像路径（qcow2）", NULL},
    {"lba",        IMG_A_U64,  true,  NULL, "Windows 系统分区起始 LBA", chk_lba},
    {"path",       IMG_A_STR,  false, NULL, "键路径：SYSTEM\\Setup\\Status（空=根）", NULL},
    {"subkeys",    IMG_A_BOOL, false, "true","列子键", NULL},
    {"values",     IMG_A_BOOL, false, "true","列值", NULL},
};
/* ★ reg.list 明细（2026-11-09）：App 要做注册表浏览器，只回 count 不够。
 *   子项拼进 JSON 数组：{"name":"...","kind":"key"|"value","type":"REG_SZ","bytes":N} */
enum { REGLS_CAP = 262144 };
typedef struct {
    img_result *out;
    int n;
    char *buf; size_t cap, len;
    bool truncated;
} reg_ls_ud;
static void reg_ls_cb(void *ud, const char *name, bool is_key, img_reg_type type, size_t len){
    reg_ls_ud *u = (reg_ls_ud*)ud;
    if(u->len + 160 >= u->cap - 2){ u->truncated = true; return; }
    /* JSON 转义（名字里可能有引号/反斜杠/控制字符） */
    char esc[600]; int k=0;
    for(const unsigned char *p=(const unsigned char*)name; *p && k<(int)sizeof esc-8; p++){
        if(*p=='"'||*p=='\\'){ esc[k++]='\\'; esc[k++]=(char)*p; }
        else if(*p<0x20) k+=snprintf(esc+k,sizeof esc-(size_t)k,"\\u%04x",*p);
        else esc[k++]=(char)*p;
    }
    esc[k]=0;
    int m;
    if(is_key)
        m=snprintf(u->buf+u->len, u->cap-u->len, "%s{\"name\":\"%s\",\"kind\":\"key\"}",
                   u->n?",":"", esc);
    else
        m=snprintf(u->buf+u->len, u->cap-u->len,
                   "%s{\"name\":\"%s\",\"kind\":\"value\",\"type\":\"%s\",\"bytes\":%llu}",
                   u->n?",":"", esc, img_reg_type_name(type), (unsigned long long)len);
    if(m>0 && (size_t)m < u->cap-u->len){ u->len+=(size_t)m; u->n++; }
    else u->truncated = true;
}
static int cmd_reg_list(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba = img_args_u64(in,"lba",0);
    const char *full = img_args_get(in,"path");
    bool want_k = img_args_bool(in,"subkeys",true);
    bool want_v = img_args_bool(in,"values",true);
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);

    img_reg_hive hive; char keypath[900] = ""; char err[256] = "";
    if(full && *full){
        int rc = reg_path_split(full, &hive, keypath, sizeof keypath, NULL, err, sizeof err);
        if(rc != IMG_OK){ img_result_fail(out,rc,"REG-PATH-BAD",full,err,NULL); return rc; }
    } else {
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","path","根目录也要写明 hive（如 SYSTEM）",NULL);
        return IMG_EINVAL;
    }
    img_parttable_t *t=NULL;
    int rc = img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb = 0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }

    img_reg *r = img_reg_open(img_ctx_path(ctx), lba, pb, hive, false, err, sizeof err);
    if(!r){
        img_result_fail(out,IMG_ESTATE,"REG-OPEN-FAIL",img_ctx_path(ctx),err,
            "这个分区可能不是 Windows 系统分区");
        return IMG_ESTATE;
    }
    /* ★ img_result_raw 只存指针不拷贝 —— 缓冲必须活到命令返回。
     *   用线程级缓冲 img_tbuf（ntfs.list 同款），不能 malloc 后 free。 */
    char *jbuf = img_tbuf(IMG_TB_NTFS_LIST, REGLS_CAP);
    if(!jbuf){ img_reg_close(r); return IMG_ENOMEM; }
    reg_ls_ud u = { out, 0, jbuf+1, (size_t)REGLS_CAP-2, 0, false };
    jbuf[0] = '[';
    rc = img_reg_enum(r, keypath, want_k, want_v, reg_ls_cb, &u, err, sizeof err);
    img_reg_close(r);
    if(rc != IMG_OK){
        img_result_fail(out,rc,(rc==IMG_ENOENT)?"REG-NOTFOUND":"REG-LIST-FAIL",full,err,NULL);
        return rc;
    }
    img_result_str(out,"hive",img_reg_hive_name(hive));
    img_result_str(out,"key",keypath);
    img_result_u64(out,"count",(uint64_t)u.n);
    /* 首字节已是 '['，回调从 jbuf+1 写；这里补闭合方括号。
     * ★ 不能 free jbuf —— img_result_raw 只存指针，生命周期要到渲染之后。 */
    jbuf[1 + u.len] = ']';
    jbuf[2 + u.len] = 0;
    if(!u.len){ jbuf[0]='['; jbuf[1]=']'; jbuf[2]=0; }
    img_result_raw(out,"items",jbuf);
    img_result_bool(out,"truncated",u.truncated);
    return IMG_OK;
}

/* ---- reg.set ---- */
static const img_arg A_REGSET[] = {
    {"img",   IMG_A_STR,  true,  NULL, "映像路径（qcow2）", NULL},
    {"lba",   IMG_A_U64,  true,  NULL, "Windows 系统分区起始 LBA", chk_lba},
    {"path",  IMG_A_STR,  true,  NULL, "值路径：SOFTWARE\\...\\CurrentBuild", NULL},
    {"type",  IMG_A_ENUM, false, "dword","值类型：dword | qword | sz | expand_sz | multi_sz | binary", NULL},
    {"value", IMG_A_STR,  false, NULL, "值（dword/qword 用十进制；sz 用文本；binary 用十六进制串）", NULL},
};
static int cmd_reg_set(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba = img_args_u64(in,"lba",0);
    const char *full = img_args_get(in,"path");
    const char *ty = img_args_get(in,"type"); if(!ty) ty = "dword";
    const char *val = img_args_get(in,"value");
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);

    img_reg_hive hive; char keybuf[900]; char err[256] = "";
    int rc = reg_path_split(full, &hive, keybuf, sizeof keybuf, NULL, err, sizeof err);
    if(rc != IMG_OK){ img_result_fail(out,rc,"REG-PATH-BAD",full,err,NULL); return rc; }
    char *last = strrchr(keybuf, '\\');
    char keypath[900], valname[300];
    if(last){ *last = 0; snprintf(keypath,sizeof keypath,"%s",keybuf); snprintf(valname,sizeof valname,"%s",last+1); }
    else    { keypath[0] = 0; snprintf(valname,sizeof valname,"%s",keybuf); }

    if(!val && strcasecmp(ty,"binary") != 0){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","value","非 binary 类型必须给 --value",NULL);
        return IMG_EINVAL;
    }

    img_parttable_t *t=NULL;
    rc = img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb = 0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }

    img_reg *r = img_reg_open(img_ctx_path(ctx), lba, pb, hive, true, err, sizeof err);
    if(!r){
        img_result_fail(out,IMG_ESTATE,"REG-OPEN-FAIL",img_ctx_path(ctx),err,
            "这个分区可能不是 Windows 系统分区");
        return IMG_ESTATE;
    }

    if(!strcasecmp(ty,"dword")){
        uint32_t d = (uint32_t)strtoul(val?val:"0", NULL, 0);
        rc = img_reg_set_dword(r, keypath, valname, d, err, sizeof err);
    } else if(!strcasecmp(ty,"qword")){
        uint64_t d = (uint64_t)strtoull(val?val:"0", NULL, 0);
        rc = img_reg_set_qword(r, keypath, valname, d, err, sizeof err);
    } else if(!strcasecmp(ty,"sz") || !strcasecmp(ty,"expand_sz")){
        img_reg_type tt = (!strcasecmp(ty,"sz")) ? IMG_REG_SZ : IMG_REG_EXPAND_SZ;
        rc = img_reg_set(r, keypath, valname, tt, val, strlen(val?val:"")+1, err, sizeof err);
    } else if(!strcasecmp(ty,"binary")){
        /* 十六进制串 → 字节 */
        size_t n = val ? strlen(val) : 0;
        if(n % 2){ img_reg_close(r); img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","value","十六进制串长度必须是偶数",NULL); return IMG_EINVAL; }
        uint8_t *b = (uint8_t*)malloc(n/2 + 1);
        if(!b){ img_reg_close(r); return IMG_ENOMEM; }
        for(size_t i=0;i<n/2;i++){
            char hx[3] = { val[i*2], val[i*2+1], 0 };
            b[i] = (uint8_t)strtoul(hx, NULL, 16);
        }
        rc = img_reg_set(r, keypath, valname, IMG_REG_BINARY, b, n/2, err, sizeof err);
        free(b);
    } else {
        img_reg_close(r);
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","type","只支持 dword/qword/sz/expand_sz/binary",NULL);
        return IMG_EINVAL;
    }
    bool changed = img_reg_changed(r);
    img_reg_close(r);
    if(rc != IMG_OK){
        img_result_fail(out,rc,"REG-SET-FAIL",full,err,"检查 hive 是否可写（SAM/SECURITY 有额外保护）");
        return rc;
    }
    img_result_str(out,"hive",img_reg_hive_name(hive));
    img_result_str(out,"key",keypath);
    img_result_str(out,"name",valname);
    img_result_str(out,"type",ty);
    img_result_bool(out,"changed",changed);
    return IMG_OK;
}

/* ---- reg.del ---- */
static const img_arg A_REGDEL[] = {
    {"img",       IMG_A_STR,  true,  NULL, "映像路径（qcow2）", NULL},
    {"lba",       IMG_A_U64,  true,  NULL, "Windows 系统分区起始 LBA", chk_lba},
    {"path",      IMG_A_STR,  true,  NULL, "键路径（SETUP=删整个值；填 --value= 则只删该值）", NULL},
    {"value",     IMG_A_STR,  false, NULL, "只删这个值名（给了就删值，不给就删键）", NULL},
    {"recursive", IMG_A_BOOL, false, "false","删键时递归（默认false：非空键报 REG-KEY-NOT-EMPTY）", NULL},
};
static int cmd_reg_del(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba = img_args_u64(in,"lba",0);
    const char *full = img_args_get(in,"path");
    const char *val = img_args_get(in,"value");
    bool rec = img_args_bool(in,"recursive",false);
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);

    img_reg_hive hive; char keypath[900]; char err[256] = "";
    int rc = reg_path_split(full, &hive, keypath, sizeof keypath, NULL, err, sizeof err);
    if(rc != IMG_OK){ img_result_fail(out,rc,"REG-PATH-BAD",full,err,NULL); return rc; }

    img_parttable_t *t=NULL;
    rc = img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb = 0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }

    img_reg *r = img_reg_open(img_ctx_path(ctx), lba, pb, hive, true, err, sizeof err);
    if(!r){
        img_result_fail(out,IMG_ESTATE,"REG-OPEN-FAIL",img_ctx_path(ctx),err,
            "这个分区可能不是 Windows 系统分区");
        return IMG_ESTATE;
    }
    if(val && *val) rc = img_reg_del_value(r, keypath, val, err, sizeof err);
    else            rc = img_reg_del_key(r, keypath, rec, err, sizeof err);
    bool changed = img_reg_changed(r);
    img_reg_close(r);
    if(rc != IMG_OK){
        const char *code = (rc==IMG_ENOENT) ? "REG-NOTFOUND"
                         : (rc==IMG_ESTATE) ? "REG-KEY-NOT-EMPTY" : "REG-DEL-FAIL";
        img_result_fail(out,rc,code,full,err,NULL);
        return rc;
    }
    img_result_str(out,"hive",img_reg_hive_name(hive));
    img_result_str(out,"path",keypath);
    if(val && *val) img_result_str(out,"value",val);
    img_result_bool(out,"changed",changed);
    return IMG_OK;
}

/* ================= img.trim（REQ-66）：按空闲位图原地释放簇 ================= */
static const img_arg A_IMGTRIM[] = {
    {"img",     IMG_A_STR,  true,  NULL,   "qcow2 映像路径", NULL},
    {"lba",     IMG_A_U64,  true,  NULL,   "分区起始 LBA（NTFS 或 FAT32）", chk_lba},
    {"dry-run", IMG_A_BOOL, false, "false","只统计，不修改（一个字节都不写）", NULL},
    {"punch",   IMG_A_BOOL, false, "true", "释放后对宿主文件打洞回收空间（不支持就只计数，不写零）", NULL},
};
static int cmd_img_trim(img_ctx *ctx, const img_args *in, img_result *out){
    uint64_t lba = img_args_u64(in,"lba",0);
    bool dry   = img_args_bool(in,"dry-run",false);
    bool punch = img_args_bool(in,"punch",true);
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);
    /* ★ 先做"根本不支持"的门禁，再看分区表 —— 否则空 overlay 会先报
     *   PART-READ-FAIL（格式不合法），用户以为映像坏了。*/
    {
        const img_qcow2_header_t *h = img_qcow2_header(q);
        if(h && h->backing_file_offset){
            img_result_fail(out,IMG_ENOTSUP,"TRIM-NOTSUP",img_ctx_path(ctx),
                "映像是 backing 链的一层：我们读不到底层数据，空闲位图会算错",NULL);
            return IMG_ENOTSUP;
        }
        if(h && h->nb_snapshots){
            char why[256];
            snprintf(why,sizeof why,"映像里有 %u 个快照：释放簇会改掉快照的历史数据，先合并或删除快照",
                     h->nb_snapshots);
            img_result_fail(out,IMG_ENOTSUP,"TRIM-NOTSUP",img_ctx_path(ctx),why,NULL);
            return IMG_ENOTSUP;
        }
    }

    img_parttable_t *t=NULL;
    int rc = img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",
                                      img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }

    img_trim_opts_t o; o.dry_run = dry; o.punch = punch;
    img_trim_result_t r; char err[256]="";
    img_span sp; img_span_prep(&sp,"img","trim","按空闲位图清理");
    img_span_kv_u64(&sp,"lba",lba);
    img_span_kv_u64(&sp,"dry_run",dry?1:0);
    img_span_begin(&sp);
    rc = img_trim(q, lba, pb, &o, &r, img_cmd_prog(), err, sizeof err);
    img_span_end(&sp,rc);
    if(rc != IMG_OK){
        const char *code = (rc==IMG_ECANCELLED) ? "CMD-CANCELLED"
                         : (r.code[0])          ? r.code
                         : (rc==IMG_ENOTSUP)    ? "TRIM-NOTSUP"
                         : (rc==IMG_ESTATE)     ? "TRIM-STATE"
                                                : "TRIM-FAIL";
        img_result_fail(out,rc,code,img_ctx_path(ctx),err[0]?err:"清理失败",
            "确认分区是 NTFS/FAT32 且卷是正常关闭的；有快照或 backing 的映像不支持原地清理");
        return rc;
    }
    img_result_str(out,"fs", r.fs);
    img_result_bool(out,"dry_run",dry);
    img_result_u64(out,"clusters_checked", r.host_clusters_checked);
    img_result_u64(out,"clusters_freed", r.clusters_freed);
    img_result_u64(out,"guest_bytes_freed", r.guest_bytes_freed);
    img_result_u64(out,"host_bytes_punched", r.host_bytes_punched);
    img_result_u64(out,"punch_failed", r.punch_failed);
    img_result_u64(out,"shared_clusters_skipped", r.shared_skipped);
    img_result_u64(out,"compressed_clusters_skipped", r.compressed_skipped);
    img_result_u64(out,"alloc_before", r.alloc_before);
    img_result_u64(out,"alloc_after", r.alloc_after);
    /* 打洞失败不致命（簇已经释放了，只是文件没变小），但要如实报。
     * 两种情形分开说，App 直接原样展示这句话：
     *   全失败 = 宿主不支持（/sdcard 上某些 FUSE 实现、部分 exFAT）
     *   部分失败 = 支持但个别区间被拒（原子写区、跨 extent 边界） */
    img_result_bool(out,"punch_ok", r.punch_failed == 0);
    if(r.punch_failed){
        if(!r.host_bytes_punched){
            img_result_str(out,"punch_note","宿主文件系统不支持打洞，簇已释放但文件没变小");
        }else{
            char note[192];
            snprintf(note,sizeof note,
                     "有 %llu 个簇打洞失败，文件只缩小了 %llu 字节（簇已全部释放，"
                     "下次 img.compact 能把这部分收回来）",
                     (unsigned long long)r.punch_failed,
                     (unsigned long long)r.host_bytes_punched);
            img_result_str(out,"punch_note",note);
        }
    }
    return IMG_OK;
}

/* ================= wim.capture（P4）：qcow2 → WIM ================= */
/* --exclude 的逗号列表 → 定长数组（copy / wim.capture 共用一份解析，
 * 免得一处改了另一处没改）。返回条数；空白会被裁掉，超长条目丢弃。*/
static int parse_excl_csv(const char *s, char (*out)[128], int cap){
    int n=0;
    const char *p=s;
    while(p && *p && n<cap){
        const char *c=strchr(p,',');
        size_t len=c?(size_t)(c-p):strlen(p);
        while(len && (*p==' '||*p=='\t')){ p++; len--; }
        while(len && (p[len-1]==' '||p[len-1]=='\t')) len--;
        if(len && len<128){ memcpy(out[n],p,len); out[n][len]=0; n++; }
        if(!c) break;
        p=c+1;
    }
    return n;
}

static const img_arg A_WIMCAP[] = {
    {"img",        IMG_A_STR,  true,  NULL, "源映像（qcow2 / vhd / vmdk / raw）", NULL},
    {"lba",        IMG_A_U64,  true,  NULL, "系统分区（NTFS）起始 LBA", chk_lba},
    {"out",        IMG_A_STR,  false, NULL, "输出 WIM 路径（estimate=true 时可省）", NULL},
    {"path",       IMG_A_STR,  false, NULL, "只捕获这个子目录（空=整盘）", NULL},
    {"comp",       IMG_A_ENUM, false, "xpress","压缩：none | xpress | lzx | lzms(ESD)", NULL},
    {"name",       IMG_A_STR,  false, "Windows","映像名", NULL},
    {"desc",       IMG_A_STR,  false, NULL, "映像描述", NULL},
    {"capture-acl",IMG_A_BOOL, false, "true","保留 ACL（默认 true）", NULL},
    {"estimate",   IMG_A_BOOL, false, "false","只估算（不抽文件、不打 WIM）", NULL},
    {"exclude",    IMG_A_STR,  false, NULL, "逗号分隔的 basename 通配（* ? []）；estimate 与实捕同口径", NULL},
    {"exclude-default", IMG_A_BOOL, false, "true","默认排掉 pagefile.sys / hiberfil.sys / swapfile.sys", NULL},
};
static int cmd_wim_capture(img_ctx *ctx, const img_args *in, img_result *out){
    /* ★ 必须显式闸：wimlib 关掉时 img_wim_capture.c 不参与编译，
     *   这里再多调一个符号就是链接错误（degraded-build-test 抓到的就是这条）。*/
    if(!IMG_WITH_WIM) return feat_unavail(out,"wimlib（WIM/ESD 捕获）",
                                          "重新编译时开启该功能并安装对应系统库");
#if IMG_WITH_WIM
    uint64_t lba = img_args_u64(in,"lba",0);
    const char *outw = img_args_get(in,"out");
    const char *sub = img_args_get(in,"path");
    const char *comp = img_args_get(in,"comp"); if(!comp) comp="xpress";
    bool est_only = img_args_bool(in,"estimate",false);
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q) return qc_open_fail(out,ctx);

    img_parttable_t *t=NULL;
    int rc = img_ctx_parttable(ctx,&t);
    if(rc||!t) return img_result_fail(out,rc?rc:IMG_ESTATE,"PART-READ-FAIL",img_ctx_path(ctx),img_strerror(rc),NULL), rc?rc:IMG_ESTATE;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].first_lba==lba){
            pb=(t->parts[i].last_lba - t->parts[i].first_lba + 1)*512; break; }
    if(!pb){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","lba","分区不存在",NULL); return IMG_EINVAL; }

    /* ★ REQ-67：压缩类型与排除规则只解析一次，estimate 与实捕都读同一个 o ——
     *   上一版 estimate 走 img_du、实捕走 img_xcopy，各有一套口径，
     *   于是出现"算 11.7 GB、实捕 7.8 GB"。*/
    img_wim_comp c = IMG_WIM_COMP_XPRESS;
    if(!strcasecmp(comp,"none")) c = IMG_WIM_COMP_NONE;
    else if(!strcasecmp(comp,"lzx")) c = IMG_WIM_COMP_LZX;
    else if(!strcasecmp(comp,"lzms")||!strcasecmp(comp,"esd")) c = IMG_WIM_COMP_LZMS;
    else if(strcasecmp(comp,"xpress")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","comp","只接受 none|xpress|lzx|lzms",NULL);
        return IMG_EINVAL;
    }
    img_wim_capture_opts_t o;
    memset(&o,0,sizeof o);
    o.comp = c;
    o.image_name = img_args_get(in,"name");
    o.image_desc = img_args_get(in,"desc");
    o.capture_acl = img_args_bool(in,"capture-acl",true);
    o.use_default_exclude = img_args_bool(in,"exclude-default",true);
    o.n_exclude = parse_excl_csv(img_args_get(in,"exclude"), o.exclude, IMG_WIM_EXCL_MAX);

    if(est_only){
        uint64_t bytes=0, files=0, excl=0; char err[256]="";
        rc = img_wim_capture_estimate(img_ctx_path(ctx), lba, pb, sub, &o,
                                      &bytes, &files, &excl, err, sizeof err);
        if(rc != IMG_OK){
            img_result_fail(out,rc,"WIM-ESTIMATE-FAIL",img_ctx_path(ctx),err,NULL);
            return rc;
        }
        img_result_u64(out,"bytes_needed",bytes);
        img_result_u64(out,"files",files);
        img_result_u64(out,"bytes_needed_mb",(bytes+1048575)/1048576);
        img_result_u64(out,"excluded",excl);
        img_result_bool(out,"exclude_default",o.use_default_exclude);
        return IMG_OK;
    }

    if(!outw || !*outw){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","out","必须给输出 WIM 路径",NULL);
        return IMG_EINVAL;
    }

    img_wim_capture_result_t r;
    char err[256]="";
    img_span sp; img_span_prep(&sp,"wim","capture","捕获成 WIM");
    img_span_kv_u64(&sp,"lba",lba); img_span_begin(&sp);
    rc = img_wim_capture(img_ctx_path(ctx), lba, pb, sub, outw, &o, &r, img_cmd_prog(), err, sizeof err);
    img_span_end(&sp,rc);
    if(rc != IMG_OK){
        /* ★ REQ-68 §3.3：取消走既有约定 CMD-CANCELLED，别报成"捕获失败" */
        if(rc == IMG_ECANCELLED){
            img_result_fail(out,rc,"CMD-CANCELLED",outw,err[0]?err:"已取消",NULL);
            return rc;
        }
        const char *code = (rc==IMG_ENOTSUP)?"WIM-CAP-NOTSUP"
                         : (rc==IMG_ESTATE)  ?"WIM-CAP-SRC-BAD"
                                             :"WIM-CAP-FAIL";
        img_result_fail(out,rc,code,outw,err[0]?err:"捕获失败",
            "临时目录要能放下整棵树（Windows ≈ 15~25 GB）；先用 estimate=true 看需求");
        return rc;
    }
    img_result_str(out,"wim",outw);
    img_result_str(out,"compression",img_wim_comp_name(c));
    img_result_u64(out,"files",r.files);
    img_result_u64(out,"dirs",r.dirs);
    img_result_u64(out,"skipped",r.skipped);   /* REQ-65 Q1：跳过的重解析点个数 */
    img_result_u64(out,"excluded",r.excluded); /* REQ-67：被 --exclude/默认名单排掉的条目数 */
    img_result_bool(out,"exclude_default",o.use_default_exclude);
    img_result_u64(out,"bytes_in",r.bytes_in);
    img_result_u64(out,"bytes_out",r.bytes_out);
    img_result_u64(out,"tmpdir_need",r.tmpdir_need);
    if(r.bytes_in && r.bytes_out)
        img_result_u64(out,"ratio_pct", r.bytes_out*100/r.bytes_in);
    return IMG_OK;
#else
    (void)ctx; (void)in; (void)out; return IMG_ENOTSUP;   /* 不可达：上一行已返回 */
#endif
}

static const img_arg A_WINFINISH[] = {
    {"img",     IMG_A_STR,  true,  NULL,   "映像路径（qcow2）", NULL},
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
    /* ★ 2026-11-05：Win10/11 状态机诊断（只读）。三张官方盘实测（Win10 x86/x64 + Win11）：
     *   CmdLine=oobe\windeploy.exe, OOBEInProgress=1, SetupPhase=4, SetupType=1,
     *   SystemSetupInProgress=1, SysprepStatus\GeneralizationState=4, 无 ChildCompletion。
     *   实测改这些键去「跳过部署」→ 开机 0xc00002e3 蓝屏，所以 modern 分支【只报不改】。 */
    if(fi.oobe_in_progress>=0)      img_result_u64(out,"oobe_in_progress",(uint64_t)fi.oobe_in_progress);
    if(fi.setup_phase>=0)           img_result_u64(out,"setup_phase",(uint64_t)fi.setup_phase);
    if(fi.generalization_state>=0)  img_result_u64(out,"generalization_state",(uint64_t)fi.generalization_state);
    img_result_str(out,"machine", fi.machine==1?"legacy(7/8/8.1, ChildCompletion)"
                                : fi.machine==2?"modern(10/11, windeploy)"
                                               : "unknown");
    if(fi.machine==2 && !dry){
        /* 明确告诉调用方：这条命令对 Win10/11 不会有效果，别指望它 */
        img_result_str(out,"note",
            "Win10/11 没有 ChildCompletion 节点，win.finish 不改任何东西；"
            "首次启动靠 Setup\\CmdLine=oobe\\windeploy.exe 自举。"
            "实测强改 Setup 键会导致 0xc00002e3 蓝屏，故不做。");
    }
    return IMG_OK;
}

/* ---------------- file.copy：跨分区 / 跨镜像 / 宿主 / ISO / WIM ----------------
 * 与 ntfs.copy（同卷）的分工：同卷仍然走 ntfs.copy（更快、只碰索引），
 * 这条覆盖剩下所有组合，也是 App 文件管理器"复制/粘贴"的唯一入口。*/
/* ---------------- 统一 copy / move / rm（2026-10-24） ----------------
 * 三条命令取代：ntfs.copy / fat.copy / file.copy / ntfs.move / fat.move /
 * ntfs.rm / fat.rm / ntfs.rename。
 * 路径语法见 include/img_path.h：qcow2!p3:/a、iso!:/b!1:/c、host:/d。*/
/* 复制/移动失败时的建议（按 rc；文案只进日志，不进 JSON 信封） */
static const char *xcopy_hint(int rc){
    switch(rc){
        case IMG_ENOENT:    return "路径不存在：先 list 父目录核对名称（大小写不敏感）";
        case IMG_EAGAIN:    return "该文件系统有多个分区：把路径写成 !p3: 或 !lba165888:";
        case IMG_EEXISTS:   return "目标已存在：改用 conflict=overwrite 或 conflict=rename";
        case IMG_ESTATE:    return "目标类型冲突（文件/目录互顶）：换目标名或先删旧对象";
        case IMG_ENOTSUP:   return "该组合不支持：目标不能是 ISO/WIM；目录复制/跨卷移动需要 recursive=true";
        case IMG_EFULL:     return "空间不足（映像内分区或宿主磁盘）";
        case IMG_ECORRUPT:  return "写完复查失败：停止写入，保留现场并考虑备份";
        case IMG_ERANGE:    return "FAT32 单文件上限 4 GiB-1：改用 NTFS 或拆分";
        case IMG_ECANCELLED:return "已取消；已写入的部分保留";
        case IMG_EREADONLY: return "ISO/WIM/ESD 是只读安装源，不能写入";
        case IMG_EPARTIAL:  return "复制成功但删源失败：两份都在，手动处理，不要自动重试";
        case IMG_EREFUSED:  return "按安全策略拒绝：宿主删除需要 allow_host_delete=true";
        case IMG_ENAMEINVALID:return "去掉 Windows 不允许的字符（< > : \" / \\ | ? *）、尾随点和空格，使用 CON/NUL/COM1 这类设备名";
        default:            return "失败：看 err.at 定位是哪一侧；需要时导出诊断日志";
    }
}

static const img_arg A_COPY[] = {
    {"src", IMG_A_STR, false, NULL, "源：/path!p3:/a、iso!:/b、host:/c（批量时可省）", NULL},
    {"dst", IMG_A_STR, false, NULL, "目标：/path!p3:/a、host:/c（批量时可省；不能是 ISO/WIM）", NULL},
    {"conflict", IMG_A_ENUM, false, "fail", "同名策略：fail | overwrite | rename（名 (1).扩展名）", NULL},
    {"recursive", IMG_A_BOOL, false, "true", "源是目录时递归复制；false=目录一律拒绝", NULL},
    {"preserve_times", IMG_A_BOOL, false, "true", "保留源时间戳", NULL},
    {"create_parents", IMG_A_BOOL, false, "true", "目标父目录不存在就建", NULL},
    {"follow_symlinks", IMG_A_BOOL, false, "false", "宿主源是否跟随符号链接", NULL},
    {"list_file", IMG_A_STR, false, NULL, "批量：每行 `源路径>目标路径`（可带 !p3:）", NULL},
    {"on_fail", IMG_A_ENUM, false, "continue", "批量失败策略：continue | stop", NULL},
    {"skip-trashed",IMG_A_BOOL,false,"true", "递归子项跳过 .trashed-*（Android 系统回收站）；顶层 src 不受影响（REQ-32）", NULL},
    {"max-depth", IMG_A_U64, false, "32", "目录递归深度上限（P1；超限报 COPY-DEPTH-EXCEEDED）", NULL},
    {"exclude", IMG_A_STR, false, NULL,    "basename 通配排除，逗号分隔（如 .trashed-*,*.tmp）；大小写敏感", NULL},
    {"exclude-from",IMG_A_STR,false,NULL,  "排除清单文件（一行一条，tar -X 风格，只按 basename 匹配）", NULL},
};
static const img_arg A_MOVE[] = {
    {"src", IMG_A_STR, false, NULL, "源（同 copy；批量时可省）", NULL},
    {"dst", IMG_A_STR, false, NULL, "目标（同 copy；批量时可省）", NULL},
    {"conflict", IMG_A_ENUM, false, "fail", "同名策略：fail | overwrite | rename", NULL},
    {"recursive", IMG_A_BOOL, false, "true", "目录移动/跨卷移动必须为 true", NULL},
    {"preserve_times", IMG_A_BOOL, false, "true", "跨卷复制时保留时间戳", NULL},
    {"create_parents", IMG_A_BOOL, false, "true", "目标父目录不存在就建", NULL},
    {"allow_host_delete", IMG_A_BOOL, false, "false", "允许删除宿主源文件（App 弹窗确认后再传 true）", NULL},
    {"list_file", IMG_A_STR, false, NULL, "批量：每行 `源路径>目标路径`", NULL},
    {"on_fail", IMG_A_ENUM, false, "continue", "批量失败策略：continue | stop", NULL},
};
static const img_arg A_RENAME[] = {
    {"path", IMG_A_STR, true, NULL, "要改名的路径（! 语法）", NULL},
    {"name", IMG_A_STR, true, NULL, "新名字（只写名字，不能带 / \\ ! :）", NULL},
};
static const img_arg A_RM[] = {
    {"path", IMG_A_STR, true, NULL, "要删除的路径（同 copy 的路径语法）", NULL},
    {"recursive", IMG_A_BOOL, false, "false", "目录递归删除（危险；UI 二次确认）", NULL},
    {"allow_host_delete", IMG_A_BOOL, false, "false", "允许删除宿主文件（App 弹窗确认后再传 true）", NULL},
};

static void p2loc(const img_path *p, img_xloc *l){
    memset(l,0,sizeof *l);
    l->kind=p->kind; l->lba=p->lba; l->index=p->index;
    if(p->kind==IMG_XK_HOST){
        /* 宿主位置：img 为空，path 就是宿主绝对路径 */
        snprintf(l->path,sizeof l->path,"%s",p->host);
    } else {
        snprintf(l->img,sizeof l->img,"%s",p->host);
        snprintf(l->path,sizeof l->path,"%s",p->inner);
    }
}

static bool path_same_volume(const img_path *a, const img_path *b){
    if(a->kind!=b->kind) return false;
    if(a->kind==IMG_XK_HOST){
        char ra[PATH_MAX], rb[PATH_MAX];
        if(realpath(a->host,ra)&&realpath(b->host,rb)) return strcmp(ra,rb)==0;
        return strcmp(a->host,b->host)==0;
    }
    if(a->kind!=IMG_XK_NTFS && a->kind!=IMG_XK_FAT) return false;
    if(a->lba!=b->lba) return false;
    char ra[PATH_MAX], rb[PATH_MAX];
    if(realpath(a->host,ra)&&realpath(b->host,rb)) return strcmp(ra,rb)==0;
    return strcmp(a->host,b->host)==0;
}

static const char *op_code(int rc, int side){
    switch(rc){
        case IMG_ENOENT:     return side==2?"PATH-DST-NOTFOUND":"PATH-SRC-NOTFOUND";
        case IMG_EAGAIN:     return "PATH-PART-AMBIGUOUS";
        case IMG_EEXISTS:    return "COPY-DST-EXISTS";
        case IMG_ESTATE:     return side==2?"COPY-DST-CONFLICT":"COPY-SRC-UNSUPPORTED";
        case IMG_ENOTSUP:    return "COPY-UNSUPPORTED";
        case IMG_EFULL:      return "COPY-NO-SPACE";
        case IMG_ECORRUPT:   return "COPY-VERIFY-FAIL";
        case IMG_ERANGE:     return "COPY-FILE-TOO-BIG";
        case IMG_ECANCELLED: return "CTL-CANCELLED";
        case IMG_EFORMAT:    return "PATH-FS-BAD";
        case IMG_EINVAL:     return "PATH-BAD";
        case IMG_ENAMETOOLONG:return "COPY-NAME-TOO-LONG";
        case IMG_ESAMEFILE:  return "COPY-SAME-FILE";
        case IMG_ECYCLE:     return "COPY-INTO-SELF";
        case IMG_EREADONLY:  return "PATH-READONLY";
        case IMG_EPARTIAL:   return "MOVE-PARTIAL";
        case IMG_EREFUSED:   return "COPY-DEPTH-EXCEEDED";   /* ★ P1：copy 深度超限；move 的宿主删源拒绝另有码 */
        case IMG_ENAMEINVALID:return "NAME-INVALID";
        default:             return "COPY-FAIL";
    }
}

static int parse_or_fail(const char *spec, img_path *p, const char *which,
                         img_result *out){
    char why[256]={0};
    int rc=img_path_parse(spec,p,why,sizeof why);
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PATH-BAD",which,why[0]?why:"路径无法解析",
            "写法：/映像.qcow2!p3:/路径、/映像.iso!:/路径、/a.esd!1:/路径、host:/手机路径");
    }
    return rc;
}

/* ================= du（REQ-40）：一条命令算递归大小/项数 ================= */
/* ================= sys.tmpdir（REQ-61 §3）=================
 * 回吐当前生效的临时目录：路径、命中的哪一档、所在卷的剩余空间、
 * 本次命令开头 sweep 收走的残留数。App 用它做空间预检与报障。
 */
static int cmd_sys_tmpdir(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx; (void)in;
    /* env 可能被调用方二次改写，而 img_tmpdir() 有进程级缓存 ——
       先重解析再比对，changed 如实反映这次解析是否变了（REQ-61 §3.1）。*/
    const char *prev = img_tmpdir();
    char prev_path[512]; snprintf(prev_path,sizeof prev_path,"%s",prev?prev:"");
    img_tmpdir_reset();
    const char *td = img_tmpdir();
    if(!td || !*td){
        img_result_fail(out,IMG_ESTATE,"TMPDIR-UNAVAILABLE",NULL,
            "没有可写临时目录","设置 IMGTOOL_TMPDIR 指向可写目录后重试");
        return IMG_ESTATE;
    }
    uint64_t free_b=0, total_b=0;
    int frc = img_tmpdir_free_bytes(&free_b,&total_b);

    img_result_str(out,"path",td);
    img_result_str(out,"source",img_tmpdir_source());
    img_result_bool(out,"writable",true);
    if(frc==IMG_OK){
        img_result_u64(out,"free_bytes",free_b);
        img_result_u64(out,"free_mb",free_b/1048576ULL);
        img_result_u64(out,"total_bytes",total_b);
    } else {
        img_result_str(out,"free_error",img_strerror(frc));
    }
    img_result_u64(out,"stale_removed",(uint64_t)(img_cmd_last_sweep()>0?img_cmd_last_sweep():0));
    /* ★ td 在这里一定非 NULL：上面 if(!td || !*td) 已经 return 了。
     *   原来写成 `td && strcmp(...)`，那半个判空是死代码（CodeQL:
     *   cpp/redundant-null-check-simple）。*/
    img_result_bool(out,"changed", strcmp(prev_path,td)?true:false);
    if(strcmp(prev_path,td)) img_result_str(out,"path_before",prev_path);
    return IMG_OK;
}

/* version（REQ-64）：App 要能核对「装到手机上的库是哪一次构建」。
 * 版本串以前写死 2026.10.23，四份不同的构建印同一个串 —— 现在带日期与提交号。*/
static int cmd_version(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx; (void)in;
    img_result_str(out,"tool","imgtool");
    img_result_str(out,"version",IMGTOOL_BUILD_DATE);
    img_result_str(out,"build_date",IMGTOOL_BUILD_DATE);
    img_result_str(out,"commit",IMGTOOL_BUILD_COMMIT);
    img_result_str(out,"platform",img_platform());
    img_result_str(out,"abi",img_abi());
    return IMG_OK;
}

static const img_arg A_DU[] = {
    {"src",       IMG_A_STR, true,  NULL,     "统一路径（host:/… / 映像!p3:/… / ISO!:/… / WIM!1:/… / 包!:/…）", NULL},
    {"limit",     IMG_A_U64, false, "2000000","最多看多少条目；超了停下并回 truncated=true（不报错）", NULL},
    {"max-depth", IMG_A_U64, false, "0",      "递归深度上限（0=不限制）", NULL},
    {"follow",    IMG_A_BOOL,false, "false",  "跟随符号链接（默认不跟，防环）", NULL},
};
static int cmd_du(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *spec=img_args_get(in,"src");
    img_path p;
    int rc=parse_or_fail(spec,&p,"src",out);
    if(rc!=IMG_OK) return rc;
    img_du_stats st;
    char err[300]="";
    rc=img_du_run(&p,img_args_u64(in,"limit",2000000),(int)img_args_u64(in,"max-depth",0),
                  img_args_bool(in,"follow",false),img_cmd_prog(),&st,err,sizeof err);
    img_path_cleanup(&p);
    if(rc!=IMG_OK){
        const char *code = (rc==IMG_ECANCELLED)?"CTL-CANCELLED":
                           (rc==IMG_ENOENT)?"PATH-SRC-NOTFOUND":
                           (rc==IMG_ENOTSUP)?"DU-UNSUPPORTED":"DU-FAIL";
        img_result_fail(out,rc,code,spec,err[0]?err:"统计失败",NULL);
        return rc;
    }
    img_result_str(out,"path",p.inner[0]?p.inner:p.host);
    img_result_u64(out,"files",st.files);
    img_result_u64(out,"dirs",st.dirs);
    img_result_u64(out,"total_bytes",st.total_bytes);
    img_result_u64(out,"alloc_bytes",st.alloc_bytes);   /* 没有就给 0（不做假） */
    img_result_u64(out,"scanned",st.scanned);
    img_result_bool(out,"truncated",st.truncated);
    return IMG_OK;
}

static int cmd_copy(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *ss=img_args_get(in,"src"), *ds=img_args_get(in,"dst");
    const char *lst=img_args_get(in,"list_file");
    if(!lst && (!ss||!ds)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","要么给 src+dst，要么给 list_file",NULL);
        return IMG_EINVAL;
    }
    int conflict=IMG_COPY_FAIL;
    const char *cf=img_args_get(in,"conflict");
    if(cf){ if(!strcmp(cf,"overwrite")) conflict=IMG_COPY_OVERWRITE;
            else if(!strcmp(cf,"rename")) conflict=IMG_COPY_RENAME;
            else if(strcmp(cf,"fail")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","conflict","只能是 fail/overwrite/rename",NULL); return IMG_EINVAL; } }
    img_xcopy_opts o={ conflict, true, true, true, false, 32, false, {{0}}, 0 };
    o.recursive=img_args_bool(in,"recursive",true);
    /* ★ REQ-32：copy 默认跳过 Android 系统回收站（.trashed-*），只作用于递归子项；
     *   --exclude 可给逗号分隔的 basename 通配；--exclude-from 一行一条。*/
    o.skip_trashed=img_args_bool(in,"skip-trashed",true);
    o.max_depth=(int)img_args_u64(in,"max-depth",32);   /* ★ P1：可调递归深度 */
    if(o.max_depth<1) o.max_depth=32;
    if(o.max_depth>256) o.max_depth=256;
    const char *ex=img_args_get(in,"exclude");
    if(ex && *ex) o.n_exclude = parse_excl_csv(ex, o.exclude, IMG_XCOPY_EXCL_MAX);
    const char *exf=img_args_get(in,"exclude-from");
    if(exf && *exf){
        FILE *fp=fopen(exf,"r");
        if(!fp){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",exf,"排除清单无法打开",NULL); return IMG_ENOENT; }
        char line[256];
        while(fgets(line,sizeof line,fp) && o.n_exclude<IMG_XCOPY_EXCL_MAX){
            char *nl=strchr(line,'\n'); if(nl) *nl=0;
            char *p0=line; while(*p0==' '||*p0=='\t') p0++;
            size_t n=strlen(p0);
            while(n && (p0[n-1]==' '||p0[n-1]=='\t'||p0[n-1]=='\r')) p0[--n]=0;
            if(!n||*p0=='#') continue;
            snprintf(o.exclude[o.n_exclude],sizeof o.exclude[0],"%s",p0);
            o.n_exclude++;
        }
        fclose(fp);
    }
    o.preserve_times=img_args_bool(in,"preserve_times",true);
    o.create_parents=img_args_bool(in,"create_parents",true);
    o.follow_symlinks=img_args_bool(in,"follow_symlinks",false);

    if(lst){
        FILE *fp=fopen(lst,"r");
        if(!fp){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",lst,"清单文件无法打开",NULL); return IMG_ENOENT; }
        const char *ofail=img_args_get(in,"on_fail");
        bool stop_on_fail=ofail && !strcmp(ofail,"stop");
        enum { ITEMS_CAP = 98304 };
        char *items=img_tbuf(IMG_TB_ITEMS1,ITEMS_CAP);
        if(!items) return tb_nomem(out,"复制明细缓冲");
        size_t il=1; items[0]='['; items[1]=0;
        int okn=0,badn=0; uint64_t bytes=0; bool truncated=false;
        char line[4200];
        while(fgets(line,sizeof line,fp)){
            char *nl=strchr(line,'\n'); if(nl) *nl=0;
            char *p0=line; while(*p0==' '||*p0=='\t') p0++;
            if(!*p0||*p0=='#') continue;
            char *sep=strrchr(p0,'>');          /* 路径里可能有 >？取最后一个 */
            if(!sep){ badn++; continue; }
            *sep=0;
            char *a=p0,*b=sep+1;
            char *e=a+strlen(a); while(e>a&&(e[-1]==' '||e[-1]=='\t')) *--e=0;
            while(*b==' '||*b=='\t') b++;
            e=b+strlen(b); while(e>b&&(e[-1]==' '||e[-1]=='\t')) *--e=0;
            if(!*a||!*b){ badn++; continue; }
            img_path ps,pd; int rc;
            memset(&ps,0,sizeof ps); memset(&pd,0,sizeof pd);
            rc=img_path_parse(a,&ps,NULL,0);
            if(rc==IMG_OK) rc=img_path_parse_dst(b,&pd,NULL,0);   /* ★ P1: dst 允许新建容器 */
            img_xcopy_stats stx; memset(&stx,0,sizeof stx);
            if(rc==IMG_OK && pd.is_readonly) rc=IMG_EREADONLY;
            if(rc==IMG_OK){
                img_xloc ls,ld; p2loc(&ps,&ls); p2loc(&pd,&ld);
                rc=img_xcopy_run(&ls,&ld,&o,img_cmd_prog(),&stx);
            }
            img_path_cleanup(&ps); img_path_cleanup(&pd);
            char ea[2200],eb[2200],ep[2200],tail[2300];
            jesc(ea,sizeof ea,a); jesc(eb,sizeof eb,b);
            jesc(ep,sizeof ep,stx.final_path);
            if(rc==IMG_OK) snprintf(tail,sizeof tail,",\"path\":\"%s\",\"ino\":%llu,\"size\":%llu",
                                    ep,(unsigned long long)stx.top_ino,(unsigned long long)stx.top_size);
            else snprintf(tail,sizeof tail,",\"ino\":0,\"size\":0,\"code\":\"%s\"",op_code(rc,0));
            int need=snprintf(items+il,ITEMS_CAP-il,
                "%s{\"src\":\"%s\",\"dst\":\"%s\",\"ok\":%s,\"files\":%llu,\"dirs\":%llu,\"bytes\":%llu%s}",
                (okn+badn)?",":"",ea,eb,rc==IMG_OK?"true":"false",
                (unsigned long long)stx.files,(unsigned long long)stx.dirs,
                (unsigned long long)stx.bytes,tail);
            if(need>0 && il+(size_t)need<ITEMS_CAP) il+=(size_t)need; else truncated=true;
            if(rc==IMG_OK){ okn++; bytes+=stx.bytes; } else { badn++; if(stop_on_fail) break; }
        }
        fclose(fp);
        if(il+2<ITEMS_CAP){ items[il++]=']'; items[il]=0; }
        img_result_u64(out,"copied",(uint64_t)okn);
        img_result_u64(out,"failed",(uint64_t)badn);
        img_result_u64(out,"bytes",bytes);
        img_result_bool(out,"truncated",truncated);
        img_result_raw(out,"items",items[1]?items:"[]");
        return IMG_OK;
    }

    img_path ps,pd;
    int rc=parse_or_fail(ss,&ps,"src",out);
    if(rc==IMG_OK) rc=img_path_parse_dst(ds,&pd,NULL,0);   /* ★ P1: dst 允许新建容器 */
    if(rc!=IMG_OK){
        img_result_fail(out,rc,"PATH-BAD",ds,
            (rc==IMG_ENOENT)?"目标容器不存在（dst 新建只支持 zip/7z/tar/tgz）":"目标路径无法解析",
            "dst 写法：/x/a.zip!:/f.txt（新建包）、host:/x、/x.qcow2!p3:/f.txt");
        img_path_cleanup(&ps); img_path_cleanup(&pd);
        return rc;
    }
    /* ★ P1（2026-11-08）：is_new_container 的包不算只读 —— 它就是要被写出来的。
     *   真只读（ISO/WIM/ESD、已存在的 RAR）仍由 pd.is_readonly 拦。 */
    if(pd.is_readonly && !pd.is_new_container){
        img_path_cleanup(&ps); img_path_cleanup(&pd);
        img_result_fail(out,IMG_EREADONLY,"PATH-READONLY",ds,"目标是只读容器（ISO/WIM/ESD/RAR）",NULL);
        return IMG_EREADONLY;
    }
    img_xloc ls,ld; p2loc(&ps,&ls); p2loc(&pd,&ld);
    img_xcopy_stats st; memset(&st,0,sizeof st);
    rc=img_xcopy_run(&ls,&ld,&o,img_cmd_prog(),&st);
    img_path_cleanup(&ps); img_path_cleanup(&pd);
    if(rc!=IMG_OK){
        char why[300];
        if(st.first_error[0])
            snprintf(why,sizeof why,"%s",st.first_error);
        else
            snprintf(why,sizeof why,"已完成 %llu 个文件 / %.1f MiB 后失败%s",
                     (unsigned long long)st.files,(double)st.bytes/1048576.0,
                     st.side==1?"（源侧）":(st.side==2?"（目标侧）":""));
        img_result_fail(out,rc,op_code(rc,st.side),
                        st.bad_name[0]?st.bad_name:ss,why,xcopy_hint(rc));
        return rc;
    }
    img_result_u64(out,"copied_files",st.files);
    img_result_u64(out,"copied_dirs",st.dirs);
    img_result_u64(out,"bytes",st.bytes);
    img_result_u64(out,"skipped",st.skipped);
    img_result_u64(out,"skipped_trashed",st.skipped_trashed);   /* REQ-32：按条目数，不按字节 */
    img_result_u64(out,"skipped_excluded",st.skipped_excluded);
    img_result_u64(out,"ads",st.ads);
    img_result_u64(out,"size",st.top_size);
    img_result_u64(out,"ino",st.top_ino);
    if(st.final_path[0]) img_result_str(out,"path",st.final_path);
    img_result_bool(out,"replaced",st.replaced);
    img_result_u64(out,"parent_total",st.parent_total);
    if(st.archive_bytes) img_result_u64(out,"archive_bytes",st.archive_bytes);   /* REQ-41：重写后的包大小 */
    img_result_str(out,"mode",path_same_volume(&ps,&pd)?"same_volume":"cross_volume");
    img_result_str(out,"src",ss);
    img_result_str(out,"dst",ds);
    if(st.final_path[0]){
        img_result_str(out,"dst_path",st.final_path);
        char par[2048]; snprintf(par,sizeof par,"%s",st.final_path);
        char *slash=strrchr(par,'/');
        if(slash){ if(slash==par) par[1]=0; else *slash=0; img_result_str(out,"parent",par); }
    }
    return IMG_OK;
}

static int cmd_move(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *ss=img_args_get(in,"src"), *ds=img_args_get(in,"dst");
    const char *lst=img_args_get(in,"list_file");
    if(!lst && (!ss||!ds)){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","src","要么给 src+dst，要么给 list_file",NULL);
        return IMG_EINVAL;
    }
    int conflict=IMG_COPY_FAIL;
    const char *cf=img_args_get(in,"conflict");
    if(cf){ if(!strcmp(cf,"overwrite")) conflict=IMG_COPY_OVERWRITE;
            else if(!strcmp(cf,"rename")) conflict=IMG_COPY_RENAME;
            else if(strcmp(cf,"fail")){ img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","conflict","只能是 fail/overwrite/rename",NULL); return IMG_EINVAL; } }
    img_xmove_opts o={ conflict, true, true, true, false, true, 32 };
    o.recursive=img_args_bool(in,"recursive",true);
    o.preserve_times=img_args_bool(in,"preserve_times",true);
    o.create_parents=img_args_bool(in,"create_parents",true);
    o.allow_host_delete=img_args_bool(in,"allow_host_delete",false);

    if(lst){
        FILE *fp=fopen(lst,"r");
        if(!fp){ img_result_fail(out,IMG_ENOENT,"SRC-NOTFOUND",lst,"清单文件无法打开",NULL); return IMG_ENOENT; }
        const char *ofail=img_args_get(in,"on_fail");
        bool stop_on_fail=ofail && !strcmp(ofail,"stop");
        enum { ITEMS_CAP = 98304 };
        char *items=img_tbuf(IMG_TB_ITEMS2,ITEMS_CAP);
        if(!items) return tb_nomem(out,"复制明细缓冲");
        size_t il=1; items[0]='['; items[1]=0;
        int okn=0,badn=0; uint64_t bytes=0; bool truncated=false;
        char line[4200];
        while(fgets(line,sizeof line,fp)){
            char *nl=strchr(line,'\n'); if(nl) *nl=0;
            char *p0=line; while(*p0==' '||*p0=='\t') p0++;
            if(!*p0||*p0=='#') continue;
            char *sep=strrchr(p0,'>');
            if(!sep){ badn++; continue; }
            *sep=0;
            char *a=p0,*b=sep+1;
            char *e=a+strlen(a); while(e>a&&(e[-1]==' '||e[-1]=='\t')) *--e=0;
            while(*b==' '||*b=='\t') b++;
            e=b+strlen(b); while(e>b&&(e[-1]==' '||e[-1]=='\t')) *--e=0;
            if(!*a||!*b){ badn++; continue; }
            img_path ps,pd; int rc;
            memset(&ps,0,sizeof ps); memset(&pd,0,sizeof pd);
            rc=img_path_parse(a,&ps,NULL,0);
            if(rc==IMG_OK) rc=img_path_parse(b,&pd,NULL,0);
            img_xmove_stats stx; memset(&stx,0,sizeof stx);
            if(rc==IMG_OK){
                img_xloc ls,ld; p2loc(&ps,&ls); p2loc(&pd,&ld);
                rc=img_xmove_run(&ls,&ld,&o,img_cmd_prog(),&stx);
            }
            img_path_cleanup(&ps); img_path_cleanup(&pd);
            char ea[2200],eb[2200],ep[2200],tail[2300];
            jesc(ea,sizeof ea,a); jesc(eb,sizeof eb,b); jesc(ep,sizeof ep,stx.final_path);
            if(rc==IMG_OK) snprintf(tail,sizeof tail,",\"path\":\"%s\",\"mode\":\"%s\"",ep,stx.mode);
            else snprintf(tail,sizeof tail,",\"code\":\"%s\"",op_code(rc,0));
            int need=snprintf(items+il,ITEMS_CAP-il,
                "%s{\"src\":\"%s\",\"dst\":\"%s\",\"ok\":%s,\"files\":%llu,\"dirs\":%llu,\"bytes\":%llu%s}",
                (okn+badn)?",":"",ea,eb,rc==IMG_OK?"true":"false",
                (unsigned long long)stx.files,(unsigned long long)stx.dirs,
                (unsigned long long)stx.bytes,tail);
            if(need>0 && il+(size_t)need<ITEMS_CAP) il+=(size_t)need; else truncated=true;
            if(rc==IMG_OK){ okn++; bytes+=stx.bytes; } else { badn++; if(stop_on_fail) break; }
        }
        fclose(fp);
        if(il+2<ITEMS_CAP){ items[il++]=']'; items[il]=0; }
        img_result_u64(out,"moved",(uint64_t)okn);
        img_result_u64(out,"failed",(uint64_t)badn);
        img_result_u64(out,"bytes",bytes);
        img_result_bool(out,"truncated",truncated);
        img_result_raw(out,"items",items[1]?items:"[]");
        return IMG_OK;
    }

    img_path ps,pd;
    int rc=parse_or_fail(ss,&ps,"src",out);
    if(rc==IMG_OK) rc=parse_or_fail(ds,&pd,"dst",out);
    if(rc!=IMG_OK){ img_path_cleanup(&ps); return rc; }
    if(ps.is_readonly || pd.is_readonly){
        img_path_cleanup(&ps); img_path_cleanup(&pd);
        img_result_fail(out,IMG_EREADONLY,"PATH-READONLY",ps.is_readonly?ss:ds,
                        "ISO/WIM/ESD 是只读安装源，不能作为移动的源或目标",NULL);
        return IMG_EREADONLY;
    }
    img_xloc ls,ld; p2loc(&ps,&ls); p2loc(&pd,&ld);
    img_xmove_stats st; memset(&st,0,sizeof st);
    rc=img_xmove_run(&ls,&ld,&o,img_cmd_prog(),&st);
    bool src_host=(ps.kind==IMG_XK_HOST);
    img_path_cleanup(&ps); img_path_cleanup(&pd);
    if(rc!=IMG_OK){
        const char *cde=op_code(rc,st.side);
        if(rc==IMG_EREFUSED) cde=src_host?"MOVE-HOST-DELETE-REFUSED":"MOVE-REFUSED";
        char why[300];
        if(st.first_error[0])
            snprintf(why,sizeof why,"%s",st.first_error);
        else
            snprintf(why,sizeof why,"mode=%s deleted=%s",
                     st.mode[0]?st.mode:"-",st.deleted?"true":"false");
        img_result_fail(out,rc,cde,st.bad_name[0]?st.bad_name:ss,why,
            rc==IMG_EPARTIAL?"复制已完成但删除源失败：两份都在，请手动处理（不要重试）":
            rc==IMG_EREFUSED?"宿主文件删除需要 App 弹窗确认并传 allow_host_delete=true":
            xcopy_hint(rc));
        return rc;
    }
    img_result_str(out,"mode",st.mode);
    img_result_u64(out,"files",st.files);
    img_result_u64(out,"dirs",st.dirs);
    img_result_u64(out,"bytes",st.bytes);
    img_result_u64(out,"ads",st.ads);
    img_result_bool(out,"deleted",st.deleted);
    img_result_bool(out,"noop",st.noop);
    img_result_bool(out,"case_only",st.case_only);
    img_result_str(out,"src",ss);
    img_result_str(out,"dst",ds);
    if(st.final_path[0]) img_result_str(out,"dst_path",st.final_path);
    return IMG_OK;
}

/* 取父目录（按 / 切；根返回 "/"） */
static void dirname_of(const char *p, char *out, size_t cap){
    snprintf(out,cap,"%s",p?p:"");
    char *s=strrchr(out,'/');
    if(!s){ out[0]=0; return; }
    if(s==out){ out[1]=0; return; }
    *s=0;
}

/* rename = 同父 move。参数是"新名字"，不是完整路径，避免 UI 拼错。*/
static int cmd_rename(img_ctx *ctx, const img_args *in, img_result *out){
    const char *ps=img_args_get(in,"path");
    const char *nn=img_args_get(in,"name");
    if(!ps||!nn||!*nn){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","name","需要 path + name",NULL);
        return IMG_EINVAL;
    }
    if(strchr(nn,'/')||strchr(nn,'\\')||strchr(nn,'!')||
       !strcmp(nn,".")||!strcmp(nn,"..")){
        img_result_fail(out,IMG_EINVAL,"CMD-ARG-BAD","name",
                        "新名字不能包含 / \\ ! 且不能是 . / ..","只写文件名本身；换目录请用 move");
        return IMG_EINVAL;
    }
    img_path p;
    int rc=parse_or_fail(ps,&p,"path",out);
    if(rc!=IMG_OK) return rc;
    if(p.kind!=IMG_XK_HOST){
        /* NTFS/FAT 的 Windows 名字规则（非法字符 / 保留设备名 / 尾点尾空格）。
         * 在打开镜像之前就拒绝 —— 零副作用。宿主按宿主规则，不拦这里。*/
        img_name_status ns=img_name_check(nn);
        if(ns!=IMG_NAME_OK){
            char det[220]; snprintf(det,sizeof det,"%s",img_name_reason(ns,nn));
            img_path_cleanup(&p);
            img_result_fail(out,IMG_ENAMEINVALID,"NAME-INVALID","name",det,
                "去掉 Windows 不允许的字符（< > : \" / \\ | ? *）、尾随点和空格，使用 CON/NUL/COM1 这类设备名");
            return IMG_ENAMEINVALID;
        }
    }
    if(p.is_readonly){
        img_path_cleanup(&p);
        img_result_fail(out,IMG_EREADONLY,"PATH-READONLY",ps,"ISO/WIM/ESD 是只读安装源",NULL);
        return IMG_EREADONLY;
    }
    char dir[2048], dst[4096];
    if(p.kind==IMG_XK_HOST){
        dirname_of(p.host,dir,sizeof dir);
        if(!strcmp(dir,"/")) snprintf(dst,sizeof dst,"/%s",nn);
        else if(dir[0])        snprintf(dst,sizeof dst,"%s/%s",dir,nn);
        else                   snprintf(dst,sizeof dst,"%s",nn);
    } else {
        dirname_of(p.inner,dir,sizeof dir);
        if(!strcmp(dir,"/")) snprintf(dst,sizeof dst,"%s!lba%llu:/%s",p.host,
                                      (unsigned long long)p.lba,nn);
        else                 snprintf(dst,sizeof dst,"%s!lba%llu:%s/%s",p.host,
                                      (unsigned long long)p.lba,dir,nn);
    }
    img_path_cleanup(&p);

    img_args *a=img_args_new();
    if(!a){ img_result_fail(out,IMG_ENOMEM,"CMD-FAILED",ps,"内存不足",NULL); return IMG_ENOMEM; }
    img_args_set(a,"src",ps);
    img_args_set(a,"dst",dst);
    static const char *flags[]={"conflict","recursive","preserve_times","create_parents",
                                "allow_host_delete",NULL};
    for(int i=0; flags[i]; i++){
        const char *v=img_args_get(in,flags[i]);
        if(v) img_args_set(a,flags[i],v);
    }
    rc=cmd_move(ctx,a,out);
    img_args_free(a);
    return rc;
}

/* ---------- rm 找不到时的"最接近的名字"（REQ-47 §4.3）----------
 * 只做 FAT：NTFS 这边只有 rw 挂载入口，为了一句提示就拿可写方式去挂用户的盘不值当。
 * 匹配规则故意简单：忽略大小写的最长公共前缀（2 分/字符）减长度差，
 * 因为用户踩的就是 8.3 短名与长名对不上的情况（WGL4_BOO.TTF vs wgl4_boot.ttf）。*/
typedef struct { char best[3][256]; int score[3]; int n; const char *want; } rm_sib;
static int rm_sib_score(const char *a, const char *b){
    int i=0;
    while(a[i] && b[i] && tolower((unsigned char)a[i])==tolower((unsigned char)b[i])) i++;
    int d=(int)strlen(a)-(int)strlen(b); if(d<0) d=-d;
    return i*2 - d;
}
static void rm_sib_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    (void)is_dir; (void)size;
    rm_sib *s=(rm_sib*)ud;
    if(!name || !*name) return;
    int sc=rm_sib_score(name,s->want);          /* want 由 ud 带进来后单独填 */
    if(s->n<3){
        snprintf(s->best[s->n],sizeof s->best[0],"%s",name); s->score[s->n]=sc; s->n++;
        return;
    }
    int w=0; for(int i=1;i<3;i++) if(s->score[i]<s->score[w]) w=i;
    if(sc>s->score[w]){ snprintf(s->best[w],sizeof s->best[0],"%s",name); s->score[w]=sc; }
}
/* 成功返回 true，out 里写一句说明（逗号分隔的名字）*/
static bool rm_try_siblings(const char *host, uint64_t lba, const char *inner,
                            const char *want, char *out, size_t cap){
    if(!host || !*host || !inner || inner[0]!='/' ) return false;
    img_io *io=img_io_open_existing(host,false);
    if(!io) return false;
    img_qcow2_opts_t qo; memset(&qo,0,sizeof qo);
    img_qcow2 *q=img_qcow2_open(io,&qo);
    if(!q){ img_io_close(io); return false; }
    bool ok=false;
    img_fat32_fs *fs=img_fat32_mount(q,lba,false);
    if(fs){
        char parent[2048]; snprintf(parent,sizeof parent,"%s",inner);
        char *sl=strrchr(parent,'/');
        if(sl){ if(sl==parent) parent[1]=0; else *sl=0; } else parent[0]=0;
        if(!parent[0]) snprintf(parent,sizeof parent,"/");
        rm_sib sib; memset(&sib,0,sizeof sib); sib.want=want;
        if(img_fat32_list_dir(fs,parent,rm_sib_cb,&sib)==IMG_OK && sib.n>0){
            /* 挑分数最高的排前面（n<=3，直接插入排序）*/
            for(int i=1;i<sib.n;i++)
                for(int j=i;j>0 && sib.score[j]>sib.score[j-1];j--){
                    char t[256]; memcpy(t,sib.best[j],sizeof t);
                    memcpy(sib.best[j],sib.best[j-1],sizeof t);
                    memcpy(sib.best[j-1],t,sizeof t);
                    int k=sib.score[j]; sib.score[j]=sib.score[j-1]; sib.score[j-1]=k;
                }
            size_t k=0;
            k+=(size_t)snprintf(out+k,cap-k,"没找到这个名字。同目录里有：");
            for(int i=0;i<sib.n && k<cap;i++)
                k+=(size_t)snprintf(out+k,cap-k,"%s%s", i?", ":"", sib.best[i]);
            k+=(size_t)snprintf(out+k,cap-k,"（FAT 不区分大小写，8.3 短名也能删）");
            ok=true;
        }
        img_fat32_unmount(fs);
    }
    img_qcow2_close(q); img_io_close(io);
    return ok;
}

static int cmd_rm(img_ctx *ctx, const img_args *in, img_result *out){
    (void)ctx;
    const char *spec=img_args_get(in,"path");
    img_path p;
    int rc=parse_or_fail(spec,&p,"path",out);
    if(rc!=IMG_OK) return rc;
    bool rec=img_args_bool(in,"recursive",false);
    bool allow=img_args_bool(in,"allow_host_delete",false);
    /* 根目录永远不删：镜像内的 "/" 和宿主的 "/" 都一样。*/
    if((p.kind==IMG_XK_HOST && (!p.host[0] || !strcmp(p.host,"/"))) ||
       (p.kind!=IMG_XK_HOST && (!p.inner[0] || !strcmp(p.inner,"/")))){
        img_path_cleanup(&p);
        img_result_fail(out,IMG_EREFUSED,"RM-ROOT-REFUSED",spec,
                        "不允许删除根目录","选择具体的文件或子目录");
        return IMG_EREFUSED;
    }
    uint64_t removed=0;
    img_xloc l; p2loc(&p,&l);
    rc=img_xrm_run(&l,rec,allow,&removed);
    bool host=(p.kind==IMG_XK_HOST);
    /* ★ 失败这里是【最后】还拿得到定位信息的地方：p 马上就要 cleanup。
     *   REQ-47 的"找不到"提示要用到 host/lba/inner，先拷出来。*/
    bool was_fat = (p.kind==IMG_XK_FAT);
    char sib_host[2048]="", sib_inner[2048]="";
    uint64_t sib_lba=p.lba;
    if(was_fat){ snprintf(sib_host,sizeof sib_host,"%s",p.host);
                 snprintf(sib_inner,sizeof sib_inner,"%s",p.inner); }
    img_path_cleanup(&p);
    if(rc!=IMG_OK){
        const char *cde=op_code(rc,0);
        if(rc==IMG_ESTATE || rc==IMG_ENOTSUP) cde="RM-DIR-NOT-EMPTY";
        if(rc==IMG_EREFUSED) cde=host?"RM-HOST-DELETE-REFUSED":"RM-REFUSED";
        static char why2[768];
        const char *why=NULL;
        if(rc==IMG_ENOENT && was_fat){
            const char *bn=strrchr(sib_inner,'/'); bn = bn?bn+1:sib_inner;
            if(rm_try_siblings(sib_host,sib_lba,sib_inner,bn,why2,sizeof why2)) why=why2;
            else why="源不存在（FAT 不区分大小写，8.3 短名也能用）";
        }
        img_result_fail(out,rc,cde,spec,
            rc==IMG_EREFUSED?(host?"宿主文件删除需要 allow_host_delete=true，且不允许删除存储根目录":
                                    "该对象不允许删除"):why,
            rc==IMG_EREADONLY?"ISO/WIM/ESD 是只读安装源":NULL);
        return rc;
    }
    img_result_str(out,"path",spec);
    img_result_u64(out,"removed",removed);
    img_result_bool(out,"deleted",true);
    return IMG_OK;
}

const img_cmd IMG_CMD_TABLE[] = {
    {"img.info",  "1.0", "显示映像基本信息（大小/分区表/分区数）",
        A_INFO,   (int)(sizeof A_INFO  /sizeof A_INFO[0]),   cmd_img_info, NULL, 0},
    {"part.show", "1.0", "打印分区表（JSON：GPT/MBR + 每个分区）",
        A_PTSHOW, (int)(sizeof A_PTSHOW/sizeof A_PTSHOW[0]), cmd_part_show, NULL, 0},
    {"fat.put",   "1.0", "把本地文件写入某个 FAT32 分区（带可选空间校验）",
        A_FATPUT, (int)(sizeof A_FATPUT/sizeof A_FATPUT[0]), cmd_fat_put, NULL, 0},
    {"fat.ls",    "1.0", "列某个 FAT32 分区里的目录",
        A_FATLS,  (int)(sizeof A_FATLS /sizeof A_FATLS[0]),  cmd_fat_ls, NULL, 0},
    {"wim.size",  "1.0", "估算某个 WIM/ESD 映像展开后需要多少空间（工具给数字，App 定政策）",
        A_WIMSIZE,(int)(sizeof A_WIMSIZE/sizeof A_WIMSIZE[0]), cmd_wim_size, NULL, 0},
    {"wim.apply","1.0", "写入 WIM/ESD 到分区（默认先格式化目标分区防残留；--format=off 保留原分区）",
        A_WIMAPPLY,(int)(sizeof A_WIMAPPLY/sizeof A_WIMAPPLY[0]), cmd_wim_apply_cmd, NULL, 0},
    {"part.scan", "1.0", "扫描分区：文件系统/容量/根目录有没有文件（App 弹不弹提示的依据，三级口径）",
        A_PARTSCAN,(int)(sizeof A_PARTSCAN/sizeof A_PARTSCAN[0]), cmd_part_scan, NULL, 0},
    {"ntfs.list", "1.0", "列 NTFS 目录（给 App 名单：显示什么/搬谁由 App 决定）",
        A_NTFSLIST,(int)(sizeof A_NTFSLIST/sizeof A_NTFSLIST[0]), cmd_ntfs_list, NULL, 0},
    {"img.clone", "1.0", "克隆保留分区到新盘（不列的分区留空 → 换系统主路径；旧映像自动改名/校验/回滚）",
        A_IMGCLONE,(int)(sizeof A_IMGCLONE/sizeof A_IMGCLONE[0]), cmd_img_clone, NULL, 0},
    {"winre.auto","1.0", "写入/删除【首次开机自动启用 WinRE】脚本（SetupComplete.cmd，官方 reagentc 机制）",
        A_WINRE_AUTO,(int)(sizeof A_WINRE_AUTO/sizeof A_WINRE_AUTO[0]), cmd_winre_auto, NULL, 0},
    {"ntfs.exists","1.0", "查 NTFS 路径是否存在（断点续跑探测原语：App 判定步骤做完没）",
        A_NTFSEXISTS,(int)(sizeof A_NTFSEXISTS/sizeof A_NTFSEXISTS[0]), cmd_ntfs_exists, NULL, 0},
    {"part.copy", "1.0", "跨映像扇区级拷贝分区（GPT→MBR 迁移的搬运工；源只读、目标稀疏）",
        A_PARTCOPY,(int)(sizeof A_PARTCOPY/sizeof A_PARTCOPY[0]), cmd_part_copy, NULL, 0},
    {"part.mbrboot","1.0", "写 MBR 引导码（参考件）+ 随机磁盘签名（没有它 BIOS 会报「未找到操作系统」）",
        A_MBRBOOT,(int)(sizeof A_MBRBOOT/sizeof A_MBRBOOT[0]), cmd_mbrboot, NULL, 0},
    {"part.layout","1.0", "BIOS/MBR 标准布局生成器（系统保留+系统+恢复0x27+扩展分区含逻辑分区）",
        A_PARTLAYOUT,(int)(sizeof A_PARTLAYOUT/sizeof A_PARTLAYOUT[0]), cmd_part_layout, NULL, 0},
    {"boot.repair","1.0","BIOS 引导一键修复（诊断→按需重写→复验→JSON）",
        A_BOOTREPAIR,(int)(sizeof A_BOOTREPAIR/sizeof A_BOOTREPAIR[0]), cmd_boot_repair, NULL, 0},
    {"fat.export","1.0", "把 FAT32 分区里的文件导出到本地（UEFI 的 BCD 在 ESP 上）",
        A_FATEXPORT,(int)(sizeof A_FATEXPORT/sizeof A_FATEXPORT[0]), cmd_fat_export, NULL, 0},
    {"fat.exists", "1.0", "查 FAT32 路径是否存在（断点续跑探测原语）",
        A_FATEXISTS,(int)(sizeof A_FATEXISTS/sizeof A_FATEXISTS[0]), cmd_fat_exists, NULL, 0},
    {"ctl.cancel","1.0", "请求取消正在执行的操作（另一线程调用；协作式，下一个检查点生效）",
        NULL, 0, cmd_ctl_cancel, NULL, 0},
    /* ---- 2026-10-20：补齐 App 建盘流水线（手册 §11.0 的 7 个缺口）---- */
    {"img.create","1.0", "建 qcow2 本体 + 分区表（★路径参数叫 out：img 会被当成已存在映像打开）",
        A_IMGCREATE,(int)(sizeof A_IMGCREATE/sizeof A_IMGCREATE[0]), cmd_img_create, NULL, 0},
    {"part.add","1.0", "在【未分配空间】里新建一块分区（只改分区表，不格式化；空间不够报 PART-NOSPACE）",
        A_PARTADD,(int)(sizeof A_PARTADD/sizeof A_PARTADD[0]), cmd_part_add, NULL, 0},
    {"part.remove","1.0", "删掉一块分区（只改分区表；扩展分区容器要先删里面的逻辑分区）",
        A_PARTREMOVE,(int)(sizeof A_PARTREMOVE/sizeof A_PARTREMOVE[0]), cmd_part_remove, NULL, 0},
    {"part.resize","1.0", "调整【最后一块】分区的大小（改分区表；不动别人的起点）",
        A_PARTRESIZE,(int)(sizeof A_PARTRESIZE/sizeof A_PARTRESIZE[0]), cmd_part_resize, NULL, 0},
    {"img.compact","1.0", "另存为紧凑映像（源只读；只写非零块；绝不碰源文件）",
        A_IMGCOMPACT,(int)(sizeof A_IMGCOMPACT/sizeof A_IMGCOMPACT[0]), cmd_img_compact, NULL, 0},
    {"img.trim","1.0", "按客户机空闲位图原地释放 qcow2 簇（NTFS/FAT32；有快照或 backing 拒绝）",
        A_IMGTRIM,(int)(sizeof A_IMGTRIM/sizeof A_IMGTRIM[0]), cmd_img_trim, NULL, 0},
    {"ntfs.label","1.0", "只改 NTFS 卷标（不格式化、不动文件数据）",
        A_FSLABEL,(int)(sizeof A_FSLABEL/sizeof A_FSLABEL[0]), cmd_ntfs_label, NULL, 0},
    {"fat.label","1.0", "只改 FAT32 卷标（BPB + 根目录卷标项；不格式化）",
        A_FSLABEL,(int)(sizeof A_FSLABEL/sizeof A_FSLABEL[0]), cmd_fat_label, NULL, 0},
    {"part.export","1.0", "导出分区表成 .json（只读；可另存原始表区）",
        A_PTEXPORT,(int)(sizeof A_PTEXPORT/sizeof A_PTEXPORT[0]), cmd_part_export, NULL, 0},
    {"part.apply","1.0", "按 .json 写整张分区表（LBA/GUID 原样；只改表，不碰数据）",
        A_PTAPPLY,(int)(sizeof A_PTAPPLY/sizeof A_PTAPPLY[0]), cmd_part_apply, NULL, 0},
    {"fs.format","1.0", "把一个分区格式化成 NTFS / FAT32（不用再调 imgntfsfs/imgmkfs）",
        A_FSFORMAT,(int)(sizeof A_FSFORMAT/sizeof A_FSFORMAT[0]), cmd_fs_format, NULL, 0},
    {"ntfs.put","1.0", "把本地文件写进 NTFS 分区（自动建父目录）",
        A_NTFSPUT,(int)(sizeof A_NTFSPUT/sizeof A_NTFSPUT[0]), cmd_ntfs_put, NULL, 0},
    {"ntfs.export","1.0", "把 NTFS 分区里的文件导出到本地（抽 bootmgr/boot.sdi/winre.wim）",
        A_NTFSEXPORT,(int)(sizeof A_NTFSEXPORT/sizeof A_NTFSEXPORT[0]), cmd_ntfs_export, NULL, 0},
    {"version","1.0","工具的构建标识（日期/提交号）：App 用它核对交付的是哪一份库",
        NULL, 0, cmd_version, NULL, 0},
    {"win.finish", "1.0", "直接写入后首次启动收尾：离线把 SYSTEM\\Setup\\Status\\ChildCompletion\\setup.exe 改成 3（微软官方修复）",
        A_WINFINISH,(int)(sizeof A_WINFINISH/sizeof A_WINFINISH[0]), cmd_win_finish, NULL, 0},
    {"reg.get", "1.0", "离线读注册表值（SYSTEM/SOFTWARE/SAM/SECURITY/DEFAULT；映像必须关机）",
        A_REGGET,(int)(sizeof A_REGGET/sizeof A_REGGET[0]), cmd_reg_get, NULL, 0},
    {"reg.set", "1.0", "离线写注册表值（事务性：提取→改→校验→写回，失败分区不变）",
        A_REGSET,(int)(sizeof A_REGSET/sizeof A_REGSET[0]), cmd_reg_set, NULL, 0},
    {"reg.list","1.0", "列注册表键的子键与值",
        A_REGLIST,(int)(sizeof A_REGLIST/sizeof A_REGLIST[0]), cmd_reg_list, NULL, 0},
    {"reg.del", "1.0", "离线删除注册表值或键（删键默认不递归）",
        A_REGDEL,(int)(sizeof A_REGDEL/sizeof A_REGDEL[0]), cmd_reg_del, NULL, 0},
    {"wim.capture", "1.0", "把 qcow2 里的系统盘捕获成 WIM/ESD（先抽到临时目录再打包；estimate=true 只估算）",
        A_WIMCAP,(int)(sizeof A_WIMCAP/sizeof A_WIMCAP[0]), cmd_wim_capture, NULL, 0},
    {"copy", "1.0", "统一复制：同卷/跨分区/跨映像/宿主/ISO/WIM；路径用 !p3: 语法",
        A_COPY,(int)(sizeof A_COPY/sizeof A_COPY[0]), cmd_copy,
        "copy \"<源路径>\" \"<目标路径>\" [--conflict=fail|overwrite|rename]", 2},
    {"move", "1.0", "统一移动/改名：同卷纯索引；跨卷复制+校验+删源；ISO/WIM 只读",
        A_MOVE,(int)(sizeof A_MOVE/sizeof A_MOVE[0]), cmd_move,
        "move \"<源路径>\" \"<目标路径>\" [--allow_host_delete=true]", 2},
    {"rm", "1.0", "统一删除：映像内文件/目录、宿主文件/目录；递归需显式开启",
        A_RM,(int)(sizeof A_RM/sizeof A_RM[0]), cmd_rm,
        "rm \"<路径>\" [--recursive=true] [--allow_host_delete=true]", 1},
    {"rename", "1.0", "统一改名：同父目录改名（copy/move 的便捷入口）",
        A_RENAME,(int)(sizeof A_RENAME/sizeof A_RENAME[0]), cmd_rename,
        "rename \"<路径>\" \"<新名字>\"", 2},
    {"ntfs.touch", "1.0", "设置盘内路径的时间（--now / --time= / --ref= 三选一；0=该项不改）",
        A_NTFSTOUCH,(int)(sizeof A_NTFSTOUCH/sizeof A_NTFSTOUCH[0]), cmd_ntfs_touch, NULL, 0},
    {"fat.touch",  "1.0", "设置 FAT 上路径的时间（同上；精度 2 秒）",
        A_FATTOUCH,(int)(sizeof A_FATTOUCH/sizeof A_FATTOUCH[0]), cmd_fat_touch, NULL, 0},
    {"ntfs.parent","1.0", "只读检查目录的 \"..\" 父引用是否自洽（搬过目录之后必查）",
        A_NTFSRPARENT,(int)(sizeof A_NTFSRPARENT/sizeof A_NTFSRPARENT[0]), cmd_ntfs_parent, NULL, 0},
    {"wim.identify","1.0", "从内容识别 WIM/ESD/ISO 里的 Windows 版本（不信文件名）",
        A_WIMID,(int)(sizeof A_WIMID/sizeof A_WIMID[0]), cmd_wim_identify, NULL, 0},
    {"wim.ls",     "1.0", "浏览 WIM/ESD 内部目录树（只读；--wim 可直接给 ISO，工具自己找出 install.wim/.esd）",
        A_WIMLS,(int)(sizeof A_WIMLS/sizeof A_WIMLS[0]), cmd_wim_ls, NULL, 0},
    {"wim.extract","1.0", "从 WIM/ESD 里只读提取【单个文件】到本地（--wim 可直接给 ISO；取消不留半成品）",
        A_WIMEXTRACT,(int)(sizeof A_WIMEXTRACT/sizeof A_WIMEXTRACT[0]), cmd_wim_extract, NULL, 0},
    {"wim.file-version","1.0", "读映像里某个【文件】的 PE 版本（跨映像取引导文件前的版本闸门；不落临时文件）",
        A_WIMFILEVER,(int)(sizeof A_WIMFILEVER/sizeof A_WIMFILEVER[0]), cmd_wim_file_version, NULL, 0},
    {"win.version","1.0", "读已装盘的真实版本：ntoskrnl 的 PE 版本 + 注册表 + SKU 交叉验证",
        A_WINVER,(int)(sizeof A_WINVER/sizeof A_WINVER[0]), cmd_win_version, NULL, 0},
    {"bcd.build","1.0", "从模板直接构建 BCD（Win7 的 BCD-Template 没有绑定，必须这条）",
        A_BCDBUILD,(int)(sizeof A_BCDBUILD/sizeof A_BCDBUILD[0]), cmd_bcd_build, NULL, 0},
    {"bcd.create","1.0", "用内置模板建一张能启动的 BCD（Vista~11 / BIOS+UEFI / 32+64 位；不要参考盘）",
        A_BCDCREATE,(int)(sizeof A_BCDCREATE/sizeof A_BCDCREATE[0]), cmd_bcd_create, NULL, 0},
    {"qc.verify","1.0", "qcow2 结构自检（别名/越界/坏 L2），进程被杀后怀疑盘坏了就跑它",
        A_QCVERIFY,(int)(sizeof A_QCVERIFY/sizeof A_QCVERIFY[0]), cmd_qc_verify, NULL, 0},
    {"bcd.gen-uefi","1.0", "UEFI 换盘：按新盘 GUID 重写 BCD（= imggenbcd，同一份库实现）",
        A_BCDGEN,(int)(sizeof A_BCDGEN/sizeof A_BCDGEN[0]), cmd_bcd_gen_uefi, NULL, 0},
    {"boot.tpl-inspect","1.0","模板预览（只读）：模板元数据 + 对本盘的适用性判定",
        A_TPLINSPECT,(int)(sizeof A_TPLINSPECT/sizeof A_TPLINSPECT[0]), cmd_boot_tpl_inspect, NULL, 0},
    {"boot.tpl-export","1.0","导出 BCD 模板（骨架 + 元数据 JSON；设备绑定清零）",
        A_TPLEXPORT,(int)(sizeof A_TPLEXPORT/sizeof A_TPLEXPORT[0]), cmd_boot_tpl_export, NULL, 0},
    {"boot.tpl-apply","1.0", "应用 BCD 模板（跨族/架构/固件拒绝；同族小版本不同→返回双方版本号）",
        A_TPLAPPLY,(int)(sizeof A_TPLAPPLY/sizeof A_TPLAPPLY[0]), cmd_boot_tpl_apply, NULL, 0},
    {"boot.plan","1.0", "判定『这个系统能不能用 GPT/MBR 启动』（只读，不写盘；App 灰按钮/弹框的依据）",
        A_BOOTPLAN,(int)(sizeof A_BOOTPLAN/sizeof A_BOOTPLAN[0]), cmd_boot_plan, NULL, 0},
    {"boot.populate","1.0", "一条命令铺完引导（UEFI：ESP+bootmgfw+Fonts+boot.sdi+BCD；BIOS：活动标志+bootmgr+MBR/VBR 码+BCD）",
        A_BOOTPOP,(int)(sizeof A_BOOTPOP/sizeof A_BOOTPOP[0]), cmd_boot_populate, NULL, 0},
    {"boot.check","1.0", "引导链自检（= imgbc）：返回逐项结果 + 必需项失败数",
        A_BOOTCHECK,(int)(sizeof A_BOOTCHECK/sizeof A_BOOTCHECK[0]), cmd_boot_check, NULL, 0},
    {"part.attr","1.0", "设 GPT 分区属性（恢复分区官方要求 0x8000000000000001）",
        A_PARTATTR,(int)(sizeof A_PARTATTR/sizeof A_PARTATTR[0]), cmd_part_attr, NULL, 0},
    {"du",        "1.0", "算文件/文件夹的递归大小与项数（统一路径；超限回 truncated 不报错；REQ-40）",
        A_DU,(int)(sizeof A_DU/sizeof A_DU[0]), cmd_du, NULL, 0},
    {"archive.create","1.0", "把选中的文件/目录打包成 zip/7z/tar/tgz/iso（rar 不能创建；REQ-39）",
        A_ARCCREATE,(int)(sizeof A_ARCCREATE/sizeof A_ARCCREATE[0]), cmd_archive_create, NULL, 0},
    {"archive.ls","1.0", "列压缩包（只读；返回 index_token 供续页；REQ-31）",
        A_ARCLS,(int)(sizeof A_ARCLS/sizeof A_ARCLS[0]), cmd_archive_ls, NULL, 0},
    {"archive.extract","1.0", "从压缩包抽文件/目录子树（只读；REQ-31）",
        A_ARCEXT,(int)(sizeof A_ARCEXT/sizeof A_ARCEXT[0]), cmd_archive_extract, NULL, 0},
    {"ntfs.mkdir","1.0", "建目录（REQ-30.2；--parents 默认 true）",
        A_NTFSMKDIR,(int)(sizeof A_NTFSMKDIR/sizeof A_NTFSMKDIR[0]), cmd_ntfs_mkdir, NULL, 0},
    {"ntfs.stat","1.0", "单条详情：大小/占用/四时间/属性/ino/链接数（REQ-30.3）",
        A_NTFSSTAT,(int)(sizeof A_NTFSSTAT/sizeof A_NTFSSTAT[0]), cmd_ntfs_stat, NULL, 0},
    {"fat.mkdir","1.0", "建目录（FAT32；REQ-30.2）",
        A_FATMKDIR,(int)(sizeof A_FATMKDIR/sizeof A_FATMKDIR[0]), cmd_fat_mkdir, NULL, 0},
    {"fat.stat","1.0", "单条详情（FAT32；REQ-30.3）",
        A_FATSTAT,(int)(sizeof A_FATSTAT/sizeof A_FATSTAT[0]), cmd_fat_stat, NULL, 0},
    {"wim.put","1.0", "把 WIM/ESD/ISO 里的一个文件直接写进分区（0 临时文件；REQ-29）",
        A_WIMPUT,(int)(sizeof A_WIMPUT/sizeof A_WIMPUT[0]), cmd_wim_put, NULL, 0},
    {"winre.find","1.0", "在 WIM/ESD 里定位 Winre.wim（只读；未找到报 WINRE-NOTFOUND）",
        A_WINREFIND,(int)(sizeof A_WINREFIND/sizeof A_WINREFIND[0]), cmd_winre_find, NULL, 0},
    {"recovery.status","1.0", "WinRE 状态（只读）：三处找 Winre.wim + 恢复分区空不空（卡片字段）",
        A_RECSTATUS,(int)(sizeof A_RECSTATUS/sizeof A_RECSTATUS[0]), cmd_recovery_status, NULL, 0},
    {"recovery.populate","1.0", "把 WinRE 填进【独立恢复分区】（UEFI）",
        A_RECPOP,(int)(sizeof A_RECPOP/sizeof A_RECPOP[0]), cmd_recovery_populate, NULL, 0},
    {"recovery.populate-system","1.0", "把 WinRE 填进系统分区的 Windows\\System32\\Recovery（BIOS）",
        A_RECPOP,(int)(sizeof A_RECPOP/sizeof A_RECPOP[0]), cmd_recovery_populate_system, NULL, 0},
    {"iso.probe","1.0", "只读卷描述符（UDF/ISO9660/Joliet）不列目录；崩了能二分是打开还是列举（REQ-45）",
        A_ISOPROBE,(int)(sizeof A_ISOPROBE/sizeof A_ISOPROBE[0]), cmd_iso_probe, NULL, 0},
    {"iso.ls","1.0", "列 ISO 里的目录（UDF；Windows 安装盘的 install.wim 只在 UDF 里）",
        A_ISOLS,(int)(sizeof A_ISOLS/sizeof A_ISOLS[0]), cmd_iso_ls, NULL, 0},
    {"iso.extract","1.0", "从 ISO 里抽文件到本地（流式 1 MiB；App 不必自己装 ISO 库）",
        A_ISOEXTRACT,(int)(sizeof A_ISOEXTRACT/sizeof A_ISOEXTRACT[0]), cmd_iso_extract, NULL, 0},
    {"sys.tmpdir","1.0", "查询当前生效的临时目录（路径、来源、剩余空间、清扫数）",
        NULL, 0, cmd_sys_tmpdir, NULL, 0},
    {"cmd.list",  "1.0", "导出命令表（名字/参数规格/帮助），App 可据此自动生成界面",
        NULL, 0, cmd_cmd_list, NULL, 0},
};
const int IMG_CMD_COUNT = (int)(sizeof IMG_CMD_TABLE/sizeof IMG_CMD_TABLE[0]);
