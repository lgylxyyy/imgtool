/* img_archive.h — 压缩包只读（REQ-31）
 *
 * 只做两件事：列目录、取文件。**绝不写回包**（写操作在命令层直接 ENOTSUP）。
 * 合作者：libarchive（format_all + filter_all），zip/apk/jar/7z/rar/tar.{gz,xz,bz2}/cab 全覆盖。
 *
 * 为什么要有"索引"：7z/rar 是流式格式，取第 1000 个条目也得从第 1 个扫起。
 * App 点进子目录时不想每次都付这个钱，所以：
 *   · 首次 archive.ls → 全量扫一遍并缓存，返回 index_token；
 *   · 续页/换目录用 token（不重扫），按前缀在内存里过滤；
 *   · 包变了（dev+ino+mtime+size 任一变化）/超时/被淘汰 → IMG_ESTALE，App 重来一次。
 * 生命周期按 REQ-34 §3：idle 10 分钟、最多 2 个槽（LRU）、总条目上限 20 万。
 */
#ifndef IMG_ARCHIVE_H
#define IMG_ARCHIVE_H
#include "img_types.h"
#include "img_progress.h"
#include "img_stream.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     path[512];      /* 包内绝对路径，/ 开头（与 wim.ls / iso.ls 一致）*/
    bool     is_dir;
    uint64_t size;
    uint64_t mtime;          /* unix 秒；没有就 0 */
    int      children_count; /* 目录：直接子项数；文件：-1 */
} img_archive_ent;
typedef void (*img_archive_ent_cb)(void *ud, const img_archive_ent *e);

/* 按魔数/扩展名认格式，给 App 显示用（"zip"/"7z"/"rar"/"tar"/"cab"/"gzip"/"xz"…）*/
int img_archive_format(const char *path, char *out, size_t cap, char *err, size_t errcap);

/* ---------- 索引（token 缓存） ---------- */
typedef struct img_archive_index img_archive_index;

/* 全量扫一遍（可能几百毫秒~几秒）。成功后索引进缓存，token 可用。*/
int img_archive_index_build(const char *path, img_archive_index **out,
                            char *err, size_t errcap);
/* 用 token 取回（identity 变了 → IMG_ESTALE）*/
int img_archive_index_by_token(const char *token, img_archive_index **out,
                               char *err, size_t errcap);
/* 用 token 前先看包还在不在（返回 IMG_OK 表示仍然有效）*/
int img_archive_index_check(const char *token, char *err, size_t errcap);

const char *img_archive_index_token(const img_archive_index *ix);
const char *img_archive_index_format(const img_archive_index *ix);
uint64_t    img_archive_index_total(const img_archive_index *ix);
bool        img_archive_index_truncated(const img_archive_index *ix);
/* RAR 固实包？App 用来标"解压会慢一点"（REQ-50）*/
bool        img_archive_index_solid(const img_archive_index *ix);
/* 按前缀走条目（prefix 形如 "/x" 或 "/"；recursive=false 只回直接子项）*/
int img_archive_index_walk(img_archive_index *ix, const char *prefix, bool recursive,
                           img_archive_ent_cb cb, void *ud);
/* 用完还给缓存（不释放数据；缓存自己按 LRU 淘汰）*/
void img_archive_index_release(img_archive_index *ix);

/* ---------- 抽取（不需要索引） ----------
 * inner 是包内路径（/ 开头）。指向目录时整棵子树抽到 dst（目录必须不存在或为空）。
 * 单个文件时 dst 是文件路径。0 临时文件（libarchive 流式读 → 直接写目标）。*/
int img_archive_extract(const char *path, const char *inner, const char *dst,
                        img_prog *prog, uint64_t *out_bytes, int *out_files,
                        char *err, size_t errcap);

/* 把条目内容流式读进回调（给 img_path / copy 用）。cb 返回非 0 中止。*/
typedef int (*img_archive_write_cb)(void *ud, const void *buf, size_t n);
int img_archive_read_entry(const char *path, const char *inner, bool is_dir,
                           img_archive_write_cb cb, void *ud,
                           uint64_t *out_bytes, img_prog *prog,
                           char *err, size_t errcap);

/* 单条查询（给 copy 用）：isdir/size/mtime。缺点是要扫一遍包（7z/rar 没有随机访问）。*/
int img_archive_stat(const char *path, const char *inner, bool *isdir,
                     uint64_t *size, uint64_t *mtime, char *err, size_t errcap);

/* 把包内单个文件变成【顺序流】（img_stream）。copy 用：源侧按 off 递增读、目标侧边读边写。
 * 内部是"生产者线程读包 → 管道 → 调用方读"，所以是 0 临时文件。用完必须 close。*/
int img_archive_stream_open(const char *path, const char *inner,
                            img_stream *out, uint64_t *size,
                            img_prog *prog, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif
