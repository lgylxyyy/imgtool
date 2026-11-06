/* img_archive_w.h — 压缩包【写】端（REQ-39 archive.create / REQ-41 写进压缩包）
 *
 * 能写：zip / 7z / tar / tar.gz / iso9660（level 3 + Joliet）。**不能写 rar**（专有格式，
 * libarchive 只有读端）—— 调用方必须在开写之前就拒绝，不能写一半再报错。
 *
 * 为什么是"整包重写"：
 *   · zip 的中央目录在文件末尾，追加要重排；
 *   · 7z / tar 是顺序格式，中间插一条必须重写；
 *   · iso 是"一张盘写一次"的布局（目录区在前、数据区在后），libarchive 也只能顺序生成。
 *   所以统一做法：临时文件 → 旧条目原样搬过去（能跳过的跳过）→ 追加新条目 → fsync → rename。
 *   失败/取消 → 临时文件删掉，**原包一个字节都不动**（REQ-41 验收 3）。
 *
 * 元数据：条目名（UTF-8）、大小、mtime 保住；zip 注释 / 属主 / ACL 保不住（调用方在
 * warnings[] 里说明，别静默丢）。
 */
#ifndef IMG_ARCHIVE_W_H
#define IMG_ARCHIVE_W_H
#include "img_types.h"
#include "img_progress.h"
#include "img_stream.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_AW_NONE = 0,
    IMG_AW_ZIP,
    IMG_AW_7Z,
    IMG_AW_TAR,
    IMG_AW_TGZ,
    IMG_AW_ISO
} img_aw_fmt;

const char  *img_aw_fmt_name(img_aw_fmt f);
/* 按扩展名定格式（.zip/.7z/.tar/.tgz/.tar.gz/.iso）；认不出返回 IMG_AW_NONE。 */
img_aw_fmt   img_aw_fmt_by_name(const char *path);
/* 已存在的包：按【魔数】定格式（扩展名会骗人）。rar/cab 返回 IMG_ENOTSUP + 人话。 */
int          img_aw_fmt_of_existing(const char *path, img_aw_fmt *out,
                                    char *err, size_t errcap);
/* 这个格式能不能写。rar/cab 不能。 */
bool         img_aw_writable(img_aw_fmt f);

typedef struct img_aw img_aw;

/* 建临时包。take_existing=true 时把 dst 现有条目搬过来（dst 不存在就当新建）。
 * skip[]：搬旧条目时跳过这些路径（skip_subtree=true 连子树一起）+ 不记进名字表。
 *   —— 覆盖写要先把"将被新内容替换"的旧条目排掉，否则包里会出现两条同名。*/
int  img_aw_begin(const char *dst, img_aw_fmt fmt, int level,
                  const char *iso_volume, bool iso_joliet,
                  bool take_existing,
                  const char *const *skip, int n_skip, bool skip_subtree,
                  img_aw **out, char *err, size_t errcap);

/* 只读名字表（计划覆盖/移动之前用：先知道包里有什么，再决定跳过谁）。
 * 不存在 → IMG_ENOENT。 */
typedef struct img_aw_names img_aw_names;
int  img_aw_names_load(const char *path, img_aw_names **out, char *err, size_t errcap);
bool img_aw_names_has(const img_aw_names *ns, const char *inner);
int  img_aw_names_lookup(const img_aw_names *ns, const char *inner, bool *isdir);
uint64_t img_aw_names_count(const img_aw_names *ns);
uint64_t img_aw_names_count_children(const img_aw_names *ns, const char *dir);
void img_aw_names_free(img_aw_names *ns);

/* 旧包/已写条目里有没有这个路径（路径 / 开头，大小写敏感 —— zip 是区分大小写的）*/
bool img_aw_has(const img_aw *w, const char *inner);
/* 1=存在（*isdir 回类型）/0=不存在 */
int  img_aw_lookup(const img_aw *w, const char *inner, bool *isdir);
/* 目录下直接子项数（给 UI 的 parent_total）*/
uint64_t img_aw_count_children(const img_aw *w, const char *dir);
uint64_t img_aw_entries(const img_aw *w);

int  img_aw_add_dir(img_aw *w, const char *inner, uint64_t mtime,
                    char *err, size_t errcap);
/* 从 stream 顺序读 size 字节写进包。io_done/io_total 是【跨文件累计】进度，可为 NULL。*/
int  img_aw_add_file(img_aw *w, const char *inner, const img_stream *st, uint64_t size,
                     uint64_t mtime, img_prog *prog,
                     uint64_t *io_done, uint64_t io_total,
                     char *err, size_t errcap);

/* 收尾：写中央目录/卷描述符 → fsync → rename 到 dst。成功后 w 被释放。*/
int  img_aw_commit(img_aw *w, uint64_t *out_size, char *err, size_t errcap);
/* 取消/失败：关掉 + 删临时文件（原包不动）。w 被释放。*/
void img_aw_abort(img_aw *w);

/* 把宿主文件塞进包（archive.create 用；内部 1 MiB 一块读）。*/
int  img_aw_add_host_file(img_aw *w, const char *inner, const char *host_path,
                          uint64_t size, uint64_t mtime, img_prog *prog,
                          uint64_t *io_done, uint64_t io_total,
                          char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif