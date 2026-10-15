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
 *    且"最后一条 alive 的时间"本身就是卡死证据）
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

static const char *img_platform(void){
#ifdef _WIN32
    return "windows";
#else
    static char b[128];
    if(!b[0]){ struct utsname u; if(uname(&u)==0) snprintf(b,sizeof b,"%s",u.sysname); else snprintf(b,sizeof b,"posix"); }
    return b;
#endif
}
static const char *img_abi(void){
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

static char *ring[RING_CAP];
static int   ring_n, ring_head;
static img_lv_t ring_lv = IMG_LV_DEBUG;      /* 环形缓冲独立级别（默认抓 debug 过程）*/

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static char lv_ch(img_lv_t l){
    switch(l){ case IMG_LV_ERROR: return 'E'; case IMG_LV_WARN: return 'W';
               case IMG_LV_INFO:  return 'I'; case IMG_LV_DEBUG: return 'D';
               default: return 'T'; }
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
    int n=snprintf(note,sizeof note,
        "{\"t\":%lld,\"l\":\"I\",\"m\":\"log\",\"ev\":\"rotate\",\"kept\":\"%s\"}\n",
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
void img_log_close(void){
    if(G.fd>=0 && G.path[0]) close(G.fd);   /* fd 模式不替调用方关 fd */
    G.fd=-1; G.out_fd=-1;
}
void img_out_open_fd(int fd, bool human){ G.out_fd=fd; G.out_human=human; }

void img_card_set_context(const char *kv_line){
    if(kv_line) snprintf(G.ctx,sizeof G.ctx,"%s",kv_line);
}
void img_log_session(const char *tool,const char *argv,const char *img,uint64_t img_size){
    if(img) snprintf(G.img,sizeof G.img,"%s",img);
    snprintf(G.ctx,sizeof G.ctx,"%s %s",tool?tool:"?",argv?argv:"");
    int n=snprintf(G.session,sizeof G.session,
        "{\"t\":0,\"l\":\"I\",\"m\":\"app\",\"ev\":\"session\",\"tool\":\"%s\",\"argv\":\"%s\","
        "\"img\":\"%s\",\"size\":%llu}\n",
        tool?tool:"?",argv?argv:"",img?img:"",(unsigned long long)img_size);
    if(n>0){ write_all(G.fd,G.session,(size_t)n); if(G.fd>=0) G.written+=(size_t)n; }
}

/* ---------- 输出（日志 + 屏幕各一份） ---------- */
static void pump(const char *line,size_t n,img_lv_t lv){
    if(log_on(lv)){
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

/* ---------- 取消标志 ---------- */
static volatile int g_cancel = 0;
void img_cancel(void){ g_cancel = 1; }
void img_cancel_reset(void){ g_cancel = 0; }
bool img_cancelled(void){ return g_cancel!=0; }

/* ---------- 屏幕人话渲染（Linux 风格；只在 human 模式用） ---------- */
static const char *ev_str(const img_ev *e, const char *k){
    for(int i=0;i<e->nf;i++) if(!strcmp(e->f[i].k,k) && e->f[i].kind==1) return e->f[i].s;
    return NULL;
}
static long long ev_num(const img_ev *e, const char *k, long long def){
    for(int i=0;i<e->nf;i++) if(!strcmp(e->f[i].k,k)) return e->f[i].n;
    return def;
}
static void human_emit(const img_ev *e){
    if(G.out_fd<0 || !G.out_human) return;
    if(e->lv>IMG_LV_INFO) return;                 /* 屏幕只显示 info 及以上 */
    char b[LINE_CAP]; int n=0;
    bool is_begin=!strcmp(e->ev,"begin"), is_done=!strcmp(e->ev,"done"),
         is_fail =!strcmp(e->ev,"fail"),  is_alive=!strcmp(e->ev,"alive");
    if(is_begin){
        n=snprintf(b,sizeof b,"▶ %s", e->msg&&e->msg[0]?e->msg:"开始");
        const char *p=ev_str(e,"path"); if(p) n+=snprintf(b+n,sizeof b-(size_t)n,"  %s",p);
        long long sz=ev_num(e,"size",-1); if(sz>=0) n+=snprintf(b+n,sizeof b-(size_t)n,"  %lld 字节",sz);
        long long pt=ev_num(e,"part",-1); if(pt>=0) n+=snprintf(b+n,sizeof b-(size_t)n,"  分区 LBA %lld",pt);
    } else if(is_done){
        long long ms=ev_num(e,"ms",-1);
        if(e->rc!=0) n=snprintf(b,sizeof b,"✖ %s 失败（%s）",ev_str(e,"op")?ev_str(e,"op"):"操作",img_strerror(e->rc));
        else         n=snprintf(b,sizeof b,"✔ %s 完成", ev_str(e,"op")?ev_str(e,"op"):"操作");
        if(ms>=0) n+=snprintf(b+n,sizeof b-(size_t)n,"（%lld ms）",ms);
    } else if(is_alive){
        if(G.hb_interval_ms<=HB_STEP_MS){          /* 只在"正常推进"时刷，卡住时别刷屏 */
            n=snprintf(b,sizeof b,"… 仍在进行：%s %s（iter=%llu）",
                       ev_str(e,"op")?ev_str(e,"op"):"", ev_str(e,"where")?ev_str(e,"where"):"",
                       (unsigned long long)ev_num(e,"iter",0));
        } else return;
    } else if(is_fail){
        return;                                    /* 失败由 img_fail_card 统一出卡片 */
    } else return;
    if(n>0){ b[n++]='\n'; write_all(G.out_fd,b,(size_t)n); }
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

/* ---------- 失败卡片（人话 + 可复制 + 落 .error.txt） ---------- */
void img_fail_card(const char *cmd,const char *code,const char *at,
                   const char *why,const char *suggest){
    char card[8192]; int n=0;
    n+=snprintf(card+n,sizeof card-(size_t)n,
        "\n════════ Icdqcow2 错误报告（整段复制即可）════════\n"
        "工具     %s · %s · %s\n"
        "命令     %s\n"
        "镜像     %s\n",
        IMGTOOL_VERSION, img_platform(), img_abi(),
        G.ctx[0]?G.ctx:(cmd?cmd:"(未设置)"), G.img[0]?G.img:"(未设置)");
    n+=snprintf(card+n,sizeof card-(size_t)n,
        "失败位置 %s\n错误码   %s\n原因     %s\n建议     %s\n",
        (at&&at[0])?at:"-", code?code:"?", (why&&why[0])?why:"-",
        (suggest&&suggest[0])?suggest:"重试；若复现请把本段与日志文件一起反馈");
    if(G.img[0]) n+=snprintf(card+n,sizeof card-(size_t)n,"日志     %s.imgtool.log\n",G.img);
    n+=snprintf(card+n,sizeof card-(size_t)n,
        "──────── 事件尾部（内存最近 %d 条，机器格式）────────\n", img_ring_count());
    /* 事件尾部：直接给 ring 里的 JSON 行（同源，便于我这边解析）*/
    int used=ring_dump_into(card+n,(int)(sizeof card-(size_t)n-64));
    if(used>0) n+=used;
    n+=snprintf(card+n,sizeof card-(size_t)n,"════════ 错误报告结束 ════════\n");

    if(G.out_fd>=0 && G.out_human) write_all(G.out_fd,card,(size_t)n);
    if(G.img[0]){                                   /* ★ 落一份文件：用户直接发这个 */
        char path[600]; snprintf(path,sizeof path,"%s.error.txt",G.img);
        int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0644);
        if(fd>=0){ write_all(fd,card,(size_t)n); close(fd); }
    }
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
static void w_key(W *w,const char *k){
    if(w->p>w->start) *w->p++=',';      /* ★ 首字段不能加逗号 */
    if(w->p<w->end) *w->p++='"';        /* ★ key 必须带引号，否则整个 JSON 非法 */
    jesc(w,k);
    if(w->p<w->end) *w->p++='"';
    if(w->p<w->end) *w->p++=':';
}
static void w_str(W *w,const char *k,const char *v){
    w_key(w,k);
    if(w->p<w->end) *w->p++='"';
    jesc(w,v?v:"");
    if(w->p<w->end) *w->p++='"';
}
static void w_num(W *w,const char *k,long long v){
    w_key(w,k);
    char t[32]; int n=snprintf(t,sizeof t,"%lld",v);
    if(w->p+n<w->end){ memcpy(w->p,t,(size_t)n); w->p+=n; }
}
static void w_hex(W *w,const char *k,long long v){
    w_key(w,k);
    char t[32]; int n=snprintf(t,sizeof t,"\"0x%llx\"",(unsigned long long)v);
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

void img_ev_emit(img_ev *e){
    if(!e||!e->enabled) return;
    if(e->magic != IMG_EV_MAGIC) return;   /* ★ 未 init 的垃圾结构 → 直接拒（实测会段错误）*/
    if(G.t0_ms==0) G.t0_ms = now_ms();     /* ★ 没开日志时 G.t0_ms 未初始化，
                                             *   t=now-0 会显示成"开机以来毫秒"（实测 2.5 亿）。
                                             *   从首个事件起算，时间戳才有意义 */
    char buf[LINE_CAP];
    W w={ buf+1, buf+1, buf+sizeof buf-2 };
    buf[0]='{';
    w_num(&w,"t",now_ms()-G.t0_ms);
    w_key(&w,"l"); if(w.p+3<w.end){ *w.p++='"'; *w.p++=lv_ch(e->lv); *w.p++='"'; }
    w_str(&w,"m",e->mod);
    w_str(&w,"ev",e->ev);
    w_num(&w,"rc",e->rc);                  /* rc 统一带（0=OK）；不再由调用方重复加 */
    if(e->err) w_num(&w,"errno",e->err);
    if(e->msg&&e->msg[0]) w_str(&w,"msg",e->msg);
    for(int i=0;i<e->nf;i++){
        if(e->f[i].kind==1)      w_str(&w,e->f[i].k,e->f[i].s);
        else if(e->f[i].kind==2) w_num(&w,e->f[i].k,e->f[i].n);
        else                     w_hex(&w,e->f[i].k,e->f[i].n);
    }
    if(w.p<w.end) *w.p++='}';
    if(w.p<w.end) *w.p++='\n';
    size_t n=(size_t)(w.p-buf);
    pump(buf,n,e->lv);
    human_emit(e);                     /* 屏幕人话行（Linux 风格）*/
    if(e->lv<=ring_lv) img_ring_add(buf);   /* 过程事件进内存（供失败卡片） */
}

/* ---------- 里程碑 ---------- */
static int g_depth=0;
static img_ev g_begin_msg;      /* 借用来保存 begin 行的 msg（img_ev_init 只存指针）*/

void img_span_prep(img_span *s,const char *mod,const char *op,const char *msg){
    if(!s) return;
    memset(s,0,sizeof *s);
    s->mod=mod; s->op=op; s->t0=now_ms(); s->depth=g_depth;
    img_ev_init(&s->ev,IMG_LV_INFO,mod,"begin",IMG_OK,msg);
    img_ev_str(&s->ev,"op",op);
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
    img_ev e;
    if(!img_ev_init(&e,IMG_LV_INFO,s->mod,"done",rc,IMG_OK==rc?"":NULL)) return;
    img_ev_str(&e,"op",s->op);
    img_ev_i64(&e,"ms",now_ms()-s->t0);
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
    if(!moved){                                   /* 原地不动 = 疑似卡死 → 拉长间隔 */
        G.hb_interval_ms*=2;
        if(G.hb_interval_ms>HB_MAX_MS) G.hb_interval_ms=HB_MAX_MS;
    } else G.hb_interval_ms=HB_STEP_MS;
    img_ev e;
    if(img_ev_init(&e,IMG_LV_INFO,mod?mod:"hb","alive",IMG_OK,"")){
        img_ev_str(&e,"op",op?op:"");
        if(where) img_ev_str(&e,"where",where);
        img_ev_u64(&e,"iter",iter);
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
        img_ev_str(&e,"code",code?code:"?");
        img_ev_str(&e,"op",op?op:"");
        if(where) img_ev_str(&e,"at",where);
        img_ev_emit(&e);
    }
    if(G.out_fd<0||!G.out_human) return;
    char b[LINE_CAP];
    int n=snprintf(b,sizeof b,
        "\n✖ 失败：%s\n   位置：%s\n   原因：%s\n   错误码：%s（%s）\n   建议：%s\n"
        "   ── 以上可直接整段复制 ──\n",
        op?op:"-", where?where:"-", why?why:"-", code?code:"?", img_strerror(rc),
        suggest?suggest:"重试；若复现请把本段与日志文件一起反馈");
    if(n>0) write_all(G.out_fd,b,(size_t)n);
}
