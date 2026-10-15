/* img_fixboot.h — NTFS 引导区（$Boot）写入
 *
 * 网络核实结论（2026-10，Microsoft Learn + thestarman 实测）：
 *  · 官方工具是 bootsect.exe /nt60 <盘>，它【只改分区 VBR】（加 /mbr 才动 MBR）
 *  · NTFS 的 $Boot 是 15 扇区；mkntfs 只写扇区0，扇区1..14 的引导代码它不写
 *  · 扇区1 里 `07 00 42 00 4f 00 4f 00 54 00 4d 00 47 00 52 00` 是
 *    长度前缀的 UTF-16 "BOOTMGR" —— 引导代码要加载的文件名
 *  · ★ 实测比对：真实 Windows 安装的 $Boot 与 Xlite 的前 5120 字节
 *    仅差 16 字节（全在 BPB/序列号区），【引导代码逐字节相同】
 *    → 扇区1..9 与扇区0 的 0x54..0x1FD 是【与卷无关】的官方机器码
 *
 * 因此采用比"复制10扇区+改11字节"更干净的算法：
 *   ① mkntfs 格式化 → BPB/序列号/总扇区数 本来就是对的
 *   ② 只把模板的 [0x54..0x1FD] 写回扇区0  ← 不碰 BPB
 *   ③ 扇区 1..9 原样拷贝                  ← 无卷相关数据
 *
 * 许可：这段是【微软专有代码】，本仓库【不内嵌】，只从调用方提供的
 *       模板（参考镜像 / ISO）读取 —— 由 App 侧决定素材来源。
 */
#ifndef IMG_FIXBOOT_H
#define IMG_FIXBOOT_H
#include "img_qcow2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从模板镜像的 tpl_lba 取引导代码，写到 dst 的 dst_lba。
 * dst 与 tpl 可以是同一个镜像（自举）。
 * 完成后 dst 的扇区0 = 自己的 BPB + 模板的引导代码，扇区1..9 = 模板原样。 */
int img_fixboot(img_qcow2 *dst, uint64_t dst_lba,
                img_qcow2 *tpl, uint64_t tpl_lba, img_prog *prog);

/* 只读校验：dst 的引导代码是否与模板一致（排除 BPB 后逐字节）
 * out_detail 可为 NULL；返回 IMG_OK=一致 */
int img_fixboot_verify(img_qcow2 *dst, uint64_t dst_lba,
                       img_qcow2 *tpl, uint64_t tpl_lba,
                       char *out_detail, int detail_cap);

#ifdef __cplusplus
}
#endif
#endif
