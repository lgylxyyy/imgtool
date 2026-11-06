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
    uint64_t sys_bytes;         /* MBR = 活动系统分区(0x27)；GPT = Windows 分区（0 = 吃满剩余） */
    uint64_t msr_bytes;         /* MSR 大小，官方 16MB */
    uint64_t recovery_bytes;    /* 恢复分区大小，0 = 不建 */
    uint64_t alignment;         /* 对齐扇区，默认 2048 */
    /* ★ MBR 的 Windows 分区大小（REQ-20261025-05）。0 = 吃满剩余（老行为）。
     *   MBR 的 sys_bytes 已经被"系统保留分区"占了，所以 Windows 单独一个字段。 */
    uint64_t win_bytes;
    /* ★ MBR 追加分区怎么用主分区槽（REQ-20261025-07 / -08）：
     *   0 = reserve（默认，老行为）：System+Windows(+Rec)+N ≥ 4 时整批进 0x0F 容器，留一个槽；
     *   1 = primary：先把 4 个槽用满，装不下就失败；
     *   2 = auto（REQ-08）：主分区最多只填到 3 个（留一个槽给容器），剩下的进容器。*/
    int      mbr_fill;
    /* ★ 追加分区（2026-10-25，REQ-20261025-01）：按顺序放在 Windows/系统分区之后。
     *   地址由布局生成器算（1 MiB 对齐），调用方只给大小。
     *   NULL / 0 = 不追加（老调用方的行为完全不变）。
     *   MBR 主分区满 4 个时自动降级成【0x0F 扩展容器 + 逻辑分区】。 */
    const uint64_t *extra_bytes;
    int             extra_count;
} img_layout_opts_t;

/* ---------- 尺寸口径（导出给前端 + 校验的唯一来源） ----------
 * 为什么单独一张表：前端（App）要靠它画范围/高亮输入框，
 * 命令行绕过前端时工具层要拿同一份数字去拦 —— 一处改，两边同步。 */
typedef struct {
    const char *cmd;        /* "img.create" / "part.add" */
    const char *arg;        /* 参数名（也是错误信封里的 at） */
    const char *role;       /* "disk"/"esp"/"msr"/"recovery"/"windows"/"sys"/"data" */
    uint64_t    min;        /* 最小（0 表示只允许 0 这种特例，见 zero_ok） */
    uint64_t    max;        /* 最大；0 = 无上限 */
    const char *unit;       /* "MiB" / "GiB" */
    bool        zero_ok;    /* true = 该参数允许 0（= 不建/吃满剩余） */
} img_arg_limit_t;

/* 查某个命令某个参数的口径；没有返回 NULL（= 不是尺寸类参数） */
const img_arg_limit_t *img_arg_limit_find(const char *cmd, const char *arg);
/* 按 role 查口径（前端画默认值/提示用）；没有返回 NULL */
const img_arg_limit_t *img_arg_limit_by_role(const char *role);
/* 渲染成 JSON：{"esp":{min,max,unit},...}（cmd.list 里直接塞） */
int img_arg_limits_json(char *buf, int cap);

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
