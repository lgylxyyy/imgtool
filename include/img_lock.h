/* img_lock.h — 极小的锁抽象（只为保护"库内全局状态"）
 *
 * 为什么需要：库里有进程级全局状态（日志器 G、环形缓冲、NTFS 前插上下文），
 *   App 多线程并发调用会互相踩 → 症状是"莫名其妙的数据错/崩溃"，最难查。
 * 契约（见 DESIGN-OUTPUT.md 第八节）：
 *   · 同一个 img_ctx / img_qcow2 句柄：一次只跑一个操作（img_cmd_exec 已内建串行化）
 *   · 不同镜像（不同句柄）可并行 —— 各模块的句柄状态是私有的
 *   · 日志/环形缓冲内部自带锁，可以随便并发写
 * 移植：Windows 上换成 SRWLOCK/CRITICAL_SECTION（只改这一个文件）。
 */
#ifndef IMG_LOCK_H
#define IMG_LOCK_H
#include "img_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
typedef struct img_lock { void *p; } img_lock_t;
#else
#include <pthread.h>
typedef pthread_mutex_t img_lock_t;
#endif

int  img_lock_init(img_lock_t *l);      /* 失败返回 IMG_EIO */
void img_lock_destroy(img_lock_t *l);
void img_lock_acquire(img_lock_t *l);
void img_lock_release(img_lock_t *l);

/* 进程级全局锁（日志/环形缓冲等共享状态用它） */
img_lock_t *img_global_lock(void);

#ifdef __cplusplus
}
#endif
#endif
