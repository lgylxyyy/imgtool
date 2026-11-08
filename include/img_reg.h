/* img_reg.h — 离线注册表 hive 通用读写（P2）
 *
 * 用途：在**镜像关机**状态下，直接改 qcow2 里 NTFS 上的注册表 hive 文件。
 *       不启动 Windows、不挂载卷、不运行 regedit。
 *
 * 目标 hive（固定 5 个，对应 Windows 的 5 个根键）：
 *   SYSTEM   → HKLM\SYSTEM
 *   SOFTWARE → HKLM\SOFTWARE
 *   SAM      → HKLM\SAM
 *   SECURITY → HKLM\SECURITY
 *   DEFAULT  → HKU\.DEFAULT
 *
 * 路径语法：`<hive>\<子键>\<子键>\...\<值名>`，例如
 *   SYSTEM\Setup\Status\ChildCompletion\setup.exe
 *   SOFTWARE\Microsoft\Windows NT\CurrentVersion\CurrentBuild
 *
 * 事务性（与 win.finish 同一套机制）：
 *   ① 从分区抽出 hive 到临时文件
 *   ② hivex 打开 → 改（内存中）
 *   ③ hivex_commit 写回临时文件
 *   ④ 读回校验（重新打开确认值已生效）
 *   ⑤ 校验通过才 img_ntfs_put_file 覆盖回去
 *   任一步失败 → 删临时文件，分区里的 hive 字节不变。
 *
 * 设计取舍：
 *   · **不做在线修改**（镜像必须关机）——绕过文件系统驱动直接改元数据，
 *     两个写入方同时改必然损坏。这是设计红线。
 *   · **不隐藏 .LOG/.LOG1**：hivex 只改主 hive，Windows 的日志文件里还留着旧记录。
 *     正常关机状态下 Windows 会以主 hive 为准；异常关机后可能回滚。
 *     调用方（App）应在改前确认镜像确实是「已关机」状态。
 */
#ifndef IMG_REG_H
#define IMG_REG_H
#include "img_types.h"
#include "img_qcow2.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 5 个标准 hive 的逻辑名（大小写不敏感） */
typedef enum {
    IMG_REG_HIVE_SYSTEM = 0,
    IMG_REG_HIVE_SOFTWARE,
    IMG_REG_HIVE_SAM,
    IMG_REG_HIVE_SECURITY,
    IMG_REG_HIVE_DEFAULT,
    IMG_REG_HIVE__COUNT
} img_reg_hive;

/* 值的类型（与 Windows 的 REG_* 一一对应） */
typedef enum {
    IMG_REG_NONE = 0,
    IMG_REG_SZ,             /* REG_SZ           字符串 */
    IMG_REG_EXPAND_SZ,      /* REG_EXPAND_SZ    可展开字符串 */
    IMG_REG_BINARY,         /* REG_BINARY       原始字节 */
    IMG_REG_DWORD,          /* REG_DWORD        32 位 */
    IMG_REG_DWORD_BE,       /* REG_DWORD_BIG_ENDIAN */
    IMG_REG_LINK,           /* REG_LINK         符号链接 */
    IMG_REG_MULTI_SZ,       /* REG_MULTI_SZ     多字符串 */
    IMG_REG_RESOURCE_LIST,
    IMG_REG_FULL_RESOURCE_DESCRIPTOR,
    IMG_REG_RESOURCE_REQUIREMENTS_LIST,
    IMG_REG_QWORD           /* REG_QWORD        64 位 */
} img_reg_type;

/* 一个值的读取结果。调用方用 img_reg_value_free() 释放 data。 */
typedef struct {
    char         *name;       /* 值名（"" = 默认值） */
    img_reg_type  type;
    uint8_t      *data;       /* 原始字节（REG_SZ 时含结尾 NUL，已转为 UTF-8） */
    size_t        len;        /* data 字节数 */
} img_reg_value;

/* ---- 打开/关闭（一次操作打开一次，内部抽出 hive 到临时文件）---- */
typedef struct img_reg img_reg;

/* 打开某个分区的某个 hive。
 *   img_path  镜像路径
 *   lba       系统分区（NTFS）起始 LBA
 *   part_bytes 分区字节数（0 = 按分区表算）
 *   hive      逻辑名（SYSTEM / SOFTWARE / ...）
 *   writable  true = 允许后续写入（会以 HIVEX_OPEN_WRITE 打开）
 *   err      失败说明（可为 NULL）
 * 返回 NULL 表示失败（err 里有原因）。 */
img_reg *img_reg_open(const char *img_path, uint64_t lba, uint64_t part_bytes,
                      img_reg_hive hive, bool writable, char *err, size_t errcap);

void img_reg_close(img_reg *r);

/* 最近一次操作是否真的改了 hive（close 时已写回分区） */
bool img_reg_changed(const img_reg *r);

/* ---- 读 ---- */
/* 读一个值。key_path 是 hive 内的键路径（如 "Setup\\Status\\ChildCompletion"），
 * value_name 是值名（"" = 默认值）。成功返回 IMG_OK 并填 out。 */
int img_reg_get(img_reg *r, const char *key_path, const char *value_name,
                img_reg_value *out, char *err, size_t errcap);
void img_reg_value_free(img_reg_value *v);

/* 列一个键下的子键与值（回调式，避免一次分配巨大数组）。
 *   key_path  "" 或 "/" = 根键
 *   want_subkeys / want_values 至少要一个为 true
 * 回调返回 false 时提前终止。 */
typedef void (*img_reg_enum_cb)(void *ud, const char *name, bool is_key,
                                img_reg_type type, size_t len);
int img_reg_enum(img_reg *r, const char *key_path,
                 bool want_subkeys, bool want_values,
                 img_reg_enum_cb cb, void *ud, char *err, size_t errcap);

/* 键是否存在 */
int img_reg_key_exists(img_reg *r, const char *key_path, bool *out, char *err, size_t errcap);

/* ---- 写 ---- */
/* 设一个值（键不存在则建）。type 决定 data 的解释方式：
 *   IMG_REG_DWORD  → data 指向 4 字节小端（调用方可直接传 &uint32_t）
 *   IMG_REG_QWORD  → data 指向 8 字节小端
 *   IMG_REG_SZ / IMG_REG_EXPAND_SZ / IMG_REG_MULTI_SZ → data 是 UTF-8 字符串
 *   IMG_REG_BINARY → data 是原始字节
 * 返回 IMG_OK 表示已改（close 时才真正落盘）。 */
int img_reg_set(img_reg *r, const char *key_path, const char *value_name,
                img_reg_type type, const void *data, size_t len,
                char *err, size_t errcap);

/* 便捷：写 DWORD / QWORD / 字符串 */
int img_reg_set_dword(img_reg *r, const char *key_path, const char *value_name,
                      uint32_t v, char *err, size_t errcap);
int img_reg_set_qword(img_reg *r, const char *key_path, const char *value_name,
                      uint64_t v, char *err, size_t errcap);
int img_reg_set_sz(img_reg *r, const char *key_path, const char *value_name,
                   const char *utf8, char *err, size_t errcap);

/* 删一个值（值不存在返回 IMG_ENOENT） */
int img_reg_del_value(img_reg *r, const char *key_path, const char *value_name,
                      char *err, size_t errcap);

/* 删一个键。recursive=false 时非空键返回 IMG_ESTATE（不误删子树）。 */
int img_reg_del_key(img_reg *r, const char *key_path, bool recursive,
                    char *err, size_t errcap);

/* ---- 工具 ---- */
const char *img_reg_hive_name(img_reg_hive h);       /* "SYSTEM" / ... */
const char *img_reg_type_name(img_reg_type t);       /* "REG_SZ" / ... */
bool img_reg_hive_from_name(const char *s, img_reg_hive *out);   /* 大小写不敏感 */

/* 解析 "SYSTEM\Setup\Status" → hive=SYSTEM, key="Setup\Status"。
 * 返回 IMG_OK / IMG_EINVAL（err 里有说明）。 */
int img_reg_split(const char *full, img_reg_hive *hive,
                  char *key, size_t keycap, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
#endif
