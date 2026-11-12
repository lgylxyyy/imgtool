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

/* <ARCH> 原值 → x86|x64|arm|arm64（不认识的一律空串） */
void img_arch_normalize(const char *raw, char *out, size_t cap){
    out[0]=0; if(!raw) return;
    if(!strcasecmp(raw,"9")   || !strcasecmp(raw,"x64")   || !strcasecmp(raw,"amd64")) snprintf(out,cap,"x64");
    else if(!strcasecmp(raw,"0") || !strcasecmp(raw,"x86") || !strcasecmp(raw,"i386")) snprintf(out,cap,"x86");
    else if(!strcasecmp(raw,"5") || !strcasecmp(raw,"arm"))                            snprintf(out,cap,"arm");
    else if(!strcasecmp(raw,"12")|| !strcasecmp(raw,"arm64")|| !strcasecmp(raw,"aarch64")) snprintf(out,cap,"arm64");
}

int img_wim_identify(img_rd_fn rd, void *ud,
                     img_wim_image_t *out, int max_images, int *out_count,
                     char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(out_count) *out_count=0;
    if(!rd || !out) return IMG_EINVAL;

    uint8_t hdr[208];
    if(rd(ud,0,hdr,sizeof hdr)!=IMG_OK){ if(err) snprintf(err,errcap,"无法读取 WIM 头"); return IMG_EIO; }
    if(memcmp(hdr,"MSWIM\0\0\0",8)!=0){ if(err) snprintf(err,errcap,"不是 WIM/ESD（魔数不符）"); return IMG_EFORMAT; }

    uint64_t xoff=g64(hdr+0x50), xsize=g64(hdr+0x58);
    if(xsize==0 || xsize>4u*1024*1024){ if(err) snprintf(err,errcap,"WIM XML 位置异常"); return IMG_EFORMAT; }
    uint8_t *raw=malloc((size_t)xsize+2);
    if(!raw) return IMG_ENOMEM;
    if(rd(ud,xoff,raw,(size_t)xsize)!=IMG_OK){ free(raw); if(err) snprintf(err,errcap,"无法读取 WIM XML"); return IMG_EIO; }

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
        /* ★ REQ-19：<ARCH> 原值直接给 App 没用 —— 微软这批镜像 x64 盘里也是 <ARCH>0</ARCH>。
         *   所以：原值进 arch_raw，arch 归一成可判断的四个值（判不出就是空串，
         *   App 那边"宁缺勿错"，不显示括号）。*/
        if(tag_value(b,"ARCH",tmp,sizeof tmp)==0){
            snprintf(im->arch_raw,sizeof im->arch_raw,"%s",tmp);
            img_arch_normalize(tmp,im->arch,sizeof im->arch);
        }
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

/* \Windows\servicing\Version\<a.b.build.rev> —— 注意它是【目录】，不是文件。
 * 同目录里还有一个 8.3 短名（实测为 617601~1.175），必须按"四段数字"校验排除。*/
typedef struct { char *p; size_t cap; int got; } svc_acc;
static void svc_cb(void *ud, const char *nm, bool isdir, uint64_t sz){
    (void)sz;
    svc_acc *c=(svc_acc*)ud;
    if(c->got || !nm || !*nm || !isdir) return;
    int a=0,b=0,d=0,e=0;
    if(sscanf(nm,"%d.%d.%d.%d",&a,&b,&d,&e)!=4 || d<=0) return;
    snprintf(c->p,c->cap,"%s",nm);
    c->got=1;
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

/* 读一个 REG_DWORD（UBR 这种）；没有/类型不对返回 -1，不报错 */
static int hive_dword(hive_h *h, hive_node_h node, const char *name, uint32_t *out){
    if(out) *out=0;
    hive_value_h v=hivex_node_get_value(h,node,name);
    if(!v) return -1;
    hive_type t=0; size_t len=0;
    if(hivex_value_type(h,v,&t,&len)<0) return -1;
    if(t!=hive_t_REG_DWORD || len!=4) return -1;
    char *d=hivex_value_value(h,v,&t,&len);
    if(!d) return -1;
    uint32_t dw=0; memcpy(&dw,d,4); free(d);
    if(out) *out=dw;
    return 0;
}

/* ---------- XP/2000 的 SP 级别兜底（REQ-75）----------
 * XP 的 SP 级别在 SYSTEM 里存成 DWORD：
 *   Select\Current → ControlSet00N\Control\Windows\CSDVersion
 *   0x100=SP1  0x200=SP2  0x300=SP3  0x400=SP4(2000)
 * 官方 SP3 安装光盘的默认 hive（HIVESFT.INF）里 SOFTWARE 没有字符串版，只有这一份。
 * ★ CurrentControlSet 是运行时的符号链接，离线 hive 里通常没有这个键，
 *   必须经 Select\Current 折算出 ControlSet00N 再走过去。 */
static void csd_from_dword(uint32_t dw, char *out, size_t cap){
    if(out && cap) out[0]=0;
    const char *s=NULL;
    switch(dw){
        case 0x100: s="Service Pack 1"; break;
        case 0x200: s="Service Pack 2"; break;
        case 0x300: s="Service Pack 3"; break;
        case 0x400: s="Service Pack 4"; break;   /* Win2000 SP4 */
        default: break;                          /* 0x0=RTM；未知值不猜，保持空串 */
    }
    if(s) snprintf(out,cap,"%s",s);
}

static void csd_try_system(const char *tf_sys, img_winver_t *out){
    if(out->csd_version[0]) return;              /* 字符串版已有，不折腾 */
    /* 只在 XP/2000 家族才多挂一路：8/8.1/10/11 本来就没有 CSDVersion 字符串
     * （REPLY-74 实测 Win8 为空串），按"空就挂 SYSTEM"会让它们每次都白挂。 */
    if(!(out->product_name[0] &&
         (strstr(out->product_name,"Windows XP") ||
          strstr(out->product_name,"Windows 2000")))) return;
    hive_h *h=hivex_open(tf_sys,0);
    if(!h) return;
    hive_node_h root=hivex_root(h);
    hive_node_h nw=0;
    hive_node_h ns=hivex_node_get_child(h,root,"Select");
    uint32_t cur=0;
    if(ns && hive_dword(h,ns,"Current",&cur)==0 && cur>=1 && cur<=999){
        char cs[32]; snprintf(cs,sizeof cs,"ControlSet%03u",cur);
        hive_node_h ncs=hivex_node_get_child(h,root,cs);
        if(ncs) nw=hivex_node_get_child(h,ncs,"Control");
    }
    if(!nw){                                     /* 个别 hive 真带 CurrentControlSet，试一下不亏 */
        hive_node_h nccs=hivex_node_get_child(h,root,"CurrentControlSet");
        if(nccs) nw=hivex_node_get_child(h,nccs,"Control");
    }
    if(nw){
        hive_node_h nwin=hivex_node_get_child(h,nw,"Windows");
        uint32_t dw=0;
        if(nwin && hive_dword(h,nwin,"CSDVersion",&dw)==0)
            csd_from_dword(dw,out->csd_version,sizeof out->csd_version);
    }
    hivex_close(h);
}

/* build 的"代" = 发布桶，不是 build/10。
 * ★ 实测教训：Win11 26300 的盘里注册表写 26300、内核等文件是 26100（24H2 基线），
 *   用 build/10 会得到 2630 与 2610 两个"代" → 误报 mismatch=true。
 *   两者都是 Windows 11，差的是 servicing，不该报警。
 * build 区间互不重叠，所以单看 build 就够： */
int img_build_family(int b){
    if(b <= 0)     return 0;
    if(b <  6000)  return 5;    /* NT5.x：XP x64 / 2003（3790/5xxx） */
    if(b <  7000)  return 60;   /* Vista  6000 / 6001 / 6002 */
    if(b <  9000)  return 61;   /* Win7   7600 / 7601 */
    if(b <  9600)  return 62;   /* Win8   9200 */
    if(b < 10000)  return 63;   /* Win8.1 9600 */
    if(b < 22000)  return 100;  /* Win10  10240 … 19045 */
    return 101;                 /* Win11  22000 / 22621 / 22631 / 26100 / 26300 … */
}

/* 投票：按"代"取众数（详见头文件注释）。分离成公开函数是为了能单测。 */
void img_winver_vote(img_winver_t *v){
    if(!v) return;
    v->consensus_build=0; v->mismatch=0;
    if(v->n_vote<=0) return;
    int best=0,bestn=0,bestreg=0;
    for(int i=0;i<v->n_vote;i++){
        int cnt=0;
        for(int j=0;j<v->n_vote;j++)
            if(img_build_family(v->vote[j].build)==img_build_family(v->vote[i].build)) cnt++;
        int isreg=!strcmp(v->vote[i].src,"registry");
        /* 平票时取注册表那一票：用户看到的"系统版本"就是它 */
        if(cnt>bestn || (cnt==bestn && isreg && !bestreg)){
            bestn=cnt; best=v->vote[i].build; bestreg=isreg;
        }
    }
    v->consensus_build=best;
    if(bestn < v->n_vote) v->mismatch=1;
}

int img_win_version(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                    img_winver_t *out, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(!q || !out) return IMG_EINVAL;
    memset(out,0,sizeof *out);

    const char *tmp=getenv("IMGTOOL_TMPDIR"); if(!tmp||!*tmp) tmp=getenv("TMPDIR");
    if(!tmp||!*tmp){ const char *t2=img_tmpdir(); if(t2) tmp=t2; }
    char tf_sw[600];
    img_tmpfile("pe-software",tf_sw,sizeof tf_sw);   /* P1-6 统一命名 */

    /* ★ 只挂载一次：把 SOFTWARE、4 个核心二进制、skus / servicing 目录都在这一次挂载里读完。
     *   分多次挂载会在"卷被标记脏"时第二次失败（实测某张盘就是这样丢掉内核版本的）。*/
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,part_lba,part_bytes);
    if(!fs){ if(err) snprintf(err,errcap,"系统分区不是合法 NTFS"); return IMG_ESTATE; }

    uint64_t got=0;
    int rc_sw=img_ntfs_copy_out(fs,"/Windows/System32/config/SOFTWARE",tf_sw,NULL,&got);

    /* XP/2000 的 SP 级别兜底要读 SYSTEM（REQ-75）：同一次挂载里一起取出来，
     * 不违背上面的"只挂载一次"；用不用由注册表读完后的家族判定决定。*/
    char tf_sys[600]; img_tmpfile("pe-system",tf_sys,sizeof tf_sys);   /* P1-6 统一命名 */
    int rc_sys=img_ntfs_copy_out(fs,"/Windows/System32/config/SYSTEM",tf_sys,NULL,&got);

    /* ② 核心二进制：这些文件被改过就开不了机，所以它们的 PE 版本最可信 */
    static const char *core[]={
        "/Windows/System32/ntoskrnl.exe",
        "/Windows/System32/ntdll.dll",
        "/Windows/System32/winload.exe",
        "/Windows/System32/winload.efi",
    };
    static const char *label[]={"ntoskrnl","ntdll","winload.exe","winload.efi"};
    char cf[4][600]; uint64_t csz[4]={0,0,0,0}; int chave[4]={0,0,0,0}, cexist[4]={0,0,0,0};
    for(int i=0;i<4;i++){
        cf[i][0]=0; csz[i]=0; chave[i]=0; cexist[i]=0;
        if(img_ntfs_path_exists(fs,core[i])!=true) continue;
        cexist[i]=1;
        { char nm[64]; snprintf(nm,sizeof nm,"pe-core-%d",i);
          img_tmpfile(nm,cf[i],sizeof cf[i]); }   /* P1-6 统一命名 */
        if(img_ntfs_copy_out(fs,core[i],cf[i],NULL,&csz[i])==IMG_OK &&
           csz[i]>0 && csz[i]<=64u*1024*1024) chave[i]=1;
        else { unlink(cf[i]); csz[i]=0; }

    }

    for(int i=0;i<4;i++){        /* 存在但没读出建造号：如实登记（WOF/CompactOS）*/
        if(chave[i] || !cexist[i]) continue;
        size_t n=strlen(out->skipped);
        if(n+strlen(label[i])+2<sizeof out->skipped)
            snprintf(out->skipped+n,sizeof out->skipped-n,"%s%s",n?",":"",label[i]);
    }

    skus_acc sc={out->skus,sizeof out->skus,0};
    img_ntfs_list_dir(fs,"/Windows/System32/spp/tokens/skus",skus_cb,&sc);
    svc_acc sa={out->servicing_version,sizeof out->servicing_version,0};
    img_ntfs_list_dir(fs,"/Windows/servicing/Version",svc_cb,&sa);

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
                hive_str(h,n,"ReleaseId",out->release_id,sizeof out->release_id);
                hive_str(h,n,"InstallationType",out->installation_type,sizeof out->installation_type);
                /* XP / Vista / 7 的服务包级别：XP 只有 2600 一个 build，SP2 与 SP3 只能靠它区分 */
                hive_str(h,n,"CSDVersion",out->csd_version,sizeof out->csd_version);
                hive_dword(h,n,"UBR",&out->ubr);
                hive_str(h,n,"CurrentVersion",out->current_version,sizeof out->current_version);
                char cb[32]="";
                hive_str(h,n,"CurrentBuild",cb,sizeof cb);
                /* ★ 2026-11-12 真盘实测（XP SP3）：XP 的 CurrentBuild 写的是 "1"，
                 *   真实 build 在 CurrentBuildNumber（REG_SZ "2600"）。两个都读，
                 *   取较大的那个——XP 修成 2600，Win10/11 两值相同不受影响。*/
                {
                    char cbn[32]={0};
                    hive_str(h,n,"CurrentBuildNumber",cbn,sizeof cbn);
                    int b1=atoi(cb), b2=atoi(cbn);
                    if(b2>b1){ snprintf(cb,sizeof cb,"%s",cbn); }
                }
                out->current_build=atoi(cb);
            }
            hivex_close(h);
        }
    }
    /* ①b XP/2000：SOFTWARE 没有字符串版 CSDVersion 时，读 SYSTEM 的 DWORD（REQ-75）*/
    if(rc_sys==IMG_OK) csd_try_system(tf_sys,out);
    unlink(tf_sys);

    unlink(tf_sw);

    /* ② 多源证据：注册表 + 4 个核心二进制的 PE 版本（改了就开不了机）
     *    + \Windows\servicing\Version\<版本> 目录名（servicing 栈自己写的）*/
    if(out->current_build>0){
        snprintf(out->vote[out->n_vote].src,sizeof out->vote[0].src,"registry");
        out->vote[out->n_vote].build=out->current_build;
        snprintf(out->vote[out->n_vote].ver,sizeof out->vote[0].ver,"%s",out->current_version);
        out->n_vote++;
    }
    /* 逐个解析刚读出来的核心二进制（i==0 即 ntoskrnl，保留详细字段） */
    for(int i=0;i<4 && out->n_vote<6;i++){
        if(!chave[i]) continue;
        FILE *fp=fopen(cf[i],"rb");
        if(!fp){ unlink(cf[i]); continue; }
        uint8_t *b=malloc((size_t)csz[i]);
        if(b && fread(b,1,(size_t)csz[i],fp)==(size_t)csz[i]){
            img_winver_t tmpv; memset(&tmpv,0,sizeof tmpv);
            if(img_pe_version(b,(size_t)csz[i],&tmpv)==IMG_OK && tmpv.kernel_build>0){
                snprintf(out->vote[out->n_vote].src,sizeof out->vote[0].src,"%s",label[i]);
                out->vote[out->n_vote].build=tmpv.kernel_build;
                snprintf(out->vote[out->n_vote].ver,sizeof out->vote[0].ver,"%s",tmpv.kernel_file_version);
                out->n_vote++;
                if(i==0){   /* ntoskrnl：保留详细字段 */
                    snprintf(out->kernel_file_version,sizeof out->kernel_file_version,"%s",tmpv.kernel_file_version);
                    snprintf(out->kernel_product_version,sizeof out->kernel_product_version,"%s",tmpv.kernel_product_version);
                    snprintf(out->kernel_company,sizeof out->kernel_company,"%s",tmpv.kernel_company);
                    snprintf(out->kernel_description,sizeof out->kernel_description,"%s",tmpv.kernel_description);
                    out->kernel_major=tmpv.kernel_major; out->kernel_minor=tmpv.kernel_minor;
                    out->kernel_build=tmpv.kernel_build;
                    out->pe_machine=tmpv.pe_machine;      /* ★ 位数：从内核 PE 头带出来 */
                }
            }
        }
        free(b); fclose(fp); unlink(cf[i]);
    }

    /* \Windows\servicing\Version\<x.y.build.rev> 目录名（servicing 栈自己写的） */
    if(sa.got){
        int a=0,b2=0,c=0,e=0;
        if(sscanf(sa.p,"%d.%d.%d.%d",&a,&b2,&c,&e)==4 && c>0 && out->n_vote<6){
            snprintf(out->vote[out->n_vote].src,sizeof out->vote[0].src,"servicing");
            out->vote[out->n_vote].build=c;
            snprintf(out->vote[out->n_vote].ver,sizeof out->vote[0].ver,"%s",sa.p);
            out->n_vote++;
        }
    }

    /* ③ 多方法投票：按"代"（build 去掉最后一位）取众数。
     *   为什么要按代而不是按精确 build：Win10 从 2004 起用启用包升级，
     *   注册表 CurrentBuild=19045（22H2）而 winload/ntoskrnl 仍是 10.0.19041.x，
     *   这是【正常】的；精确比对会把正版盘误判成"被改过"（实测 Xlite 盘就是这样）。
     *   跨代不一致（例如 7601 vs 19041、19041 vs 22000）才是真的被换过。 */
    img_winver_vote(out);

    if(!out->product_name[0] && !out->kernel_file_version[0]){
        if(err) snprintf(err,errcap,"无法读取版本（SOFTWARE 与 ntoskrnl 都没拿到）");
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
    /* ★ REQ-37：不是所有引导文件都从偏移 0 开始就是 PE。
     *   实测 Win11 26H1 的 \Windows\Boot\PCAT\bootmgr 前置了 24 KB 的 16 位引导壳
     *   （首字节 E9 ...，没有 MZ），真正的 PE 在偏移 24456。
     *   所以：首部不是 MZ 时，在文件里扫"看起来确实是 PE"的 MZ（必须 MZ+e_lfanew 指到
     *   PE\0\0 + 合法 Machine），找到就用它当基址。扫描有上限，别在大文件上白烧 CPU。*/
    size_t base=0;
    if(!(buf[0]=='M' && buf[1]=='Z')){
        size_t limit = len < (32u<<20) ? len : (32u<<20);
        size_t found=SIZE_MAX;
        for(size_t o=0; o+0x40<limit; o++){
            if(buf[o]!='M' || buf[o+1]!='Z') continue;
            uint32_t pe2=r32(buf+o+0x3C);
            if((size_t)pe2+0x18 > limit-o) continue;
            if(memcmp(buf+o+pe2,"PE\0\0",4)!=0) continue;
            uint16_t mach=r16(buf+o+pe2+4);
            if(mach!=0x14c && mach!=0x8664 && mach!=0xaa64 && mach!=0x1c4 && mach!=0x200) continue;
            found=o; break;
        }
        if(found==SIZE_MAX) return IMG_EFORMAT;
        PEDBG("embedded PE at %zu (len=%zu)\n",found,len);
        base=found;
    }
    buf+=base; len-=base;
    out->pe_offset=(uint32_t)base;
    uint32_t pe=r32(buf+0x3C);
    if(pe+0x18>len || memcmp(buf+pe,"PE\0\0",4)!=0){ PEDBG("bad PE sig pe=%u len=%zu\n",pe,len); return IMG_EFORMAT; }
    if(out) out->pe_machine = r16(buf+pe+4);   /* ★ 位数最硬的证据：PE 头 Machine 字段 */
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

    /* 资源目录：type(16=RT_VERSION) → name(1) → lang → 数据项。
     * ★ 不能硬走三层：实测 Win11 26H1 的 \Windows\Boot\PCAT\bootmgr 里，
     *   第 1 层的 RT_VERSION 项直接就是数据项（高位没置，没有 name/lang 两层）。
     *   所以每一层都允许"子目录 or 数据项"两种。*/
    const uint8_t *res=buf+off;
    size_t rlen=len-off;
    uint32_t n_name=r16(res+12), n_id=r16(res+14);   /* +12=命名项，+14=ID 项 */
    const uint8_t *ent=res+16;
    uint32_t leaf=0;
    for(uint32_t i=0;i<n_name+n_id;i++){
        if((size_t)i*8+24>rlen) break;
        uint32_t id=r32(ent+i*8), po=r32(ent+i*8+4);
        if(id!=16) continue;
        if(!(po & 0x80000000)){ leaf=po; break; }    /* 直接是数据项 */
        const uint8_t *l2=res+(po & 0x7FFFFFFF);
        if(l2+16>res+rlen) return IMG_ENOENT;
        if(r16(l2+12)+r16(l2+14)<1) return IMG_ENOENT;
        uint32_t po2=r32(l2+16+4);
        if(po2 & 0x80000000){
            const uint8_t *l3=res+(po2 & 0x7FFFFFFF);
            if(l3+16>res+rlen) return IMG_ENOENT;
            if(r16(l3+12)+r16(l3+14)<1) return IMG_ENOENT;
            uint32_t po3=r32(l3+16+4);
            if(po3 & 0x80000000) return IMG_ENOENT;
            leaf=po3;
        }else leaf=po2;
        break;
    }
    if(!leaf || (size_t)leaf+16>rlen){ PEDBG("no RT_VERSION leaf=%u rlen=%zu\n",leaf,rlen); return IMG_ENOENT; }
    uint32_t data_rva=r32(res+leaf), data_sz=r32(res+leaf+4);
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
