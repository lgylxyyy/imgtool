#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* clock_gettime / CLOCK_MONOTONIC */
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "img_types.h"
#include <sys/time.h>

const char *img_strerror(int e){
    switch(e){
    case IMG_OK:         return "成功";
    case IMG_EINVAL:     return "参数非法";
    case IMG_ENOMEM:     return "内存不足";
    case IMG_EIO:        return "底层读写失败";
    case IMG_ENOENT:     return "对象不存在";
    case IMG_ENOTSUP:    return "暂不支持该特性";
    case IMG_EFORMAT:    return "格式不合法";
    case IMG_ECORRUPT:   return "数据损坏";
    case IMG_EAGAIN:     return "需要重试";
    case IMG_ECRC:       return "校验和不符";
    case IMG_EFULL:      return "空间不足";
    case IMG_EEXISTS:    return "已存在";
    case IMG_ESTATE:     return "当前状态不允许该操作";
    case IMG_ECANCELLED: return "已取消";
    case IMG_ETRUNC:     return "数据被截断";
    case IMG_ERANGE:     return "请求范围越界（偏移/长度超出映像）";
    case IMG_ECYCLE:     return "会造出目录环（搬进自己或自己的子树）";
    case IMG_ESAMEFILE:  return "源和目标是同一个文件";
    case IMG_ENAMETOOLONG:return "文件名超过长度上限";
    case IMG_EPARTIAL:   return "复制成功但删除源失败（两份都在）";
    case IMG_EREFUSED:   return "按安全策略拒绝";
    case IMG_EREADONLY:  return "目标是只读容器";
    case IMG_ENAMEINVALID:return "名字是 Windows 不接受的";
    case IMG_ESTALE:     return "索引/句柄已过期（包变了或超时）";
    default:             return "未知错误";
    }
}

const char *img_phase_name(img_phase_t p){
    switch(p){
    case IMG_PH_NONE:      return "none";
    case IMG_PH_OPEN:      return "open";
    case IMG_PH_CREATE:    return "create";
    case IMG_PH_EXPAND:    return "expand";
    case IMG_PH_PARTITION: return "partition";
    case IMG_PH_FORMAT:    return "format";
    case IMG_PH_DEPLOY:    return "deploy";
    case IMG_PH_BROWSE:    return "browse";
    case IMG_PH_CHECK:     return "check";
    case IMG_PH_FIX:       return "fix";
    case IMG_PH_BCD:       return "bcd";
    case IMG_PH_EXTRACT:   return "extract";
    case IMG_PH_COPY:      return "copy";
    case IMG_PH_PACK:      return "pack";
    case IMG_PH_WRITE:     return "write";
    default:               return "?";
    }
}

const char *img_op_name(img_op_t o){
    switch(o){
    case IMG_OP_NONE:        return "none";
    case IMG_OP_READ:        return "read";
    case IMG_OP_ALLOC:       return "alloc_cluster";
    case IMG_OP_WRITE:       return "write";
    case IMG_OP_SYNC_INDEX:  return "sync_index";
    case IMG_OP_WRITE_FILE:  return "write_file";
    case IMG_OP_WRITE_INDEX: return "write_index";
    case IMG_OP_SCAN:        return "scan";
    case IMG_OP_VERIFY:      return "verify";
    case IMG_OP_COMPRESS:    return "compress";
    case IMG_OP_DECOMPRESS:  return "decompress";
    default:                 return "?";
    }
}

/* ---- 可写临时目录（见 img_types.h 的说明）---- */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/statvfs.h>        /* stat/lstat/S_ISDIR（P1-6 统一临时文件） */
#include <sys/types.h>
/* ---- P1-6（2026-11-08）：统一临时目录策略 ----
 * 优先级：IMGTOOL_TMPDIR → TMPDIR → <镜像同目录>/.imgtool-tmp。
 * ★ 不再回落 "."（以前会把 imgtool-src-* 等撒在手机任意目录）。
 *   ★ 已删除「容器所在目录」档（REQ-61 要求 3）：映像可能在只读路径或
 *   别人的应用目录下，往那儿建目录要么失败、要么把临时文件留在用户可见处。 */
static int mk_ok(const char *d){
    /* 能写能进；并且真建一下（hint 目录可能还不存在） */
    if(!d||!*d) return 0;
    if(access(d,F_OK)!=0){ if(mkdir(d,0755)!=0) return 0; }
    return access(d,W_OK|X_OK)==0;
}
static void host_rm_rf_dir(const char *path);   /* 见文件尾（P1-6） */
/* ★ REQ-61 §3：来源标签（env / tmpdir / hint / fallback），供 sys.tmpdir 照实回报。
 *   cached 从函数级 static 提到文件级，才能被 img_tmpdir_reset() 清掉。 */
static char  g_tmp_cached[512];
static char  g_tmp_source[16] = "none";
const char *img_tmpdir_source(void){ img_tmpdir(); return g_tmp_source; }
void img_tmpdir_reset(void){ g_tmp_cached[0]=0; g_tmp_source[0]=0; }

const char *img_tmpdir(void){
    char *cached = g_tmp_cached;
    if(cached[0]) return cached;
    const char *cand[3]; int nc = 0;
    const char *tag[3];
    const char *e1 = getenv("IMGTOOL_TMPDIR"), *e2 = getenv("TMPDIR");
    if(e1 && *e1){ cand[nc]=e1; tag[nc++]="env"; }
    if(e2 && *e2){ cand[nc]=e2; tag[nc++]="tmpdir"; }
    /* ★ REQ-61 §0 要求 3：不再有「容器所在目录」这一档。
     *   目标映像可能在只读路径或别人的应用目录下，往那儿建 .imgtool-tmp
     *   要么失败、要么把临时文件留在用户看得见的地方。
     *   调用方（App）必须显式设 IMGTOOL_TMPDIR；没设就退到 /tmp。 */
    cand[nc]="/tmp"; tag[nc++]="fallback";
    for(int i = 0; i < nc; i++){
        size_t L = strlen(cand[i]);
        while(L && cand[i][L-1] == '/') L--;
        if(!L) continue;
        if(L >= sizeof cached - 16) continue;
        memcpy(cached, cand[i], L); cached[L] = 0;
        if(mk_ok(cached)){ snprintf(g_tmp_source,sizeof g_tmp_source,"%s",tag[i]); return cached; }
        cached[0] = 0;
    }
    g_tmp_source[0]=0;
    return NULL;
}

/* ---- ntfs/wimlib 占位设备文件（见 img_types.h 说明）----
 * ★ 生命周期（工程债 #14）：这个 0 字节文件以前【永不删除】——每跑一个进程就在
 *   临时目录里留一个 imgtool-dev-<pid>。长时间用下来能攒出成百上千个。
 *   现在两条路都堵上：
 *     ① atexit 删除（正常退出、img_cmd 返回错误码退出都覆盖）；
 *     ② 每次创建前扫一遍同目录，把【已经死掉的 pid】留下的清掉
 *        （kill(pid,0)==ESRCH 才算死；别人的活进程不动）。
 *   SIGKILL 仍然会漏一个，由下一次运行的第 ② 步收走。*/
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>

static char g_devpath[560];
static bool g_dev_atexit_done;
static void dev_placeholder_cleanup(void){ if(g_devpath[0]) unlink(g_devpath); }
static bool pid_is_alive(long pid){
    if(pid<=0) return false;
    if(kill((pid_t)pid,0)==0) return true;
    return errno != ESRCH;               /* EPERM = 进程在，只是不归我们管 */
}
static void dev_placeholder_sweep(const char *td){
    DIR *d=opendir(td);
    if(!d) return;
    struct dirent *de;
    while((de=readdir(d))){
        long pid=0;
        if(sscanf(de->d_name,"imgtool-dev-%ld",&pid)!=1) continue;
        if(pid==(long)getpid()) continue;
        if(pid_is_alive(pid)) continue;
        char victim[1200];
        snprintf(victim,sizeof victim,"%s/%s",td,de->d_name);
        unlink(victim);
    }
    closedir(d);
}

const char *img_dev_placeholder(void){
    if(g_devpath[0]) return g_devpath;
    const char *td = img_tmpdir();
    if(!td) return NULL;
    dev_placeholder_sweep(td);
    snprintf(g_devpath, sizeof g_devpath, "%s/imgtool-dev-%ld", td, (long)getpid());
    int fd = open(g_devpath, O_RDWR | O_CREAT, 0666);
    if(fd < 0){ g_devpath[0] = 0; return NULL; }
    close(fd);
    if(!g_dev_atexit_done){ atexit(dev_placeholder_cleanup); g_dev_atexit_done=true; }
    return g_devpath;
}

/* ---- P1-6：统一临时文件命名 + 清理 ------------------------------
 * 文件：  imgtool-tmp-<用途>-<pid>-<n>
 * 目录：  imgtool-tmp-<用途>-<pid>-XXXXXX（mkdtemp）
 * atexit：删自己 pid 的 imgtool-tmp-*（文件与目录都收）。
 * sweep：  调用方（App 启动 / 每条命令开头）清【已死 pid】的。 */
/* imgtool-tmp-<purpose>-<pid>-<n> -> extract pid.
 * Scan backwards for two dashes; the segment between them must be all digits.
 * Purpose tokens contain dashes (iso-wim / pe-core / win-finish) - handled. */
static bool tmp_name_pid(const char *name, long *pid){
    if(strncmp(name,"imgtool-tmp-",12)) return false;
    int len=(int)strlen(name);
    int dash1=-1, dash2=-1;
    for(int i=len-1;i>=12;i--){
        if(name[i]!='-') continue;
        if(dash1<0){ dash1=i; continue; }
        dash2=i; break;
    }
    if(dash1<0||dash2<0) return false;
    if(dash1-dash2<=1) return false;
    long v=0;
    for(int i=dash2+1;i<dash1;i++){
        if(name[i]<'0'||name[i]>'9') return false;
        v=v*10+(name[i]-'0');
    }
    *pid=v;
    return true;
}

static int g_tmpseq;
static bool g_tmp_atexit_done;

static void tmp_atexit_cleanup(void){
    const char *td = img_tmpdir();
    if(!td) return;
    DIR *d=opendir(td);
    if(!d) return;
    struct dirent *de;
    char victim[1200];
    while((de=readdir(d))){
        long pid=0;
        if(!tmp_name_pid(de->d_name,&pid)) continue;
        if(pid!=(long)getpid()) continue;
        snprintf(victim,sizeof victim,"%s/%s",td,de->d_name);
        struct stat st;
        if(stat(victim,&st)!=0) continue;
        if(S_ISDIR(st.st_mode)) host_rm_rf_dir(victim);   /* 见下 */
        else unlink(victim);
    }
    closedir(d);
}

const char *img_tmpfile(const char *purpose, char *out, size_t cap){
    const char *td = img_tmpdir();
    if(!td || !purpose || !*purpose || !out) return NULL;
    for(int i=0;i<4096;i++){
        snprintf(out,cap,"%s/imgtool-tmp-%s-%ld-%d",td,purpose,(long)getpid(),++g_tmpseq);
        int fd=open(out,O_CREAT|O_EXCL|O_WRONLY,0600);
        if(fd>=0){ close(fd); unlink(out);          /* 只是要个名字；调用方自己 open */
                   if(!g_tmp_atexit_done){ atexit(tmp_atexit_cleanup); g_tmp_atexit_done=true; }
                   return out; }
        if(errno!=EEXIST) return NULL;
    }
    return NULL;
}
int img_tmpfile_dir(const char *purpose, char *out, size_t cap){
    const char *td = img_tmpdir();
    if(!td || !purpose || !*purpose || !out) return IMG_EINVAL;
    char tmpl[1200];
    snprintf(tmpl,sizeof tmpl,"%s/imgtool-tmp-%s-%ld-XXXXXX",td,purpose,(long)getpid());
    if(!mkdtemp(tmpl)) return IMG_EIO;
    snprintf(out,cap,"%s",tmpl);
    if(!g_tmp_atexit_done){ atexit(tmp_atexit_cleanup); g_tmp_atexit_done=true; }
    return IMG_OK;
}

/* 递归删目录（img_types.c 里自己带一份，host_rm_rf 在 xcopy 是 static 的） */
static void host_rm_rf_dir(const char *path){
    DIR *d=opendir(path);
    if(!d){ unlink(path); return; }
    struct dirent *de;
    char sub[1200];
    while((de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        snprintf(sub,sizeof sub,"%s/%s",path,de->d_name);
        /* ★ REQ-69 ④：先 unlink 再判目录 —— 以前每个条目一次 lstat()，
         *   3.3 万个文件实测 6.8 秒（rm -rf 是 3.9 秒），取消后"卡 7 秒"就是它。*/
        if(unlink(sub)==0) continue;
        if(errno==EISDIR || errno==EPERM){
            if(rmdir(sub)==0) continue;
            host_rm_rf_dir(sub);
        }
    }
    closedir(d);
    rmdir(path);
}

/* ★ REQ-70 §3：带时间预算的删除。取消时先真删，但最多花 budget_ms；
 *   超时就把剩下的改名为 -garbage 交给 sweep —— 既不让用户等，也不让盘一直占着。*/
typedef struct { long long deadline; int timed_out; int ticks; } rmtd;
static long long rm_now_ms(void){
    struct timeval tv; gettimeofday(&tv,NULL);     /* 墙钟够用；不依赖 CLOCK_* */
    return (long long)tv.tv_sec*1000 + tv.tv_usec/1000;
}
static void host_rm_rf_dir_td(const char *path, rmtd *t){
    if(t->timed_out) return;
    DIR *d=opendir(path);
    if(!d){ unlink(path); return; }
    struct dirent *de;
    char sub[1200];
    while((de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,"..")) continue;
        if(++t->ticks >= 64){
            t->ticks = 0;
            if(rm_now_ms() > t->deadline){ t->timed_out=1; break; }
        }
        snprintf(sub,sizeof sub,"%s/%s",path,de->d_name);
        if(unlink(sub)==0) continue;
        if(errno==EISDIR || errno==EPERM){
            if(rmdir(sub)==0) continue;
            host_rm_rf_dir_td(sub,t);
            if(t->timed_out) break;
        }
    }
    closedir(d);
    if(!t->timed_out) rmdir(path);
}
int img_tmpdir_remove_td(const char *path, int budget_ms){
    if(!path || !*path) return IMG_EINVAL;
    /* ★ CodeQL cpp/toctou-race-condition：不做 lstat 预检 —— 检查和使用之间
     *   文件可能被调包。直接 O_NOFOLLOW|O_DIRECTORY 打开：是【真目录】才递归删；
     *   符号链接/普通文件直接 unlink（不跟随，语义同原来的 lstat 判断）。 */
    int fd = open(path, O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
    if(fd < 0){
        if(errno==ENOENT) return IMG_OK;
        if(errno==ELOOP || errno==ENOTDIR){ unlink(path); return IMG_OK; }
        return IMG_EIO;
    }
    close(fd);                                   /* 只用它做原子判定，删树走下面的路径版 */
    rmtd t;
    t.deadline = rm_now_ms() + (budget_ms > 0 ? budget_ms : 0);
    t.timed_out = 0; t.ticks = 0;
    host_rm_rf_dir_td(path,&t);
    return t.timed_out ? IMG_ETIMEDOUT : IMG_OK;
}

/* ★ REQ-69 ④：取消时【不】同步删树 —— 33,000 个文件要 6.8 秒，用户看起来就是"点了没反应"。
 * 改个名字（O(1)）交给 img_tmpdir_sweep() 下次清；名字带 "garbage"，sweep 见到就不看 pid 死活。*/
int img_tmpdir_discard(const char *path){
    if(!path || !*path) return IMG_EINVAL;
    /* ★ CodeQL cpp/toctou-race-condition：原来的 lstat 预检只用来判 ENOENT，
     *   和后面的 rename 之间有窗口。直接 rename —— rename 本身就是原子的，
     *   ENOENT（已经没了）按成功处理，语义与原来一致。 */
    char dst[1300];
    if(snprintf(dst,sizeof dst,"%s-garbage",path) >= (int)sizeof dst)
        return img_tmpdir_remove(path);
    if(rename(path,dst)==0) return IMG_OK;
    if(errno==ENOENT) return IMG_OK;
    return img_tmpdir_remove(path);          /* 改名失败（不该发生）→ 走老路 */
}

/* 递归删除临时目录（公开给 wim.capture / recovery 用，替代 system("rm -rf")）。*/
int img_tmpdir_remove(const char *path){
    if(!path || !*path) return IMG_EINVAL;
    /* ★ CodeQL cpp/toctou-race-condition：不做 lstat 预检。先 unlink——
     *   文件/符号链接一步到位；目录会报 EISDIR → 走递归删。
     *   与 host_rm_rf_dir 内部「先 unlink 再判目录」是同一套口径（REQ-69 ④）。 */
    if(unlink(path)==0) return IMG_OK;
    if(errno==ENOENT) return IMG_OK;
    if(errno==EISDIR || errno==EPERM){
        host_rm_rf_dir(path);
        return IMG_OK;
    }
    return IMG_EIO;
}

/* sweep 一次目录（内部共用） */
static int tmp_sweep_one(const char *td){
    if(!td || !*td) return 0;
    DIR *d=opendir(td);
    if(!d) return 0;
    int removed=0;
    struct dirent *de;
    char victim[1200];
    while((de=readdir(d))){
        /* -garbage 结尾：上一次"取消后待清"留下的，不看 pid 死活，直接删 */
        char base[256];
        snprintf(base,sizeof base,"%s",de->d_name);
        size_t bl=strlen(base);
        bool garbage=false;
        if(bl>8 && !strcmp(base+bl-8,"-garbage")){ base[bl-8]=0; garbage=true; }
        long pid=0;
        if(!tmp_name_pid(garbage?base:de->d_name,&pid)) continue;
        if(!garbage && pid==(long)getpid()) continue;
        if(!garbage && pid_is_alive(pid)) continue;   /* 别人的活进程不动 */
        snprintf(victim,sizeof victim,"%s/%s",td,de->d_name);
        struct stat st;
        if(lstat(victim,&st)!=0) continue;
        if(S_ISDIR(st.st_mode)) host_rm_rf_dir(victim);
        else unlink(victim);
        removed++;
    }
    closedir(d);
    return removed;
}
/* ★ sweep 遍历【所有候选】而不是 img_tmpdir() 的缓存结果：
 *   hint 在命令解析时才设置，而 img_tmpdir() 有 static 缓存——
 *   第一条命令 sweep 时 hint 还没来。直接把每个可能的目录都扫一遍，
 *   哪个目录里藏着死 pid 的 imgtool-tmp-* 都能收走。 */
int img_tmpdir_sweep(void){
    int total=0;
    const char *cand[6]; int nc=0;
    const char *e1=getenv("IMGTOOL_TMPDIR"), *e2=getenv("TMPDIR");
    if(e1 && *e1) cand[nc++]=e1;
    if(e2 && *e2) cand[nc++]=e2;
    cand[nc++]="/tmp";
    for(int i=0;i<nc;i++) total += tmp_sweep_one(cand[i]);
    return total;
}

/* REQ-61 §3：临时目录所在卷的剩余空间（sys.tmpdir 用）。*/
int img_tmpdir_free_bytes(uint64_t *out_free, uint64_t *out_total){
    if(out_free) *out_free=0;
    if(out_total) *out_total=0;
    const char *td = img_tmpdir();
    if(!td || !*td) return IMG_ESTATE;
    struct statvfs vb;
    if(statvfs(td,&vb)!=0) return IMG_EIO;
    uint64_t bs=(uint64_t)vb.f_frsize ? (uint64_t)vb.f_frsize : (uint64_t)vb.f_bsize;
    if(out_free)  *out_free  = (uint64_t)vb.f_bavail * bs;
    if(out_total) *out_total = (uint64_t)vb.f_blocks * bs;
    return IMG_OK;
}
