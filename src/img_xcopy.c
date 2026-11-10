#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_xcopy.c — 统一复制引擎（见 include/img_xcopy.h 的设计说明）
 *
 * 组合矩阵（本文件全覆盖）：
 *   NTFS ↔ NTFS   （同卷 / 同镜像跨分区 / 跨 qcow2）
 *   NTFS ↔ FAT32  （同镜像跨分区 / 跨 qcow2）
 *   FAT32 ↔ FAT32 （同上）
 *   宿主 → NTFS/FAT32（含递归目录；手机 → 镜像）
 *   NTFS/FAT32 → 宿主（含递归目录；镜像 → 手机）
 *   ISO → NTFS/FAT32（流式，不落临时文件）
 *   WIM/ESD → NTFS/FAT32（先材料化到临时目录，再按本地树复制）
 *
 * 纪律：
 *   · 拒绝对"源是目录但没开 recursive"的情况做任何写动作（零副作用）；
 *   · 目标已存在时按 conflict 政策处理，默认 fail；
 *   · 每个文件先写临时名再改名（由各 fs 层保证），失败/取消不留半成品；
 *   · 目录时间在子树复制完之后才写（否则会被子项创建时间覆盖）；
 *   · 符号链接默认跳过并计数，不跟随（防止复制出环）。
 */
#include "img_xcopy.h"
#include "img_archive_w.h"
#include "img_archive.h"
#include <fnmatch.h>
#include "img_cmd.h"
#include "img_ntfs_fs.h"
#include "img_fat32_fs.h"
#include "img_iso.h"
#include "img_wim_fs.h"
#include "img_partition.h"
#include "img_stream.h"
#include "img_lock.h"
#include "img_name.h"
#include "img_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <limits.h>

#ifndef IMG_WITH_WIM
#define IMG_WITH_WIM 0
#endif
#ifndef IMG_WITH_NTFS3G
#define IMG_WITH_NTFS3G 0
#endif

#define XC_MAX_DEPTH 32
#define XC_NAME_MAX  1024   /* 255 个中文字符 ≈ 765 字节；留足但不无限 */

/* ================= 小工具 ================= */
static void xstrlcpy(char *d, const char *s, size_t cap){
    if(!d||!cap) return;
    if(!s){ d[0]=0; return; }
    size_t n=strlen(s); if(n>=cap) n=cap-1;
    memcpy(d,s,n); d[n]=0;
}
static const char *base_of(const char *p){
    const char *s=strrchr(p,'/');
    if(!s) return p;
    if(!s[1] && s!=p){                     /* 末尾有斜杠 → 往前找 */
        const char *q=s-1;
        while(q>p && *q=='/') q--;
        const char *r=q;
        while(r>p && r[-1]!='/') r--;
        return r;
    }
    return s+1;
}
/* dir + "/" + name，根目录不重复斜杠 */
static int join_path(const char *dir, const char *name, char *out, size_t cap){
    int n;
    if(!dir||!dir[0]||!strcmp(dir,"/")) n=snprintf(out,cap,"/%s",name);
    else n=snprintf(out,cap,"%s/%s",dir,name);
    return (n<0||(size_t)n>=cap)?IMG_EINVAL:IMG_OK;
}
static void parent_of(const char *path, char *out, size_t cap){
    xstrlcpy(out,path,cap);
    char *s=strrchr(out,'/');
    if(!s){ out[0]=0; return; }
    if(s==out){ out[1]=0; return; }
    *s=0;
}

/* ================= 宿主文件系统 ================= */
static int host_mkdir_p(const char *path, mode_t mode){
    if(!path||!*path) return IMG_EINVAL;
    char tmp[1200]; xstrlcpy(tmp,path,sizeof tmp);
    size_t n=strlen(tmp);
    while(n>1 && tmp[n-1]=='/') tmp[--n]=0;
    for(char *p=tmp+1; *p; p++){
        if(*p!='/' ) continue;
        *p=0;
        if(mkdir(tmp,mode)!=0 && errno!=EEXIST) return IMG_EIO;
        *p='/';
    }
    if(mkdir(tmp,mode)!=0 && errno!=EEXIST) return IMG_EIO;
    return IMG_OK;
}
static void host_rm_rf(const char *path){
    struct stat st;
    if(lstat(path,&st)!=0) return;
    if(S_ISDIR(st.st_mode)){
        DIR *d=opendir(path);
        if(d){
            struct dirent *de;
            while((de=readdir(d))){
                if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
                char c[2048]; snprintf(c,sizeof c,"%s/%s",path,de->d_name);
                host_rm_rf(c);
            }
            closedir(d);
        }
        rmdir(path);
    } else unlink(path);
}

typedef struct { int fd; } host_rd;
static int host_read_cb(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    host_rd *h=(host_rd*)ud;
    if(got) *got=0;
    ssize_t r=pread(h->fd,buf,want,(off_t)off);
    if(r<0) return IMG_EIO;
    *got=(size_t)r;
    return IMG_OK;
}

/* ================= 端点 ================= */
typedef struct {
    img_xloc    loc;
    img_ctx    *ctx;         /* 自己打开的镜像（不共享时） */
    img_qcow2  *q;
    img_ntfs_fs *ntfs;
    img_fat32_fs *fat;
    img_iso    *iso;
    bool        own_ctx;     /* ctx 是不是自己打开的（共享时不能关） */
    bool        own_mount;   /* fs 句柄是不是自己挂的（同一分区复用时不能重复卸载） */
    uint64_t    lba_used;
    char        root[4096];  /* 实际源根（WIM 材料化后指向临时目录） */
    char        tmp_root[2048];   /* WIM 临时目录（关闭时要删） */
    /* REQ-41：目标是压缩包时的写句柄（整包重写到临时文件，收尾才 rename） */
    img_aw     *aw;
    img_aw_names *ns;        /* 计划覆盖时用的只读名字表 */
    uint64_t    aw_io_done, aw_io_total;
    char        aw_err[256];
} xep;

static int fs_at(img_qcow2 *q, uint64_t lba, img_xkind want){
    uint8_t b[512];
    if(img_qcow2_read_full(q, lba*512ULL, b, sizeof b)!=IMG_OK) return 0;
    if(b[510]!=0x55 || b[511]!=0xAA) return 0;
    if(want==IMG_XK_NTFS) return memcmp(b+3,"NTFS    ",8)==0;
    if(want==IMG_XK_FAT)  return memcmp(b+82,"FAT32   ",8)==0;
    return 0;
}

/* 自动挑分区：恰好一个候选才返回；0 个 / 多个都报错 */
static int pick_lba(img_qcow2 *q, img_xkind k, uint64_t *out){
    img_parttable_t t;
    int rc=img_parttable_read(q,&t,0);
    if(rc!=IMG_OK) return rc;
    uint64_t found=0; int n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(!t.parts[i].used) continue;
        if(fs_at(q,t.parts[i].first_lba,k)){ found=t.parts[i].first_lba; n++; }
    }
    if(n==1){ *out=found; return IMG_OK; }
    return (n==0)?IMG_ENOENT:IMG_EAGAIN;      /* EAGAIN = 多个候选，必须让用户显式给 lba */
}

static int part_bytes_at(img_qcow2 *q, uint64_t lba, uint64_t *out){
    img_parttable_t t;
    int rc=img_parttable_read(q,&t,0);
    if(rc!=IMG_OK) return rc;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(t.parts[i].used && t.parts[i].first_lba==lba){
            *out=(t.parts[i].last_lba - t.parts[i].first_lba + 1)*512ULL;
            return *out?IMG_OK:IMG_EINVAL;
        }
    }
    return IMG_ENOENT;
}

static void ep_close(xep *e){
    if(!e) return;
    if(e->aw){ img_aw_abort(e->aw); e->aw=NULL; }     /* 没 commit 就关 = 失败：临时文件删掉 */
    if(e->ns){ img_aw_names_free(e->ns); e->ns=NULL; }
    if(e->ntfs && e->own_mount){ img_ntfs_umount(e->ntfs); }
    e->ntfs=NULL;
    if(e->fat && e->own_mount){ img_fat32_unmount(e->fat); }
    e->fat=NULL;
    if(e->iso){ img_iso_close(e->iso); e->iso=NULL; }
    if(e->ctx && e->own_ctx){ img_ctx_close(e->ctx); }
    e->ctx=NULL;
    e->q=NULL;
    if(e->tmp_root[0]){ host_rm_rf(e->tmp_root); e->tmp_root[0]=0; }
}

/* shared!=NULL 表示"源和目标在同一张镜像"：共用同一个 img_ctx（否则两个句柄写同一文件必炸） */
static int ep_open(xep *e, const img_xloc *loc, img_ctx *shared, bool writable,
                   const xep *reuse){
    memset(e,0,sizeof *e);
    e->loc=*loc;
    xstrlcpy(e->root, loc->path, sizeof e->root);
    if(loc->kind==IMG_XK_HOST) return IMG_OK;
    if(loc->kind==IMG_XK_ARCHIVE) return IMG_OK;   /* 压缩包不需要持久端点：按需开流 */

    if(loc->kind==IMG_XK_ISO){
        char err[256]={0};
        int rc=img_iso_open_path(&e->iso, loc->img, err, sizeof err);
        if(rc!=IMG_OK) return rc;
        return IMG_OK;
    }
    if(loc->kind==IMG_XK_WIM){
#if IMG_WITH_WIM
        img_wim_stat_t st;
        char err[256]={0};
        int rc=img_wim_stat(loc->img, loc->index?loc->index:1, loc->path, &st, err, sizeof err);
        if(rc!=IMG_OK || !st.exists) return (rc==IMG_OK)?IMG_ENOENT:rc;
        const char *tmp=img_tmpdir();
        if(!tmp||!*tmp) return IMG_ESTATE;
        char tmpl[600];
        if(img_tmpfile_dir("xwim",tmpl,sizeof tmpl)!=IMG_OK) return IMG_EIO;
        char *dir=tmpl;
        xstrlcpy(e->tmp_root, dir, sizeof e->tmp_root);
        rc=img_wim_extract_tree(loc->img, loc->index?loc->index:1, loc->path,
                                dir, NULL, err, sizeof err);
        if(rc!=IMG_OK) return rc;
        /* ★ P1（2026-11-08）：wimlib_extract_paths 抽 "/1" 到 out_dir 时，
         *   会在 out_dir 下【保留 "/1" 这一层】（实测 out_dir/1/2/3）。
         *   所以源根要拼上 loc->path，才是用户要的那棵子树。 */
        snprintf(e->root,sizeof e->root,"%s%s",dir,loc->path);
        /* 材料化之后就是一棵普通本地树：后续 stat/list/read 全走宿主分支。
         * tmp_root 仍然记着，关闭端点时整棵删掉。*/
        e->loc.kind=IMG_XK_HOST;
        return IMG_OK;
#else
        return IMG_ENOTSUP;
#endif
    }

    /* NTFS / FAT：镜像端点 */
    if(shared){
        e->ctx=shared; e->own_ctx=false;
        e->q=img_ctx_qcow2(shared);
    } else {
        char err[256]={0};
        e->ctx=img_ctx_open(loc->img, writable, false, err, sizeof err);
        if(!e->ctx) return IMG_ENOENT;
        e->own_ctx=true;
        e->q=img_ctx_qcow2(e->ctx);
    }
    if(!e->q) return IMG_EFORMAT;

    uint64_t lba=loc->lba;
    if(!lba){
        int prc=pick_lba(e->q, loc->kind, &lba);
        if(prc==IMG_ENOENT) return IMG_ENOENT;          /* 没有这种分区 */
        if(prc!=IMG_OK) return prc;                     /* ESTATE = 多个候选 */
    } else {
        if(!fs_at(e->q,lba,loc->kind)) return IMG_ESTATE;
    }
    uint64_t pb=0;
    int rc=part_bytes_at(e->q,lba,&pb);
    if(rc!=IMG_OK) return rc;
    e->lba_used=lba;

    /* 同一张镜像 + 同一个分区 + 同一个文件系统：共用已经挂好的卷。
     * 两层嵌套挂载同一个 NTFS 卷会让两套缓存对着同一个 $MFT 写 = 必然损坏。*/
    if(reuse && reuse->q==e->q && reuse->lba_used==lba && reuse->loc.kind==loc->kind){
        if(loc->kind==IMG_XK_NTFS && reuse->ntfs){ e->ntfs=reuse->ntfs; e->own_mount=false; return IMG_OK; }
        if(loc->kind==IMG_XK_FAT && reuse->fat){ e->fat=reuse->fat; e->own_mount=false; return IMG_OK; }
    }

    if(loc->kind==IMG_XK_NTFS){
        e->ntfs=img_ntfs_mount_rw(e->q,lba,pb);
        if(!e->ntfs) return IMG_ESTATE;
        e->own_mount=true;
    } else {
        e->fat=img_fat32_mount(e->q,lba,true);
        if(!e->fat) return IMG_ESTATE;
        e->own_mount=true;
    }
    return IMG_OK;
}

/* ================= 源：stat / list / 打开流 ================= */
static int src_stat(xep *s, const char *path, bool *isdir, img_src_meta *meta){
    memset(meta,0,sizeof *meta);
    switch(s->loc.kind){
    case IMG_XK_HOST:{
        struct stat st;
        if(stat(path,&st)!=0) return IMG_ENOENT;
        *isdir=S_ISDIR(st.st_mode);
        meta->size=(uint64_t)st.st_size;
        meta->has_times=true;
        meta->times[0]=(uint64_t)st.st_ctime;
        meta->times[1]=(uint64_t)st.st_mtime;
        meta->times[2]=(uint64_t)st.st_atime;
        meta->times[3]=(uint64_t)st.st_ctime;
        return IMG_OK;
    }
    case IMG_XK_ARCHIVE:{
        bool d=false; uint64_t sz=0, mt=0; char err[160]="";
        int rc=img_archive_stat(s->loc.img,path,&d,&sz,&mt,err,sizeof err);
        if(rc!=IMG_OK) return rc;
        *isdir=d;
        meta->size=d?0:sz;
        if(mt){ meta->has_times=true; meta->times[0]=mt; meta->times[1]=mt; meta->times[2]=mt; meta->times[3]=mt; }
        return IMG_OK;
    }
    case IMG_XK_NTFS:{
        uint64_t ino=0; bool d=false;
        int rc=img_ntfs_verify_path(s->ntfs,path,&ino,&d);
        if(rc!=IMG_OK) return rc;
        *isdir=d;
        if(d){
            uint64_t t[4];
            if(img_ntfs_file_times(s->ntfs,path,t)==IMG_OK){
                meta->has_times=true;
                meta->times[0]=t[0]; meta->times[1]=t[1]; meta->times[2]=t[2]; meta->times[3]=t[3];
            }
            return IMG_OK;
        }
        img_ntfs_rd *rd=NULL;
        rc=img_ntfs_rd_open(s->ntfs,path,&rd,meta);
        if(rd) img_ntfs_rd_close(rd);
        return rc;
    }
    case IMG_XK_FAT:{
        if(img_fat32_dir_exists(s->fat,path)){ *isdir=true;
            uint16_t t[5];
            if(img_fat32_file_times(s->fat,path,t)==IMG_OK){
                meta->has_times=true;
                meta->times[0]=(uint64_t)img_fat32_dos_to_epoch(t[0],t[1]);
                meta->times[1]=(uint64_t)img_fat32_dos_to_epoch(t[3],t[4]);
                meta->times[2]=(uint64_t)img_fat32_dos_to_epoch(0,t[2]);
                meta->times[3]=meta->times[1];
            }
            return IMG_OK;
        }
        img_fat32_rd *rd=NULL;
        int rc=img_fat32_rd_open(s->fat,path,&rd,meta);
        if(rd) img_fat32_rd_close(rd);
        if(rc==IMG_ENOENT) return IMG_ENOENT;
        *isdir=false;
        return rc;
    }
    case IMG_XK_ISO:{
        uint64_t sz=0; bool d=false;
        int rc=img_iso_stat(s->iso,path,&sz,&d);
        if(rc!=IMG_OK) return rc;
        *isdir=d; meta->size=sz;
        return IMG_OK;
    }
    default: return IMG_ENOTSUP;
    }
}

typedef struct { char name[XC_NAME_MAX]; bool isdir; uint64_t size; bool skip; } xent;
typedef struct { xent *v; int n, cap; int overflow; } xlist;

/* ★ 2026-11-10（REQ-65 §4）：目录条目不再截断在 256。
 *   以前 cap 固定 256、满了直接 overflow=1 丢掉条目 → 最后报 IMG_EFULL。
 *   NTFS 上 \Windows\WinSxS 上千条、\Windows\servicing\Packages 上万条，
 *   固定上限一律走不通；更糟的是"静默丢条目"会做出一个缺文件的 WIM。
 *   上限 64k 条目（xent ≈1KB → 最多 64 MB），病态目录仍按 EFULL 报错。*/
#define XLIST_MAX 65536
static bool xlist_grow(xlist *l){
    int nc = l->cap ? l->cap*2 : 256;
    if(nc > XLIST_MAX) return false;
    xent *nv=(xent*)realloc(l->v,(size_t)nc*sizeof(xent));
    if(!nv) return false;
    l->v=nv; l->cap=nc;
    return true;
}
static void xlist_push(xlist *l, const char *name, bool isdir, uint64_t size){
    if(l->n>=l->cap && !xlist_grow(l)){ l->overflow=1; return; }
    /* 名字长到连缓冲都放不下（≈1023 字节）：不截断、不跳过 —— 直接失败。
     * 各文件系统自己的 255 字符上限由目标层按"字符/码元"判定
     * （NTFS: UTF-16 码元；FAT: UTF-16 码元；宿主: 255 字节）。*/
    if(strlen(name) >= XC_NAME_MAX){ l->overflow=2; return; }
    xstrlcpy(l->v[l->n].name,name,XC_NAME_MAX);
    l->v[l->n].isdir=isdir;
    l->v[l->n].size=size;
    l->v[l->n].skip=false;
    l->n++;
}
static void cb_ntfs(void *ud, const char *name, bool is_dir, uint64_t size){
    xlist_push((xlist*)ud,name,is_dir,size);
}
static void cb_fat(void *ud, const char *name, bool is_dir, uint64_t size){
    xlist_push((xlist*)ud,name,is_dir,size);
}
/* ★ P1：压缩包索引 walk 的回调（条目带完整路径，只取名） */
static void cb_arch(void *ud, const img_archive_ent *e){
    if(!e) return;
    const char *nm=strrchr(e->path,'/');
    nm = nm ? nm+1 : e->path;
    if(!*nm) return;                        /* 根目录自身 */
    xlist_push((xlist*)ud, nm, e->is_dir, e->size);
}
static void cb_iso(void *ud, const char *name, bool is_dir, uint64_t size, uint64_t mtime){
    (void)mtime;
    xlist_push((xlist*)ud,name,is_dir,size);
}

static int src_list(xep *s, const char *path, xlist *l){
    memset(l,0,sizeof *l);
    l->cap=256;
    l->v=(xent*)calloc((size_t)l->cap,sizeof(xent));
    if(!l->v) return IMG_ENOMEM;
    int rc=IMG_OK;
    switch(s->loc.kind){
    case IMG_XK_HOST:{
        DIR *d=opendir(path);
        if(!d){ free(l->v); l->v=NULL; return IMG_ENOENT; }
        struct dirent *de;
        while((de=readdir(d))){
            if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
            char c[1400]; snprintf(c,sizeof c,"%s/%s",path,de->d_name);
            struct stat st;
            if(lstat(c,&st)!=0) continue;
            /* 符号链接一律跳过（不跟随）：跟随会复制出环/把整盘拖进来。 */
            if(S_ISLNK(st.st_mode)){
                if(l->n>=l->cap){
                    int nc=l->cap*2;
                    xent *nv=(xent*)realloc(l->v,(size_t)nc*sizeof(xent));
                    if(!nv){ rc=IMG_ENOMEM; break; }
                    l->v=nv; l->cap=nc;
                }
                xstrlcpy(l->v[l->n].name,de->d_name,XC_NAME_MAX);
                l->v[l->n].isdir=false; l->v[l->n].size=0; l->v[l->n].skip=true;
                l->n++;
                continue;
            }
            if(l->n>=l->cap){
                int nc=l->cap*2;
                xent *nv=(xent*)realloc(l->v,(size_t)nc*sizeof(xent));
                if(!nv){ rc=IMG_ENOMEM; break; }
                l->v=nv; l->cap=nc;
            }
            xlist_push(l,de->d_name,S_ISDIR(st.st_mode),(uint64_t)st.st_size);
        }
        closedir(d);
        break;
    }
    case IMG_XK_NTFS:
        rc=img_ntfs_list_dir_sizes(s->ntfs,path,cb_ntfs,l);
        break;
    case IMG_XK_FAT:
        rc=img_fat32_list_dir(s->fat,path,cb_fat,l);
        break;
    case IMG_XK_ISO:{
        /* ★ img_iso_list 成功时返回【条目数】（>=0），不是 IMG_OK(0)。
         *   直接当 rc 用会把"9 个条目"误判成错误 +9。*/
        int n=img_iso_list(s->iso,path,cb_iso,l);
        rc=(n>=0)?IMG_OK:n;
        break;
    }
    case IMG_XK_ARCHIVE:{
        /* ★ P1（2026-11-08）：压缩包目录枚举。索引扫一遍包（20 万条 ≈0.4s），
         *   walk 拿【直接子项】。回调是 (ud, const img_archive_ent*)。 */
        img_archive_index *ix=NULL; char err[256]="";
        int rc2=img_archive_index_build(s->loc.img,&ix,err,sizeof err);
        if(rc2!=IMG_OK){ rc=rc2; break; }
        rc=img_archive_index_walk(ix,path,false,cb_arch,l);
        img_archive_index_release(ix);
        break;
    }
    default: rc=IMG_ENOTSUP; break;
    }
    if(rc==IMG_OK && l->overflow==2) rc=IMG_ENAMETOOLONG;
    else if(rc==IMG_OK && l->overflow) rc=IMG_EFULL;
    if(rc!=IMG_OK){ free(l->v); l->v=NULL; }
    return rc;
}

typedef struct { img_ntfs_rd *rd; } ntfs_stream_ud;
static int ntfs_stream_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    return img_ntfs_rd_read(((ntfs_stream_ud*)ud)->rd,off,buf,want,got);
}
typedef struct { img_fat32_rd *rd; } fat_stream_ud;
static int fat_stream_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    return img_fat32_rd_read(((fat_stream_ud*)ud)->rd,off,buf,want,got);
}
typedef struct { img_iso *iso; const char *path; uint64_t size; } iso_stream_ud;
static int iso_stream_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    iso_stream_ud *u=(iso_stream_ud*)ud;
    if(got) *got=0;
    if(off>=u->size) return IMG_OK;
    size_t n=(size_t)((u->size-off<(uint64_t)want)?(u->size-off):(uint64_t)want);
    if(!n) return IMG_OK;
    int rc=img_iso_read_range(u->iso,u->path,off,buf,n);
    if(rc!=IMG_OK) return rc;
    *got=n;
    return IMG_OK;
}

/* 打开源流。*holder 由调用方 free；流里指向 holder 的内存。 */
static int src_open_stream(xep *s, const char *path, img_stream *out, void **holder){
    *holder=NULL;
    switch(s->loc.kind){
    case IMG_XK_HOST:{
        host_rd *h=(host_rd*)calloc(1,sizeof *h);
        if(!h) return IMG_ENOMEM;
        h->fd=open(path,O_RDONLY);
        if(h->fd<0){ free(h); return IMG_ENOENT; }
        out->ud=h; out->read=host_read_cb; out->close=NULL;
        *holder=h;
        return IMG_OK;
    }
    case IMG_XK_NTFS:{
        ntfs_stream_ud *h=(ntfs_stream_ud*)calloc(1,sizeof *h);
        if(!h) return IMG_ENOMEM;
        int rc=img_ntfs_rd_open(s->ntfs,path,&h->rd,NULL);
        if(rc!=IMG_OK){ free(h); return rc; }
        out->ud=h; out->read=ntfs_stream_read; out->close=NULL;
        *holder=h;
        return IMG_OK;
    }
    case IMG_XK_FAT:{
        fat_stream_ud *h=(fat_stream_ud*)calloc(1,sizeof *h);
        if(!h) return IMG_ENOMEM;
        int rc=img_fat32_rd_open(s->fat,path,&h->rd,NULL);
        if(rc!=IMG_OK){ free(h); return rc; }
        out->ud=h; out->read=fat_stream_read; out->close=NULL;
        *holder=h;
        return IMG_OK;
    }
    case IMG_XK_ARCHIVE:{
        img_stream st; uint64_t sz=0; char err[160]="";
        int rc=img_archive_stream_open(s->loc.img,path,&st,&sz,NULL,err,sizeof err);
        if(rc!=IMG_OK) return rc;
        img_stream *hs=(img_stream*)calloc(1,sizeof *hs);
        if(!hs){ if(st.close) st.close(st.ud); return IMG_ENOMEM; }
        *hs=st;
        *out=*hs;
        *holder=hs;           /* close 时要 join 线程 + 关管道，不能只 free */
        return IMG_OK;
    }
    case IMG_XK_ISO:{
        iso_stream_ud *h=(iso_stream_ud*)calloc(1,sizeof *h);
        if(!h) return IMG_ENOMEM;
        uint64_t sz=0; bool d=false;
        int rc=img_iso_stat(s->iso,path,&sz,&d);
        if(rc!=IMG_OK || d){ free(h); return (rc==IMG_OK)?IMG_ENOTSUP:rc; }
        h->iso=s->iso; h->path=path; h->size=sz;
        out->ud=h; out->read=iso_stream_read; out->close=NULL;
        *holder=h;
        return IMG_OK;
    }
    default: return IMG_ENOTSUP;
    }
}
static void src_close_stream(xep *s, void *holder){
    if(!holder) return;
    switch(s->loc.kind){
    case IMG_XK_HOST: close(((host_rd*)holder)->fd); break;
    case IMG_XK_NTFS: img_ntfs_rd_close(((ntfs_stream_ud*)holder)->rd); break;
    case IMG_XK_FAT:  img_fat32_rd_close(((fat_stream_ud*)holder)->rd); break;
    case IMG_XK_ARCHIVE:{
        img_stream *hs=(img_stream*)holder;
        if(hs->close) hs->close(hs->ud);
        break;
    }
    default: break;
    }
    free(holder);
}

/* ================= 目标 ================= */
static int dst_exists(xep *d, const char *path, bool *isdir){
    switch(d->loc.kind){
    case IMG_XK_ARCHIVE:{
        if(!d->aw && !d->ns) return 0;
        bool dd=false;
        int hit = d->aw ? img_aw_lookup(d->aw,path,&dd) : img_aw_names_lookup(d->ns,path,&dd);
        if(hit<=0) return 0;
        if(isdir) *isdir=dd;
        return 1;
    }
    case IMG_XK_HOST:{
        struct stat st;
        if(stat(path,&st)!=0) return 0;
        *isdir=S_ISDIR(st.st_mode);
        return 1;
    }
    case IMG_XK_NTFS:{
        uint64_t ino=0; bool dd=false;
        if(img_ntfs_verify_path(d->ntfs,path,&ino,&dd)!=IMG_OK) return 0;
        *isdir=dd; return 1;
    }
    case IMG_XK_FAT:{
        if(img_fat32_dir_exists(d->fat,path)){ *isdir=true; return 1; }
        if(img_fat32_path_exists(d->fat,path)){ *isdir=false; return 1; }
        return 0;
    }
    default: return 0;
    }
}
static int dst_mkdir_p(xep *d, const char *path){
    if(d->loc.kind==IMG_XK_ARCHIVE){
        if(!d->aw) return IMG_ESTATE;
        char err[200]="";
        int rc=img_aw_add_dir(d->aw,path,0,err,sizeof err);
        if(rc!=IMG_OK && d->aw_err[0]==0) snprintf(d->aw_err,sizeof d->aw_err,"%s",err);
        return rc;
    }
    if(d->loc.kind==IMG_XK_HOST) return host_mkdir_p(path,0755);
    if(d->loc.kind==IMG_XK_NTFS) return img_ntfs_mkdir_p(d->ntfs,path);
    if(d->loc.kind==IMG_XK_FAT)  return img_fat32_mkdir_p(d->fat,path);
    return IMG_ENOTSUP;
}
static int dst_unique_name(xep *d, const char *dir, const char *base, char *out, size_t cap){
    if(d->loc.kind==IMG_XK_ARCHIVE){
        for(int i=1;i<1000;i++){
            const char *dot=strrchr(base,'.');
            bool has_ext=dot && dot!=base && dot[1];
            char cand[400];
            if(has_ext) snprintf(cand,sizeof cand,"%.*s (%d)%s",(int)(dot-base),base,i,dot);
            else snprintf(cand,sizeof cand,"%s (%d)",base,i);
            char full[1400];
            if(join_path(dir,cand,full,sizeof full)!=IMG_OK) return IMG_EINVAL;
            bool exists = d->aw ? img_aw_has(d->aw,full) : (d->ns?img_aw_names_has(d->ns,full):false);
            if(!exists){ xstrlcpy(out,cand,cap); return IMG_OK; }
        }
        return IMG_EEXISTS;
    }
    if(d->loc.kind==IMG_XK_HOST){
        for(int i=1;i<1000;i++){
            const char *dot=strrchr(base,'.');
            bool has_ext=dot && dot!=base && dot[1];
            char cand[400];
            if(has_ext) snprintf(cand,sizeof cand,"%.*s (%d)%s",(int)(dot-base),base,i,dot);
            else snprintf(cand,sizeof cand,"%s (%d)",base,i);
            char full[1400];
            if(join_path(dir,cand,full,sizeof full)!=IMG_OK) return IMG_EINVAL;
            struct stat st;
            if(stat(full,&st)!=0){ xstrlcpy(out,cand,cap); return IMG_OK; }
        }
        return IMG_EEXISTS;
    }
    if(d->loc.kind==IMG_XK_NTFS) return img_ntfs_unique_name(d->ntfs,dir,base,out,cap);
    if(d->loc.kind==IMG_XK_FAT)  return (img_fat32_unique_name(d->fat,dir,base,out,cap)==0)?IMG_OK:IMG_EEXISTS;
    return IMG_ENOTSUP;
}
static int dst_set_times(xep *d, const char *path, const img_src_meta *meta){
    if(d->loc.kind==IMG_XK_ARCHIVE) return IMG_OK;   /* 条目时间在建条目时已写 */
    if(!meta->has_times) return IMG_OK;
    if(d->loc.kind==IMG_XK_NTFS){
        uint64_t t[4]; memcpy(t,meta->times,sizeof t);
        return img_ntfs_set_times(d->ntfs,path,t);
    }
    if(d->loc.kind==IMG_XK_FAT){
        uint16_t t5[5]={0,0,0,0,0}, tm=0, dt=0;
        img_fat32_epoch_to_dos((int64_t)meta->times[0],&t5[0],&t5[1]);
        img_fat32_epoch_to_dos((int64_t)meta->times[2],&tm,&t5[2]);
        img_fat32_epoch_to_dos((int64_t)meta->times[1],&tm,&dt);
        t5[3]=tm; t5[4]=dt;
        return img_fat32_file_set_times(d->fat,path,t5);
    }
    if(d->loc.kind==IMG_XK_HOST){
        struct timespec ts[2];
        ts[0].tv_sec=(time_t)meta->times[2]; ts[0].tv_nsec=0;
        ts[1].tv_sec=(time_t)meta->times[1]; ts[1].tv_nsec=0;
        return utimensat(AT_FDCWD,path,ts,0)==0?IMG_OK:IMG_EIO;
    }
    return IMG_ENOTSUP;
}

/* 写一个文件（目标已解析成完整路径）。out_* 可为 NULL。 */
static int dst_write_file(xep *d, const char *final, const img_stream *src,
                          const img_src_meta *meta, const img_xcopy_opts *o,
                          img_prog *prog, bool existed, uint64_t *out_size, bool *out_replaced){
    if(out_size) *out_size=meta->size;
    if(out_replaced) *out_replaced=existed;
    if(d->loc.kind==IMG_XK_ARCHIVE){
        if(!d->aw) return IMG_ESTATE;
        uint64_t mt = (o->preserve_times && meta->has_times) ? meta->times[1] : 0;
        d->aw_err[0]=0;
        int rc=img_aw_add_file(d->aw,final,src,meta->size,mt,prog,
                               &d->aw_io_done,d->aw_io_total,d->aw_err,sizeof d->aw_err);
        return rc;
    }
    if(d->loc.kind==IMG_XK_NTFS){
        img_copy_opts co={ existed?IMG_COPY_OVERWRITE:IMG_COPY_FAIL, o->preserve_times, o->create_parents };
        img_copy_result r;
        int rc=img_ntfs_write_stream(d->ntfs,final,src,meta,&co,prog,&r);
        if(rc==IMG_OK && out_size) *out_size=r.size;
        return rc;
    }
    if(d->loc.kind==IMG_XK_FAT){
        int rc=img_fat32_write_stream(d->fat,final,src,meta,
                                      existed?IMG_COPY_OVERWRITE:IMG_COPY_FAIL,
                                      o->preserve_times,prog,NULL,0,out_size,out_replaced);
        return rc;
    }
    if(d->loc.kind==IMG_XK_HOST){
        char dir[2048]; parent_of(final,dir,sizeof dir);
        if(dir[0]){
            int rc=host_mkdir_p(dir,0755);
            if(rc!=IMG_OK) return rc;
        }
        char tmp[4096];
        int fd=-1;
        {
            /* ★ P1-6：统一命名 imgtool-tmp-hw-<pid>-<i>（同目录，保原子 rename） */
            char dir[2048]; parent_of(final,dir,sizeof dir);
            for(int i=0;i<64;i++){
                snprintf(tmp,sizeof tmp,"%s/imgtool-tmp-hw-%ld-%d",
                         dir[0]?dir:".",(long)getpid(),i);
                fd=open(tmp,O_CREAT|O_EXCL|O_WRONLY,0644);
                if(fd>=0) break;
                if(errno!=EEXIST) return IMG_EIO;
            }
        }
        if(fd<0) return IMG_EIO;
        enum { CH=1u<<20 };
        uint8_t *buf=(uint8_t*)malloc(CH);
        int rc=buf?IMG_OK:IMG_ENOMEM;
        uint64_t off=0;
        if(prog){ img_prog_phase(prog,IMG_PH_EXTRACT,"复制文件");
                  img_prog_op(prog,IMG_OP_WRITE); img_prog_bytes(prog,0,meta->size); }
        while(rc==IMG_OK && off<meta->size){
            if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
            size_t want=(size_t)((meta->size-off<CH)?(meta->size-off):CH);
            size_t got=0;
            rc=src->read(src->ud,off,buf,want,&got);
            if(rc!=IMG_OK) break;
            if(got==0){ rc=IMG_EIO; break; }
            size_t w=0;
            while(w<got){
                ssize_t n=write(fd,buf+w,got-w);
                if(n<=0){ rc=IMG_EIO; break; }
                w+=(size_t)n;
            }
            if(rc!=IMG_OK) break;
            off+=got;
            if(prog) img_prog_bytes(prog,off,meta->size);
        }
        free(buf);
        if(close(fd)!=0 && rc==IMG_OK) rc=IMG_EIO;
        if(rc==IMG_OK && off!=meta->size) rc=IMG_EIO;
        if(rc!=IMG_OK){ unlink(tmp); return rc; }
        if(rename(tmp,final)!=0){ unlink(tmp); return IMG_EIO; }
        if(o->preserve_times) (void)dst_set_times(d,final,meta);
        return IMG_OK;
    }
    return IMG_ENOTSUP;
}

/* 目标目录条目数（给 UI 的 parent_total；失败返回 0 不致命） */
typedef struct { uint64_t n; } cnt_ud;
static void cnt_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    (void)name;(void)is_dir;(void)size; ((cnt_ud*)ud)->n++;
}
static uint64_t dst_count(xep *d, const char *dir){
    if(d->loc.kind==IMG_XK_ARCHIVE)
        return d->aw ? img_aw_count_children(d->aw,dir) : 0;
    if(d->loc.kind==IMG_XK_HOST){
        DIR *dp=opendir(dir);
        if(!dp) return 0;
        uint64_t n=0; struct dirent *de;
        while((de=readdir(dp))) if(strcmp(de->d_name,".")&&strcmp(de->d_name,"..")) n++;
        closedir(dp);
        return n;
    }
    cnt_ud u={0};
    if(d->loc.kind==IMG_XK_NTFS) (void)img_ntfs_list_dir(d->ntfs,dir,cnt_cb,&u);
    else if(d->loc.kind==IMG_XK_FAT) (void)img_fat32_list_dir(d->fat,dir,cnt_cb,&u);
    return u.n;
}

/* NTFS 大小写不敏感：把用户写的路径换成盘上真实大小写，
 * 否则 libntfs-3g 的精确查找会 ENOENT（move_path 内部用的就是精确查找）。*/
typedef struct { const char *want; char *out; size_t cap; int found; } case_ud;
static void case_cb(void *ud, const char *name, bool is_dir, uint64_t size){
    (void)is_dir;(void)size;
    case_ud *c=(case_ud*)ud;
    if(c->found || !name) return;
    if(!strcasecmp(name,c->want)){ snprintf(c->out,c->cap,"%s",name); c->found=1; }
}
static int actual_case_path(xep *e, const char *path, char *out, size_t cap){
    if(e->loc.kind!=IMG_XK_NTFS && e->loc.kind!=IMG_XK_FAT){
        snprintf(out,cap,"%s",path); return IMG_OK;
    }
    char dir[4096], name[1024];
    parent_of(path,dir,sizeof dir);
    xstrlcpy(name,base_of(path),sizeof name);
    if(!name[0] || !strcmp(name,".")){ xstrlcpy(out,path,cap); return IMG_OK; }
    /* ★ 回调把"盘上的真实名字"写进 found，不能直接写 out：
     *   下面还要拼 dir + 名字，写进 out 会被拼装覆盖掉（曾经因此
     *   a.txt→A.TXT 的 actual 一直是用户输入的大小写）。*/
    char found[1024]; found[0]=0;
    case_ud c={name,found,sizeof found,0};
    int lrc = (e->loc.kind==IMG_XK_FAT)
            ? img_fat32_list_dir(e->fat,dir,case_cb,&c)
            : img_ntfs_list_dir(e->ntfs,dir,case_cb,&c);
    if(lrc==IMG_OK && c.found){
        size_t dl=strlen(dir), nl=strlen(found);
        if(strcmp(dir,"/")==0){
            if(nl+2<=cap){ out[0]='/'; memcpy(out+1,found,nl+1); return IMG_OK; }
        } else if(dl+nl+2<=cap){
            memcpy(out,dir,dl); out[dl]='/'; memcpy(out+dl+1,found,nl+1); return IMG_OK;
        }
    }
    xstrlcpy(out,path,cap);
    return IMG_OK;
}

/* ================= 递归复制 ================= */
/* 两个端点是不是"同一个盘 + 同一个分区 + 同一种文件系统"（决定能否共享挂载/判环） */
static bool same_volume(const xep *a, const xep *b){
    if(a->loc.kind==IMG_XK_HOST && b->loc.kind==IMG_XK_HOST) return true;
    if(!a->q || a->q!=b->q) return false;
    return a->loc.kind==b->loc.kind && a->lba_used==b->lba_used;
}
static int path_cmp(const xep *a, const xep *b, const char *x, const char *y){
    /* NTFS/FAT 是大小写不敏感的卷；宿主按字节比 */
    bool ci=(a->loc.kind==IMG_XK_NTFS||a->loc.kind==IMG_XK_FAT||
             b->loc.kind==IMG_XK_NTFS||b->loc.kind==IMG_XK_FAT);
    return ci ? strcasecmp(x,y) : strcmp(x,y);
}
/* target 是否落在 src 目录的子树里（含自身） */
static bool is_under(const xep *s, const xep *d, const char *src, const char *target){
    size_t n=strlen(src);
    while(n>1 && src[n-1]=='/') n--;
    if(path_cmp(s,d,src,target)==0) return true;
    if(strncasecmp(target,src,n)!=0) return false;
    return target[n]=='/';
}
typedef struct {
    xep *s, *d;
    const img_xcopy_opts *o;
    img_prog *prog;
    img_xcopy_stats *st;
    int max_depth;
} xctx;

static int copy_node(xctx *c, const char *spath, const char *dpath, int depth, bool top);

/* 解析"最终目标路径"：dst 是目录（已存在目录 / 以 / 结尾）→ 追加源名 */
static int resolve_final(xctx *c, const char *spath, const char *dpath, bool top,
                         char *final, size_t cap, bool *existed, bool *ex_is_dir){
    const char *sname=base_of(spath);
    size_t dl=strlen(dpath);
    bool dst_is_dir=(dl && dpath[dl-1]=='/');
    bool exists=false, isdir=false;
    if(!dst_is_dir) exists=dst_exists(c->d,dpath,&isdir)!=0;
    if(exists && isdir) dst_is_dir=true;

    if(top && dst_is_dir){
        char dir[2048]; xstrlcpy(dir,dpath,sizeof dir);
        size_t n=strlen(dir);
        while(n>1 && dir[n-1]=='/') dir[--n]=0;
        int rc=join_path(dir,sname,final,cap);
        if(rc!=IMG_OK) return rc;
    } else if(top && !dst_is_dir && !exists){
        /* 目标不存在：按用户给的完整路径写 */
        xstrlcpy(final,dpath,cap);
    } else if(top){
        xstrlcpy(final,dpath,cap);
    } else {
        xstrlcpy(final,dpath,cap);
    }

    bool e=false, ed=false;
    e=dst_exists(c->d,final,&ed)!=0;
    *existed=e; *ex_is_dir=e?ed:false;
    return IMG_OK;
}

/* 冲突处理（仅对"最终路径已存在"的情况）。返回新路径、是否存在。 */
static int apply_conflict(xctx *c, const char *final, bool *existed,
                          bool *ex_is_dir, bool src_is_dir,
                          char *out, size_t cap){
    if(!*existed){ xstrlcpy(out,final,cap); return IMG_OK; }
    if(src_is_dir){
        if(*ex_is_dir){
            if(c->o->conflict==IMG_COPY_FAIL) return IMG_EEXISTS;
            if(c->o->conflict==IMG_COPY_RENAME){
                char dir[2048], base[XC_NAME_MAX], uniq[XC_NAME_MAX];
                parent_of(final,dir,sizeof dir);
                xstrlcpy(base,base_of(final),sizeof base);
                int rc=dst_unique_name(c->d,dir[0]?dir:"/",base,uniq,sizeof uniq);
                if(rc!=IMG_OK) return rc;
                rc=join_path(dir[0]?dir:"/",uniq,out,cap);
                if(rc!=IMG_OK) return rc;
                *existed=false; *ex_is_dir=false;
                return IMG_OK;
            }
            xstrlcpy(out,final,cap);         /* 合并（overwrite 语义） */
            return IMG_OK;
        }
        return IMG_ESTATE;                    /* 文件挡住目录 */
    }
    if(*ex_is_dir) return IMG_ESTATE;         /* 目录挡住文件 */
    if(c->o->conflict==IMG_COPY_FAIL) return IMG_EEXISTS;
    if(c->o->conflict==IMG_COPY_RENAME){
        char dir[2048], base[XC_NAME_MAX], uniq[XC_NAME_MAX];
        parent_of(final,dir,sizeof dir);
        xstrlcpy(base,base_of(final),sizeof base);
        int rc=dst_unique_name(c->d,dir[0]?dir:"/",base,uniq,sizeof uniq);
        if(rc!=IMG_OK) return rc;
        rc=join_path(dir[0]?dir:"/",uniq,out,cap);
        if(rc!=IMG_OK) return rc;
        *existed=false; *ex_is_dir=false;
        return IMG_OK;
    }
    xstrlcpy(out,final,cap);                  /* overwrite */
    return IMG_OK;
}

/* ================= 目标：压缩包（REQ-41 ①） =================
 * 写包只能整包重写（zip 中央目录在尾部、7z/tar 是顺序格式）。所以：
 *   ① 先把"包里已有什么"读成名字表（不读数据）；
 *   ② conflict=overwrite 时走一遍源树，算出"哪些旧条目会被替换" → 重写时跳过它们
 *      （否则包里会出现两条同名，解压时谁赢看实现）；
 *   ③ 开临时包，搬旧条目 → 追加新条目 → rename。
 * conflict=fail / rename 不需要 ②：fail 在第一条冲突处失败（临时包删掉，原包不动），
 * rename 直接挑一个不冲突的新名字。*/
static int x_excluded(const img_xcopy_opts *o, const char *name);   /* 定义在后面 */

typedef struct { char **v; int n, cap; } ac_skip;

static int ac_skip_add(ac_skip *l, const char *p){
    if(l->n>=l->cap){
        int nc=l->cap?l->cap*2:16;
        char **nv=(char**)realloc(l->v,(size_t)nc*sizeof(char*));
        if(!nv) return IMG_ENOMEM;
        l->v=nv; l->cap=nc;
    }
    l->v[l->n]=strdup(p);
    if(!l->v[l->n]) return IMG_ENOMEM;
    l->n++;
    return IMG_OK;
}
static void ac_skip_free(ac_skip *l){
    if(!l) return;
    for(int i=0;i<l->n;i++) free(l->v[i]);
    free(l->v); l->v=NULL; l->n=l->cap=0;
}

/* 源树干跑：只 stat/list，不读数据；把"会被新内容覆盖掉的旧路径"记进 sk。
 * 与 copy_dir/copy_file 的目标路径算法保持一致（都走 resolve_final）。*/
static int arch_plan_node(xctx *c, const img_aw_names *ns, const char *spath, const char *dpath,
                          int depth, bool top, ac_skip *sk){
    if(depth>c->max_depth) return IMG_ESTATE;
    img_src_meta meta; bool isdir=false;
    int rc=src_stat(c->s,spath,&isdir,&meta);
    if(rc!=IMG_OK) return rc;
    char final[4096]; bool existed=false, ed=false;
    rc=resolve_final(c,spath,dpath,top,final,sizeof final,&existed,&ed);
    if(rc!=IMG_OK) return rc;
    if(img_aw_names_has(ns,final)){
        rc=ac_skip_add(sk,final);
        if(rc!=IMG_OK) return rc;
    }
    if(!isdir) return IMG_OK;
    xlist l;
    rc=src_list(c->s,spath,&l);
    if(rc!=IMG_OK) return rc;
    for(int i=0;i<l.n && rc==IMG_OK;i++){
        if(l.v[i].skip) continue;
        if(x_excluded(c->o,l.v[i].name)) continue;
        char cs[4096], cd[4096];
        if(join_path(spath,l.v[i].name,cs,sizeof cs)!=IMG_OK ||
           join_path(final ,l.v[i].name,cd,sizeof cd)!=IMG_OK){ rc=IMG_EINVAL; break; }
        rc=arch_plan_node(c,ns,cs,cd,depth+1,false,sk);
    }
    free(l.v);
    return rc;
}

/* 开写之前：定格式 + 准备名字表 + 开临时包。*/
static int arch_dst_prepare(xctx *c){
    xep *d=c->d;
    char err[256]="";
    img_aw_fmt f=IMG_AW_NONE;
    int rc=img_aw_fmt_of_existing(d->loc.img,&f,err,sizeof err);
    bool exists=(rc==IMG_OK);
    if(rc==IMG_ENOENT){
        f=img_aw_fmt_by_name(d->loc.img);
        if(f==IMG_AW_NONE){
            snprintf(d->aw_err,sizeof d->aw_err,
                     "这个扩展名认不出格式（用 .zip / .7z / .tar / .tgz / .iso）；新建包请用 archive.create");
            return IMG_EINVAL;
        }
    }else if(rc==IMG_ENOTSUP || rc==IMG_EREADONLY){
        snprintf(d->aw_err,sizeof d->aw_err,"%s",err[0]?err:"这个包只能读，不能写（RAR/ISO 请用 7z 或 archive.create）");
        return IMG_EREADONLY;
    }else if(rc!=IMG_OK){
        snprintf(d->aw_err,sizeof d->aw_err,"%s",err[0]?err:"原包无法打开");
        return rc;
    }
    if(!img_aw_writable(f)){
        snprintf(d->aw_err,sizeof d->aw_err,"这个包只能读，不能写（RAR/ISO）");
        return IMG_EREADONLY;
    }

    ac_skip sk={0};
    int nskip=0; const char *const *skv=NULL;
    if(exists && c->o->conflict==IMG_COPY_OVERWRITE){
        rc=img_aw_names_load(d->loc.img,&d->ns,err,sizeof err);
        if(rc!=IMG_OK){
            snprintf(d->aw_err,sizeof d->aw_err,"%s",err[0]?err:"原包无法打开（已损坏？）");
            return (rc==IMG_ENOENT)?IMG_ENOENT:IMG_EFORMAT;
        }
        rc=arch_plan_node(c,d->ns,c->s->root,d->loc.path,0,true,&sk);
        if(rc!=IMG_OK){
            img_aw_names_free(d->ns); d->ns=NULL; ac_skip_free(&sk);
            snprintf(d->aw_err,sizeof d->aw_err,"规划覆盖时失败（源目录读不动？）");
            return rc;
        }
        nskip=sk.n; skv=(const char *const *)sk.v;
    }

    /* 进度总量：源是文件就它的大小；目录算出全部大小（多走一遍，但进度条才有意义）*/
    if(d->aw_io_total==0){
        /* 粗算：源侧逐个 stat 太贵，这里只给"已知文件大小"，目录先给 0（用 item 显示当下文件）*/
        img_src_meta m; bool isdir=false;
        if(src_stat(c->s,c->s->root,&isdir,&m)==IMG_OK && !isdir) d->aw_io_total=m.size;
    }

    int brc=img_aw_begin(d->loc.img,f,5,NULL,true,exists,skv,nskip,false,&d->aw,err,sizeof err);
    ac_skip_free(&sk);
    if(brc!=IMG_OK){
        if(d->ns){ img_aw_names_free(d->ns); d->ns=NULL; }
        snprintf(d->aw_err,sizeof d->aw_err,"%s",err[0]?err:"建临时包失败");
        return (brc==IMG_ENOTSUP)?IMG_EREADONLY:brc;
    }
    return IMG_OK;
}

/* 收尾：成功 → rename；失败 → 删临时文件（原包不动）。*/
static int arch_dst_finish(xep *d, bool ok, uint64_t *out_size){
    if(d->ns){ img_aw_names_free(d->ns); d->ns=NULL; }
    if(!d->aw) return IMG_OK;
    if(!ok){ img_aw_abort(d->aw); d->aw=NULL; return IMG_OK; }
    char err[256]="";
    int rc=img_aw_commit(d->aw,out_size,err,sizeof err);
    d->aw=NULL;
    if(rc!=IMG_OK && d->aw_err[0]==0) snprintf(d->aw_err,sizeof d->aw_err,"%s",err[0]?err:"写包失败");
    return rc;
}

/* 目标名字长度：NTFS/FAT 按字符（255），宿主按字节（255）。超了直接报，不截断。*/
static size_t u8_chars(const char *s){
    size_t n=0;
    for(const unsigned char *p=(const unsigned char*)s; *p; p++)
        if((*p & 0xC0) != 0x80) n++;
    return n;
}
static bool name_too_long(xep *d, const char *path){
    const char *b=base_of(path);
    if(d->loc.kind==IMG_XK_HOST) return strlen(b) > 255;
    return u8_chars(b) > 255;
}

/* Windows 文件名前置校验：只对 NTFS/FAT 目标。宿主按宿主规则（ext4 允许 ':' 等）。
 * ★ 必须在【任何写动作之前】调用 —— 拒绝时盘上不能多出任何东西。*/
static int check_dst_name(xep *d, const char *path, char *why, size_t cap){
    if(!d) return IMG_OK;
    if(d->loc.kind!=IMG_XK_NTFS && d->loc.kind!=IMG_XK_FAT) return IMG_OK;
    const char *b=base_of(path);
    if(!b[0]) return IMG_OK;                 /* 卷根没有末级名字，交给上层判 */
    img_name_status s=img_name_check(b);
    if(s==IMG_NAME_OK) return IMG_OK;
    if(why&&cap) snprintf(why,cap,"%s",img_name_reason(s,b));
    return IMG_ENAMEINVALID;
}

static int copy_file(xctx *c, const char *spath, const char *dpath, bool top){
    img_src_meta meta; bool isdir=false;
    int rc=src_stat(c->s,spath,&isdir,&meta);
    if(rc!=IMG_OK){ c->st->side=1; return rc; }
    if(isdir){
        /* ★ 2026-11-10（REQ-65 §3）：重解析点（junction / mount point / symlink）。
         *   NTFS 目录列表把 \Documents and Settings 这类 junction 报成【文件】
         *   （size=0），到这里 stat 出来才发现是目录。以前直接 IMG_ESTATE，
         *   而它几乎存在于每张 Vista 之后的系统盘上 → 捕获映像 100% 失败。
         *   与宿主源的符号链接同策略：跳过、计数、继续，不复制也不报错。*/
        c->st->skipped++;
        return IMG_OK;
    }

    char final[4096]; bool existed=false, ed=false;
    rc=resolve_final(c,spath,dpath,top,final,sizeof final,&existed,&ed);
    if(rc!=IMG_OK) return rc;
    char target[4096];
    rc=apply_conflict(c,final,&existed,&ed,false,target,sizeof target);
    if(rc!=IMG_OK) return rc;
    rc=check_dst_name(c->d,target,c->st->first_error,sizeof c->st->first_error);
    if(rc!=IMG_OK){ c->st->side=2; xstrlcpy(c->st->bad_name,target,sizeof c->st->bad_name);
                    return rc; }                       /* 零副作用：还没开写 */
    if(name_too_long(c->d,target)){ c->st->side=2; return IMG_ENAMETOOLONG; }
    /* 源和目标是同一个文件：读的同时覆盖会毁掉源。要"自己复制一份"请用 conflict=rename。*/
    if(same_volume(c->s,c->d) && path_cmp(c->s,c->d,spath,target)==0) return IMG_ESAMEFILE;

    if(!c->o->create_parents){
        char par[2048]; parent_of(target,par,sizeof par);
        bool pd=false;
        if(par[0] && !dst_exists(c->d,par,&pd)){ c->st->side=2; return IMG_ENOENT; }
    }

    img_stream st; void *holder=NULL;
    rc=src_open_stream(c->s,spath,&st,&holder);
    if(rc!=IMG_OK){ c->st->side=1; return rc; }
    if(c->prog) img_prog_item(c->prog,spath);
    bool replaced=false;
    rc=dst_write_file(c->d,target,&st,&meta,c->o,c->prog,existed,NULL,&replaced);
    src_close_stream(c->s,holder);
    if(rc!=IMG_OK){ c->st->side=2; return rc; }
    /* NTFS → NTFS 保留命名数据流（ADS）；FAT/宿主没有这个概念，只能丢弃。*/
    if(c->s->loc.kind==IMG_XK_NTFS && c->d->loc.kind==IMG_XK_NTFS){
        int n=0;
        rc=img_ntfs_copy_xattrs(c->s->ntfs,spath,c->d->ntfs,target,&n);
        if(rc!=IMG_OK){ c->st->side=2; return rc; }
        c->st->ads+=(uint64_t)n;
    }
    c->st->files++;
    c->st->bytes+=meta.size;
    if(top){
        xstrlcpy(c->st->final_path,target,sizeof c->st->final_path);
        c->st->replaced=replaced;
        c->st->top_size=meta.size;
        if(c->d->loc.kind==IMG_XK_NTFS){
            uint64_t ino=0; bool id=false;
            if(img_ntfs_verify_path(c->d->ntfs,target,&ino,&id)==IMG_OK) c->st->top_ino=ino;
        }
    }
    return IMG_OK;
}

/* REQ-32：递归子项的排除规则。只认 basename；大小写敏感。
 * 返回 0=要，1=.trashed-*，2=--exclude 命中。*/
static int x_excluded(const img_xcopy_opts *o, const char *name){
    if(!o || !name || !*name) return 0;
    if(o->skip_trashed && !strncmp(name,".trashed-",9)) return 1;
    for(int i=0;i<o->n_exclude;i++)
        if(fnmatch(o->exclude[i],name,0)==0) return 2;
    return 0;
}

static int copy_dir(xctx *c, const char *spath, const char *dpath, int depth, bool top){
    if(depth>c->max_depth) return IMG_EREFUSED;   /* ★ P1：深度超限（原来 ESTATE 会被误报成"目标类型冲突"）*/
    char final[4096]; bool existed=false, ed=false;
    int rc=resolve_final(c,spath,dpath,top,final,sizeof final,&existed,&ed);
    if(rc!=IMG_OK) return rc;
    char target[4096];
    rc=apply_conflict(c,final,&existed,&ed,true,target,sizeof target);
    if(rc!=IMG_OK) return rc;
    rc=check_dst_name(c->d,target,c->st->first_error,sizeof c->st->first_error);
    if(rc!=IMG_OK){ c->st->side=2; xstrlcpy(c->st->bad_name,target,sizeof c->st->bad_name);
                    return rc; }                       /* 零副作用：还没建目录 */
    if(name_too_long(c->d,target)){ c->st->side=2; return IMG_ENAMETOOLONG; }
    /* 造环：把目录复制进它自己/它的子树 —— 会在建任何东西之前拒绝 */
    if(same_volume(c->s,c->d) && is_under(c->s,c->d,spath,target)) return IMG_ECYCLE;

    if(!existed){
        rc=dst_mkdir_p(c->d,target);
        if(rc!=IMG_OK){ c->st->side=2; return rc; }
        c->st->dirs++;
    } else {
        /* 合并：目标已存在同名目录 —— 不重复计数 */
    }

    xlist l;
    rc=src_list(c->s,spath,&l);
    if(rc!=IMG_OK){ c->st->side=1; return rc; }
    char failed[4096]; failed[0]=0;
    for(int i=0;i<l.n && rc==IMG_OK;i++){
        if(l.v[i].skip){ c->st->skipped++; continue; }
        int ex=x_excluded(c->o,l.v[i].name);        /* REQ-32：顶层目录本身不参与（这里只滤子项）*/
        if(ex){ if(ex==1) c->st->skipped_trashed++; else c->st->skipped_excluded++; continue; }
        char cs[4096], cd[4096];
        if(join_path(spath,l.v[i].name,cs,sizeof cs)!=IMG_OK ||
           join_path(target,l.v[i].name,cd,sizeof cd)!=IMG_OK){ rc=IMG_EINVAL; break; }
        if(l.v[i].isdir) rc=copy_dir(c,cs,cd,depth+1,false);
        else             rc=copy_file(c,cs,cd,false);
        if(rc!=IMG_OK) snprintf(failed,sizeof failed,"%s",cs);
    }
    free(l.v);
    if(rc!=IMG_OK){
        /* ★ 2026-11-10（REQ-65 §5）：把出错的那条源路径写进 first_error，
         *   上层（wim.capture）才有东西可显示。以前冒号后面永远是空的。*/
        if(!c->st->first_error[0] && failed[0])
            snprintf(c->st->first_error,sizeof c->st->first_error,"%s",failed);
        return rc;
    }
    if(top) xstrlcpy(c->st->final_path,target,sizeof c->st->final_path);

    /* 目录时间：子树写完之后再设（否则会被子项创建时间改掉） */
    if(c->o->preserve_times){
        img_src_meta dm;
        if(src_stat(c->s,spath,&(bool){false},&dm)==IMG_OK)
            (void)dst_set_times(c->d,target,&dm);
    }
    return IMG_OK;
}

static int copy_node(xctx *c, const char *spath, const char *dpath, int depth, bool top){
    img_src_meta meta; bool isdir=false;
    int rc=src_stat(c->s,spath,&isdir,&meta);
    if(rc!=IMG_OK) return rc;
    if(isdir){
        if(!c->o->recursive) return IMG_ENOTSUP;   /* 政策拒绝：零副作用 */
        return copy_dir(c,spath,dpath,depth,top);
    }
    return copy_file(c,spath,dpath,top);
}

/* ================= 解析 ================= */
const char *img_xkind_name(img_xkind k){
    switch(k){
    case IMG_XK_NTFS: return "ntfs";
    case IMG_XK_FAT:  return "fat";
    case IMG_XK_HOST: return "host";
    case IMG_XK_ISO:  return "iso";
    case IMG_XK_WIM:  return "wim";
    default: return "?";
    }
}

int img_xcopy_parse(const char *spec, img_xloc *out, char *why, size_t whycap){
    if(!spec||!out) return IMG_EINVAL;
    if(why&&whycap) why[0]=0;
    memset(out,0,sizeof *out);
    out->index=1;
    const char *c=strchr(spec,':');
    if(!c || c==spec){
        if(why) snprintf(why,whycap,"位置串要写成 ntfs:/路径、fat:/路径、host:/路径、iso:/路径、wim:/路径");
        return IMG_EINVAL;
    }
    const char *at=strchr(spec,'@');
    if(at && at>c) at=NULL;
    size_t pl=(size_t)((at?at:c)-spec);
    if(pl==4 && !strncasecmp(spec,"ntfs",4)) out->kind=IMG_XK_NTFS;
    else if(pl==3 && !strncasecmp(spec,"fat",3)) out->kind=IMG_XK_FAT;
    else if(pl==4 && !strncasecmp(spec,"host",4)) out->kind=IMG_XK_HOST;
    else if(pl==3 && !strncasecmp(spec,"iso",3)) out->kind=IMG_XK_ISO;
    else if(pl==3 && !strncasecmp(spec,"wim",3)) out->kind=IMG_XK_WIM;
    else { if(why) snprintf(why,whycap,"不认识的前缀 \"%.*s\"（只支持 ntfs/fat/host/iso/wim）",(int)pl,spec); return IMG_EINVAL; }

    const char *p=c+1;
    /* 允许把分区号写进位置串：ntfs@165888:/path、fat@2048:/path。
     * 批量粘贴（--list_file）时每行都可能指向不同分区，全局 --dst_lba 不够用。*/
    if((out->kind==IMG_XK_NTFS||out->kind==IMG_XK_FAT) && at){
        char *end=NULL;
        unsigned long long lba=strtoull(at+1,&end,0);
        if(end && end==c && end>at+1) out->lba=(uint64_t)lba;
    }
    /* wim 支持 wim:1:/路径 —— 索引可选 */
    if(out->kind==IMG_XK_WIM){
        if(p[0]>='1'&&p[0]<='9'){
            char *end=NULL;
            long idx=strtol(p,&end,10);
            if(end && *end==':'){ out->index=(int)idx; p=end+1; }
        }
    }
    if(!*p){ if(why) snprintf(why,whycap,"路径为空"); return IMG_EINVAL; }
    if(p[0]!='/'){
        if(why) snprintf(why,whycap,"路径必须以 / 开头（收到 \"%s\"）",p);
        return IMG_EINVAL;
    }
    xstrlcpy(out->path,p,sizeof out->path);
    return IMG_OK;
}

/* ================= 入口 ================= */
int img_xcopy_run(const img_xloc *src, const img_xloc *dst,
                  const img_xcopy_opts *opt, img_prog *prog,
                  img_xcopy_stats *st){
    img_xcopy_stats sink; if(!st) st=&sink;
    memset(st,0,sizeof *st);
    if(!src||!dst) return IMG_EINVAL;

    img_xcopy_opts o = { IMG_COPY_FAIL, true, true, true, false, XC_MAX_DEPTH, false, {{0}}, 0 };
    if(opt) o=*opt;
    if(o.max_depth<=0 || o.max_depth>256) o.max_depth=XC_MAX_DEPTH;

    int rc=0;
    if(dst->kind==IMG_XK_ISO || dst->kind==IMG_XK_WIM) return IMG_ENOTSUP;

    /* 同一张镜像 → 必须共用一个 img_ctx（两个句柄写同一文件 = 必炸） */
    bool same=false;
    img_xloc s=*src, d=*dst;
    /* 源/目标位置里的 img 由命令层填好 */
    if(s.img[0] && d.img[0] &&
       (s.kind==IMG_XK_NTFS||s.kind==IMG_XK_FAT) &&
       (d.kind==IMG_XK_NTFS||d.kind==IMG_XK_FAT)){
        char ra[PATH_MAX], rb[PATH_MAX];
        if(realpath(s.img,ra) && realpath(d.img,rb)) same=(strcmp(ra,rb)==0);
        else same=(strcmp(s.img,d.img)==0);
    }

    img_ctx *shared=NULL;
    if(same){
        char err[256]={0};
        shared=img_ctx_open(d.img,true,false,err,sizeof err);
        if(!shared) return IMG_ENOENT;
    }

    xep se, de;
    memset(&se,0,sizeof se); memset(&de,0,sizeof de);
    rc=ep_open(&de,&d,shared,true,NULL);
    if(rc!=IMG_OK){ st->side=2; goto out; }
    rc=ep_open(&se,&s,shared,false,&de);
    if(rc!=IMG_OK){ st->side=1; goto out; }

    /* 源是目录但没开 recursive：在写任何东西之前拒绝 */
    {
        img_src_meta m; bool isdir=false;
        rc=src_stat(&se,se.root,&isdir,&m);
        if(rc!=IMG_OK){ st->side=1; goto out; }
        if(isdir && !o.recursive){ rc=IMG_ENOTSUP; goto out; }
    }

    if(prog){ img_prog_phase(prog,IMG_PH_COPY,"复制");
              img_prog_op(prog,IMG_OP_WRITE); }
    {
        xctx c; memset(&c,0,sizeof c);
        c.s=&se; c.d=&de; c.o=&o; c.prog=prog; c.st=st; c.max_depth=o.max_depth;
        /* ★ REQ-41：目标是压缩包 → 先开临时包（可能已把旧条目搬进来），复制走同一套
         *   copy_node；成功才 rename，失败/取消临时文件删掉、原包字节不变。*/
        if(de.loc.kind==IMG_XK_ARCHIVE){
            rc=arch_dst_prepare(&c);
            if(rc!=IMG_OK){
                st->side=2;
                if(de.aw_err[0]) xstrlcpy(st->first_error,de.aw_err,sizeof st->first_error);
            }
        }
        if(rc==IMG_OK) rc=copy_node(&c,se.root,de.loc.path,0,true);
        if(rc==IMG_OK && st->final_path[0]){       /* 必须在收尾之前数（收尾后句柄没了）*/
            char par[2048]; xstrlcpy(par,st->final_path,sizeof par);
            char *sl=strrchr(par,'/');
            if(sl){ if(sl==par) par[1]=0; else *sl=0; st->parent_total=dst_count(&de,par); }
        }
        if(de.loc.kind==IMG_XK_ARCHIVE){
            uint64_t wsz=0;
            int frc=arch_dst_finish(&de, rc==IMG_OK, &wsz);
            if(rc==IMG_OK && frc!=IMG_OK){
                rc=frc; st->side=2;
                if(de.aw_err[0]) xstrlcpy(st->first_error,de.aw_err,sizeof st->first_error);
            }
            if(rc==IMG_OK) st->archive_bytes=wsz;
        }
    }
    if(rc==IMG_OK && prog) img_prog_done(prog,"复制完成");

out:
    ep_close(&se);
    ep_close(&de);
    if(shared) img_ctx_close(shared);
    return rc;
}

/* ================= 统一移动 / 删除（2026-10-24） ================= */

/* 删除前拒绝这些"一删就全没"的宿主路径（即使调用方开了 allow_host_delete） */
static bool host_dangerous(const char *p){
    if(!p || !*p) return true;
    static const char *bad[] = {
        "/", "/storage", "/storage/emulated", "/storage/emulated/0",
        "/sdcard", "/data", "/system", NULL
    };
    for(int i=0; bad[i]; i++) if(!strcmp(p,bad[i])) return true;
    return false;
}

static int host_remove_count(const char *path, bool recursive, uint64_t *removed){
    struct stat st;
    if(lstat(path,&st)!=0) return IMG_ENOENT;
    if(S_ISDIR(st.st_mode)){
        if(!recursive){
            if(rmdir(path)!=0) return (errno==ENOTEMPTY)?IMG_ESTATE:IMG_EIO;
            if(removed) (*removed)++;
            return IMG_OK;
        }
        DIR *d=opendir(path);
        if(d){
            struct dirent *de;
            while((de=readdir(d))){
                if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
                char c[4096]; snprintf(c,sizeof c,"%s/%s",path,de->d_name);
                int rc=host_remove_count(c,true,removed);
                if(rc!=IMG_OK){ closedir(d); return rc; }
            }
            closedir(d);
        }
        if(rmdir(path)!=0) return IMG_EIO;
        if(removed) (*removed)++;
        return IMG_OK;
    }
    if(unlink(path)!=0) return IMG_EIO;
    if(removed) (*removed)++;
    return IMG_OK;
}

static int ep_remove(xep *e, const char *path, bool recursive, uint64_t *removed){
    if(e->loc.kind==IMG_XK_HOST)
        return host_remove_count(path,recursive,removed);
    img_src_meta m; bool isdir=false;
    int rc=src_stat(e,path,&isdir,&m);
    if(rc!=IMG_OK) return rc;
    if(e->loc.kind==IMG_XK_NTFS){
        if(isdir){
            if(!recursive){
                rc=img_ntfs_rm(e->ntfs,path);
                if(rc==IMG_OK && removed) (*removed)++;
                return rc;
            }
            int n=0; rc=img_ntfs_rm_tree(e->ntfs,path,&n);
            if(rc==IMG_OK && removed) *removed += (uint64_t)n;
            return rc;
        }
        rc=img_ntfs_rm(e->ntfs,path);
        if(rc==IMG_OK && removed) (*removed)++;
        return rc;
    }
    if(e->loc.kind==IMG_XK_FAT){
        if(isdir){
            if(!recursive){
                rc=img_fat32_rm(e->fat,path);
                if(rc==IMG_OK && removed) (*removed)++;
                return rc;
            }
            int n=0; rc=img_fat32_rm_tree(e->fat,path,&n);
            if(rc==IMG_OK && removed) *removed += (uint64_t)n;
            return rc;
        }
        rc=img_fat32_rm(e->fat,path);
        if(rc==IMG_OK && removed) (*removed)++;
        return rc;
    }
    return IMG_EREADONLY;
}

/* 宿主：按"父目录所在文件系统"判断能不能直接 rename。
 * 不能用 realpath(文件) 比较 —— 那是判断"同一个文件"，不是"同一个盘"。*/
static bool host_parent_dev(const char *p, dev_t *out){
    char dir[4096]; parent_of(p,dir,sizeof dir);
    if(!dir[0]){ snprintf(dir,sizeof dir,"/"); }
    struct stat st;
    while(stat(dir,&st)!=0 || !S_ISDIR(st.st_mode)){
        char up[4096]; parent_of(dir,up,sizeof up);
        if(!up[0] || !strcmp(up,dir)) break;
        snprintf(dir,sizeof dir,"%s",up);
    }
    if(stat(dir,&st)!=0) return false;
    *out=st.st_dev;
    return true;
}
static bool loc_same_volume(const img_xloc *a, const img_xloc *b){
    if(a->kind!=b->kind) return false;
    if(a->kind==IMG_XK_HOST){
        dev_t da=0, db=0;
        if(!host_parent_dev(a->path,&da) || !host_parent_dev(b->path,&db)) return false;
        return da==db;
    }
    if(a->kind!=IMG_XK_NTFS && a->kind!=IMG_XK_FAT) return false;
    if(a->lba!=b->lba) return false;
    char ra[PATH_MAX], rb[PATH_MAX];
    if(realpath(a->img,ra) && realpath(b->img,rb)) return strcmp(ra,rb)==0;
    return strcmp(a->img,b->img)==0;
}

int img_xmove_run(const img_xloc *src, const img_xloc *dst,
                  const img_xmove_opts *opt, img_prog *prog,
                  img_xmove_stats *st){
    img_xmove_stats sink; if(!st) st=&sink;
    memset(st,0,sizeof *st);
    if(!src||!dst) return IMG_EINVAL;
    img_xmove_opts o={ IMG_COPY_FAIL, true, true, true, false, true, XC_MAX_DEPTH };
    if(opt) o=*opt;
    if(o.max_depth<=0 || o.max_depth>256) o.max_depth=XC_MAX_DEPTH;

    if(src->kind==IMG_XK_ISO || src->kind==IMG_XK_WIM || src->kind==IMG_XK_ARCHIVE)
        return IMG_EREADONLY;      /* 压缩包只能"复制出来"，不能 move（删不了包内条目）*/
    if(dst->kind==IMG_XK_ISO || dst->kind==IMG_XK_WIM) return IMG_EREADONLY;

    if(loc_same_volume(src,dst)){
        /* ★ REQ-61 §2：st_dev 相同不代表能 rename —— Android 上
         *   /data/user/0/<pkg>/files 与 /storage/emulated/0/Android/data/<pkg>/files
         *   是 bind mount：同 st_dev、跨挂载点，rename 必 EXDEV。
         *   此时退出同卷分支，落到下面的「复制 → 校验 → 删源」。 */
        bool exdev = false;
        xep se,de; memset(&se,0,sizeof se); memset(&de,0,sizeof de);
        img_ctx *shared=NULL;
        bool same_img=false;
        if(src->kind==IMG_XK_NTFS || src->kind==IMG_XK_FAT){
            char ra[PATH_MAX], rb[PATH_MAX];
            if(realpath(src->img,ra) && realpath(dst->img,rb)) same_img=(strcmp(ra,rb)==0);
            else same_img=(strcmp(src->img,dst->img)==0);
        }
        int rc=0;
        if(same_img){
            char err[256]={0};
            shared=img_ctx_open(dst->img,true,false,err,sizeof err);
            if(!shared) return IMG_ENOENT;
        }
        rc=ep_open(&de,dst,shared,true,NULL);
        if(rc!=IMG_OK){ if(shared) img_ctx_close(shared); return rc; }
        rc=ep_open(&se,src,shared,true,&de);
        if(rc!=IMG_OK){ ep_close(&de); if(shared) img_ctx_close(shared); return rc; }

        img_xcopy_opts co={ o.conflict, o.recursive, o.preserve_times, o.create_parents, false, o.max_depth, false, {{0}}, 0 };
        xctx c; memset(&c,0,sizeof c);
        c.s=&se; c.d=&de; c.o=&co; c.prog=prog; c.max_depth=o.max_depth;
        img_xcopy_stats tmp; memset(&tmp,0,sizeof tmp); c.st=&tmp;

        img_src_meta meta; bool isdir=false;
        rc=src_stat(&se,se.root,&isdir,&meta);
        if(getenv("IMG_XCOPY_DEBUG")) fprintf(stderr,"[mv] src_stat rc=%d root=%s\n",rc,se.root);
        if(rc==IMG_OK){
            char final[4096]; bool existed=false, ed=false;
            /* 先把"用户写的源路径"换成盘上真实大小写（NTFS/FAT）；
             * 否则 a.txt→A.TXT 这种操作的比较对象是错的。*/
            char sactual[4096];
            actual_case_path(&se,se.root,sactual,sizeof sactual);
            /* 目标路径与源只差大小写（a.txt → A.TXT）：这就是"改名"，不能走
             * resolve_final —— 那里会把已存在的同名目录当成"搬进去"，
             * 目录大小写改名就会被拼成 /Dir1/DIR1 并误报 COPY-INTO-SELF。*/
            bool ci_target = (se.loc.kind==IMG_XK_NTFS || se.loc.kind==IMG_XK_FAT) &&
                             !strcasecmp(sactual,de.loc.path) && strcmp(sactual,de.loc.path);
            /* 盘上真实路径就等于目标（用户大小写写错但本来就是那个名字）⇒ 交给 no-op 分支 */
            bool same_target = !strcmp(sactual,de.loc.path);
            if(ci_target || same_target) snprintf(final,sizeof final,"%s",de.loc.path);
            else rc=resolve_final(&c,sactual,de.loc.path,true,final,sizeof final,&existed,&ed);
            if(getenv("IMG_XCOPY_DEBUG")) fprintf(stderr,"[mv] resolve rc=%d final=%s existed=%d actual=%s\n",rc,final,existed,sactual);
            char target[4096];
            bool case_only=false;
            if(rc==IMG_OK){
                /* mv a a：no-op 成功（与 shell 行为一致）；必须在冲突判定之前。*/
                if(!strcmp(sactual,final)){
                    snprintf(st->mode,sizeof st->mode,"rename");
                    st->files=isdir?0:1; st->dirs=isdir?1:0;
                    st->bytes=isdir?0:meta.size;
                    st->noop=true;
                    size_t fl=strlen(final);
                    if(fl>=sizeof st->final_path) fl=sizeof st->final_path-1;
                    memcpy(st->final_path,final,fl); st->final_path[fl]=0;
                    rc=IMG_OK;
                    goto mv_done;
                }
                /* 大小写改名：NTFS/FAT 都大小写不敏感，按"已存在"拒绝会把正常操作判死。
                 * 必须【先于】造环判定：is_under 是大小写无关的，a→A 会被误判成"搬进自己"。
                 * （FAT 侧由 img_fat32_rename 用"临时名 → 目标名"实现，见该函数注释）*/
                case_only = (se.loc.kind==IMG_XK_NTFS || se.loc.kind==IMG_XK_FAT) &&
                            !strcasecmp(sactual,final) && strcmp(sactual,final);
            }
            if(rc==IMG_OK && !case_only && isdir && is_under(&se,&de,sactual,final)){
                rc=IMG_ECYCLE;   /* 目录搬进自己/自己的子树：写之前拒绝 */
            }
            if(getenv("IMG_XCOPY_DEBUG")) fprintf(stderr,"[mv] case_only=%d kind=%d final=%s root=%s\n",(int)case_only,(int)se.loc.kind,final,se.root);
            if(rc==IMG_OK && case_only){
                snprintf(target,sizeof target,"%s",final);
                existed=false;
            } else if(rc==IMG_OK){
                rc=apply_conflict(&c,final,&existed,&ed,isdir,target,sizeof target);
            }
            if(rc==IMG_OK && case_only){
                st->case_only=true;
                if(se.loc.kind==IMG_XK_NTFS)
                    rc=img_ntfs_move_path(se.ntfs,sactual,target,true,true);
                else if(se.loc.kind==IMG_XK_FAT)
                    rc=img_fat32_rename(se.fat,sactual,target);
                else rc=IMG_EEXISTS;
            }
            if(rc==IMG_OK && !case_only){
                /* Windows 非法名字：改名/移动的目标末级名先校验，拒绝时零副作用 */
                rc=check_dst_name(&de,target,st->first_error,sizeof st->first_error);
                if(rc!=IMG_OK){ st->side=2;
                                xstrlcpy(st->bad_name,target,sizeof st->bad_name);
                                goto mv_done; }
                /* create_parents=false 且目标父目录不存在：必须在写之前拒绝，
                 * 并把责任归到目标侧（否则会误报"源不存在"）。*/
                if(!o.create_parents){
                    char tdir[4096]; parent_of(target,tdir,sizeof tdir);
                    bool td=false;
                    if(tdir[0] && !dst_exists(&de,tdir,&td)){
                        rc=IMG_ENOENT; st->side=2; goto mv_done;
                    }
                }
                if(se.loc.kind==IMG_XK_NTFS){
                    rc=img_ntfs_move_path(se.ntfs,sactual,target,
                                          o.conflict==IMG_COPY_OVERWRITE && existed,true);
                } else if(se.loc.kind==IMG_XK_FAT){
                    if(o.conflict==IMG_COPY_OVERWRITE && existed){
                        int rr=img_fat32_rm(se.fat,target);
                        if(rr!=IMG_OK && rr!=IMG_ENOENT) rc=rr;
                    }
                    if(rc==IMG_OK) rc=img_fat32_rename(se.fat,sactual,target);
                } else if(se.loc.kind==IMG_XK_HOST){
                    if(o.create_parents){
                        char pdir[4096]; parent_of(target,pdir,sizeof pdir);
                        if(pdir[0]) host_mkdir_p(pdir,0755);
                    }
                    if(rename(se.root,target)!=0){
                        if(errno==EXDEV){ exdev=true; rc=IMG_OK; goto mv_done; }
                        rc=(errno==ENOENT)?IMG_ENOENT:IMG_EIO;
                    }
                } else {
                    rc=IMG_ENOTSUP;
                }
            }
            if(rc==IMG_OK){
                bool pd=false;
                if(dst_exists(&de,target,&pd)==0) rc=IMG_ECORRUPT;   /* 写后自证 */
                char par_s[4096], par_t[4096];
                parent_of(se.root,par_s,sizeof par_s);
                parent_of(target,par_t,sizeof par_t);
                snprintf(st->mode,sizeof st->mode,"%s",
                         strcmp(par_s,par_t)?"move":"rename");
                st->deleted = true;      /* 同卷：旧名字已消失（no-op 在上面提前返回）*/
                st->files = isdir?0:1;
                st->dirs  = isdir?1:0;
                st->bytes = isdir?0:meta.size;
                size_t tl=strlen(target);
                if(tl>=sizeof st->final_path) tl=sizeof st->final_path-1;
                memcpy(st->final_path,target,tl); st->final_path[tl]=0;
            }
        }
mv_done:
        if(getenv("IMG_XCOPY_DEBUG")) fprintf(stderr,"[mv] final rc=%d exdev=%d\n",rc,(int)exdev);
        ep_close(&se); ep_close(&de);
        if(shared) img_ctx_close(shared);
        if(exdev) goto xcross;          /* 端点已关，走跨卷兜底 */
        return rc;
    }

xcross:

    /* ---- 跨卷 / 跨镜像 / 宿主↔镜像：复制 → 校验 → 删源 ---- */
    if(src->kind==IMG_XK_HOST && !o.allow_host_delete) return IMG_EREFUSED;

    /* 先看源是文件还是目录，决定复制是否允许、以及删源用哪种方式 */
    xep se; memset(&se,0,sizeof se);
    int rc=ep_open(&se,src,NULL,true,NULL);
    if(rc!=IMG_OK) return rc;
    img_src_meta smeta; bool sdir=false;
    rc=src_stat(&se,se.root,&sdir,&smeta);
    ep_close(&se);
    if(rc!=IMG_OK) return rc;
    if(sdir && !o.recursive) return IMG_ENOTSUP;

    img_xcopy_opts co={ o.conflict, o.recursive, o.preserve_times, o.create_parents, false, o.max_depth, false, {{0}}, 0 };
    img_xcopy_stats cs; memset(&cs,0,sizeof cs);
    rc=img_xcopy_run(src,dst,&co,prog,&cs);
    if(rc!=IMG_OK) return rc;
    if(img_cancelled()) return IMG_ECANCELLED;
    if(o.verify && !sdir && cs.bytes!=smeta.size) return IMG_ECORRUPT;

    /* 删源：重新打开（复制过程已关闭端点） */
    xep de; memset(&de,0,sizeof de);
    rc=ep_open(&de,src,NULL,true,NULL);
    if(rc!=IMG_OK) return IMG_EPARTIAL;
    uint64_t removed=0;
    rc=ep_remove(&de,de.root,o.recursive,&removed);
    ep_close(&de);
    st->files=cs.files; st->dirs=cs.dirs; st->bytes=cs.bytes; st->ads=cs.ads;
    snprintf(st->mode,sizeof st->mode,"cross_volume");
    snprintf(st->final_path,sizeof st->final_path,"%s",cs.final_path);
    if(rc!=IMG_OK){ st->deleted=false; return IMG_EPARTIAL; }
    st->deleted=true;
    return IMG_OK;
}

int img_xrm_run(const img_xloc *loc, bool recursive, bool allow_host_delete,
                uint64_t *removed){
    if(removed) *removed=0;
    if(!loc) return IMG_EINVAL;
    if(loc->kind==IMG_XK_ISO || loc->kind==IMG_XK_WIM) return IMG_EREADONLY;
    if(loc->kind==IMG_XK_HOST){
        if(!allow_host_delete) return IMG_EREFUSED;
        /* 用 realpath 做"危险根"判定（能吃掉 .. 和符号链接），
         * 但真正删除仍用原路径（避免把"删链接"变成"删链接指向的目录"）。*/
        char canon[PATH_MAX];
        if(realpath(loc->path,canon)){
            if(host_dangerous(canon)) return IMG_EREFUSED;
        } else if(host_dangerous(loc->path)) return IMG_EREFUSED;
        if(host_dangerous(loc->path)) return IMG_EREFUSED;
        return host_remove_count(loc->path,recursive,removed);
    }
    if(loc->kind==IMG_XK_ARCHIVE){
        /* REQ-41 ①：rm 包内条目 = 重写整包（跳过该路径/子树）。
         * 先看包的格式：rar/cab/iso → 只读，原包不动。*/
        char err[256]="";
        img_aw_fmt f=IMG_AW_NONE;
        int rc=img_aw_fmt_of_existing(loc->img,&f,err,sizeof err);   /* ★ loc->img 才是包文件 */
        if(rc==IMG_ENOENT) return IMG_ENOENT;
        if(rc!=IMG_OK || !img_aw_writable(f)) return IMG_EREADONLY;
        img_aw_names *ns=NULL;
        rc=img_aw_names_load(loc->img,&ns,err,sizeof err);
        if(rc==IMG_ENOENT) return IMG_ENOENT;
        if(rc!=IMG_OK) return rc;
        bool isdir=false;
        int ex=img_aw_names_lookup(ns,loc->path,&isdir);
        uint64_t kids=img_aw_names_count_children(ns,loc->path);
        img_aw_names_free(ns);
        if(!ex) return IMG_ENOENT;
        if(isdir && !recursive && kids>0) return IMG_ESTATE;   /* 非空目录：不许删 */
        img_aw *w=NULL;
        const char *skip[1]={loc->path};
        rc=img_aw_begin(loc->img,f,5,NULL,true,true,skip,1,true,&w,err,sizeof err);
        if(rc!=IMG_OK) return (rc==IMG_ENOTSUP)?IMG_EREADONLY:rc;
        uint64_t sz=0;
        rc=img_aw_commit(w,&sz,err,sizeof err);
        if(rc!=IMG_OK) return rc;
        if(removed) *removed = isdir ? (kids+1) : 1;
        return IMG_OK;
    }
    xep e; memset(&e,0,sizeof e);
    int rc=ep_open(&e,loc,NULL,true,NULL);
    if(rc!=IMG_OK) return rc;
    rc=ep_remove(&e,e.root,recursive,removed);
    ep_close(&e);
    return rc;
}
