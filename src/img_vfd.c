#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_vfd.c — 虚拟文件描述符（原理见 include/img_vfd.h）
 *
 * 两张表，容量都很小：
 *   mnt[] 一次挂载 = 一个 ISO 成员（img_iso 句柄 + 成员路径 + 大小 + 引用计数）
 *   fh[]  wimlib 拿到的每个 fd 对应一条（指向 mnt[]，带自己的读写位置）
 *
 * ★ 所有 __wrap_* 对"不是我们的 fd / 不是假路径"一律原样转 __real_*，
 *   所以对普通文件、目录、socket 的行为一个字节都不变。
 * ★ __real_* 只有在链接时给了对应的 --wrap= 才存在：
 *   glibc 专有的 __open_2 / __realpath_chk 用 __GLIBC__ 包起来（bionic 没有）。
 */
#include "img_vfd.h"
#include "img_iso.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef IMG_WIM_VFD
#define IMG_WIM_VFD 0
#endif

#ifdef IMG_BT_TRACE
#include <execinfo.h>
#include <signal.h>
static void bt_alarm(int sig){
    (void)sig;
    void *bt[48]; int n=backtrace(bt,48);
    backtrace_symbols_fd(bt,n,2);
    _exit(9);
}
__attribute__((constructor)) static void bt_init(void){
    if(getenv("IMG_BT")){ signal(SIGALRM,bt_alarm); alarm(4); }
}
#endif
#define VFD_MAX_MNT 4
#define VFD_MAX_FH  8
#define VFD_PREFIX  "/imgtool-vfd/"

typedef struct {
    bool     used;
    img_iso *iso;
    char     inner[512];
    uint64_t size;
    int      refs;          /* 打开的 fd 数；降到 0 时关 ISO */
    bool     drop;          /* unmount 过，等最后一个 fd 关掉再收 */
} vfd_mnt;

typedef struct {
    bool     used;
    int      fd;
    int      mnt;
    uint64_t off;
} vfd_fh;

static vfd_mnt g_mnt[VFD_MAX_MNT];
static vfd_fh  g_fh[VFD_MAX_FH];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* 调试开关：IMG_VFD_DEBUG=1 时把每次 fd 操作打到 stderr（排查"卡住"用）*/
static int vdbg(void){ static int on=-1; if(on<0){ const char *e=getenv("IMG_VFD_DEBUG"); on=(e&&*e&&*e!='0')?1:0; } return on; }
#define VDBG(...) do{ if(vdbg()){ fprintf(stderr,"[vfd] " __VA_ARGS__); fflush(stderr); } }while(0)

/* ============================ 挂载表 ============================ */
bool img_vfd_ready(void){ return IMG_WIM_VFD ? true : false; }

int img_vfd_active(void){
    int n=0;
    pthread_mutex_lock(&g_mu);
    for(int i=0;i<VFD_MAX_MNT;i++) if(g_mnt[i].used) n++;
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* ★ 只"摘走"，不在这里关 —— img_iso_close → img_io_close → close() 会再进 __wrap_close
 *   拿同一把锁，等于自己把自己锁死（2026-10-26 实测死锁栈就在这里）。
 *   调用方拿返回值在【锁外】关。*/
static img_iso *mnt_take_locked(int i){
    if(!g_mnt[i].used) return NULL;
    img_iso *iso=g_mnt[i].iso;
    memset(&g_mnt[i],0,sizeof g_mnt[i]);
    return iso;
}

int img_vfd_mount(const char *iso_path, const char *inner_path,
                  char *vpath, size_t cap, uint64_t *size,
                  char *err, size_t errcap){
    if(vpath && cap) vpath[0]=0;
    if(size) *size=0;
    if(err && errcap) err[0]=0;
    if(!IMG_WIM_VFD){
        if(err && errcap) snprintf(err,errcap,"本构建的 wimlib 不是静态链接，直读 ISO 不可用");
        return IMG_ENOTSUP;
    }
    if(!iso_path || !inner_path) return IMG_EINVAL;
    img_iso *iso=NULL;
    int rc=img_iso_open_path(&iso,iso_path,err,errcap);
    if(rc!=IMG_OK) return rc;
    uint64_t sz=0; bool isdir=false;
    rc=img_iso_stat(iso,inner_path,&sz,&isdir);
    if(rc!=IMG_OK || isdir){
        img_iso_close(iso);
        if(err && errcap) snprintf(err,errcap,"ISO 里没有这个文件：%s",inner_path);
        return IMG_ENOENT;
    }
    int slot=-1;
    pthread_mutex_lock(&g_mu);
    for(int i=0;i<VFD_MAX_MNT;i++) if(!g_mnt[i].used){ slot=i; break; }
    if(slot>=0){
        g_mnt[slot].used=true;
        g_mnt[slot].iso=iso;
        g_mnt[slot].size=sz;
        g_mnt[slot].refs=0;
        g_mnt[slot].drop=false;
        snprintf(g_mnt[slot].inner,sizeof g_mnt[slot].inner,"%s",inner_path);
    }
    pthread_mutex_unlock(&g_mu);
    if(slot<0){
        img_iso_close(iso);
        if(err && errcap) snprintf(err,errcap,"虚拟句柄用满了（同时打开太多 ISO）");
        return IMG_EFULL;
    }
    VDBG("mount slot=%d size=%llu inner=%s\n", slot, (unsigned long long)sz, inner_path);
    if(vpath && cap) snprintf(vpath,cap,"%s%d",VFD_PREFIX,slot);
    if(size) *size=sz;
    return IMG_OK;
}

/* 把假路径解析成槽号；不是假路径返回 -1 */
static int slot_of_path(const char *path){
    if(!path) return -1;
    size_t pl=strlen(VFD_PREFIX);
    if(strncmp(path,VFD_PREFIX,pl)!=0) return -1;
    const char *d=path+pl;
    if(d[0]<'0'||d[0]>'9') return -1;
    int v=0;
    for(; *d>='0'&&*d<='9'; d++){
        v=v*10+(*d-'0');
        if(v>=VFD_MAX_MNT) return -1;
    }
    if(*d) return -1;
    return v;
}

/* 按 fd 找读写句柄（调用方持锁）*/
static int fh_of_fd_locked(int fd){
    for(int i=0;i<VFD_MAX_FH;i++) if(g_fh[i].used && g_fh[i].fd==fd) return i;
    return -1;
}

void img_vfd_unmount(const char *vpath){
    int s=slot_of_path(vpath);
    if(s<0) return;
    pthread_mutex_lock(&g_mu);
    img_iso *iso=NULL;
    if(g_mnt[s].used){
        if(g_mnt[s].refs<=0) iso=mnt_take_locked(s);
        else g_mnt[s].drop=true;      /* 还有 fd 开着，等 close 收 */
    }
    pthread_mutex_unlock(&g_mu);
    if(iso) img_iso_close(iso);       /* ★ 锁外关 */
}

/* ============================ __real_*（必须自己定义！）============================
 * ★ 2026-10-26（REQ-18）血的教训：`-Wl,--wrap=X` 只在【可执行文件】里由链接器
 *   自动把 __real_X 指回原符号；到了【共享库】，__real_X 没人定义 → 变成动态未定义符号
 *   → libimgapp.so 带 -z now，dlopen 阶段就解析失败 → App 里每一条命令都 JNI-FAIL。
 *   （imgcmd 是可执行文件，所以当时"本地好好的"——这就是漏测的地方。）
 * 这里用 dlsym(RTLD_NEXT) 拿真正的 libc 函数：**不能直接写 open(...)**，因为同一个 TU
 * 里的调用也会被 --wrap 改道到 __wrap_open，等于自己递归自己。
 * 函数指针缓存一次，之后就是一次间接调用。 */
typedef ssize_t (*fn_read_t)(int, void*, size_t);
typedef ssize_t (*fn_pread_t)(int, void*, size_t, off_t);
typedef off_t   (*fn_lseek_t)(int, off_t, int);
typedef int     (*fn_fstat_t)(int, struct stat*);
typedef int     (*fn_close_t)(int);
typedef int     (*fn_open_t)(const char*, int, ...);
typedef char   *(*fn_realpath_t)(const char*, char*);

/* 用 memcpy 做 void*→函数指针的转换（直接强转在 -Wpedantic 下会报警） */
#define REAL_FN(type, name, sym) \
    static type name##_fn; \
    if(!name##_fn){ void *sym_p_=dlsym(RTLD_NEXT, sym); \
                    memcpy(&name##_fn, &sym_p_, sizeof sym_p_); }

ssize_t __real_read(int fd, void *b, size_t n){
    REAL_FN(fn_read_t, real_read, "read");
    return real_read_fn ? real_read_fn(fd,b,n) : (errno=ENOSYS, (ssize_t)-1);
}
ssize_t __real_pread(int fd, void *b, size_t n, off_t o){
    REAL_FN(fn_pread_t, real_pread, "pread");
    return real_pread_fn ? real_pread_fn(fd,b,n,o) : (errno=ENOSYS, (ssize_t)-1);
}
off_t __real_lseek(int fd, off_t o, int w){
    REAL_FN(fn_lseek_t, real_lseek, "lseek");
    return real_lseek_fn ? real_lseek_fn(fd,o,w) : (errno=ENOSYS, (off_t)-1);
}
int __real_fstat(int fd, struct stat *st){
    REAL_FN(fn_fstat_t, real_fstat, "fstat");
    return real_fstat_fn ? real_fstat_fn(fd,st) : (errno=ENOSYS, -1);
}
int __real_close(int fd){
    REAL_FN(fn_close_t, real_close, "close");
    return real_close_fn ? real_close_fn(fd) : (errno=ENOSYS, -1);
}
int __real_open(const char *p, int f, ...){
    mode_t m=0;
    if(f & (O_CREAT|O_TMPFILE)){ va_list ap; va_start(ap,f); m=(mode_t)va_arg(ap,int); va_end(ap); }
    REAL_FN(fn_open_t, real_open, "open");
    return real_open_fn ? real_open_fn(p,f,m) : (errno=ENOSYS, -1);
}
int __real_open64(const char *p, int f, ...){
    mode_t m=0;
    if(f & (O_CREAT|O_TMPFILE)){ va_list ap; va_start(ap,f); m=(mode_t)va_arg(ap,int); va_end(ap); }
    REAL_FN(fn_open_t, real_open64, "open64");
    return real_open64_fn ? real_open64_fn(p,f,m) : (errno=ENOSYS, -1);
}
char *__real_realpath(const char *p, char *r){
    REAL_FN(fn_realpath_t, real_realpath, "realpath");
    return real_realpath_fn ? real_realpath_fn(p,r) : NULL;
}
/* LFS 变体（Debian 的 libwim.a 用 open64/pread64/lseek64/fstat64；
 * LP64 上 off64_t==off_t、struct stat64==struct stat）*/
ssize_t __real_pread64(int fd, void *b, size_t n, off_t o){ return __real_pread(fd,b,n,o); }
off_t   __real_lseek64(int fd, off_t o, int w){ return __real_lseek(fd,o,w); }
int     __real_fstat64(int fd, void *st){ return __real_fstat(fd,(struct stat*)st); }
#ifdef __GLIBC__
int __real___open_2(const char *p, int f){
    mode_t m=0;
    typedef int (*fn_t)(const char*, int, ...);
    static fn_t fn; if(!fn){ void *p_=dlsym(RTLD_NEXT,"__open_2"); memcpy(&fn,&p_,sizeof p_); }
    return fn ? fn(p,f,m) : (errno=ENOSYS, -1);
}
char *__real___realpath_chk(const char *p, char *r, size_t len){
    REAL_FN(fn_realpath_t, real_realpath_chk, "__realpath_chk");
    if(real_realpath_chk_fn) { (void)len; return real_realpath_chk_fn(p,r); }
    return __real_realpath(p,r);
}
#endif

/* ============================ 包装体 ============================ */
int __wrap_open(const char *path, int flags, ...){
    VDBG("open(%s) flags=%d\n", path?path:"(null)", flags);
    mode_t mode=0;
    if(flags & (O_CREAT|O_TMPFILE)){
        va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap);
        return __real_open(path,flags,mode);
    }
    int s=slot_of_path(path);
    if(s<0) return __real_open(path,flags);
    int fd=__real_open("/dev/null",O_RDONLY);
    if(fd<0) return -1;
    pthread_mutex_lock(&g_mu);
    int k=-1;
    for(int i=0;i<VFD_MAX_FH;i++) if(!g_fh[i].used){ k=i; break; }
    if(k<0 || !g_mnt[s].used){
        pthread_mutex_unlock(&g_mu);
        __real_close(fd);
        errno=EMFILE;
        return -1;
    }
    g_fh[k].used=true; g_fh[k].fd=fd; g_fh[k].mnt=s; g_fh[k].off=0;
    g_mnt[s].refs++;
    pthread_mutex_unlock(&g_mu);
    return fd;
}

#ifdef __GLIBC__
int __wrap___open_2(const char *path, int flags){
    return __wrap_open(path, flags);
}
char *__wrap___realpath_chk(const char *path, char *resolved, size_t len){
    if(slot_of_path(path)>=0){
        if(resolved){
            if(len) strncpy(resolved,path,len-1), resolved[len-1]=0;
            return resolved;
        }
        return strdup(path);
    }
    return __real___realpath_chk(path,resolved,len);
}
#endif

ssize_t __wrap_read(int fd, void *buf, size_t n){
    VDBG("read(fd=%d,n=%zu)\n", fd, n);
    int k;
    pthread_mutex_lock(&g_mu);
    k=fh_of_fd_locked(fd);
    if(k<0){ pthread_mutex_unlock(&g_mu); return __real_read(fd,buf,n); }
    int s=g_fh[k].mnt;
    uint64_t off=g_fh[k].off;
    uint64_t size=g_mnt[s].size;
    img_iso *iso=g_mnt[s].iso;
    char inner[512];
    snprintf(inner,sizeof inner,"%s",g_mnt[s].inner);
    if(off>=size){ pthread_mutex_unlock(&g_mu); return 0; }   /* EOF */
    size_t want = (size-off) < (uint64_t)n ? (size_t)(size-off) : n;
    pthread_mutex_unlock(&g_mu);
    int rc=img_iso_read_range(iso,inner,off,buf,want);
    if(rc!=IMG_OK){ errno=EIO; return -1; }
    pthread_mutex_lock(&g_mu);
    if(fh_of_fd_locked(fd)==k) g_fh[k].off=off+want;
    pthread_mutex_unlock(&g_mu);
    return (ssize_t)want;
}

ssize_t __wrap_pread(int fd, void *buf, size_t n, off_t off){
    VDBG("pread(fd=%d,n=%zu,off=%lld)\n", fd, n, (long long)off);
    int k;
    pthread_mutex_lock(&g_mu);
    k=fh_of_fd_locked(fd);
    if(k<0){ pthread_mutex_unlock(&g_mu); return __real_pread(fd,buf,n,off); }
    int s=g_fh[k].mnt;
    uint64_t size=g_mnt[s].size;
    img_iso *iso=g_mnt[s].iso;
    char inner[512];
    snprintf(inner,sizeof inner,"%s",g_mnt[s].inner);
    pthread_mutex_unlock(&g_mu);
    if(off<0){ errno=EINVAL; return -1; }
    if((uint64_t)off>=size) return 0;
    size_t want = (size-(uint64_t)off) < (uint64_t)n ? (size_t)(size-(uint64_t)off) : n;
    int rc=img_iso_read_range(iso,inner,(uint64_t)off,buf,want);
    if(rc!=IMG_OK){ errno=EIO; return -1; }
    return (ssize_t)want;
}

off_t __wrap_lseek(int fd, off_t off, int whence){
    VDBG("lseek(fd=%d,off=%lld,w=%d)\n", fd, (long long)off, whence);
    int k;
    pthread_mutex_lock(&g_mu);
    k=fh_of_fd_locked(fd);
    if(k<0){ pthread_mutex_unlock(&g_mu); return __real_lseek(fd,off,whence); }
    int s=g_fh[k].mnt;
    int64_t base;
    switch(whence){
    case SEEK_SET: base=0; break;
    case SEEK_CUR: base=(int64_t)g_fh[k].off; break;
    case SEEK_END: base=(int64_t)g_mnt[s].size; break;
    default: pthread_mutex_unlock(&g_mu); errno=EINVAL; return -1;
    }
    int64_t no=base+(int64_t)off;
    if(no<0){ pthread_mutex_unlock(&g_mu); errno=EINVAL; return -1; }
    g_fh[k].off=(uint64_t)no;
    pthread_mutex_unlock(&g_mu);
    return (off_t)no;
}

int __wrap_fstat(int fd, struct stat *st){
    VDBG("fstat(fd=%d)\n", fd);
    int k;
    pthread_mutex_lock(&g_mu);
    k=fh_of_fd_locked(fd);
    if(k<0){ pthread_mutex_unlock(&g_mu); return __real_fstat(fd,st); }
    uint64_t size=g_mnt[g_fh[k].mnt].size;
    pthread_mutex_unlock(&g_mu);
    if(st){
        memset(st,0,sizeof *st);
        st->st_mode = S_IFREG | 0444;
        st->st_size = (off_t)size;
        st->st_blksize = 4096;
        st->st_nlink = 1;
    }
    return 0;
}

int __wrap_close(int fd){
    VDBG("close(fd=%d)\n", fd);
    int k;
    img_iso *iso=NULL;
    pthread_mutex_lock(&g_mu);
    k=fh_of_fd_locked(fd);
    if(k>=0){
        int s=g_fh[k].mnt;
        g_fh[k].used=false;
        if(g_mnt[s].used){
            if(g_mnt[s].refs>0) g_mnt[s].refs--;
            if(g_mnt[s].refs==0 && g_mnt[s].drop) iso=mnt_take_locked(s);
        }
    }
    pthread_mutex_unlock(&g_mu);
    if(iso) img_iso_close(iso);        /* ★ 必须锁外关：img_iso_close → close() → __wrap_close */
    return __real_close(fd);
}

char *__wrap_realpath(const char *path, char *resolved){
    VDBG("realpath(%s)\n", path?path:"(null)");
    /* ★ wimlib 的 begin_read() 会 realpath(文件路径)：假路径在磁盘上不存在，
     *   不拦就会拿到 ENOENT 直接失败（WIMLIB_ERR_NO_FILENAME）。*/
    if(slot_of_path(path)>=0){
        if(resolved) return strcpy(resolved,path);
        return strdup(path);
    }
    return __real_realpath(path,resolved);
}

/* ============================ LFS 变体（open64/pread64/lseek64/fstat64）============================
 * Debian 的 libwim.a 用 open64() 打开 WIM（-_FILE_OFFSET_BITS=64），只包 open 拦不住。
 * Android 的 libwim.a 用的是 open()，这两个在这边用不到，但留着没有副作用。*/
int __wrap_open64(const char *path, int flags, ...){
    VDBG("open64(%s) flags=%d\n", path?path:"(null)", flags);
    mode_t mode=0;
    if(flags & (O_CREAT|O_TMPFILE)){
        va_list ap; va_start(ap,flags); mode=(mode_t)va_arg(ap,int); va_end(ap);
        return __real_open64(path,flags,mode);
    }
    int s=slot_of_path(path);
    if(s<0) return __real_open64(path,flags);
    int fd=__real_open("/dev/null",O_RDONLY);
    if(fd<0) return -1;
    pthread_mutex_lock(&g_mu);
    int k=-1;
    for(int i=0;i<VFD_MAX_FH;i++) if(!g_fh[i].used){ k=i; break; }
    if(k<0 || !g_mnt[s].used){
        pthread_mutex_unlock(&g_mu);
        __real_close(fd);
        errno=EMFILE;
        return -1;
    }
    g_fh[k].used=true; g_fh[k].fd=fd; g_fh[k].mnt=s; g_fh[k].off=0;
    g_mnt[s].refs++;
    pthread_mutex_unlock(&g_mu);
    return fd;
}

ssize_t __wrap_pread64(int fd, void *buf, size_t n, off_t off){
    return __wrap_pread(fd,buf,n,off);
}

off_t __wrap_lseek64(int fd, off_t off, int whence){
    return __wrap_lseek(fd,off,whence);
}

int __wrap_fstat64(int fd, void *st){
    return __wrap_fstat(fd,(struct stat*)st);
}
