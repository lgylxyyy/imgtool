#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* ntfs_dev_test — 证明 libntfs-3g 能通过【我们的回调】读 qcow2 里的 NTFS */
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_ntfs_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/inode.h>

/* ---------- 我们的回调 → ntfs_device_operations 桥接 ---------- */
static int b_open (struct ntfs_device *d, int fl){ return img_ntfs_cb_open (d->d_private, fl); }
static int b_close(struct ntfs_device *d){ return img_ntfs_cb_close(d->d_private); }
static s64  b_seek (struct ntfs_device *d, s64 o, int w){ return img_ntfs_cb_seek(d->d_private,o,w); }
static s64  b_read (struct ntfs_device *d, void *b, s64 c){ return img_ntfs_cb_read(d->d_private,b,c); }
static s64  b_write(struct ntfs_device *d, const void *b, s64 c){ return img_ntfs_cb_write(d->d_private,b,c); }
static s64  b_pread(struct ntfs_device *d, void *b, s64 c, s64 o){ return img_ntfs_cb_pread(d->d_private,b,c,o); }
static s64  b_pwrite(struct ntfs_device *d, const void *b, s64 c, s64 o){ return img_ntfs_cb_pwrite(d->d_private,b,c,o); }
static int  b_sync (struct ntfs_device *d){ return img_ntfs_cb_sync(d->d_private); }
static int  b_stat (struct ntfs_device *d, struct stat *s){ return img_ntfs_cb_stat(d->d_private, s); }
static int  b_ioctl(struct ntfs_device *d, unsigned long r, void *a){ return img_ntfs_cb_ioctl(d->d_private,r,a); }

static struct ntfs_device_operations QOPS = {
    b_open, b_close, b_seek, b_read, b_write,
    b_pread, b_pwrite, b_sync, b_stat, b_ioctl,
};

int main(int argc, char **argv){
    if(argc < 4){
        fprintf(stderr,"用法: %s <qcow2> <partition_lba> <ntfs_raw>\n", argv[0]);
        return 2;
    }
    uint64_t lba = strtoull(argv[2], NULL, 0);

    img_io_opts_t io = {0}; io.writable = true;
    img_io *f = img_io_open_path(argv[1], &io);
    if(!f){ printf("❌ 打不开 qcow2\n"); return 1; }
    img_qcow2_opts_t qo = {0};
    img_qcow2 *q = img_qcow2_open(f, &qo);
    if(!q){ printf("❌ 不是 qcow2\n"); return 1; }
    if(img_qcow2_write_init(q) != IMG_OK){ printf("❌ write_init\n"); return 1; }

    img_parttable_t t;
    if(img_parttable_read(q, &t, 0) != IMG_OK){ printf("❌ 分区表\n"); return 1; }
    uint64_t pbytes = 0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba == lba)
            pbytes = (t.parts[i].last_lba - t.parts[i].first_lba + 1) * 512;
    if(!pbytes){ printf("❌ 无 LBA=%llu 分区\n", (unsigned long long)lba); return 1; }

    /* ① 把 NTFS 镜像灌进分区 */
    FILE *fp = fopen(argv[3], "rb");
    if(!fp){ printf("❌ 打不开 NTFS 镜像\n"); return 1; }
    fseek(fp,0,SEEK_END); long fsz=ftell(fp); fseek(fp,0,SEEK_SET);
    if((uint64_t)fsz > pbytes){ printf("❌ 镜像 > 分区\n"); return 1; }
    unsigned char *buf = malloc((size_t)fsz);
    if(!buf || fread(buf,1,(size_t)fsz,fp)!=(size_t)fsz){ printf("❌ 读镜像\n"); return 1; }
    fclose(fp);
    int rc = img_qcow2_write(q, lba*512, buf, (size_t)fsz);
    free(buf);
    if(rc){ printf("❌ 写分区: %s\n", img_strerror(rc)); return 1; }
    printf("  ① NTFS 镜像 %ld 字节 已写入 qcow2 分区 (LBA %llu)\n", fsz, (unsigned long long)lba);

    /* ② 造设备 + 上下文 */
    img_ntfs_ctx *ctx = img_ntfs_ctx_new(q, lba, pbytes);
    if(!ctx){ printf("❌ 上下文\n"); return 1; }
    ctx->io = f;
    struct ntfs_device *dev = ntfs_device_alloc("imgtool-qcow2", 0, &QOPS, ctx);
    if(!dev){ printf("❌ ntfs_device_alloc\n"); return 1; }

    /* ③ ★ 关键：用 ntfs_device_mount 走我们的回调 */
    ntfs_volume *vol = ntfs_device_mount(dev, 0);
    if(!vol){
        printf("❌ ntfs_device_mount 失败 — errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    printf("  ② ✅ ntfs_device_mount 成功 —— libntfs-3g 接受了我们的 ntfs_device 回调!\n");
    printf("     · cluster_size    = %u\n", vol->cluster_size);
    printf("     · mft_record_size = %u\n", vol->mft_record_size);
    printf("     · nr_clusters     = %lld\n", (long long)vol->nr_clusters);
    printf("     · free_clusters   = %lld\n", (long long)vol->free_clusters);
    printf("     · mft_lcn         = %lld\n", (long long)vol->mft_lcn);

    /* ④ 打开根目录（真正走 MFT 解析 → 会触发我们的 pread） */
    ntfs_inode *root = ntfs_inode_open(vol, FILE_root);
    if(!root){
        printf("❌ 打开根目录失败（MFT 解析没走通）\n");
        ntfs_umount(vol, false);
        return 1;
    }
    printf("  ③ ✅ 根目录 inode 打开成功（MFT 解析经由我们的 pread 读取）\n");
    printf("     · root mft_no = %lld  data_size = %lld 字节\n",
           (long long)root->mft_no, (long long)root->data_size);
    ntfs_inode_close(root);

    ntfs_umount(vol, false);
    printf("  ④ umount 完成\n");
    printf("\n✅ 结论：qcow2 分区已可被 libntfs-3g 当作块设备读取，无临时文件 / 无 loop / 无 FUSE\n");
    return 0;
}
