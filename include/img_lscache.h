/* img_lscache.h — 目录会话缓存（listing session，2026-11-18）
 *
 * 为什么要有它：文件页「飞速往下翻」的前提是翻页不重开容器。
 *   以前 wim.ls 每条命令都独立打开 ISO → 定位 WIM → 虚拟挂载 → 遍历到 offset，
 *   每页 100-300ms 且越翻越慢（遍历 O(offset)）。
 *   会话把「一次遍历全目录」的结果缓存在进程内存里，翻页变成 O(1) 切片渲染
 *   （500 条 ≈ 5-10ms），搜索也能在缓存上做（5000 条 < 5ms）。
 *
 * 生命周期：
 *   · 建会话 → 拿到 token（`ls-<pid>-<seq>`）
 *   · 每条命令入口调用 img_lsc_gc()：清理超过 IMG_LSC_TTL_MS 未使用的会话
 *   · token 不存在/过期 → IMG_ENOENT（调用方报 TOKEN-EXPIRED，App 重开一次即可）
 *   · 进程退出 atexit 清理
 *
 * 内存：会话总量上限 IMG_LSC_MEM_CAP（64MB）；超了 img_lsc_open 失败，
 *   调用方退回「每次遍历」的老路径（cached=false），不会炸内存。
 */
#ifndef IMG_LSCACHE_H
#define IMG_LSCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一条目录项（建会话时由调用方填 path；path 会被拷贝）。 */
typedef struct {
    char    *path;      /* 完整路径（如 /Windows/System32/aaa.dll） */
    uint64_t size;
    uint64_t mtime;
    uint32_t attrs;
    uint32_t depth;     /* 相对会话根目录的深度（0 = 直接子项） */
    bool     isdir;
} img_lsc_ent;

#define IMG_LSC_MAX_SESS 8
#define IMG_LSC_TTL_MS   (60 * 1000)
#define IMG_LSC_MEM_CAP  (64u * 1024u * 1024u)

/**
 * 建会话。ents[0..n) 是遍历到的全部条目（内部按值复制，path 用 strdup）。
 * truncated=true 表示调用方因条目上限提前停了（会话仍可用，total 就是 n）。
 *
 * @param token 输出：会话 id（至少 32 字节）
 * @return IMG_OK / IMG_ENOMEM（超内存上限或分配失败 → 调用方走老路径）
 */
int img_lsc_open(const img_lsc_ent *ents, size_t n, bool truncated,
                 char *token, size_t tokcap);

/**
 * 翻页渲染：把 [offset, offset+limit) 的条目渲染成 JSON 数组写入 json（含首尾 []）。
 * @return IMG_OK；IMG_ENOENT = token 无效/过期（App 应重开一次）
 */
int img_lsc_page(const char *token, uint64_t offset, uint64_t limit,
                 char *json, size_t cap,
                 uint64_t *out_total, uint64_t *out_count, bool *out_has_more);

/**
 * 搜索：在会话缓存上做【不区分大小写的子串】匹配（path 全路径），结果分页渲染。
 * 5000 条量级 < 5ms（内存扫描），这是「输入即搜」的前提。
 * @return IMG_OK；IMG_ENOENT = token 无效/过期
 */
int img_lsc_search(const char *token, const char *q, uint64_t offset, uint64_t limit,
                   char *json, size_t cap,
                   uint64_t *out_total, uint64_t *out_count, bool *out_has_more);

/** 清理过期会话（每条命令入口调一次；便宜，无后台线程）。 */
void img_lsc_gc(void);

/** 释放全部会话（atexit 注册；也可手动调）。 */
void img_lsc_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* IMG_LSCACHE_H */