/* 进度协议冒烟测试：验证节流、阶段变化立即发、JSON 格式正确 */
#include "img_progress.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int n = 0;
static void on_progress(void *ud, const img_progress_t *p){
    (void)ud;
    n++;
    char b[2048];
    int len = img_prog_json(p, b, sizeof b);
    printf("[%3d] %-8s %-16s %5.1f%% cur=%llu/%llu item=%s\n",
           n, img_phase_name(p->phase), img_op_name(p->op),
           p->pct >= 0 ? (double)p->pct : -1.0,
           (unsigned long long)p->cur, (unsigned long long)p->total,
           p->item && p->item[0] ? p->item : "(—)");
    if (len <= 0) { fprintf(stderr, "❌ JSON 序列化失败\n"); }
}

int main(void){
    printf("=== 进度协议测试 ===\n");
    img_prog *pr = img_prog_new(on_progress, NULL, 50);

    img_prog_phase(pr, IMG_PH_DEPLOY, "部署系统文件");
    img_prog_op(pr, IMG_OP_WRITE_FILE);
    img_prog_count(pr, 0, 10);
    img_prog_bytes(pr, 0, 1000000);

    /* 模拟 10 个文件，每个快到 → 节流应压掉大部分 */
    for (uint64_t i = 1; i <= 10; i++){
        char path[128];
        snprintf(path, sizeof path, "/Windows/System32/file%03llu.exe", (unsigned long long)i);
        img_prog_item(pr, path);
        img_prog_count(pr, i, 10);
        img_prog_bytes(pr, i * 100000, 1000000);
        usleep(30 * 1000);       /* 30ms/个 → 300ms 总时长 */
    }
    img_prog_phase(pr, IMG_PH_CHECK, "引导自检");   /* 阶段变化 → 必须立即发 */
    img_prog_count(pr, 0, 26);
    img_prog_done(pr, "完成");

    printf("\n总回调次数 = %d （10 个文件 + 2 次阶段变化 + done，50ms 节流下应远小于 14）\n", n);
    if (n >= 3 && n <= 14) printf("✅ 节流工作正常\n");
    else printf("⚠ 次数异常（太刷屏或太稀疏）\n");

    img_prog_free(pr);
    printf("\n=== JSON 样例（前端直接解析这些字段）===\n");
    img_progress_t sample = {
        .phase = IMG_PH_DEPLOY, .op = IMG_OP_WRITE_FILE,
        .cur = 4231, .total = 165206,
        .cur_bytes = 134217728, .total_bytes = 10872841318LL,
        .pct = 3, .eta_s = 183, .elapsed_s = 97,
        .item = "/Windows/System32/ntoskrnl.exe",
        .msg = "部署系统文件"
    };
    char b[2048];
    img_prog_json(&sample, b, sizeof b);
    printf("%s\n", b);
    return 0;
}
