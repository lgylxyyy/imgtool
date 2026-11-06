/* img_iso.h — ISO/UDF 只读访问（抽 install.wim / boot.sdi 用）
 *
 * 为什么用 libudfread 而不是自己解析：
 *   Windows 安装 ISO 是 **UDF bridge** —— ISO9660 目录树里往往只有一个 README，
 *   真正的 sources/install.wim 只在 UDF 里（libarchive 至今不支持 UDF，
 *   见 libarchive/libarchive#1036）。libudfread 是 VideoLAN 的纯 C 只读 UDF 实现
 *   （LGPL-2.1），并且允许把【自定义块设备回调】接进去 —— 所以它同样适用于
 *   Android SAF 的 fd（我们只需要提供 read(lba)/size 两个回调）。
 *
 * 层归属（DESIGN-OUTPUT.md §十一）：ISO 抽取归**工具**，App 只下命令。
 *   App 不再需要自己找 ISO 库、也不需要有"执行文件操作"的权限。
 */
#ifndef IMG_ISO_H
#define IMG_ISO_H
#include "img_types.h"
#include "img_io.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_iso img_iso;

/* 回调：列目录用。name 是 MUTF-8（普通 ASCII 名字就是原样）*/
/* mtime：unix 秒 UTC；ISO9660 取自目录记录的 7 字节时间，UDF 取自文件的 FE。
 * 取不到给 0，**不报错**（REQ-41 ②）。*/
typedef void (*img_iso_entry_cb)(void *ud, const char *name, bool is_dir,
                                 uint64_t size, uint64_t mtime);

/* 打开镜像。io 由调用方持有（本模块不关闭它）；path 版内部自己开/关。
 * 失败返回负错误码，err 里放人话（可为 NULL）。*/
int  img_iso_open_io(img_iso **out, img_io *io, char *err, size_t errcap);
int  img_iso_open_path(img_iso **out, const char *path, char *err, size_t errcap);
void img_iso_close(img_iso *iso);

/* 卷标（UDF Volume Identifier）；没有返回 NULL */
const char *img_iso_volume_id(img_iso *iso);

/* 列目录。path 以 / 开头（NULL/"" 视为根）。大小写不敏感查找。
 * 成功返回条目数（>=0）。*/
int img_iso_list(img_iso *iso, const char *path, img_iso_entry_cb cb, void *ud);

/* 查一个路径。存在返回 IMG_OK 并填 size/is_dir；不存在返回 IMG_ENOENT。*/
int img_iso_stat(img_iso *iso, const char *path, uint64_t *size, bool *is_dir);

/* 把文件抽到本地 dst（流式，1 MiB 一块；不整文件进内存）。
 * max_bytes>0 时只抽前 max_bytes（自测/预览用）。out_bytes 返回实际写出字节数。*/
/* 读文件的一段（不落盘）：给"只读 WIM 头/XML"此类场景用 */
int img_iso_read_range(img_iso *iso, const char *path, uint64_t off, void *buf, size_t len);

int img_iso_extract(img_iso *iso, const char *path, const char *dst,
                    uint64_t max_bytes, img_prog *prog, uint64_t *out_bytes);

/* 实际走的是哪条路（REQ-45：崩溃/空目录时 App 能报"崩在 udf 还是 iso9660"）*/
const char *img_iso_kind(img_iso *iso);   /* "udf" / "joliet" / "iso9660" / "?" */

/* 只做"打开 + 读卷描述符"，不列目录（两段式预检；REQ-45 §3）。
 * 数据结构和目录列表无关，几个块就够。*/
typedef struct {
    bool     udf;          /* 走了 UDF（Windows 安装盘）*/
    bool     joliet;       /* ISO9660 + Joliet */
    bool     iso9660;      /* 纯 ISO9660 */
    uint32_t lb_size;      /* 逻辑块大小（通常 2048）*/
    uint64_t root_lb;      /* UDF 根目录 FE 的 lb（分区内）*/
    uint32_t iso_root_lba; /* ISO9660 根目录扇区 */
    uint32_t iso_root_len;
    char     volume[128];
} img_iso_probe_t;
int img_iso_probe_path(const char *path, img_iso_probe_t *out, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif
