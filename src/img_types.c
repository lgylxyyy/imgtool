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
#include <string.h>
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
