/* img_host.c — 宿主空间实现（POSIX；Windows 移植时改 _WIN32 分支） */
#define _GNU_SOURCE          /* fstatfs / posix_fallocate 需要（glibc 扩展）*/
#include "img_host.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#include <linux/falloc.h>
#endif

uint64_t img_host_free_bytes_fd(int fd){
#ifdef _WIN32
    (void)fd; return 0;                    /* Windows：走 path 版本（移植时实现） */
#else
    if(fd<0) return 0;
    struct statfs v;
    /* ★ 用 fstatfs 而不是 fstatvfs：后者只收路径，fd 模式（Android SAF）无法获取；
     *   fstatfs(fd) 对"路径模式"和"fd 模式"都适用。*/
    if(fstatfs(fd,&v)<0) return 0;         /* 0 = 未知（如管道类 fd）*/
    /* ★ f_bavail 才是"本进程真能用的"：f_blocks 含 root 保留块，两者能差几个 G */
    return (uint64_t)v.f_bavail * (uint64_t)v.f_bsize;
#endif
}
uint64_t img_host_free_bytes_path(const char *path){
#ifdef _WIN32
    (void)path; return 0;
#else
    if(!path||!*path) return 0;
    struct statvfs v;
    if(statvfs(path,&v)<0) return 0;
    return (uint64_t)v.f_bavail * (uint64_t)v.f_bsize;
#endif
}
uint64_t img_host_free_bytes(const img_io *io){
    if(!io) return 0;
    /* ★ 路径模式与 fd 模式【都有 fd】→ 统一用 fstatfs(fd)：
     *   这样 Android SAF 传进来的 fd 也能查到宿主剩余空间（不依赖 App 传参）。*/
    int fd=img_io_fd((img_io*)io);
    if(fd>=0){
        uint64_t b=img_host_free_bytes_fd(fd);
        if(b) return b;
    }
    const char *p=img_io_path(io);
    return p? img_host_free_bytes_path(p) : 0;
}

uint64_t img_host_estimate_write(uint64_t internal_bytes, bool compressed){
    if(!compressed){
        /* 不压缩：数据占多少宿主就占多少，再加 qcow2 元数据（头/L1/L2/refcount）约 3% */
        return internal_bytes + internal_bytes*3/100 + 4*1024*1024;
    }
    /* 压缩簇：最坏（全零/不可压）等价于不压缩 → 按不压缩给（宁可高估） */
    return img_host_estimate_write(internal_bytes, false);
}

int img_host_reserve(int fd, uint64_t bytes){
#ifdef _WIN32
    (void)fd; (void)bytes; return IMG_ENOTSUP;
#else
    if(fd<0||!bytes) return IMG_EINVAL;
    if(posix_fallocate(fd,0,(off_t)bytes)==0) return IMG_OK;
    /* 文件系统不支持 fallocate（部分 Android 外置存储 / FUSE）→ 明确告知"无法保证" */
    return (errno==ENOTSUP||errno==EOPNOTSUPP) ? IMG_ENOTSUP : IMG_EIO;
#endif
}
