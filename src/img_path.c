#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_path.c — 统一容器路径解析（见 include/img_path.h）
 *
 * 设计取舍：
 *   · 用 ! 而不是 / 做容器边界：宿主路径和容器内路径都用 /，没有边界就必然靠猜；
 *   · 类型按魔数判断：扩展名会骗人（.qcow2 目录、.iso 改名）；
 *   · ISO 里的 WIM 先抽到临时文件：wimlib 只吃真实文件路径（这是上游限制）；
 *   · 所有"猜"的地方一律报错并给出可选写法，不静默降级。
 */
#include "img_path.h"
#include "img_qcow2.h"
#include "img_io.h"
#include "img_partition.h"
#include "img_iso.h"
#include "img_wim_fs.h"
#include "img_lock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef IMG_WITH_WIM
#define IMG_WITH_WIM 0
#endif

#define P_CAP 2048

static void setwhy(char *why, size_t cap, const char *msg){
    if(why && cap) snprintf(why, cap, "%s", msg ? msg : "");
}
static void setwhy2(char *why, size_t cap, const char *a, const char *b){
    if(why && cap) snprintf(why, cap, "%s%s", a ? a : "", b ? b : "");
}

/* ---------- 转义：宿主/容器段里的 \! 代表字面 ! ---------- */
static void unescape_seg(const char *s, size_t n, char *out, size_t cap){
    size_t o=0;
    for(size_t i=0;i<n && o+1<cap;i++){
        if(s[i]=='\\' && i+1<n && s[i+1]=='!'){ out[o++]='!'; i++; }
        else out[o++]=s[i];
    }
    out[o]=0;
}
static const char *find_bang(const char *s){
    for(const char *p=s; *p; p++){
        if(*p=='\\' && p[1]){ p++; continue; }   /* 跳过转义对 */
        if(*p=='!') return p;
    }
    return NULL;
}
/* 在 s 里找第一个未转义的 '!'，返回位置；同时把前半段反转义到 buf */
static const char *split_bang(const char *s, char *buf, size_t cap){
    const char *b=find_bang(s);
    size_t n=b ? (size_t)(b-s) : strlen(s);
    unescape_seg(s,n,buf,cap);
    return b;
}

/* ---------- 容器内路径归一：\ → /、压 //、拒绝 .. ---------- */
static int normalize_inner(const char *in, char *out, size_t cap, char *why, size_t whycap){
    if(!in || !*in){ snprintf(out,cap,"/"); return IMG_OK; }
    char tmp[P_CAP];
    size_t o=0;
    if(in[0]!='/' && in[0]!='\\'){ if(o+1<sizeof tmp) tmp[o++]='/'; }
    for(const char *p=in; *p && o+2<sizeof tmp; p++){
        if(*p=='\\' && p[1]=='!'){ tmp[o++]='!'; p++; continue; }  /* \! = 字面 ! */
        if(*p=='\\'){ tmp[o++]='/'; continue; }
        if(*p=='/' && o && tmp[o-1]=='/') continue;
        tmp[o++]=*p;
    }
    tmp[o]=0;
    /* 拒绝 ..（按 / 分段检查） */
    const char *p=tmp;
    while(*p){
        while(*p=='/') p++;
        const char *s=p;
        while(*p && *p!='/') p++;
        size_t l=(size_t)(p-s);
        if(l==2 && s[0]=='.' && s[1]=='.'){
            setwhy(why,whycap,"容器内路径不能包含 ..（不支持穿越）");
            return IMG_EINVAL;
        }
    }
    if(o>=cap){ setwhy(why,whycap,"容器内路径过长"); return IMG_ENAMETOOLONG; }
    snprintf(out,cap,"%s",tmp);
    return IMG_OK;
}

/* ---------- 魔数识别 ---------- */
enum { CT_QCOW2=1, CT_ISO=2, CT_WIM=3, CT_ARCHIVE=4 };
static int detect_container(const char *path, int *type, char *why, size_t cap){
    int fd=open(path,O_RDONLY);
    if(fd<0){ setwhy2(why,cap,"打不开容器文件：",path); return IMG_ENOENT; }
    unsigned char b[8];
    ssize_t n=pread(fd,b,8,0);
    if(n==8 && b[0]=='Q' && b[1]=='F' && b[2]=='I' && b[3]==0xfb){
        close(fd); *type=CT_QCOW2; return IMG_OK;
    }
    if(n==8 && !memcmp(b,"MSWIM\0\0\0",8)){ close(fd); *type=CT_WIM; return IMG_OK; }
    /* 压缩包（REQ-31）：zip/7z/rar/cab/gzip/xz/bzip2 认魔数；tar 看 257 处的 "ustar" */
    if(n>=4 && b[0]=='P' && b[1]=='K' && (b[2]==3||b[2]==5||b[2]==7)){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    if(n>=6 && !memcmp(b,"7z\xBC\xAF\x27\x1C",6)){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    if(n>=7 && !memcmp(b,"Rar!\x1a\x07",6)){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    if(n>=4 && !memcmp(b,"MSCF",4)){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    if(n>=2 && b[0]==0x1f && b[1]==0x8b){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    if(n>=6 && !memcmp(b,"\xFD""7zXZ",5)){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    if(n>=3 && !memcmp(b,"BZh",3)){ close(fd); *type=CT_ARCHIVE; return IMG_OK; }
    /* ★ P1（2026-11-08）：tar 的魔数在偏移 257（"ustar"）—— 注释早就写了，
     *   但代码一直没实现，导致 .tar 包被当成"不认识的容器"。 */
    {
        unsigned char tb[6]={0};
        if(pread(fd,tb,6,257)==6 && (!memcmp(tb,"ustar",5) || !memcmp(tb,"ustar\0",6))){
            close(fd); *type=CT_ARCHIVE; return IMG_OK;
        }
    }
    n=pread(fd,b,8,0x8001);
    if(n==8 && (!memcmp(b,"CD001",5) || !memcmp(b,"BEA01",5) ||
                !memcmp(b,"NSR02",5) || !memcmp(b,"NSR03",5))){
        close(fd); *type=CT_ISO; return IMG_OK;
    }
    close(fd);
    /* ★ P3（2026-11-09）：raw 磁盘（无魔数、无文件系统签名）。
     *   判据：扩展名 .img/.raw/.dd/.bin 或 文件大小是 512 的整数倍且 >= 1MiB。
     *   后者会误判一些随机文件，所以优先看扩展名；扩展名不认识时再看大小。 */
    {
        const char *ext = strrchr(path, '.');
        bool raw_ext = ext && (!strcasecmp(ext,".img") || !strcasecmp(ext,".raw") ||
                               !strcasecmp(ext,".dd")  || !strcasecmp(ext,".bin"));
        uint64_t fsz = 0;
        struct stat st;
        if(stat(path, &st) == 0) fsz = (uint64_t)st.st_size;
        if(raw_ext || (fsz >= (1ULL<<20) && fsz % 512 == 0)){
            *type = CT_QCOW2;    /* 复用 CT_QCOW2：后续按分区处理，img_qcow2_open 内部探测格式 */
            return IMG_OK;
        }
    }
    setwhy2(why,cap,"不认识的容器（不是 qcow2 / raw / ISO / WIM/ESD）：",path);
    return IMG_EFORMAT;
}

/* ---------- qcow2 分区 ---------- */
typedef struct { uint64_t lba, bytes; } pend;
static int list_parts(img_qcow2 *q, pend *out, int max){
    img_parttable_t t;
    int rc=img_parttable_read(q,&t,0);
    if(rc!=IMG_OK) return rc;
    int n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS && n<max;i++){
        if(!t.parts[i].used) continue;
        out[n].lba=t.parts[i].first_lba;
        out[n].bytes=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512ULL;
        n++;
    }
    return n;
}
static int fs_at(img_qcow2 *q, uint64_t lba, img_xkind *kind, char *why, size_t cap){
    unsigned char b[512];
    if(img_qcow2_read_full(q,lba*512ULL,b,sizeof b)!=IMG_OK){
        setwhy(why,cap,"读分区引导扇区失败"); return IMG_EIO;
    }
    if(!memcmp(b+3,"NTFS    ",8)){ *kind=IMG_XK_NTFS; return IMG_OK; }
    if(!memcmp(b+3,"EXFAT   ",8)){
        setwhy(why,cap,"该分区是 exFAT：当前只支持 NTFS / FAT32"); return IMG_EFORMAT;
    }
    if(!memcmp(b+82,"FAT32   ",8)){ *kind=IMG_XK_FAT; return IMG_OK; }
    setwhy(why,cap,"该分区不是 NTFS / FAT32（未格式化或类型不符）");
    return IMG_EFORMAT;
}

/* ---------- 打开 qcow2（只读探测） ---------- */
typedef struct { img_io *io; img_qcow2 *q; } qopen;
static int qc_open_ro(const char *path, qopen *o, char *why, size_t cap){
    o->io=NULL; o->q=NULL;
    o->io=img_io_open_existing(path,false);
    if(!o->io){ setwhy2(why,cap,"打不开镜像：",path); return IMG_ENOENT; }
    img_qcow2_opts_t opt={0};
    opt.read_only=true;
    o->q=img_qcow2_open(o->io,&opt);
    if(!o->q){ img_io_close(o->io); o->io=NULL; setwhy(why,cap,"不是合法的 qcow2"); return IMG_EFORMAT; }
    return IMG_OK;
}
static void qc_close(qopen *o){
    if(o->q){ img_qcow2_close(o->q); o->q=NULL; }
    if(o->io){ img_io_close(o->io); o->io=NULL; }
}

/* selector: pN | lbaN | 空（qcow2 不允许空） */
static int resolve_part(img_path *p, const char *sel, char *why, size_t cap){
    qopen o;
    int rc=qc_open_ro(p->host,&o,why,cap);
    if(rc!=IMG_OK) return rc;
    pend parts[IMG_MAX_PARTITIONS];
    int n=list_parts(o.q,parts,IMG_MAX_PARTITIONS);
    if(n<0){ qc_close(&o); setwhy(why,cap,"读分区表失败"); return n; }
    uint64_t lba=0;
    if(sel[0]=='p' && sel[1]){
        char *end=NULL; long idx=strtol(sel+1,&end,10);
        if(!end || *end || idx<1 || idx>n){
            qc_close(&o);
            snprintf(why,cap,"分区 %s 不存在（本镜像共 %d 个已用分区）",sel,n);
            return IMG_ENOENT;
        }
        lba=parts[idx-1].lba;
    } else if(!strncmp(sel,"lba",3) && sel[3]){
        char *end=NULL; unsigned long long v=strtoull(sel+3,&end,0);
        if(!end || *end){ qc_close(&o); setwhy(why,cap,"LBA 写法应为 lba165888"); return IMG_EINVAL; }
        int found=0;
        for(int i=0;i<n;i++) if(parts[i].lba==(uint64_t)v){ found=1; break; }
        if(!found){ qc_close(&o); setwhy(why,cap,"该 LBA 不在分区表里"); return IMG_ENOENT; }
        lba=(uint64_t)v;
    } else {
        qc_close(&o);
        setwhy(why,cap,"qcow2 路径必须写分区：!p3: 或 !lba165888:");
        return IMG_EINVAL;
    }
    img_xkind k=IMG_XK_NONE;
    rc=fs_at(o.q,lba,&k,why,cap);
    qc_close(&o);
    if(rc!=IMG_OK) return rc;
    p->kind=k; p->lba=lba;
    return IMG_OK;
}

/* ---------- ISO 里的 WIM → 临时文件 ---------- */
static int materialize_iso_wim(const char *iso_path, const char *inner,
                               char *out, size_t cap, char *why, size_t whycap){
    img_iso *iso=NULL;
    char err[256]={0};
    int rc=img_iso_open_path(&iso,iso_path,err,sizeof err);
    if(rc!=IMG_OK){ setwhy(why,whycap,err[0]?err:"打开 ISO 失败"); return rc; }
    uint64_t sz=0; bool isdir=false;
    rc=img_iso_stat(iso,inner,&sz,&isdir);
    if(rc!=IMG_OK || isdir){
        img_iso_close(iso);
        setwhy2(why,whycap,"ISO 里没有这个文件：",inner);
        return (rc==IMG_OK)?IMG_ESTATE:rc;
    }
    const char *tmp=img_tmpdir();
    if(!tmp || !*tmp){ img_iso_close(iso); setwhy(why,whycap,"没有可写的临时目录"); return IMG_ESTATE; }
    /* ★ P1-6：统一临时命名（imgtool-tmp-iso-wim-<pid>-<n>） */
    char tmpl[P_CAP];
    if(img_tmpfile("iso-wim",tmpl,sizeof tmpl)==NULL){
        img_iso_close(iso); setwhy(why,whycap,"建临时文件失败（临时目录不可写）"); return IMG_EIO;
    }
    uint64_t got=0;
    rc=img_iso_extract(iso,inner,tmpl,0,NULL,&got);
    img_iso_close(iso);
    if(rc!=IMG_OK){ unlink(tmpl); setwhy(why,whycap,"从 ISO 抽 WIM 失败"); return rc; }
    size_t tl=strlen(tmpl);
    if(tl>=cap){ unlink(tmpl); setwhy(why,whycap,"临时文件路径过长"); return IMG_ENAMETOOLONG; }
    memcpy(out,tmpl,tl+1);
    return IMG_OK;
}

/* ---------- 顶层解析 ---------- */
int img_path_parse(const char *spec, img_path *out, char *why, size_t whycap){
    return img_path_parse_ex(spec,out,false,why,whycap);
}
int img_path_parse_ex(const char *spec, img_path *out, bool is_dst, char *why, size_t whycap){
    if(why && whycap) why[0]=0;
    if(!spec || !*spec || !out){ setwhy(why,whycap,"路径为空"); return IMG_EINVAL; }
    memset(out,0,sizeof *out);
    out->index=1;

    if(!strncasecmp(spec,"host:",5)){
        const char *h=spec+5;
        if(!*h){ setwhy(why,whycap,"host: 后面没有路径"); return IMG_EINVAL; }
        if(h[0]!='/'){ setwhy(why,whycap,"host: 后面必须是绝对路径（以 / 开头）"); return IMG_EINVAL; }
        if(!strchr(h,'!')){            /* 纯宿主路径 */
            unescape_seg(h,strlen(h),out->host,sizeof out->host);
            out->kind=IMG_XK_HOST;
            return IMG_OK;
        }
        spec=h;                        /* 带容器：剥掉 host: 继续往下解析（REQ-31）*/
    }

    char head[P_CAP];
    const char *bang=split_bang(spec,head,sizeof head);
    if(!*head){ setwhy(why,whycap,"! 前面缺少容器路径"); return IMG_EINVAL; }
    snprintf(out->host,sizeof out->host,"%s",head);

    if(!bang){
        if(head[0]!='/'){ setwhy(why,whycap,"宿主路径必须是绝对路径（以 / 开头）"); return IMG_EINVAL; }
        out->kind=IMG_XK_HOST; return IMG_OK;
    }

    int ct=0;
    int rc=detect_container(out->host,&ct,why,whycap);
    if(rc==IMG_ENOENT && is_dst){
        /* ★ P1（2026-11-08）：目标侧允许"新建容器"。按扩展名判：
         *   zip/7z/tar/tgz → CT_ARCHIVE；iso → CT_ISO（稍后写层报只读）；
         *   其余扩展名照旧报 ENOENT（不打哑谜）。 */
        const char *ext=strrchr(out->host,'.');
        if(ext && (!strncasecmp(ext,".zip",4)||!strncasecmp(ext,".7z",3)||
                   !strncasecmp(ext,".tar",4)||!strncasecmp(ext,".tgz",4)))
            ct=CT_ARCHIVE;
        else if(ext && !strncasecmp(ext,".iso",4))
            ct=CT_ISO;
        else
            return rc;
        rc=IMG_OK;
        out->is_new_container=true;
    }
    if(rc!=IMG_OK) return rc;

    const char *rest=bang+1;
    const char *colon=strchr(rest,':');
    if(!colon){
        setwhy(why,whycap,"! 后面要写 选择器:路径（例如 !p3:/Users 或 !:/bootmgr）");
        return IMG_EINVAL;
    }
    char sel[64];
    size_t sl=(size_t)(colon-rest);
    if(sl>=sizeof sel){ setwhy(why,whycap,"选择器过长"); return IMG_EINVAL; }
    memcpy(sel,rest,sl); sel[sl]=0;
    const char *inner=colon+1;

    if(ct==CT_QCOW2){
        rc=resolve_part(out,sel,why,whycap);
        if(rc!=IMG_OK) return rc;
        rc=normalize_inner(inner,out->inner,sizeof out->inner,why,whycap);
        if(rc!=IMG_OK) return rc;
        out->is_readonly=false;
        return IMG_OK;
    }
    if(ct==CT_ARCHIVE){
        if(sel[0]){ setwhy(why,whycap,"压缩包是单卷容器：选择器留空（!:/path）"); return IMG_EINVAL; }
        rc=normalize_inner(inner,out->inner,sizeof out->inner,why,whycap);
        if(rc!=IMG_OK) return rc;
        out->kind=IMG_XK_ARCHIVE;
        /* 压缩包不整体只读：zip/7z/tar 可写（整包重写，见 img_archive_w.c），
         * rar/cab/iso 由写层判 IMG_EREADONLY。REQ-41 */
        out->is_readonly=false;
        return IMG_OK;
    }
    if(ct==CT_ISO){
        if(sel[0]){ setwhy(why,whycap,"ISO 是单卷容器：选择器留空（!:/path）"); return IMG_EINVAL; }
        /* ISO 里还能再进 WIM：/sources/install.esd!1:/Windows/... */
        char innerbuf[P_CAP];
        const char *b2=split_bang(inner,innerbuf,sizeof innerbuf);
        if(!b2){
            rc=normalize_inner(inner,out->inner,sizeof out->inner,why,whycap);
            if(rc!=IMG_OK) return rc;
            out->kind=IMG_XK_ISO;
            out->is_readonly=true;
            return IMG_OK;
        }
        /* innerbuf 是 ISO 内的 WIM 路径；b2 后面是 <index>:<wim内路径> */
        char wimpath[P_CAP];
        rc=normalize_inner(innerbuf,wimpath,sizeof wimpath,why,whycap);
        if(rc!=IMG_OK) return rc;
        char tmpwim[P_CAP];
        rc=materialize_iso_wim(out->host,wimpath,tmpwim,sizeof tmpwim,why,whycap);
        if(rc!=IMG_OK) return rc;
        snprintf(out->temp_path,sizeof out->temp_path,"%s",tmpwim);
        snprintf(out->host,sizeof out->host,"%s",tmpwim);
        const char *c2=strchr(b2+1,':');
        if(!c2){ unlink(tmpwim); setwhy(why,whycap,"WIM 段要写 索引:路径（例如 !1:/Windows）"); return IMG_EINVAL; }
        char is[32]; size_t il=(size_t)(c2-(b2+1));
        if(il>=sizeof is){ unlink(tmpwim); setwhy(why,whycap,"WIM 索引过长"); return IMG_EINVAL; }
        memcpy(is,b2+1,il); is[il]=0;
        out->index = is[0] ? atoi(is) : 1;
        if(out->index<1){ unlink(tmpwim); setwhy(why,whycap,"WIM 索引必须 >= 1"); return IMG_EINVAL; }
        rc=normalize_inner(c2+1,out->inner,sizeof out->inner,why,whycap);
        if(rc!=IMG_OK){ unlink(tmpwim); return rc; }
        out->kind=IMG_XK_WIM;
        out->is_readonly=true;
        return IMG_OK;
    }
    /* CT_WIM */
    out->index = sel[0] ? atoi(sel) : 1;
    if(out->index<1){ setwhy(why,whycap,"WIM 索引必须 >= 1"); return IMG_EINVAL; }
    rc=normalize_inner(inner,out->inner,sizeof out->inner,why,whycap);
    if(rc!=IMG_OK) return rc;
    out->kind=IMG_XK_WIM;
    out->is_readonly=true;
    return IMG_OK;
}

int img_path_format(const img_path *p, char *out, size_t cap){
    if(!p||!out||!cap) return IMG_EINVAL;
    const char *sel="";
    char selbuf[64];
    if(p->kind==IMG_XK_NTFS || p->kind==IMG_XK_FAT){
        /* 用 lba 而不是 pN：格式化后仍可复制粘贴，不依赖分区顺序 */
        snprintf(selbuf,sizeof selbuf,"lba%llu",(unsigned long long)p->lba);
        sel=selbuf;
        snprintf(out,cap,"%s!%s:%s",p->host,sel,p->inner);
    } else if(p->kind==IMG_XK_ISO){
        snprintf(out,cap,"%s!:%s",p->host,p->inner);
    } else if(p->kind==IMG_XK_WIM){
        snprintf(out,cap,"%s!%d:%s",p->host,p->index,p->inner);
    } else {
        snprintf(out,cap,"%s",p->host);
    }
    return IMG_OK;
}

void img_path_cleanup(img_path *p){
    if(!p) return;
    if(p->temp_path[0]){ unlink(p->temp_path); p->temp_path[0]=0; }
}
