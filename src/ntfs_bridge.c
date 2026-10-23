#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* ntfs_bridge.c — 我们的 10 个回调 → ntfs_device_operations */
#include "ntfs_bridge.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ★ 优先用设备自带的上下文（d_private）：这样【两个 NTFS 卷可以同时打开】，
 *   跨镜像/跨分区复制才有得谈。d_private 为空时才退回进程级全局（mkntfs 那条老路）。
 *   构造函数会把 ctx 塞进 dev->d_private，见 img_ntfs_fs.c / img_ntfs_ops.c。 */
static img_ntfs_ctx *CTX(struct ntfs_device *d){
    if (d && d->d_private) return (img_ntfs_ctx*)d->d_private;
    return img_ntfs_get_active_ctx();
}

static int  b_open (struct ntfs_device *d, int f){ return img_ntfs_cb_open((void*)CTX(d),f); }
static int  b_close(struct ntfs_device *d){ return img_ntfs_cb_close((void*)CTX(d)); }
static s64  b_seek (struct ntfs_device *d, s64 o, int w){ return img_ntfs_cb_seek((void*)CTX(d),(long long)o,w); }
static s64  b_read (struct ntfs_device *d, void *p, s64 c){ return img_ntfs_cb_read((void*)CTX(d),p,(long long)c); }
static s64  b_write(struct ntfs_device *d, const void *p, s64 c){ return img_ntfs_cb_write((void*)CTX(d),p,(long long)c); }
static s64  b_pread(struct ntfs_device *d, void *p, s64 c, s64 o){ return img_ntfs_cb_pread((void*)CTX(d),p,(long long)c,(long long)o); }
static s64  b_pwrite(struct ntfs_device *d, const void *p, s64 c, s64 o){ return img_ntfs_cb_pwrite((void*)CTX(d),p,(long long)c,(long long)o); }
static int  b_sync (struct ntfs_device *d){ return img_ntfs_cb_sync((void*)CTX(d)); }
static int  b_stat (struct ntfs_device *d, struct stat *s){ return img_ntfs_cb_stat((void*)CTX(d),s); }
static int  b_ioctl(struct ntfs_device *d, unsigned long r, void *a){ return img_ntfs_cb_ioctl((void*)CTX(d),r,a); }

static struct ntfs_device_operations ROPS = {
    b_open, b_close, b_seek, b_read, b_write,
    b_pread, b_pwrite, b_sync, b_stat, b_ioctl,
};
struct ntfs_device_operations *img_ntfs_device_ops(void){ return &ROPS; }
