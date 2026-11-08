/* img_wim_capture.h — 从 qcow2 里的 NTFS 分区捕获成 WIM（P4）
 *
 * 为什么需要"先抽到临时目录"：
 *   wimlib 的 wimlib_add_tree() 只接受**宿主文件系统路径**，不能直接读 NTFS。
 *   所以流程必然是：
 *     ① img_ntfs 全树遍历 → 抽到 <tmp>/imgtool-tmp-wimcap-<pid>-XXXXXX
 *     ② wimlib create_new_wim + add_tree + write
 *     ③ 删临时目录
 *
 *   ★ 这是库限制，不是实现偷懒。单文件抽取（wim.extract / wim.put）能做到
 *     0 临时文件，但"全盘打包"必须过一遍磁盘。
 *
 * 空间要求：临时目录必须能放下【整棵系统树】（Windows 装完 ≈ 15~25 GB）。
 *   调用方（App）应先 du 算大小、确认剩余空间够，再调用。
 */
#ifndef IMG_WIM_CAPTURE_H
#define IMG_WIM_CAPTURE_H
#include "img_types.h"
#include "img_qcow2.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_WIM_COMP_NONE = 0,   /* 不压缩（最快，体积最大）*/
    IMG_WIM_COMP_XPRESS,     /* XPRESS（WIM 默认，兼容性最好）*/
    IMG_WIM_COMP_LZX,        /* LZX（压缩率好，Win7+ 都认）*/
    IMG_WIM_COMP_LZMS        /* LZMS（ESD 用，压缩率最高，慢）*/
} img_wim_comp;

typedef struct {
    img_wim_comp comp;
    const char  *image_name;    /* 映像名（默认 "Windows"）*/
    const char  *image_desc;    /* 映像描述（可 NULL）*/
    bool         capture_acl;   /* 是否保留 ACL（默认 true）*/
    uint64_t     tmpdir_need;   /* 输出：估算需要的临时空间（字节）*/
} img_wim_capture_opts_t;

typedef struct {
    uint64_t files;             /* 打包了多少文件 */
    uint64_t dirs;
    uint64_t bytes_in;          /* 原始字节 */
    uint64_t bytes_out;         /* WIM 字节 */
    uint64_t tmpdir_need;       /* 估算需要的临时空间（= bytes_in）*/
} img_wim_capture_result_t;

/* 从 qcow2 的某个 NTFS 分区捕获成 WIM。
 *   img_path   镜像路径
 *   lba        系统分区起始 LBA
 *   part_bytes 分区字节数（0 = 按分区表算）
 *   src_subdir 只捕获这个子目录（NULL 或 "/" = 整盘）
 *   out_wim    输出 WIM 路径
 *   err        失败原因
 * 返回 IMG_OK / 错误码。 */
int img_wim_capture(const char *img_path, uint64_t lba, uint64_t part_bytes,
                    const char *src_subdir, const char *out_wim,
                    const img_wim_capture_opts_t *opts,
                    img_wim_capture_result_t *out,
                    img_prog *prog, char *err, size_t errcap);

/* 估算捕获需要多少临时空间（只读，扫 NTFS 全树，不抽文件）*/
int img_wim_capture_estimate(const char *img_path, uint64_t lba, uint64_t part_bytes,
                             const char *src_subdir, uint64_t *out_bytes,
                             uint64_t *out_files, char *err, size_t errcap);

const char *img_wim_comp_name(img_wim_comp c);

#ifdef __cplusplus
}
#endif
#endif
