/* img_identify.c — 从内容读 Windows 版本（WIM/ESD 的 XML；已装盘的 SOFTWARE 注册表）*/
#define _DEFAULT_SOURCE
#include "img_identify.h"
#include "img_ntfs_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <hivex.h>

/* UTF-16LE → UTF-8（中文名要能显示，不能全变 '?'）*/
static size_t u16_to_utf8(const uint8_t *src, size_t nbytes, char *out, size_t cap){
    size_t j=0;
    for(size_t i=0;i+1<nbytes;i+=2){
        uint32_t c=(uint32_t)(src[i] | (src[i+1]<<8));
        if(c>=0xD800 && c<=0xDBFF && i+3<nbytes){           /* 代理对 */
            uint32_t c2=(uint32_t)(src[i+2] | (src[i+3]<<8));
            if(c2>=0xDC00 && c2<=0xDFFF){ c=0x10000+((c-0xD800)<<10)+(c2-0xDC00); i+=2; }
        }
        if(!c) break;
        if(c<0x80){ if(j+1>=cap) break; out[j++]=(char)c; }
        else if(c<0x800){ if(j+2>=cap) break; out[j++]=(char)(0xC0|(c>>6)); out[j++]=(char)(0x80|(c&0x3F)); }
        else if(c<0x10000){ if(j+3>=cap) break; out[j++]=(char)(0xE0|(c>>12)); out[j++]=(char)(0x80|((c>>6)&0x3F)); out[j++]=(char)(0x80|(c&0x3F)); }
        else { if(j+4>=cap) break; out[j++]=(char)(0xF0|(c>>18)); out[j++]=(char)(0x80|((c>>12)&0x3F)); out[j++]=(char)(0x80|((c>>6)&0x3F)); out[j++]=(char)(0x80|(c&0x3F)); }
    }
    out[j]=0;
    return j;
}

static uint64_t g64(const uint8_t *p){ return (uint64_t)p[0]|((uint64_t)p[1]<<8)|((uint64_t)p[2]<<16)|((uint64_t)p[3]<<24)
        |((uint64_t)p[4]<<32)|((uint64_t)p[5]<<40)|((uint64_t)p[6]<<48)|((uint64_t)p[7]<<56); }

/* 在 UTF-16LE 文本里取 <TAG>value</TAG>（只处理 ASCII 字符）*/
static int tag_value(const char *t, const char *tag, char *out, size_t cap){
    char open[64], close[64];
    snprintf(open,sizeof open,"<%s>",tag);
    snprintf(close,sizeof close,"</%s>",tag);
    const char *p=strstr(t,open);
    if(!p) return -1;
    p+=strlen(open);
    const char *e=strstr(p,close);
    if(!e) return -1;
    size_t n=(size_t)(e-p); if(n>=cap) n=cap-1;
    memcpy(out,p,n); out[n]=0;
    /* 去尾空白 */
    while(n && (out[n-1]==' '||out[n-1]=='\r'||out[n-1]=='\n')) out[--n]=0;
    return 0;
}

int img_wim_identify(img_rd_fn rd, void *ud,
                     img_wim_image_t *out, int max_images, int *out_count,
                     char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(out_count) *out_count=0;
    if(!rd || !out) return IMG_EINVAL;

    uint8_t hdr[208];
    if(rd(ud,0,hdr,sizeof hdr)!=IMG_OK){ if(err) snprintf(err,errcap,"读不到 WIM 头"); return IMG_EIO; }
    if(memcmp(hdr,"MSWIM\0\0\0",8)!=0){ if(err) snprintf(err,errcap,"不是 WIM/ESD（魔数不符）"); return IMG_EFORMAT; }

    uint64_t xoff=g64(hdr+0x50), xsize=g64(hdr+0x58);
    if(xsize==0 || xsize>4u*1024*1024){ if(err) snprintf(err,errcap,"WIM XML 位置异常"); return IMG_EFORMAT; }
    uint8_t *raw=malloc((size_t)xsize+2);
    if(!raw) return IMG_ENOMEM;
    if(rd(ud,xoff,raw,(size_t)xsize)!=IMG_OK){ free(raw); if(err) snprintf(err,errcap,"读不到 WIM XML"); return IMG_EIO; }

    /* UTF-16LE → ASCII（够解析标签；非 ASCII 变 '?'）*/
    size_t tn=(size_t)xsize*2;           /* UTF-8 最多是 UTF-16 的 2 倍 */
    char *t=malloc(tn+1);
    if(!t){ free(raw); return IMG_ENOMEM; }
    u16_to_utf8(raw,(size_t)xsize,t,tn+1);
    free(raw);

    int cnt=0;
    const char *p=t;
    while(cnt<max_images){
        const char *b=strstr(p,"<IMAGE INDEX=\"");
        if(!b) break;
        const char *e=strstr(b,"</IMAGE>");
        if(!e) break;
        img_wim_image_t *im=&out[cnt];
        memset(im,0,sizeof *im);
        im->index=atoi(b+14);
        tag_value(b,"NAME",im->name,sizeof im->name);
        tag_value(b,"DISPLAYNAME",im->display_name,sizeof im->display_name);
        tag_value(b,"EDITIONID",im->edition_id,sizeof im->edition_id);
        char tmp[32]="";
        if(tag_value(b,"ARCH",tmp,sizeof tmp)==0) snprintf(im->arch,sizeof im->arch,"%s",tmp);
        if(tag_value(b,"TOTALBYTES",tmp,sizeof tmp)==0) im->total_bytes=strtoull(tmp,NULL,10);
        if(tag_value(b,"DIRCOUNT",tmp,sizeof tmp)==0) im->dir_count=atoi(tmp);
        if(tag_value(b,"FILECOUNT",tmp,sizeof tmp)==0) im->file_count=atoi(tmp);
        if(tag_value(b,"MAJOR",tmp,sizeof tmp)==0) im->major=atoi(tmp);
        if(tag_value(b,"MINOR",tmp,sizeof tmp)==0) im->minor=atoi(tmp);
        if(tag_value(b,"BUILD",tmp,sizeof tmp)==0) im->build=atoi(tmp);
        /* 语言：取第一个 <LANGUAGE> */
        if(tag_value(b,"LANGUAGE",im->languages,sizeof im->languages)!=0) im->languages[0]=0;
        cnt++;
        p=e+8;
    }
    free(t);
    if(out_count) *out_count=cnt;
    if(!cnt){ if(err) snprintf(err,errcap,"XML 里没有 <IMAGE>"); return IMG_EFORMAT; }
    return IMG_OK;
}

/* skus 目录名收集（edition 证据）*/
typedef struct { char *p; size_t cap, len; } skus_acc;
static void skus_cb(void *ud, const char *nm, bool isdir, uint64_t sz){
    (void)sz;
    skus_acc *c=(skus_acc*)ud;
    if(!isdir || !nm || !*nm) return;
    size_t n=strlen(nm);
    if(c->len+n+2>=c->cap) return;
    if(c->len) c->p[c->len++]=',', c->p[c->len]=0;
    memcpy(c->p+c->len,nm,n); c->len+=n; c->p[c->len]=0;
}

/* ---------- 已装盘：读 SOFTWARE 里的 Windows NT\CurrentVersion ---------- */
static int hive_str(hive_h *h, hive_node_h node, const char *name, char *out, size_t cap){
    if(cap) out[0]=0;
    hive_value_h v=hivex_node_get_value(h,node,name);
    if(!v) return -1;
    char *d=NULL; size_t len=0; hive_type t=0;
    d=hivex_value_value(h,v,&t,&len);
    if(!d) return -1;
    if(t==hive_t_string || t==hive_t_expand_string){
        u16_to_utf8((const uint8_t*)d,len,out,cap);
    } else {
        size_t n=len<cap-1?len:cap-1;
        memcpy(out,d,n); out[n]=0;
    }
    free(d);
    return 0;
}

int img_win_version(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                    img_winver_t *out, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(!q || !out) return IMG_EINVAL;
    memset(out,0,sizeof *out);

    const char *tmp=getenv("IMGTOOL_TMPDIR"); if(!tmp||!*tmp) tmp=getenv("TMPDIR");
    if(!tmp||!*tmp) tmp="/tmp";
    char tf_sw[600], tf_kn[600];
    snprintf(tf_sw,sizeof tf_sw,"%s/imgtool-software-%ld",tmp,(long)getpid());
    snprintf(tf_kn,sizeof tf_kn,"%s/imgtool-ntos-%ld.exe",tmp,(long)getpid());

    /* ★ 只挂载一次：把 SOFTWARE、ntoskrnl.exe、skus 目录都在这一次挂载里读完。
     *   分多次挂载会在"卷被标记脏"时第二次失败（实测某张盘就是这样丢掉内核版本的）。*/
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,part_lba,part_bytes);
    if(!fs){ if(err) snprintf(err,errcap,"系统分区不是合法 NTFS"); return IMG_ESTATE; }

    uint64_t got=0;
    int rc_sw=img_ntfs_copy_out(fs,"/Windows/System32/config/SOFTWARE",tf_sw,NULL,&got);
    uint64_t ksz=0;
    int rc_kn=img_ntfs_copy_out(fs,"/Windows/System32/ntoskrnl.exe",tf_kn,NULL,&ksz);

    if(getenv("IMG_PE_DEBUG")) fprintf(stderr,"[ver] rc_sw=%d rc_kn=%d ksz=%llu\n",rc_sw,rc_kn,(unsigned long long)ksz);
    skus_acc sc={out->skus,sizeof out->skus,0};
    img_ntfs_list_dir(fs,"/Windows/System32/spp/tokens/skus",skus_cb,&sc);

    img_ntfs_umount(fs);

    /* ① 注册表 */
    if(rc_sw==IMG_OK){
        hive_h *h=hivex_open(tf_sw,0);
        if(h){
            hive_node_h n=hivex_root(h);
            const char *path[]={"Microsoft","Windows NT","CurrentVersion"};
            for(int i=0;i<3 && n;i++) n=hivex_node_get_child(h,n,path[i]);
            if(n){
                hive_str(h,n,"ProductName",out->product_name,sizeof out->product_name);
                hive_str(h,n,"EditionID",out->edition_id,sizeof out->edition_id);
                hive_str(h,n,"BuildLab",out->build_lab,sizeof out->build_lab);
                hive_str(h,n,"DisplayVersion",out->display_version,sizeof out->display_version);
                hive_str(h,n,"CurrentVersion",out->current_version,sizeof out->current_version);
                char cb[32]="";
                hive_str(h,n,"CurrentBuild",cb,sizeof cb);
                if(!cb[0]) hive_str(h,n,"CurrentBuildNumber",cb,sizeof cb);
                out->current_build=atoi(cb);
            }
            hivex_close(h);
        }
    }
    unlink(tf_sw);

    /* ② 特征文件：ntoskrnl.exe 的 PE 版本资源 */
    if(rc_kn==IMG_OK && ksz>0 && ksz<64u*1024*1024){
        FILE *fp=fopen(tf_kn,"rb");
        if(fp){
            uint8_t *b=malloc((size_t)ksz);
            if(b && fread(b,1,(size_t)ksz,fp)==(size_t)ksz) img_pe_version(b,(size_t)ksz,out);
            free(b); fclose(fp);
        }
    }
    unlink(tf_kn);

    if(out->kernel_build>0 && out->current_build>0 &&
       out->kernel_build != out->current_build) out->mismatch=1;

    if(!out->product_name[0] && !out->kernel_file_version[0]){
        if(err) snprintf(err,errcap,"读不出版本（SOFTWARE 与 ntoskrnl 都没拿到）");
        return IMG_ENOENT;
    }
    return IMG_OK;
}

/* ================================================================== */
/* PE 版本资源解析（读 ntoskrnl.exe 的 VS_VERSIONINFO）                */
/* ================================================================== */
#define PEDBG(...) do{ if(getenv("IMG_PE_DEBUG")) fprintf(stderr,"[pe] " __VA_ARGS__); }while(0)
static uint32_t r32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t r16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }

/* 在版本信息块里找某个 String（VS_VERSIONINFO 的子结构）*/
static void vs_string(const uint8_t *base, size_t len, const char *key, char *out, size_t cap){
    char pat[64]; snprintf(pat,sizeof pat,"%s",key);
    size_t klen=strlen(pat);
    for(size_t i=0;i+8<len;i++){
        /* 找 16 位对齐的 key（UTF-16LE）*/
        if(base[i]==pat[0] && base[i+1]==0){
            size_t j=0, ok=1;
            for(; j<klen; j++){
                if(i+2*j+1>=len || base[i+2*j]!=(uint8_t)pat[j] || base[i+2*j+1]!=0){ ok=0; break; }
            }
            if(!ok) continue;
            size_t after=i+2*klen;
            /* key 后面是 padding 到 4 字节，然后 wValueLength/wType，再是字符串 */
            while(after%4) after++;
            if(after+4>=len) continue;
            uint16_t vlen=r16(base+after);
            size_t sp=after+4;
            if(sp+ (size_t)vlen*2 > len) vlen=(uint16_t)((len-sp)/2);
            size_t n=0;
            for(size_t t=0;t<vlen && n+1<cap;t++){
                uint16_t c=(uint16_t)(base[sp+2*t] | (base[sp+2*t+1]<<8));
                if(!c) break;
                out[n++]=(c<128)?(char)c:'?';
            }
            out[n]=0;
            return;
        }
    }
}

int img_pe_version(const uint8_t *buf, size_t len, img_winver_t *out){
    if(!buf || len<0x40 || !out) return IMG_EINVAL;
    if(buf[0]!='M' || buf[1]!='Z') return IMG_EFORMAT;
    uint32_t pe=r32(buf+0x3C);
    if(pe+0x18>len || memcmp(buf+pe,"PE\0\0",4)!=0){ PEDBG("bad PE sig pe=%u len=%zu\n",pe,len); return IMG_EFORMAT; }
    uint16_t nsec=r16(buf+pe+6);
    uint16_t optsz=r16(buf+pe+20);
    const uint8_t *opt=buf+pe+24;
    uint16_t magic=r16(opt);
    uint32_t dd = (magic==0x20b) ? 112 : 96;      /* DataDirectory 起点 */
    if(optsz<dd+16*3) return IMG_EFORMAT;
    uint32_t rsrc_rva=r32(opt+dd+2*8+0);
    uint32_t rsrc_sz =r32(opt+dd+2*8+4);
    if(!rsrc_rva || !rsrc_sz){ PEDBG("no rsrc rva=%u sz=%u\n",rsrc_rva,rsrc_sz); return IMG_ENOENT; }

    const uint8_t *sec=buf+pe+24+optsz;
    /* RVA → 文件偏移 */
    uint32_t off=0;
    for(int i=0;i<nsec;i++){
        const uint8_t *s=sec+i*40;
        uint32_t va=r32(s+12), raw=r32(s+20), rsz=r32(s+16);
        if(rsrc_rva>=va && rsrc_rva<va+rsz){ off=raw+(rsrc_rva-va); break; }
    }
    if(!off || off+16>len){ PEDBG("rsrc not mapped off=%u\n",off); return IMG_ENOENT; }

    /* 资源目录三层：type(16=RT_VERSION) → name(1) → lang */
    const uint8_t *res=buf+off;
    size_t rlen=len-off;
    uint32_t nent=r16(res+12), nname=r16(res+14);
    const uint8_t *ent=res+16;
    uint32_t ver_off=0;
    for(uint32_t i=0;i<nent+nname;i++){
        uint32_t id=r32(ent+i*8), po=r32(ent+i*8+4);
        if(!(po & 0x80000000)) continue;
        if(id==16){ ver_off=po & 0x7FFFFFFF; break; }
    }
    if(!ver_off || ver_off+16>rlen){ PEDBG("no RT_VERSION ver_off=%u rlen=%zu\n",ver_off,rlen); return IMG_ENOENT; }
    const uint8_t *lvl2=res+ver_off;
    nent=r16(lvl2+12); nname=r16(lvl2+14);
    if(nent+nname<1) return IMG_ENOENT;
    uint32_t po=r32(lvl2+16+4);
    if(!(po & 0x80000000)) return IMG_ENOENT;
    const uint8_t *lvl3=res+(po & 0x7FFFFFFF);
    if(lvl3+16>res+rlen) return IMG_ENOENT;
    uint32_t n3=r16(lvl3+12)+r16(lvl3+14);
    if(n3<1) return IMG_ENOENT;
    uint32_t doff=r32(lvl3+16+4);
    if(doff & 0x80000000) return IMG_ENOENT;      /* 应该是叶子：数据项 */
    if(doff+16>rlen) return IMG_ENOENT;
    uint32_t data_rva=r32(res+doff), data_sz=r32(res+doff+4);
    uint32_t d_off=0;
    for(int i=0;i<nsec;i++){
        const uint8_t *s=sec+i*40;
        uint32_t va=r32(s+12), raw=r32(s+20), rsz=r32(s+16);
        if(data_rva>=va && data_rva<va+rsz){ d_off=raw+(data_rva-va); break; }
    }
    if(!d_off || d_off+40>len) return IMG_ENOENT;

    const uint8_t *vi=buf+d_off;
    size_t vlen = data_sz<=len-d_off ? data_sz : (len-d_off);

    /* VS_FIXEDFILEINFO：在块里找签名 0xFEEF04BD */
    int found=0;
    for(size_t k=0;k+52<=vlen && k<512;k+=4){
        if(r32(vi+k)==0xFEEF04BDu){
            uint32_t fms=r32(vi+k+8), fls=r32(vi+k+12);
            uint32_t pms=r32(vi+k+16), pls=r32(vi+k+20);
            out->kernel_major=(int)(fms>>16);
            out->kernel_minor=(int)(fms&0xFFFF);
            out->kernel_build=(int)(fls>>16);
            snprintf(out->kernel_file_version,sizeof out->kernel_file_version,
                     "%u.%u.%u.%u", fms>>16, fms&0xFFFF, fls>>16, fls&0xFFFF);
            snprintf(out->kernel_product_version,sizeof out->kernel_product_version,
                     "%u.%u.%u.%u", pms>>16, pms&0xFFFF, pls>>16, pls&0xFFFF);
            found=1;
            break;
        }
    }
    if(!found){ PEDBG("no VS_FIXEDFILEINFO\n"); return IMG_ENOENT; }

    vs_string(vi,vlen,"CompanyName",out->kernel_company,sizeof out->kernel_company);
    vs_string(vi,vlen,"FileDescription",out->kernel_description,sizeof out->kernel_description);
    return IMG_OK;
}
