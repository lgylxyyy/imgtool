/* img_rar.h — RAR 读取（REQ-50）：solid / RAR5 / 加密检测
 *
 * 为什么不用 libarchive：它的 RAR 解码只覆盖"能流式解"的子集，solid 包直接
 *   "RAR solid archive support unavailable"（用户那个 221 MB 的 CS1.6 包就是）。
 * 这里内置 unrar 官方解码器（third_party/unrar，UnRAR license：可自由分发，
 *   但不能用来做 RAR 压缩器；我们不提供 RAR 写端）。
 *
 * 接口只做三件事：扫头（列目录）、按条目取数据（流式）、整棵抽取。
 * 所有入口都吃"包内路径"（/ 开头），与 img_archive 的规范一致。
 */
#ifndef IMG_RAR_H
#define IMG_RAR_H
#include "img_types.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     path[512];
    bool     is_dir;
    uint64_t size;
    uint64_t packed;
    uint64_t mtime;      /* unix 秒；取不到 0 */
    bool     solid;      /* 该条目在固实块里（解压要从前面的条目顺序解）*/
    bool     encrypted;
} img_rar_ent;

typedef int (*img_rar_ent_cb)(void *ud, const img_rar_ent *e);
typedef int (*img_rar_data_cb)(void *ud, const void *buf, size_t n);

/* 魔数判断（不认扩展名）：Rar!\x1a\x07\x00 / Rar!\x1a\x07\x01\x00 */
bool img_rar_is_rar(const char *path);

/* 扫全部头（不解压）。*out_solid 回"这个包是否固实"。 */
int img_rar_scan(const char *path, img_rar_ent_cb cb, void *ud,
                 bool *out_solid, char *err, size_t errcap);

/* 单条查询（找不到 IMG_ENOENT；目录的 size 回 0）。 */
int img_rar_stat(const char *path, const char *inner, bool *isdir,
                 uint64_t *size, uint64_t *mtime, char *err, size_t errcap);

/* 把一个条目的内容流式吐给 cb（solid 会自动顺序解压前面的条目）。 */
int img_rar_read(const char *path, const char *inner,
                 img_rar_data_cb cb, void *ud, uint64_t *out_bytes,
                 img_prog *prog, char *err, size_t errcap);

/* 把条目（文件或整棵目录）抽到宿主 dst。*out_files 回文件数。 */
int img_rar_extract(const char *path, const char *inner, const char *dst,
                    img_prog *prog, uint64_t *out_bytes, int *out_files,
                    char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif