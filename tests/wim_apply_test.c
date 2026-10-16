/* wim_apply_test — WIM/ESD 直灌的两条路径验证（全流程最慢的一步，必须实测）
 *
 *   ① 正常跑完：rc=IMG_OK，日志有 begin + done(带 duration_ms)
 *   ② 中途取消：另一个线程 img_cancel() → rc=IMG_ECANCELLED，
 *      操作**迅速**停下（不挂死），日志有 begin + fail(E 级, 带 error_code)
 *      说明：WIM 直灌走 ntfs_device 回调，取消是靠回调返回 ECANCELED 穿透 wimlib 的，
 *            **无法回滚**（目标分区会停在半写状态 → 卡片会写"该分区需重做"）。
 *            本测试只保证：能停下、不崩、镜像文件本身仍完好（qemu-img check 由外面脚本跑）。
 *
 * 用法: wim_apply_test <qcow2> <part_lba> <esd> <index> [cancel_after_ms]
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE   /* usleep */
#endif
#define _POSIX_C_SOURCE 200809L
#include "img_cmd.h"
#include "img_event.h"
#include "img_io.h"
#include "img_qcow2.h"
#include "img_wim_apply.h"
#include "img_progress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

static const char *g_img, *g_esd;
static uint64_t g_lba;
static int g_idx, g_rc = 999;
static img_qcow2 *g_q;
static img_prog *g_prog;

static void on_prog(void *ud, const img_progress_t *p){
    (void)ud;
    /* 只演示：真实 CLI/App 会拿这个刷进度条 */
    if(p && p->msg) fprintf(stderr,"  [prog] %s\n", p->msg);
}

static void *th_apply(void *a){
    (void)a;
    img_wim_apply_opts_t o; memset(&o,0,sizeof o);
    g_rc = img_wim_apply(g_esd, g_idx, g_q, g_lba, &o, g_prog);
    return NULL;
}

int main(int argc, char **argv){
    if(argc<5){ fprintf(stderr,"用法: %s <qcow2> <part_lba> <esd> <index> [cancel_after_ms]\n",argv[0]); return 2; }
    setvbuf(stdout,NULL,_IONBF,0);
    img_log_cli(argc, argv, "wim_apply_test");   /* 支持 IMGTOOL_LOG / --log= */
    g_img=argv[1]; g_lba=strtoull(argv[2],NULL,0); g_esd=argv[3]; g_idx=atoi(argv[4]);
    int cancel_ms = (argc>5)? atoi(argv[5]) : 0;

    img_io_opts_t io={0}; io.writable=true;
    img_io *fh=img_io_open_path(g_img,&io);
    if(!fh){ printf("❌ 打不开镜像\n"); return 1; }
    img_qcow2_opts_t qo={0}; g_q=img_qcow2_open(fh,&qo);
    if(!g_q || img_qcow2_write_init(g_q)!=IMG_OK){ printf("❌ qcow2\n"); return 1; }
    img_set_image(g_img);
    g_prog = img_prog_new(on_prog,NULL,200);
    img_cancel_reset();

    pthread_t t;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    pthread_create(&t,NULL,th_apply,NULL);
    if(cancel_ms>0){
        usleep((useconds_t)cancel_ms*1000);
        printf("  → 发出取消（已跑 %d ms）\n",cancel_ms);
        img_cancel();
    }
    pthread_join(t,NULL);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    long ms=(t1.tv_sec-t0.tv_sec)*1000+(t1.tv_nsec-t0.tv_nsec)/1000000;

    printf("  结果: rc=%d (%s)  用时 %ld ms\n", g_rc, img_strerror(g_rc), ms);
    if(cancel_ms>0){
        printf("  %s\n", g_rc==IMG_ECANCELLED ? "✅ 取消后返回 IMG_ECANCELLED" : "❌ 取消后返回码不对");
        if(ms > cancel_ms + 3000) printf("  ❌ 停下太慢（%ld ms > %d+3000）\n", ms, cancel_ms);
        else printf("  ✅ 迅速停下（%ld ms）\n", ms);
    } else {
        printf("  %s\n", g_rc==IMG_OK ? "✅ 正常跑完" : "❌ 未正常完成");
    }
    img_prog_free(g_prog);
    img_qcow2_close(g_q); img_io_close(fh);
    return (cancel_ms>0 ? (g_rc==IMG_ECANCELLED?0:1) : (g_rc==IMG_OK?0:1));
}
