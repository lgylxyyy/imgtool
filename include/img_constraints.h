/* img_constraints.h — 分区/磁盘的合法性约束（★ 唯一真相源）
 *
 * 设计原则：限制放在【工具层】而不是前端。
 *   ① 前端可能被替换（Web/iOS/CLI），规则不能重写
 *   ② 绕过前端直接调 API 也必须被拦住
 *   ③ 前端需要显示 min/max/建议 → 所以同时【导出】这些数值
 *
 * 规则来源：
 *   - Microsoft Learn: "BIOS/MBR-based hard drive partitions"
 *   - Microsoft Learn: "Configure UEFI/GPT-based hard drive partitions"
 *   - qcow2 / MBR / GPT 规范
 */
#ifndef IMG_CONSTRAINTS_H
#define IMG_CONSTRAINTS_H
#include "img_types.h"
#include "img_partition.h"
/* img_check_result_t 定义在 img_types.h（避免与 img_partition.h 循环依赖） */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 各类分区的尺寸约束 ---------- */
typedef enum {
    IMG_ROLE_NONE = 0,
    IMG_ROLE_ESP = 1,       /* EFI 系统分区 (UEFI/GPT) */
    IMG_ROLE_SYSPART,       /* BIOS/MBR 系统分区（活动分区） */
    IMG_ROLE_MSR,           /* Microsoft 保留分区 */
    IMG_ROLE_WINDOWS,       /* Windows 系统分区 */
    IMG_ROLE_RECOVERY,      /* Windows RE 恢复分区 */
    IMG_ROLE_DATA,          /* 普通数据分区 */
    IMG_ROLE_COUNT
} img_part_role_t;

typedef struct {
    uint64_t min_bytes;     /* 硬下限，小于必拒 */
    uint64_t rec_bytes;     /* 官方推荐 */
    uint64_t def_bytes;     /* 默认值（我们创建时用） */
    uint64_t max_bytes;     /* 硬上限，超过必拒（0 = 无上限） */
    bool     must_fat32;    /* 必须是 FAT32 */
    bool     must_ntfs;     /* 必须是 NTFS */
    bool     must_active;   /* 必须设为活动分区 (MBR) */
    const char *note;       /* 说明（给前端显示） */
} img_role_limits_t;

/* 导出某角色的约束（前端画滑块/提示用）。永远返回有效指针。 */
const img_role_limits_t *img_role_limits(img_part_role_t role);

/* ---------- 校验 ---------- */
typedef struct {
    img_table_t table;          /* 目标表类型 */
    uint64_t    disk_sectors;
    uint32_t    sector_size;    /* 512 或 4096（影响 ESP 最小值） */
    uint64_t    first_lba;
    uint64_t    sectors;
    img_part_role_t role;
    img_mbr_type_t mbr_type;    /* MBR 分区用 */
    const char *gpt_type_guid;  /* GPT 分区用 */
    bool        active;
    int         existing_count; /* 加之前已有几个分区（MBR 限制用） */
} img_part_request_t;

/* 校验单个分区 */
int img_constraints_check_partition(const img_part_request_t *req, img_check_result_t *out);
/* 校验整个表（含 MBR 数量/类型限制、重叠、超盘等） */
int img_constraints_check_table(const img_parttable_t *t, img_check_result_t *out);

/* ---------- 布局模板 ---------- */
typedef struct {
    uint64_t esp_bytes;         /* ESP 大小（UEFI） */
    uint64_t sys_bytes;         /* BIOS 系统分区大小 */
    uint64_t msr_bytes;         /* MSR 大小，官方 16MB */
    uint64_t recovery_bytes;    /* 恢复分区大小，0 = 不建 */
    uint64_t alignment;         /* 对齐扇区，默认 2048 */
} img_layout_opts_t;

/* 用默认值填充 */
void img_layout_defaults(img_layout_opts_t *o, img_table_t table);
/* 按角色+表类型生成完整分区表（不写盘）。返回 IMG_OK 或错误。 */
int img_layout_build(img_parttable_t *t, uint64_t disk_sectors,
                     img_table_t table, const img_layout_opts_t *o,
                     img_check_result_t *err);

/* ---------- 表类型转换 ---------- */
/* MBR ↔ GPT 转换（就地，会重写分区表；分区 LBA 保持不变）
 * 注意：MBR→GPT 会补 ESP/MSR？不会 —— 只把现有分区"翻译"过去，
 *       需要 ESP/MSR 请用 img_layout_build 重建。 */
int img_convert_mbr_to_gpt(img_parttable_t *t, img_check_result_t *err);
int img_convert_gpt_to_mbr(img_parttable_t *t, uint32_t disk_signature,
                           img_check_result_t *err);

#ifdef __cplusplus
}
#endif
#endif
