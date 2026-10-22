#define _POSIX_C_SOURCE 200809L
/* img_cmd.c — 命令表骨架：参数校验 / 结果信封 / 上下文 / 分发 */
#include "img_cmd.h"
#include "img_text.h"
#ifndef IMG_WITH_WIM
#define IMG_WITH_WIM 0
#endif
#ifndef IMG_WITH_NTFS3G
#define IMG_WITH_NTFS3G 0
#endif
#ifndef IMG_WITH_ZLIB
#define IMG_WITH_ZLIB 0
#endif
#ifndef IMG_WITH_NTFS
#define IMG_WITH_NTFS 0
#endif
#include "img_io.h"
#include "img_event.h"
#include "img_lock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <time.h>

static long long now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* ================= 参数集合 ================= */
#define ARGS_MAX 24
struct img_args {
    struct { char k[24]; char v[256]; } a[ARGS_MAX];
    int n;
};
img_args *img_args_new(void){ return calloc(1,sizeof(img_args)); }
void img_args_free(img_args *a){ free(a); }
int img_args_count(const img_args *a){ return a?a->n:0; }
int img_args_set(img_args *a, const char *k, const char *v){
    if(!a||!k||!v) return IMG_EINVAL;
    for(int i=0;i<a->n;i++) if(!strcmp(a->a[i].k,k)){     /* 覆盖同名 */
        snprintf(a->a[i].v,sizeof a->a[i].v,"%s",v); return IMG_OK;
    }
    if(a->n>=ARGS_MAX) return IMG_EFULL;
    snprintf(a->a[a->n].k,sizeof a->a[a->n].k,"%s",k);
    snprintf(a->a[a->n].v,sizeof a->a[a->n].v,"%s",v);
    a->n++;
    return IMG_OK;
}
const char *img_args_get(const img_args *a, const char *k){
    if(!a||!k) return NULL;
    for(int i=0;i<a->n;i++) if(!strcmp(a->a[i].k,k)) return a->a[i].v;
    return NULL;
}
uint64_t img_args_u64(const img_args *a, const char *k, uint64_t def){
    const char *v=img_args_get(a,k); if(!v||!*v) return def;
    return strtoull(v,NULL,0);
}
long long img_args_i64(const img_args *a, const char *k, long long def){
    const char *v=img_args_get(a,k); if(!v||!*v) return def;
    return strtoll(v,NULL,0);
}
bool img_args_bool(const img_args *a, const char *k, bool def){
    const char *v=img_args_get(a,k); if(!v||!*v) return def;
    if(!strcmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"on")||!strcasecmp(v,"yes")) return true;
    if(!strcmp(v,"0")||!strcasecmp(v,"false")||!strcasecmp(v,"off")||!strcasecmp(v,"no")) return false;
    return def;
}

/* 严格数字解析：必须整串是数字（防止 "123abc" 被当成 123 静默接受） */
static bool parse_u64_strict(const char *s, uint64_t *out){
    if(!s||!*s) return false;
    char *end=NULL; unsigned long long v=strtoull(s,&end,0);
    if(!end||*end) return false;
    *out=v; return true;
}
static bool parse_i64_strict(const char *s, long long *out){
    if(!s||!*s) return false;
    char *end=NULL; long long v=strtoll(s,&end,0);
    if(!end||*end) return false;
    *out=v; return true;
}

int img_args_validate(const img_args *a, const img_arg *spec, int nargs,
                      char *why, size_t whycap){
    if(why&&whycap) why[0]=0;
    /* ★ 未知参数必须报错：否则 App 把 --lba 拼成 --lbaa 会被静默忽略、
     *   然后按默认值"成功"执行 —— 静默做错事比报错危险得多。*/
    if(a){
        for(int i=0;i<a->n;i++){
            bool known=false;
            for(int j=0;j<nargs;j++) if(!strcmp(a->a[i].k,spec[j].name)){ known=true; break; }
            if(!known){
                if(why) snprintf(why,whycap,"未知参数 --%s（用 --help 看支持的参数）",a->a[i].k);
                return IMG_EINVAL;
            }
        }
    }
    for(int i=0;i<nargs;i++){
        const img_arg *A=&spec[i];
        const char *v=img_args_get(a,A->name);
        if((!v||!*v) && A->def) v=A->def;
        if(!v||!*v){
            if(A->required){
                if(why) snprintf(why,whycap,"缺少必填参数 --%s（%s）",A->name,A->help?A->help:"");
                return IMG_EINVAL;
            }
            continue;
        }
        switch(A->ty){
            case IMG_A_U64: { uint64_t x; if(!parse_u64_strict(v,&x)){ if(why) snprintf(why,whycap,"参数 --%s 必须是数字，收到 \"%s\"",A->name,v); return IMG_EINVAL; } break; }
            case IMG_A_I64: { long long x; if(!parse_i64_strict(v,&x)){ if(why) snprintf(why,whycap,"参数 --%s 必须是整数，收到 \"%s\"",A->name,v); return IMG_EINVAL; } break; }
            case IMG_A_BOOL:{
                if(strcasecmp(v,"1")&&strcasecmp(v,"0")&&strcasecmp(v,"true")&&strcasecmp(v,"false")
                   &&strcasecmp(v,"on")&&strcasecmp(v,"off")&&strcasecmp(v,"yes")&&strcasecmp(v,"no")){
                    if(why) snprintf(why,whycap,"参数 --%s 必须是布尔（1/0/on/off），收到 \"%s\"",A->name,v);
                    return IMG_EINVAL;
                }
                break;
            }
            default: break;
        }
        if(A->check){ int rc=A->check(v,why,whycap); if(rc!=IMG_OK) return rc; }
    }
    return IMG_OK;
}

/* ================= 结果信封 ================= */
void img_result_begin(img_result *r, const char *cmd){
    if(!r) return;
    memset(r,0,sizeof *r);
    r->ok=true; r->cmd=cmd; r->t0_ms=now_ms();
}
static int slot(img_result *r){
    if(r->nf>=IMG_RES_FIELDS){ r->nf_drop++; return -1; }   /* ★ 不静默丢：渲染时会带 _overflow */
    return r->nf++;
}
void img_result_str(img_result *r,const char *k,const char *v){
    int i=slot(r); if(i<0) return;
    snprintf(r->f[i].k,sizeof r->f[i].k,"%s",k);
    r->f[i].ty=1; snprintf(r->f[i].s,sizeof r->f[i].s,"%s",v?v:"");
}
void img_result_u64(img_result *r,const char *k,uint64_t v){
    int i=slot(r); if(i<0) return;
    snprintf(r->f[i].k,sizeof r->f[i].k,"%s",k); r->f[i].ty=2; r->f[i].n=(long long)v;
}
void img_result_i64(img_result *r,const char *k,long long v){
    int i=slot(r); if(i<0) return;
    snprintf(r->f[i].k,sizeof r->f[i].k,"%s",k); r->f[i].ty=2; r->f[i].n=v;
}
void img_result_bool(img_result *r,const char *k,bool v){
    int i=slot(r); if(i<0) return;
    snprintf(r->f[i].k,sizeof r->f[i].k,"%s",k); r->f[i].ty=3; r->f[i].b=v;
}
void img_result_raw(img_result *r,const char *k,const char *json){
    int i=slot(r); if(i<0) return;
    snprintf(r->f[i].k,sizeof r->f[i].k,"%s",k);
    r->f[i].ty=4;
    /* 只存指针（列表可能很大，不拷贝）；调用方需保证渲染前一直有效。
     * ★ 用专门的 raw 字段而不是往 char[] 里塞指针：后者在 ARM 上是未对齐访问。*/
    r->f[i].raw = json;
}
void img_result_fail(img_result *r,int rc,const char *code,const char *at,
                     const char *why,const char *suggest){
    if(!r) return;
    r->ok=false; r->rc=rc;
    snprintf(r->code,sizeof r->code,"%s",code?code:"?");
    snprintf(r->why,sizeof r->why,"%s",why?why:"");
    snprintf(r->at,sizeof r->at,"%s",at?at:"");
    snprintf(r->suggest,sizeof r->suggest,"%s",suggest?suggest:"重试；若复现请把本结果与日志一起反馈");
    /* ★ 错误也要写事件流（E 级：任何日志级别都不会漏），失败卡片与日志同源 */
    img_ev e;
    if(img_ev_init(&e,IMG_LV_ERROR,"cmd","fail",rc,r->why)){
        img_ev_str(&e,"code",r->code);
        img_ev_str(&e,"cmd",r->cmd?r->cmd:"");
        if(r->at[0]) img_ev_str(&e,"at",r->at);
        img_ev_emit(&e);
    }
    /* ★ 人话失败卡片（屏幕 + <镜像>.error.txt）：用户能整段复制发给我们 */
    img_error_record(r->cmd, r->code, rc, r->at, NULL, r->why);   /* 结构化快照；
                                                                   * 卡片/人话行由消费端（CLI/App）用各自的文案表渲染 */
}

/* ---------- JSON 渲染（栈上，无 malloc） ---------- */
typedef struct { char *p,*start,*end; } W;
static void jesc(W *w,const char *s){
    for(; s&&*s; s++){
        unsigned char c=(unsigned char)*s; const char *rep=NULL; char tmp[8];
        switch(c){
            case '"': rep="\\\""; break; case '\\': rep="\\\\"; break;
            case '\n': rep="\\n"; break; case '\r': rep="\\r"; break; case '\t': rep="\\t"; break;
            default: if(c<0x20){ snprintf(tmp,sizeof tmp,"\\u%04x",c); rep=tmp; } break;
        }
        if(rep){ size_t l=strlen(rep); if(w->p+l<w->end){ memcpy(w->p,rep,l); w->p+=l; } }
        else if(w->p+1<w->end) *w->p++=(char)c;
    }
}
static void wkey(W *w,const char *k){
    if(w->p>w->start)*w->p++=',';
    if(w->p<w->end)*w->p++='"';        /* ★ key 必须带引号，否则整个 JSON 非法 */
    jesc(w,k);
    if(w->p<w->end)*w->p++='"';
    if(w->p<w->end)*w->p++=':';
}
static void wstr(W *w,const char *k,const char *v){ wkey(w,k); if(w->p<w->end)*w->p++='"'; jesc(w,v?v:""); if(w->p<w->end)*w->p++='"'; }
static void wraw(W *w,const char *k,const char *raw){ wkey(w,k); while(*raw && w->p<w->end) *w->p++=*raw++; }
static void wnum(W *w,const char *k,long long v){ wkey(w,k); char t[32]; int n=snprintf(t,sizeof t,"%lld",v); if(w->p+n<w->end){ memcpy(w->p,t,(size_t)n); w->p+=n; } }

int img_result_json(const img_result *r, char *buf, int cap){
    if(!r||!buf||cap<16) return IMG_EINVAL;
    W w={buf+1,buf+1,buf+cap-1};
    buf[0]='{';
    wraw(&w,"ok", r->ok?"true":"false");
    wstr(&w,"cmd", r->cmd?r->cmd:"");
    wnum(&w,"version",1);
    wnum(&w,"duration_ms", now_ms()-r->t0_ms);
    if(r->ok){
        wkey(&w,"data"); if(w.p<w.end)*w.p++='{';
        char *ds=w.p;
        W dw={w.p,ds,w.end};
        for(int i=0;i<r->nf;i++){
            if(r->f[i].ty==1) wstr(&dw,r->f[i].k,r->f[i].s);
            else if(r->f[i].ty==2) wnum(&dw,r->f[i].k,r->f[i].n);
            else if(r->f[i].ty==4) wraw(&dw,r->f[i].k,r->f[i].raw?r->f[i].raw:"null");
            else wraw(&dw,r->f[i].k,r->f[i].b?"true":"false");
        }
        if(r->nf_drop>0) wnum(&dw,"_overflow",r->nf_drop);
        w.p=dw.p;
        if(w.p<w.end)*w.p++='}';
    } else {
        wkey(&w,"err"); if(w.p<w.end)*w.p++='{';
        char *es=w.p; W ew={w.p,es,w.end};
        wnum(&ew,"rc",r->rc);
        wstr(&ew,"error_code",r->code);
        if(r->at[0]) wstr(&ew,"at",r->at);
        /* why/suggest 是【文案】，不进信封（库零文案；App 用 error_code 查自己的表）*/
        w.p=ew.p;
        if(w.p<w.end)*w.p++='}';
    }
    if(w.p<w.end)*w.p++='}';
    if(w.p<w.end)*w.p++='\n';
    *w.p=0;
    return (int)(w.p-buf);
}

/* ================= 进度通道（P2） ================= */
static img_progress_cb g_prog_cb = NULL;
static void           *g_prog_ud = NULL;
static img_prog       *g_prog    = NULL;

void img_cmd_set_progress(img_progress_cb cb, void *ud){
    g_prog_cb = cb; g_prog_ud = ud;
    if(g_prog){ img_prog_free(g_prog); g_prog=NULL; }
    if(cb) g_prog = img_prog_new(cb, ud, 200);
}
img_prog *img_cmd_prog(void){ return g_prog; }

/* ================= 上下文 ================= */
struct img_ctx {
    img_lock_t lk;               /* ★ 同一句柄串行：并发调同一张盘会踩内部缓存 */
    char     path[512];
    bool     writable, check_size;
    img_io  *io;
    img_qcow2 *q;
    img_parttable_t pt;
    bool     pt_valid;
};
img_ctx *img_ctx_open(const char *image_path, bool writable, bool check_size,
                      char *err, size_t errcap){
    if(!image_path||!*image_path){ if(err) snprintf(err,errcap,"镜像路径为空"); return NULL; }
    img_ctx *c=calloc(1,sizeof *c);
    if(!c) return NULL;
    snprintf(c->path,sizeof c->path,"%s",image_path);
    img_set_image(image_path);        /* 卡片/.error.txt 要用（即使没开日志）*/
    c->writable=writable; c->check_size=check_size;
    img_lock_init(&c->lk);
    /* ★ 命令表的镜像入口【绝不创建文件】：用户在 App 里选错/手输一个不存在的路径时，
     *   过去会留下 0 字节垃圾镜像 + 报 QC-OPEN-FAIL（像"镜像损坏"）。现在直接 IMG-OPEN-FAIL。
     *   建盘是 imglayout mk / img_clone 的职责，它们仍用 img_io_open_path()。*/
    c->io=img_io_open_existing(c->path,writable);
    if(!c->io){ if(err) snprintf(err,errcap,"无法打开镜像：%s（不存在或不是普通文件）",c->path); free(c); return NULL; }
    return c;
}
void img_ctx_close(img_ctx *c){
    if(!c) return;
    if(c->q){ img_qcow2_close(c->q); c->q=NULL; }
    if(c->io){ img_io_close(c->io); c->io=NULL; }
    img_lock_destroy(&c->lk);
    free(c);
}
const char  *img_ctx_path(const img_ctx *c){ return c?c->path:""; }
bool         img_ctx_check_size(const img_ctx *c){ return c?c->check_size:false; }
img_io     *img_ctx_io(img_ctx *c){ return c?c->io:NULL; }
img_qcow2   *img_ctx_qcow2(img_ctx *c){
    if(!c) return NULL;
    if(c->q) return c->q;
    img_qcow2_opts_t o={0};
    c->q=img_qcow2_open(c->io,&o);
    if(c->q && c->writable) img_qcow2_write_init(c->q);
    return c->q;
}
int img_ctx_parttable(img_ctx *c, img_parttable_t **out){
    if(!c||!out) return IMG_EINVAL;
    if(!img_ctx_qcow2(c)) return IMG_EFORMAT;
    if(!c->pt_valid){
        int rc=img_parttable_read(c->q,&c->pt,0);
        if(rc!=IMG_OK) return rc;
        c->pt_valid=true;
    }
    *out=&c->pt;
    return IMG_OK;
}

/* ================= 分发 ================= */
extern const img_cmd IMG_CMD_TABLE[];
extern const int     IMG_CMD_COUNT;

const img_cmd *img_cmd_table(int *out_count){
    if(out_count) *out_count=IMG_CMD_COUNT;
    return IMG_CMD_TABLE;
}
const img_cmd *img_cmd_find(const char *name){
    if(!name) return NULL;
    for(int i=0;i<IMG_CMD_COUNT;i++) if(!strcmp(IMG_CMD_TABLE[i].name,name)) return &IMG_CMD_TABLE[i];
    return NULL;
}
int img_cmd_exec(const char *name, img_ctx *ctx, const img_args *in, img_result *out){
    const img_cmd *c=img_cmd_find(name);
    img_result_begin(out,name);
    if(!c){
        img_result_fail(out,IMG_ENOENT,"CMD-NOTFOUND",name,"没有这个命令","用 --list 查看可用命令");
        return IMG_ENOENT;
    }
    /* ★ 镜像路径的权威来源是 ctx（App 先打开镜像再调命令）。
     *   所以：ctx 存在而 args 里未提供 img 时，自动注入 —— 否则每个命令都得让
     *   App 重复传一遍路径，还会出现"ctx 是 A 盘、--img 是 B 盘"的不一致。*/
    img_args *tmp=NULL;
    const img_args *use=in;
    if(ctx && (!in || !img_args_get(in,"img"))){
        tmp=img_args_new();
        if(in) for(int i=0;i<in->n;i++) img_args_set(tmp,in->a[i].k,in->a[i].v);
        img_args_set(tmp,"img",img_ctx_path(ctx));
        use=tmp;
    }
    char why[256];
    int rc=img_args_validate(use,c->args,c->nargs,why,sizeof why);
    if(rc!=IMG_OK){
        if(tmp) img_args_free(tmp);
        img_result_fail(out,rc,"CMD-ARG-BAD",name,why,"检查参数名/类型；用 --help=<命令> 看用法");
        return rc;
    }
    /* ★ 同一句柄串行：App 从多个线程调同一个 ctx 时排队执行，
     *   避免踩坏 qcow2 的 L1/L2 缓存与 L2 表缓冲 */
    img_lock_t *lk = ctx ? &ctx->lk : NULL;
    img_lock_acquire(lk);
    rc=c->run(ctx,use,out);
    img_lock_release(lk);
    if(tmp) img_args_free(tmp);
    img_cancel_reset();            /* ★ 本次命令结束 → 清取消标志，避免污染下一个命令 */
    if(rc==IMG_ECANCELLED && out->ok) (void)img_result_fail(out,rc,"CMD-CANCELLED",NULL,NULL,NULL);
    if(rc!=IMG_OK && out->ok)          /* handler 忘了填 err：兜一层，保证信封永远完整 */
        img_result_fail(out,rc,"CMD-FAILED",name,img_strerror(rc),NULL);
    return rc;
}
int img_cmd_list_json(char *buf,int cap){
    if(!buf||cap<32) return IMG_EINVAL;
    W w={buf+1,buf+1,buf+cap-1};
    buf[0]='{'; wraw(&w,"version","1");
    /* 本构建带了哪些可选库 —— App 据此把不可用命令【提前灰掉】，不是让用户点了才报错（硬缺口 #3）*/
    {   char feat[160];
        snprintf(feat,sizeof feat,
            "{\"wimlib\":%s,\"ntfs3g\":%s,\"zlib\":%s,\"mkntfs\":%s}",
            IMG_WITH_WIM?"true":"false", IMG_WITH_NTFS3G?"true":"false",
            IMG_WITH_ZLIB?"true":"false", IMG_WITH_NTFS?"true":"false");
        wraw(&w,"features",feat);       /* 走 writer：逗号/引号由它管 */
    }
    wkey(&w,"cmds"); if(w.p<w.end)*w.p++='[';
    for(int i=0;i<IMG_CMD_COUNT;i++){
        const img_cmd *c=&IMG_CMD_TABLE[i];
        if(i){ if(w.p<w.end)*w.p++=','; }
        if(w.p<w.end)*w.p++='{';
        char *cs=w.p; W cw={w.p,cs,w.end};
        wstr(&cw,"name",c->name);
        wstr(&cw,"since",c->since?c->since:"");
        wstr(&cw,"help",c->help?img_t(c->help):"");   /* ★ 中英双份 */
        wkey(&cw,"args"); if(cw.p<cw.end)*cw.p++='[';
        for(int j=0;j<c->nargs;j++){
            const img_arg *A=&c->args[j];
            if(j){ if(cw.p<cw.end)*cw.p++=','; }
            if(cw.p<cw.end)*cw.p++='{';
            char *as=cw.p; W aw={cw.p,as,cw.end};
            wstr(&aw,"name",A->name);
            const char *ty="str";
            switch(A->ty){ case IMG_A_U64: ty="u64"; break; case IMG_A_I64: ty="i64"; break;
                           case IMG_A_BOOL: ty="bool"; break; case IMG_A_ENUM: ty="enum"; break; default: break; }
            wstr(&aw,"type",ty);
            wraw(&aw,"required",A->required?"true":"false");
            if(A->def) wstr(&aw,"default",A->def);
            if(A->help) wstr(&aw,"help",img_t(A->help));   /* ★ 中英双份 */
            cw.p=aw.p;
            if(cw.p<cw.end)*cw.p++='}';
        }
        if(cw.p<cw.end)*cw.p++=']';
        w.p=cw.p;
        if(w.p<w.end)*w.p++='}';
    }
    /* ★ 装不下就明确失败：以前会吐半截 JSON（App 解析直接报错，
     *   用户看到的是"命令表导出失败"此类莫名其妙的东西）。*/
    if(w.p >= w.end-1) return 0;
    *w.p++=']';
    *w.p++='}';
    *w.p++='\n';
    *w.p=0;
    return (int)(w.p-buf);
}
