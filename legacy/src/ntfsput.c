/* ntfsput: 把宿主文件写入 NTFS 卷（自动建父目录），可设 Windows 属性
 * 用法: ntfsput <volume> <hostfile> <ntfsPath> [attrHex]
 *   attrHex 例: 0x06 (SYSTEM|HIDDEN)  0x22 (ARCHIVE|SYSTEM)
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <time.h>
#define __timespec_defined
#ifndef S_IFDIR
#define S_IFDIR 0040000
#define S_IFREG 0100000
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/layout.h>

static u8 tonc(ntfschar *dst, const char *s, int max){ u8 n=0; while(*s && n<max) dst[n++]=(ntfschar)(unsigned char)*s++; return n; }

int main(int argc, char **argv){
    if (argc < 4){ fprintf(stderr,"用法: %s <volume> <hostfile> <ntfsPath> [attrHex]\n", argv[0]); return 2; }
    const char *volPath = argv[1], *host = argv[2], *ntfsPath = argv[3];
    unsigned attr = (argc>4) ? (unsigned)strtoul(argv[4], NULL, 0) : 0x20;

    ntfs_volume *vol = ntfs_mount(volPath, 0);
    if(!vol){ fprintf(stderr,"挂载失败: %s\n", strerror(errno)); return 1; }
    struct SECURITY_CONTEXT scx; memset(&scx,0,sizeof scx);
    scx.vol = vol; scx.uid=0; scx.gid=0; scx.umask=0;

    char *dup = strdup(ntfsPath);
    char *save=NULL;
    ntfs_inode *cur = ntfs_inode_open(vol, FILE_root);
    if(!cur){ fprintf(stderr,"root 失败\n"); return 1; }
    ntfs_inode *root = cur;
    /* 逐级确保目录存在 */
    char *tok = strtok_r(dup, "/", &save);
    char *next = strtok_r(NULL, "/", &save);
    while (next) {
        ntfschar nm[256]; u8 l = tonc(nm, tok, 255);
        ntfs_inode *child = NULL;
        /* 找已存在的 */
        char prefix[600]="/"; strncat(prefix, tok, 100); /* 简化：用 pathname 定位 */
        child = ntfs_pathname_to_inode(vol, root, ntfsPath); /* 占位，下面改用相对查找 */
        if (child) ntfs_inode_close(child);
        /* 用 dir 列表判断 */
        ntfs_inode *found = NULL;
        int (*dummy)(void)=NULL; (void)dummy;
        /* 简单做法：尝试 create，EEXIST 则 open */
        ntfs_inode *nd = ntfs_create(cur, 0, nm, l, S_IFDIR);
        if (!nd) {
            if (errno != EEXIST) { fprintf(stderr,"建目录失败 %s: %s\n", tok, strerror(errno)); return 1; }
            /* 打开已有：按名字在 cur 下查找 */
            ntfs_inode *tmp = ntfs_pathname_to_inode(vol, root, ntfsPath);
            if (tmp) { ntfs_inode_close(tmp); }
            /* 直接用 readdir 找不到，退回：创建失败但可能已存在 → 用 pathname 相对 cur */
            nd = ntfs_pathname_to_inode(vol, cur, tok);
            if (!nd) { fprintf(stderr,"目录 %s 无法定位\n", tok); return 1; }
        }
        if (cur != root) ntfs_inode_close(cur);
        cur = nd;
        tok = next; next = strtok_r(NULL, "/", &save);
    }
    /* 最后一级 = 文件名 */
    ntfschar fn[256]; u8 fl = tonc(fn, tok, 255);
    ntfs_inode *f = ntfs_create(cur, 0, fn, fl, S_IFREG);
    if (!f){
        if (errno != EEXIST){ fprintf(stderr,"建文件失败 %s: %s\n", tok, strerror(errno)); return 1; }
        ntfs_inode_close(cur);
        cur = ntfs_pathname_to_inode(vol, root, ntfsPath);
        if (!cur){ fprintf(stderr,"定位已有文件失败\n"); return 1; }
        f = ntfs_inode_open(vol, cur->mft_no); /* 取可写句柄 */
        ntfs_inode_close(cur);
        if (!f) { fprintf(stderr,"打开已有文件失败\n"); return 1; }
    }
    /* 写数据 */
    FILE *fp = fopen(host, "rb");
    if (!fp){ fprintf(stderr,"读宿主文件失败: %s\n", strerror(errno)); return 1; }
    ntfs_attr *na = ntfs_attr_open(f, AT_DATA, NULL, 0);
    if (!na) { fprintf(stderr,"打开 $DATA 失败\n"); return 1; }
    char buf[1<<20]; size_t n; long long off=0; long long total=0;
    while ((n = fread(buf,1,sizeof buf,fp)) > 0){
        if (ntfs_attr_pwrite(na, off, n, buf) != (long long)n){ fprintf(stderr,"写入失败 @%lld\n", off); return 1; }
        off += n; total += n;
    }
    fclose(fp); ntfs_attr_pclose(na);

    char av[4]; av[0]=attr&0xff; av[1]=(attr>>8)&0xff; av[2]=(attr>>16)&0xff; av[3]=(attr>>24)&0xff;
    int ar = ntfs_set_ntfs_attrib(f, av, 4, 0);
    /* 关键：先关父目录再关文件。
     * 实测：文件 close 会把 FILE_NAME 尺寸写入父目录索引，若随后父目录
     * 用陈旧缓存刷盘，会把尺寸覆盖回 0（/Boot/BCD 曾出现 size=0）。 */
    if (cur != root) { ntfs_inode_close(cur); cur = NULL; }
    ntfs_inode_close(f);
    if (cur) ntfs_inode_close(cur);
    ntfs_inode_close(root);
    ntfs_umount(vol, 1);
    printf("写入 %lld 字节 -> %s (attr=0x%x, attrib rc=%d)\n", total, ntfsPath, attr, ar);
    return 0;
}
