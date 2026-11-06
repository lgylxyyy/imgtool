/* img_ptfile.h — 分区表文件（.json）：导出 / 读入（REQ-20261026-14）
 *
 * 为什么单独一个模块：App 要做"把这张盘的分区表存下来，以后照着长一份"。
 * 表就是表 —— 只有 LBA/大小/类型/分区 GUID/名字/属性/活动位，**不含任何文件数据**。
 *
 * 文件格式（工具端定，App 只读）：
 *   { "kind":"icdq.parttable", "v":1, "table":"GPT|MBR", "sectorSize":512,
 *     "diskSectors":N, "diskGuid":"…", "diskSignature":"0x…", "mbrCode":null|"hex",
 *     "parts":[ {"first","last?","sectors","mbri","active","logical",
 *                "typeGuid","partGuid","name","attributes"} ] }
 * ★ 字段名与 part.show 的 pt.parts[] 一致，App 的预览代码不用写第二套。
 */
#ifndef IMG_PTFILE_H
#define IMG_PTFILE_H
#include "img_types.h"
#include "img_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_PTFILE_KIND "icdq.parttable"
#define IMG_PTFILE_V    1

typedef struct {
    img_parttable_t t;                  /* 解析出来的表（含 parts[]） */
    uint64_t disk_sectors;              /* 文件里记的盘大小（**导入时只当提示**，不是约束） */
    uint8_t  mbr_code[446];             /* MBR 引导码（没给就全 0） */
    bool     has_mbr_code;
} img_ptfile_t;

/* 读文件 → 解析 → **结构类校验**（不含"装不装得下目标盘"，那要看目标盘）。
 * 失败时：*code 填错误码（PT-FILE-BAD / PT-OVERLAP / PT-TOO-MANY / PT-DUP-GUID），
 *         at = 出问题的字段名，why = 人话；返回非 IMG_OK。
 * 成功返回 IMG_OK，out 可直接喂给 img_parttable_write()。 */
int img_ptfile_load(const char *path, img_ptfile_t *out,
                    char *code, size_t codecap,
                    char *at, size_t atcap,
                    char *why, size_t whycap);

/* 把表渲染成规范 JSON（供 part.export / 预览）。返回写入的字节数，失败 ≤0。 */
int img_ptfile_dump(const img_parttable_t *t, const uint8_t *mbr_code,
                    char *buf, int cap);

/* 校验"这张表装不装得下 disk_sectors 这么大的盘"。
 * 装不下时返回 false 并给 need_sectors（最小盘扇区数）与 need_gb（向上取整的 GiB）。 */
bool img_ptfile_fits(const img_parttable_t *t, uint64_t disk_sectors,
                     uint64_t *need_sectors, uint64_t *need_gb);

#ifdef __cplusplus
}
#endif
#endif