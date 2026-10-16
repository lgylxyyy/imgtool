#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "img_io.h"
#include "img_event.h"   /* I/O 看门狗（无进展超时）*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __linux__
#include <linux/falloc.h>
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE 0x02
#define FALLOC_FL_KEEP_SIZE   0x01
#endif
#endif

struct img_io {
    int      fd;
    char     path[512];   /* 路径模式记录路径（fd 模式为空）*/
    bool     own_fd;       /* 关闭时是否 close() */
    bool     writable;
    bool     is_fd_mode;   /* 调用者给的 fd（Android SAF）*/
    uint64_t size_cache;   /* 0 = 需要重新 stat */
    bool     size_valid;
};

static int map_errno(int e){
    switch(e){
    case 0:      return IMG_OK;
    case EACCES: case EPERM:  return IMG_EINVAL;
    case ENOENT: return IMG_ENOENT;
    case EIO:    return IMG_EIO;
    case ENOSPC: case EDQUOT: return IMG_EFULL;
    case ENOMEM: case EAGAIN: return IMG_ENOMEM;
    case ERANGE: return IMG_ETRUNC;
    default:     return IMG_EIO;
    }
}

img_io *img_io_open_path(const char *path, const img_io_opts_t *o){
    if(!path) return NULL;
    int flags = (o && o->writable) ? (O_RDWR | O_CREAT) : (O_RDONLY);
    if(o && o->writable) flags |= O_CLOEXEC;
    if(o && o->truncate) flags |= O_TRUNC;
    int fd = open(path, flags, 0644);
    if(fd < 0) return NULL;
    img_io_opts_t oo = o?*o:(img_io_opts_t){0};
    if(oo.initial_size && oo.writable){
        /* 预置文件大小（qcow2 创建用）—— 用 ftruncate 而非占满空间 */
        if(ftruncate(fd, (off_t)oo.initial_size) != 0){ close(fd); return NULL; }
    }
    img_io *io = calloc(1,sizeof *io);
    if(!io){ close(fd); return NULL; }
    io->fd = fd;
    snprintf(io->path,sizeof io->path,"%s",path); io->own_fd = true; io->writable = (flags & O_RDWR)==O_RDWR;
    io->is_fd_mode = false;
    return io;
}

img_io *img_io_open_fd(int fd, bool take_ownership, const img_io_opts_t *o){
    /* ★ fd 生命周期契约（DESIGN-OUTPUT.md 第八节）：
     *   take_ownership=true → img_io_close() 会关掉它；false → 调用方自己关，
     *   且必须保证在 img_io_close() 之前一直有效（App 侧别提前 close）。
     *   这里先做基本校验：fd 必须有效、可 seek、且是普通文件/块设备
     *   （传进来目录 fd、管道、已关闭的 fd 都会立刻得到明确错误，而不是后面莫名失败）*/
    if(fd<0) return NULL;
    struct stat st;
    if(fstat(fd,&st)<0) return NULL;
    if(!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) return NULL;
    {
        int fl=fcntl(fd,F_GETFL);
        if(fl<0) return NULL;
        int acc=fl & O_ACCMODE;
        if(o && o->writable && acc==O_RDONLY) return NULL;
        off_t cur=lseek(fd,0,SEEK_CUR);      /* 管道/不可 seek 在这里就会被拒 */
        if(cur==(off_t)-1) return NULL;
    }
    if(fd < 0) return NULL;
    img_io *io = calloc(1,sizeof *io);
    if(!io) return NULL;
    io->fd = fd; io->own_fd = take_ownership;
    io->writable = true;                 /* SAF 给的 fd 通常 rw；假定可写，写失败再报错 */
    io->is_fd_mode = true;
    if(o && o->initial_size && io->writable){
        /* Android SAF 的 fd 可能不支持 ftruncate，失败也不致命 */
        int trc = ftruncate(fd,(off_t)o->initial_size);
        (void)trc;
    }
    return io;
}

void img_io_close(img_io *io){
    if(!io) return;
    if(io->own_fd && io->fd >= 0) close(io->fd);
    free(io);
}

int img_io_read(img_io *io, uint64_t off, void *buf, size_t len, size_t *done){
    if(img_io_timed_out()) return IMG_ETIMEDOUT;   /* 看门狗：卡死后后续 I/O 立刻失败 */
    if(!io||!buf) return IMG_EINVAL;
    size_t got = 0;
    while(got < len){
        ssize_t r = pread(io->fd, (char*)buf+got, len-got, (off_t)(off+got));
        if(r == 0) break;                       /* EOF */
        if(r < 0){
            if(errno == EINTR) continue;
            return map_errno(errno);
        }
        got += (size_t)r;
    }
    if(done) *done = got;
    img_io_touch();                          /* 这次 I/O 完成了 → 看门狗重新计时 */
    return IMG_OK;
}

int img_io_write(img_io *io, uint64_t off, const void *buf, size_t len, size_t *done){
    if(img_io_timed_out()) return IMG_ETIMEDOUT;   /* 看门狗：卡死后后续 I/O 立刻失败 */
    if(!io||!buf) return IMG_EINVAL;
    if(!io->writable) return IMG_EINVAL;
    size_t put = 0;
    while(put < len){
        ssize_t r = pwrite(io->fd, (const char*)buf+put, len-put, (off_t)(off+put));
        if(r < 0){
            if(errno == EINTR) continue;
            return map_errno(errno);
        }
        put += (size_t)r;
    }
    if(done) *done = put;
    io->size_valid = false;                  /* 大小可能变了 */
    img_io_touch();                          /* 这次 I/O 完成了 → 看门狗重新计时 */
    return IMG_OK;
}

int img_io_read_full(img_io *io, uint64_t off, void *buf, size_t len){
    size_t d=0; int rc = img_io_read(io,off,buf,len,&d);
    if(rc) return rc;
    return (d==len) ? IMG_OK : IMG_ETRUNC;
}
int img_io_write_full(img_io *io, uint64_t off, const void *buf, size_t len){
    size_t d=0; int rc = img_io_write(io,off,buf,len,&d);
    if(rc) return rc;
    return (d==len) ? IMG_OK : IMG_ETRUNC;
}

int img_io_sync(img_io *io){
    if(!io) return IMG_EINVAL;
    return fsync(io->fd)==0 ? IMG_OK : map_errno(errno);
}

int img_io_truncate(img_io *io, uint64_t new_size){
    if(!io||!io->writable) return IMG_EINVAL;
    return ftruncate(io->fd,(off_t)new_size)==0 ? IMG_OK : map_errno(errno);
}

uint64_t img_io_size(img_io *io){
    if(!io) return 0;
    if(io->size_valid) return io->size_cache;
    struct stat st;
    if(fstat(io->fd,&st)==0){
        io->size_cache = (uint64_t)st.st_size;
        io->size_valid = true;
        return io->size_cache;
    }
    return 0;
}

bool     img_io_writable(const img_io *io){ return io && io->writable; }
const char *img_io_path(const img_io *io){
    if(!io) return NULL;
    return (io->is_fd_mode || !io->path[0]) ? NULL : io->path;
}
bool     img_io_is_fd(const img_io *io){ return io && io->is_fd_mode; }
int      img_io_fd(img_io *io){ return io ? io->fd : -1; }

int img_io_punch(img_io *io, uint64_t off, uint64_t len){
    if(!io||!io->writable) return IMG_EINVAL;
#ifdef __linux__
    if(fallocate(io->fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
                 (off_t)off, (off_t)len)==0) return IMG_OK;
    /* 多数文件系统不支持 → 静默降级：写零（仍能腾出空间，只是慢） */
#endif
    size_t z = len>65536?65536:(size_t)len;
    char *nul = calloc(1,z);
    if(!nul) return IMG_ENOMEM;
    int rc = IMG_OK;
    for(uint64_t i=0;i<len;i+=z){
        size_t n = (len-i)<z?(size_t)(len-i):z;
        if((rc = img_io_write(io, off+i, nul, n, NULL))!=IMG_OK) break;
    }
    free(nul);
    return rc;
}
