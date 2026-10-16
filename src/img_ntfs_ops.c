/* img_ntfs_ops.c —— NTFS 设备通用管道（非 GPL：仅我方代码 + LGPL 头）
 *
 * ★ 为什么从 img_ntfs_fmt.c 拆出来（库缺失降级 #3 的根因）：
 *   "活动 ctx + 10 回调 + ops 结构体 + probe" 是【所有 NTFS 用户】（img_ntfs_fs /
 *   bootcheck / wim 直灌）都要的通用管道；而 img_ntfs_fmt.c 只在 IMG_WITH_NTFS=ON
 *   （mkntfs GPL 对象链入）时才编译 —— 于是"关掉 mkntfs"会连带把 NTFS 全家打没。
 *   现在：本文件跟 NTFS3G 走；fmt.c 只留 mkntfs 专用包装。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>        /* ★ 必须在 ntfs 头之前，否则 ntfstime.h 重定义 timespec */
#include <sys/types.h>
#include <sys/stat.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/device.h>

/* 由上层在调用 format 之前设置；open 回调用它取上下文。 */
static img_ntfs_ctx *g_fmt_ctx = NULL;
/* fmt.c（mkntfs 包装）用它把格式化上下文挂上/摘下 */
void img_ntfs_set_active_ctx(img_ntfs_ctx *ctx){ g_fmt_ctx = ctx; }
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
