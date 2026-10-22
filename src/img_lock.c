/* img_lock.c — 锁实现（POSIX 用 pthread；Windows 换 SRWLOCK 只改这里）*/
#define _POSIX_C_SOURCE 200809L
#include "img_lock.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
int img_lock_destroy_safe(img_lock_t *l){
    if(!l) return IMG_EINVAL;
#ifdef _WIN32
    return IMG_ENOTSUP;            /* Windows 移植时用 TryEnterCriticalSection 实现 */
#else
    int r=pthread_mutex_trylock(l);
    if(r!=0) return (r==EBUSY)?IMG_EAGAIN:IMG_EIO;   /* 仍被持有 → 绝不 destroy */
    pthread_mutex_unlock(l);
    pthread_mutex_destroy(l);
    return IMG_OK;
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

/* 进程级全局锁 —— ★ 必须用【一次性初始化】原语，不能用"标志位+普通赋值"的懒初始化。
 * 旧写法 `if(!g_global_state){ img_lock_init(&g_global); g_global_state=1; }` 本身是竞态：
 *   两个线程可同时看到未初始化 → 对【同一把】mutex 执行第二次 pthread_mutex_init；
 *   若此刻第三个线程正持锁，第二次 init 会把 __data.__owner 清零 →
 *   ①glibc 断言 abort（pthread_mutex_lock.c:94: mutex->__data.__owner == 0）
 *   ②或锁彻底失效（两个线程同时进临界区 → 日志半行、环形缓冲踩内存）。
 * 而且 g_global_state 是无屏障的普通 int：可能"看到已初始化但 init 还没生效"。
 * POSIX 用 pthread_once；Windows 移植用 InitOnceExecuteOnce（只改这个文件）。*/
static img_lock_t g_global;
#ifdef _WIN32
#include <windows.h>
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static int g_bad;
static BOOL CALLBACK once_cb(PINIT_ONCE io,PVOID c,PVOID *rv){
    (void)io;(void)c;(void)rv;
    if(img_lock_init(&g_global)!=IMG_OK){ g_bad=1; return FALSE; }
    return TRUE;
}
img_lock_t *img_global_lock(void){
    if(!InitOnceExecuteOnce(&g_once,once_cb,NULL,NULL)) return NULL;
    return g_bad?NULL:&g_global;
}
#else
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_bad;                 /* init 失败过一次就别再返回半死的锁 */
static void once_init(void){ if(img_lock_init(&g_global)!=IMG_OK) g_bad=1; }
img_lock_t *img_global_lock(void){
    pthread_once(&g_once,once_init);
    return g_bad?NULL:&g_global;
}
#endif
