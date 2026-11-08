/* img_archive_w.c — 压缩包写端（见 img_archive_w.h 的设计说明）
 *
 * 实测约束（别改回去）：
 *   1. libarchive 的 iso9660 写端能顺序写 >4 GiB（iso-level=3），不需要 seek；
 *      但**目录树必须边加边写**，不能"先加完所有 header 再补数据"。
 *   2. 7z 写端要 liblzma（compression=lzma2）；没编进来就退回 copy（不压缩）。
 *   3. zip 的 zip64 必须显式开：>4 GiB 单文件 / >65535 项时不开直接写坏。
 *   4. archive_write_open_fd 不接管 fd —— 我们自己 close（否则 rename 前刷不下去）。
 */
#include "img_archive_w.h"
#include "img_event.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if !defined(IMG_WITH_ARCHIVE)
#define IMG_WITH_ARCHIVE 0
#endif

#if IMG_WITH_ARCHIVE

#define AW_CHUNK (1u<<20)

/* ---------- 名字表（判断"包里有没有这条"/子项数） ---------- */
typedef struct {
    char  **keys;
    size_t  cap, n;
} aw_set;

static uint64_t aw_hash(const char *s){
    uint64_t h=1469598103934665603ULL;
    for(const unsigned char *p=(const unsigned char*)s; *p; p++){ h^=*p; h*=1099511628211ULL; }
    return h;
}
static int aw_set_grow(aw_set *s){
    size_t nc=s->cap? s->cap*2 : 256;
    char **nk=(char**)calloc(nc,sizeof(char*));
    if(!nk) return IMG_ENOMEM;
    for(size_t i=0;i<s->cap;i++){
        if(!s->keys[i]) continue;
        size_t j=(size_t)(aw_hash(s->keys[i]) & (nc-1));
        while(nk[j]) j=(j+1)&(nc-1);
        nk[j]=s->keys[i];
    }
    free(s->keys);
    s->keys=nk; s->cap=nc;
    return IMG_OK;
}
static int aw_set_add(aw_set *s, const char *k){
    if(!k || !*k) return IMG_OK;
    if(!s->cap || (s->n+1)*10 >= s->cap*7){
        int rc=aw_set_grow(s);
        if(rc!=IMG_OK) return rc;
    }
    size_t j=(size_t)(aw_hash(k) & (s->cap-1));
    while(s->keys[j]){
        if(!strcmp(s->keys[j],k)) return IMG_OK;
        j=(j+1)&(s->cap-1);
    }
    s->keys[j]=strdup(k);
    if(!s->keys[j]) return IMG_ENOMEM;
    s->n++;
    return IMG_OK;
}
static bool aw_set_has(const aw_set *s, const char *k){
    if(!s || !s->cap || !k || !*k) return false;
    size_t j=(size_t)(aw_hash(k) & (s->cap-1));
    while(s->keys[j]){
        if(!strcmp(s->keys[j],k)) return true;
        j=(j+1)&(s->cap-1);
    }
    return false;
}
static void aw_set_free(aw_set *s){
    if(!s) return;
    for(size_t i=0;i<s->cap;i++) free(s->keys[i]);
    free(s->keys);
    s->keys=NULL; s->cap=s->n=0;
}

/* ---------- 路径规范化 ---------- */
/* 包内路径：/ 开头、去掉重复 /、去尾 /；根写作 "/" */
static void aw_norm(const char *in, char *out, size_t cap){
    if(!in || !*in){ snprintf(out,cap,"/"); return; }
    char tmp[2048]; size_t o=0;
    if(in[0]!='/'){ if(o+1<sizeof tmp) tmp[o++]='/'; }
    for(const char *p=in; *p && o+2<sizeof tmp; p++){
        if(*p=='\\'){ if(o && tmp[o-1]=='/') continue; tmp[o++]='/'; continue; }
        if(*p=='/' && o && tmp[o-1]=='/') continue;
        tmp[o++]=*p;
    }
    tmp[o]=0;
    while(o>1 && tmp[o-1]=='/') tmp[--o]=0;
    if(!o){ snprintf(out,cap,"/"); return; }
    snprintf(out,cap,"%s",tmp);
}
/* 写进 archive 的 pathname：不带前导 /（绝对路径会被 libarchive 拒/写成怪东西）*/
static void aw_write_name(const char *inner, char *out, size_t cap, bool isdir){
    aw_norm(inner,out,cap);
    const char *s=out;
    while(*s=='/') s++;
    if(s!=out) memmove(out,s,strlen(s)+1);
    if(!*out){ snprintf(out,cap,"."); return; }
    if(isdir){
        size_t n=strlen(out);
        if(n+1<cap && out[n-1]!='/'){ out[n]='/'; out[n+1]=0; }
    }
}
static bool aw_under(const char *path, const char *prefix){
    size_t n=strlen(prefix);
    if(n>1 && prefix[n-1]=='/') n--;
    if(strncmp(path,prefix,n)!=0) return false;
    return path[n]==0 || path[n]=='/';
}

/* ---------- 句柄 ---------- */
struct img_aw {
    struct archive *a;
    int    fd;
    char   dst[2048];
    char   tmp[2048];
    img_aw_fmt fmt;
    bool   committed;
    aw_set set;
    uint64_t entries;
    uint64_t bytes_out;
};

const char *img_aw_fmt_name(img_aw_fmt f){
    switch(f){
    case IMG_AW_ZIP: return "zip";
    case IMG_AW_7Z:  return "7z";
    case IMG_AW_TAR: return "tar";
    case IMG_AW_TGZ: return "tgz";
    case IMG_AW_ISO: return "iso";
    default: return "?";
    }
}
static void aw_set_err(char *err, size_t cap, const char *fmt, const char *a, const char *b){
    if(err&&cap) snprintf(err,cap,fmt,a?a:"",b?b:"");
}

static bool ends_with_ci(const char *s, const char *suf){
    size_t n=strlen(s), m=strlen(suf);
    if(m>n) return false;
    return strcasecmp(s+n-m,suf)==0;
}
img_aw_fmt img_aw_fmt_by_name(const char *path){
    if(!path) return IMG_AW_NONE;
    if(ends_with_ci(path,".zip") || ends_with_ci(path,".apk") || ends_with_ci(path,".jar")) return IMG_AW_ZIP;
    if(ends_with_ci(path,".7z"))  return IMG_AW_7Z;
    if(ends_with_ci(path,".tgz") || ends_with_ci(path,".tar.gz")) return IMG_AW_TGZ;
    if(ends_with_ci(path,".tar")) return IMG_AW_TAR;
    if(ends_with_ci(path,".iso")) return IMG_AW_ISO;
    return IMG_AW_NONE;
}
bool img_aw_writable(img_aw_fmt f){
    return f==IMG_AW_ZIP || f==IMG_AW_7Z || f==IMG_AW_TAR || f==IMG_AW_TGZ || f==IMG_AW_ISO;
}

/* 魔数：只认能写的四种 + 明确告诉调用方 rar/cab 是只读 */
int img_aw_fmt_of_existing(const char *path, img_aw_fmt *out, char *err, size_t errcap){
    if(out) *out=IMG_AW_NONE;
    if(!path||!out) return IMG_EINVAL;
    int fd=open(path,O_RDONLY);
    if(fd<0){
        if(errno==ENOENT) return IMG_ENOENT;      /* 不存在 = 新建，不是错误 */
        if(err&&errcap) snprintf(err,errcap,"打不开压缩包：%s",path);
        return IMG_ENOENT;
    }
    unsigned char b[8]={0};
    ssize_t n=read(fd,b,sizeof b);
    /* tar 的魔数在第 257 字节 */
    unsigned char t[8]={0};
    if(n==8) (void)pread(fd,t,sizeof t,257);
    close(fd);
    if(n>=4 && b[0]=='P' && b[1]=='K'){ *out=IMG_AW_ZIP; return IMG_OK; }
    if(n>=6 && !memcmp(b,"7z\xBC\xAF\x27\x1C",6)){ *out=IMG_AW_7Z; return IMG_OK; }
    if(n==8 && !memcmp(b,"MSWIM\0\0\0",8)){
        if(err&&errcap) snprintf(err,errcap,"这是 WIM，不是压缩包");
        return IMG_EFORMAT;
    }
    if(n>=6 && (!memcmp(t,"ustar",5) || !memcmp(t,"USTAR",5))){ *out=IMG_AW_TAR; return IMG_OK; }
    if(n>=2 && b[0]==0x1f && b[1]==0x8b){
        /* .tar.gz 与单独 gzip 分不出来：按扩展名兜底，否则当 tgz */
        img_aw_fmt f=img_aw_fmt_by_name(path);
        *out=(f==IMG_AW_TGZ||f==IMG_AW_TAR)?f:IMG_AW_TGZ;
        return IMG_OK;
    }
    if(n>=6 && !memcmp(b,"Rar!\x1a\x07",6)){
        if(err&&errcap) snprintf(err,errcap,"RAR 只能读，不能创建/追加（请用 7z）");
        return IMG_ENOTSUP;
    }
    if(n>=4 && !memcmp(b,"MSCF",4)){
        if(err&&errcap) snprintf(err,errcap,"CAB 只能读，不能写");
        return IMG_ENOTSUP;
    }
    /* ISO 的 CD001 在 0x8001 */
    if(n==8){
        int f2=open(path,O_RDONLY);
        if(f2>=0){
            unsigned char ib[8]={0};
            if(pread(f2,ib,sizeof ib,0x8001)==8 && (!memcmp(ib,"CD001",5)||!memcmp(ib,"BEA01",5))){
                close(f2);
                if(err&&errcap) snprintf(err,errcap,"ISO 不能追加；整张重建请用 archive.create --format=iso");
                return IMG_ENOTSUP;
            }
            close(f2);
        }
    }
    if(err&&errcap) snprintf(err,errcap,"不认识的包格式（不是 zip/7z/tar）：%s",path);
    return IMG_EFORMAT;
}

/* 建临时文件（同目录，rename 才是原子的）*/
static int aw_mktmp(img_aw *w, char *err, size_t errcap){
    size_t dl=strlen(w->dst);
    if(dl+32>=sizeof w->tmp){
        aw_set_err(err,errcap,"目标路径太长：%s","",NULL);
        return IMG_ENAMETOOLONG;
    }
    /* ★ P1-6：统一临时命名 imgtool-tmp-aw-<pid>-<n>（同目录，保 rename 原子性） */
    {
        char dir[2048]; snprintf(dir,sizeof dir,"%s",w->dst);
        char *sl=strrchr(dir,'/');
        if(!sl) snprintf(w->tmp,sizeof w->tmp,"imgtool-tmp-aw-%ld-XXXXXX",(long)getpid());
        else {
            *sl=0;
            snprintf(w->tmp,sizeof w->tmp,"%s/imgtool-tmp-aw-%ld-XXXXXX",dir,(long)getpid());
        }
    }
    w->fd=mkstemp(w->tmp);
    if(w->fd<0){
        if(err&&errcap) snprintf(err,errcap,"建临时文件失败：%s",strerror(errno));
        return IMG_EIO;
    }
    fchmod(w->fd,0644);
    return IMG_OK;
}

static int aw_setup_writer(img_aw *w, int level, const char *iso_volume, bool iso_joliet,
                           char *err, size_t errcap){
    struct archive *a=archive_write_new();
    if(!a){ aw_set_err(err,errcap,"内存不足","",NULL); return IMG_ENOMEM; }
    int r=ARCHIVE_OK;
    if(level<1) level=5;
    if(level>9) level=9;
    char lv[8]; snprintf(lv,sizeof lv,"%d",level);
    switch(w->fmt){
    case IMG_AW_ZIP:
        r=archive_write_set_format_zip(a);
        archive_write_set_format_option(a,"zip","zip64","1");
        archive_write_set_format_option(a,"zip","compression","deflate");
        archive_write_set_format_option(a,"zip","compression-level",lv);
        break;
    case IMG_AW_7Z:
        r=archive_write_set_format_7zip(a);
        archive_write_set_format_option(a,"7zip","compression","lzma2");
        archive_write_set_format_option(a,"7zip","compression-level",lv);
        break;
    case IMG_AW_TAR:
        r=archive_write_set_format_pax_restricted(a);
        break;
    case IMG_AW_TGZ:
        r=archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_gzip(a);
        archive_write_set_filter_option(a,"gzip","compression-level",lv);
        break;
    case IMG_AW_ISO:
        r=archive_write_set_format_iso9660(a);
        archive_write_set_format_option(a,"iso9660","iso-level","3");
        archive_write_set_format_option(a,"iso9660","joliet", iso_joliet?"1":"0");
        archive_write_set_format_option(a,"iso9660","rockridge","0");   /* Windows 不认识 RR */
        archive_write_set_format_option(a,"iso9660","volume-id",
                                        (iso_volume&&*iso_volume)?iso_volume:"IMGTool");
        break;
    default:
        archive_write_free(a);
        aw_set_err(err,errcap,"这个格式不能写","",NULL);
        return IMG_ENOTSUP;
    }
    if(r!=ARCHIVE_OK){
        if(err&&errcap) snprintf(err,errcap,"初始化归档失败：%s",archive_error_string(a));
        archive_write_free(a);
        return IMG_ESTATE;
    }
    r=archive_write_open_fd(a,w->fd);
    if(r!=ARCHIVE_OK){
        if(err&&errcap) snprintf(err,errcap,"打开输出失败：%s",archive_error_string(a));
        archive_write_free(a);
        return IMG_EIO;
    }
    w->a=a;
    return IMG_OK;
}

/* 把 dst 现有条目搬进新包（同一格式；压缩等级可以不同）*/
static int aw_take_existing(img_aw *w, const char *const *skip, int n_skip, bool skip_subtree,
                            img_prog *prog, char *err, size_t errcap){
    struct archive *r=archive_read_new();
    if(!r) return IMG_ENOMEM;
    archive_read_support_filter_all(r);
    archive_read_support_format_all(r);
    if(archive_read_open_filename(r,w->dst,128*1024)!=ARCHIVE_OK){
        if(err&&errcap) snprintf(err,errcap,"原包打不开（已损坏？）：%s",archive_error_string(r)?archive_error_string(r):"格式错误");
        archive_read_free(r);
        return IMG_EFORMAT;
    }
    struct archive_entry *e=NULL;
    int rc=IMG_OK;
    char *buf=(char*)malloc(AW_CHUNK);
    if(!buf){ archive_read_free(r); return IMG_ENOMEM; }
    char key[2048], nm[2048];
    for(;;){
        int hr=archive_read_next_header(r,&e);
        if(hr==ARCHIVE_EOF) break;
        if(hr<ARCHIVE_WARN){ rc=IMG_EFORMAT; break; }
        const char *pn=archive_entry_pathname(e);
        if(!pn) continue;
        aw_norm(pn,key,sizeof key);
        bool isdir=(archive_entry_filetype(e)==AE_IFDIR);
        bool skipped=false;
        for(int si=0;si<n_skip && !skipped;si++){
            const char *sp=skip[si];
            if(!sp||!*sp) continue;
            if(skip_subtree ? aw_under(key,sp) : !strcmp(key,sp)) skipped=true;
        }
        if(skipped){ archive_read_data_skip(r); continue; }
        aw_write_name(key,nm,sizeof nm,isdir);
        archive_entry_set_pathname(e,nm);
        if(archive_write_header(w->a,e)!=ARCHIVE_OK){
            if(err&&errcap) snprintf(err,errcap,"搬旧条目失败（%s）：%s",nm,archive_error_string(w->a));
            rc=IMG_EIO; break;
        }
        if(isdir || archive_entry_size(e)<=0){
            /* 空文件也要走一遍 header（上面已写）*/
        }
        if(!isdir){
            for(;;){
                la_ssize_t n=archive_read_data(r,buf,AW_CHUNK);
                if(n==0) break;
                if(n<0){ rc=IMG_EFORMAT; break; }
                if(archive_write_data(w->a,buf,(size_t)n)<0){ rc=IMG_EIO; break; }
            }
            if(rc!=IMG_OK) break;
        }
        w->entries++;
        if(isdir){ char dk[2050]; snprintf(dk,sizeof dk,"/%s/", key[0]=='/'?key+1:key); aw_set_add(&w->set,dk); }
        else aw_set_add(&w->set,key);
        if(prog) img_prog_item(prog,key);
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
    }
    free(buf);
    archive_read_free(r);
    if(rc!=IMG_OK && err&&errcap && !err[0])
        snprintf(err,errcap,"搬旧条目失败：%s",archive_error_string(w->a)?archive_error_string(w->a):"格式/数据错误");
    return rc;
}

int img_aw_begin(const char *dst, img_aw_fmt fmt, int level,
                 const char *iso_volume, bool iso_joliet,
                 bool take_existing,
                 const char *const *skip, int n_skip, bool skip_subtree,
                 img_aw **out, char *err, size_t errcap){
    if(out) *out=NULL;
    if(err&&errcap) err[0]=0;
    if(!dst || !*dst || !out) return IMG_EINVAL;
    if(!img_aw_writable(fmt)){
        if(err&&errcap) snprintf(err,errcap,"这个格式不能写（RAR 只能读，请用 7z）");
        return IMG_ENOTSUP;
    }
    img_aw *w=(img_aw*)calloc(1,sizeof *w);
    if(!w) return IMG_ENOMEM;
    w->fmt=fmt;
    snprintf(w->dst,sizeof w->dst,"%s",dst);
    int rc=aw_mktmp(w,err,errcap);
    if(rc==IMG_OK) rc=aw_setup_writer(w,level,iso_volume,iso_joliet,err,errcap);
    if(rc==IMG_OK && take_existing){
        struct stat st;
        if(stat(w->dst,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0)
            rc=aw_take_existing(w,skip,n_skip,skip_subtree,NULL,err,errcap);
    }
    if(rc!=IMG_OK){
        if(w->a){ archive_write_close(w->a); archive_write_free(w->a); w->a=NULL; }
        if(w->fd>=0) close(w->fd);
        if(w->tmp[0]) unlink(w->tmp);
        aw_set_free(&w->set);
        free(w);
        return rc;
    }
    *out=w;
    return IMG_OK;
}

static int aw_set_lookup(const aw_set *s, const char *inner, bool *isdir){
    if(isdir) *isdir=false;
    char k[2048]; aw_norm(inner,k,sizeof k);
    if(!strcmp(k,"/")){ if(isdir) *isdir=true; return 1; }   /* 根永远算目录 */
    if(aw_set_has(s,k)) return 1;
    char dk[2050]; snprintf(dk,sizeof dk,"%s/",k);
    if(aw_set_has(s,dk)){ if(isdir) *isdir=true; return 1; }
    return 0;
}
bool img_aw_has(const img_aw *w, const char *inner){
    return w ? aw_set_lookup(&w->set,inner,NULL)>0 : false;
}
int img_aw_lookup(const img_aw *w, const char *inner, bool *isdir){
    return w ? aw_set_lookup(&w->set,inner,isdir) : 0;
}
uint64_t img_aw_entries(const img_aw *w){ return w?w->entries:0; }
uint64_t img_aw_count_children(const img_aw *w, const char *dir){
    if(!w) return 0;
    char d[2048]; aw_norm(dir,d,sizeof d);
    size_t dl=strlen(d);
    uint64_t n=0;
    for(size_t i=0;i<w->set.cap;i++){
        const char *k=w->set.keys[i];
        if(!k) continue;
        size_t kl=strlen(k);
        while(kl>1 && k[kl-1]=='/') kl--;          /* dir 键的尾 / 不算 */
        char tmp[2048];
        if(kl>=sizeof tmp) continue;
        memcpy(tmp,k,kl); tmp[kl]=0;
        if(dl==1 && d[0]=='/'){
            if(tmp[0]=='/' && !strchr(tmp+1,'/')) n++;
            continue;
        }
        if(strncmp(tmp,d,dl)!=0 || tmp[dl]!='/') continue;
        if(strchr(tmp+dl+1,'/')) continue;
        if(tmp[dl+1]) n++;
    }
    return n;
}

int img_aw_add_dir(img_aw *w, const char *inner, uint64_t mtime, char *err, size_t errcap){
    if(!w||!w->a) return IMG_ESTATE;
    char key[2048], nm[2048];
    aw_norm(inner,key,sizeof key);
    aw_write_name(key,nm,sizeof nm,true);
    struct archive_entry *e=archive_entry_new();
    if(!e) return IMG_ENOMEM;
    archive_entry_set_pathname(e,nm);
    archive_entry_set_filetype(e,AE_IFDIR);
    archive_entry_set_perm(e,0755);
    archive_entry_set_size(e,0);
    archive_entry_set_mtime(e,(time_t)(mtime?mtime:(uint64_t)time(NULL)),0);
    int r=archive_write_header(w->a,e);
    archive_entry_free(e);
    if(r!=ARCHIVE_OK){
        if(err&&errcap) snprintf(err,errcap,"写目录条目失败（%s）：%s",nm,archive_error_string(w->a));
        return IMG_EIO;
    }
    w->entries++;
    char dk[2050]; snprintf(dk,sizeof dk,"%s%s", strcmp(key,"/")?"":"/", key);
    /* 统一成 "/a/b/" */
    snprintf(dk,sizeof dk,"%s%s", (key[0]=='/')?key:"/", "/");
    return aw_set_add(&w->set,dk);
}

/* 补父目录条目：copy 一个文件到 /a/b/c.txt 时，/a、/a/b 可能还不存在。
 * 必须在写文件 header 之前补（写完数据就不能再插 header 了）。*/
static int aw_ensure_parents(img_aw *w, const char *key, uint64_t mtime,
                             char *err, size_t errcap){
    char path[2048];
    snprintf(path,sizeof path,"%s",key);
    char *p=path+1;
    while((p=strchr(p,'/'))!=NULL){
        *p=0;
        char dk[2050]; snprintf(dk,sizeof dk,"%s/",path);
        if(!aw_set_has(&w->set,dk)){
            char nm[2048];
            aw_write_name(path,nm,sizeof nm,true);
            struct archive_entry *e=archive_entry_new();
            if(!e) return IMG_ENOMEM;
            archive_entry_set_pathname(e,nm);
            archive_entry_set_filetype(e,AE_IFDIR);
            archive_entry_set_perm(e,0755);
            archive_entry_set_size(e,0);
            archive_entry_set_mtime(e,(time_t)(mtime?mtime:(uint64_t)time(NULL)),0);
            int r=archive_write_header(w->a,e);
            archive_entry_free(e);
            if(r!=ARCHIVE_OK){
                if(err&&errcap) snprintf(err,errcap,"写父目录条目失败（%s）：%s",nm,archive_error_string(w->a));
                return IMG_EIO;
            }
            w->entries++;
            int rc=aw_set_add(&w->set,dk);
            if(rc!=IMG_OK) return rc;
        }
        *p='/';
        p++;
    }
    return IMG_OK;
}

int img_aw_add_file(img_aw *w, const char *inner, const img_stream *st, uint64_t size,
                    uint64_t mtime, img_prog *prog,
                    uint64_t *io_done, uint64_t io_total,
                    char *err, size_t errcap){
    if(!w||!w->a||!st) return IMG_ESTATE;
    char key[2048], nm[2048];
    aw_norm(inner,key,sizeof key);
    int prc=aw_ensure_parents(w,key,mtime,err,errcap);
    if(prc!=IMG_OK) return prc;
    aw_write_name(key,nm,sizeof nm,false);
    struct archive_entry *e=archive_entry_new();
    if(!e) return IMG_ENOMEM;
    archive_entry_set_pathname(e,nm);
    archive_entry_set_filetype(e,AE_IFREG);
    archive_entry_set_perm(e,0644);
    archive_entry_set_size(e,(la_int64_t)size);
    archive_entry_set_mtime(e,(time_t)(mtime?mtime:(uint64_t)time(NULL)),0);
    int r=archive_write_header(w->a,e);
    archive_entry_free(e);
    if(r!=ARCHIVE_OK){
        if(err&&errcap) snprintf(err,errcap,"写条目失败（%s）：%s",nm,archive_error_string(w->a));
        return IMG_EIO;
    }
    char *buf=(char*)malloc(AW_CHUNK);
    if(!buf) return IMG_ENOMEM;
    uint64_t off=0;
    int rc=IMG_OK;
    if(prog) img_prog_item(prog,key);
    while(off<size){
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        size_t want=(size_t)((size-off<AW_CHUNK)?(size-off):AW_CHUNK);
        size_t got=0;
        rc=st->read(st->ud,off,buf,want,&got);
        if(rc!=IMG_OK) break;
        if(got==0){ rc=IMG_EIO; break; }              /* 源比声明短 */
        size_t put=0;
        while(put<got){
            la_ssize_t n=archive_write_data(w->a,buf+put,got-put);
            if(n<=0){ rc=IMG_EIO; break; }
            put+=(size_t)n;
        }
        if(rc!=IMG_OK) break;
        off+=got;
        if(prog && io_total){
            uint64_t cur=(io_done?*io_done:0)+off;
            img_prog_bytes(prog,cur,io_total);
        }
    }
    free(buf);
    if(rc!=IMG_OK){
        if(err&&errcap && !err[0]) snprintf(err,errcap,"写条目数据失败（%s）：%s",nm,
                                            archive_error_string(w->a)?archive_error_string(w->a):"读写错误");
        return rc;
    }
    if(io_done) *io_done += size;
    w->entries++;
    w->bytes_out += size;
    return aw_set_add(&w->set,key);
}

typedef struct { int fd; } aw_hostrd;
static int aw_host_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    aw_hostrd *h=(aw_hostrd*)ud;
    if(got) *got=0;
    size_t done=0;
    while(done<want){
        ssize_t n=pread(h->fd,(char*)buf+done,want-done,(off_t)(off+done));
        if(n<0){ if(errno==EINTR) continue; return IMG_EIO; }
        if(n==0) break;
        done+=(size_t)n;
    }
    if(got) *got=done;
    return IMG_OK;
}
int img_aw_add_host_file(img_aw *w, const char *inner, const char *host_path,
                         uint64_t size, uint64_t mtime, img_prog *prog,
                         uint64_t *io_done, uint64_t io_total,
                         char *err, size_t errcap){
    int fd=open(host_path,O_RDONLY);
    if(fd<0){
        if(err&&errcap) snprintf(err,errcap,"源文件打不开：%s",host_path);
        return IMG_ENOENT;
    }
    aw_hostrd h={fd};
    img_stream st; memset(&st,0,sizeof st);
    st.ud=&h; st.read=aw_host_read; st.close=NULL;
    int rc=img_aw_add_file(w,inner,&st,size,mtime,prog,io_done,io_total,err,errcap);
    close(fd);
    return rc;
}

int img_aw_commit(img_aw *w, uint64_t *out_size, char *err, size_t errcap){
    if(!w) return IMG_EINVAL;
    int rc=IMG_OK;
    if(w->a){
        if(archive_write_close(w->a)!=ARCHIVE_OK){
            if(err&&errcap) snprintf(err,errcap,"收尾失败：%s",
                                     archive_error_string(w->a)?archive_error_string(w->a):"写中央目录/卷描述符失败");
            rc=IMG_EIO;
        }
        archive_write_free(w->a);
        w->a=NULL;
    }
    if(rc==IMG_OK && fsync(w->fd)!=0) rc=IMG_EIO;
    if(w->fd>=0){ close(w->fd); w->fd=-1; }
    struct stat st; uint64_t sz=0;
    if(rc==IMG_OK){
        if(rename(w->tmp,w->dst)!=0){
            if(err&&errcap) snprintf(err,errcap,"重命名到目标失败：%s",strerror(errno));
            rc=IMG_EIO;
        }else{
            w->committed=true;
            if(stat(w->dst,&st)==0) sz=(uint64_t)st.st_size;
        }
    }
    if(rc!=IMG_OK) unlink(w->tmp);
    if(out_size) *out_size=sz;
    aw_set_free(&w->set);
    free(w);
    return rc;
}

void img_aw_abort(img_aw *w){
    if(!w) return;
    if(w->a){ archive_write_close(w->a); archive_write_free(w->a); w->a=NULL; }
    if(w->fd>=0){ close(w->fd); w->fd=-1; }
    if(w->tmp[0]) unlink(w->tmp);
    aw_set_free(&w->set);
    free(w);
}

/* ---------- 只读名字表 ---------- */
struct img_aw_names { aw_set set; };
int img_aw_names_load(const char *path, img_aw_names **out, char *err, size_t errcap){
    if(out) *out=NULL;
    if(!path) return IMG_EINVAL;
    struct archive *r=archive_read_new();
    if(!r) return IMG_ENOMEM;
    archive_read_support_filter_all(r);
    archive_read_support_format_all(r);
    if(archive_read_open_filename(r,path,128*1024)!=ARCHIVE_OK){
        if(errno==ENOENT){ archive_read_free(r); return IMG_ENOENT; }
        if(err&&errcap) snprintf(err,errcap,"原包打不开：%s",archive_error_string(r)?archive_error_string(r):path);
        archive_read_free(r);
        return IMG_EFORMAT;
    }
    img_aw_names *ns=(img_aw_names*)calloc(1,sizeof *ns);
    if(!ns){ archive_read_free(r); return IMG_ENOMEM; }
    struct archive_entry *e=NULL;
    char key[2048];
    for(;;){
        int hr=archive_read_next_header(r,&e);
        if(hr==ARCHIVE_EOF) break;
        if(hr<ARCHIVE_WARN) break;
        const char *pn=archive_entry_pathname(e);
        if(!pn) continue;
        aw_norm(pn,key,sizeof key);
        bool isdir=(archive_entry_filetype(e)==AE_IFDIR);
        if(isdir){ char dk[2050]; snprintf(dk,sizeof dk,"%s/",key); aw_set_add(&ns->set,dk); }
        else aw_set_add(&ns->set,key);
    }
    archive_read_free(r);
    *out=ns;
    return IMG_OK;
}
bool img_aw_names_has(const img_aw_names *ns, const char *inner){
    return ns ? aw_set_lookup(&ns->set,inner,NULL)>0 : false;
}
int img_aw_names_lookup(const img_aw_names *ns, const char *inner, bool *isdir){
    return ns ? aw_set_lookup(&ns->set,inner,isdir) : 0;
}
uint64_t img_aw_names_count(const img_aw_names *ns){ return ns?ns->set.n:0; }
uint64_t img_aw_names_count_children(const img_aw_names *ns, const char *dir){
    if(!ns) return 0;
    char d[2048]; aw_norm(dir,d,sizeof d);
    size_t dl=strlen(d);
    uint64_t n=0;
    for(size_t i=0;i<ns->set.cap;i++){
        const char *k=ns->set.keys[i];
        if(!k) continue;
        size_t kl=strlen(k);
        while(kl>1 && k[kl-1]=='/') kl--;
        char tmp[2048];
        if(kl>=sizeof tmp) continue;
        memcpy(tmp,k,kl); tmp[kl]=0;
        if(dl==1 && d[0]=='/'){
            if(tmp[0]=='/' && !strchr(tmp+1,'/')) n++;
            continue;
        }
        if(strncmp(tmp,d,dl)!=0 || tmp[dl]!='/') continue;
        if(strchr(tmp+dl+1,'/')) continue;
        if(tmp[dl+1]) n++;
    }
    return n;
}
void img_aw_names_free(img_aw_names *ns){
    if(!ns) return;
    aw_set_free(&ns->set);
    free(ns);
}

#else  /* !IMG_WITH_ARCHIVE：全部降级成 ENOTSUP，但符号要有（否则链接失败） */

struct img_aw { int unused; };
const char *img_aw_fmt_name(img_aw_fmt f){ (void)f; return "?"; }
img_aw_fmt img_aw_fmt_by_name(const char *p){ (void)p; return IMG_AW_NONE; }
bool img_aw_writable(img_aw_fmt f){ (void)f; return false; }
int img_aw_fmt_of_existing(const char *p, img_aw_fmt *o, char *e, size_t c){
    (void)p; if(o) *o=IMG_AW_NONE;
    if(e&&c) snprintf(e,c,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
int img_aw_begin(const char *d, img_aw_fmt f, int l, const char *v, bool j,
                 bool t, const char *const *s, int ns, bool st, img_aw **o, char *e, size_t c){
    (void)d;(void)f;(void)l;(void)v;(void)j;(void)t;(void)s;(void)ns;(void)st;
    if(o) *o=NULL;
    if(e&&c) snprintf(e,c,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
bool img_aw_has(const img_aw *w, const char *p){ (void)w;(void)p; return false; }
uint64_t img_aw_count_children(const img_aw *w, const char *d){ (void)w;(void)d; return 0; }
uint64_t img_aw_entries(const img_aw *w){ (void)w; return 0; }
int img_aw_add_dir(img_aw *w, const char *i, uint64_t m, char *e, size_t c){
    (void)w;(void)i;(void)m;(void)e;(void)c; return IMG_ENOTSUP;
}
int img_aw_add_file(img_aw *w, const char *i, const img_stream *s, uint64_t z, uint64_t m,
                    img_prog *p, uint64_t *d, uint64_t t, char *e, size_t c){
    (void)w;(void)i;(void)s;(void)z;(void)m;(void)p;(void)d;(void)t;(void)e;(void)c;
    return IMG_ENOTSUP;
}
int img_aw_add_host_file(img_aw *w, const char *i, const char *h, uint64_t z, uint64_t m,
                         img_prog *p, uint64_t *d, uint64_t t, char *e, size_t c){
    (void)w;(void)i;(void)h;(void)z;(void)m;(void)p;(void)d;(void)t;(void)e;(void)c;
    return IMG_ENOTSUP;
}
int img_aw_commit(img_aw *w, uint64_t *s, char *e, size_t c){
    (void)w;(void)s;(void)e;(void)c; return IMG_ENOTSUP;
}
void img_aw_abort(img_aw *w){ (void)w; }
int  img_aw_names_load(const char *p, img_aw_names **o, char *e, size_t c){
    (void)p; if(o) *o=NULL;
    if(e&&c) snprintf(e,c,"本构建未启用 libarchive（IMG_WITH_ARCHIVE=OFF）");
    return IMG_ENOTSUP;
}
bool img_aw_names_has(const img_aw_names *n, const char *i){ (void)n;(void)i; return false; }
int  img_aw_names_lookup(const img_aw_names *n, const char *i, bool *d){ (void)n;(void)i;(void)d; return 0; }
int  img_aw_lookup(const img_aw *w, const char *i, bool *d){ (void)w;(void)i;(void)d; return 0; }
uint64_t img_aw_names_count(const img_aw_names *n){ (void)n; return 0; }
uint64_t img_aw_names_count_children(const img_aw_names *n, const char *d){ (void)n;(void)d; return 0; }
void img_aw_names_free(img_aw_names *n){ (void)n; }

#endif