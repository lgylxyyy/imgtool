/* rmbench.c —— 量一下"取消后清理临时目录"到底多贵（REQ-69 ④ 的嫌疑点） */
#include "img_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static double ms(const struct timespec *a, const struct timespec *b){
    return (double)(b->tv_sec - a->tv_sec)*1000.0 + (double)(b->tv_nsec - a->tv_nsec)/1e6;
}

int main(int argc, char **argv){
    int n = argc > 1 ? atoi(argv[1]) : 33000;
    const char *dir = "/tmp/rmbench-t";
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir); system(cmd);
    mkdir(dir, 0755);
    char p[512];
    snprintf(p, sizeof p, "%s/sub", dir); mkdir(p, 0755);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for(int i = 0; i < n; i++){
        snprintf(p, sizeof p, "%s/sub/f%06d.dat", dir, i);
        FILE *f = fopen(p, "wb");
        if(!f) { printf("创建失败 i=%d\n", i); return 1; }
        fwrite("0123456789abcdef", 1, 16, f);
        fclose(f);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("建 %d 个文件：%.0f ms\n", n, ms(&a,&b));

    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = img_tmpdir_remove(dir);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("img_tmpdir_remove 删 %d 个文件：%.0f ms（rc=%d）\n", n, ms(&a,&b), rc);

    /* 对照：rm -rf 同一批 */
    mkdir(dir, 0755);
    snprintf(p, sizeof p, "%s/sub", dir); mkdir(p, 0755);
    for(int i = 0; i < n; i++){
        snprintf(p, sizeof p, "%s/sub/f%06d.dat", dir, i);
        FILE *f = fopen(p, "wb"); fwrite("0123456789abcdef", 1, 16, f); fclose(f);
    }
    char rc2[256]; snprintf(rc2, sizeof rc2, "rm -rf %s", dir);
    clock_gettime(CLOCK_MONOTONIC, &a);
    system(rc2);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("rm -rf 对照：%.0f ms\n", ms(&a,&b));
    return 0;
}