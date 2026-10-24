/* img_name.h — Windows 文件名合法性（NTFS / FAT32 目标写前校验）
 *
 * 为什么需要：工具能写任意字节序列当名字，但 Windows 打不开/看不见某些名字。
 *   · 字符 `< > : " / \ | ? *` 与 ASCII 0x00–0x1F 在 Win32 里非法；
 *   · 尾随 `.` / 空格会被 Win32 静默去掉 ⇒ 写的名字和看到的名字对不上；
 *   · CON/PRN/AUX/NUL/COM1..9/LPT1..9（含带扩展名）是设备名，不是文件。
 * 一旦写进去，用户在资源管理器里可能连删都删不掉，所以必须【写前】拒绝。
 *
 * 只管末级名字，不管路径；宿主（ext4 等）不走这里 —— 那是宿主的规则。
 */
#ifndef IMG_NAME_H
#define IMG_NAME_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_NAME_OK = 0,
    IMG_NAME_EMPTY,       /* 空名字 / "." / ".." */
    IMG_NAME_BAD_CHAR,    /* < > : " / \ | ? * 或 0x00–0x1F */
    IMG_NAME_TRAILING,    /* 以 '.' 或空格 结尾 */
    IMG_NAME_RESERVED,    /* CON PRN AUX NUL COM1..9 LPT1..9（含带扩展名） */
    IMG_NAME_TOO_LONG     /* 超过 255 个 UTF-16 码元（NTFS/FAT 都是这个上限） */
} img_name_status;

/* 校验一个末级名字（UTF-8）。返回 IMG_NAME_OK 表示 Windows 能正常处理。 */
img_name_status img_name_check(const char *name);

/* 稳定英文 id（写日志 / 测试断言用）：empty / bad-char / trailing / reserved / too-long */
const char *img_name_status_id(img_name_status s);

/* 中文短原因，直接放进 err.at 给用户看；非法名字会被截断到 64 字节。 */
const char *img_name_reason(img_name_status s, const char *name);

#ifdef __cplusplus
}
#endif
#endif