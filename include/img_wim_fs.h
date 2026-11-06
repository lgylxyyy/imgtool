/* img_wim_fs.h — WIM/ESD 内部【只读】访问：列目录树 + 抽单个文件
 *
 * 只读承诺：本头里没有、也不会加任何"写回 WIM"的接口 —— WIM/ESD 是安装源，
 * 源被改动就等于素材报废（与 part.copy 的"源镜像只读打开"同一类纪律）。
 *
 * 与 img_identify.c 的分工：identify 只要 WIM 头+XML（任何 reader 可喂，含 SAF fd、
 * ISO 内偏移）；浏览/抽取要解压与去重，只能交给 wimlib，而 wimlib 只吃真实文件路径
 * → ISO 内的 install.wim 必须先复制成本地文件（命令层会把这件事的代价如实报出来）。
 */
#ifndef IMG_WIM_FS_H
#define IMG_WIM_FS_H
#include "img_types.h"
#include "img_stream.h"
#include "img_progress.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 目录条目回调。
 * name      : 条目名（不含父路径）
 * full_path : WIM 内绝对路径（以 / 开头，WIM 内部约定）
 * size      : 未命名数据流大小；目录为 0
 * attributes: 标准 Windows FILE_ATTRIBUTE_* 值（原样透出，App 想显示 hidden/system 就用）
 * depth     : 相对【映像根】的深度（不是相对查询目录 —— 老坑，别再按 1 判断）*/
typedef void (*img_wim_ent_cb)(void *ud, const char *name, const char *full_path,
                               bool is_dir, uint64_t size, uint64_t mtime,
                               uint32_t attributes, size_t depth);

typedef struct {
    bool     exists;
    bool     is_dir;
    uint64_t size_bytes;
    uint32_t attributes;
} img_wim_stat_t;

/* 列目录。recursive=false 只回直接子项（走 wimlib 的 CHILDREN 标志）。
 * 返回 IMG_OK / IMG_ENOENT（目录不存在）/ IMG_EINVAL（索引越界、路径不以 / 开头）/ IMG_EIO。*/
int img_wim_list(const char *wim_path, int index, const char *dir, bool recursive,
                 img_wim_ent_cb cb, void *ud, char *err, size_t errcap);

/* 查映像内的一个路径（是文件还是目录、多大）。
 * wimlib 没有单条 stat API → 内部列父目录找名字（大小写不敏感）。*/
int img_wim_stat(const char *wim_path, int index, const char *inner_path,
                 img_wim_stat_t *out, char *err, size_t errcap);

/* 抽【单个文件】到本地路径 dst（会覆盖）。流式 1 MiB，带进度与取消；
 * 取消时删除半成品（与 iso.extract 同契约）。目录一律 IMG_ENOTSUP。
 * 实现：wimlib_extract_paths 到临时目录（img_tmpdir()）→ 复制出来 → 递归删临时目录。*/
/* 抽【整个子树】到调用方给的目录 out_dir（保留 WIM 内结构：out_dir + inner_path）。
 * 给跨文件系统复制用（file.copy 的源是 WIM/ESD 时，先材料化一次再按本地树复制）。
 * out_dir 必须已存在且为空（调用方用 mkdtemp 建）。*/
int img_wim_extract_tree(const char *wim_path, int index, const char *inner_path,
                         const char *out_dir, img_prog *prog,
                         char *err, size_t errcap);

/* 内容判位数（REQ-19 / REQ-22）：
 *   ① SysWOW64 目录 / System32\wow64.dll → 64 位家族；
 *   ② 否则按【小到大】抽一个系统文件读 PE Machine：smss.exe → wininit.exe →
 *      ntdll.dll → ntoskrnl.exe（0x014c=x86 0x8664=x64 0xAA64=arm64 0x01c4=arm）；
 *   ③ 判不出 → out 置空串（调用方再退介质级信号 / XML），不猜。
 * src（可选）回填"凭什么"：`syswow64` / `wow64` / `pe:smss.exe` …
 * 返回非 0 时 out 为空串；why 里是给人看的原因。*/
int img_wim_probe_arch(const char *wim_path, int index, char *out, size_t cap,
                       char *src, size_t srccap, char *why, size_t whycap);

/* 这个 WIM 本身是不是一份 WinRE 映像？（根下没有 \Windows → 是）
 * 给 winre.find / recovery.populate 认"裸 winre.wim"用（REQ-24/25）。*/
int img_wim_is_winre_self(const char *wim_path, bool *out_self, char *why, size_t whycap);

/* 流式取单个文件（REQ-29）：wimlib 用 TO_STDOUT 解码，字节**按顺序**从 rfd 交出来。
 *   consume(ud, rfd, total_size, err, errcap) 在【辅助线程】里被调用，负责把字节搬走；
 *   本函数在【调用线程】里跑 wimlib（实测：wimlib 放进 pthread 再配 pipe 会死锁）。
 *   rfd 由我们关，调用方不要关。返回 0 = 两侧都成功。*/
typedef int (*img_wim_consume_fn)(void *ud, int rfd, uint64_t total,
                                  char *err, size_t errcap);
int img_wim_stream_consume(const char *wim_path,int index,const char *inner_path,
                           img_wim_consume_fn consume, void *ud,
                           uint64_t *out_size, img_prog *prog,
                           char *err, size_t errcap);

int img_wim_extract_dir(const char *wim_path,int index,const char *inner_path,
                        const char *dst_dir,img_prog *prog,uint64_t *out_bytes,
                        int *out_files,char *err,size_t errcap);

int img_wim_extract_file(const char *wim_path, int index, const char *inner_path,
                         const char *dst_path, img_prog *prog, uint64_t *out_bytes,
                         char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif
