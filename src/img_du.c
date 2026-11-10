/* img_du.c — 递归大小与项数（REQ-40，见 img_du.h 的口径说明）
 *
 * 五类后端各走各的"列目录"，但计数规则只有一份（du_tick + 三个字段）：
 * 别在后端里各写一遍 —— 否则 /Windows 在 NTFS 上和在压缩包里报出来会对不上。
 *
 * ★ 递归用显式栈（不用嵌套函数、不用递归调用自己）：
 *   clang/NDK 不支持 GCC 的嵌套函数；目录树深度也有上限（栈 64 层够）。
 */
#include "img_du.h"
#include "img_cmd.h"
#include "img_event.h"
#include "img_partition.h"
#include "img_ntfs_fs.h"
#include "img_fat32_fs.h"
#include "img_iso.h"
#include "img_wim_fs.h"
#include "img_archive.h"
#include "img_name.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if !defined(IMG_WITH_WIM)
#define IMG_WITH_WIM 0
#endif
#if !defined(IMG_WITH_NTFS3G)
#define IMG_WITH_NTFS3G 0
#endif

/* ★ 2026-11-10（REQ-65 §6）：目录队列原来是 qbuf[64]，满了就【静默丢整棵子树】。
 *   系统盘（WinSxS / servicing\Packages）待处理目录远多于 64 个，于是
 *   wim.capture --estimate 的 files 少算 38%、bytes 少算 36% —— App 的空间预检
 *   按这个数放行，真正捕获时跑到一半炸在磁盘上。改成按需扩容。*/
#define DU_QCAP_INIT 64

typedef struct {
    img_path     *p;
    uint64_t      limit;
    int           max_depth;
    bool          follow;
    img_prog     *prog;
    img_du_stats *st;
    const img_du_opts *o;      /* REQ-67：排除规则（可为 NULL）*/
} ducx;

typedef struct { char path[2048]; int depth; } du_dirq;

static bool du_push(du_dirq **q, int *cap, int *sp, const char *path, int depth){
    if(*sp >= *cap){
        int nc = *cap ? (*cap)*2 : DU_QCAP_INIT;
        if(nc > 1<<20) return false;                 /* 100 万目录项 ≈ 2 GB，不可能 */
        du_dirq *nq=(du_dirq*)realloc(*q,(size_t)nc*sizeof(du_dirq));
        if(!nq) return false;
        *q=nq; *cap=nc;
    }
    snprintf((*q)[*sp].path,sizeof (*q)[0].path,"%s",path);
    (*q)[*sp].depth=depth;
    (*sp)++;
    return true;
}
static bool du_hit(const ducx *c){
    return c->limit && c->st->scanned>=c->limit;
}
static void du_tick(ducx *c, const char *path){
    c->st->scanned++;
    if(c->prog){
        img_prog_item(c->prog,path?path:"");
        img_prog_count(c->prog,c->st->scanned,0);
    }
}
static void du_join(const char *dir, const char *name, char *out, size_t cap){
    size_t dl=strlen(dir);
    if(dl==1 && dir[0]=='/') snprintf(out,cap,"/%s",name?name:"");
    else snprintf(out,cap,"%s/%s",dir,name?name:"");
}

/* REQ-67：命中的条目直接不计数（目录也不入栈 → 整棵子树不算）。
 * 与 copy / wim.capture 用同一个匹配器，口径不会分叉。*/
static int du_excl(ducx *c, const char *name){
    if(!c->o || c->o->n_exclude <= 0 || !name || !*name) return 0;
    if(img_name_match_any(c->o->exclude, c->o->n_exclude, name)){
        c->st->excluded++;
        return 1;
    }
    return 0;
}

/* basename（最后一个 '/' 之后）；没有 '/' 时返回原串 */
static const char *du_base(const char *path){
    if(!path) return "";
    const char *s = strrchr(path, '/');
    return s ? s+1 : path;
}

/* ---------- 宿主 ---------- */
static int du_host(ducx *c, const char *path, int depth){
    DIR *d=opendir(path);
    if(!d) return IMG_ENOENT;
    struct dirent *de;
    int rc=IMG_OK;
    while((de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        if(du_excl(c,de->d_name)) continue;
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        if(du_hit(c)){ c->st->truncated=true; break; }
        char full[4096]; du_join(path,de->d_name,full,sizeof full);
        struct stat st;
        if(lstat(full,&st)!=0) continue;
        if(S_ISLNK(st.st_mode) && !c->follow) continue;   /* 默认不跟：环 / 把整盘拖进来 */
        du_tick(c,full);
        if(S_ISDIR(st.st_mode)){
            c->st->dirs++;
            if(c->max_depth==0 || depth<c->max_depth){
                rc=du_host(c,full,depth+1);
                if(rc!=IMG_OK) break;
            }
        }else{
            c->st->files++;
            c->st->total_bytes += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return rc;
}

/* ---------- NTFS（一次遍历带 size/alloc） ---------- */
#if IMG_WITH_NTFS3G
typedef struct {
    ducx      *c;
    du_dirq   *q; int *sp; int *qcap;
    int        depth;
    char       cur[2048];
    int        rc;
} du_ntfs_ctx;

static void du_ntfs_cb(void *ud, const img_ntfs_ent_t *e){
    du_ntfs_ctx *x=(du_ntfs_ctx*)ud;
    if(x->rc!=IMG_OK) return;
    if(du_excl(x->c, e->name?e->name:"")) return;      /* REQ-67 */
    if(img_cancelled()){ x->rc=IMG_ECANCELLED; return; }
    if(du_hit(x->c)){ x->c->st->truncated=true; return; }
    char full[2048]; du_join(x->cur,e->name?e->name:"",full,sizeof full);
    du_tick(x->c,full);
    if(e->is_dir){
        x->c->st->dirs++;
        if(x->c->max_depth==0 || x->depth<x->c->max_depth){
            if(!du_push(&x->q,x->qcap,x->sp,full,x->depth+1)) x->c->st->truncated=true;
        }
    }else{
        x->c->st->files++;
        x->c->st->total_bytes += e->size;
        x->c->st->alloc_bytes += e->alloc_bytes;
    }
}
#endif

/* ---------- FAT ---------- */
#if IMG_WITH_NTFS3G
typedef struct {
    ducx      *c;
    du_dirq   *q; int *sp; int *qcap;
    int        depth;
    char       cur[2048];
    int        rc;
} du_fat_ctx;

static void du_fat_cb(void *ud, const img_fat32_ent_t *e){
    du_fat_ctx *x=(du_fat_ctx*)ud;
    if(x->rc!=IMG_OK) return;
    if(du_excl(x->c, e->name?e->name:"")) return;      /* REQ-67 */
    if(img_cancelled()){ x->rc=IMG_ECANCELLED; return; }
    if(du_hit(x->c)){ x->c->st->truncated=true; return; }
    char full[2048]; du_join(x->cur,e->name?e->name:"",full,sizeof full);
    du_tick(x->c,full);
    if(e->is_dir){
        x->c->st->dirs++;
        if(x->c->max_depth==0 || x->depth<x->c->max_depth){
            if(!du_push(&x->q,x->qcap,x->sp,full,x->depth+1)) x->c->st->truncated=true;
        }
    }else{
        x->c->st->files++;
        x->c->st->total_bytes += e->size;
    }
}
#endif

/* ---------- ISO ---------- */
typedef struct {
    ducx    *c;
    img_iso *iso;
    du_dirq *q; int *sp; int *qcap;
    int      depth;
    char     cur[2048];
    int      rc;
} du_iso_ctx;

static void du_iso_cb(void *ud, const char *name, bool is_dir, uint64_t size, uint64_t mtime){
    (void)mtime;
    du_iso_ctx *x=(du_iso_ctx*)ud;
    if(x->rc!=IMG_OK) return;
    if(du_excl(x->c, name)) return;                     /* REQ-67 */
    if(img_cancelled()){ x->rc=IMG_ECANCELLED; return; }
    if(du_hit(x->c)){ x->c->st->truncated=true; return; }
    char full[2048]; du_join(x->cur,name,full,sizeof full);
    du_tick(x->c,full);
    if(is_dir){
        x->c->st->dirs++;
        if(x->c->max_depth==0 || x->depth<x->c->max_depth){
            if(!du_push(&x->q,x->qcap,x->sp,full,x->depth+1)) x->c->st->truncated=true;
        }
    }else{
        x->c->st->files++;
        x->c->st->total_bytes += size;
    }
}

/* ---------- WIM ---------- */
#if IMG_WITH_WIM
typedef struct { ducx *c; const char *self; } du_wim_ctx;
static void du_wim_cb(void *ud, const char *name, const char *full, bool is_dir,
                      uint64_t size, uint64_t mtime, uint32_t attrs, size_t depth){
    (void)name;(void)attrs;(void)depth;(void)mtime;
    du_wim_ctx *u=(du_wim_ctx*)ud;
    if(!u) return;
    if(du_excl(u->c, du_base(full))) return;            /* REQ-67 */
    if(du_hit(u->c)){ u->c->st->truncated=true; return; }
    if(full && u->self && !strcasecmp(full,u->self)) return;   /* --src 自己不算一项 */
    du_tick(u->c,full);
    if(is_dir) u->c->st->dirs++;
    else { u->c->st->files++; u->c->st->total_bytes += size; }
}
#endif

/* ---------- 压缩包 ---------- */
typedef struct { ducx *c; const char *self; } du_arc_ctx;
static void du_arc_cb(void *ud, const img_archive_ent *e){
    du_arc_ctx *u=(du_arc_ctx*)ud;
    if(!u || !e) return;
    if(du_excl(u->c, du_base(e->path))) return;         /* REQ-67 */
    if(u->c->limit && u->c->st->scanned>=u->c->limit){ u->c->st->truncated=true; return; }
    if(u->self && !strcmp(e->path,u->self)) return;
    du_tick(u->c,e->path);
    if(e->is_dir) u->c->st->dirs++;
    else { u->c->st->files++; u->c->st->total_bytes += e->size; }
}

int img_du_run(const img_path *p, uint64_t limit, int max_depth, bool follow,
               img_prog *prog, img_du_stats *st, char *err, size_t errcap){
    return img_du_run_ex(p, limit, max_depth, follow, NULL, prog, st, err, errcap);
}

int img_du_run_ex(const img_path *p, uint64_t limit, int max_depth, bool follow,
                  const img_du_opts *o, img_prog *prog, img_du_stats *st,
                  char *err, size_t errcap){
    if(err&&errcap) err[0]=0;
    if(!p||!st) return IMG_EINVAL;
    memset(st,0,sizeof *st);
    ducx c; memset(&c,0,sizeof c);
    c.p=(img_path*)p; c.limit=limit; c.max_depth=max_depth; c.follow=follow;
    c.prog=prog; c.st=st; c.o=o;
    if(prog){ img_prog_phase(prog,IMG_PH_BROWSE,"统计大小"); img_prog_op(prog,IMG_OP_SCAN); }

    switch(p->kind){
    case IMG_XK_HOST:{
        struct stat s;
        if(lstat(p->host,&s)!=0){
            if(err&&errcap) snprintf(err,errcap,"路径不存在：%s",p->host);
            return IMG_ENOENT;
        }
        if(!S_ISDIR(s.st_mode)){
            st->files=1; st->total_bytes=(uint64_t)s.st_size; st->scanned=1;
            return IMG_OK;
        }
        return du_host(&c,p->host,1);      /* --src 自己不算一项 */
    }
    case IMG_XK_ARCHIVE:{
        img_archive_index *ix=NULL;
        int rc=img_archive_index_build(p->host,&ix,err,errcap);
        if(rc!=IMG_OK) return rc;
        du_arc_ctx u={&c,p->inner};
        rc=img_archive_index_walk(ix,p->inner,true,du_arc_cb,&u);
        img_archive_index_release(ix);
        return rc;
    }
    case IMG_XK_ISO:{
        img_iso *iso=NULL;
        int rc=img_iso_open_path(&iso,p->host,err,errcap);
        if(rc!=IMG_OK) return rc;
        uint64_t sz=0; bool isdir=false;
        rc=img_iso_stat(iso,p->inner,&sz,&isdir);
        if(rc!=IMG_OK){ img_iso_close(iso); return rc; }
        if(!isdir){
            st->files=1; st->total_bytes=sz; st->scanned=1;
            img_iso_close(iso);
            return IMG_OK;
        }
        {
            du_dirq *q=NULL; int qcap=0, sp=0;
            if(!du_push(&q,&qcap,&sp,p->inner,1)){ img_iso_close(iso); return IMG_ENOMEM; }
            rc=IMG_OK;
            while(sp>0 && rc==IMG_OK){
                sp--;
                du_iso_ctx x; memset(&x,0,sizeof x);
                x.c=&c; x.iso=iso; x.q=q; x.sp=&sp; x.qcap=&qcap; x.depth=q[sp].depth; x.rc=IMG_OK;
                snprintf(x.cur,sizeof x.cur,"%s",q[sp].path);
                int n=img_iso_list(iso,x.cur,du_iso_cb,&x);
                q=x.q;         /* ★ 同上 */
                if(n<0) rc=n; else rc=x.rc;
            }
        }
        img_iso_close(iso);
        return rc;
    }
    case IMG_XK_WIM:{
#if IMG_WITH_WIM
        du_wim_ctx u={&c,p->inner};
        char e2[256]="";
        return img_wim_list(p->host,p->index?p->index:1,p->inner,true,du_wim_cb,&u,e2,sizeof e2);
#else
        if(err&&errcap) snprintf(err,errcap,"本构建未启用 wimlib");
        return IMG_ENOTSUP;
#endif
    }
    case IMG_XK_NTFS:
    case IMG_XK_FAT:{
#if IMG_WITH_NTFS3G
        img_ctx *ctx=img_ctx_open(p->host,false,false,err,errcap);
        if(!ctx) return IMG_ENOENT;
        img_qcow2 *q=img_ctx_qcow2(ctx);
        img_parttable_t t; uint64_t pb=0;
        int rc=img_parttable_read(q,&t,0);
        if(rc==IMG_OK){
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t.parts[i].used && t.parts[i].first_lba==p->lba){
                    pb=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512ULL; break;
                }
        }
        if(!pb){ img_ctx_close(ctx); return IMG_ENOENT; }

        if(p->kind==IMG_XK_NTFS){
            img_ntfs_fs *fs=img_ntfs_mount_rw(q,p->lba,pb);
            if(!fs){ img_ctx_close(ctx); return IMG_ESTATE; }
            uint64_t ino=0; bool isdir=false;
            rc=img_ntfs_verify_path(fs,p->inner,&ino,&isdir);
            if(rc!=IMG_OK){ img_ntfs_umount(fs); img_ctx_close(ctx); return rc; }
            if(!isdir){
                img_ntfs_ent_t e; rc=img_ntfs_stat(fs,p->inner,&e);
                if(rc==IMG_OK){ st->files=1; st->total_bytes=e.size; st->alloc_bytes=e.alloc_bytes; st->scanned=1; }
            }else{
                du_dirq *qbuf=NULL; int qcap=0, sp=0;
                if(!du_push(&qbuf,&qcap,&sp,p->inner,1)){ img_ntfs_umount(fs); img_ctx_close(ctx); return IMG_ENOMEM; }
                rc=IMG_OK;
                while(sp>0 && rc==IMG_OK){
                    sp--;
                    du_ntfs_ctx x; memset(&x,0,sizeof x);
                    x.c=&c; x.q=qbuf; x.sp=&sp; x.qcap=&qcap; x.depth=qbuf[sp].depth; x.rc=IMG_OK;
                    snprintf(x.cur,sizeof x.cur,"%s",qbuf[sp].path);
                    rc=img_ntfs_list_dir_full(fs,x.cur,du_ntfs_cb,&x);
                qbuf=x.q;      /* ★ du_push 里 realloc 换过地址，外层要跟上 */
                    if(rc==IMG_OK) rc=x.rc;
                }
            }
            img_ntfs_umount(fs);
        }else{
            img_fat32_fs *fs=img_fat32_mount(q,p->lba,false);
            if(!fs){ img_ctx_close(ctx); return IMG_ESTATE; }
            if(!img_fat32_dir_exists(fs,p->inner)){
                img_fat32_rd *rd=NULL; img_src_meta m;
                rc=img_fat32_rd_open(fs,p->inner,&rd,&m);
                if(rd) img_fat32_rd_close(rd);
                if(rc==IMG_OK){ st->files=1; st->total_bytes=m.size; st->scanned=1; }
            }else{
                du_dirq *qbuf=NULL; int qcap=0, sp=0;
                if(!du_push(&qbuf,&qcap,&sp,p->inner,1)){ img_fat32_unmount(fs); img_ctx_close(ctx); return IMG_ENOMEM; }
                rc=IMG_OK;
                while(sp>0 && rc==IMG_OK){
                    sp--;
                    du_fat_ctx x; memset(&x,0,sizeof x);
                    x.c=&c; x.q=qbuf; x.sp=&sp; x.qcap=&qcap; x.depth=qbuf[sp].depth; x.rc=IMG_OK;
                    snprintf(x.cur,sizeof x.cur,"%s",qbuf[sp].path);
                    rc=img_fat32_list_dir_full(fs,x.cur,du_fat_cb,&x);
                qbuf=x.q;      /* ★ 同上 */
                    if(rc==IMG_OK) rc=x.rc;
                }
            }
            img_fat32_unmount(fs);
        }
        img_ctx_close(ctx);
        return rc;
#else
        if(err&&errcap) snprintf(err,errcap,"本构建未启用 libntfs-3g");
        return IMG_ENOTSUP;
#endif
    }
    default:
        if(err&&errcap) snprintf(err,errcap,"这个位置不支持统计");
        return IMG_ENOTSUP;
    }
}