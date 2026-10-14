#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_ntfs_dev.c — qcow2 → ntfs_device 后端（本文件属我方代码，无 GPL 依赖） */
#include "img_ntfs_dev.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

img_ntfs_ctx *img_ntfs_ctx_new(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes){
    if(!q || part_bytes == 0) return NULL;
    img_ntfs_ctx *c = calloc(1, sizeof *c);
    if(!c) return NULL;
    c->q    = q;
    c->base = part_lba * 512ULL;
    c->size = part_bytes;
    c->pos  = 0;
    return c;
}
void img_ntfs_ctx_free(img_ntfs_ctx *c){ free(c); }

static img_ntfs_ctx *ctx_of(void *dev, int *err){
    /* dev 是 struct ntfs_device*，d_private 是第一个可变字段之后——
     * 但为了让桥接层简单，约定：桥接层会把 d_private 直接传进来。 */
    img_ntfs_ctx *c = (img_ntfs_ctx*)dev;
    if(!c){ if(err) *err = EINVAL; return NULL; }
    return c;
}

int img_ntfs_cb_open(void *dev, int flags){
    (void)flags;
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c) return -1;
    c->pos = 0;
    return 0;
}

int img_ntfs_cb_close(void *dev){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c) return -1;
    if(c->io) img_io_sync(c->io);
    return 0;
}

long long img_ntfs_cb_seek(void *dev, long long offset, int whence){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c) { errno = EINVAL; return -1; }
    long long np;
    switch(whence){
    case 0: np = offset; break;                          /* SEEK_SET */
    case 1: np = (long long)c->pos + offset; break;      /* SEEK_CUR */
    case 2: np = (long long)c->size + offset; break;     /* SEEK_END */
    default: errno = EINVAL; return -1;
    }
    if(np < 0){ errno = EINVAL; return -1; }
    c->pos = (uint64_t)np;
    return np;
}

static long long do_read(img_ntfs_ctx *c, void *buf, long long n, uint64_t off){
    if(n <= 0) return 0;
    if(off >= c->size) return 0;                 /* EOF */
    uint64_t avail = c->size - off;
    uint64_t want = (uint64_t)n;
    if(want > avail) want = avail;               /* 不越过分区尾 */
    int rc = img_qcow2_read_full(c->q, c->base + off, buf, (size_t)want);
    if(rc != IMG_OK){ errno = EIO; return -1; }
    return (long long)want;
}

static long long do_write(img_ntfs_ctx *c, const void *buf, long long n, uint64_t off){
    if(n <= 0) return 0;
    if(off >= c->size){ errno = ENOSPC; return -1; }
    uint64_t avail = c->size - off;
    uint64_t want = (uint64_t)n;
    if(want > avail) want = avail;
    int rc = img_qcow2_write(c->q, c->base + off, buf, (size_t)want);
    if(rc != IMG_OK){ errno = (rc==IMG_EFULL)?ENOSPC:EIO; return -1; }
    return (long long)want;
}

long long img_ntfs_cb_read(void *dev, void *buf, long long n){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c){ errno = EINVAL; return -1; }
    long long r = do_read(c, buf, n, c->pos);
    if(r > 0) c->pos += (uint64_t)r;
    return r;
}

long long img_ntfs_cb_write(void *dev, const void *buf, long long n){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c){ errno = EINVAL; return -1; }
    long long r = do_write(c, buf, n, c->pos);
    if(r > 0) c->pos += (uint64_t)r;
    return r;
}

long long img_ntfs_cb_pread(void *dev, void *buf, long long n, long long off){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c || off < 0){ errno = EINVAL; return -1; }
    return do_read(c, buf, n, (uint64_t)off);
}

long long img_ntfs_cb_pwrite(void *dev, const void *buf, long long n, long long off){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c || off < 0){ errno = EINVAL; return -1; }
    return do_write(c, buf, n, (uint64_t)off);
}

int img_ntfs_cb_sync(void *dev){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c) return -1;
    return c->io ? (img_io_sync(c->io)==IMG_OK ? 0 : -1) : 0;
}

int img_ntfs_cb_stat(void *dev, void *stbuf){
    img_ntfs_ctx *c = ctx_of(dev, NULL);
    if(!c || !stbuf){ errno = EINVAL; return -1; }
    struct stat *st = (struct stat*)stbuf;
    memset(st, 0, sizeof *st);
    st->st_size  = (off_t)c->size;
    st->st_mode  = S_IFBLK | 0660;
    st->st_blksize = 512;
    st->st_blocks  = (blkcnt_t)((c->size + 511) / 512);
    return 0;
}

int img_ntfs_cb_ioctl(void *dev, unsigned long request, void *argp){
    (void)dev; (void)argp;
    errno = ENOTTY;
    /* 某些代码会问 BLKGETSIZE64 之类；我们已在 stat 里给了 size，
     * 其余一律返回"不支持"让调用方降级。 */
    (void)request;
    return -1;
}

int img_ntfs_partition_read(img_qcow2 *q, uint64_t part_lba, uint64_t off, void *buf, size_t n){
    if(!q||!buf) return IMG_EINVAL;
    return img_qcow2_read_full(q, part_lba*512ULL + off, buf, n);
}
