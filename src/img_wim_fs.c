/* img_wim_fs.c — WIM/ESD 内部的【只读】访问：列目录树 + 抽单个文件
 *
 * ★ 层归属：机制在库里，政策在 App（DESIGN-OUTPUT.md §十一）。App 只下命令、
 *   不需要自己装 WIM 库，也不需要任何文件操作权限。
 *
 * 为什么这两件事必须走 wimlib，而不复用 img_identify.c 里那套自研解析：
 *   identify 只要 WIM 头 + XML 元数据，任何 reader（含 SAF fd、ISO 内偏移）都能喂；
 *   但目录树与文件数据在压缩资源表里（LZX / LZMS+solid），自己去啃解压+去重+分块
 *   校验等于重造 wimlib，而且 ESD 那条路极易静默出错。
 *   代价：wimlib **只接受真实文件路径**（没有块设备回调），所以 ISO 里的
 *   install.wim 必须先落成本地文件才能浏览/抽取 —— 这就是 resolve_source()
 *   存在的理由，也是它必须把"要复制多少字节"如实报给调用方的理由。
 *
 * 只读承诺（写死在这里，App 侧无需再防）：本模块只用
 *   wimlib_open_wim / wimlib_iterate_dir_tree / wimlib_extract_paths，
 *   没有任何写回 API；目标盘上绝不产生副作用。
 */
#if IMG_WITH_WIM
#include "img_wim_fs.h"
#include "img_lock.h"        /* img_tmpdir()：临时目录不许写死 /tmp */
#include "img_stream.h"
#include "img_event.h"
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wimlib.h>

/* ---------- 内部工具 ---------- */
static const char *base_name(const char *p){
    const char *s=strrchr(p,'/');
    return s? (s+1) : p;
}
/* 递归删临时目录（extract 用；只删我们自己 mkdtemp 出来的那一个）*/
static void rm_rf(const char *dir){
    DIR *d=opendir(dir);
    if(!d) return;
    struct dirent *de;
    while((de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        char p[1024]; snprintf(p,sizeof p,"%s/%s",dir,de->d_name);
        struct stat st;
        if(lstat(p,&st)==0 && S_ISDIR(st.st_mode)) rm_rf(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

/* ★ wimlib 1.14 的头声明了 wimlib_get_error_string()，但发行版的 libwim.so 没导出
 *   这个符号（链接期 undefined reference）—— 与 img_wim_apply.c 同一对策：
 *   我们只需要一句说明 + 错误码，自己给一条，不依赖"声明存在但符号缺失"的 API。*/
static const char *wim_rc_str(int r){
    static char b[64];
    snprintf(b,sizeof b,"wimlib 错误码 %d",r);
    return b;
}

/* ---------- 打开与索引校验 ---------- */
typedef struct { WIMStruct *w; int nimages; } openwim;
static int open_checked(const char *path,int index,openwim *o,char *err,size_t errcap){
    o->w=NULL; o->nimages=0;
    wimlib_global_init(0);
    int rc=wimlib_open_wim(path,0,&o->w);
    if(rc!=0){
        if(err) snprintf(err,errcap,"打开 WIM/ESD 失败（%s）",wim_rc_str(rc));
        o->w=NULL;
        /* 文件不在/不是 WIM → ENOENT；是 WIM 但读坏 → EFORMAT */
        return (rc==WIMLIB_ERR_OPEN)?IMG_ENOENT:IMG_EFORMAT;
    }
    struct wimlib_wim_info wi; memset(&wi,0,sizeof wi);
    wimlib_get_wim_info(o->w,&wi);
    o->nimages=(int)wi.image_count;
    if(index<1||index>o->nimages){
        if(err) snprintf(err,errcap,"映像索引 %d 超出范围（本文件共 %d 个映像）",index,o->nimages);
        wimlib_free(o->w); o->w=NULL; return IMG_ERANGE;   /* 与 wim.apply 同一返回码，App 不用两套判断 */
    }
    return IMG_OK;
}

/* ---------- 列目录 ---------- */
typedef struct { img_wim_ent_cb cb; void *ud; size_t base_depth; bool root; } lscx;
static int ls_cb(const struct wimlib_dir_entry *de,void *user_ctx){
    lscx *c=(lscx*)user_ctx;
    if(!de||!de->full_path) return 0;
    if(de->depth==0) return 0;                      /* 根自身不是一条目录项 */
    bool isdir=(de->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY)!=0;
    /* 大小 = 未命名数据流（wimlib 保证 streams[0] 就是主数据流；
     * 备用数据流从 1 开始，计 num_named_streams 个）*/
    uint64_t sz=de->streams[0].resource.uncompressed_size;
    uint64_t mt=(de->last_write_time.tv_sec>0)?(uint64_t)de->last_write_time.tv_sec:0;
    c->cb(c->ud, de->filename?(const char*)de->filename:base_name((const char*)de->full_path),
          (const char*)de->full_path, isdir, sz, mt, de->attributes, de->depth);
    return 0;
}
int img_wim_list(const char *wim_path,int index,const char *dir,bool recursive,
                 img_wim_ent_cb cb,void *ud,char *err,size_t errcap){
    if(!wim_path||!cb) return IMG_EINVAL;
    if(!dir||!dir[0]) dir="/";
    if(dir[0]!='/') return IMG_EINVAL;
    openwim o; int rc=open_checked(wim_path,index,&o,err,errcap);
    if(rc!=IMG_OK) return rc;
    lscx c={cb,ud,0,false};
    /* 非递归用 CHILDREN（wimlib 只回调直接子项，depth 语义不用我们自己兜）；
     * 递归用 RECURSIVE（整棵子树，大映像可达几十万条 —— 上限由命令层管）*/
    int flags = recursive ? WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE
                          : WIMLIB_ITERATE_DIR_TREE_FLAG_CHILDREN;
    int wrc=wimlib_iterate_dir_tree(o.w,index,(const wimlib_tchar*)dir,flags,ls_cb,&c);
    wimlib_free(o.w);
    if(wrc!=0){
        if(err){
            snprintf(err,errcap,"列 %s 失败：%s",dir,wim_rc_str(wrc));
        }
        /* 目录不存在与"读不出来"要分开：App 的文案完全不同。
         * wimlib 各版本给出的错误码不完全一致（此版本只有 PATH_DOES_NOT_EXIST 等少数几个），
         * 所以除了认码还认错误文本，避免把"没这个目录"报成"库坏了"。*/
        bool notfound=(wrc==WIMLIB_ERR_PATH_DOES_NOT_EXIST)
                   || (err && (strstr(err,"does not exist")||strstr(err,"not exist")
                               ||strstr(err,"No such file")||strstr(err,"not a directory")));
        return notfound?IMG_ENOENT:IMG_EIO;
    }
    return IMG_OK;
}

/* ---------- 查一个内部路径（是文件还是目录、多大）----------
 * wimlib 没有"stat 单条路径"的 API → 列它的父目录找名字（大小写不敏感，
 * WIM 内部路径本来就是 Windows 语义）。 */
typedef struct { char want[256]; bool hit,isdir; uint64_t size; uint32_t attrs; } stcx;
static void st_cb(void *ud,const char *name,const char *full,bool isdir,uint64_t size,uint64_t mt,uint32_t attrs,size_t depth){
    (void)full;(void)depth;(void)mt;
    stcx *s=(stcx*)ud;
    if(s->hit) return;
    if(name && !strcasecmp(name,s->want)){ s->hit=true; s->isdir=isdir; s->size=size; s->attrs=attrs; }
}
int img_wim_stat(const char *wim_path,int index,const char *inner_path,img_wim_stat_t *out,
                 char *err,size_t errcap){
    if(!wim_path||!inner_path||inner_path[0]!='/') return IMG_EINVAL;
    char parent[512]; const char *slash=strrchr(inner_path,'/');
    if(!slash||!slash[1]) return IMG_EINVAL;
    size_t plen=(size_t)(slash-inner_path);
    if(plen==0) snprintf(parent,sizeof parent,"/");
    else{ if(plen>=sizeof parent) return IMG_EINVAL; memcpy(parent,inner_path,plen); parent[plen]=0; }
    stcx s; memset(&s,0,sizeof s);
    snprintf(s.want,sizeof s.want,"%s",slash+1);
    int rc=img_wim_list(wim_path,index,parent,false,st_cb,&s,err,errcap);
    if(rc!=IMG_OK) return rc;
    if(!s.hit){ if(err) snprintf(err,errcap,"WIM 里没有这个路径：%s",inner_path); return IMG_ENOENT; }
    if(out){ out->exists=true; out->is_dir=s.isdir; out->size_bytes=s.size; out->attributes=s.attrs; }
    return IMG_OK;
}

/* ---------- 抽【单个文件】到本地（只读操作）---------- */
int img_wim_extract_tree(const char *wim_path,int index,const char *inner_path,
                         const char *out_dir,img_prog *prog,char *err,size_t errcap){
    if(!wim_path||!inner_path||!out_dir) return IMG_EINVAL;
    if(inner_path[0]!='/') return IMG_EINVAL;
    if(prog){ img_prog_phase(prog,IMG_PH_EXTRACT,"从 WIM/ESD 解出"); img_prog_op(prog,IMG_OP_READ); }
    openwim o; int rc=open_checked(wim_path,index,&o,err,errcap);
    if(rc!=IMG_OK) return rc;
    const wimlib_tchar *paths[1]={ (const wimlib_tchar*)inner_path };
    int wrc=wimlib_extract_paths(o.w,index,(const wimlib_tchar*)out_dir,paths,1,
                                 WIMLIB_EXTRACT_FLAG_NO_ACLS);
    wimlib_free(o.w);
    if(wrc!=0){ if(err) snprintf(err,errcap,"解出失败：%s",wim_rc_str(wrc)); return IMG_EIO; }
    return IMG_OK;
}

/* ---------- 递归复制 / 统计（给"抽目录"用）---------- */
#include <dirent.h>
static int cp_r(const char *src, const char *dst){
    struct stat sb;
    if(lstat(src,&sb)!=0) return -1;
    if(S_ISDIR(sb.st_mode)){
        if(mkdir(dst,0755)!=0 && errno!=EEXIST) return -1;
        DIR *d=opendir(src); if(!d) return -1;
        struct dirent *e; int rc=0;
        while((e=readdir(d))!=NULL){
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            char a[1024],b[1024];
            snprintf(a,sizeof a,"%s/%s",src,e->d_name);
            snprintf(b,sizeof b,"%s/%s",dst,e->d_name);
            if(cp_r(a,b)!=0){ rc=-1; break; }
        }
        closedir(d);
        return rc;
    }
    FILE *in=fopen(src,"rb"); if(!in) return -1;
    FILE *out=fopen(dst,"wb"); if(!out){ fclose(in); return -1; }
    char *buf=(char*)malloc(1<<20);
    int rc=0;
    if(!buf) rc=-1;
    else{
        size_t n;
        while((n=fread(buf,1,1<<20,in))>0) if(fwrite(buf,1,n,out)!=n){ rc=-1; break; }
        free(buf);
    }
    fclose(in);
    if(fclose(out)!=0) rc=-1;
    return rc;
}
static void tree_stats(const char *path, uint64_t *bytes, int *files, int *dirs){
    struct stat sb;
    if(lstat(path,&sb)!=0) return;
    if(S_ISDIR(sb.st_mode)){
        if(dirs) (*dirs)++;
        DIR *d=opendir(path); if(!d) return;
        struct dirent *e;
        while((e=readdir(d))!=NULL){
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            char a[1024]; snprintf(a,sizeof a,"%s/%s",path,e->d_name);
            tree_stats(a,bytes,files,dirs);
        }
        closedir(d);
        return;
    }
    if(bytes) *bytes += (uint64_t)sb.st_size;
    if(files) (*files)++;
}

/* 抽一个【目录子树】到本地（REQ-28 C″）。
 *   dst_dir 必须不存在（或空目录）：目录合并的语义太容易出人命，宁可让调用方明确。*/
int img_wim_extract_dir(const char *wim_path,int index,const char *inner_path,
                        const char *dst_dir,img_prog *prog,uint64_t *out_bytes,
                        int *out_files,char *err,size_t errcap){
    (void)prog;
    if(out_bytes) *out_bytes=0;
    if(out_files) *out_files=0;
    if(!wim_path||!inner_path||!dst_dir) return IMG_EINVAL;
    if(inner_path[0]!='/') return IMG_EINVAL;
    img_wim_stat_t st; memset(&st,0,sizeof st);
    int rc=img_wim_stat(wim_path,index,inner_path,&st,err,errcap);
    if(rc!=IMG_OK) return rc;
    if(!st.is_dir){ if(err) snprintf(err,errcap,"不是目录：%s",inner_path); return IMG_EINVAL; }
    struct stat ds;
    if(stat(dst_dir,&ds)==0){
        if(!S_ISDIR(ds.st_mode)){ if(err) snprintf(err,errcap,"目标已存在且不是目录：%s",dst_dir); return IMG_ESTATE; }
        DIR *d=opendir(dst_dir); int n=0;
        if(d){ struct dirent *e; while((e=readdir(d))){ if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")) n++; } closedir(d); }
        if(n>0){ if(err) snprintf(err,errcap,"目标目录非空（本命令不做合并）：%s",dst_dir); return IMG_ESTATE; }
    }
    const char *tmp=img_tmpdir();
    if(!tmp||!tmp[0]){ if(err) snprintf(err,errcap,"找不到可写临时目录（设 IMGTOOL_TMPDIR 或 TMPDIR）"); return IMG_ESTATE; }
    char tmpl[600];
    if(img_tmpfile_dir("wimdir",tmpl,sizeof tmpl)!=IMG_OK) return IMG_EIO;
    char *dir=mkdtemp(tmpl);
    if(!dir){ if(err) snprintf(err,errcap,"建临时目录失败：%s",strerror(errno)); return IMG_EIO; }

    openwim o; rc=open_checked(wim_path,index,&o,err,errcap);
    if(rc!=IMG_OK){ rmdir(dir); return rc; }
    const wimlib_tchar *paths[1]={ (const wimlib_tchar*)inner_path };
    int wrc=wimlib_extract_paths(o.w,index,(const wimlib_tchar*)dir,paths,1,
                                 WIMLIB_EXTRACT_FLAG_NO_ACLS);
    wimlib_free(o.w);
    if(wrc!=0){
        if(err) snprintf(err,errcap,"抽取失败：%s",wim_rc_str(wrc));
        rm_rf(dir); rmdir(dir); return IMG_EIO;
    }
    char got[1100]; snprintf(got,sizeof got,"%s%s",dir,inner_path);
    struct stat sb;
    if(stat(got,&sb)!=0 || !S_ISDIR(sb.st_mode)){
        if(err) snprintf(err,errcap,"抽完了但找不到落地目录：%s",got);
        rm_rf(dir); rmdir(dir); return IMG_EIO;
    }
    /* 先试 rename（同盘几毫秒）；跨盘（/data 临时 → /sdcard 目标）退化成递归复制 */
    int moved=0;
    if(rename(got,dst_dir)==0) moved=1;
    else{
        if(rmdir(dst_dir)==0 || errno==ENOENT){ /* 空目录先让开 */ }
        if(cp_r(got,dst_dir)==0) moved=1;
    }
    if(moved){ uint64_t b=0; int f=0; tree_stats(dst_dir,&b,&f,NULL); if(out_bytes)*out_bytes=b; if(out_files)*out_files=f; }
    rm_rf(dir); rmdir(dir);
    if(!moved){ if(err) snprintf(err,errcap,"移动到目标失败：%s",strerror(errno)); return IMG_EIO; }
    return IMG_OK;
}

int img_wim_extract_file(const char *wim_path,int index,const char *inner_path,
                         const char *dst_path,img_prog *prog,uint64_t *out_bytes,
                         char *err,size_t errcap){
    if(!wim_path||!inner_path||!dst_path) return IMG_EINVAL;
    if(inner_path[0]!='/'||!inner_path[1]) return IMG_EINVAL;
    img_wim_stat_t st; memset(&st,0,sizeof st);
    int rc=img_wim_stat(wim_path,index,inner_path,&st,err,errcap);
    if(rc!=IMG_OK) return rc;
    if(st.is_dir){ if(err) snprintf(err,errcap,"这是个目录：%s（本命令只抽单个文件）",inner_path); return IMG_ENOTSUP; }

    const char *tmp=img_tmpdir();
    if(!tmp||!tmp[0]){ if(err) snprintf(err,errcap,"找不到可写临时目录（设 IMGTOOL_TMPDIR 或 TMPDIR）"); return IMG_ESTATE; }
    char tmpl[600];
    if(img_tmpfile("wim",tmpl,sizeof tmpl)==NULL) return IMG_EIO;
    char *dir=mkdtemp(tmpl);
    if(!dir){ if(err) snprintf(err,errcap,"建临时目录失败：%s",strerror(errno)); return IMG_EIO; }

    openwim o; rc=open_checked(wim_path,index,&o,err,errcap);
    if(rc!=IMG_OK){ rm_rf(dir); rmdir(dir); return rc; }
    const wimlib_tchar *paths[1]={ (const wimlib_tchar*)inner_path };
    /* NO_ACLS：Bionic/Linux 上没有 Windows ACL 语义，要 ACL 会让 wimlib 做无用功；
     * 这里是"抽一个文件给人看"，不是"装系统"（装系统那条走 wim.apply，另一套标志）。*/
    int wrc=wimlib_extract_paths(o.w,index,(const wimlib_tchar*)dir,paths,1,
                                 WIMLIB_EXTRACT_FLAG_NO_ACLS);
    wimlib_free(o.w);
    if(wrc!=0){
        if(err) snprintf(err,errcap,"抽取失败：%s",wim_rc_str(wrc));
        rm_rf(dir); rmdir(dir); return IMG_EIO;
    }
    /* wimlib 保留内部目录结构 → 真身在 dir + inner_path */
    char got[1100]; snprintf(got,sizeof got,"%s%s",dir,inner_path);
    struct stat sb;
    if(stat(got,&sb)!=0){
        if(err) snprintf(err,errcap,"抽完了但找不到落地文件（wimlib 结构假设不成立）：%s",got);
        rm_rf(dir); rmdir(dir); return IMG_EIO;
    }
    /* 复制到目标（1 MiB 流式：GB 级也不会把内存吃穿；带进度与取消）*/
    FILE *in=fopen(got,"rb"), *out=fopen(dst_path,"wb");
    if(!in||!out){
        if(err) snprintf(err,errcap,"打开临时/目标文件失败：%s",strerror(errno));
        if(in) fclose(in);
        if(out) unlink(dst_path);
        rm_rf(dir); rmdir(dir); return IMG_EIO;
    }
    static char buf[1048576];
    size_t n; uint64_t done=0; bool cancelled=false;
    while((n=fread(buf,1,sizeof buf,in))>0){
        if(fwrite(buf,1,n,out)!=n){ break; }
        done+=n;
        if(prog){ img_prog_bytes(prog,done,(uint64_t)sb.st_size); }
        if(img_prog_cancelled(prog)){ cancelled=true; break; }
    }
    fclose(in); fclose(out);
    rm_rf(dir); rmdir(dir);
    if(cancelled){
        unlink(dst_path);                      /* ★ 取消不留半成品（与 iso.extract 同契约）*/
        if(err) snprintf(err,errcap,"已取消（目标文件已删除）");
        if(prog) img_prog_bytes(prog,0,(uint64_t)sb.st_size);
        return IMG_ECANCELLED;
    }
    if(done!=(uint64_t)sb.st_size){
        if(err) snprintf(err,errcap,"写出 %llu ≠ 声明 %lld 字节",
                         (unsigned long long)done,(long long)sb.st_size);
        return IMG_ETRUNC;
    }
    if(out_bytes) *out_bytes=done;
    return IMG_OK;
}

/* ---------- 内容判位数（REQ-19 §B / REQ-22）----------
 * XML 的 <ARCH> 不可信（微软 x64 盘里也写 0；NTLite 那边同样把这种盘报成
 * "unknown architecture"）。用盘里的东西判，谁也改不了：
 *   ① \Windows\SysWOW64 目录 或 \Windows\System32\wow64.dll → 64 位家族
 *      （SysWOW64 只在 64 位 Windows 存在）；
 *   ② 读一个小文件的 PE Machine 定死具体位数 —— 顺序按【从小到大】：
 *      smss.exe(~130 KB) → wininit.exe → ntdll.dll → ntoskrnl.exe(7 MB)，
 *      0x014c=x86 0x8664=x64 0xAA64=arm64 0x01c4=arm；
 *   ③ 都判不出 → out 置空串（调用方再试介质级信号 / XML），绝不猜。
 * ★ PE 头的位置不能按"读前 512 字节"找：真 ntoskrnl 的 e_lfanew=0x280，
 *   512 字节窗口看不到 PE 签名 → 以前所有 32 位映像都悄悄退回 XML（REQ-22 的根因）。
 *   现在先读 4 KB，越界就按 e_lfanew 补读。
 * ★ 写在 reason 里的是"凭什么"，直接透给宿主（arch_source）。*/
static int pe_machine_of(const char *path, uint16_t *mach, char *why, size_t whycap){
    unsigned char hdr[4096];
    FILE *f=fopen(path,"rb");
    if(!f){ if(why) snprintf(why,whycap,"打不开临时文件"); return IMG_ENOENT; }
    size_t got=fread(hdr,1,sizeof hdr,f);
    if(got<0x40 || hdr[0]!='M' || hdr[1]!='Z'){
        fclose(f); if(why) snprintf(why,whycap,"不是 MZ（%zu 字节）",got); return IMG_EFORMAT;
    }
    uint32_t lfanew=(uint32_t)hdr[0x3c]|((uint32_t)hdr[0x3d]<<8)
                   |((uint32_t)hdr[0x3e]<<16)|((uint32_t)hdr[0x3f]<<24);
    unsigned char pe[8];
    if((size_t)lfanew+8 <= got) memcpy(pe,hdr+lfanew,8);
    else {
        if(fseeko(f,(off_t)lfanew,SEEK_SET)!=0 || fread(pe,1,8,f)!=8){
            fclose(f); if(why) snprintf(why,whycap,"PE 头在 0x%x，读不到",lfanew); return IMG_EFORMAT;
        }
    }
    fclose(f);
    if(memcmp(pe,"PE\0\0",4)!=0){ if(why) snprintf(why,whycap,"PE 签名不对（0x%x 处）",lfanew); return IMG_EFORMAT; }
    *mach=(uint16_t)((unsigned)pe[4]|((unsigned)pe[5]<<8));
    return IMG_OK;
}
static const char *arch_of_machine(uint16_t m){
    switch(m){
        case 0x014c: return "x86";
        case 0x8664: return "x64";
        case 0xAA64: return "arm64";
        case 0x01c4: return "arm";
        default:     return NULL;
    }
}
static const char *base_of(const char *p){
    const char *s=strrchr(p,'/');
    return s?s+1:p;
}

int img_wim_probe_arch(const char *wim_path, int index, char *out, size_t cap,
                       char *src, size_t srccap, char *why, size_t whycap){
    if(out && cap) out[0]=0;
    if(src && srccap) src[0]=0;
    if(why && whycap) why[0]=0;

    /* ① 64 位家族（最省事，两次 stat，不抽任何文件）*/
    img_wim_stat_t st; char e[256]="";
    bool fam64=false;
    if(img_wim_stat(wim_path,index,"/Windows/SysWOW64",&st,e,sizeof e)==IMG_OK && st.is_dir){
        fam64=true; if(src) snprintf(src,srccap,"syswow64");
    } else if(img_wim_stat(wim_path,index,"/Windows/System32/wow64.dll",&st,e,sizeof e)==IMG_OK && !st.is_dir){
        fam64=true; if(src) snprintf(src,srccap,"wow64");
    }

    /* ② 小文件优先定死具体位数 */
    static const char *cand[]={
        "/Windows/System32/smss.exe",      /* ~130 KB，最省 */
        "/Windows/System32/wininit.exe",   /* ~120 KB */
        "/Windows/System32/ntdll.dll",     /* ~1.9 MB */
        "/Windows/System32/ntoskrnl.exe",  /* ~7 MB，最后 */
    };
    const char *td=img_tmpdir();
    if(!td){ if(why) snprintf(why,whycap,"没有可写临时目录"); return IMG_ENOTSUP; }
    int last_rc=IMG_ENOENT;
    for(size_t i=0;i<sizeof cand/sizeof cand[0];i++){
        if(img_wim_stat(wim_path,index,cand[i],&st,e,sizeof e)!=IMG_OK || st.is_dir) continue;
        char tmp[528];
        snprintf(tmp,sizeof tmp,"%s/imgarchXXXXXX",td);
        int fd=mkstemp(tmp);
        if(fd<0){ if(why) snprintf(why,whycap,"建不了临时文件"); return IMG_EIO; }
        close(fd);
        uint64_t nb=0;
        char e2[256]="";
        int rc=img_wim_extract_file(wim_path,index,cand[i],tmp,NULL,&nb,e2,sizeof e2);
        if(rc!=IMG_OK){ unlink(tmp); last_rc=rc; continue; }
        uint16_t m=0; char w2[200]="";
        int prc=pe_machine_of(tmp,&m,w2,sizeof w2);
        unlink(tmp);
        if(prc!=IMG_OK){ last_rc=prc; continue; }
        const char *a=arch_of_machine(m);
        if(!a){ last_rc=IMG_EFORMAT; if(why) snprintf(why,whycap,"PE Machine=0x%04x 不认识",m); continue; }
        if(out) snprintf(out,cap,"%s",a);
        if(src){ snprintf(src,srccap,"pe:%s",base_of(cand[i])); }
        if(why) snprintf(why,whycap,"%s 的 PE Machine=0x%04x",base_of(cand[i]),m);
        return IMG_OK;
    }
    /* ③ 只判出"64 位家族"（有 SysWOW64 / wow64.dll），但没有可读的系统文件定死 x64/arm64：
     *   按 x64 处理（arm64 的 Windows 极罕见，而且它也有 SysWOW64 —— 只是没法再细分）。
     *   arch_source 会写成 syswow64/wow64，宿主想严格就自己判。*/
    if(fam64){
        if(out) snprintf(out,cap,"x64");
        if(why) snprintf(why,whycap,"%s：64 位家族（未能细分 x64/arm64，按 x64）",
                         (src&&src[0])?src:"SysWOW64/wow64.dll");
        return IMG_OK;
    }
    if(why) snprintf(why,whycap,"内容判不出（%s）",e[0]?e:"没有可用的系统文件");
    return last_rc;
}

/* ---------- 这个 WIM 本身是不是一份 WinRE 映像（REQ-24/25）----------
 * 判据：映像根下【没有 \Windows 目录】。Windows 安装映像的根一定有 Windows\；
 *   WinRE 映像的根是 \WindowsRE、\$WinREAgent 之类。
 * 只看根的一层（不递归），几毫秒。*/
typedef struct { bool has_windows; } ws_ctx;
static int ws_cb(const struct wimlib_dir_entry *d, void *ud){
    ws_ctx *c=(ws_ctx*)ud;
    if(!d || !d->full_path) return 0;
    /* 只认【根下的第一层】叫 Windows 的那一项（/Windows、\Windows）：
     *   \Windows\System32 这种更深的不算（它们是 WinRE 里没有的，但要防构造映像）*/
    const char *p=d->full_path;
    while(*p=='\\'||*p=='/') p++;
    if(!strncasecmp(p,"Windows",7)){
        const char *q=p+7;
        if(*q==0 || *q=='\\' || *q=='/') c->has_windows=true;
    }
    return 0;
}
int img_wim_is_winre_self(const char *wim_path, bool *out_self, char *why, size_t whycap){
    if(out_self) *out_self=false;
    if(why && whycap) why[0]=0;
    if(!wim_path) return IMG_EINVAL;
    WIMStruct *w=NULL;
    if(wimlib_open_wim(wim_path,0,&w)!=0){ if(why&&whycap) snprintf(why,whycap,"WIM 打不开"); return IMG_ENOENT; }
    ws_ctx c={false};
    /* ★ 必须带 RECURSIVE：flags=0 时 wimlib 只回调根节点本身，看不到子项 ——
     *   那样任何映像都会被判成"根下没有 Windows"= self（踩过：WinPE 的 boot.wim 被当 payload）。*/
    wimlib_iterate_dir_tree(w,1,"/",WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE,ws_cb,&c);
    wimlib_free(w);
    if(out_self) *out_self=!c.has_windows;
    if(why && whycap) snprintf(why,whycap,c.has_windows?"根下有 \\Windows（是安装映像）":"根下没有 \\Windows（是 WinRE 映像）");
    return IMG_OK;
}
/* ================= 流式取文件（REQ-29）=================
 * wimlib 的 WIMLIB_EXTRACT_FLAG_TO_STDOUT：解码（含 LZMS/solid）由它做，字节从 fd 1 出来。
 * 于是"从 WIM/ESD/ISO 里取一个文件"不必先落临时目录。
 *
 * ★★ 三条实测出来的硬约束（别改回去，会死锁/报错）★★
 *   1. TO_STDOUT 必须配一个【存在的目录】当 target：传 NULL → WIMLIB_ERR_INVALID_PARAM(24)；
 *   2. wimlib 只能在【调用线程】里跑：放到 pthread 里再让别的线程读 pipe，双方一起卡死
 *      （单线程 + 外部 `| cat` 正常，线程 + 普通文件也正常 —— 就是"线程 + pipe"死锁）；
 *   3. 于是消费者（读 pipe 的那一方）放到辅助线程：wimlib 在主调线程写，辅助线程读。
 *
 * ★ fd 1 是进程全局的：改道期间把日志/进度切到 dup 出来的真 stdout，
 *   否则那几行输出会混进字节流，解出来的文件就是坏的。*/
typedef struct {
    const char *wim, *inner;
    int         index;
    img_wim_consume_fn consume;
    void       *ud;
    uint64_t    size;
    int         rc;
    char       *err;
    size_t      errcap;
} wcons_t;

static enum wimlib_progress_status wcons_prog(enum wimlib_progress_msg msg,
                                             union wimlib_progress_info *info, void *ctx){
    img_prog *pg=(img_prog*)ctx;
    if(msg==WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS && pg && info)
        img_prog_bytes(pg, info->extract.completed_bytes, info->extract.total_bytes);
    return img_cancelled()?WIMLIB_PROGRESS_STATUS_ABORT:WIMLIB_PROGRESS_STATUS_CONTINUE;
}

static void *wcons_thread(void *ud){
    wcons_t *c=(wcons_t*)ud;
    int fd=c->index;                      /* 复用字段：管道读端 */
    c->rc=c->consume(c->ud,fd,c->size,
                     (c->err && c->errcap && !c->err[0])?c->err:NULL,
                     (c->err && c->errcap && !c->err[0])?c->errcap:0);
    if(fd>=0) close(fd);
    return NULL;
}

int img_wim_stream_consume(const char *wim_path,int index,const char *inner_path,
                           img_wim_consume_fn consume, void *ud,
                           uint64_t *out_size, img_prog *prog,
                           char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(out_size) *out_size=0;
    if(!wim_path||!inner_path||!consume) return IMG_EINVAL;
    if(inner_path[0]!='/'){ if(err&&errcap) snprintf(err,errcap,"WIM 内路径必须以 / 开头"); return IMG_EINVAL; }
    img_wim_stat_t st; memset(&st,0,sizeof st);
    int rc=img_wim_stat(wim_path,index,inner_path,&st,err,errcap);
    if(rc!=IMG_OK) return rc;
    if(st.is_dir){ if(err&&errcap) snprintf(err,errcap,"这是目录：%s（流式只支持单个文件）",inner_path); return IMG_ENOTSUP; }

    openwim o; rc=open_checked(wim_path,index,&o,err,errcap);
    if(rc!=IMG_OK) return rc;
    wimlib_register_progress_function(o.w,wcons_prog,prog);

    int fds[2];
    if(pipe(fds)!=0){ wimlib_free(o.w); if(err&&errcap) snprintf(err,errcap,"建管道失败：%s",strerror(errno)); return IMG_EIO; }

#ifdef F_SETPIPE_SZ
    /* 管道默认 64 KB：1 MB 一块地来回倒，系统调用太多（真机 338 MB 差 ~2 倍）。
     * 放大到 1 MB 只影响吞吐，不改变语义。*/
    if(fcntl(fds[1],F_SETPIPE_SZ,1<<20)<0){ /* 不许改就算了 */ }
#endif
    wcons_t c; memset(&c,0,sizeof c);
    c.wim=wim_path; c.inner=inner_path; c.index=fds[0];   /* 复用字段：读端 fd */
    c.consume=consume; c.ud=ud; c.size=st.size_bytes;
    c.err=err; c.errcap=errcap; c.rc=IMG_OK;
    pthread_t th; bool th_on=false;
    if(pthread_create(&th,NULL,wcons_thread,&c)!=0){
        close(fds[0]); close(fds[1]); wimlib_free(o.w);
        if(err&&errcap) snprintf(err,errcap,"建消费线程失败");
        return IMG_ESTATE;
    }
    th_on=true;

    /* 消费侧提前退出（写盘失败/取消）时，wimlib 继续写管道会 EPIPE ——
     * 不忽略 SIGPIPE 的话整个进程直接被杀（App 里就是"闪退"）。*/
    struct sigaction sa_new, sa_old;
    memset(&sa_new,0,sizeof sa_new);
    sa_new.sa_handler=SIG_IGN;
    sigaction(SIGPIPE,&sa_new,&sa_old);

    /* 本线程跑 wimlib（约束 ②）；fd 1 临时改成管道写端 */
    int saved=dup(1);
    int logfd=img_log_out_fd();
    if(saved>=0 && logfd==1) img_log_set_out_fd(saved);
    if(dup2(fds[1],1)<0){
        rc=IMG_EIO;
        if(err&&errcap&&!err[0]) snprintf(err,errcap,"管道重定向失败：%s",strerror(errno));
    } else {
        close(fds[1]);
        const char *tdir=img_tmpdir();
        const wimlib_tchar *paths[1]={ (const wimlib_tchar*)inner_path };
        int wrc=wimlib_extract_paths(o.w,index,
                    (const wimlib_tchar*)(tdir&&tdir[0]?tdir:"."),paths,1,
                    WIMLIB_EXTRACT_FLAG_TO_STDOUT|WIMLIB_EXTRACT_FLAG_NO_ACLS);
        if(wrc!=0){
            rc=IMG_EIO;
            if(err&&errcap&&!err[0]) snprintf(err,errcap,"抽取失败：%s",wim_rc_str(wrc));
        }
        dup2(saved,1);                    /* 还原（顺带关掉写端 → 消费线程看到 EOF）*/
    }
    if(saved>=0) close(saved);
    if(logfd==1) img_log_set_out_fd(1);
    wimlib_free(o.w);
    if(th_on) pthread_join(th,NULL);
    sigaction(SIGPIPE,&sa_old,NULL);
    if(rc==IMG_OK && c.rc!=IMG_OK) rc=c.rc;   /* 消费侧的错误优先报（更贴合语义）*/
    if(rc==IMG_OK && out_size) *out_size=st.size_bytes;
    return rc;
}

#endif /* IMG_WITH_WIM */

#if !IMG_WITH_WIM
int img_wim_stream_consume(const char *wim_path,int index,const char *inner_path,
                           img_wim_consume_fn consume, void *ud, uint64_t *out_size,
                           img_prog *prog, char *err, size_t errcap){
    (void)wim_path;(void)index;(void)inner_path;(void)consume;(void)ud;(void)prog;
    if(out_size) *out_size=0;
    if(err&&errcap) snprintf(err,errcap,"本构建未启用 WIM/ESD（IMG_WITH_WIM=OFF）");
    return IMG_ENOTSUP;
}
#endif
