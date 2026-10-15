/* Stage 5 关键实验：wimlib 直接 apply WIM 到 qcow2 里的 NTFS 分区
 * 原理：wimlib 的 NTFS 模式调 ntfs_mount(target) → 该函数用
 *       ntfs_device_default_io_ops 宏（= ntfs_device_unix_io_ops）
 *       → 我们在 img_ntfs_fmt.c 里定义的同名符号 → 我们的 10 个回调 → qcow2
 * 结果：零临时目录、零 loop、零 FUSE、零 root */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wimlib.h>

int main(int argc, char **argv){
    if(argc<5){ fprintf(stderr,"用法: %s <qcow2> <part_lba> <wim> <index>\n",argv[0]); return 2; }
    uint64_t lba = strtoull(argv[2],NULL,0);
    int idx = atoi(argv[4]);

    img_io_opts_t io={0}; io.writable=true;
    img_io *f = img_io_open_path(argv[1], &io);
    if(!f){ printf("❌ 打不开镜像\n"); return 1; }
    img_qcow2_opts_t qo={0};
    img_qcow2 *q = img_qcow2_open(f, &qo);
    if(!q || img_qcow2_write_init(q)!=IMG_OK){ printf("❌ qcow2\n"); return 1; }
    img_parttable_t t; img_parttable_read(q,&t,0);
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==lba)
            pb = (t.parts[i].last_lba - t.parts[i].first_lba + 1)*512;
    if(!pb){ printf("❌ 找不到分区\n"); return 1; }
    printf("  分区 LBA=%llu 大小=%.1f MiB\n",(unsigned long long)lba, pb/1048576.0);

    /* wimlib 需要能 stat 到目标是普通文件 → 造占位文件 */
    /* 不截断：wimlib 需要 stat 到它、且按普通文件探测目标类型 */
    int d=open("imgtool-dev", O_RDWR|O_CREAT, 0666); if(d>=0) close(d);

    img_ntfs_ctx_activate(q, lba, pb);      /* 让回调落到这个分区 */

    if(wimlib_global_init(0)!=0){ printf("❌ wimlib_global_init\n"); return 1; }
    WIMStruct *wim=NULL;
    int r = wimlib_open_wim(argv[3], 0, &wim);
    if(r){ printf("❌ open_wim: %s\n", wimlib_get_error_string(r)); return 1; }
    printf("  WIM 打开成功\n");

    printf("  开始 apply →（wimlib 内部走我们的 ntfs_device_unix_io_ops）\n");
    r = wimlib_extract_image(wim, idx, "imgtool-dev", WIMLIB_EXTRACT_FLAG_NTFS);
    if(r){ printf("❌ extract_image: %s\n", wimlib_get_error_string(r)); return 1; }
    printf("  ✅ apply 完成\n");
    wimlib_free(wim);
    img_ntfs_ctx_deactivate();
    return 0;
}
