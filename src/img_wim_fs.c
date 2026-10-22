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
static char *dup_tstr(const wimlib_tchar *s){
    if(!s) return NULL;
    char *o=malloc(strlen((const char*)s)+1);
    if(o) strcpy(o,(const char*)s);
    return o;
}

/* ★ wimlib 1.14 的头声明了 wimlib_get_error_string()，但发行版的 libwim.so 没导出
 *   这个符号（链接期 undefined reference）—— 与 img_wim_apply.c 同一对策：
 *   我们只需要一句人话 + 错误码，自己给一条，不依赖"声明存在但符号缺失"的 API。*/
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
    c->cb(c->ud, de->filename?(const char*)de->filename:base_name((const char*)de->full_path),
          (const char*)de->full_path, isdir, sz, de->attributes, de->depth);
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
static void st_cb(void *ud,const char *name,const char *full,bool isdir,uint64_t size,uint32_t attrs,size_t depth){
    (void)full;(void)depth;
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
    char tmpl[600]; snprintf(tmpl,sizeof tmpl,"%s/imgtool-wim-XXXXXX",tmp);
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
#endif /* IMG_WITH_WIM */
