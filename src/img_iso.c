/* img_iso.c — ISO(UDF) 只读访问：自己解析 ECMA-167，不依赖外部库
 *
 * 为什么不用现成的：
 *   · libarchive 不支持 UDF（libarchive#1036，Windows ISO 的 ISO9660 目录树是空的）；
 *   · libudfread（VideoLAN）只认"分区号匹配"的普通 UDF；微软安装盘的 Partition
 *     Descriptor 里分区号是 0x0BAD（实测 2989），它当场放弃（"failed reading
 *     Volume Descriptor Sequence"）；
 *   · libcdio 只有 ISO9660/Joliet，同样够不到 install.wim；
 *   · 7-Zip 的 UDF 是 C++，为了抽一个文件拖进 C++ 运行时与 LGPL 组合，不值。
 *   → 自己写只读实现：只做"按路径找文件 + 流式读出来"两件事，~400 行，
 *     不引入任何新依赖（Android 侧也少一个 .so）。
 *
 * 只读，绝不修改 ISO 一个字节。块大小按 LVD 的 logical_block_size（通常 2048）。
 */
#define _DEFAULT_SOURCE
#include "img_iso.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>

/* ---- ECMA-167 小工具 ---- */
static uint16_t g16(const uint8_t *p){ return (uint16_t)(p[0] | (p[1]<<8)); }
static uint32_t g32(const uint8_t *p){ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t g64(const uint8_t *p){ return (uint64_t)g32(p) | ((uint64_t)g32(p+4)<<32); }

#define TAG_AVDP   2
#define TAG_PD     5
#define TAG_LVD    6
#define TAG_TERM   8
#define TAG_FSD    256
#define TAG_FID    257
#define TAG_FE     261
#define TAG_EFE    266

typedef struct { uint64_t bytes; uint32_t lb; } lext_t;   /* extent：字节长度 + 分区内起始 lb */

struct img_iso {
    img_io  *io;
    bool     own_io;
    uint32_t lb_size;      /* LVD logical block size（通常 2048）*/
    uint32_t part_start;   /* 分区起点（lb_size 单位）*/
    uint64_t fsd_lb;       /* File Set Descriptor 在分区内的 lb */
    uint64_t root_lb;      /* 根目录 FE 的 lb */
    int      mode;         /* 0=UDF（首选） 1=ISO9660/Joliet（回退）*/
    uint32_t iso_root_lba; /* ISO9660 根目录所在扇区（2048）*/
    uint32_t iso_root_len;
    bool     iso_joliet;
    char     vol[128];
    char     err[200];
};

/* ---------- 基本读 ---------- */
static int rd_lb(img_iso *s, uint64_t lb, void *buf, uint32_t nlb){
    if(!s || !s->io) return IMG_EINVAL;
    uint64_t off = ((uint64_t)s->part_start + lb) * s->lb_size;
    if(img_io_read_full(s->io, off, buf, (size_t)nlb * s->lb_size) != IMG_OK) return IMG_EIO;
    return IMG_OK;
}
static int rd_abs(img_iso *s, uint64_t lb, void *buf){
    return img_io_read_full(s->io, lb * s->lb_size, buf, s->lb_size);
}

/* ---------- 打开：AVDP → VDS → LVD → FSD ---------- */
/* ---------- ISO9660 / Joliet（回退；纯 ISO9660 的盘也能读） ---------- */
typedef struct { char name[300]; bool dir; uint32_t lba; uint32_t size; uint8_t flags; } iso_ent;

/* 名字规范化：ISO9660 形如 "NAME.EXT;1"；Joliet 是 UCS-2BE */
static void iso_name_norm(const uint8_t *p, int n, bool joliet, char *out, size_t cap){
    size_t k=0;
    if(joliet){
        for(int i=0;i+1<n && k+1<cap;i+=2){
            uint16_t ch=(uint16_t)((p[i]<<8)|p[i+1]);
            out[k++] = (ch && ch<128) ? (char)ch : (ch?'?':0);
            if(!ch) break;
        }
    } else {
        for(int i=0;i<n && k+1<cap;i++) out[k++]=(char)p[i];
    }
    out[k]=0;
    char *semi=strchr(out,';'); if(semi) *semi=0;       /* 去版本号 */
    size_t L=strlen(out);
    if(L && out[L-1]=='.') out[L-1]=0;                  /* 去尾点（目录）*/
}

/* 遍历一个 ISO9660 目录；每条记录回调 */
typedef void (*iso_ent_cb)(void *ud, const iso_ent *e);
static int iso_iter(img_iso *s, uint32_t lba, uint32_t len, iso_ent_cb cb, void *ud){
    if(!len) return IMG_OK;
    uint8_t *buf=malloc(len);
    if(!buf) return IMG_ENOMEM;
    if(img_io_read_full(s->io,(uint64_t)lba*2048,buf,len)!=IMG_OK){ free(buf); return IMG_EIO; }
    uint32_t off=0;
    while(off < len){
        uint8_t rl=buf[off];
        if(rl==0){ off = ((off/2048)+1)*2048; continue; }   /* 记录不跨扇区 */
        if(off+rl>len || rl<34) break;
        const uint8_t *r=buf+off;
        iso_ent e; memset(&e,0,sizeof e);
        e.lba = (uint32_t)r[2] | ((uint32_t)r[3]<<8) | ((uint32_t)r[4]<<16) | ((uint32_t)r[5]<<24);
        e.size= (uint32_t)r[10]| ((uint32_t)r[11]<<8)| ((uint32_t)r[12]<<16)| ((uint32_t)r[13]<<24);
        e.flags=r[25];
        e.dir=(e.flags & 0x02)!=0;
        int nl=r[32];
        if(33+nl<=rl) iso_name_norm(r+33,nl,s->iso_joliet,e.name,sizeof e.name);
        /* 跳过 "." / ".."：ISO9660 里它们的名字是 0x00 / 0x01 单字节，不是字面点 */
        bool dot = (nl==1 && (r[33]==0 || r[33]==1));
        if(!dot && e.name[0] && strcmp(e.name,".") && strcmp(e.name,"..") && cb) cb(ud,&e);
        off += rl;
    }
    free(buf);
    return IMG_OK;
}

/* 在目录里按名字找（大小写不敏感）；同一名字多段（>4GB 用多 extent）合起来 */
typedef struct { const char *want; iso_ent hit; int n; uint32_t extents[32]; uint32_t ext_lens[32]; uint64_t total; } iso_find;
static void iso_find_cb(void *ud, const iso_ent *e){
    iso_find *f=(iso_find*)ud;
    if(strcasecmp(e->name,f->want)) return;
    f->hit=*e; f->n++;
    if(f->n<=32){
        f->extents[f->n-1]=e->lba;
        f->ext_lens[f->n-1]=e->size;
        f->total += e->size;
    }
}
static int iso_find_in(img_iso *s, uint32_t lb, uint32_t len, const char *name, iso_find *out){
    memset(out,0,sizeof *out); out->want=name;
    int rc=iso_iter(s,lb,len,iso_find_cb,out);
    if(rc!=IMG_OK) return rc;
    return out->n? IMG_OK : IMG_ENOENT;
}
static int iso_resolve(img_iso *s, const char *path, uint32_t *lb, uint32_t *len,
                       bool *is_dir, uint64_t *size, iso_find *files){
    uint32_t cur_lb=s->iso_root_lba, cur_len=s->iso_root_len;
    bool d=true; uint64_t sz=0;
    if(path && *path && strcmp(path,"/")){
        char buf[512]; snprintf(buf,sizeof buf,"%s",path);
        char *save=NULL;
        for(char *tok=strtok_r(buf,"/\\",&save); tok; tok=strtok_r(NULL,"/\\",&save)){
            if(!*tok) continue;
            iso_find f;
            if(iso_find_in(s,cur_lb,cur_len,tok,&f)!=IMG_OK) return IMG_ENOENT;
            cur_lb=f.hit.lba; cur_len=f.hit.size; d=f.hit.dir; sz=f.total;
            if(files) *files=f;
        }
    }
    if(lb) *lb=cur_lb;
    if(len) *len=cur_len;
    if(is_dir) *is_dir=d;
    if(size) *size=sz;
    return IMG_OK;
}

/* 尝试按 ISO9660 打开（PVD 在 16；有 Joliet SVD 就用它的名字）*/
static int open_iso9660(img_iso *s, uint8_t *blk, char *err, size_t errcap){
    if(rd_abs(s,16,blk)!=IMG_OK || memcmp(blk+1,"CD001",5)!=0) return IMG_EFORMAT;
    const uint8_t *rr=blk+156;                       /* PVD 的根目录记录 */
    s->iso_root_lba = (uint32_t)rr[2] | ((uint32_t)rr[3]<<8) | ((uint32_t)rr[4]<<16) | ((uint32_t)rr[5]<<24);
    s->iso_root_len = (uint32_t)rr[10]| ((uint32_t)rr[11]<<8)| ((uint32_t)rr[12]<<16)| ((uint32_t)rr[13]<<24);
    s->iso_joliet=false;
    {   int vl=blk[40]; if(vl>0 && vl<33){
            char nm[64]; iso_name_norm(blk+40+1,vl,false,nm,sizeof nm);
            snprintf(s->vol,sizeof s->vol,"%s",nm);
        }
    }
    /* SVD（type=2）且转义序列以 % 开头 → Joliet，名字是 UCS-2 */
    for(int sec=17; sec<32; sec++){
        if(rd_abs(s,(uint64_t)sec,blk)!=IMG_OK) break;
        if(memcmp(blk+1,"CD001",5)!=0) break;
        if(blk[0]==255) break;                       /* 结束 */
        if(blk[0]==2 && blk[88]=='%'){               /* %/@ %/C %/E */
            const uint8_t *jr=blk+156;
            s->iso_root_lba=(uint32_t)jr[2]|((uint32_t)jr[3]<<8)|((uint32_t)jr[4]<<16)|((uint32_t)jr[5]<<24);
            s->iso_root_len=(uint32_t)jr[10]|((uint32_t)jr[11]<<8)|((uint32_t)jr[12]<<16)|((uint32_t)jr[13]<<24);
            s->iso_joliet=true;
            /* 卷标 */
            int vl=blk[80]; if(vl>0 && vl<33){
                char nm[64]; iso_name_norm(blk+81,vl,true,nm,sizeof nm);
                snprintf(s->vol,sizeof s->vol,"%s",nm);
            }
            break;
        }
    }
    if(!s->iso_root_lba){ if(err) snprintf(err,errcap,"ISO9660 根目录记录不可用"); return IMG_EFORMAT; }
    s->mode=1;
    (void)err; (void)errcap;
    return IMG_OK;
}

int img_iso_open_io(img_iso **out, img_io *io, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(!out || !io) return IMG_EINVAL;
    *out=NULL;
    img_iso *s=calloc(1,sizeof *s);
    if(!s) return IMG_ENOMEM;
    s->io=io; s->own_io=false; s->lb_size=2048;

    uint8_t *blk=malloc(2048);
    if(!blk){ free(s); return IMG_ENOMEM; }

    int forced_iso = (getenv("IMG_ISO_FORCE_ISO9660")!=NULL);

    /* ---------- ① 先试 UDF（Windows 安装盘的正路） ---------- */
    if(!forced_iso){
        uint64_t total = img_io_size(io)/2048;
        uint64_t cand[3] = {256, total? total-1 : 0, total>256 ? total-257 : 0};
        uint32_t vds_lb=0, vds_len=0; int found=0;
        for(int i=0;i<3 && !found;i++){
            if(cand[i]==0) continue;
            if(rd_abs(s,cand[i],blk)!=IMG_OK) continue;
            if(g16(blk)!=TAG_AVDP) continue;
            vds_len=g32(blk+16); vds_lb=g32(blk+20);
            found=1;
        }
        if(found){
            uint32_t nlb = vds_len/2048; if(nlb>64) nlb=64;
            uint32_t pstart=0; int have_pd=0, have_lvd=0;
            for(uint32_t i=0;i<nlb;i++){
                if(rd_abs(s,vds_lb+i,blk)!=IMG_OK) break;
                uint16_t tag=g16(blk);
                if(tag==TAG_TERM) break;
                if(tag==TAG_PD && !have_pd){
                    /* 不按"分区号"匹配：微软安装盘的分区号是 0x0BAD（实测 2989）*/
                    pstart=g32(blk+188); have_pd=1;
                } else if(tag==TAG_LVD && !have_lvd){
                    uint32_t lbs=g32(blk+212);
                    if(lbs>=512 && lbs<=65536) s->lb_size=lbs;
                    s->fsd_lb=g32(blk+252);
                    have_lvd=1;
                    const uint8_t *vid=blk+84;
                    if(vid[0]==8){ int n=0; while(n<127 && vid[1+n] && n<127){ s->vol[n]=(char)vid[1+n]; n++; }
                                   s->vol[n]=0; }
                }
            }
            if(have_lvd && have_pd){
                s->part_start=pstart;
                if(rd_lb(s,s->fsd_lb,blk,1)==IMG_OK && g16(blk)==TAG_FSD){
                    s->root_lb=g32(blk+404);
                    s->mode=0;
                    free(blk);
                    *out=s;
                    return IMG_OK;
                }
            }
        }
    }

    /* ---------- ② 回退：ISO9660 / Joliet（Linux 光盘、工具盘） ---------- */
    int rc=open_iso9660(s,blk,err,errcap);
    if(rc==IMG_OK){ free(blk); *out=s; return IMG_OK; }

    free(blk); free(s);
    if(err) snprintf(err,errcap,
        "不是 UDF 也不是 ISO9660（Windows 安装盘的 ISO9660 目录树是空的，install.wim 只在 UDF 里）");
    return IMG_EFORMAT;
}
int img_iso_open_path(img_iso **out, const char *path, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(!out || !path || !*path){ if(err) snprintf(err,errcap,"路径为空"); return IMG_EINVAL; }
    img_io *io=img_io_open_existing(path,false);      /* 只读；绝不创建 */
    if(!io){ if(err) snprintf(err,errcap,"无法打开 ISO：%s（不存在或不是普通文件）",path); return IMG_ENOENT; }
    int rc=img_iso_open_io(out,io,err,errcap);
    if(rc!=IMG_OK){ img_io_close(io); return rc; }
    (*out)->own_io=true;
    return IMG_OK;
}
void img_iso_close(img_iso *s){
    if(!s) return;
    if(s->own_io && s->io) img_io_close(s->io);
    free(s);
}
const char *img_iso_volume_id(img_iso *s){ return (s && s->vol[0]) ? s->vol : NULL; }

/* ---------- File Entry：解析数据位置 ---------- */
typedef struct {
    bool     is_dir;
    uint64_t size;
    bool     embedded;          /* 小目录/小文件：数据就嵌在 FE 里 */
    const uint8_t *emb;         /* 指向 FE 缓冲（调用方要保证缓冲活着）*/
    uint32_t emb_len;
    int      n_ext;
    lext_t   ext[64];
} fe_t;

static int read_fe(img_iso *s, uint64_t lb, uint8_t *buf, size_t bufsz, fe_t *out){
    if(rd_lb(s,lb,buf,1)!=IMG_OK) return IMG_EIO;
    uint16_t tag=g16(buf);
    if(tag!=TAG_FE && tag!=TAG_EFE) return IMG_EFORMAT;
    memset(out,0,sizeof *out);
    out->is_dir = (buf[27]==4);
    uint32_t len_ea, len_ad, ad_off;
    if(tag==TAG_FE){
        out->size=g64(buf+56); len_ea=g32(buf+168); len_ad=g32(buf+172); ad_off=176;
    } else {
        out->size=g64(buf+56); len_ea=g32(buf+208); len_ad=g32(buf+212); ad_off=216;
    }
    if(len_ea>2048 || len_ad>2048 || ad_off+len_ea+len_ad>bufsz) return IMG_EFORMAT;
    const uint8_t *ad=buf+ad_off+len_ea;
    uint16_t flags=g16(buf+34);
    unsigned adtype=flags & 0x3;
    if(getenv("IMG_ISO_DEBUG"))
        fprintf(stderr,"[iso] FE lb=%llu tag=%u type=%u size=%llu ea=%u ad=%u adtype=%u\n",
                (unsigned long long)lb, tag, out->is_dir?4:5,
                (unsigned long long)out->size, len_ea, len_ad, adtype);
    if(adtype==3){                      /* 内嵌数据 */
        out->embedded=true; out->emb=ad; out->emb_len=len_ad;
        return IMG_OK;
    }
    int sz = (adtype==1 || adtype==2) ? 16 : 8;   /* long/extended vs short */
    uint64_t sum=0;
    for(uint32_t i=0;i+sz<=len_ad && out->n_ext<64;i+=sz){
        uint32_t l=g32(ad+i);
        uint32_t type=l>>30; l&=0x3FFFFFFF;
        if(!l) break;
        if(type!=0) continue;            /* 未分配/空洞：跳过 */
        out->ext[out->n_ext].bytes=l;
        out->ext[out->n_ext].lb=g32(ad+i+4);
        sum+=l;
        out->n_ext++;
    }
    /* ★ 单位歧义：ECMA-167 说 short_ad 的长度是【逻辑块】，但微软 CDIMAGE 做的
     *   安装盘实测写的是【字节】（info=413738 / blocks_recorded=203 / AD len=413738）。
     *   UDF 1.02 与 2.x 在这点上的实现并不统一 → 用 logical_blocks_recorded(u64@64)
     *   当裁判：AD 总长 == 记录块数 → 按块；否则按字节。*/
    uint64_t brec=g64(buf+64);
    if(!(sum==brec && brec>0)){
        /* 字节单位：原样保留 */
    } else {
        for(int i=0;i<out->n_ext;i++) out->ext[i].bytes *= s->lb_size;
    }
    return IMG_OK;
}

/* 把一个 FE 的数据整体读进内存（目录用；文件走流式）*/
static int read_fe_data(img_iso *s, const fe_t *fe, uint8_t **out, size_t *outlen){
    if(fe->embedded){ *out=(uint8_t*)fe->emb; *outlen=fe->emb_len; return IMG_OK; }
    size_t total=0;
    for(int i=0;i<fe->n_ext;i++) total += (size_t)fe->ext[i].bytes;
    if(!total){ *out=NULL; *outlen=0; return IMG_OK; }
    if(total > 16u*1024*1024) return IMG_ERANGE;      /* 目录不该这么大 */
    /* ★ 按【整块】分配：extent 的字节数通常不是块大小的整数倍，
     *   直接 malloc(total) 再按块读会越界 1~2047 字节（实测堆被踩烂 → abort）。*/
    size_t rounded = ((total + s->lb_size - 1) / s->lb_size) * s->lb_size;
    uint8_t *b=malloc(rounded), *p=b;
    if(!b) return IMG_ENOMEM;
    for(int i=0;i<fe->n_ext;i++){
        uint64_t n=fe->ext[i].bytes;
        uint32_t nlb=(uint32_t)((n+s->lb_size-1)/s->lb_size);
        if(rd_lb(s,fe->ext[i].lb,p,nlb)!=IMG_OK){ free(b); return IMG_EIO; }
        p += (size_t)nlb * s->lb_size;
    }
    *out=b; *outlen=total;
    return IMG_OK;
}

/* ---------- 目录遍历 ---------- */
typedef struct { const char *want; uint64_t lb; bool found; bool is_dir; uint64_t size; } find_t;

static int iter_dir(img_iso *s, uint64_t dir_lb,
                    void (*cb)(void*, const char*, bool, uint64_t, uint64_t), void *ud){
    uint8_t feb[4096]; fe_t fe;
    int rc=read_fe(s,dir_lb,feb,sizeof feb,&fe);
    if(rc!=IMG_OK) return rc;
    if(!fe.is_dir) return IMG_EFORMAT;
    uint8_t *data=NULL; size_t len=0;
    rc=read_fe_data(s,&fe,&data,&len);
    if(rc!=IMG_OK) return rc;
    size_t i=0;
    while(i+38<=len){
        const uint8_t *f=data+i;
        if(g16(f)!=TAG_FID) break;
        uint8_t ch=f[18], nlen=f[19];
        uint32_t icb_lb=g32(f+24);
        if(ch & 0x08){ i += 40 + ((nlen+3)&~3u); continue; }      /* parent */
        char name[300]; int n=0;
        const uint8_t *np=f+38+g16(f+36);                          /* 跳过 implementation use */
        if(nlen && (np[0]==8 || np[0]==16)){
            if(np[0]==8){ for(int k=1;k<nlen && n<255;k++) name[n++]=(char)np[k]; }
            else { for(int k=1;k+1<nlen && n<253;k+=2){
                       uint16_t c=(uint16_t)(np[k]<<8|np[k+1]);
                       if(c<0x80) name[n++]=(char)c; else name[n++]='?'; } }
        } else { for(int k=0;k<nlen && n<255;k++) name[n++]=(char)np[k]; }
        name[n]=0;
        if(n>0 && strcmp(name,".") && strcmp(name,"..")){
            bool isdir=(ch & 0x02)!=0;
            uint64_t sz=0;
            if(!isdir){
                uint8_t fb2[4096]; fe_t f2;
                if(read_fe(s,icb_lb,fb2,sizeof fb2,&f2)==IMG_OK) sz=f2.size;
            }
            if(cb) cb(ud,name,isdir,sz,icb_lb);
        }
        size_t ent = 38 + g16(f+36) + nlen;
        i += (ent + 3) & ~(size_t)3;
    }
    if(!fe.embedded) free(data);
    return IMG_OK;
}

/* ---------- 路径解析（大小写不敏感） ---------- */
typedef struct { const char *want; uint64_t lb; bool dir; uint64_t size; int hit; } find_ctx;
static void find_cb(void *ud, const char *name, bool isdir, uint64_t sz, uint64_t lb){
    find_ctx *c=(find_ctx*)ud;
    if(c->hit || !name) return;
    if(!strcasecmp(name,c->want)){ c->hit=1; c->lb=lb; c->dir=isdir; c->size=sz; }
}
static int find_in_dir(img_iso *s, uint64_t dir_lb, const char *name,
                       uint64_t *out_lb, bool *is_dir, uint64_t *size){
    find_ctx c; memset(&c,0,sizeof c); c.want=name;
    int rc=iter_dir(s,dir_lb,find_cb,&c);
    if(rc!=IMG_OK) return rc;
    if(!c.hit) return IMG_ENOENT;
    if(out_lb) *out_lb=c.lb;
    if(is_dir) *is_dir=c.dir;
    if(size) *size=c.size;
    return IMG_OK;
}
static int resolve(img_iso *s, const char *path, uint64_t *out_lb, bool *is_dir, uint64_t *size){
    if(!s) return IMG_EINVAL;
    uint64_t cur=s->root_lb; bool d=true; uint64_t sz=0;
    if(path && *path && strcmp(path,"/")){
        char buf[512]; snprintf(buf,sizeof buf,"%s",path);
        char *save=NULL;
        for(char *tok=strtok_r(buf,"/\\",&save); tok; tok=strtok_r(NULL,"/\\",&save)){
            if(!*tok) continue;
            uint64_t lb=0; bool dd=false; uint64_t ss=0;
            int rc=find_in_dir(s,cur,tok,&lb,&dd,&ss);
            if(rc!=IMG_OK) return IMG_ENOENT;
            cur=lb; d=dd; sz=ss;
        }
    }
    if(out_lb) *out_lb=cur;
    if(is_dir) *is_dir=d;
    if(size) *size=sz;
    return IMG_OK;
}

int img_iso_stat(img_iso *s, const char *path, uint64_t *size, bool *is_dir){
    if(s && s->mode==1){
        uint32_t lb=0, len=0; bool d=false; uint64_t sz=0;
        int rc=iso_resolve(s,path,&lb,&len,&d,&sz,NULL);
        if(rc!=IMG_OK) return rc;
        if(is_dir) *is_dir=d;
        if(size) *size=sz;
        return IMG_OK;
    }
    if(size) *size=0;
    if(is_dir) *is_dir=false;
    uint64_t lb=0; bool d=false; uint64_t sz=0;
    int rc=resolve(s,path,&lb,&d,&sz);
    if(rc!=IMG_OK) return rc;
    if(is_dir) *is_dir=d;
    if(size) *size=sz;
    return IMG_OK;
}

/* ---------- 列目录 ---------- */
typedef struct { img_iso_entry_cb cb; void *ud; int n; } list_ctx;
static void list_cb(void *ud, const char *name, bool isdir, uint64_t sz, uint64_t lb){
    (void)lb;
    list_ctx *c=(list_ctx*)ud;
    c->n++;
    if(c->cb) c->cb(c->ud,name,isdir,sz);
}
/* ISO9660 level 3 里 >4 GiB 的文件会被拆成多条连续记录（除最后一条外都带
 * multi-extent 标志 0x80）。实测一张 4.70 GiB 的假 ISO：
 *   不合并 → ls 里出现两条 BIG.BIN（4.0 GiB + 0.7 GiB），"列出来像两个文件"
 *   合并后 → 一条 4,046,796,288… 实际 5,046,796,288 字节，与 extract 一致。
 * 注意：read/extract 早就会按多条 extent 拼（md5 对得上），只有"列目录"漏了。 */
typedef struct { img_iso_entry_cb cb; void *ud; int n;
                 char pend[300]; bool pend_dir; uint64_t pend_sz; bool pend_more; } iso_lctx;
static void iso_lctx_flush(iso_lctx *c){
    if(!c->pend[0]) return;
    c->n++; if(c->cb) c->cb(c->ud, c->pend, c->pend_dir, c->pend_sz);
    c->pend[0]=0; c->pend_more=false;
}
static void iso_list_wrap(void *ud, const iso_ent *e){
    iso_lctx *c=ud;
    bool more = (e->flags & 0x80) != 0;         /* multi-extent：后面还有 */
    if(c->pend_more && !strcasecmp(c->pend, e->name) && c->pend_dir==e->dir){
        c->pend_sz += e->size;
        c->pend_more = more;
        if(!more) iso_lctx_flush(c);
        return;
    }
    iso_lctx_flush(c);
    snprintf(c->pend,sizeof c->pend,"%s",e->name);
    c->pend_dir=e->dir; c->pend_sz=e->size; c->pend_more=more;
    if(!more) iso_lctx_flush(c);
}
int img_iso_list(img_iso *s, const char *path, img_iso_entry_cb cb, void *ud){
    if(s && s->mode==1){
        uint32_t lb=0, len=0; bool d=false;
        int rc=iso_resolve(s,(path&&*path)?path:"/",&lb,&len,&d,NULL,NULL);
        if(rc!=IMG_OK) return rc;
        if(!d) return IMG_ENOTSUP;
        iso_lctx c; memset(&c,0,sizeof c); c.cb=cb; c.ud=ud;
        rc=iso_iter(s,lb,len,iso_list_wrap,&c);
        if(rc==IMG_OK) iso_lctx_flush(&c);
        return rc==IMG_OK? c.n : rc;
    }
    uint64_t lb=0; bool d=false;
    int rc=resolve(s,(path && *path)?path:"/",&lb,&d,NULL);
    if(rc!=IMG_OK) return rc;
    if(!d) return IMG_ENOTSUP;
    list_ctx c={cb,ud,0};
    rc=iter_dir(s,lb,list_cb,&c);
    if(rc!=IMG_OK) return rc;
    return c.n;
}

/* ---------- 抽取（流式 1 MiB） ---------- */
int img_iso_read_range(img_iso *s, const char *path, uint64_t off, void *buf, size_t len){
    if(!s || !path || !buf || !len) return IMG_EINVAL;
    if(s->mode==1){
        iso_find f; uint32_t lb=0,ll=0; bool d=false; uint64_t sz=0;
        int rc=iso_resolve(s,path,&lb,&ll,&d,&sz,&f);
        if(rc!=IMG_OK) return rc;
        if(d) return IMG_ENOTSUP;
        uint8_t *p=(uint8_t*)buf; uint64_t done=0, base=0;
        for(int i=0;i<f.n && i<32 && done<len;i++){
            uint64_t elen=f.ext_lens[i];
            if(off < base+elen){
                uint64_t skip = off>base ? off-base : 0;
                uint64_t want = elen-skip; if(want>len-done) want=len-done;
                if(img_io_read_full(s->io,(uint64_t)f.extents[i]*2048+skip,p+done,(size_t)want)!=IMG_OK) return IMG_EIO;
                done+=want; off+=want;
            }
            base+=elen;
        }
        return done==len ? IMG_OK : IMG_ETRUNC;
    }
    uint64_t lb=0; bool d=false; uint64_t fsz=0;
    int rc=resolve(s,path,&lb,&d,&fsz);
    if(rc!=IMG_OK) return rc;
    if(d) return IMG_ENOTSUP;
    uint8_t feb[4096]; fe_t fe;
    rc=read_fe(s,lb,feb,sizeof feb,&fe);
    if(rc!=IMG_OK) return rc;
    uint8_t *p=(uint8_t*)buf; uint64_t done=0, base=0;
    for(int i=0;i<fe.n_ext && done<len;i++){
        uint64_t elen=fe.ext[i].bytes;
        if(off < base+elen){
            uint64_t skip = off>base ? off-base : 0;
            uint64_t want = elen-skip; if(want>len-done) want=len-done;
            if(img_io_read_full(s->io,((uint64_t)s->part_start+fe.ext[i].lb)*s->lb_size+skip,
                                p+done,(size_t)want)!=IMG_OK) return IMG_EIO;
            done+=want; off+=want;
        }
        base+=elen;
    }
    return done==len ? IMG_OK : IMG_ETRUNC;
}

int img_iso_extract(img_iso *s, const char *path, const char *dst,
                    uint64_t max_bytes, img_prog *prog, uint64_t *out_bytes){
    if(out_bytes) *out_bytes=0;
    if(!s || !path || !dst) return IMG_EINVAL;
    if(s->mode==1){
        iso_find f;
        uint32_t lb=0,len=0; bool d=false; uint64_t sz=0;
        int rc=iso_resolve(s,path,&lb,&len,&d,&sz,&f);
        if(rc!=IMG_OK) return rc;
        if(d) return IMG_ENOTSUP;
        uint64_t want=(max_bytes && max_bytes<f.total)?max_bytes:f.total;
        FILE *o=fopen(dst,"wb"); if(!o) return IMG_EIO;
        if(prog){ img_prog_op(prog,IMG_OP_READ);
                  img_prog_phase(prog,IMG_PH_EXTRACT,"从 ISO 抽取");
                  img_prog_bytes(prog,0,want); }
        enum { CHUNK=1u<<20 };
        uint8_t *buf=malloc(CHUNK);
        if(!buf){ fclose(o); return IMG_ENOMEM; }
        uint64_t done=0;
        for(int i=0;i<f.n && i<32 && rc==IMG_OK;i++){
            uint64_t off=(uint64_t)f.extents[i]*2048, left=f.ext_lens[i];
            while(left && rc==IMG_OK){
                if(want && done>=want) break;
                size_t n=(size_t)(left<CHUNK?left:CHUNK);
                if(want && done+n>want) n=(size_t)(want-done);
                if(img_io_read_full(s->io,off,buf,n)!=IMG_OK){ rc=IMG_EIO; break; }
                if(fwrite(buf,1,n,o)!=n){ rc=IMG_EIO; break; }
                off+=n; left-=n; done+=n;
                if(prog) img_prog_bytes(prog,done,want?want:done);
                img_hb("iso","extract","iso9660",done>>20);
                if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
            }
        }
        free(buf); fclose(o);
        if(rc!=IMG_OK) unlink(dst);
        if(out_bytes) *out_bytes=done;
        return rc;
    }
    uint64_t lb=0; bool d=false; uint64_t fsz=0;
    int rc=resolve(s,path,&lb,&d,&fsz);
    if(rc!=IMG_OK) return rc;
    if(d) return IMG_ENOTSUP;

    uint8_t feb[4096]; fe_t fe;
    rc=read_fe(s,lb,feb,sizeof feb,&fe);
    if(rc!=IMG_OK) return rc;

    uint64_t total = fe.size ? fe.size : fsz;
    uint64_t want = (max_bytes && (!total || max_bytes<total)) ? max_bytes : total;

    FILE *o=fopen(dst,"wb");
    if(!o) return IMG_EIO;
    if(prog){ img_prog_op(prog,IMG_OP_DECOMPRESS);
              img_prog_phase(prog,IMG_PH_EXTRACT,"从 ISO 抽取");
              img_prog_bytes(prog,0,want); }

    enum { CHUNK = 1u<<20 };
    uint8_t *buf=malloc(CHUNK);
    if(!buf){ fclose(o); return IMG_ENOMEM; }
    uint64_t done=0;
    if(fe.embedded){
        size_t n=fe.emb_len; if(want && n>want) n=(size_t)want;
        if(n && fwrite(fe.emb,1,n,o)!=n) rc=IMG_EIO; else done=n;
    } else {
        for(int i=0;i<fe.n_ext && rc==IMG_OK;i++){
            uint64_t left=fe.ext[i].bytes;
            uint64_t off=((uint64_t)s->part_start + fe.ext[i].lb) * s->lb_size;
            while(left && rc==IMG_OK){
                if(want && done>=want) break;
                size_t n=(size_t)(left<CHUNK?left:CHUNK);
                if(want && done+n>want) n=(size_t)(want-done);
                if(img_io_read_full(s->io,off,buf,n)!=IMG_OK){ rc=IMG_EIO; break; }
                if(fwrite(buf,1,n,o)!=n){ rc=IMG_EIO; break; }
                off+=n; left-=n; done+=n;
                if(prog) img_prog_bytes(prog,done,want?want:done);
                img_hb("iso","extract","copy_out",done>>20);
                if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
            }
        }
    }
    free(buf); fclose(o);
    if(out_bytes) *out_bytes=done;
    if(rc!=IMG_OK) unlink(dst);
    return rc;
}
