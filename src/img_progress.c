#define _POSIX_C_SOURCE 200809L
#include "img_progress.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct img_prog {
    img_progress_cb cb;
    void *ud;
    int throttle_ms;
    long long last_ms;          /* 上次真正上报的时刻 */
    long long start_ms;
    img_phase_t last_phase;
    img_op_t    last_op;
    char item_buf[512];
    char msg_buf[160];
    volatile int cancelled;
    uint64_t ov_cur, ov_total;
    int      ov_pct;
    char     kind_buf[16];
    uint64_t arc_cur, arc_total;
    /* ★ 2026-11-10（释放进度偏差）：速率与 ETA 只按【当前阶段】算。
     *   以前 elapsed 从进程启动算、速率用 cur/elapsed，单位还可能是"条目数"，
     *   于是一边灌一边跳（wim.apply 剩 200 MB 时报"剩 0 秒"）。*/
    long long phase_ms;
    uint64_t  phase_base;       /* 本阶段起点的 work 值（字节优先）*/
    struct { long long t; uint64_t v; } smp[16];
    int  smp_n;
    int  last_pct;
    int  eta_s;
    img_progress_t cur;
};

/* 进度的"工作量"单位：有字节口径就用字节，否则用条目数。
 * ★ 两套数不能混算 —— 混了就是"63 个流里走了 62 个"≈"剩 0 秒"。*/
static uint64_t work_cur(const img_progress_t *c){
    return c->total_bytes ? c->cur_bytes : c->cur;
}
static uint64_t work_total(const img_progress_t *c){
    return c->total_bytes ? c->total_bytes : c->total;
}

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static void strcopy(char *d,size_t cap,const char *s){
    if(!d||!cap) return;
    if(!s){ d[0]=0; return; }
    snprintf(d,cap,"%s",s);
}

img_prog *img_prog_new(img_progress_cb cb, void *ud, int throttle_ms){
    struct img_prog *p = calloc(1,sizeof *p);
    if(!p) return NULL;
    p->cb = cb; p->ud = ud;
    p->throttle_ms = throttle_ms>0 ? throttle_ms : 200;
    p->last_ms = 0; p->start_ms = now_ms();
    p->last_phase = IMG_PH_NONE; p->last_op = IMG_OP_NONE;
    p->cur.pct = -1; p->cur.eta_s = -1; p->cur.elapsed_s = 0;
    p->cur.overall_pct = -1; p->ov_pct = -1;
    p->last_pct = -1; p->eta_s = -1; p->smp_n = 0;
    p->phase_ms = p->start_ms;
    snprintf(p->kind_buf,sizeof p->kind_buf,"%s","raw");
    p->cur.bytes_kind=p->kind_buf;
    return p;
}
void img_prog_free(img_prog *p){ free(p); }

static void do_emit(img_prog *p, int force){
    if(!p || !p->cb) return;
    long long t = now_ms();
    int changed = (p->cur.phase != p->last_phase) || (p->cur.op != p->last_op);
    if(!force && !changed && (t - p->last_ms) < p->throttle_ms) return;

    p->last_ms = t;
    p->last_phase = p->cur.phase;
    p->last_op    = p->cur.op;
    p->cur.elapsed_s = (int)((t - p->start_ms)/1000);

    /* 采样：窗口速率（用来判断"这一会儿有没有在动"）*/
    {
        if(p->smp_n == (int)(sizeof p->smp/sizeof p->smp[0])){
            memmove(p->smp, p->smp+1, sizeof p->smp[0] * (size_t)(p->smp_n-1));
            p->smp_n--;
        }
        p->smp[p->smp_n].t = t;
        p->smp[p->smp_n].v = work_cur(&p->cur);
        p->smp_n++;
        while(p->smp_n > 2 && t - p->smp[0].t > 30000){
            memmove(p->smp, p->smp+1, sizeof p->smp[0] * (size_t)(p->smp_n-1));
            p->smp_n--;
        }
    }

    /* eta：只用【当前阶段】的速率，单位与 pct 同一套（字节优先）。
     *   窗口速率（≥1.5 s 才有值）+ 阶段平均（≥6 s 才有值）各占一半：
     *   前者跟得上速度变化，后者吃掉 LZMS 解压那种几十秒的静默。*/
    {
        uint64_t wc = work_cur(&p->cur), wt = work_total(&p->cur);
        int eta = -1;
        if(wt > 0 && wc >= wt) eta = 0;
        else if(wt > 0){
            double rate = 0;
            if(p->smp_n >= 2){
                long long span = t - p->smp[0].t;
                uint64_t v0 = p->smp[0].v, v1 = p->smp[p->smp_n-1].v;
                if(span >= 1500 && v1 > v0) rate = (double)(v1 - v0) / ((double)span/1000.0);
            }
            long long pel = t - p->phase_ms;
            if(pel >= 6000){
                uint64_t base = (p->phase_base < wc) ? p->phase_base : wc;
                double cum = (double)(wc - base) / ((double)pel/1000.0);
                if(cum > 0) rate = (rate > 0) ? (rate + cum) / 2.0 : cum;
            }
            if(rate > 0){
                double e = (double)(wt - wc) / rate;
                if(e > 172800.0) e = 172800.0;
                eta = (int)e;
                if(eta < 1) eta = 1;
                if(p->eta_s > 0) eta = (int)(p->eta_s*0.6 + eta*0.4);   /* 平滑，不来回跳 */
            } else {
                eta = p->eta_s;          /* 卡住时保持上一次，别闪成"未知" */
            }
        }
        p->eta_s = eta;
        p->cur.eta_s = eta;
    }

    /* 百分比：有字节口径就按字节，没有才按条目数（两套绝不能混）。
     * ★ REQ-68 §4：两套都拿不到（total 还是 0）→ 保持 -1（"不确定"），
     *   不再硬给 0 —— App 的圆环要能把"不知道"和"0%"分开画。*/
    {
        int pct = -1;
        if(p->cur.total_bytes > 0)
            pct = (int)((double)p->cur.cur_bytes * 100.0 / (double)p->cur.total_bytes);
        else if(p->cur.total > 0)
            pct = (int)((double)p->cur.cur * 100.0 / (double)p->cur.total);
        if(pct > 100) pct = 100;
        if(pct >= 0 && pct < p->last_pct) pct = p->last_pct;   /* 阶段内不倒退 */
        if(pct >= 0) p->last_pct = pct;
        p->cur.pct = pct;
    }
    /* 全局进度（单调）*/
    {
        int opc = -1;
        if(p->ov_total > 0){
            int pc = (int)((double)p->ov_cur * 100.0 / (double)p->ov_total);
            if(pc > 100) pc = 100;
            if(pc < 0) pc = 0;
            if(pc < p->ov_pct) pc = p->ov_pct;      /* 绝不倒退 */
            p->ov_pct = pc;
            opc = pc;
        }
        p->cur.overall_pct = opc;
        p->cur.overall_cur_bytes = p->ov_cur;
        p->cur.overall_total_bytes = p->ov_total;
    }
    p->cur.bytes_kind = p->kind_buf;
    p->cur.archive_cur_bytes = p->arc_cur;
    p->cur.archive_total_bytes = p->arc_total;
    p->cur.item = p->item_buf;
    p->cur.msg  = p->msg_buf;
    img_io_touch();               /* 有进度 = 没无响应（长循环里可能很久不写盘）*/
    p->cb(p->ud, &p->cur);
}

void img_prog_bytes_kind(img_prog *p, const char *kind,
                         uint64_t archive_cur, uint64_t archive_total){
    if(!p) return;
    if(kind) snprintf(p->kind_buf,sizeof p->kind_buf,"%s",kind);
    p->arc_cur=archive_cur;
    p->arc_total=archive_total;
}
void img_prog_overall(img_prog *p, uint64_t cur, uint64_t total){
    if(!p) return;
    if(total) p->ov_total = total;
    if(cur > p->ov_cur) p->ov_cur = cur;              /* 单调：小值忽略 */
    else if(p->ov_total && p->ov_cur > p->ov_total) p->ov_cur = p->ov_total;
    if(p->ov_total && p->ov_cur > p->ov_total) p->ov_cur = p->ov_total;
    do_emit(p, 0);
}
void img_prog_phase(img_prog *p, img_phase_t ph, const char *msg){
    if(!p) return;
    if(p->cur.phase != ph){
        /* ★ 换阶段 = 换一套数值。留着上一阶段的 3/3 会让新阶段第一帧就报 100%，
         *   App 那边看到的是"进度条瞬间冲到 95% 又掉回来"。*/
        p->cur.phase = ph;
        p->cur.cur = 0; p->cur.total = 0;
        p->cur.cur_bytes = 0; p->cur.total_bytes = 0;
        p->cur.pct = -1; p->cur.eta_s = -1;
        p->last_pct = -1;
        p->eta_s = -1;
        p->smp_n = 0;
        p->phase_ms = now_ms();
        p->phase_base = 0;
    }
    if(msg) strcopy(p->msg_buf, sizeof p->msg_buf, msg);
    p->cur.msg = p->msg_buf;
    do_emit(p, 1);                    /* 阶段变化立即上报 */
}
void img_prog_op(img_prog *p, img_op_t op){
    if(!p) return;
    p->cur.op = op;
    do_emit(p, 1);                    /* 操作变化立即上报 */
}
void img_prog_count(img_prog *p, uint64_t cur, uint64_t total){
    if(!p) return;
    p->cur.cur = cur;
    p->cur.total = total;
    do_emit(p,0);
}
void img_prog_bytes(img_prog *p, uint64_t a, uint64_t b){
    if(!p) return;
    p->cur.cur_bytes = a;
    p->cur.total_bytes = b;
    do_emit(p,0);
}
void img_prog_item(img_prog *p, const char *item){
    if(!p) return;
    const char *n = item ? item : "";
    if(strcmp(p->item_buf, n) != 0){
        strcopy(p->item_buf, sizeof p->item_buf, n);
        p->cur.item = p->item_buf;
        do_emit(p,1);                  /* 当前文件换了 → 立即上报 */
    }
}
void img_prog_emit(img_prog *p){
    if(p) do_emit(p,1);
}

void img_prog_done(img_prog *p, const char *msg){
    if(!p) return;
    if(p->cur.total) p->cur.cur = p->cur.total;
    if(p->cur.total_bytes) p->cur.cur_bytes = p->cur.total_bytes;
    p->cur.pct = 100; p->cur.eta_s = 0;
    p->last_pct = 100; p->eta_s = 0;
    if(msg) strcopy(p->msg_buf,sizeof p->msg_buf,msg);
    strcopy(p->item_buf,sizeof p->item_buf,"");
    /* 全局进度（单调）*/
    {
        int opc = -1;
        if(p->ov_total > 0){
            int pc = (int)((double)p->ov_cur * 100.0 / (double)p->ov_total);
            if(pc > 100) pc = 100;
            if(pc < 0) pc = 0;
            if(pc < p->ov_pct) pc = p->ov_pct;      /* 绝不倒退 */
            p->ov_pct = pc;
            opc = pc;
        }
        p->cur.overall_pct = opc;
        p->cur.overall_cur_bytes = p->ov_cur;
        p->cur.overall_total_bytes = p->ov_total;
    }
    p->cur.item = p->item_buf;
    p->cur.msg  = p->msg_buf;
    do_emit(p,1);
}
/* ★ REQ-68 §3.2：阶段收尾的那一条 100%。必须在换 phase 之前调，
 *   否则圆环会停在 99% 再跳回 0%，看起来像回退。 */
void img_prog_seal(img_prog *p, const char *msg){
    if(!p) return;
    if(p->cur.total) p->cur.cur = p->cur.total;
    if(p->cur.total_bytes) p->cur.cur_bytes = p->cur.total_bytes;
    p->cur.pct = 100; p->cur.eta_s = 0;
    p->last_pct = 100; p->eta_s = 0;
    if(msg) strcopy(p->msg_buf, sizeof p->msg_buf, msg);
    p->cur.msg = p->msg_buf;
    do_emit(p, 1);
}

void img_prog_cancel(img_prog *p){
    if(!p) return;
    p->cancelled = 1;
    img_cancel();                    /* ★ 同时置全局标志：模块的取消检查点读的是它 */
}
int img_prog_cancelled(const img_prog *p){
    return p ? p->cancelled : 0;
}

static void jesc(char *d,int cap,const char *s){
    int j=0;
    for(unsigned char c = s?*s:0; s && *s; s++, c=*s){
        if(j>=cap-6) break;
        switch(c){
        case '"':  d[j++]='\\'; d[j++]='"';  break;
        case '\\': d[j++]='\\'; d[j++]='\\'; break;
        case '\n': d[j++]='\\'; d[j++]='n';  break;
        case '\r': d[j++]='\\'; d[j++]='r';  break;
        case '\t': d[j++]='\\'; d[j++]='t';  break;
        default:
            if(c<0x20){ j+=snprintf(d+j,cap-j,"\\u%04x",c); }
            else d[j++]=c;
        }
    }
    d[j]=0;
}

int img_prog_json(const img_progress_t *pr, char *buf, int cap){
    if(!pr||!buf||cap<8) return 0;
    char it[640]={0}, mg[320]={0};
    jesc(it,sizeof it, pr->item?pr->item:"");
    jesc(mg,sizeof mg, pr->msg ?pr->msg :"");
    return snprintf(buf,cap,
        "{\"ev\":\"progress\",\"phase\":\"%s\",\"op\":\"%s\","
        "\"cur\":%llu,\"total\":%llu,"
        "\"curBytes\":%llu,\"totalBytes\":%llu,"
        "\"pct\":%d,\"eta\":%d,\"elapsed\":%d,"
        "\"overall_pct\":%d,\"overall_cur_bytes\":%llu,\"overall_total_bytes\":%llu,"
        "\"bytes_kind\":\"%s\",\"archive_cur_bytes\":%llu,\"archive_total_bytes\":%llu,"
        "\"item\":\"%s\",\"msg\":\"%s\"}",
        img_phase_name(pr->phase), img_op_name(pr->op),
        (unsigned long long)pr->cur,(unsigned long long)pr->total,
        (unsigned long long)pr->cur_bytes,(unsigned long long)pr->total_bytes,
        pr->pct, pr->eta_s, pr->elapsed_s,
        pr->overall_pct, (unsigned long long)pr->overall_cur_bytes,
        (unsigned long long)pr->overall_total_bytes,
        pr->bytes_kind?pr->bytes_kind:"raw",
        (unsigned long long)pr->archive_cur_bytes, (unsigned long long)pr->archive_total_bytes,
        it, mg);
}

void img_progress_to_stdout(void *ud, const img_progress_t *p){
    (void)ud;
    char b[2048];
    if(img_prog_json(p, b, sizeof b) > 0){
        fputs(b, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}
