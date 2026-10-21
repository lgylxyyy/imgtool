/* img_identify.h — 从【内容】判断 Windows 版本（文件名会骗人） */
#ifndef IMG_IDENTIFY_H
#define IMG_IDENTIFY_H
#include "img_types.h"
#include "img_io.h"
#include "img_qcow2.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 随机读回调：iso 用 img_iso_read_range，普通文件用 img_io_read_full 包一层 */
typedef int (*img_rd_fn)(void *ud, uint64_t off, void *buf, size_t len);

typedef struct {
    int      index;
    char     name[128];        /* <NAME>，如 Windows 7 PROFESSIONAL */
    char     display_name[128];/* <DISPLAYNAME>，可能空 */
    char     edition_id[64];   /* <EDITIONID>，如 Professional */
    char     arch[16];         /* <ARCH>：0=x86 5=arm 9=x64 12=arm64 */
    int      major, minor, build;
    char     languages[96];    /* 逗号分隔 */
    uint64_t total_bytes;      /* 展开大小 */
    int      dir_count, file_count;
} img_wim_image_t;

/* 读 WIM/ESD 的 XML（只读头 + 尾部 XML，不解析数据块，几毫秒） */
int img_wim_identify(img_rd_fn rd, void *ud,
                     img_wim_image_t *out, int max_images, int *out_count,
                     char *err, size_t errcap);

/* 读一张【已装好的盘】里 Windows 的真实版本（读 \Windows\System32\config\SOFTWARE）*/
typedef struct {
    /* ① 注册表：\Windows\System32\config\SOFTWARE → Microsoft\Windows NT\CurrentVersion
     *    ⚠ 可以被第三方改（改完还能开机）*/
    char product_name[96];   /* Windows 7 Professional */
    char edition_id[64];
    char build_lab[96];
    char display_version[64];/* Win10+ 的 21H2 之类；Win7 为空 */
    char current_version[32];/* 6.1 / 10.0 */
    int  current_build;      /* 7601 / 19044 / 28000 */

    /* ② 特征文件：\Windows\System32\ntoskrnl.exe 的 PE 版本资源
     *    ⚠ 这个改不动——改了内核就启动不了，所以它才是"真相"*/
    char kernel_file_version[40];   /* 6.1.7601.17514 */
    char kernel_product_version[40];
    char kernel_company[96];
    char kernel_description[128];   /* Microsoft® Windows® Operating System */
    int  kernel_major, kernel_minor, kernel_build;

    /* ③ 版本证据：\Windows\System32\spp\tokens\skus 下的目录名（Professional 等）*/
    char skus[160];

    /* ④ 判定：注册表 vs 内核 */
    int  mismatch;          /* 1 = 不一致（疑似被改过/精简过）*/
} img_winver_t;
int img_win_version(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                    img_winver_t *out, char *err, size_t errcap);

/* 纯函数：从内存里的 PE 文件解析版本资源（给 ntoskrnl.exe 用）。
 * 不读盘、不依赖库 —— 便于单独测试。*/
int img_pe_version(const uint8_t *buf, size_t len, img_winver_t *out);

#ifdef __cplusplus
}
#endif
#endif
