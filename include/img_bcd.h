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
/* 恢复（WinRE）加载器常用 GUID —— 仅作占位，真实 GUID 需在创建对象时生成 */
#define IMG_BCD_GUID_RECOVERY    "b0b53260-3b27-4f0a-8d0f-6b3e0c1a2b3c"
/* 官方安装介质的 RAMDisk 选项对象（含 32000004 = \boot\boot.sdi） */
#define IMG_BCD_GUID_RAMDISK_OPT "7619dcc8-fafe-11d9-b411-000476eba25f"
/* 官方安装介质的 WinPE 加载器对象 */
#define IMG_BCD_GUID_WINPE       "7619dcc9-fafe-11d9-b411-000476eba25f"

/* 元素 ID（十六进制字符串形式，与 reg 键名一致） */
#define IMG_BCD_EL_DESC          "11000001"
#define IMG_BCD_EL_DEVICE        "12000002"
#define IMG_BCD_EL_OSDEVICE      "12000004"
#define IMG_BCD_EL_LOCALE        "12000005"
#define IMG_BCD_EL_INHERITS      "14000006"
#define IMG_BCD_EL_SYSROOT       "22000002"
#define IMG_BCD_EL_DISPLAYORDER  "24000001"
/* ★ 元素 ID 实测修正（来自 4 份真实 BCD 对照）：
 *   25000004 实测值恒为小数字 3/30/30/3 —— 这是【Timeout(秒)】，不是 RecoverySequence。
 *   24000001 = DisplayOrder (GUID 列表, t=7)
 *   24000010 = ToolsDisplayOrder (GUID 列表, t=7)
 *   23000003 = 指向默认加载器的 GUID 字符串
 *   32000004 = RAMDisk SDI 路径（\boot\boot.sdi，存在于官方 ISO BCD）
 *   26000010 / 26000022 / 260000b0 = WinPE 加载器开关
 * ⚠ RecoverySequence 的真实元素 ID 尚未从真值反推到（4 份参考 BCD 均未配置该项）
 *   —— 在拿到权威 ID 之前，img_bcd_set_recovery_sequence 返回 IMG_ENOTSUP，避免写出坏 BCD。*/
#define IMG_BCD_EL_RECOVERYSEQ   "25000004"   /* ⚠ 已证实是 Timeout，勿用 */

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
