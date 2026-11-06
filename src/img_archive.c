/* img_archive.c — 压缩包只读（REQ-31）+ 索引 token 缓存（REQ-34 §3）
 *
 * 设计要点：
 *   · 只读：本文件没有任何写回包的代码路径（命令层对写操作直接 ENOTSUP）。
 *   · 流式：libarchive 逐块读，抽文件时直接写目标，0 临时文件。
 *   · 7z/rar 是流式格式 —— 取深层条目必须从包头扫起。所以列表走"全量索引 + token"，
 *     App 点进子目录不再重扫；抽单文件按需求【不需要 token】（预览一个文件不必先索引）。
 *   · 缓存：2 槽 LRU、idle 10 分钟、条目上限 20 万，identity（dev+ino+mtime+size）变了即失效。
 *     进程重启自然失效（内存态，不落盘）—— 这是 REQ-34 要的语义。
 */
#include "img_archive.h"
#include "img_rar.h"
#include "img_7z_solid.h"
#include "img_stream.h"
#include "img_cmd.h"
#include "img_progress.h"
#include "img_event.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>

#if !defined(IMG_WITH_ARCHIVE)
#define IMG_WITH_ARCHIVE 0
#endif

#if IMG_WITH_ARCHIVE

#define ARC_MAX_ENTRIES   200000u
#define ARC_SLOTS         2
#define ARC_IDLE_MS       (10u*60u*1000u)

struct img_archive_index {
    char      path[1024];
    char      fmt[24];
    size_t    n, cap;
    char    **names;             /* 与 v 平行（strdup）*/
    img_archive_ent *v;
    bool      truncated;
    bool      solid;          /* RAR 固实包（REQ-50）*/
    uint64_t  built_ms;
};

/* ---------------- 小工具 ---------------- */
static uint64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u;
}
static bool utf8_ok(const char *s){
    const unsigned char *p=(const unsigned char*)s;
    while(*p){
        if(*p<0x80){ p++; continue; }
        int n = (*p>=0xF0&&*p<0xF5)?4 : (*p>=0xE0)?3 : (*p>=0xC0)?2 : 0;
        if(!n) return false;
        p++;
        for(int i=1;i<n;i++){ if((p[0]&0xC0)!=0x80) return false; p++; }
    }
    return true;
}
/* 包内路径规范化：去掉 "./"、前导 "/"、结尾 "/"，再加"/"开头；空路径→"/" */
static void norm_path(const char *in, char *out, size_t cap){
    while(in[0]=='.' && in[1]=='/') in+=2;
    while(*in=='/') in++;
    snprintf(out,cap,"%s",in?in:"");
    size_t n=strlen(out);
    while(n>1 && out[n-1]=='/') out[--n]=0;
    if(n==0){ snprintf(out,cap,"/"); return; }
    if(out[0]!='/'){ memmove(out+1,out,n+1); out[0]='/'; }
}
static const char *fmt_from_archive(struct archive *a){
    /* ★ 这些不是互斥的位标志，是【数值区间】：tar=0x30000 里也含 0x10000 位，
     *   直接 c&ARCHIVE_FORMAT_ZIP 会把 tar/tar.gz 全判成 zip（踩过）。必须比 base mask。*/
    int c=archive_format(a) & ARCHIVE_FORMAT_BASE_MASK;
    switch(c){
        case ARCHIVE_FORMAT_ZIP:     return "zip";
        case ARCHIVE_FORMAT_7ZIP:    return "7z";
        case ARCHIVE_FORMAT_RAR_V5:  return "rar5";
        case ARCHIVE_FORMAT_RAR:     return "rar";
        case ARCHIVE_FORMAT_CAB:     return "cab";
        case ARCHIVE_FORMAT_TAR:     return "tar";
        case ARCHIVE_FORMAT_CPIO:    return "cpio";
        case ARCHIVE_FORMAT_ISO9660: return "iso9660";
        case ARCHIVE_FORMAT_EMPTY:   return "empty";
        default: break;
    }
    return "unknown";
}
static int open_archive(const char *path, struct archive **out, bool cp936){
    struct archive *a=archive_read_new();
    if(!a) return IMG_ENOMEM;
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if(cp936){
        /* 老 zip 用 GBK 存名字（没打 UTF-8 标志）：让 libarchive 按 CP936 转成 UTF-8。
         * 有 UTF-8 标志的条目不受影响。*/
        archive_read_set_options(a,"hdrcharset=CP936");
    }
    int r=archive_read_open_filename(a,path,128*1024);
    if(r!=ARCHIVE_OK){
        archive_read_free(a);
        return IMG_EFORMAT;
    }
    *out=a;
    return IMG_OK;
}
static void set_err(char *err, size_t cap, const char *fmt, const char *arg){
    if(err && cap) snprintf(err,cap,fmt,arg?arg:"");
}

int img_archive_format(const char *path, char *out, size_t cap, char *err, size_t errcap){
    if(out && cap) out[0]=0;
    if(!path||!out) return IMG_EINVAL;
#if IMG_WITH_RAR
    if(img_rar_is_rar(path)){ snprintf(out,cap,"rar"); return IMG_OK; }   /* REQ-50 */
#endif
    struct archive *a=NULL;
    if(open_archive(path,&a,false)!=IMG_OK){
        set_err(err,errcap,"打不开压缩包：%s",path);
        return IMG_EFORMAT;
    }
    struct archive_entry *e;       /* format 要读过 header 才定得下来 */
    (void)archive_read_next_header(a,&e);
    snprintf(out,cap,"%s",fmt_from_archive(a));
    archive_read_free(a);
    return IMG_OK;
}

/* ---------------- 索引 ---------------- */
/* 目录子项计数：开地址哈希（path 指针 → 计数）*/
typedef struct { const char *key; int cnt; } chash_slot;
static uint64_t hstr(const char *s){
    uint64_t h=1469598103934665603ull;
    for(; *s; s++){ h^=(unsigned char)*s; h*=1099511628211ull; }
    return h;
}
static void count_children(img_archive_index *ix, int *out_counts){
    size_t cap=1; while(cap < ix->n*2 && cap < (1u<<22)) cap<<=1;
    chash_slot *h=(chash_slot*)calloc(cap,sizeof *h);
    if(!h) return;                       /* 内存不够就不填 children_count（-1）*/
    for(size_t i=0;i<ix->n;i++){
        const char *p=ix->v[i].path;
        char parent[512]; snprintf(parent,sizeof parent,"%s",p);
        char *slash=strrchr(parent,'/');
        if(!slash) continue;
        if(slash==parent) parent[1]=0; else *slash=0;
        uint64_t idx=hstr(parent)&(cap-1);
        while(h[idx].key && strcmp(h[idx].key,parent)) idx=(idx+1)&(cap-1);
        if(!h[idx].key) h[idx].key=strdup(parent);    /* 只在哈希里用，最后统一 free? 简化：不 free（进程内少量）*/
        h[idx].cnt++;
    }
    for(size_t i=0;i<ix->n;i++){
        if(!ix->v[i].is_dir) continue;
        uint64_t idx=hstr(ix->v[i].path)&(cap-1);
        int c=0;
        while(h[idx].key){
            if(!strcmp(h[idx].key,ix->v[i].path)){ c=h[idx].cnt; break; }
            idx=(idx+1)&(cap-1);
        }
        out_counts[i]=c;
    }
    for(size_t i=0;i<cap;i++) free((void*)h[i].key);
    free(h);
}

static img_archive_index *index_alloc(size_t cap){
    img_archive_index *ix=(img_archive_index*)calloc(1,sizeof *ix);
    if(!ix) return NULL;
    ix->cap=cap;
    ix->v=(img_archive_ent*)calloc(cap,sizeof *ix->v);
    ix->names=(char**)calloc(cap,sizeof *ix->names);
    if(!ix->v||!ix->names){ free(ix->v); free(ix->names); free(ix); return NULL; }
    return ix;
}
/* ★ 条目数组扩容：以前只有 index_alloc(4096) 而没有扩容，
 *   超过 4096 条的包（用户的 CS1.6 RAR 有 4901 条）会直接写越界 →
 *   SIGSEGV/堆损坏。libarchive 路径同样有这个坑，只是以前的测试包都 <4096。*/
static bool index_grow(img_archive_index *ix){
    if(ix->n < ix->cap) return true;
    if(ix->cap >= ARC_MAX_ENTRIES) return false;
    size_t ncap = ix->cap ? ix->cap*2 : 1024;
    if(ncap > ARC_MAX_ENTRIES) ncap = ARC_MAX_ENTRIES;
    if(ncap <= ix->cap) return false;
    img_archive_ent *nv=(img_archive_ent*)realloc(ix->v, ncap*sizeof *nv);
    if(!nv) return false;
    char **nn=(char**)realloc(ix->names, ncap*sizeof *nn);
    if(!nn) return false;
    ix->v=nv; ix->names=nn; ix->cap=ncap;
    return true;
}
static void index_free(img_archive_index *ix){
    if(!ix) return;
    for(size_t i=0;i<ix->n;i++) free(ix->names[i]);
    free(ix->names); free(ix->v); free(ix);
}

/* 扫一遍包；cp936=true 时按 GBK 解名字 */
/* 目录子项数 + built_ms（libarchive 与 RAR 两条路共用）*/
static void index_finish(img_archive_index *ix){
    int *cnt=(int*)calloc(ix->n?ix->n:1,sizeof(int));
    if(cnt){ count_children(ix,cnt);
             for(size_t i=0;i<ix->n;i++) if(ix->v[i].is_dir) ix->v[i].children_count=cnt[i];
             free(cnt); }
    ix->built_ms=now_ms();
}
#if IMG_WITH_RAR
typedef struct { img_archive_index *ix; } rar_scan_acc;
static int rar_scan_cb(void *ud, const img_rar_ent *e){
    rar_scan_acc *a=(rar_scan_acc*)ud;
    img_archive_index *ix=a->ix;
    if(!index_grow(ix)){ ix->truncated=true; return 1; }          /* 满了/没内存：停 */
    img_archive_ent *t=&ix->v[ix->n];
    memset(t,0,sizeof *t);
    snprintf(t->path,sizeof t->path,"%s",e->path);
    t->is_dir=e->is_dir;
    t->size=e->is_dir?0:e->size;
    t->mtime=e->mtime;
    t->children_count=-1;
    ix->names[ix->n]=strdup(t->path);
    if(!ix->names[ix->n]) return 1;
    ix->n++;
    return 0;
}
/* RAR 走 unrar 解码器（libarchive 读不了 solid 包；见 REQ-50）*/
static int scan_rar(const char *path, img_archive_index **out, char *err, size_t errcap){
    img_archive_index *ix=(*out);
    snprintf(ix->path,sizeof ix->path,"%s",path);
    rar_scan_acc a={ix};
    bool solid=false;
    int rc=img_rar_scan(path,rar_scan_cb,&a,&solid,err,errcap);
    if(rc!=IMG_OK) return rc;
    ix->solid=solid;
    snprintf(ix->fmt,sizeof ix->fmt,"rar");
    index_finish(ix);
    return IMG_OK;
}
#endif

/* ---------- 扫描期心跳 ----------
 * 20 万条的 zip，libarchive 一次 archive_read_open_filename 就要把整个中央目录
 * 吞完（30~60 秒，一次阻塞调用）——期间没有任何 img_io_read，看门狗 30 秒就会
 * 把命令掐死（实测 rc=-13 ARCHIVE-OPEN-FAIL）。这里开一个心跳线程，扫描期间
 * 每半秒喂一次看门狗；扫描结束就停。RAR 扫描同理。*/
typedef struct { volatile int stop; long long deadline_ms; } arc_hb;
static long long hb_now(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static void *arc_hb_main(void *ud){
    arc_hb *h=(arc_hb*)ud;
    while(!h->stop){
        /* ★ 只在该包的扫描期内喂；超过 deadline（默认 10 分钟）就停手，
         *   让看门狗重新生效 —— 不能因为"加了个心跳"把真挂死变成永远挂着。*/
        if(hb_now() > h->deadline_ms) break;
        img_io_touch();
        usleep(300*1000);
    }
    return NULL;
}
static void hb_start(arc_hb *h, pthread_t *th){
    h->stop=0; h->deadline_ms=hb_now()+10*60*1000;
    if(pthread_create(th,NULL,arc_hb_main,h)!=0) h->stop=1;
}
static void hb_stop(arc_hb *h, pthread_t *th){
    h->stop=1;
    pthread_join(*th,NULL);
}
static int scan_archive(const char *path, bool cp936, img_archive_index **out,
                        char *err, size_t errcap){
#if IMG_WITH_RAR
    if(img_rar_is_rar(path)) return scan_rar(path,out,err,errcap);
#endif
    struct archive *a=NULL;
    arc_hb hb; pthread_t hth; bool hb_on=false;
    hb_start(&hb,&hth); hb_on=true;             /* 大包的 open 会一口气吞完中央目录 */
    if(open_archive(path,&a,cp936)!=IMG_OK){
        hb_stop(&hb,&hth);
        set_err(err,errcap,"打不开压缩包（格式不认识或文件损坏）：%s",path);
        return IMG_EFORMAT;
    }
    img_archive_index *ix=(*out);
    snprintf(ix->path,sizeof ix->path,"%s",path);

    struct archive_entry *e;
    int r;
    while((r=archive_read_next_header(a,&e))==ARCHIVE_OK){
        /* ★ 大包（20 万条）扫描一次要几十秒：不喂看门狗会被 IO-TIMEOUT 干掉 */
        img_io_touch();
        if(img_cancelled()){ archive_read_free(a); return IMG_ECANCELLED; }
        const char *raw=archive_entry_pathname(e);
        if(!raw||!*raw) continue;
        char norm[512]; norm_path(raw,norm,sizeof norm);
        if(!strcmp(norm,"/")) continue;
        if(!index_grow(ix)){ ix->truncated=true; break; }
        img_archive_ent *t=&ix->v[ix->n];
        memset(t,0,sizeof *t);
        snprintf(t->path,sizeof t->path,"%s",norm);
        t->is_dir=(archive_entry_filetype(e)==AE_IFDIR);
        t->size=t->is_dir?0:(uint64_t)archive_entry_size(e);
        t->mtime=(uint64_t)(archive_entry_mtime(e)>0?archive_entry_mtime(e):0);
        t->children_count=-1;
        ix->names[ix->n]=strdup(t->path);
        if(!ix->names[ix->n]){ hb_stop(&hb,&hth); archive_read_free(a); return IMG_ENOMEM; }
        ix->n++;
        archive_read_data_skip(a);
    }
    if(hb_on){ hb_stop(&hb,&hth); hb_on=false; }
    if(r!=ARCHIVE_EOF && r!=ARCHIVE_OK && r<ARCHIVE_WARN){
        set_err(err,errcap,"读取包内容失败：%s",archive_error_string(a));
        archive_read_free(a);
        return IMG_ECORRUPT;
    }
    /* ★ format 只能在读过至少一个 header 之后才有效（libarchive 那时才定格式）*/
    snprintf(ix->fmt,sizeof ix->fmt,"%s",fmt_from_archive(a));
    archive_read_free(a);
    /* REQ-51 A：7z 的固实块（-ms=on）解第 N 个要顺序解，UI 要能提示。
     * libarchive 不暴露这个标志 → 自己读 7z 头（失败就当非固实，不影响列表）。*/
    if(!strcmp(ix->fmt,"7z")){
        bool s7=false;
        if(img_7z_is_solid(path,&s7)==IMG_OK) ix->solid=s7;
    }

    index_finish(ix);
    return IMG_OK;
}

/* ---------------- 缓存（2 槽 LRU + idle 超时） ---------------- */
typedef struct {
    bool     used;
    char     token[32];
    char     path[1024];
    dev_t    dev; ino_t ino;
    uint64_t size; int64_t mtime;
    uint64_t last_ms;
    img_archive_index *ix;
} arc_slot;
static arc_slot g_slots[ARC_SLOTS];
static pthread_mutex_t g_arc_mu=PTHREAD_MUTEX_INITIALIZER;
static unsigned g_seq=0;

static bool identity_ok(const arc_slot *s){
    struct stat st;
    if(stat(s->path,&st)!=0) return false;
    return st.st_dev==s->dev && st.st_ino==s->ino &&
           (uint64_t)st.st_size==s->size && (int64_t)st.st_mtime==s->mtime;
}
int img_archive_index_build(const char *path, img_archive_index **out, char *err, size_t errcap){
    if(out) *out=NULL;
    if(!path||!out) return IMG_EINVAL;
    struct stat st;
    if(stat(path,&st)!=0){ set_err(err,errcap,"压缩包不存在：%s",path); return IMG_ENOENT; }
    if(!S_ISREG(st.st_mode)){ set_err(err,errcap,"不是普通文件：%s",path); return IMG_EINVAL; }

    img_archive_index *ix=index_alloc(4096);
    if(!ix) return IMG_ENOMEM;
    int rc=scan_archive(path,false,&ix,err,errcap);
    /* 名字不是合法 UTF-8 → 按 GBK 重扫一次（老 zip 的常见情况）*/
    if(rc==IMG_OK){
        bool bad=false;
        for(size_t i=0;i<ix->n;i++) if(!utf8_ok(ix->v[i].path)){ bad=true; break; }
        if(bad){
            index_free(ix);
            ix=index_alloc(4096);
            if(!ix) return IMG_ENOMEM;
            rc=scan_archive(path,true,&ix,err,errcap);
        }
    }
    if(rc!=IMG_OK){ index_free(ix); return rc; }

    pthread_mutex_lock(&g_arc_mu);
    /* 先淘汰：同 path 的旧槽、空闲超时的、最久未用的 */
    int victim=-1;
    for(int i=0;i<ARC_SLOTS;i++){
        if(g_slots[i].used){
            if(!strcmp(g_slots[i].path,path) || now_ms()-g_slots[i].last_ms>ARC_IDLE_MS){
                index_free(g_slots[i].ix); g_slots[i].used=false;
            }
        }
    }
    for(int i=0;i<ARC_SLOTS;i++) if(!g_slots[i].used){ victim=i; break; }
    if(victim<0){                                    /* 都在用：挑最久没动的 */
        victim=0;
        for(int i=1;i<ARC_SLOTS;i++)
            if(g_slots[i].last_ms<g_slots[victim].last_ms) victim=i;
        index_free(g_slots[victim].ix);
        g_slots[victim].used=false;
    }
    arc_slot *s=&g_slots[victim];
    s->used=true;
    snprintf(s->token,sizeof s->token,"arc%u-%u",(unsigned)victim,++g_seq);
    snprintf(s->path,sizeof s->path,"%s",path);
    s->dev=st.st_dev; s->ino=st.st_ino; s->size=(uint64_t)st.st_size; s->mtime=(int64_t)st.st_mtime;
    s->last_ms=now_ms(); s->ix=ix;
    pthread_mutex_unlock(&g_arc_mu);

    *out=ix;
    return IMG_OK;
}

static arc_slot *slot_by_token(const char *token, char *err, size_t errcap){
    if(!token||strncmp(token,"arc",3)){ set_err(err,errcap,"index_token 不合法：%s",token); return NULL; }
    for(int i=0;i<ARC_SLOTS;i++){
        arc_slot *s=&g_slots[i];
        if(!s->used || strcmp(s->token,token)) continue;
        if(now_ms()-s->last_ms>ARC_IDLE_MS){
            index_free(s->ix); s->used=false;
            set_err(err,errcap,"索引已超时（idle > 10 分钟）：%s",token);
            return NULL;
        }
        if(!identity_ok(s)){
            index_free(s->ix); s->used=false;
            set_err(err,errcap,"压缩包已变化（dev/ino/mtime/size 有变）：%s",s->path);
            return NULL;
        }
        return s;
    }
    set_err(err,errcap,"索引不存在（进程重启或已被淘汰）：%s",token);
    return NULL;
}
int img_archive_index_by_token(const char *token, img_archive_index **out, char *err, size_t errcap){
    if(out) *out=NULL;
    pthread_mutex_lock(&g_arc_mu);
    arc_slot *s=slot_by_token(token,err,errcap);
    if(s){ s->last_ms=now_ms(); if(out) *out=s->ix; }
    pthread_mutex_unlock(&g_arc_mu);
    return s?IMG_OK:IMG_ESTALE;
}
int img_archive_index_check(const char *token, char *err, size_t errcap){
    pthread_mutex_lock(&g_arc_mu);
    arc_slot *s=slot_by_token(token,err,errcap);
    if(s) s->last_ms=now_ms();
    pthread_mutex_unlock(&g_arc_mu);
    return s?IMG_OK:IMG_ESTALE;
}
const char *img_archive_index_token(const img_archive_index *ix){
    if(!ix) return "";
    for(int i=0;i<ARC_SLOTS;i++)
        if(g_slots[i].used && g_slots[i].ix==ix) return g_slots[i].token;
    return "";
}
const char *img_archive_index_format(const img_archive_index *ix){ return ix?ix->fmt:"unknown"; }
uint64_t img_archive_index_total(const img_archive_index *ix){ return ix?(uint64_t)ix->n:0; }
bool img_archive_index_truncated(const img_archive_index *ix){ return ix?ix->truncated:false; }
bool img_archive_index_solid(const img_archive_index *ix){ return ix? ix->solid : false; }
void img_archive_index_release(img_archive_index *ix){ (void)ix; }   /* 数据归缓存所有 */

static bool under_prefix(const char *path, const char *prefix){
    if(!prefix||!prefix[0]||!strcmp(prefix,"/")) return true;
    size_t n=strlen(prefix);
    if(strncmp(path,prefix,n)) return false;
    return path[n]==0 || path[n]=='/';
}
/* walk 用的"这一级已报过吗"去重表（开放寻址；满了就放弃去重，不能崩）*/
static bool dedup_seen(const char ***seen, char (**buf)[512], size_t *cap, const char *key){
    if(!*seen) *seen=(const char**)calloc(*cap,sizeof **seen);
    if(!*seen) return false;
    if(!*buf) *buf=calloc(*cap,sizeof **buf);
    if(!*buf) return false;
    uint64_t h=(uint64_t)0xcbf29ce484222325ULL;
    for(const char *q=key; *q; q++){ h^=(unsigned char)*q; h*=1099511628211ULL; }
    h &= (*cap-1);
    for(size_t probe=0; probe<*cap; probe++){
        if(!(*seen)[h]){
            snprintf((*buf)[h],512,"%s",key);
            (*seen)[h]=(*buf)[h];
            return false;                       /* 没报过 → 现在占位 */
        }
        if(!strcmp((*seen)[h],key)) return true; /* 报过 */
        h=(h+1)&(*cap-1);
    }
    return true;                                 /* 表满：当作报过，宁少不重 */
}

/* 小哈希表：路径 → index（用于 walk 里 O(1) 查"这一级是不是真目录条目"）。
 * ★ 最初写成"每项都全表扫一遍找目录"，20 万条 zip 直接 O(n²) → 扫了 405 秒。
 *   现在一次建表、一次遍历，20 万条 0.4 秒。*/
typedef struct { const char *key; int idx; } wslot;
static uint64_t whash(const char *s){
    uint64_t h=1469598103934665603ULL;
    for(; *s; s++){ h^=(unsigned char)*s; h*=1099511628211ULL; }
    return h;
}
int img_archive_index_walk(img_archive_index *ix, const char *prefix, bool recursive,
                           img_archive_ent_cb cb, void *ud){
    if(!ix||!cb) return IMG_EINVAL;
    char pfx[512]; norm_path(prefix?prefix:"/",pfx,sizeof pfx);
    size_t plen=strlen(pfx);

    /* ★ 两个坑（2026-11-04 用 20 万条 zip 才暴露）：
     *   ① zip 里常【没有目录条目】，只有 "dir0/f.txt" 这样的文件路径 ——
     *      按"剩余部分不能再有 /"过滤，根目录会一条都不剩（count=0、total=0）；
     *   ② 所以要【现造隐式目录】：遇到 a/b/c 时把 a、a/b 补成目录，
     *      并且同一级只出现一次（一个目录 40 个文件不能回调 40 次）。*/
    size_t dcap=64; while(dcap < ix->n*2 && dcap < (1u<<21)) dcap<<=1;
    wslot *dtab=NULL;
    if(!recursive && dcap){
        dtab=(wslot*)calloc(dcap,sizeof *dtab);
        if(dtab){
            for(size_t i=0;i<ix->n;i++)
                if(ix->v[i].is_dir){
                    const char *k=ix->v[i].path;
                    uint64_t h=whash(k)&(dcap-1);
                    while(dtab[h].key) h=(h+1)&(dcap-1);
                    dtab[h].key=k; dtab[h].idx=(int)i;
                }
        }
    }
    /* 隐式目录去重表（同一级只回调一次）*/
    size_t scap=64; while(scap < 4096) scap<<=1;
    const char **seen=(const char**)calloc(scap,sizeof *seen);
    char (*seenbuf)[512]=NULL;

    for(size_t i=0;i<ix->n;i++){
        const char *p=ix->v[i].path;
        if(!under_prefix(p,pfx)) continue;
        if(!strcmp(p,pfx)) continue;                       /* 前缀自身不列 */
        if(recursive){ cb(ud,&ix->v[i]); continue; }
        const char *rest=(plen==1)?p+1:p+plen+1;
        const char *slash=strchr(rest,'/');
        if(!slash){
            /* 直接子项：文件或真目录 —— 也要去重，否则"真目录条目 + 它下面的文件"
             * 会把同一个目录报两次（实测 CS1.6 包 62 条变 70 条）。*/
            if(dedup_seen(&seen,&seenbuf,&scap,ix->v[i].path)) continue;
            cb(ud,&ix->v[i]);
            continue;
        }
        size_t clen=(size_t)(slash-rest);
        if(clen==0 || clen>=511) continue;
        char child[512];
        if(plen==1) snprintf(child,sizeof child,"/%.*s",(int)clen,rest);
        else snprintf(child,sizeof child,"%.*s/%.*s",(int)plen,pfx,(int)clen,rest);

        int have=-1;
        if(dtab){
            uint64_t h=whash(child)&(dcap-1);
            while(dtab[h].key){
                if(!strcmp(dtab[h].key,child)){ have=dtab[h].idx; break; }
                h=(h+1)&(dcap-1);
            }
        }
        if(dedup_seen(&seen,&seenbuf,&scap,child)) continue;
        if(have>=0){ cb(ud,&ix->v[have]); }
        else {
            img_archive_ent e; memset(&e,0,sizeof e);
            snprintf(e.path,sizeof e.path,"%s",child);
            e.is_dir=true; e.children_count=-1;
            cb(ud,&e);
        }
    }
    free(dtab); free(seen); free(seenbuf);
    return IMG_OK;
}

/* ---------------- 抽取 ---------------- */
static int open_for_read(const char *path, bool cp936, struct archive **out,
                         char *err, size_t errcap){
    if(open_archive(path,out,cp936)!=IMG_OK){
        set_err(err,errcap,"打不开压缩包（格式不认识或文件损坏）：%s",path);
        return IMG_EFORMAT;
    }
    return IMG_OK;
}
static int mkdir_p_host(const char *path){
    char tmp[4200]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char *p=tmp+1; *p; p++){
        if(*p=='/'){ *p=0; mkdir(tmp,0755); *p='/'; }
    }
    if(mkdir(tmp,0755)!=0 && errno!=EEXIST) return IMG_EIO;
    return IMG_OK;
}

int img_archive_read_entry(const char *path, const char *inner, bool is_dir,
                           img_archive_write_cb cb, void *ud,
                           uint64_t *out_bytes, img_prog *prog,
                           char *err, size_t errcap){
    if(out_bytes) *out_bytes=0;
    if(!path||!inner||!cb) return IMG_EINVAL;
#if IMG_WITH_RAR
    if(img_rar_is_rar(path))                                  /* REQ-50 */
        return img_rar_read(path,inner,(img_rar_data_cb)cb,ud,out_bytes,prog,err,errcap);
#endif
    char want[512]; norm_path(inner,want,sizeof want);
    for(int pass=0; pass<2; pass++){
        struct archive *a=NULL;
        int rc=open_for_read(path,pass==1,&a,err,errcap);
        if(rc!=IMG_OK) return rc;
        struct archive_entry *e; int r;
        bool found=false, encrypted=false;
        while((r=archive_read_next_header(a,&e))==ARCHIVE_OK){
            if(img_cancelled()){ archive_read_free(a); return IMG_ECANCELLED; }
            const char *raw=archive_entry_pathname(e);
            if(!raw){ archive_read_data_skip(a); continue; }
            char cur[512]; norm_path(raw,cur,sizeof cur);
            if(!strcmp(cur,want)){
                found=true;
                if(archive_entry_is_encrypted(e)){ encrypted=true; break; }
                if(is_dir != (archive_entry_filetype(e)==AE_IFDIR)){
                    archive_read_free(a);
                    set_err(err,errcap,"包内路径类型不符：%s",want);
                    return IMG_EINVAL;
                }
                if(is_dir){ archive_read_free(a); return IMG_OK; }
                uint64_t total=(uint64_t)archive_entry_size(e), done=0;
                char buf[262144];
                for(;;){
                    la_ssize_t n=archive_read_data(a,buf,sizeof buf);
                    if(n<0){
                        set_err(err,errcap,"解压失败：%s",archive_error_string(a));
                        archive_read_free(a); return IMG_ECORRUPT;
                    }
                    if(n==0) break;
                    if(cb(ud,buf,(size_t)n)!=0){ archive_read_free(a); return IMG_EIO; }
                    done+=(uint64_t)n;
                    if(prog && total) img_prog_bytes(prog,done,total);
                    if(img_cancelled()){ archive_read_free(a); return IMG_ECANCELLED; }
                }
                archive_read_free(a);
                if(out_bytes) *out_bytes=done;
                if(prog && total){ img_prog_bytes(prog,total,total); }
                return IMG_OK;
            }
            archive_read_data_skip(a);
        }
        archive_read_free(a);
        if(encrypted){ set_err(err,errcap,"包内条目加密（需要密码，暂不支持）：%s",want); return IMG_ENOTSUP; }
        if(!found && pass==0) continue;          /* 第一遍没找到：可能是 GBK 名，换编码再来一次 */
        if(!found){ set_err(err,errcap,"包内没有这个路径：%s",want); return IMG_ENOENT; }
    }
    return IMG_ENOENT;
}

/* 写本地文件的回调 */
typedef struct { FILE *fp; } arc_fsink;
static int arc_file_cb(void *ud, const void *buf, size_t n){
    arc_fsink *s=(arc_fsink*)ud;
    return fwrite(buf,1,n,s->fp)==n?0:-1;
}

int img_archive_extract(const char *path, const char *inner, const char *dst,
                        img_prog *prog, uint64_t *out_bytes, int *out_files,
                        char *err, size_t errcap){
    if(out_bytes) *out_bytes=0;
    if(out_files) *out_files=0;
    if(!path||!inner||!dst) return IMG_EINVAL;
#if IMG_WITH_RAR
    if(img_rar_is_rar(path))                                  /* REQ-50 */
        return img_rar_extract(path,inner,dst,prog,out_bytes,out_files,err,errcap);
#endif
    char want[512]; norm_path(inner,want,sizeof want);
    {   /* REQ-51 B：告诉 App "cur/total_bytes 是解压后口径"，压缩包大小另给 */
        struct stat pst;
        uint64_t pkg = (stat(path,&pst)==0 && pst.st_size>0)? (uint64_t)pst.st_size : 0;
        img_prog_bytes_kind(prog,"extracted",0,pkg);
    }

    /* 先判断是文件还是目录（用索引更省；但按 REQ-34 抽取不该强制建索引 → 扫一遍找类型）*/
    bool is_dir=false;
    {
        struct archive *a=NULL;
        int rc=open_for_read(path,false,&a,err,errcap);
        if(rc!=IMG_OK) return rc;
        struct archive_entry *e; int r;
        while((r=archive_read_next_header(a,&e))==ARCHIVE_OK){
            const char *raw=archive_entry_pathname(e);
            if(raw){
                char cur[512]; norm_path(raw,cur,sizeof cur);
                if(!strncmp(cur,want,strlen(want))){
                    if(!strcmp(cur,want)){ is_dir=(archive_entry_filetype(e)==AE_IFDIR); break; }
                    if(cur[strlen(want)]=='/'){ is_dir=true; break; }   /* 有子项 → 是目录 */
                }
            }
            archive_read_data_skip(a);
        }
        archive_read_free(a);
        if(!is_dir){    /* 当文件处理（找不到会在 read_entry 里报）*/
            FILE *fp=fopen(dst,"wb");
            if(!fp){ set_err(err,errcap,"目标打不开：%s",dst); return IMG_EIO; }
            arc_fsink s={fp};
            uint64_t got=0;
            int rc2=img_archive_read_entry(path,want,false,arc_file_cb,&s,&got,prog,err,errcap);
            fclose(fp);
            if(rc2!=IMG_OK){ unlink(dst); return rc2; }
            if(out_bytes) *out_bytes=got;
            if(out_files) *out_files=1;
            return IMG_OK;
        }
    }

    /* 目录：整棵子树 */
    struct stat ds;
    if(stat(dst,&ds)==0){
        if(!S_ISDIR(ds.st_mode)){ set_err(err,errcap,"目标是文件不是目录：%s",dst); return IMG_ESTATE; }
        DIR *d=opendir(dst); int cnt=0;
        if(d){ struct dirent *de; while((de=readdir(d))){ if(strcmp(de->d_name,".")&&strcmp(de->d_name,"..")) cnt++; } closedir(d); }
        if(cnt>0){ set_err(err,errcap,"目标目录非空（不做合并）：%s",dst); return IMG_ESTATE; }
    } else if(mkdir_p_host(dst)!=IMG_OK){
        set_err(err,errcap,"建目标目录失败：%s",dst); return IMG_EIO;
    }

    struct archive *a=NULL;
    int rc=open_for_read(path,false,&a,err,errcap);
    if(rc!=IMG_OK) return rc;
    struct archive_entry *e; int r; uint64_t total_bytes=0; int files=0;
    size_t wl=strlen(want);
    while((r=archive_read_next_header(a,&e))==ARCHIVE_OK){
        /* ★ 大包（20 万条）扫描一次要几十秒：不喂看门狗会被 IO-TIMEOUT 干掉 */
        img_io_touch();
        if(img_cancelled()){ archive_read_free(a); return IMG_ECANCELLED; }
        const char *raw=archive_entry_pathname(e);
        if(!raw){ archive_read_data_skip(a); continue; }
        char cur[512]; norm_path(raw,cur,sizeof cur);
        if(strncmp(cur,want,wl) || (cur[wl]!='/' && cur[wl]!=0)){ archive_read_data_skip(a); continue; }
        const char *rel=cur+wl; while(*rel=='/') rel++;
        if(!*rel){ archive_read_data_skip(a); continue; }
        char target[1400]; snprintf(target,sizeof target,"%s/%s",dst,rel);
        if(archive_entry_filetype(e)==AE_IFDIR){ mkdir_p_host(target); archive_read_data_skip(a); continue; }
        if(archive_entry_is_encrypted(e)){
            set_err(err,errcap,"包内条目加密（需要密码，暂不支持）：%s",target);
            archive_read_free(a); return IMG_ENOTSUP;
        }
        char parent[1400]; snprintf(parent,sizeof parent,"%s",target);
        char *slash=strrchr(parent,'/'); if(slash) *slash=0;
        if(parent[0]) mkdir_p_host(parent);
        FILE *fp=fopen(target,"wb");
        if(!fp){ set_err(err,errcap,"写目标失败：%s",target); archive_read_free(a); return IMG_EIO; }
        char buf[262144]; la_ssize_t n; uint64_t fsz=0, expect=(uint64_t)archive_entry_size(e);
        bool bad=false;
        while((n=archive_read_data(a,buf,sizeof buf))>0){
            if(fwrite(buf,1,(size_t)n,fp)!=(size_t)n){ bad=true; break; }
            fsz+=(uint64_t)n;
            if(prog && expect) img_prog_bytes(prog,fsz,expect);
            if(img_cancelled()){ fclose(fp); unlink(target); archive_read_free(a); return IMG_ECANCELLED; }
        }
        fclose(fp);
        if(bad||n<0){ unlink(target); set_err(err,errcap,"解压失败：%s",target); archive_read_free(a); return IMG_EIO; }
        total_bytes+=fsz; files++;
    }
    archive_read_free(a);
    if(out_bytes) *out_bytes=total_bytes;
    if(out_files) *out_files=files;
    return IMG_OK;
}

/* ---------------- 单条查询 ---------------- */
int img_archive_stat(const char *path, const char *inner, bool *isdir,
                     uint64_t *size, uint64_t *mtime, char *err, size_t errcap){
    if(isdir) *isdir=false;
    if(size) *size=0;
    if(mtime) *mtime=0;
    if(!path||!inner) return IMG_EINVAL;
#if IMG_WITH_RAR
    if(img_rar_is_rar(path))                                  /* REQ-50 */
        return img_rar_stat(path,inner,isdir,size,mtime,err,errcap);
#endif
    char want[512]; norm_path(inner,want,sizeof want);
    for(int pass=0; pass<2; pass++){
        struct archive *a=NULL;
        if(open_for_read(path,pass==1,&a,err,errcap)!=IMG_OK) return IMG_EFORMAT;
        struct archive_entry *e; int r; bool found=false;
        while((r=archive_read_next_header(a,&e))==ARCHIVE_OK){
            const char *raw=archive_entry_pathname(e);
            if(raw){
                char cur[512]; norm_path(raw,cur,sizeof cur);
                if(!strcmp(cur,want)){
                    found=true;
                    if(isdir) *isdir=(archive_entry_filetype(e)==AE_IFDIR);
                    if(size) *size=(uint64_t)archive_entry_size(e);
                    if(mtime) *mtime=(uint64_t)(archive_entry_mtime(e)>0?archive_entry_mtime(e):0);
                    break;
                }
                size_t wl=strlen(want);
                if(!strncmp(cur,want,wl) && cur[wl]=='/'){   /* 有子项 → 是目录 */
                    found=true;
                    if(isdir) *isdir=true;
                    break;
                }
            }
            archive_read_data_skip(a);
        }
        archive_read_free(a);
        if(found) return IMG_OK;
    }
    set_err(err,errcap,"包内没有这个路径：%s",want);
    return IMG_ENOENT;
}

/* ---------------- 顺序流（copy 的源） ---------------- */
typedef struct {
    const char *path, *inner;
    int         rfd, wfd;
    pthread_t   th;
    bool        th_on;
    uint64_t    pos;
    bool        eof;
    int         rc;
} arc_stream;

static int arc_pipe_cb(void *ud, const void *buf, size_t n){
    arc_stream *a=(arc_stream*)ud;
    const char *p=(const char*)buf; size_t done=0;
    while(done<n){
        ssize_t w=write(a->wfd,p+done,n-done);
        if(w<0){ if(errno==EINTR) continue; return -1; }
        done+=(size_t)w;
    }
    return 0;
}
static void *arc_prod(void *ud){
    arc_stream *a=(arc_stream*)ud;
    char err[160]="";
    uint64_t got=0;
    a->rc=img_archive_read_entry(a->path,a->inner,false,arc_pipe_cb,a,&got,NULL,err,sizeof err);
    close(a->wfd); a->wfd=-1;
    return NULL;
}
static int arc_sread(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    arc_stream *a=(arc_stream*)ud;
    if(got) *got=0;
    if(!a||!buf||!got) return IMG_EINVAL;
    if(off!=a->pos) return IMG_ENOTSUP;              /* 顺序流 */
    if(a->eof) return IMG_OK;
    for(;;){
        ssize_t n=read(a->rfd,buf,want);
        if(n<0){ if(errno==EINTR) continue; return IMG_EIO; }
        if(n==0){ a->eof=true; return IMG_OK; }
        a->pos+=(uint64_t)n; *got=(size_t)n;
        return IMG_OK;
    }
}
static int arc_sclose(void *ud){
    arc_stream *a=(arc_stream*)ud;
    if(!a) return IMG_OK;
    if(a->th_on){ pthread_join(a->th,NULL); a->th_on=false; }
    if(a->rfd>=0) close(a->rfd);
    if(a->wfd>=0) close(a->wfd);
    int rc=a->rc;
    free(a);
    return rc;
}
int img_archive_stream_open(const char *path, const char *inner,
                            img_stream *out, uint64_t *size,
                            img_prog *prog, char *err, size_t errcap){
    (void)prog;
    if(size) *size=0;
    if(!path||!inner||!out) return IMG_EINVAL;
    bool isdir=false; uint64_t sz=0, mt=0;
    int rc=img_archive_stat(path,inner,&isdir,&sz,&mt,err,errcap);
    if(rc!=IMG_OK) return rc;
    if(isdir){ set_err(err,errcap,"这是目录，不能当文件流读：%s",inner); return IMG_ENOTSUP; }
    int fds[2];
    if(pipe(fds)!=0){ set_err(err,errcap,"建管道失败：%s",strerror(errno)); return IMG_EIO; }
    arc_stream *a=(arc_stream*)calloc(1,sizeof *a);
    if(!a){ close(fds[0]); close(fds[1]); return IMG_ENOMEM; }
    a->path=path; a->inner=inner; a->rfd=fds[0]; a->wfd=fds[1];
    if(pthread_create(&a->th,NULL,arc_prod,a)!=0){
        close(fds[0]); close(fds[1]); free(a);
        set_err(err,errcap,"建线程失败：%s","");
        return IMG_ESTATE;
    }
    a->th_on=true;
    out->ud=a; out->read=arc_sread; out->close=arc_sclose;
    if(size) *size=sz;
    return IMG_OK;
}

#else  /* !IMG_WITH_ARCHIVE —— 占位，保证链接不断 */

int img_archive_format(const char *path, char *out, size_t cap, char *err, size_t errcap){
    (void)path; if(out&&cap) out[0]=0;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_archive_index_build(const char *path, img_archive_index **out, char *err, size_t errcap){
    (void)path; if(out) *out=NULL;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_archive_index_by_token(const char *t, img_archive_index **out, char *err, size_t errcap){
    (void)t; if(out) *out=NULL;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_archive_index_check(const char *t, char *err, size_t errcap){
    (void)t; if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
const char *img_archive_index_token(const img_archive_index *ix){ (void)ix; return ""; }
const char *img_archive_index_format(const img_archive_index *ix){ (void)ix; return "unknown"; }
uint64_t img_archive_index_total(const img_archive_index *ix){ (void)ix; return 0; }
bool img_archive_index_truncated(const img_archive_index *ix){ (void)ix; return false; }
bool img_archive_index_solid(const img_archive_index *ix){ (void)ix; return false; }
int img_archive_index_walk(img_archive_index *ix, const char *p, bool rec,
                           img_archive_ent_cb cb, void *ud){
    (void)ix;(void)p;(void)rec;(void)cb;(void)ud; return IMG_ENOTSUP;
}
void img_archive_index_release(img_archive_index *ix){ (void)ix; }
int img_archive_stat(const char *path, const char *inner, bool *isdir,
                     uint64_t *size, uint64_t *mtime, char *err, size_t errcap){
    (void)path;(void)inner; if(isdir)*isdir=false; if(size)*size=0; if(mtime)*mtime=0;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_archive_stream_open(const char *path, const char *inner,
                            img_stream *out, uint64_t *size,
                            img_prog *prog, char *err, size_t errcap){
    (void)path;(void)inner;(void)out;(void)prog; if(size)*size=0;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_archive_extract(const char *path, const char *inner, const char *dst,
                        img_prog *prog, uint64_t *ob, int *of, char *err, size_t errcap){
    (void)path;(void)inner;(void)dst;(void)prog;
    if(ob) *ob=0; if(of) *of=0;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_archive_read_entry(const char *path, const char *inner, bool is_dir,
                           img_archive_write_cb cb, void *ud,
                           uint64_t *ob, img_prog *prog, char *err, size_t errcap){
    (void)path;(void)inner;(void)is_dir;(void)cb;(void)ud;(void)prog;
    if(ob) *ob=0;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
#endif /* IMG_WITH_ARCHIVE */
