/* img_lswalk.c — 见 img_lswalk.h（REQ-83：统一路径上的递归遍历 + 子串过滤）
 *
 * 六类后端各走各的「列目录」，但【过滤、路径拼接、深度、取消、进度】只有一份：
 *   HOST    opendir/readdir + lstat
 *   NTFS    img_ntfs_list_dir_full（一次遍历带 size/时间）
 *   FAT32   img_fat32_list_dir_full
 *   ISO     img_iso_list
 *   WIM     img_wim_list（wimlib 自带递归，回调直接给完整路径）
 *   压缩包  img_archive_index_build + walk（索引在内存里，7z/rar 只扫一次）
 *
 * 递归一律用显式栈（不写递归函数）：NDK/PRoot 下深目录会把调用栈吃穿，
 * 而且栈里存的是 2048 字节的路径，一帧就 2 KB —— 64 层就是 128 KB。
 */
#include "img_lswalk.h"
#include "img_cmd.h"
#include "img_partition.h"
#include "img_ntfs_fs.h"
#include "img_fat32_fs.h"
#include "img_iso.h"
#include "img_wim_fs.h"
#include "img_archive.h"
#include "img_event.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(IMG_WITH_WIM)
#define IMG_WITH_WIM 0
#endif
#if !defined(IMG_WITH_NTFS3G)
#define IMG_WITH_NTFS3G 0
#endif

typedef struct { char path[2048]; uint32_t depth; } lw_dirq;

typedef struct {
    img_lswalk_cb cb;
    void         *ud;
    const char   *q;         /* NULL = 不过滤 */
    uint64_t      scanned;
} lw_ctx;

/* 不区分大小写的子串匹配（与 wim.search / iso.find 同一语义） */
static bool lw_sub_ci(const char *hay, const char *needle){
    if(!needle || !*needle) return true;
    if(!hay) return false;
    size_t nl=strlen(needle);
    for(const char *p=hay; *p; p++)
        if(!strncasecmp(p,needle,nl)) return true;
    return false;
}

static void lw_join(const char *dir, const char *name, char *out, size_t cap){
    size_t dl=dir?strlen(dir):0;
    while(dl>1 && dir[dl-1]=='/') dl--;              /* 去掉尾斜杠，避免 // */
    if(dl<=1) snprintf(out,cap,"/%s",name?name:"");
    else      snprintf(out,cap,"%.*s/%s",(int)dl,dir,name?name:"");
}

/* 相对 root 的层数：直接子项=1。WIM/压缩包的回调深度口径不一，统一用这个算。 */
static uint32_t lw_depth_of(const char *root, const char *full){
    size_t rl=(root&&*root)?strlen(root):0;
    const char *s=full?full:"";
    if(rl && rl<=strlen(s) && !strncmp(s,root,rl)) s+=rl;
    while(*s=='/') s++;
    if(!*s) return 0;
    uint32_t d=1;
    for(const char *p=s; *p; p++) if(*p=='/') d++;
    return d;
}

/* 唯一出口：q 过滤在这里做（各后端不许自己过滤，否则口径又分叉） */
static void lw_emit(lw_ctx *c, const char *path, bool is_dir, uint64_t size,
                    uint64_t mtime, uint32_t depth){
    c->scanned++;
    if(c->q && !lw_sub_ci(path, c->q)) return;
    img_lswalk_ent e;
    snprintf(e.path,sizeof e.path,"%s",path?path:"");
    e.is_dir=is_dir;
    e.size=size;
    e.mtime=mtime;
    e.depth=depth;
    c->cb(c->ud,&e);
}

static bool lw_push(lw_dirq **q, int *cap, int *sp, const char *path, uint32_t depth){
    if(*sp>=*cap){
        int nc=*cap?(*cap)*2:64;
        if(nc>(1<<20)) return false;                 /* 100 万目录 ≈ 2 GB，不可能 */
        lw_dirq *nq=(lw_dirq*)realloc(*q,(size_t)nc*sizeof *nq);
        if(!nq) return false;
        *q=nq; *cap=nc;
    }
    snprintf((*q)[*sp].path,sizeof (*q)[*sp].path,"%s",path?path:"");
    (*q)[*sp].depth=depth;
    (*sp)++;
    return true;
}

static int lw_tick(lw_ctx *c, img_prog *prog, const char *dir){
    if(img_cancelled()) return IMG_ECANCELLED;
    if(prog){
        img_prog_item(prog,dir?dir:"");
        img_prog_count(prog,c->scanned,0);
    }
    return IMG_OK;
}

/* ---------------- 宿主目录 ---------------- */
static int lw_host(const img_path *p, bool rec, lw_ctx *c, img_prog *prog,
                   char *err, size_t errcap){
    struct stat st;
    if(lstat(p->host,&st)!=0){
        if(err&&errcap) snprintf(err,errcap,"路径不存在：%s",p->host);
        return IMG_ENOENT;
    }
    if(!S_ISDIR(st.st_mode)){
        lw_emit(c,p->host,false,(uint64_t)st.st_size,(uint64_t)st.st_mtime,0);
        return IMG_OK;
    }
    lw_dirq *q=NULL; int qcap=0, sp=0;
    if(!lw_push(&q,&qcap,&sp,p->host,0)){
        free(q);
        if(err&&errcap) snprintf(err,errcap,"内存不足");
        return IMG_ENOMEM;
    }
    int rc=IMG_OK;
    while(sp>0 && rc==IMG_OK){
        sp--;
        char cur[2048];
        snprintf(cur,sizeof cur,"%s",q[sp].path);          /* ★ 先拷出来：lw_push 会 realloc q */
        uint32_t dep=q[sp].depth;
        if((rc=lw_tick(c,prog,cur))!=IMG_OK) break;
        DIR *d=opendir(cur);
        if(!d){
            /* 根打不开才是错；子目录没权限就跳过（手机目录里这种很常见） */
            if(dep==0){ rc=IMG_ENOENT; if(err&&errcap) snprintf(err,errcap,"目录无法打开：%s（%s）",cur,strerror(errno)); }
            continue;
        }
        struct dirent *de;
        while((de=readdir(d))){
            if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
            if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
            char full[2048];
            lw_join(cur,de->d_name,full,sizeof full);
            struct stat s2;
            if(lstat(full,&s2)!=0) continue;
            bool isd=S_ISDIR(s2.st_mode);
            lw_emit(c,full,isd,(uint64_t)s2.st_size,(uint64_t)s2.st_mtime,dep+1);
            if(isd && rec) lw_push(&q,&qcap,&sp,full,dep+1);
        }
        closedir(d);
    }
    free(q);
    return rc;
}

/* ---------------- NTFS / FAT（qcow2 分区） ---------------- */
#if IMG_WITH_NTFS3G
typedef struct {
    lw_ctx  *c;
    img_ntfs_fs *fs;
    lw_dirq **q; int *sp, *cap;
    char      cur[2048];
    uint32_t  depth;
    bool      rec;
    int       rc;
} lw_ntfs_t;

static void lw_ntfs_cb(void *ud, const img_ntfs_ent_t *e){
    lw_ntfs_t *x=(lw_ntfs_t*)ud;
    if(x->rc!=IMG_OK) return;
    if(img_cancelled()){ x->rc=IMG_ECANCELLED; return; }
    char full[2048];
    lw_join(x->cur,e->name?e->name:"",full,sizeof full);
    lw_emit(x->c,full,e->is_dir,e->size,e->times[1],x->depth+1);
    if(e->is_dir && x->rec && !lw_push(x->q,x->cap,x->sp,full,x->depth+1)) x->rc=IMG_ENOMEM;
}

typedef struct {
    lw_ctx  *c;
    img_fat32_fs *fs;
    lw_dirq **q; int *sp, *cap;
    char      cur[2048];
    uint32_t  depth;
    bool      rec;
    int       rc;
} lw_fat_t;

static void lw_fat_cb(void *ud, const img_fat32_ent_t *e){
    lw_fat_t *x=(lw_fat_t*)ud;
    if(x->rc!=IMG_OK) return;
    if(img_cancelled()){ x->rc=IMG_ECANCELLED; return; }
    char full[2048];
    lw_join(x->cur,e->name?e->name:"",full,sizeof full);
    lw_emit(x->c,full,e->is_dir,e->size,e->mtime,x->depth+1);
    if(e->is_dir && x->rec && !lw_push(x->q,x->cap,x->sp,full,x->depth+1)) x->rc=IMG_ENOMEM;
}
#endif

static int lw_disk(const img_path *p, bool rec, lw_ctx *c, img_prog *prog,
                   char *err, size_t errcap){
#if IMG_WITH_NTFS3G
    (void)rec;                                   /* 递归由下面的显式栈走 */
    img_ctx *ctx=img_ctx_open(p->host,false,false,err,errcap);
    if(!ctx) return IMG_ENOENT;
    img_qcow2 *q=img_ctx_qcow2(ctx);
    img_parttable_t *t=NULL;
    int rc=img_ctx_parttable(ctx,&t);
    uint64_t pb=0;
    if(rc==IMG_OK && t)
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==p->lba){
                pb=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512ULL; break;
            }
    if(!pb){ img_ctx_close(ctx); if(err&&errcap) snprintf(err,errcap,"分区不存在（lba=%llu）",(unsigned long long)p->lba);
             return IMG_ENOENT; }

    if(p->kind==IMG_XK_NTFS){
        img_ntfs_fs *fs=img_ntfs_mount_rw(q,p->lba,pb);
        if(!fs){ img_ctx_close(ctx); if(err&&errcap) snprintf(err,errcap,"不是合法 NTFS 分区");
                 return IMG_ESTATE; }
        uint64_t ino=0; bool isdir=false;
        rc=img_ntfs_verify_path(fs,p->inner,&ino,&isdir);
        if(rc!=IMG_OK){
            img_ntfs_umount(fs); img_ctx_close(ctx);
            if(err&&errcap) snprintf(err,errcap,"NTFS 里没有这个路径：%s",p->inner);
            return rc;
        }
        if(!isdir){
            img_ntfs_ent_t e;
            if(img_ntfs_stat(fs,p->inner,&e)==IMG_OK)
                lw_emit(c,p->inner,false,e.size,e.times[1],0);
        }else{
            lw_dirq *qb=NULL; int qcap=0, sp=0;
            if(!lw_push(&qb,&qcap,&sp,p->inner,0)){
                img_ntfs_umount(fs); img_ctx_close(ctx);
                if(err&&errcap) snprintf(err,errcap,"内存不足");
                return IMG_ENOMEM;
            }
            while(sp>0 && rc==IMG_OK){
                sp--;
                lw_ntfs_t x; memset(&x,0,sizeof x);
                x.c=c; x.fs=fs; x.q=&qb; x.sp=&sp; x.cap=&qcap; x.depth=qb[sp].depth; x.rec=rec; x.rc=IMG_OK;
                snprintf(x.cur,sizeof x.cur,"%s",qb[sp].path);
                if((x.rc=lw_tick(c,prog,x.cur))!=IMG_OK){ rc=x.rc; break; }
                rc=img_ntfs_list_dir_full(fs,x.cur,lw_ntfs_cb,&x);
                if(rc==IMG_OK) rc=x.rc;
                if(rc==IMG_ENOENT && x.depth>0) rc=IMG_OK;   /* 子目录读不到：跳过，不算失败 */
            }
            free(qb);
        }
        img_ntfs_umount(fs);
    }else{
        img_fat32_fs *fs=img_fat32_mount(q,p->lba,false);
        if(!fs){ img_ctx_close(ctx); if(err&&errcap) snprintf(err,errcap,"不是合法 FAT32 分区");
                 return IMG_ESTATE; }
        img_fat32_stat_t st;
        rc=img_fat32_stat(fs,p->inner,&st);
        if(rc!=IMG_OK){
            img_fat32_unmount(fs); img_ctx_close(ctx);
            if(err&&errcap) snprintf(err,errcap,"FAT32 里没有这个路径：%s",p->inner);
            return rc;
        }
        if(!st.is_dir){
            lw_emit(c,p->inner,false,st.size,st.mtime,0);
        }else{
            lw_dirq *qb=NULL; int qcap=0, sp=0;
            if(!lw_push(&qb,&qcap,&sp,p->inner,0)){
                img_fat32_unmount(fs); img_ctx_close(ctx);
                if(err&&errcap) snprintf(err,errcap,"内存不足");
                return IMG_ENOMEM;
            }
            while(sp>0 && rc==IMG_OK){
                sp--;
                lw_fat_t x; memset(&x,0,sizeof x);
                x.c=c; x.fs=fs; x.q=&qb; x.sp=&sp; x.cap=&qcap; x.depth=qb[sp].depth; x.rec=rec; x.rc=IMG_OK;
                snprintf(x.cur,sizeof x.cur,"%s",qb[sp].path);
                if((x.rc=lw_tick(c,prog,x.cur))!=IMG_OK){ rc=x.rc; break; }
                rc=img_fat32_list_dir_full(fs,x.cur,lw_fat_cb,&x);
                if(rc==IMG_OK) rc=x.rc;
                if(rc==IMG_ENOENT && x.depth>0) rc=IMG_OK;
            }
            free(qb);
        }
        img_fat32_unmount(fs);
    }
    img_ctx_close(ctx);
    return rc;
#else
    (void)p; (void)rec; (void)c; (void)prog;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 libntfs-3g");
    return IMG_ENOTSUP;
#endif
}

/* ---------------- ISO ---------------- */
typedef struct {
    lw_ctx *c;
    img_iso *iso;
    lw_dirq **q; int *sp, *cap;
    char cur[2048];
    uint32_t depth;
    bool rec;
    int rc;
} lw_iso_t;

static void lw_iso_cb(void *ud, const char *name, bool is_dir, uint64_t size, uint64_t mtime){
    lw_iso_t *x=(lw_iso_t*)ud;
    if(x->rc!=IMG_OK || !name) return;
    if(img_cancelled()){ x->rc=IMG_ECANCELLED; return; }
    char full[2048];
    lw_join(x->cur,name,full,sizeof full);
    lw_emit(x->c,full,is_dir,size,mtime,x->depth+1);
    if(is_dir && x->rec && !lw_push(x->q,x->cap,x->sp,full,x->depth+1)) x->rc=IMG_ENOMEM;
}

static int lw_iso(const img_path *p, bool rec, lw_ctx *c, img_prog *prog,
                  char *err, size_t errcap){
    img_iso *iso=NULL;
    int rc=img_iso_open_path(&iso,p->host,err,errcap);
    if(rc!=IMG_OK) return rc;
    uint64_t sz=0; bool isdir=false;
    rc=img_iso_stat(iso,p->inner,&sz,&isdir);
    if(rc!=IMG_OK){ img_iso_close(iso); if(err&&errcap) snprintf(err,errcap,"ISO 里没有这个路径：%s",p->inner);
                    return rc; }
    if(!isdir){
        lw_emit(c,p->inner,false,sz,0,0);
        img_iso_close(iso);
        return IMG_OK;
    }
    lw_dirq *q=NULL; int qcap=0, sp=0;
    if(!lw_push(&q,&qcap,&sp,p->inner,0)){ img_iso_close(iso); if(err&&errcap) snprintf(err,errcap,"内存不足"); return IMG_ENOMEM; }
    while(sp>0 && rc==IMG_OK){
        sp--;
        lw_iso_t x; memset(&x,0,sizeof x);
        x.c=c; x.iso=iso; x.q=&q; x.sp=&sp; x.cap=&qcap; x.depth=q[sp].depth; x.rec=rec; x.rc=IMG_OK;
        snprintf(x.cur,sizeof x.cur,"%s",q[sp].path);
        if((x.rc=lw_tick(c,prog,x.cur))!=IMG_OK){ rc=x.rc; break; }
        int n=img_iso_list(iso,x.cur,lw_iso_cb,&x);
        if(n<0) rc=(x.depth>0)?IMG_OK:(int)n;
        else rc=x.rc;
    }
    free(q);
    img_iso_close(iso);
    return rc;
}

/* ---------------- WIM/ESD ---------------- */
#if IMG_WITH_WIM
typedef struct { lw_ctx *c; const char *root; const char *self; } lw_wim_t;
static void lw_wim_cb(void *ud, const char *name, const char *full, bool is_dir,
                      uint64_t size, uint64_t mtime, uint32_t attrs, size_t depth){
    (void)name; (void)attrs; (void)depth;
    lw_wim_t *x=(lw_wim_t*)ud;
    if(!x || !full) return;
    if(x->self && !strcasecmp(full,x->self)) return;   /* spec 自己不算一项 */
    lw_emit(x->c,full,is_dir,size,mtime,lw_depth_of(x->root,full));
}
#endif

static int lw_wim(const img_path *p, bool rec, lw_ctx *c, img_prog *prog,
                  char *err, size_t errcap){
    #if IMG_WITH_WIM
    char e2[256]="";
    img_wim_stat_t st;
    int rc=img_wim_stat(p->host,p->index?p->index:1,p->inner,&st,e2,sizeof e2);
    if(rc==IMG_OK && !st.exists){
        if(err&&errcap) snprintf(err,errcap,"WIM 里没有这个路径：%s",p->inner);
        return IMG_ENOENT;
    }
    if(img_cancelled()) return IMG_ECANCELLED;
    if(prog) img_prog_item(prog,p->inner);
    lw_wim_t u={c,p->inner,p->inner};
    return img_wim_list(p->host,p->index?p->index:1,p->inner,rec,lw_wim_cb,&u,e2,sizeof e2);
#else
    (void)p; (void)rec; (void)c; (void)prog;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 wimlib");
    return IMG_ENOTSUP;
#endif
}

/* ---------------- 压缩包 ---------------- */
typedef struct { lw_ctx *c; const char *root; const char *self; } lw_arc_t;
static void lw_arc_cb(void *ud, const img_archive_ent *e){
    lw_arc_t *x=(lw_arc_t*)ud;
    if(!x || !e) return;
    if(x->self && !strcmp(e->path,x->self)) return;
    lw_emit(x->c,e->path,e->is_dir,e->size,e->mtime,lw_depth_of(x->root,e->path));
}

static int lw_arc(const img_path *p, bool rec, lw_ctx *c, img_prog *prog,
                  char *err, size_t errcap){
    img_archive_index *ix=NULL;
    int rc=img_archive_index_build(p->host,&ix,err,errcap);
    if(rc!=IMG_OK) return rc;
    if(prog) img_prog_item(prog,p->inner);
    lw_arc_t u={c,p->inner,p->inner};
    rc=img_archive_index_walk(ix,p->inner,rec,lw_arc_cb,&u);
    img_archive_index_release(ix);
    return rc;
}

/* ---------------- 入口 ---------------- */
int img_lswalk_run(const img_path *p, bool recursive, const char *q,
                   img_lswalk_cb cb, void *ud, img_prog *prog,
                   char *err, size_t errcap){
    if(err&&errcap) err[0]=0;
    if(!p||!cb) return IMG_EINVAL;
    lw_ctx c;
    memset(&c,0,sizeof c);
    c.cb=cb;
    c.ud=ud;
    c.q=(q&&*q)?q:NULL;
    switch(p->kind){
    case IMG_XK_HOST:    return lw_host(p,recursive,&c,prog,err,errcap);
    case IMG_XK_NTFS:
    case IMG_XK_FAT:     return lw_disk(p,recursive,&c,prog,err,errcap);
    case IMG_XK_ISO:     return lw_iso(p,recursive,&c,prog,err,errcap);
    case IMG_XK_WIM:     return lw_wim(p,recursive,&c,prog,err,errcap);
    case IMG_XK_ARCHIVE: return lw_arc(p,recursive,&c,prog,err,errcap);
    default:
        if(err&&errcap) snprintf(err,errcap,"这个位置不支持搜索");
        return IMG_ENOTSUP;
    }
}