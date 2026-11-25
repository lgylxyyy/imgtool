/* img_lswalk.h — 统一路径上的「递归遍历 + 子串过滤」（REQ-83，2026-11-19）
 *
 * 为什么单独一层：App 要「任何地方搜任何关键字都能搜全」，而每类容器
 * （宿主目录 / qcow2 里的 NTFS·FAT / ISO / WIM / 压缩包）原来各有各的
 * 列目录 API、各有各的递归写法。搜索如果按容器各写一遍，口径必然漂移
 * （total 是过滤前还是过滤后、has_more 怎么算、大小写敏不敏感…）。
 *
 * 所以：**遍历与过滤只写这一份**，命令层（img.search）只负责分页与序列化。
 * 后端六类共用一套语义：
 *   · path 一律是「容器内绝对路径」（宿主目录则是绝对宿主路径），/ 开头；
 *   · contains 匹配整条 path，**不区分大小写**；
 *   · recursive=false 只列一层；true 走整棵子树；
 *   · 深度上限由 path 长度与后端自身兜底，这里不再加一层（防病态路径靠 2048 截断）。
 *
 * 取消：内部检查 img_cancelled() → IMG_ECANCELLED（App 取消搜索时用）。
 * 进度：调用方传 prog 即可看到「正在扫哪个目录」；本层不自己建进度器。
 */
#ifndef IMG_LSWALK_H
#define IMG_LSWALK_H
#include "img_types.h"
#include "img_path.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     path[2048];   /* 完整路径（容器内 / 开头；宿主则是绝对路径） */
    bool     is_dir;
    uint64_t size;
    uint64_t mtime;        /* unix 秒；取不到给 0（不做假） */
    uint32_t depth;        /* 相对 spec：spec 自己=0，直接子项=1 */
} img_lswalk_ent;

typedef void (*img_lswalk_cb)(void *ud, const img_lswalk_ent *e);

/* 遍历 p（已由 img_path_parse 解析好）。
 *   recursive=false → 只列 p->inner（HOST 则是 p->host）这一层；
 *   q=NULL/空串  → 不过滤；否则不区分大小写子串匹配 e.path 后才回调；
 *   prog 可为 NULL。
 * 返回：IMG_OK / IMG_ECANCELLED / 后端错误码。err 里填人话说明（可 NULL）。
 *
 * ★ 如果 p 里有 ISO 内 WIM 的临时抽取文件，调用方用完要 img_path_cleanup(p)，
 *   本层不动它（与 img_du 同一约定）。 */
int img_lswalk_run(const img_path *p, bool recursive, const char *q,
                   img_lswalk_cb cb, void *ud, img_prog *prog,
                   char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif