#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* ntfs_bridge.c — 我们的 10 个回调 → ntfs_device_operations */
#include "ntfs_bridge.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static img_ntfs_ctx *CTX(void){ return img_ntfs_get_active_ctx(); }

static int  b_open (struct ntfs_device *d, int f){ (void)d;(void)f; return img_ntfs_cb_open((void*)CTX(),0); }
static int  b_close(struct ntfs_device *d){ (void)d; return img_ntfs_cb_close((void*)CTX()); }
static s64  b_seek (struct ntfs_device *d, s64 o, int w){ (void)d; return img_ntfs_cb_seek((void*)CTX(),(long long)o,w); }
static s64  b_read (struct ntfs_device *d, void *p, s64 c){ (void)d; return img_ntfs_cb_read((void*)CTX(),p,(long long)c); }
static s64  b_write(struct ntfs_device *d, const void *p, s64 c){ (void)d; return img_ntfs_cb_write((void*)CTX(),p,(long long)c); }
static s64  b_pread(struct ntfs_device *d, void *p, s64 c, s64 o){ (void)d; return img_ntfs_cb_pread((void*)CTX(),p,(long long)c,(long long)o); }
static s64  b_pwrite(struct ntfs_device *d, const void *p, s64 c, s64 o){ (void)d; return img_ntfs_cb_pwrite((void*)CTX(),p,(long long)c,(long long)o); }
static int  b_sync (struct ntfs_device *d){ (void)d; return img_ntfs_cb_sync((void*)CTX()); }
static int  b_stat (struct ntfs_device *d, struct stat *s){ (void)d; return img_ntfs_cb_stat((void*)CTX(),s); }
static int  b_ioctl(struct ntfs_device *d, unsigned long r, void *a){ (void)d; return img_ntfs_cb_ioctl((void*)CTX(),r,a); }

static struct ntfs_device_operations ROPS = {
    b_open, b_close, b_seek, b_read, b_write,
    b_pread, b_pwrite, b_sync, b_stat, b_ioctl,
};
struct ntfs_device_operations *img_ntfs_device_ops(void){ return &ROPS; }
