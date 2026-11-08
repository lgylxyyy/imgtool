/* img_rar.c — RAR 读取（REQ-50），解码器用 third_party/unrar 的官方 C API（dll.hpp）
 *
 * 为什么不用 libarchive：它的 RAR 解码只覆盖"能流式解"的子集，碰到 solid 包
 *   直接报 "RAR solid archive support unavailable"（用户那个 221 MB 的 CS1.6 包）。
 *   unrar 官方解码器 solid / RAR5 / 分卷 / 加密全都覆盖，并且带一个稳定的 C 接口。
 *
 * 设计：
 *   · 列目录只用 RAR_OM_LIST 扫头，不解压；
 *   · 取数据/抽取用 RAR_OM_EXTRACT + UCM_PROCESSDATA 回调，文件由我们写：
 *     路径穿越、取消、进度全在手里，不交给 unrar 的 DestPath；
 *   · solid：前面的条目用 RAR_SKIP 跳过（unrar 内部顺序解压），所以"取最后一个文件"
 *     会慢——格式决定的，进度按已处理字节报；
 *   · 加密：UCM_NEEDPASSWORD 直接返回 0（我们不做密码）→ IMG_ENOTSUP，
 *     命令层翻成 ARCHIVE-ENCRYPTED，绝不说成"格式坏了"。
 *
 * license：third_party/unrar/license.txt（UnRAR license：可自由分发，
 *   但不能用于制作 RAR 压缩器；本工具只读，不提供 RAR 写端）。
 */
#include "img_rar.h"
#include "img_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <wchar.h>

#if IMG_WITH_RAR

/* dll.hpp 在 _UNIX 下才定义 HANDLE/CALLBACK/PASCAL/LPARAM → 必须先定义再包含。
 * 只在【本文件】定义，避免污染其它源文件。*/
#ifndef _UNIX
#define _UNIX 1
#endif
#include "dll.hpp"

/* ---------------- 小工具 ---------------- */
/* wchar_t（Linux 上是 UTF-32）→ UTF-8。unrar 的 char 版走 wcsrtombs，受 locale
 * 影响（C locale 下中文会变 '?'），所以只用 FileNameW。*/
static void w2u(const wchar_t *w, char *out, size_t cap){
    size_t k=0;
    if(!cap) return;
    out[0]=0;
    for(; w && *w; w++){
        uint32_t c=(uint32_t)*w;
        char b[4]; int n=0;
        if(c<0x80) b[n++]=(char)c;
        else if(c<0x800){ b[n++]=(char)(0xC0|(c>>6)); b[n++]=(char)(0x80|(c&0x3F)); }
        else if(c<0x10000){ b[n++]=(char)(0xE0|(c>>12)); b[n++]=(char)(0x80|((c>>6)&0x3F)); b[n++]=(char)(0x80|(c&0x3F)); }
        else { b[n++]=(char)(0xF0|(c>>18)); b[n++]=(char)(0x80|((c>>12)&0x3F));
               b[n++]=(char)(0x80|((c>>6)&0x3F)); b[n++]=(char)(0x80|(c&0x3F)); }
        if(k+(size_t)n+1>=cap) break;
        memcpy(out+k,b,(size_t)n); k+=(size_t)n;
    }
    out[k]=0;
}
/* RAR 的 FileTime 是 DOS 时间（高 16 位日期、低 16 位时间）→ unix 秒 */
static uint64_t dos_to_unix(unsigned int ft){
    if(!ft) return 0;
    int t=(int)(ft & 0xFFFF), d=(int)(ft >> 16);
    struct tm tm; memset(&tm,0,sizeof tm);
    tm.tm_year=((d>>9)&0x7F)+80;
    tm.tm_mon =((d>>5)&0x0F)-1;
    tm.tm_mday=(d&0x1F);
    tm.tm_hour=(t>>11)&0x1F;
    tm.tm_min =(t>>5)&0x3F;
    tm.tm_sec =(t&0x1F)*2;
    if(tm.tm_mon<0 || tm.tm_mon>11 || tm.tm_mday<1 || tm.tm_mday>31) return 0;
    time_t v=mktime(&tm);
    return v==(time_t)-1 ? 0 : (uint64_t)v;
}
static void norm_path(const char *in, char *out, size_t cap){
    while(in[0]=='.' && in[1]=='/') in+=2;
    while(*in=='/') in++;
    snprintf(out,cap,"%s",in?in:"");
    size_t n=strlen(out);
    while(n>1 && out[n-1]=='/') out[--n]=0;
    if(!n){ snprintf(out,cap,"/"); return; }
    if(out[0]!='/'){ memmove(out+1,out,n+1); out[0]='/'; }
}
static bool name_under(const char *name, const char *want){
    if(!strcmp(want,"/")) return true;
    size_t wl=strlen(want);
    if(!strncmp(name,want,wl) && (name[wl]==0 || name[wl]=='/')) return true;
    return false;
}
static void rel_path(const char *name, const char *want, char *out, size_t cap){
    const char *p=name;
    if(!strcmp(want,"/")){ while(*p=='/') p++; }
    else { p=name+strlen(want); while(*p=='/') p++; }
    snprintf(out,cap,"%s",p);
}
static int mkdir_p(const char *path){
    char tmp[4096]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char *p=tmp+1; *p; p++) if(*p=='/'){ *p=0; mkdir(tmp,0755); *p='/'; }
    if(mkdir(tmp,0755)!=0 && errno!=EEXIST) return IMG_EIO;
    return IMG_OK;
}
static bool has_dotdot(const char *rel){
    const char *p=rel;
    while(p && *p){
        if(p[0]=='.' && p[1]=='.' && (p[2]=='/' || p[2]==0)) return true;
        p=strchr(p,'/'); if(!p) break; p++;
    }
    return false;
}

/* ---------------- 读回调 ---------------- */
typedef struct {
    img_rar_data_cb cb;
    void            *ud;
    uint64_t         bytes;
    img_prog        *prog;
    uint64_t         total;
    bool             cancelled;
    bool             need_password;
    bool             write_failed;
} rar_rd;

static int CALLBACK rar_callback(UINT msg, LPARAM user, LPARAM p1, LPARAM p2){
    rar_rd *c=(rar_rd*)user;
    /* Open 阶段（rx.Callback）还没有 rd 上下文：user==0。这里必须容忍，
     * 否则一开 solid 包就踩空指针（V1 实测 SIGSEGV）。*/
    if(!c) return (msg==UCM_PROCESSDATA) ? 1 : 0;
    switch(msg){
    case UCM_PROCESSDATA:
        if(img_cancelled()){ c->cancelled=true; return -1; }
        if(p2>0){
            if(c->cb && c->cb(c->ud,(const void*)p1,(size_t)p2)!=0){ c->write_failed=true; return -1; }
            c->bytes+=(uint64_t)p2;
            if(c->prog) img_prog_bytes(c->prog, c->bytes, c->total?c->total:0);
        }
        return 1;
    case UCM_CHANGEVOLUME:
        /* ★ 分卷（.part1.rar / .r00）：unrar 问"下一卷在哪"。我们不做交互，
         *   直接让它按自己的默认命名规则去找（返回 1 = "就用你算出来的名字"）。
         *   找不到就会以 ERAR_EOPEN 失败，错误信息里带卷名，够定位。*/
        return (p2==RAR_VOL_ASK) ? 1 : 0;
    case UCM_NEEDPASSWORD:
    case UCM_NEEDPASSWORDW:
        c->need_password=true;
        return 0;
    default:
        return 0;
    }
}
static HANDLE rar_open(const char *path, int mode, unsigned int *flags,
                       char *err, size_t cap, bool *pw){
    struct RAROpenArchiveDataEx rx;
    memset(&rx,0,sizeof rx);
    rx.ArcName=(char*)path;
    rx.OpenMode=(unsigned int)mode;
    HANDLE h=RAROpenArchiveEx(&rx);
    if(pw) *pw=false;
    if(!h){
        if(pw && rx.OpenResult==ERAR_MISSING_PASSWORD) *pw=true;
        switch(rx.OpenResult){
        case ERAR_EOPEN:            snprintf(err,cap,"RAR 打不开（路径/权限）：%s",path); break;
        case ERAR_BAD_ARCHIVE:      snprintf(err,cap,"不是 RAR 或文件被截断：%s",path); break;
        case ERAR_MISSING_PASSWORD: snprintf(err,cap,"这个 RAR 有密码（不支持解密，需要密码才能读）"); break;
        case ERAR_LARGE_DICT:       snprintf(err,cap,"RAR 字典太大（解压内存不够）"); break;
        default:                    snprintf(err,cap,"RAR 打开失败（错误码 %u）：%s",rx.OpenResult,path); break;
        }
        return NULL;
    }
    if(flags) *flags=rx.Flags;
    return h;
}
static void hdr_to_ent(const struct RARHeaderDataEx *hd, img_rar_ent *e){
    memset(e,0,sizeof *e);
    char nm[512];
    w2u(hd->FileNameW, nm, sizeof nm);
    if(!nm[0]) snprintf(nm,sizeof nm,"%.*s",(int)sizeof hd->FileName,hd->FileName);
    norm_path(nm,e->path,sizeof e->path);
    e->is_dir   = (hd->Flags & RHDF_DIRECTORY)!=0;
    e->encrypted= (hd->Flags & RHDF_ENCRYPTED)!=0;
    e->solid    = (hd->Flags & RHDF_SOLID)!=0;
    e->size     = ((uint64_t)hd->UnpSizeHigh<<32) | hd->UnpSize;
    e->packed   = ((uint64_t)hd->PackSizeHigh<<32) | hd->PackSize;
    e->mtime    = dos_to_unix(hd->FileTime);
    if(e->is_dir) e->size=0;
}
/* 统一的"读头 → 失败就转为可读文本" */
static int rar_next_header(HANDLE h, struct RARHeaderDataEx *hd, img_rar_ent *e,
                           char *err, size_t cap, bool *pw){
    memset(hd,0,sizeof *hd);
    int r=RARReadHeaderEx(h,hd);
    if(pw) *pw=false;
    if(r==ERAR_END_ARCHIVE) return 1;
    if(r==ERAR_MISSING_PASSWORD || r==ERAR_BAD_PASSWORD){
        if(pw) *pw=true;
        snprintf(err,cap,"这个 RAR 有密码（不支持解密）");
        return IMG_ENOTSUP;
    }
    if(r!=0){ snprintf(err,cap,"RAR 头损坏（错误码 %d，包可能被截断）",r); return IMG_ECORRUPT; }
    if(e) hdr_to_ent(hd,e);
    return 0;
}
/* 跳过当前条目（solid 时 unrar 内部会顺序解压）*/
static int rar_skip(HANDLE h, char *err, size_t cap, bool *pw){
    int r=RARProcessFile(h,RAR_SKIP,NULL,NULL);
    if(pw) *pw=false;
    if(r==0) return IMG_OK;
    if(r==ERAR_MISSING_PASSWORD || r==ERAR_BAD_PASSWORD){
        if(pw) *pw=true;
        snprintf(err,cap,"这个 RAR 有密码（不支持解密）");
        return IMG_ENOTSUP;
    }
    snprintf(err,cap,"读取 RAR 失败（错误码 %d，包可能损坏）",r);
    return IMG_ECORRUPT;
}

/* ---------------- 公开：魔数 ---------------- */
bool img_rar_is_rar(const char *path){
    if(!path||!*path) return false;
    FILE *f=fopen(path,"rb");
    if(!f) return false;
    unsigned char b[8]={0}; size_t n=fread(b,1,sizeof b,f);
    fclose(f);
    if(n>=7 && !memcmp(b,"Rar!\x1a\x07\x00",7)) return true;
    if(n>=8 && !memcmp(b,"Rar!\x1a\x07\x01\x00",8)) return true;
    return false;
}

/* ---------------- 公开：扫头 ---------------- */
int img_rar_scan(const char *path, img_rar_ent_cb cb, void *ud,
                 bool *out_solid, char *err, size_t cap){
    if(err && cap) err[0]=0;
    if(out_solid) *out_solid=false;
    if(!path||!cb) return IMG_EINVAL;
    bool pw=false;
    HANDLE h=rar_open(path,RAR_OM_LIST,NULL,err,cap,&pw);
    if(!h) return pw?IMG_ENOTSUP:IMG_EFORMAT;
    int rc=IMG_OK;
    for(;;){
        /* 大包扫描同样要喂看门狗（4901 条 1.3s，20 万条会到 30s 线）*/
        img_io_touch();
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        struct RARHeaderDataEx hd; img_rar_ent e;
        int r=rar_next_header(h,&hd,&e,err,cap,&pw);
        if(r==1) break;
        if(r!=0){ rc=(r==IMG_ENOTSUP)?IMG_ENOTSUP:IMG_ECORRUPT; break; }
        if(e.solid && out_solid) *out_solid=true;
        /* 回调返回非 0 = "够了，别再扫了"（找到目标/超出上限），不是取消。
         * 以前把它当 IMG_ECANCELLED → archive.extract 4 ms 就报 CMD-CANCELLED。*/
        if(e.path[0] && strcmp(e.path,"/") && cb(ud,&e)!=0) break;
        rc=rar_skip(h,err,cap,&pw);
        if(rc!=IMG_OK){ if(pw) rc=IMG_ENOTSUP; break; }
    }
    RARCloseArchive(h);
    return rc;
}

/* ---------------- 公开：单条查询 ---------------- */
typedef struct { const char *want; bool found; img_rar_ent e; } rar_find_ctx;
static int rar_find_cb(void *ud, const img_rar_ent *e){
    rar_find_ctx *c=(rar_find_ctx*)ud;
    if(strcasecmp(e->path,c->want)) return 0;
    c->e=*e; c->found=true;
    return 1;                                   /* 命中就停 */
}
int img_rar_stat(const char *path, const char *inner, bool *isdir,
                 uint64_t *size, uint64_t *mtime, char *err, size_t cap){
    if(isdir) *isdir=false;
    if(size) *size=0;
    if(mtime) *mtime=0;
    if(!path||!inner) return IMG_EINVAL;
    char want[512]; norm_path(inner,want,sizeof want);
    rar_find_ctx c; memset(&c,0,sizeof c); c.want=want;
    int rc=img_rar_scan(path,rar_find_cb,&c,NULL,err,cap);
    if(rc!=IMG_OK) return rc;
    if(!c.found) return IMG_ENOENT;
    if(isdir) *isdir=c.e.is_dir;
    if(size)  *size=c.e.size;
    if(mtime) *mtime=c.e.mtime;
    return IMG_OK;
}

/* ---------------- 公开：流式读一条 ---------------- */
int img_rar_read(const char *path, const char *inner, img_rar_data_cb cb, void *ud,
                 uint64_t *out_bytes, img_prog *prog, char *err, size_t cap){
    if(out_bytes) *out_bytes=0;
    if(err && cap) err[0]=0;
    if(!path||!inner||!cb) return IMG_EINVAL;
    char want[512]; norm_path(inner,want,sizeof want);
    bool pw=false;
    HANDLE h=rar_open(path,RAR_OM_EXTRACT,NULL,err,cap,&pw);
    if(!h) return pw?IMG_ENOTSUP:IMG_EFORMAT;

    rar_rd rd; memset(&rd,0,sizeof rd);
    rd.cb=cb; rd.ud=ud; rd.prog=prog;
    RARSetCallback(h, rar_callback, (LPARAM)&rd);

    int rc=IMG_ENOENT;
    for(;;){
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        struct RARHeaderDataEx hd; img_rar_ent e;
        int r=rar_next_header(h,&hd,&e,err,cap,&pw);
        if(r==1) break;                                     /* 到尾了，没找到 */
        if(r!=0){ rc=(r==IMG_ENOTSUP)?IMG_ENOTSUP:IMG_ECORRUPT; break; }
        if(!strcasecmp(e.path,want)){
            if(e.is_dir){ snprintf(err,cap,"这是目录，不能当文件读"); rc=IMG_ENOTSUP; break; }
            rd.total=e.size;
            int xr=RARProcessFile(h,RAR_TEST,NULL,NULL);
            if(rd.cancelled){ rc=IMG_ECANCELLED; break; }
            if(rd.need_password){ snprintf(err,cap,"这个 RAR 有密码（不支持解密）"); rc=IMG_ENOTSUP; break; }
            if(rd.write_failed){ snprintf(err,cap,"写入目标失败"); rc=IMG_EIO; break; }
            if(xr!=0){
                if(xr==ERAR_MISSING_PASSWORD || xr==ERAR_BAD_PASSWORD){
                    snprintf(err,cap,"这个 RAR 有密码（不支持解密）"); rc=IMG_ENOTSUP;
                } else { snprintf(err,cap,"解压失败（错误码 %d，可能是包损坏）",xr); rc=IMG_ECORRUPT; }
                break;
            }
            if(out_bytes) *out_bytes=rd.bytes;
            rc=IMG_OK;
            break;
        }
        rc=rar_skip(h,err,cap,&pw);
        if(rc!=IMG_OK) break;
        rc=IMG_ENOENT;                                      /* 继续找 */
    }
    RARCloseArchive(h);
    if(rc==IMG_ENOENT && err && cap && !err[0]) snprintf(err,cap,"包内没有这个文件");
    return rc;
}

/* ---------------- 公开：抽取（文件或整棵目录）---------------- */
typedef struct { FILE *fp; } rar_file_ctx;
static int rar_file_cb(void *ud, const void *buf, size_t n){
    rar_file_ctx *c=(rar_file_ctx*)ud;
    return (c && c->fp && fwrite(buf,1,n,c->fp)==n) ? 0 : -1;
}

typedef struct { uint64_t bytes, packed; int files; const char *want; } rar_tot_ctx;
static int rar_tot_cb(void *ud, const img_rar_ent *e){
    rar_tot_ctx *c=(rar_tot_ctx*)ud;
    if(e->is_dir) return 0;
    c->bytes+=e->size; c->packed+=e->packed; c->files++;
    return 0;
}

int img_rar_extract(const char *path, const char *inner, const char *dst,
                    img_prog *prog, uint64_t *out_bytes, int *out_files,
                    char *err, size_t cap){
    if(out_bytes) *out_bytes=0;
    if(out_files) *out_files=0;
    if(err && cap) err[0]=0;
    if(!path||!inner||!dst) return IMG_EINVAL;
    char want[512]; norm_path(inner,want,sizeof want);

    bool isdir=false; uint64_t sz=0,mt=0;
    int rc=img_rar_stat(path,want,&isdir,&sz,&mt,err,cap);
    if(rc!=IMG_OK) return rc;

    /* ---- 单文件 ---- */
    if(!isdir){
        rar_file_ctx fc; fc.fp=fopen(dst,"wb");
        if(!fc.fp){ snprintf(err,cap,"建不了目标文件：%s",dst); return IMG_EIO; }
        uint64_t got=0;
        rc=img_rar_read(path,want,rar_file_cb,&fc,&got,prog,err,cap);
        fclose(fc.fp);
        if(rc!=IMG_OK){ remove(dst); return rc; }
        if(out_bytes) *out_bytes=got;
        if(out_files) *out_files=1;
        return IMG_OK;
    }

    /* ---- 整棵目录：先算总量（进度用），再一遍写出 ---- */
    rar_tot_ctx tc; memset(&tc,0,sizeof tc); tc.want=want;
    bool dummy_solid=false;
    rc=img_rar_scan(path,rar_tot_cb,&tc,&dummy_solid,err,cap);
    if(rc!=IMG_OK) return rc;
    if(mkdir_p(dst)!=IMG_OK){ snprintf(err,cap,"建不了目标目录：%s",dst); return IMG_EIO; }
    /* REQ-51 B：解压后口径 + 压缩包口径都给（RAR 有 per-entry packed，能算准）*/
    img_prog_bytes_kind(prog,"extracted",0,tc.packed);

    bool pw=false;
    HANDLE h=rar_open(path,RAR_OM_EXTRACT,NULL,err,cap,&pw);
    if(!h) return pw?IMG_ENOTSUP:IMG_EFORMAT;

    uint64_t bytes=0, arc_done=0; int files=0;
    for(;;){
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        struct RARHeaderDataEx hd; img_rar_ent e;
        int r=rar_next_header(h,&hd,&e,err,cap,&pw);
        if(r==1){ rc=IMG_OK; break; }
        if(r!=0){ rc=(r==IMG_ENOTSUP)?IMG_ENOTSUP:IMG_ECORRUPT; break; }

        if(!name_under(e.path,want) || !strcmp(e.path,want)){
            rc=rar_skip(h,err,cap,&pw); if(rc!=IMG_OK) break;
            continue;
        }
        char rel[2048]; rel_path(e.path,want,rel,sizeof rel);
        if(!rel[0]){ rc=rar_skip(h,err,cap,&pw); if(rc!=IMG_OK) break; continue; }
        if(has_dotdot(rel)){ snprintf(err,cap,"包内路径含 ..（拒绝解压）"); rc=IMG_EFORMAT; break; }
        char full[4096]; snprintf(full,sizeof full,"%s/%s",dst,rel);

        if(e.is_dir){
            if(mkdir_p(full)!=IMG_OK){ snprintf(err,cap,"建不了目录：%s",full); rc=IMG_EIO; break; }
            rc=rar_skip(h,err,cap,&pw); if(rc!=IMG_OK) break;
            continue;
        }
        {   char par[4096]; snprintf(par,sizeof par,"%s",full);
            char *sl=strrchr(par,'/'); if(sl){ *sl=0; if(par[0]) mkdir_p(par); } }
        rar_file_ctx fc; fc.fp=fopen(full,"wb");
        if(!fc.fp){ snprintf(err,cap,"建不了文件：%s",full); rc=IMG_EIO; break; }
        rar_rd rd; memset(&rd,0,sizeof rd);
        rd.cb=rar_file_cb; rd.ud=&fc;
        RARSetCallback(h, rar_callback, (LPARAM)&rd);
        int xr=RARProcessFile(h,RAR_TEST,NULL,NULL);
        fclose(fc.fp);
        if(rd.cancelled){ rc=IMG_ECANCELLED; break; }
        if(rd.need_password){ snprintf(err,cap,"这个 RAR 有密码（不支持解密）"); rc=IMG_ENOTSUP; break; }
        if(rd.write_failed){ snprintf(err,cap,"写文件失败：%s",full); rc=IMG_EIO; break; }
        if(xr!=0){
            if(xr==ERAR_MISSING_PASSWORD || xr==ERAR_BAD_PASSWORD){
                snprintf(err,cap,"这个 RAR 有密码（不支持解密）"); rc=IMG_ENOTSUP;
            } else { snprintf(err,cap,"解压失败（错误码 %d）：%s",xr,e.path); rc=IMG_ECORRUPT; }
            break;
        }
        bytes+=rd.bytes; files++;
        if(prog){
            img_prog_bytes(prog,bytes,tc.bytes?tc.bytes:0);
            img_prog_bytes_kind(prog,"extracted",arc_done,tc.packed);
            img_prog_item(prog,e.path);
        }
        arc_done += e.packed;
        img_hb("rar","extract","unpack",bytes>>20);
    }
    RARCloseArchive(h);
    if(out_bytes) *out_bytes=bytes;
    if(out_files) *out_files=files;
    return rc;
}

#endif /* IMG_WITH_RAR */