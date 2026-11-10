/* img_du.h — 一条命令算"文件/文件夹递归大小与项数"（REQ-40）
 *
 * 口径（写死，App 与 UI 都按这个显示，改之前先改文档）：
 *   files      = 递归到的【文件】个数
 *   dirs       = 递归到的【子目录】个数（不含 --src 自己）
 *   total_bytes= 这些文件的 size 之和
 *   alloc_bytes= 有就给（NTFS 的簇对齐大小），没有给 0（不做假）
 *   scanned    = 实际看过多少条目（给进度用）
 * 超过 limit → truncated=true + 已统计值，**不报错**（用户看到"只算了前 N 项"比错误码好）。
 *
 * --src 是统一路径（host: / qcow2!p3: / iso! / wim! / 压缩包!），一处调用覆盖五类。
 */
#ifndef IMG_DU_H
#define IMG_DU_H
#include "img_types.h"
#include "img_path.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t total_bytes;
    uint64_t alloc_bytes;
    uint64_t scanned;
    uint64_t excluded;     /* 被 --exclude 规则排除的条目数（REQ-67）*/
    bool     truncated;
} img_du_stats;

/* ★ 2026-11-10（REQ-67）：估算与实捕必须同口径，否则又是"算 11.7、实捕 7.8"。
 *   exclude 是 basename 通配，语义与 copy / wim.capture 完全相同
 *   （img_name_match_any：无通配时大小写不敏感）。*/
typedef struct {
    const char *const *exclude;
    int  n_exclude;
} img_du_opts;

/* limit=0 → 不限制；max_depth=0 → 不限制；follow=false 不跟随符号链接。*/
int img_du_run(const img_path *p, uint64_t limit, int max_depth, bool follow,
               img_prog *prog, img_du_stats *st, char *err, size_t errcap);

/* 带排除规则的版本（o 可为 NULL，等价于 img_du_run）。*/
int img_du_run_ex(const img_path *p, uint64_t limit, int max_depth, bool follow,
                  const img_du_opts *o, img_prog *prog, img_du_stats *st,
                  char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif