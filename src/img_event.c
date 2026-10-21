#define _POSIX_C_SOURCE 200809L
/* img_event.c — 事件流实现（契约见 DESIGN-OUTPUT.md）
 *
 * 为什么这么写：
 *  · 一行一次 write()、栈上缓冲、不 malloc、不 fsync
 *    → 开销可忽略；进程被杀时页缓存里的日志已落盘（断电才可能丢最后几行）
 *  · 级别不够时 img_ev_init 返回 0，调用方直接 return → 关日志/低级别 = 零开销
 *  · 日志只写【里程碑 + 错误】；过程事件进内存环形缓冲（不占磁盘，出错时随卡片交出）
 *    ★ 两边的级别是【独立】的：日志可以只记 info，但环形缓冲仍按 debug 抓过程，
 *      这样"用户只开了 info"也能在出错卡片里看到过程细节
 *  · 轮转：单文件到上限 → 改名 .1（旧的删）→ 占用恒定；轮转后补写会话头
 *  · 心跳退避：正常 2s/条，原地不动 2→4→8…→60s（卡住时反而更省，
 *    且"最后一条 alive 的时间"本身就是无响应证据）
 */
#include "img_event.h"
#include "img_types.h"
#include "img_lock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef _WIN32
#include <sys/utsname.h>
#endif

const char *img_platform(void){
#ifdef _WIN32
    return "windows";
#else
    static char b[128];
    if(!b[0]){ struct utsname u; if(uname(&u)==0) snprintf(b,sizeof b,"%s",u.sysname); else snprintf(b,sizeof b,"posix"); }
    return b;
#endif
}
const char *img_abi(void){
    static char b[32];
    if(!b[0]) snprintf(b,sizeof b,"%zu-bit",sizeof(void*)*8);
    return b;
}

#define HB_STEP_MS    2000
#define HB_MAX_MS    60000
#define RING_CAP      1000      /* 过程事件条数（≈200KB 内存，0 磁盘）*/
#define RING_MAXLEN    240
#define LINE_CAP      1024

static struct {
    int      fd;
    char     path[512];
    img_lv_t lv;
    size_t   max_bytes, written;
    int      keep;
    int      out_fd;
    bool     out_human;
    char     session[LINE_CAP];
    char     img[512];
    char     ctx[512];          /* 命令与参数一行（卡片头部）*/
    long long t0_ms;
    const char *hb_mod, *hb_op, *hb_where;
    long long hb_last_ms, hb_interval_ms;
    uint64_t  hb_iter;
} G = { .fd=-1, .out_fd=-1, .max_bytes=4u*1024*1024, .keep=1,
        .lv=IMG_LV_INFO, .hb_interval_ms=HB_STEP_MS };

static int ring_dump_into(char *buf,int cap);   /* 前置声明：失败卡片/快照要用 */
static img_text_cb g_text_cb; static void *g_text_ud;
static bool s_session_pending = false;   /* 会话行待写（首个事件时补）*/
static char *ring[RING_CAP];
static int   ring_n, ring_head;
static img_lv_t ring_lv = IMG_LV_DEBUG;      /* 环形缓冲独立级别（默认抓 debug 过程）*/

static long long now_ms(void){   /* 单调钟：只用于算"耗时/间隔"，不用于时间戳 */
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static bool log_on(img_lv_t lv){ return (G.fd>=0||G.out_fd>=0) && lv<=G.lv; }
bool img_log_enabled(img_lv_t lv){ return log_on(lv) || lv<=ring_lv; }
img_lv_t img_log_level(void){ return G.lv; }
void img_log_set_level(img_lv_t lv){ G.lv=lv; }
void img_ring_set_level(img_lv_t lv){ ring_lv=lv; }

static void write_all(int fd, const char *b, size_t n){
    if(fd<0) return;
    size_t off=0;
    while(off<n){
        ssize_t w=write(fd,b+off,n-off);
        if(w<0){ if(errno==EINTR) continue; return; }  /* 日志失败不能拖垮主流程 */
        if(w==0) return;
        off+=(size_t)w;
    }
}

/* ---------- 轮转 ---------- */
static void rotate_if_needed(void){
    if(G.fd<0 || G.path[0]==0 || G.written < G.max_bytes) return;
    char bak[560]; bak[0]=0;
    if(G.keep>0){ snprintf(bak,sizeof bak,"%s.1",G.path); unlink(bak); }
    close(G.fd);
    if(G.keep>0) rename(G.path,bak);
    G.fd = open(G.path, O_WRONLY|O_CREAT|O_APPEND, 0644);
    G.written = 0;
    if(G.fd<0) return;
    char note[LINE_CAP];
    int n=snprintf(note,sizeof note, "t=%lld l=I m=log ev=rotate kept=%s\n",
                          now_ms()-G.t0_ms, bak[0]?bak:"(discarded)");
    if(n>0){ write_all(G.fd,note,(size_t)n); G.written+=(size_t)n; }
    if(G.session[0]){                                  /* ★ 半截日志也能单独看懂 */
        write_all(G.fd,G.session,strlen(G.session));
        G.written+=strlen(G.session);
    }
}

/* ---------- 打开/关闭 ---------- */
static int open_common(img_lv_t lv,size_t max_bytes,int keep){
    G.lv=lv; G.max_bytes=max_bytes?max_bytes:4u*1024*1024; G.keep=keep;
    G.written=0; G.t0_ms=now_ms(); G.hb_last_ms=0; G.hb_interval_ms=HB_STEP_MS;
    if(G.fd<0 && G.path[0]){
        G.fd = open(G.path, O_WRONLY|O_CREAT|O_APPEND, 0644);
        if(G.fd<0) return IMG_EIO;
    }
    if(G.fd>=0){ off_t e=lseek(G.fd,0,SEEK_END); G.written = e>0?(size_t)e:0; }
    return IMG_OK;
}
int img_log_open(const char *path, img_lv_t lv, size_t max_bytes, int keep){
    if(path) snprintf(G.path,sizeof G.path,"%s",path);
    return open_common(lv,max_bytes,keep);
}
int img_log_open_fd(int fd, img_lv_t lv, size_t max_bytes, int keep){
    G.fd=fd; G.path[0]=0;          /* fd 模式不能改名轮转（App 自己管，见契约）*/
    return open_common(lv,max_bytes,keep);
}
const char *img_log_path(void){
    return (G.fd>=0 && G.path[0]) ? G.path : NULL;
}
void img_log_close(void){
    if(G.fd>=0 && G.path[0]) close(G.fd);   /* fd 模式不替调用方关 fd */
    G.fd=-1; G.out_fd=-1;
}
void img_out_open_fd(int fd, bool human){ G.out_fd=fd; G.out_human=human; }
void img_out_set_text(img_text_cb cb, void *ud){ g_text_cb=cb; g_text_ud=ud; }

/* 记录当前镜像：失败快照 / CLI 的 .error.txt 要用（即使没开日志也要有） */
void img_set_image(const char *path){
    if(path) snprintf(G.img,sizeof G.img,"%s",path);
}
void img_card_set_context(const char *kv_line){
    if(kv_line) snprintf(G.ctx,sizeof G.ctx,"%s",kv_line);
}
/* 第一次打点前把会话行补上（进程内只补一次）*/
static void session_flush(void){
    if(!s_session_pending) return;
    s_session_pending = false;
    if(G.fd<0 || !G.session[0]) return;
    write_all(G.fd,G.session,strlen(G.session));
    G.written += strlen(G.session);
}

/* ---------- 输出（日志 + 屏幕各一份） ---------- */
/* force=true：无视 --log-level 一律写（ERROR 本来就总是过；cancel 需要它）*/
static void pump(const char *line,size_t n,img_lv_t lv,bool force){
    if(force || log_on(lv)){
        img_lock_t *lk=img_global_lock();   /* ★ 并发写日志会交错成半行/坏 JSON */
        img_lock_acquire(lk);
        write_all(G.fd,line,n);
        if(G.fd>=0){ G.written+=n; rotate_if_needed(); }
        /* 屏幕：json 模式原样吐 JSON；human 模式由 human_emit() 渲染人话行
         * （这里只负责日志文件，避免两边重复渲染）*/
        if(G.out_fd>=0 && !G.out_human) write_all(G.out_fd,line,n);
        img_lock_release(lk);
    }
}

/* ---------- I/O 看门狗（无进展超时；与取消是一套：看门狗→取消→I/O fail-fast） ---------- */
#include <pthread.h>
#include <stdatomic.h>
static _Atomic long long g_last_io_ms = 0;    /* 最近一次"读写完成"的时刻 */
static volatile int  g_io_timeout_ms = 0;     /* 0=关闭 */
static volatile int  g_io_timed_out  = 0;     /* 已判超时（I/O 入口据此 fail-fast）*/
static volatile int  g_wd_running    = 0;
static pthread_t     g_wd_thread;

void img_io_touch(void){ atomic_store_explicit(&g_last_io_ms, now_ms(), memory_order_relaxed); }
bool img_io_timed_out(void){ return g_io_timed_out!=0; }

static void *wd_main(void *ud){
    (void)ud;
    while(g_wd_running){
        int tmo=g_io_timeout_ms;
        if(tmo<=0 || g_io_timed_out){ struct timespec ts={0,200*1000*1000}; nanosleep(&ts,NULL); continue; }
        struct timespec ts={0, (long)((tmo<2000?tmo/4:500)*1000*1000)};
        nanosleep(&ts,NULL);
        long long last=atomic_load_explicit(&g_last_io_ms, memory_order_relaxed);
        if(last==0) continue;                       /* 还没开始过 I/O：不计时 */
        long long gap=now_ms()-last;
        if(gap >= tmo && !g_io_timed_out){
            g_io_timed_out=1;
            /* ★ E 级：任何日志级别都不丢（无响应是必须留痕的事件）*/
            img_ev e;
            if(img_ev_init(&e, IMG_LV_ERROR, "io", "fail", IMG_ETIMEDOUT, "I/O 无进展超时")){
                img_ev_str(&e,"error_code","IO-TIMEOUT");
                img_ev_i64(&e,"gap_ms",gap);
                img_ev_i64(&e,"timeout_ms",tmo);
                img_ev_emit(&e);
            }
            img_cancel();                            /* 协作式：各检查点退出 */
        }
    }
    return NULL;
}
void img_io_watchdog_start(int timeout_ms){
    g_io_timeout_ms = timeout_ms>0 ? timeout_ms : 0;
    g_io_timed_out  = 0;                          /* 新窗口 = 新状态（否则 once stuck, stuck forever）*/
    if(g_io_timeout_ms) img_io_touch();             /* 从"现在"开始计时 */
    if(!g_wd_running){
        g_wd_running=1;
        if(pthread_create(&g_wd_thread,NULL,wd_main,NULL)!=0) g_wd_running=0;
    }
}
void img_io_watchdog_stop(void){
    g_io_timed_out = 0;                            /* 停止即复位（测试/重跑要干净状态）*/
    if(!g_wd_running) return;
    g_wd_running=0;
    pthread_join(g_wd_thread,NULL);
    g_io_timeout_ms=0;
}

/* ---------- 取消标志 ---------- */
static volatile int g_cancel = 0;
void img_cancel(void){ g_cancel = 1; }
void img_cancel_reset(void){ g_cancel = 0; }
bool img_cancelled(void){ return g_cancel!=0; }

/* ============================================================
 * 语言与文案（★ 库只发码，语言由调用方决定 —— 多语言支持的地基）
 *
 *   事件里已经带了语义码：event=begin/done/fail、action=format/put、
 *   error_code=FAT-ENOSPC + 全部数值。App 拿这些就能渲染成自己的语言。
 *   下面这张表只给两个"兜底消费方"用：
 *     ① CLI（--lang=en|zh，默认 zh）
 *     ② 库自己的人话行（屏幕消费者）
 *   App 想完全自己 i18n 时，用 img_error_info() 拿结构化字段即可，不必查表。
 * ============================================================ */

/* ---------- 上一次失败的结构化快照（App 渲染本地化卡片用） ---------- */
static img_error_info_t g_last_err;
static int g_have_err = 0;
void img_error_record(const char *cmd, const char *code, int rc, const char *at,
                      const char *what, const char *status);
void img_error_record(const char *cmd, const char *code, int rc, const char *at,
                      const char *what, const char *status){
    memset(&g_last_err,0,sizeof g_last_err);
    snprintf(g_last_err.cmd,sizeof g_last_err.cmd,"%s",cmd?cmd:"");
    snprintf(g_last_err.code,sizeof g_last_err.code,"%s",code?code:"");
    g_last_err.rc = rc;
    if(at)    snprintf(g_last_err.at,sizeof g_last_err.at,"%s",at);
    if(what)  snprintf(g_last_err.what,sizeof g_last_err.what,"%s",what);
    if(status)snprintf(g_last_err.status,sizeof g_last_err.status,"%s",status);
    snprintf(g_last_err.img,sizeof g_last_err.img,"%.240s",G.img);
    snprintf(g_last_err.ctx,sizeof g_last_err.ctx,"%.240s",G.ctx);
    /* ★ 以前这里硬拼 "<img>.imgtool.log"：没开日志 / 用 --log=别的路径 时都是错的。
     *   现在只报【真正打开着的】那个文件；没有就留空，由卡片/App 明说"未开启"。*/
    { const char *lp=img_log_path();
      if(lp) snprintf(g_last_err.log,sizeof g_last_err.log,"%s",lp); }
    g_last_err.events_len = ring_dump_into(g_last_err.events,(int)sizeof g_last_err.events);
    g_have_err = 1;
}
void img_error_info(img_error_info_t *out){
    if(!out) return;
    memset(out,0,sizeof *out);
    if(g_have_err) *out = g_last_err;   /* 结构体整体拷贝（固定大小，无指针）*/
}

typedef struct { char *p; int cap; int n; } dumpbuf;
static int dump_cb(void *ud,const char *line){
    dumpbuf *d=(dumpbuf*)ud;
    int l=(int)strlen(line);
    if(d->n+l+1 >= d->cap) return 1;
    memcpy(d->p+d->n,line,(size_t)l); d->n+=l;
    return 0;
}
static int ring_dump_into(char *buf,int cap){
    if(!buf||cap<32) return 0;
    dumpbuf d={buf,cap-1,0};
    img_ring_dump(&d,dump_cb,0);        /* max=0 → 全部 */
    buf[d.n]=0;
    return d.n;
}


/* ---------- 过程事件环形缓冲（独立于日志级别） ---------- */
void img_ring_add(const char *line){
    if(!line) return;
    size_t n=strlen(line);
    if(n>RING_MAXLEN) n=RING_MAXLEN;
    char *cp=malloc(n+1);
    if(!cp) return;
    memcpy(cp,line,n); cp[n]=0;
    img_lock_t *lk=img_global_lock();
    img_lock_acquire(lk);
    if(ring[ring_head]) free(ring[ring_head]);
    ring[ring_head]=cp;
    ring_head=(ring_head+1)%RING_CAP;
    if(ring_n<RING_CAP) ring_n++;
    img_lock_release(lk);
}
int img_ring_count(void){ return ring_n; }
int img_ring_dump(void *ud,int (*cb)(void*,const char*),int max){
    if(!cb) return 0;
    if(max<=0||max>ring_n) max=ring_n;
    int start=(ring_head-max+RING_CAP*2)%RING_CAP, cnt=0;
    for(int i=0;i<max;i++){
        int idx=(start+i)%RING_CAP;
        if(!ring[idx]) continue;
        cnt++;
        if(cb(ud,ring[idx])) break;
    }
    return cnt;
}

/* ---------- JSON 行构造（栈上，无 malloc） ---------- */

typedef struct { char *p,*start,*end; } W;

static void jesc(W *w,const char *s){
    for(; s&&*s; s++){
        unsigned char c=(unsigned char)*s;
        const char *rep=NULL; char tmp[8];
        switch(c){
            case '"':  rep="\\\""; break;
            case '\\': rep="\\\\"; break;
            case '\n': rep="\\n";  break;
            case '\r': rep="\\r";  break;
            case '\t': rep="\\t";  break;
            default: if(c<0x20){ snprintf(tmp,sizeof tmp,"\\u%04x",c); rep=tmp; } break;
        }
        if(rep){ size_t l=strlen(rep); if(w->p+l<w->end){ memcpy(w->p,rep,l); w->p+=l; } }
        else if(w->p+1<w->end) *w->p++=(char)c;
    }
}


/* ---------- logfmt 写出（键不加引号、数字裸写、字符串仅"必要时"加引号）----------
 * 为什么不用 JSON：JSON 规定【键与字符串都必须带引号】→ 每行多 20~30 字节，
 *   而且人读着费劲（用户原话"那么多引号"）。改用 key=value：
 *     键/数字/布尔  → 永不加引号
 *     字符串        → 只有含 空格/制表/引号/= 反斜杠 或为空 时才加引号并转义
 *   例：t=1 l=I m=fat32 ev=begin rc=0 msg="格式化 FAT32 分区" op=format lba=2048
 * 仍可机器解析（logfmt 是常见格式，规则与 TOML/12-factor 日志一致）。
 * 注意：stdout 上的【结果信封】仍是 JSON（App 侧契约，标准格式不能动）。 */
static bool lf_need_quote(const char *v){
    if(!v||!*v) return true;                    /* 空串必须引，否则看不出"有没有值" */
    for(const char *p=v;*p;p++){
        unsigned char c=(unsigned char)*p;
        if(c==' '||c=='\t'||c=='"'||c=='='||c=='\\'||c<0x20) return true;
    }
    return false;
}
static void lf_sep(W *w){ if(w->p<w->end && w->p>w->start) *w->p++=' '; }
static void lf_key(W *w,const char *k){ lf_sep(w); jesc(w,k); if(w->p<w->end) *w->p++='='; }
static void lf_val_str(W *w,const char *v){
    if(lf_need_quote(v)){ if(w->p<w->end)*w->p++='"'; jesc(w,v?v:""); if(w->p<w->end)*w->p++='"'; }
    else { while(v && *v && w->p<w->end) *w->p++=*v++; }
}
static void lf_kv_str(W *w,const char *k,const char *v){ lf_key(w,k); lf_val_str(w,v?v:""); }
/* 日志字段值（不是"文案"，不随语言变）：level 用大写全词（slog/zap/logfmt 惯例）*/
static const char *lv_name(img_lv_t l){
    switch(l){ case IMG_LV_ERROR: return "ERROR"; case IMG_LV_WARN: return "WARN";
               case IMG_LV_INFO:  return "INFO";  case IMG_LV_DEBUG: return "DEBUG";
               default: return "TRACE"; }
}
/* 时间戳格式由用户定："2026-10-15 18:20:31.452"（本地时间、无 T、无 Z、空格分隔、带毫秒）
 * ★ 值里含空格 → logfmt 规范强制加引号（整行唯一"必须"带引号的字段）。*/
static void fmt_time(char buf[32]){
    struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts);
    struct tm tmv; localtime_r(&ts.tv_sec,&tmv);
    char tmp[64];
    int n=snprintf(tmp,sizeof tmp,"%04d-%02d-%02d %02d:%02d:%02d.%03d",
                   tmv.tm_year+1900, tmv.tm_mon+1, tmv.tm_mday,
                   tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)(ts.tv_nsec/1000000));
    if(n<0) n=0;
    if(n>31) n=31;
    memcpy(buf,tmp,(size_t)n); buf[n]=0;
}

static void lf_kv_time(W *w,const char *k,const char *v){
    lf_key(w,k);
    if(w->p<w->end) *w->p++='"';        /* 值含空格 → 规范强制加引号 */
    jesc(w,v);
    if(w->p<w->end) *w->p++='"';
}
static void lf_kv_num(W *w,const char *k,long long v){
    lf_key(w,k);
    char t[32]; int n=snprintf(t,sizeof t,"%lld",v);
    if(w->p+n<w->end){ memcpy(w->p,t,(size_t)n); w->p+=n; }
}
static void lf_kv_hex(W *w,const char *k,long long v){
    lf_key(w,k);
    char t[32]; int n=snprintf(t,sizeof t,"0x%llx",(unsigned long long)v);
    if(w->p+n<w->end){ memcpy(w->p,t,(size_t)n); w->p+=n; }
}

int img_ev_init(img_ev *e,img_lv_t lv,const char *mod,const char *ev,int rc,const char *msg){
    if(!e) return 0;
    memset(e,0,sizeof *e);
    e->magic = IMG_EV_MAGIC;               /* ★ 哨兵：未 init 就 emit 的垃圾结构会被拒 */
    e->lv=lv; e->mod=mod; e->ev=ev; e->rc=rc; e->msg=msg;
    e->enabled = img_log_enabled(lv);      /* 日志够了 或 环形缓冲要抓 → 才继续 */
    return e->enabled;
}
void img_ev_free(img_ev *e){ if(e) e->magic=0; }
void img_ev_str(img_ev *e,const char *k,const char *v){
    if(!e||!e->enabled||e->nf>=IMG_EV_MAXF) return;
    e->f[e->nf].k=k; e->f[e->nf].kind=1; e->f[e->nf].s=v; e->nf++;
}
void img_ev_u64(img_ev *e,const char *k,uint64_t v){
    if(!e||!e->enabled||e->nf>=IMG_EV_MAXF) return;
    e->f[e->nf].k=k; e->f[e->nf].kind=2; e->f[e->nf].n=(long long)v; e->nf++;
}
void img_ev_i64(img_ev *e,const char *k,long long v){
    if(!e||!e->enabled||e->nf>=IMG_EV_MAXF) return;
    e->f[e->nf].k=k; e->f[e->nf].kind=2; e->f[e->nf].n=v; e->nf++;
}
void img_ev_hex(img_ev *e,const char *k,uint64_t v){
    if(!e||!e->enabled||e->nf>=IMG_EV_MAXF) return;
    e->f[e->nf].k=k; e->f[e->nf].kind=3; e->f[e->nf].n=(long long)v; e->nf++;
}
void img_ev_errno(img_ev *e,int err){ if(e) e->err=err; }


void img_log_session(const char *tool,const char *argv,const char *img,uint64_t img_size){
    if(img) snprintf(G.img,sizeof G.img,"%s",img);
    snprintf(G.ctx,sizeof G.ctx,"%s %s",tool?tool:"?",argv?argv:"");
        {
        W w={ G.session, G.session, G.session+sizeof G.session-3 };
        w.p = G.session;
        w.start = G.session;
        char tv[32]; fmt_time(tv);
        lf_kv_time(&w,"time",tv);
        lf_kv_str(&w,"level","INFO");
        lf_kv_str(&w,"tool",tool?tool:"?");
        lf_kv_str(&w,"event","session");
        if(argv&&argv[0]) lf_kv_str(&w,"args",argv);
        if(img&&img[0])   lf_kv_str(&w,"image",img);
        if(img_size) lf_kv_num(&w,"size_bytes",(long long)img_size);
        if(w.p<w.end) *w.p++='\n';
        int n=(int)(w.p-G.session);
        if(n>0) s_session_pending = (G.fd>=0);
    }
}

void img_ev_emit(img_ev *e){
    if(!e||!e->enabled) return;
    if(e->magic != IMG_EV_MAGIC) return;   /* ★ 未 init 的垃圾结构 → 直接拒（实测会段错误）*/
    char buf[LINE_CAP];
    W w={ buf, buf, buf+sizeof buf-4 };
    /* ① 时间（引号是 logfmt 规范强制：值里有空格）*/
    { char t[32]; fmt_time(t); lf_kv_time(&w,"time",t); }
    /* ② level / logger / event（业界惯例键名）*/
    { char tmp[16]; snprintf(tmp,sizeof tmp,"%s",lv_name(e->lv)); lf_kv_str(&w,"level",tmp); }
    lf_kv_str(&w,"logger",e->mod?e->mod:"");
    lf_kv_str(&w,"event",e->ev?e->ev:"");
    /* ③ rc：成功且是 begin/alive 不打（省字节）；done/fail 必打（契约：完成行带结果）*/
    if(e->rc!=0 || (e->ev && (!strcmp(e->ev,"done")||!strcmp(e->ev,"fail"))))
        lf_kv_num(&w,"rc",e->rc);
    if(e->err) lf_kv_num(&w,"errno",e->err);
    /* message 只是【开发期备注】写进日志（内容可能是中文），不是 App 契约；
     * App 渲染界面/卡片时请用 event/action/error_code + 数值（见 DESIGN-OUTPUT.md §八）*/
    if(e->msg&&e->msg[0]) lf_kv_str(&w,"message",e->msg);
    for(int i=0;i<e->nf;i++){
        if(e->f[i].kind==1)      lf_kv_str(&w,e->f[i].k,e->f[i].s);
        else if(e->f[i].kind==2) lf_kv_num(&w,e->f[i].k,e->f[i].n);
        else                     lf_kv_hex(&w,e->f[i].k,e->f[i].n);
    }
    if(w.p<w.end) *w.p++='\n';
    size_t n=(size_t)(w.p-buf);
    if(G.fd>=0) session_flush();       /* 先补会话行，再写事件 */
    {   int lvl_e = (e->lv==IMG_LV_ERROR) || (e->ev && !strcmp(e->ev,"cancel"));
        pump(buf,n,e->lv,(bool)lvl_e);   /* ERROR / cancel：任何日志级别都不丢 */ }
    if(e->lv<=ring_lv) img_ring_add(buf);
    if(g_text_cb) g_text_cb(e, g_text_ud);   /* ★ 文案归消费端（CLI/App 各用自己的语言表）*/
}

/* ---------- 里程碑 ---------- */
static int g_depth=0;
static img_ev g_begin_msg;      /* 借用来保存 begin 行的 msg（img_ev_init 只存指针）*/

void img_span_prep(img_span *s,const char *mod,const char *op,const char *msg){
    if(!s) return;
    memset(s,0,sizeof *s);
    s->mod=mod; s->op=op; s->t0=now_ms(); s->depth=g_depth;
    img_ev_init(&s->ev,IMG_LV_INFO,mod,"begin",IMG_OK,msg);
    img_ev_str(&s->ev,"action",op);
    (void)g_begin_msg;
}
void img_span_kv_str(img_span *s,const char *k,const char *v){
    if(s && !s->started) img_ev_str(&s->ev,k,v);
}
void img_span_kv_u64(img_span *s,const char *k,uint64_t v){
    if(s) img_ev_u64(&s->ev,k,v);       /* begin 前攒到 begin；begin 后不影响 */
}
void img_span_begin(img_span *s){
    if(!s) return;
    s->started=1; g_depth++;
    img_ev_emit(&s->ev);
}
void img_span_end(img_span *s,int rc){
    if(!s) return;
    if(g_depth>0) g_depth--;
    /* ★ 三种收尾，语义分开（用户取消既不是"完成"也不是"出错"）：
     *   rc==0          → event=done   （INFO）
     *   rc==ECANCELLED → event=cancel （WARN；且任何日志级别都不丢 —— 操作未执行完必须留痕）
     *   其它           → event=fail   （ERROR）
     *   调用方（CLI/App）据此区分"用户点了停止"和"真的坏了"。*/
    img_lv_t    lv  = (rc==0) ? IMG_LV_INFO  : (rc==IMG_ECANCELLED ? IMG_LV_WARN : IMG_LV_ERROR);
    const char *evn = (rc==0) ? "done"       : (rc==IMG_ECANCELLED ? "cancel"    : "fail");
    img_ev e;
    if(!img_ev_init(&e,lv,s->mod,evn,rc,NULL)) return;
    img_ev_str(&e,"action",s->op);
    if(rc==IMG_ECANCELLED) img_ev_str(&e,"error_code","CMD-CANCELLED");
    img_ev_i64(&e,"duration_ms",now_ms()-s->t0);
    img_ev_emit(&e);
}

/* ---------- 心跳 ---------- */
void img_hb(const char *mod,const char *op,const char *where,uint64_t iter){
    if(!img_log_enabled(IMG_LV_INFO)) return;
    long long t=now_ms();
    if(!G.hb_last_ms){
        G.hb_last_ms=t; G.hb_interval_ms=HB_STEP_MS;
        G.hb_mod=mod; G.hb_op=op; G.hb_where=where; G.hb_iter=iter;
        return;
    }
    if(t-G.hb_last_ms < G.hb_interval_ms) return;
    bool moved = (iter!=G.hb_iter)||(where!=G.hb_where)||(op!=G.hb_op);
    if(!moved){                                   /* 原地不动 = 疑似无响应 → 拉长间隔 */
        G.hb_interval_ms*=2;
        if(G.hb_interval_ms>HB_MAX_MS) G.hb_interval_ms=HB_MAX_MS;
    } else G.hb_interval_ms=HB_STEP_MS;
    img_ev e;
    if(img_ev_init(&e,IMG_LV_INFO,mod?mod:"hb","alive",IMG_OK,"")){
        img_ev_str(&e,"action",op?op:"");
        if(where) img_ev_str(&e,"where",where);
        img_ev_u64(&e,"iterations",iter);
        img_ev_u64(&e,"gap_ms",(uint64_t)(t-G.hb_last_ms));
        if(!moved) img_ev_u64(&e,"stuck_ms",(uint64_t)G.hb_interval_ms);
        img_ev_emit(&e);
    }
    G.hb_last_ms=t; G.hb_mod=mod; G.hb_op=op; G.hb_where=where; G.hb_iter=iter;
}

/* ---------- 失败卡片（最小可用；0c 补环境指纹/状态/建议表/.error.txt） ---------- */
void img_error_report(int rc,const char *code,const char *mod,const char *op,
                      const char *where,const char *why,const char *suggest){
    img_ev e;
    if(img_ev_init(&e,IMG_LV_ERROR,mod?mod:"err","fail",rc,why)){   /* ★ E 级：任何级别都写 */
        img_ev_str(&e,"error_code",code?code:"?");
        img_ev_str(&e,"action",op?op:"");
        if(where) img_ev_str(&e,"at",where);
        img_ev_emit(&e);
    }
    img_error_record(NULL, code, rc, where, NULL, why);   /* 结构化快照 */
    if(G.out_fd<0||!G.out_human) return;
    char b[LINE_CAP];
    int n=snprintf(b,sizeof b,
        "\n✖ 失败：%s\n   位置：%s\n   原因：%s\n   错误码：%s（%s）\n   建议：%s\n"
        "   ── 以上可直接整段复制 ──\n",
        op?op:"-", where?where:"-", why?why:"-", code?code:"?", img_strerror(rc),
        suggest?suggest:"重试；若复现请把本段与日志文件一起反馈");
    if(n>0) write_all(G.out_fd,b,(size_t)n);
}

/* ---------- 老 CLI 的 --log 接入 ---------- */
int img_log_cli(int argc, char **argv, const char *tool){
    const char *path=NULL, *lvl=NULL, *maxb=NULL; bool plain=false;
    for(int i=1;i<argc;i++){
        const char *a=argv[i];
        if(!a || a[0]!='-') continue;
        if(!strcmp(a,"--log")){ plain=true; continue; }           /* 不给值 → 用默认路径 */
        if(!strncmp(a,"--log=",6)) path=a+6;
        else if(!strncmp(a,"--log-level=",12)) lvl=a+12;
        else if(!strncmp(a,"--log-max=",10))   maxb=a+10;
    }
    if(!path && !plain){
        /* 环境变量：shell 流水线只要 export IMGTOOL_LOG=... 就能让【每个工具】
         *   写进同一份日志（追加模式），不用挨个工具加参数。*/
        const char *ev=getenv("IMGTOOL_LOG");
        if(ev && *ev) path=ev;
        else return 0;                                          /* 没要日志：什么都不做 */
    }
    if(!path && plain){
        /* 默认放镜像旁边：<argv[1]>.imgtool.log（无法获取参数就用 cwd 下的名字）*/
        const char *seed = (argc>1 && argv[1] && argv[1][0]!='-') ? argv[1] : "imgtool";
        static char def[480];
        snprintf(def,sizeof def,"%s.imgtool.log",seed);
        path=def;
    }
    if(!lvl) lvl=getenv("IMGTOOL_LOG_LEVEL");   /* 环境变量同样适用 */
    img_lv_t lv=IMG_LV_INFO;
    if(lvl){
        if(!strcmp(lvl,"error")) lv=IMG_LV_ERROR; else if(!strcmp(lvl,"warn")) lv=IMG_LV_WARN;
        else if(!strcmp(lvl,"debug")) lv=IMG_LV_DEBUG; else if(!strcmp(lvl,"trace")) lv=IMG_LV_TRACE;
    }
    size_t mb=4u*1024*1024;
    if(maxb){ uint64_t v=strtoull(maxb,NULL,0); if(v>0) mb=(size_t)v; }
    if(img_log_open(path,lv,mb,1)!=IMG_OK) return -1;
    img_log_session(tool?tool:"imgtool", argc>1?argv[1]:"", NULL, 0);
    return 1;
}
int img_log_strip_args(int argc, char **argv, char **out, int outcap){
    int n=0;
    for(int i=0;i<argc && n<outcap;i++){
        const char *a=argv[i];
        if(a && a[0]=='-' && (!strncmp(a,"--log",5))) continue;   /* 连带它的值一起跳过：
                                                                   * --log=xx 整个跳；下一项要跳 */
        if(a && (!strcmp(a,"--log")||!strcmp(a,"--log-level")||!strcmp(a,"--log-max")) && i+1<argc){
            i++; continue;
        }
        out[n++]=argv[i];
    }
    return n;
}

