/* img_bcd.h — BCD（注册表 hive）读写
 *
 * BCD 是 Windows 注册表 hive 文件，存放引导配置。
 * 容器格式与元素 ID 规律见 reference/BCD-FORMAT.md。
 *
 * 实现：直接调用 libhivex（LGPL-2.1+），它已处理：
 *   hive 校验、cell 分配、bin 重平衡、次序号自增。
 * 我们不做 hive 底层格式，只做 BCD 语义层。
 */
#ifndef IMG_BCD_H
#define IMG_BCD_H
#include "img_types.h"
#include "img_qcow2.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 常量 ---------- */
/* Windows Boot Manager 对象 GUID（永远固定） */
#define IMG_BCD_GUID_BOOTMGR     "9dea862c-5cdd-4e70-acc1-f32b344d4795"
/* RAMDisk 选项对象 GUID（固定） */
#define IMG_BCD_GUID_RAMDISK     "ae5534e0-a924-466e-b634-5386399ec2bc"
/* 恢复（WinRE）加载器常用 GUID */
#define IMG_BCD_GUID_RECOVERY    "b0b53260-3b27-4f0a-8d0f-6b3e0c1a2b3c"

/* 元素 ID（十六进制字符串形式，与 reg 键名一致） */
#define IMG_BCD_EL_DESC          "11000001"
#define IMG_BCD_EL_DEVICE        "12000002"
#define IMG_BCD_EL_OSDEVICE      "12000004"
#define IMG_BCD_EL_LOCALE        "12000005"
#define IMG_BCD_EL_INHERITS      "14000006"
#define IMG_BCD_EL_SYSROOT       "22000002"
#define IMG_BCD_EL_DISPLAYORDER  "24000001"
#define IMG_BCD_EL_RECOVERYSEQ   "25000004"

typedef struct img_bcd img_bcd;   /* 不透明句柄 */

/* 打开（读写）。template_bytes 可为 NULL（用文件），否则用内存副本作为模板。 */
img_bcd *img_bcd_open_file(const char *path);
img_bcd *img_bcd_open_mem (const void *data, size_t len);
int      img_bcd_save     (img_bcd *b, const char *path);      /* 落盘 */
int      img_bcd_save_mem (img_bcd *b, void **out, size_t *out_len);
void     img_bcd_close    (img_bcd *b);

/* 语义层操作 —— 全部返回 IMG_OK / IMG_E* */
/* 设 bootmgr 的显示名（影响引导菜单文案） */
int img_bcd_set_bootmgr_name(img_bcd *b, const char *name);
/* 设 bootmgr 的 RecoverySequence 指向某恢复对象 GUID */
int img_bcd_set_recovery_sequence(img_bcd *b, const char *recovery_guid);
/* 取 bootmgr DisplayOrder 第一项（默认启动项 GUID），out 至少 39 字节 */
int img_bcd_get_display_order_first(img_bcd *b, char *out, size_t out_len);
/* 某元素是否存在 */
bool img_bcd_has_element(img_bcd *b, const char *object_guid, const char *element_id);
/* 取字符串型元素（utf-8 输出），调用方 free */
char *img_bcd_get_string(img_bcd *b, const char *object_guid, const char *element_id);

#ifdef __cplusplus
}
#endif
#endif
