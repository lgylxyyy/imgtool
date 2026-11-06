/* img_identify.h — 从【内容】判断 Windows 版本（文件名会不准确） */
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
    char     arch[16];         /* 归一化：x86|x64|arm|arm64；判不出为空串 */
    char     arch_raw[16];     /* <ARCH> 原值（0/9/12/…，排查用；对微软镜像常是 "0"）*/
    int      major, minor, build;
    char     languages[96];    /* 逗号分隔 */
    uint64_t total_bytes;      /* 展开大小 */
    int      dir_count, file_count;
} img_wim_image_t;

/* <ARCH> 原值 → 归一化位数（REQ-19）*/
void img_arch_normalize(const char *raw, char *out, size_t cap);

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
    uint16_t pe_machine;          /* 内核 PE 的 Machine：0x14c=x86 0x8664=x64 0xAA64=arm64 */
    uint32_t pe_offset;           /* PE 头在文件里的偏移（bootmgr 那种带 16 位引导壳的 >0）*/
    char servicing_version[40];   /* \Windows\servicing\Version\<x.y.build.rev> */
    char skipped[120];            /* 文件存在但无法读取版本（WOF/CompactOS 压缩）的核心文件 */

    /* ④ 多方法投票（每个证据源报一个 build；不一致就说明被改过）*/
    struct { char src[24]; int build; char ver[40]; } vote[6];
    int  n_vote;
    int  consensus_build;   /* 多数票的 build（0=没有足够证据）*/
    int  mismatch;          /* 1 = 多个来源互相矛盾 */
} img_winver_t;
int img_win_version(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                    img_winver_t *out, char *err, size_t errcap);

/* build 的"代"（19041/19045 → 1904；7601 → 760）。Win10 起启用包升级会让
 * 注册表 CurrentBuild(19045) 与核心文件版本(10.0.19041.x) 不一致，那是正常的。*/
int  img_build_family(int build);
/* 多源投票（读盘时由 img_win_version 调用；单测也直接用它）：
 * 按"代"取众数写 consensus_build，跨代不一致则 mismatch=1。*/
void img_winver_vote(img_winver_t *v);

/* 纯函数：从内存里的 PE 文件解析版本资源（给 ntoskrnl.exe 用）。
 * 不读盘、不依赖库 —— 便于单独测试。*/
int img_pe_version(const uint8_t *buf, size_t len, img_winver_t *out);

#ifdef __cplusplus
}
#endif
#endif
