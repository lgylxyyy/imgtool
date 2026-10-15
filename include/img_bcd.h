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
/* 元素 ID 权威来源：geoffchappell.com/notes/windows/boot/bcd/elements.htm
 * （bcdedit /? + BCDSRV.DLL 类型库反查，2026-10 经 Tavily 检索确认）
 *
 * ★ 元素 ID 是个 dword，按位段解读（这是理解一切的钥匙）：
 *     0xF0000000 = class   1=library  2=application  3=device  4=template
 *     0x0F000000 = format  1=device   2=string       3=GUID    4=GUID list
 *                                    5=integer      6=boolean 7=integer list
 *
 * 已确认的元素（格式括号里是 format 段）：
 *   11000001 (device)    Description           二进制结构 88B
 *   12000002 (string)    Device                文件路径
 *   12000004 (string)    OSDevice / 显示名
 *   12000005 (string)    Locale
 *   14000006 (GUID list) Inherits
 *   14000008 (GUID list) ★RecoverySequence     ← bcdedit 名 recoverysequence，Vista+
 *   1500004b (integer)   ...
 *   16000009 (boolean)   recoveryenabled
 *   16000060 (boolean)   DetectHAL
 *   21000001 (device)    OSLoader OSDevice
 *   22000002 (string)    SystemRoot            \Windows
 *   23000003 (GUID)      ResumeObject          ← 单个 GUID（不是列表！）
 *   24000001 (GUID list) DisplayOrder
 *   24000010 (GUID list) ToolsDisplayOrder
 *   25000004 (integer)   Timeout(秒)           ← 我曾误认成 RecoverySequence
 *   250000e0 (integer)   bootstatuspolicy
 *   32000004 (string)    RAMDisk SDI 路径      \boot\boot.sdi
 */
#define IMG_BCD_EL_RECOVERYSEQ   "14000008"   /* GUID list — 已确认 */
#define IMG_BCD_EL_RECOVERYENAB  "16000009"   /* boolean   — bcdedit recoveryenabled */


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

/* ---------- 对象复制 / 恢复条目构建 ----------
 * 策略：不手写 device 对象二进制（错一个字段 Windows 就静默忽略），
 *       而是从【官方真值】复制对象再改 WIM 路径。 */

/* 把 src_path（参考 BCD）里的对象 guid 整体复制到 dst */
int img_bcd_copy_object(img_bcd *dst, const char *src_path, const char *guid);
/* 新建空对象 */
int img_bcd_create_object(img_bcd *b, const char *guid);
/* 设字符串元素（UTF-8 入参） */
int img_bcd_set_string(img_bcd *b, const char *guid, const char *eid, const char *value);
/* 把对象加入 bootmgr 的 DisplayOrder（幂等） */
int img_bcd_displayorder_add(img_bcd *b, const char *guid);

/* 官方 LTSC ISO 里两个可复用的对象（已实测存在） */
#define IMG_BCD_GUID_ISO_RAMDISK  "7619dcc8-fafe-11d9-b411-000476eba25f"
#define IMG_BCD_GUID_ISO_WINPE    "7619dcc9-fafe-11d9-b411-000476eba25f"
/* WinPE loader 上与 ramdisk 相关的元素 */
#define IMG_BCD_EL_WIM_DEVICE     "21000001"   /* OSLoader 的 OSDevice（内嵌 WIM 路径）*/
#define IMG_BCD_EL_APP_DEVICE     "11000001"   /* library 的 ApplicationDevice —— ★
                                                * 官方 WinPE loader 上它与 21000001 内容
                                                * 相同；bootmgr 主要读这一个，两个都得改 */
#define IMG_BCD_EL_BOOTSDI_PATH   "32000004"   /* ramdiskoptions: \boot\boot.sdi */

/* device 对象(21000001/11000001) 内嵌 WIM 路径的读/写
 * 结构见 src/img_bcd.c 里 patch 函数上方注释；结构不符时返回 IMG_ECORRUPT（不硬写）*/
int img_bcd_patch_device_wimpath(img_bcd *b, const char *guid, const char *eid,
                                 const char *new_path);
int img_bcd_get_device_wimpath(img_bcd *b, const char *guid, const char *eid,
                               char *out, size_t outlen);
