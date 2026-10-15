/* img_lock.c — 锁实现（POSIX 用 pthread；Windows 换 SRWLOCK 只改这里）*/
#define _POSIX_C_SOURCE 200809L
#include "img_lock.h"
#include <stdlib.h>
#include <string.h>

int img_lock_init(img_lock_t *l){
    if(!l) return IMG_EINVAL;
#ifdef _WIN32
    return IMG_ENOTSUP;            /* 待 Windows 移植时实现 */
#else
    return pthread_mutex_init(l,NULL)==0 ? IMG_OK : IMG_EIO;
#endif
}
void img_lock_destroy(img_lock_t *l){
    if(!l) return;
#ifndef _WIN32
    pthread_mutex_destroy(l);
#endif
}
void img_lock_acquire(img_lock_t *l){
    if(!l) return;
#ifndef _WIN32
    pthread_mutex_lock(l);
#endif
}
void img_lock_release(img_lock_t *l){
    if(!l) return;
#ifndef _WIN32
    pthread_mutex_unlock(l);
#endif
}

/* 进程级全局锁：懒初始化（C11 起局部 static 初始化是线程安全的，但为兼容
 * 更老的编译器这里用一次性初始化）*/
static img_lock_t g_global;
static int g_global_state;        /* 0=未初始化 1=好 */
img_lock_t *img_global_lock(void){
    if(!g_global_state){
        if(img_lock_init(&g_global)!=IMG_OK) return NULL;
        g_global_state=1;
    }
    return &g_global;
}
