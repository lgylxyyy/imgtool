/* progress_test.c — 进度/ETA 口径回归（REQ-67 + 释放进度偏差）
 *
 * 抓的是三个曾经真实存在的错：
 *   ① 换阶段不清数：格式化留下的 3/3 被 extract 拿去用，第一帧就 pct=100
 *      （App 看到"进度条瞬间冲到 95% 又掉回来"）；
 *   ② pct 用条目数算而不用字节：63 个流走完 62 个 → 98%，可实际还剩 30%；
 *   ③ eta 用 cur/elapsed（单位可能是"流"、elapsed 还是整条命令）→ 剩 200 MB 报"剩 0 秒"。
 */
#include "img_progress.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static img_progress_t g_last;
static int g_events = 0;
static void on_prog(void *ud, const img_progress_t *p){
    (void)ud;
    if(p){ g_last = *p; g_events++; }
}

static int pass = 0, fail = 0;
static void ok(int cond, const char *what){
    if(cond){ printf("  ✅ %s\n", what); pass++; }
    else    { printf("  ❌ %s\n", what); fail++; }
}

int main(void){
    printf("════ img_progress 口径 ════\n");
    img_prog *p = img_prog_new(on_prog, NULL, 30);
    if(!p){ printf("  ❌ img_prog_new 失败\n"); return 1; }

    /* ① 阶段切换必须清零（模拟"格式化 3/3 → 解压"这条真实路径）*/
    img_prog_phase(p, IMG_PH_FORMAT, "格式化");
    img_prog_count(p, 3, 3);
    img_prog_emit(p);                       /* 节流 30 ms：同一毫秒内要强制推一次 */
    ok(g_last.pct == 100, "格式化阶段 3/3 报 100%");

    img_prog_phase(p, IMG_PH_EXTRACT, "解压文件数据");
    ok(g_last.cur == 0 && g_last.total == 0, "换阶段后 cur/total 归零（不再残留 3/3）");
    ok(g_last.pct == -1, "换阶段后 pct=-1（不确定态；不再第一帧就 100%，也不假装 0%）");
    ok(g_last.eta_s <= 0, "换阶段后 eta 未知（不继承上一段）");

    /* ② pct 必须按字节，不能按条目数 */
    img_prog_bytes(p, 10u*1024*1024, 100u*1024*1024);
    img_prog_count(p, 90, 100);            /* 条目数 90%，字节只有 10% */
    img_prog_emit(p);
    ok(g_last.pct == 10, "pct 按字节算（10 MB/100 MB = 10%），不被 90/100 的条目数带偏");

    /* ③ 阶段内不倒退 */
    img_prog_bytes(p, 5u*1024*1024, 100u*1024*1024);
    img_prog_emit(p);
    ok(g_last.pct == 10, "字节数回退（5 MB）时 pct 仍保持 10%（不倒退）");

    /* ④ ETA：喂 2 秒、走到一半 → 剩余时间应≈2 秒，而不是 0 秒或几小时 */
    img_prog_bytes(p, 0, 100u*1024*1024);
    usleep(2000*1000);
    img_prog_bytes(p, 50u*1024*1024, 100u*1024*1024);
    usleep(50*1000);
    img_prog_bytes(p, 50u*1024*1024, 100u*1024*1024);   /* 推一次，触发节流后上报 */
    ok(g_last.eta_s >= 1 && g_last.eta_s <= 4,
       "ETA≈剩余/速率（2 秒走一半 → 1~4 秒）");

    /* ⑤ 卡住时保持上一个 ETA，不闪成"未知"也不飙成天文数字 */
    int keep = g_last.eta_s;
    usleep(300*1000);
    img_prog_emit(p);
    ok(g_last.eta_s >= 1 && g_last.eta_s <= keep + 2, "无进展时 ETA 不跳变");

    /* ⑥ 完成 */
    img_prog_done(p, "done");
    ok(g_last.pct == 100 && g_last.eta_s == 0, "完成事件 pct=100 / eta=0");
    ok(g_events > 3, "事件确实发出去了");

    img_prog_free(p);
    printf("\n════ 结果：通过 %d · 失败 %d ════\n", pass, fail);
    return fail ? 1 : 0;
}