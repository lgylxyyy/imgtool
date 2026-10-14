/* img_partition.h — MBR / GPT 分区表读写
 *
 * MBR (Legacy BIOS)：
 *   0x1BE-0x1FD  4 项 × 16B = 64B；每项：status(1) CHS起(3) type(1) CHS末(3) LBA(4) size(4)
 *   0x1FE-0x1FF  签名 55 AA
 *   ★ 只有 1 项能是 0x80（活动），多个 → "Invalid partition table"
 *
 * GPT (UEFI)：
 *   LBA0 protective MBR（type 0xEE）+ LBA1 主头 + LBA2..33 分区项
 *   尾部有备份头 + 备份分区项
 *   ★ 魔数必须是【8 字节】"EFI PART"（我曾只写 4 字节 → OVMF 不识别）
 *   ★ 主/备头和分区项都要 CRC32 校验
 */
#ifndef IMG_PARTITION_H
#define IMG_PARTITION_H
#include "img_types.h"
#include "img_qcow2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMG_MAX_PARTITIONS 128
#define IMG_SECTOR_SIZE     512

/* MBR 分区类型（Windows 常用） */
typedef enum {
    IMG_MBR_EMPTY     = 0x00,
    IMG_MBR_FAT12     = 0x01,
    IMG_MBR_FAT16     = 0x04,
    IMG_MBR_EXTENDED  = 0x05,
    IMG_MBR_FAT16B    = 0x06,
    IMG_MBR_NTFS_EXFAT= 0x07,   /* ★ NTFS / exFAT / UDF / HPFS 都用它，需看内容 */
    IMG_MBR_FAT32     = 0x0b,
    IMG_MBR_FAT32_LBA = 0x0c,
    IMG_MBR_FAT16_LBA = 0x0e,
    IMG_MBR_EXTENDED_L=0x0f,
    IMG_MBR_RECOVERY  = 0x27,   /* ★ 微软官方：恢复分区用 0x27，别用 0x07 */
    IMG_MBR_ESP       = 0xef,
    IMG_MBR_LINUX     = 0x83,
    IMG_MBR_GPT_PROT  = 0xee,   /* GPT 保护 MBR */
} img_mbr_type_t;

/* 常见 GPT 分区类型 GUID（小端文本形式，与 sgdisk/Windows 一致） */
#define IMG_GUID_ESP       "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"  /* EFI System */
#define IMG_GUID_MSR       "e3c9e316-0b5c-4db8-817d-f92df00215ae"  /* MSR */
#define IMG_GUID_BASIC     "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"  /* Windows 基本数据 */
#define IMG_GUID_RECOVERY  "de94bba4-06d1-4d40-a16a-bfd50179d6ac"

/* ---- GPT 分区属性位 ----
 * UEFI 规范（bit 0-47 通用）：bit0=PlatformRequired bit1=NoBlockIO bit2=Hidden
 * 保留位 bit4-47 必须为 0；bit48-63 为【类型专用】（由类型 GUID 所有者定义）。
 * 下面几个是微软 Basic Data/Recovery 类型的专用位（Windows SDK Gpt.h）。 */
#define IMG_GPT_ATTR_PLATFORM_REQUIRED (1ULL<<0)          /* 平台必需（UEFI 通用） */
#define IMG_GPT_ATTR_NO_DRIVE_LETTER   (0x8000000000000000ULL) /* ★ 微软 bit63：不分配盘符 */
#define IMG_GPT_ATTR_HIDDEN            (0x4000000000000000ULL) /* 微软 bit62：隐藏 */
#define IMG_GPT_ATTR_SHADOW            (0x2000000000000000ULL) /* 微软 bit61：BitLocker 相关 */

/* 恢复分区默认属性：明确要求"不分配盘符"（用户需求）。
 * ⚠️ Windows 通常靠【类型 GUID】识别并隐藏，属性位是双保险。
 * 实测值待 19044 装好后用 reagentc /info 校验。 */
#define IMG_GPT_ATTR_RECOVERY_DEFAULT  IMG_GPT_ATTR_NO_DRIVE_LETTER

typedef struct {
    bool     used;
    uint64_t first_lba;      /* 含 */
    uint64_t last_lba;       /* 含 */
    uint32_t sectors_hi;     /* (GPT) 保留给 >2^32 */
    uint8_t  status;         /* MBR: 0x80 活动 / 0x00；GPT: 无意义 */
    uint8_t  type;           /* MBR type；GPT 用 type_guid */
    char     type_guid[40];  /* GPT 小端 GUID 文本 */
    char     part_guid[40];  /* GPT 分区唯一 GUID */
    char     name[73];       /* GPT 分区名 (UTF-16LE → UTF-8, 最多36字符) */
    uint64_t attributes;
} img_partition_t;

typedef struct {
    img_table_t table;                /* NONE / MBR / GPT */
    uint64_t    disk_sectors;         /* 磁盘总扇区 */
    int         count;                /* 已用槽位数（含空洞） */
    /* MBR */
    uint8_t     mbr_code[446];        /* 引导码（保留原样） */
    uint32_t    disk_signature;       /* MBR 偏移 440 的签名 */
    /* GPT */
    char        disk_guid[40];
    uint64_t    header_lba;
    uint64_t    alt_header_lba;
    uint32_t    part_entry_first_lba;
    uint32_t    part_entry_last_lba;
    uint32_t    part_entry_count;
    uint32_t    part_entry_size;
    uint64_t    part_entry_crc;
    uint64_t    current_lba;
    uint64_t    backup_lba;
    uint32_t    usable_first, usable_last;
    img_partition_t parts[IMG_MAX_PARTITIONS];
} img_parttable_t;

typedef void (*img_table_progress)(void *ud, int cur, int total, const char *what);

/* ---------- 读 ---------- */
/* 依据 disk_sectors 读分区表（自动判 MBR/GPT）。disk_sectors 可传 0 → 用镜像虚拟大小。 */
int img_parttable_read(img_qcow2 *q, img_parttable_t *out, uint64_t disk_sectors);
/* 只渲染成 JSON（前端面板直接用） */
int img_parttable_json(const img_parttable_t *t, char *buf, int cap);

/* ---------- 找空白区 ---------- */
typedef struct { uint64_t first_lba, last_lba, sectors; } img_range_t;
/* 从 start_lba 起找连续未分配区域（align=扇区对齐，默认2048）。
 * 返回找到的个数，写入 out（最多 max）。 */
int img_parttable_free_regions(const img_parttable_t *t, uint64_t total_sectors,
                               img_range_t *out, int max, uint64_t align,
                               uint64_t start_lba);

/* ---------- 写 ---------- */
int img_parttable_write(img_qcow2 *q, img_parttable_t *t);

/* ---------- 增删改 ---------- */
/* 新建分区（first=0 表示自动选合适空位）。返回其在 parts 中的下标。 */
int img_partition_add(img_parttable_t *t, uint64_t first_lba, uint64_t sectors,
                      img_mbr_type_t mbr_type, const char *gpt_type_guid,
                      const char *name, bool active, int *out_index);
int img_partition_remove(img_parttable_t *t, int index);
/* 改类型（MBR type 或 GPT type guid） */
int img_partition_set_type(img_parttable_t *t, int index, img_mbr_type_t mbr_type,
                           const char *gpt_type_guid);
/* 改活动标记（仅 MBR） */
int img_partition_set_active(img_parttable_t *t, int index, bool active);

/* ---------- 分区调整（DiskGenius / 傲梅分区助手 同款逻辑） ----------
 *
 * 非末位分区：调整后【立即调整后方分区】，两分区始终保持紧邻（无空洞）。
 *   · 调小 → 后方分区起点左移，自动吃掉腾出的空间
 *   · 调大 → 向后方分区借：后方分区起点右移，自动缩小
 *
 * 末位分区：
 *   · 调小 → 尾部【划出空余】（可用来新建分区）
 *   · 调大 → 空间够则直接扩；不够返回 IMG_E_NEED_PREV_SHRINK
 *             （由 App 询问用户"要不要压缩前面的分区"）
 *
 * new_sectors = 调整后的【目标扇区数】。 */
#define IMG_E_NEED_PREV_SHRINK (-1001)   /* 末位分区扩容空间不足，需压缩前一个 */

int img_partition_resize(img_parttable_t *t, int index, uint64_t new_sectors,
                         img_check_result_t *err);

/* 把第 index 个分区一直顶到【可用末尾】，精确不取整，后端零空余。
 * GPT 可用末尾 = 盘末扇区 - 备份表 - 备份头；MBR = 盘末扇区 - 1 */
uint64_t img_parttable_usable_last(const img_parttable_t *t);
int img_partition_fill_remaining(img_parttable_t *t, int index, img_check_result_t *err);

/* ---------- 工具 ---------- */
uint32_t img_crc32(const void *data, size_t len);
void     img_guid_new(char out[40]);                  /* 生成随机 GUID（文本形式） */
const char *img_mbr_type_name(uint8_t t);
/* 校验 GPT 类型/分区 GUID 文本是否合法 */
int img_partition_guid_valid(const char *s);

#ifdef __cplusplus
}
#endif
#endif
