#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_ntfs_fmt.c — 进程内 NTFS 格式化
 *
 * 关键手法（★ 零改动上游源码，许可证最干净）：
 *   ntfsprogs 的 mkntfs.c 通过宏
 *       #define ntfs_device_default_io_ops  ntfs_device_unix_io_ops
 *   取得全局结构体地址。我们【在可执行文件里提供同名定义】，
 *   ELF 符号前插会使 mkntfs 用我们的 qcow2 回调，而非库里的文件后端。
 */
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>   /* optind/optarg (getopt) */
#include <time.h>        /* ★ 必须在 ntfs 头之前，否则 ntfstime.h 重定义 timespec */
#include <sys/types.h>
#include <sys/stat.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/device.h>

/* 由上层在调用 format 之前设置；open 回调用它取上下文。 */
static img_ntfs_ctx *g_fmt_ctx = NULL;
img_ntfs_ctx *img_ntfs_get_active_ctx(void){ return g_fmt_ctx; }

static img_ntfs_ctx *g_ext_ctx = NULL;
void img_ntfs_ctx_activate(img_qcow2 *q, uint64_t lba, uint64_t bytes){
    if(g_ext_ctx){ img_ntfs_ctx_free(g_ext_ctx); g_ext_ctx = NULL; }
    g_ext_ctx = img_ntfs_ctx_new(q, lba, bytes);
    g_fmt_ctx  = g_ext_ctx;
}
void img_ntfs_ctx_deactivate(void){
    g_fmt_ctx  = NULL;
    if(g_ext_ctx){ img_ntfs_ctx_free(g_ext_ctx); g_ext_ctx = NULL; }
}
img_ntfs_ctx *img_ntfs_ctx_current(void){ return g_fmt_ctx; }

/* ---------- 10 个回调（dev->d_private 就是我们的 ctx）---------- */

static int  o_open (struct ntfs_device *d, int fl){
    (void)d; (void)fl;
    img_ntfs_ctx *c = img_ntfs_get_active_ctx();
    if(getenv("IMG_NTFS_DEBUG"))
        fprintf(stderr,"  [dbg] o_open ctx=%p (OUR ops=%s)\n",(void*)c, c?"":"← NULL!");
    return img_ntfs_cb_open ((void*)c, fl);
}
static int  o_close(struct ntfs_device *d){ (void)d; return img_ntfs_cb_close((void*)img_ntfs_get_active_ctx()); }
/* ★ mkntfs 用 ntfs_device_alloc(name,0,&ops,NULL) 建设备，d_private 是 NULL，
 *   所以必须从我们自己的全局上下文取，不能指望 d_private。 */
static s64 o_seek (struct ntfs_device *d, s64 o, int w){ (void)d; return img_ntfs_cb_seek((void*)img_ntfs_get_active_ctx(),(long long)o,w); }
static s64 o_read (struct ntfs_device *d, void *b, s64 c){ (void)d; return img_ntfs_cb_read((void*)img_ntfs_get_active_ctx(),b,(long long)c); }
static s64 o_write(struct ntfs_device *d, const void *b, s64 c){ (void)d; return img_ntfs_cb_write((void*)img_ntfs_get_active_ctx(),b,(long long)c); }
static s64 o_pread(struct ntfs_device *d, void *b, s64 c, s64 o){ (void)d; return img_ntfs_cb_pread((void*)img_ntfs_get_active_ctx(),b,(long long)c,(long long)o); }
static s64 o_pwrite(struct ntfs_device *d, const void *b, s64 c, s64 o){ (void)d; return img_ntfs_cb_pwrite((void*)img_ntfs_get_active_ctx(),b,(long long)c,(long long)o); }
static int  o_sync (struct ntfs_device *d){ (void)d; return img_ntfs_cb_sync((void*)img_ntfs_get_active_ctx()); }
static int  o_stat (struct ntfs_device *d, struct stat *s){ (void)d; return img_ntfs_cb_stat((void*)img_ntfs_get_active_ctx(), s); }
static int  o_ioctl(struct ntfs_device *d, unsigned long r, void *a){ (void)d; return img_ntfs_cb_ioctl((void*)img_ntfs_get_active_ctx(),r,a); }

/* ★★ 我们的定义（覆盖 libntfs-3g 里的同名全局量）★★ */
struct ntfs_device_operations ntfs_device_unix_io_ops = {
    o_open, o_close, o_seek, o_read, o_write,
    o_pread, o_pwrite, o_sync, o_stat, o_ioctl,
};

/* mkntfs 的真实入口（由构建时 objcopy 重命名） */
extern int mkntfs_main(int argc, char **argv);

void img_ntfs_fmt_defaults(img_ntfs_fmt_opts_t *o, uint64_t lba, uint64_t bytes){
    if(!o) return;
    memset(o, 0, sizeof *o);
    o->partition_lba = lba;
    o->partition_bytes = bytes;
    o->sector_size = 512;
    o->force = true;
    o->quick = true;
}

int img_ntfs_format(img_qcow2 *q, const img_ntfs_fmt_opts_t *o, img_prog *prog){
    if(!q || !o || o->partition_bytes < 1024*1024) return IMG_EINVAL;

    img_ntfs_ctx *ctx = img_ntfs_ctx_new(q, o->partition_lba, o->partition_bytes);
    if(!ctx) return IMG_ENOMEM;
    g_fmt_ctx = ctx;

    if(prog){
        img_prog_phase(prog, IMG_PH_FORMAT, "格式化 NTFS");
        img_prog_op(prog, IMG_OP_WRITE);
        img_prog_count(prog, 0, 3);
    }

    /* mkntfs 的 argv（dev 名只是个字符串，我们的 open 忽略它）*/
    char label[16];
    const char *lab = (o->label && o->label[0]) ? o->label : "NO NAME";
    snprintf(label, sizeof label, "%.11s", lab);

    /* mkntfs 参数（实测确认）：
     *   -s <BYTES>  扇区大小
     *   -c <BYTES>  簇大小（注意是字节，不是扇区数）
     *   -I          = 禁用索引（不是扇区！）
     *   -F/-Q       强制 / 快速格式化                                      */
    char secbuf[24], spcbuf[24];
    snprintf(secbuf, sizeof secbuf, "%u", o->sector_size ? o->sector_size : 512);
    snprintf(spcbuf, sizeof spcbuf, "%u",
             o->sectors_per_cluster ? (unsigned)o->sectors_per_cluster * 512u : 0u);

    char *argv[20];
    int  argc = 0;
    argv[argc++] = (char*)"mkntfs";
    if(o->force) argv[argc++] = (char*)"-F";
    if(o->quick) argv[argc++] = (char*)"-Q";
    argv[argc++] = (char*)"-L";  argv[argc++] = label;
    argv[argc++] = (char*)"-s";  argv[argc++] = secbuf;
    if(o->sectors_per_cluster){
        argv[argc++] = (char*)"-c";  argv[argc++] = spcbuf;
    }
    argv[argc++] = (char*)"imgtool-dev";
    argv[argc] = NULL;

    /* getopt 全局状态跨调用不重置会越界（实测段错误）——每次调用前清干净 */
    optind  = 1;      /* glibc: 置 1 或 0 均可；置 0 额外重解析 POSIX 环境 */
    opterr  = 1;
    optarg  = NULL;
    optopt  = 0;
    int rc = mkntfs_main(argc, argv);

    g_fmt_ctx = NULL;
    if(prog){
        if(rc == 0){ img_prog_count(prog, 3, 3); img_prog_done(prog, "NTFS 格式化完成"); }
    }
    img_ntfs_ctx_free(ctx);
    return (rc == 0) ? IMG_OK : IMG_EIO;
}

/* ---------- 校验 ---------- */
static inline uint16_t l16(const void *p){ const uint8_t*b=p; return (uint16_t)(b[0]|b[1]<<8); }
static inline uint64_t l64(const void *p){ const uint8_t*b=p;
    return (uint64_t)b[0]|(uint64_t)b[1]<<8|(uint64_t)b[2]<<16|(uint64_t)b[3]<<24|
           (uint64_t)b[4]<<32|(uint64_t)b[5]<<40|(uint64_t)b[6]<<48|(uint64_t)b[7]<<56; }

int img_ntfs_probe(img_qcow2 *q, uint64_t part_lba, img_ntfs_info_t *out){
    if(!q) return IMG_EINVAL;
    uint8_t b[512];
    int rc = img_qcow2_read_full(q, part_lba*512ULL, b, 512);
    if(rc) return rc;
    if(b[3]!='N' || b[4]!='T' || b[5]!='F' || b[6]!='S') return IMG_EFORMAT;
    if(b[510]!=0x55 || b[511]!=0xAA) return IMG_EFORMAT;
    if(out){
        out->bytes_per_sector   = l16(b+0x0B);
        out->sectors_per_cluster= b[0x0D];
        out->total_sectors      = l64(b+0x28);
        out->mft_cluster        = l64(b+0x30);
        out->mftmirr_cluster    = l64(b+0x38);
    }
    return IMG_OK;
}
