/* cancel_test — 取消（P0-2）反向验证：
 *   ① 另一个线程置取消 → 正在写的大文件必须以 IMG_ECANCELLED 停下
 *   ② 取消后【目标文件不存在】且【剩余空间回到基线】→ 不留半成品、不漏簇
 *   ③ 下一个操作必须正常（取消标志不会污染后续操作）
 *   ④ 覆盖写时取消 → 旧文件【完好无损】（顺序：先写新、后释放旧）
 * 用法: cancel_test <qcow2> <ESP_LBA> <源文件>
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* usleep */
#endif
#define _POSIX_C_SOURCE 200809L
#include "img_cmd.h"
#include "img_event.h"
#include "img_fat32_fs.h"
#include "img_qcow2.h"
#include "img_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails=0;
static void chk(const char *w,int ok_){ printf("  %s %s\n",ok_?"✅":"❌",w); if(!ok_) fails++; }

static img_qcow2 *g_q; static uint64_t g_lba; static const char *g_src, *g_dst;
static int g_rc = 999;
static void *th_put(void *a){
    (void)a;
    img_fat32_fs *fs=img_fat32_mount(g_q,g_lba,true);
    if(!fs){ g_rc=IMG_EFORMAT; return NULL; }
    g_rc=img_fat32_put_file(fs,g_src,g_dst,NULL);
    img_fat32_unmount(fs);
    return NULL;
}
static uint64_t free_now(void){
    img_fat32_fs *fs=img_fat32_mount(g_q,g_lba,false);
    uint64_t b=fs?img_fat32_free_bytes(fs):0;
    if(fs) img_fat32_unmount(fs);
    return b;
}
static uint64_t size_of(const char *p){
    img_fat32_fs *fs=img_fat32_mount(g_q,g_lba,false);
    if(!fs) return 0;
    uint64_t s=0; img_fat32_file_size(fs,p,&s);
    img_fat32_unmount(fs);
    return s;
}
static bool exists(const char *p){
    img_fat32_fs *fs=img_fat32_mount(g_q,g_lba,false);
    if(!fs) return false;
    bool e=img_fat32_path_exists(fs,p);
    img_fat32_unmount(fs);
    return e;
}

int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"用法: %s <qcow2> <ESP_LBA> <源文件>\n",argv[0]); return 2; }
    setvbuf(stdout,NULL,_IONBF,0);
    struct stat st_src;
    if(stat(argv[3],&st_src)!=0){ printf("❌ 源文件不存在\n"); return 1; }

    uint64_t lba=strtoull(argv[2],NULL,0);
    img_io_opts_t io={0}; io.writable=true;
    img_io *fh=img_io_open_path(argv[1],&io);
    if(!fh){ printf("❌ 打不开镜像\n"); return 1; }
    img_qcow2_opts_t qo={0}; g_q=img_qcow2_open(fh,&qo);
    if(!g_q || img_qcow2_write_init(g_q)!=IMG_OK){ printf("❌ qcow2\n"); return 1; }
    g_lba=lba; g_src=argv[3];

    /* 先清场：同一张盘重复跑时，上一轮留下的文件会让"不留半成品"的断言假失败 */
    { img_fat32_fs *fs=img_fat32_mount(g_q,lba,true);
      if(fs){ img_fat32_rm(fs,"/EFI/cancelled.bin"); img_fat32_unmount(fs); } }

    printf("════ ① 写入中途取消 ════\n");
    img_cancel_reset();
    uint64_t base=free_now();
    pthread_t t; g_dst="/EFI/cancelled.bin";
    pthread_create(&t,NULL,th_put,NULL);
    usleep(150*1000);                 /* 等它进入分块循环 */
    img_cancel();                     /* ← 用户点"取消" */
    pthread_join(t,NULL);
    printf("     写入返回 rc=%d (%s)\n", g_rc, img_strerror(g_rc));
    chk("取消后返回 IMG_ECANCELLED", g_rc==IMG_ECANCELLED);
    chk("目标文件不存在（不留半成品文件）", !exists("/EFI/cancelled.bin"));
    chk("剩余空间回到基线（不漏簇）", free_now()==base);

    printf("════ ② 取消标志不污染下一个操作 ════\n");
    img_cancel_reset();
    pthread_create(&t,NULL,th_put,NULL);       /* g_dst 已是 after_cancel */
    pthread_join(t,NULL);
    printf("     写入返回 rc=%d (%s)\n", g_rc, img_strerror(g_rc));
    chk("下一个操作正常成功", g_rc==IMG_OK);
    chk("确实写入了（大小=源文件）", size_of("/EFI/cancelled.bin")==(uint64_t)st_src.st_size);

    printf("════ ③ 覆盖写时取消 → 旧文件要完好（先写新、后释放旧）════\n");
    img_cancel_reset();
    pthread_create(&t,NULL,th_put,NULL);       /* 覆盖同一路径 */
    usleep(80*1000); img_cancel();
    pthread_join(t,NULL);
    printf("     覆盖写返回 rc=%d (%s)\n", g_rc, img_strerror(g_rc));
    if(g_rc==IMG_ECANCELLED){
        chk("覆盖写被取消", 1);
        chk("旧文件仍在且大小未变（目录项没悬空）",
            size_of("/EFI/cancelled.bin")==(uint64_t)st_src.st_size);
    } else if(g_rc==IMG_OK){
        printf("  ⚠ 源文件太小，80ms 内就写完了 —— 这条没测到（换更大的源文件可测）\n");
    } else {
        chk("覆盖写返回值合理", 0);
    }

    printf("\n════ 结果：%s（失败 %d）════\n", fails?"❌ 有问题":"✅ 取消契约通过", fails);
    img_qcow2_close(g_q); img_io_close(fh);
    return fails?1:0;
}
