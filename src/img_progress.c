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
    img_progress_t cur;
};

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

    /* eta: 用已处理比例线性外推 */
    p->cur.eta_s = -1;
    if(p->cur.total && p->cur.cur && p->cur.cur < p->cur.total && p->cur.elapsed_s > 0){
        double rate = (double)p->cur.cur / (double)p->cur.elapsed_s;
        if(rate > 0) p->cur.eta_s = (int)((double)(p->cur.total - p->cur.cur)/rate);
    }
    p->cur.pct = -1;
    if(p->cur.total && p->cur.total <= 100){
        int pc = (int)(p->cur.cur * 100 / p->cur.total);
        p->cur.pct = pc>100?100:(pc<0?0:pc);
    } else if(p->cur.total_bytes && p->cur.total){
        int pc = (int)((double)p->cur.cur / (double)p->cur.total * 100.0);
        p->cur.pct = pc>100?100:(pc<0?0:pc);
    }
    p->cur.item = p->item_buf;
    p->cur.msg  = p->msg_buf;
    img_io_touch();               /* 有进度 = 没卡死（长循环里可能很久不写盘）*/
    p->cb(p->ud, &p->cur);
}

void img_prog_phase(img_prog *p, img_phase_t ph, const char *msg){
    if(!p) return;
    p->cur.phase = ph;
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
    if(msg) strcopy(p->msg_buf,sizeof p->msg_buf,msg);
    strcopy(p->item_buf,sizeof p->item_buf,"");
    p->cur.item = p->item_buf;
    p->cur.msg  = p->msg_buf;
    do_emit(p,1);
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
        "\"item\":\"%s\",\"msg\":\"%s\"}",
        img_phase_name(pr->phase), img_op_name(pr->op),
        (unsigned long long)pr->cur,(unsigned long long)pr->total,
        (unsigned long long)pr->cur_bytes,(unsigned long long)pr->total_bytes,
        pr->pct, pr->eta_s, pr->elapsed_s, it, mg);
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
