/* progress_reuse_test — REQ-79：进程级进度对象的「命令间残留」
 *
 * 背景：App 的 JNI 层只在进程启动时 img_cmd_set_progress() 一次，之后所有命令
 * 复用同一个 img_prog。以前每条命令开始时不清状态，于是（真机实测）：
 *   ① 第二条命令的 elapsed 从上一条的时间接着算（「一打开就 20:23」）；
 *   ② 上一条 xcopy 的 files_cur/files_total 留在下一条命令里（compact 显示 1/1）；
 *   ③ dst_item 上一条命令的目标留在下一条里。
 *
 * CLI 每条命令是独立进程，测不到这个 bug —— 本测试在【同一进程】里连跑两条命令
 * （中间 sleep 3 秒），断言第二条：elapsed 从 0 起、files 归零、dst 为空。
 *
 * 用法: progress_reuse_test <工作目录>
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "img_cmd.h"
#include "img_progress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int fails = 0;
static void chk(const char *w, int ok_, const char *extra){
    printf("  %s %s%s%s\n", ok_ ? "✅" : "❌", w,
           (extra && extra[0]) ? "  " : "", (extra && extra[0]) ? extra : "");
    if(!ok_) fails++;
}

static int g_stage;                 /* 1 = 第一条命令；2 = 第二条命令 */
static img_progress_t ev1_last, ev2_first, ev2_last;
static int ev1_n, ev2_n;
static char ev2_dst[640];

static void on_prog(void *ud, const img_progress_t *pr){
    (void)ud;
    if(g_stage == 1){
        ev1_last = *pr;
        ev1_n++;
    } else {
        if(ev2_n == 0){
            ev2_first = *pr;
            snprintf(ev2_dst, sizeof ev2_dst, "%s", pr->dst_item ? pr->dst_item : "");
        }
        ev2_last = *pr;
        ev2_n++;
    }
}

static int run_cmd3(const char *name,
                    const char *k1, const char *v1,
                    const char *k2, const char *v2,
                    const char *k3, const char *v3){
    img_args *a = img_args_new();
    if(!a) return -1;
    if(k1 && img_args_set(a, k1, v1) != IMG_OK) { img_args_free(a); return -1; }
    if(k2 && img_args_set(a, k2, v2) != IMG_OK) { img_args_free(a); return -1; }
    if(k3 && img_args_set(a, k3, v3) != IMG_OK) { img_args_free(a); return -1; }
    img_result r; memset(&r, 0, sizeof r);
    int rc = img_cmd_exec(name, NULL, a, &r);
    img_args_free(a);
    return rc;
}

int main(int argc, char **argv){
    if(argc < 2){ fprintf(stderr, "用法: %s <工作目录>\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *wd = argv[1];
    mkdir(wd, 0755);

    char qc[1200], src[1200], dst[1200], hsrc[1300], hdst[1300];
    snprintf(qc,  sizeof qc,  "%s/reuse.qcow2", wd);
    snprintf(src, sizeof src, "%s/src", wd);
    snprintf(dst, sizeof dst, "%s/dst", wd);
    snprintf(hsrc,sizeof hsrc,"host:%s", src);
    snprintf(hdst,sizeof hdst,"host:%s", dst);
    mkdir(src, 0755);
    mkdir(dst, 0755);

    /* 源树里放一个文件（copy 才有东西可报） */
    char f[1300];
    snprintf(f, sizeof f, "%s/a.txt", src);
    FILE *fp = fopen(f, "wb");
    if(!fp){ printf("❌ 建不出源文件\n"); return 1; }
    fputs("hello\n", fp); fclose(fp);

    /* 1GB 空 qcow2（sparsify 有进度、但没有文件概念） */
    int rc = run_cmd3("img.create", "out", qc, "size-gb", "1", "format", "qcow2");
    chk("img.create 成功", rc == IMG_OK, "");

    img_cmd_set_progress(on_prog, &g_stage);

    printf("════ 第一条：copy（会给 files/dst 留下痕迹）════\n");
    g_stage = 1;
    rc = run_cmd3("copy", "src", hsrc, "dst", hdst, NULL, NULL);
    chk("copy 成功", rc == IMG_OK, "");
    chk("copy 有进度事件", ev1_n > 0, "");
    chk("copy 的 files_total > 0（痕迹确实留下了）", ev1_last.files_total > 0, "");

    /* ★ 关键：命令之间隔 3 秒。elapsed 若不清零，第二条会带着这 3 秒。 */
    printf("════ 中间等 3 秒（给「累计」留出可观测窗口）════\n");
    sleep(3);

    printf("════ 第二条：img.sparsify（应完全没有上一条的残留）════\n");
    /* 带映像的命令要 img_ctx（CLI 的 --img 走 ctx，不是普通参数） */
    char cerr[256] = "";
    img_ctx *ctx = img_ctx_open(qc, true, false, cerr, sizeof cerr);
    if(!ctx){ printf("❌ 打不开 ctx：%s\n", cerr); return 1; }
    g_stage = 2;
    img_args *a2 = img_args_new();
    img_args_set(a2, "dry-run", "true");
    img_result r2; memset(&r2, 0, sizeof r2);
    rc = img_cmd_exec("img.sparsify", ctx, a2, &r2);
    if(rc != IMG_OK) printf("     [debug] rc=%d code=%s why=%s\n", rc, r2.code, r2.why);
    img_args_free(a2);
    img_ctx_close(ctx);
    chk("sparsify 成功", rc == IMG_OK, "");
    chk("sparsify 有进度事件", ev2_n > 0, "");

    char why[128];
    snprintf(why, sizeof why, "elapsed_s=%d（累计则 ≥3）", ev2_first.elapsed_s);
    chk("① elapsed 从 0 起，不累计", ev2_first.elapsed_s == 0, why);

    snprintf(why, sizeof why, "files=%llu/%llu", (unsigned long long)ev2_first.files_cur,
             (unsigned long long)ev2_first.files_total);
    chk("② files_cur/files_total 不残留", ev2_first.files_cur == 0 && ev2_first.files_total == 0, why);

    chk("③ dst_item 不残留（上一条是 host 目标）", ev2_dst[0] == '\0', "");

    printf("\n════ 通过 %d · 失败 %d ════\n", 9 - fails, fails);
    return fails ? 1 : 0;
}