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

/* ---- 「分区设备」对象构造（88 字节，格式已实测破译）----
 * 用于把 ramdisksdidevice(31000003) 从"本地设备"改为显式指向某分区。 */
int img_bcd_make_partition_device(const char *part_guid, const char *disk_guid,
                                  uint8_t out[88]);
int img_bcd_set_element_binary(img_bcd *b, const char *guid, const char *eid,
                               const void *data, size_t len);
int img_bcd_get_element_binary(img_bcd *b, const char *guid, const char *eid,
                               void *out, size_t cap, size_t *out_len);
/* 把 device 对象里的分区 GUID / 磁盘 GUID 换成新盘的值（换盘重建后必做） */
int img_bcd_patch_device_guids(img_bcd *b, const char *obj_guid, const char *eid,
                               const char *part_guid, const char *disk_guid);

/* ---- 全对象 GUID 批量映射（换盘必做）----
 * 参考 BCD 里同一块盘会出现【多个】分区设备对象（不止 11000001/21000001）：
 * 实测 Win11 参考件还有 21000026、31000003 等。只改已知元素 ID 会漏掉它们，
 * bootmgr/winload 仍会去找旧盘 → 随机 GUID 新盘启动失败。
 * 本函数按 hive 结构遍历所有 type=6 的 88 字节设备对象：
 *   分区 GUID：old_os→new_os / old_esp→new_esp / old_rec→new_rec（可传 NULL 跳过）
 *   磁盘 GUID：old_disk→new_disk（强烈建议传；所有设备都应指向新盘）
 * 只改 blob 的 [32..47]（分区 GUID）与 [56..71]（磁盘 GUID），其它字节原样。
 * patched 返回改动的对象数（可为 NULL）。 */
int img_bcd_patch_all_guid_bindings(img_bcd *b,
        const char *old_os,  const char *new_os,
        const char *old_esp, const char *new_esp,
        const char *old_rec, const char *new_rec,
        const char *old_disk, const char *new_disk,
        int *patched);
/* ------------------------------------------------------------------ */
/* 一站式换盘：把【参考 BCD】里的设备对象重映射到【新盘 GUID】           */
/* ------------------------------------------------------------------ */
/* 背景：UEFI 换盘（克隆/新建）后，BCD 里所有 type=6 设备对象都还指着参考盘。
 * 只改 11000001/21000001 两个加载器会漏掉 memtest/resume/ramdisk → 0xc000000e。
 * 本函数把 imggenbcd 那套"先读旧值 → 全对象映射 → 兜底"的流程收进库里，
 * 让 CLI（imggenbcd）和命令表（bcd.gen-uefi）走同一份实现，不再各写一遍。
 *
 *   req.sys_guid  新盘系统分区 partGuid（必填）
 *   req.disk_guid 新盘 diskGuid（必填）
 *   req.esp_guid  新盘 ESP partGuid（NULL = 只映射系统分区，ESP 对象保持原样）
 *   req.old_rec_guid/new_rec_guid  恢复分区重映射（都可 NULL）
 * 出参 hits = 实际改动的设备对象数（0 表示参考件里没有可映射的设备，调用方应报错）。
 * 返回 IMG_OK / IMG_EINVAL（GUID 非法）/ IMG_EFORMAT（参考件不是 UEFI BCD）。*/
typedef struct {
    const char *sys_guid;
    const char *disk_guid;
    const char *esp_guid;
    const char *old_rec_guid;
    const char *new_rec_guid;
} img_bcd_remap_req_t;
int img_bcd_remap(img_bcd *b, const img_bcd_remap_req_t *req,
                  int *hits, char *err, size_t errcap);

/* ------------------------------------------------------------------ */
/* 从模板"直接构建" BCD（不需要参考盘的旧 GUID）                        */
/* ------------------------------------------------------------------ */
/* 场景：Win7/8 光盘里的 BCD-Template 没有任何分区绑定（实测 0 个），
 *       boot.repair / imggenbcd 那类"读旧值再映射"的路子用不了。
 * 入参：template_path 模板 hive；out_path 输出；uefi=1 用 bootmgfw.efi/winload.efi，
 *       否则 bootmgr/winload.exe。MBR 需要 mbr_sig + sys_off + sr_off（字节偏移）；
 *       GPT 需要 sys_guid + disk_guid（esp_guid 可选）。
 * 出参：err 里填写说明。*/
typedef struct {
    const char *template_path;
    const char *out_path;
    bool        uefi;
    const char *desc;
    uint32_t    mbr_sig;
    uint64_t    sys_off, sr_off;
    const char *sys_guid, *disk_guid, *esp_guid;
} img_bcd_build_req_t;
int img_bcd_build(const img_bcd_build_req_t *rq, char *err, size_t errcap);

/* ------------------------------------------------------------------ */
/* bcd.create：用【内置模板】建一张能启动的 BCD（用户不必提供参考盘）     */
/* ------------------------------------------------------------------ */
/* 覆盖：Vista/7/8/8.1/10/11；BIOS(MBR) 与 UEFI(GPT)；x86 与 x64 不分包
 * （BCD 只记 \Windows\system32\winload.exe|.efi，与位数无关）。
 * 做法：内置一张真实能启动的 BCD 当底座，把【每一处与盘有关的东西】按目标重写：
 *   设备（签名+字节偏移 / 分区GUID+磁盘GUID）、加载器路径、SystemRoot、
 *   描述、locale、默认项、displayorder、超时；并删掉 WinRE/ramdisk 对象
 *   （默认不建恢复分区；要保留就 keep_recovery=true，并自行填 WinRE 文件）。
 * 32/64 位与分区表都不需要调用方区分——只要给对参数。
 */
typedef enum { IMG_BCD_FAM_AUTO = 0, IMG_BCD_FAM_LEGACY = 1, IMG_BCD_FAM_MODERN = 2 } img_bcd_family;
typedef struct {
    bool            uefi;
    img_bcd_family  family;        /* AUTO=按 modern 处理（调用方通常已用 win.version 定好）*/
    const char     *desc;          /* 菜单显示名，默认 "Windows" */
    const char     *locale;        /* 默认 "zh-CN" */
    int             timeout;       /* 默认 5 秒 */
    uint32_t        mbr_sig;       /* BIOS：磁盘签名 */
    uint64_t        sys_off, sr_off;  /* BIOS：字节偏移（sr_off=0 时 bootmgr 也用 sys_off）*/
    const char     *sys_guid, *disk_guid, *esp_guid;  /* UEFI */
    bool            keep_recovery; /* true=保留模板里的 WinRE/ramdisk 对象 */

    /* ★ 2026-11-04：模板导入（boot.tpl-apply）。
     *   非 NULL 时用【这份 BCD】当地座代替内置模板；设备对象照样按当前盘的
     *   签名/偏移/GUID 现生成 —— 所以导入的模板里就算残留了别人机器的绑定，
     *   也不会被写进目标盘。NULL = 用内置模板（老行为）。*/
    const void     *template_bytes;
    size_t          template_len;
} img_bcd_create_req_t;

int img_bcd_create_mem (const img_bcd_create_req_t *rq, void **out, size_t *out_len,
                        char *err, size_t errcap);
int img_bcd_create_file(const img_bcd_create_req_t *rq, const char *out_path,
                        char *err, size_t errcap);
int img_bcd_template_size(int family, bool uefi);   /* 诊断：内置模板字节数 */

/* 把一份 BCD 里【所有分区设备】的本机绑定清零（导出模板用）：
 *   MBR 设备 → [32..39] 字节偏移、[56..59] 磁盘签名 清零
 *   GPT 设备 → [32..47] 分区 GUID、[56..71] 磁盘 GUID 清零
 * 只认 88 字节且 type=6 的二进制元素，别的字节一个不动。
 * 返回 IMG_OK；*stripped 返回清了多少处。 */
int img_bcd_strip_devices(img_bcd *b, int *stripped);

/* 结构统计（模板元数据用）：对象数 / 元素总数 / 加载器数（12000002 含 winload）*/
typedef struct { int objects, elements, loaders, ramdisk; } img_bcd_stats_t;
int img_bcd_stats(img_bcd *b, img_bcd_stats_t *out);

/* MBR 分区设备（88 字节）。真机布局：type=6@16, len=72@24, 字节偏移@32,
 * flag=1@52, 磁盘签名@56。★ flag 那位漏了会 0xc000000e（设备不存在）。 */
int img_bcd_make_mbr_device(uint32_t sig, uint64_t off_bytes, unsigned char blob[88]);

/* MBR/BIOS 设备命门：读/写【磁盘签名 + 分区字节偏移】（见 img_bcd.c 布局注释） */
int img_bcd_get_mbr_binding(img_bcd *b,const char*obj_guid,const char*eid,
                            uint32_t *sig_out,uint64_t *off_out);
int img_bcd_set_mbr_binding(img_bcd *b,const char*obj_guid,const char*eid,
                            uint32_t sig,uint64_t off);

/* ---- 全对象扫描：MBR/BIOS 设备对象（type=6）批量改命门 ----
 * 一张 BIOS BCD 里通常不止三处设备（实测 Win11 参考件 5 处：
 *   bootmgr/memtest → 系统保留分区，resume/OSLoader/OSDevice → 系统分区）。
 * 只改已知三处会漏掉 memtest/resume；而在整个文件里裸替换签名字节又可能
 * 误伤恰好等于签名的其它数据。本函数按【hive 结构】遍历所有 type=6 设备：
 *   签名：sig==old_sig 时改成 new_sig（old_sig=0 表示全部改成 new_sig）
 *   偏移：命中 maps[i].old_off 时改成 maps[i].new_off；未命中保持原值
 * 只改 blob 的 [32..39]（偏移）与 [56..59]（签名），其它字节原样。
 * patched 返回实际改动的元素个数（可为 NULL）。 */
typedef struct { uint64_t old_off, new_off; } img_bcd_off_map;
int img_bcd_patch_all_mbr_bindings(img_bcd *b,
        uint32_t old_sig, uint32_t new_sig,
        const img_bcd_off_map *maps, size_t n_maps, int *patched);

/* 只读遍历所有 MBR/BIOS 设备对象（测试/诊断用）。
 * obj_guid 不带花括号。回调返回非 0 → 中止遍历并原样返回该值。 */
typedef int (*img_bcd_mbr_visit_fn)(void *ud, const char *obj_guid, const char *eid,
                                    uint32_t sig, uint64_t off);
int img_bcd_foreach_mbr_binding(img_bcd *b, img_bcd_mbr_visit_fn fn, void *ud);

/* 读 device 元素里的【分区 GUID】【磁盘 GUID】（文本形式，out 各需 ≥37 字节）。
 * 元素不是「分区设备」(type=6) 时返回 IMG_EFORMAT —— 换盘重建时用来分辨
 * 哪些 device 指向哪个分区（bootmgr 指 ESP、OS 加载器指系统分区）。 */
int img_bcd_get_device_guids(img_bcd *b, const char *obj_guid, const char *eid,
                             char *part_out, char *disk_out);

/* 枚举 Objects 下所有对象 GUID（不含花括号）。调用方用 img_bcd_free_object_list 释放 */
int  img_bcd_list_objects(img_bcd *b, char ***out_guids, size_t *out_n);
void img_bcd_free_object_list(char **guids, size_t n);
int img_bcd_set_ramdisksdi_partition(img_bcd *b, const char *ramdisk_guid,
                                     const char *part_guid, const char *disk_guid);

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
