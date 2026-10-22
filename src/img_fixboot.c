#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_fixboot.c — NTFS 引导区写入（算法见头文件注释） */
#include "img_fixboot.h"
#include "img_vbr_tpl.h"
#include <string.h>
#include <stdio.h>

/* 扇区0 中【引导代码】的范围：BPB/EBPB 结束后到签名前
 *   0x00..0x0A  jump + OEM  "NTFS    "      （mkntfs 已写对）
 *   0x0B..0x53  BPB + 扩展 BPB + 卷序列号    （卷相关，mkntfs 写对）
 *   0x54..0x1FD 引导代码                      ← 只有这一段要从模板取
 *   0x1FE..0x1FF 55 AA                         （mkntfs 已写对）*/
#define CODE_START 0x54
#define CODE_END   0x1FE      /* 不含 */

int img_fixboot_mem(img_qcow2 *dst, uint64_t dst_lba, const uint8_t *tsec_in, img_prog *prog){
    if(!dst||!tsec_in) return IMG_EINVAL;
    const uint8_t *tsec = tsec_in;
    uint8_t dsec[512];
    int rc = img_qcow2_read_full(dst, dst_lba*512ULL, dsec, 512);
    if(rc) return rc;

    if(tsec[510]!=0x55 || tsec[511]!=0xAA) return IMG_ECORRUPT;   /* 模板不是引导扇区 */
    if(memcmp(tsec+3, "NTFS", 4)!=0)        return IMG_EFORMAT;   /* 模板不是 NTFS */

    if(prog){ img_prog_phase(prog, IMG_PH_FORMAT, "写入 NTFS 引导代码");
              img_prog_op(prog, IMG_OP_WRITE); img_prog_count(prog,0,10); }

    /* ① 扇区0 = 自己的 [0x00..0x53] + 模板的 [0x54..0x1FD] + 模板签名 */
    /* ★ BPB 几何字段：mkntfs 留 0，Windows 引导代码靠它算 CHS；
     *   不补 → SeaBIOS "A disk read error occurred"（实测 2026-10-20）。 */
    dsec[0x18] = 63;               /* sectors per track */
    dsec[0x1A] = 255;              /* heads */
    { uint32_t hid=(uint32_t)dst_lba; memcpy(dsec+0x1C, &hid, 4); }
    memcpy(dsec + CODE_START, tsec + CODE_START, CODE_END - CODE_START);
    dsec[510] = 0x55; dsec[511] = 0xAA;
    rc = img_qcow2_write(dst, dst_lba*512ULL, dsec, 512);
    if(rc) return rc;
    if(prog) img_prog_count(prog,1,10);

    /* ② 扇区1..9 原样拷贝（实测与卷无关，逐字节与真实 Windows 相同）*/
    for(int s=1;s<10;s++){
        rc = img_qcow2_write(dst, (dst_lba + (uint64_t)s)*512ULL, tsec + s*512, 512);
        if(rc) return rc;
        if(prog) img_prog_count(prog, s+1, 10);
    }
    if(prog) img_prog_done(prog, "NTFS 引导代码写入完成");
    return IMG_OK;
}

int img_ntfs_fix_geometry(img_qcow2 *q, uint64_t lba){
    if(!q) return IMG_EINVAL;
    uint8_t s[512];
    int rc = img_qcow2_read_full(q, lba*512ULL, s, sizeof s);
    if(rc) return rc;
    if(memcmp(s+3, "NTFS    ", 8)!=0) return IMG_EFORMAT;
    s[0x18] = 63;
    s[0x1A] = 255;
    { uint32_t hid=(uint32_t)lba; memcpy(s+0x1C, &hid, 4); }
    return img_qcow2_write(q, lba*512ULL, s, sizeof s);
}

int img_fixboot_verify_mem(img_qcow2 *dst, uint64_t dst_lba, const uint8_t *tsec_in,
                           char *out_detail, int detail_cap){
    if(!dst||!tsec_in) return IMG_EINVAL;
    const uint8_t *tsec = tsec_in;
    uint8_t dsec[10*512];
    int rc = img_qcow2_read_full(dst, dst_lba*512ULL, dsec, sizeof dsec);
    if(rc) return rc;

    int code_diff=0, sec19_diff=0;
    for(int i=CODE_START;i<CODE_END;i++) if(tsec[i]!=dsec[i]) code_diff++;
    for(int i=512;i<10*512;i++)         if(tsec[i]!=dsec[i]) sec19_diff++;
    if(out_detail && detail_cap>0)
        snprintf(out_detail,(size_t)detail_cap,
                 "扇区0引导代码差异 %d 字节 · 扇区1..9 差异 %d 字节", code_diff, sec19_diff);
    return (code_diff==0 && sec19_diff==0) ? IMG_OK : IMG_ECRC;
}

/* 从模板镜像取 10 个扇区，再走 mem 版（保持旧调用方不变）*/
int img_fixboot(img_qcow2 *dst, uint64_t dst_lba, img_qcow2 *tpl,
                uint64_t tpl_lba, img_prog *prog){
    if(!dst||!tpl) return IMG_EINVAL;
    uint8_t tsec[10*512];
    int rc = img_qcow2_read_full(tpl, tpl_lba*512ULL, tsec, sizeof tsec);
    if(rc) return rc;
    return img_fixboot_mem(dst, dst_lba, tsec, prog);
}
/* 用编译期内置模板（reference/ntfs-vbr.bin 的逐字节副本）——
 * 这样"造 BIOS 启动盘"不再需要用户手里另有一张带引导代码的盘。*/
int img_fixboot_builtin(img_qcow2 *dst, uint64_t dst_lba, img_prog *prog){
    return img_fixboot_mem(dst, dst_lba, IMG_VBR_TPL, prog);
}
int img_fixboot_verify_builtin(img_qcow2 *dst, uint64_t dst_lba, char *out_detail, int detail_cap){
    return img_fixboot_verify_mem(dst, dst_lba, IMG_VBR_TPL, out_detail, detail_cap);
}

int img_fixboot_verify(img_qcow2 *dst, uint64_t dst_lba, img_qcow2 *tpl,
                       uint64_t tpl_lba, char *out_detail, int detail_cap){
    if(!dst||!tpl) return IMG_EINVAL;
    uint8_t tsec[10*512];
    int rc = img_qcow2_read_full(tpl, tpl_lba*512ULL, tsec, sizeof tsec);
    if(rc) return rc;
    return img_fixboot_verify_mem(dst, dst_lba, tsec, out_detail, detail_cap);
}
