#include "img_types.h"

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
    case IMG_ERANGE:     return "请求范围越界（偏移/长度超出镜像）";
    case IMG_ECYCLE:     return "会造出目录环（搬进自己或自己的子树）";
    case IMG_ESAMEFILE:  return "源和目标是同一个文件";
    case IMG_ENAMETOOLONG:return "文件名超过长度上限";
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
const char *img_tmpdir(void){
    static char cached[512];
    if(cached[0]) return cached;
    const char *cand[4]; int nc = 0;
    const char *e1 = getenv("IMGTOOL_TMPDIR"), *e2 = getenv("TMPDIR");
    if(e1 && *e1) cand[nc++] = e1;
    if(e2 && *e2) cand[nc++] = e2;
    cand[nc++] = "/tmp";
    cand[nc++] = ".";
    for(int i = 0; i < nc; i++){
        size_t L = strlen(cand[i]);
        while(L && cand[i][L-1] == '/') L--;          /* 去掉尾巴上的 /，免得拼出 // */
        if(!L) L = 1;                                  /* 根目录本身 */
        if(L >= sizeof cached - 16) continue;
        memcpy(cached, cand[i], L); cached[L] = 0;
        if(access(cached, W_OK | X_OK) == 0) return cached;
        cached[0] = 0;
    }
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
