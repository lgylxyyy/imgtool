#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_fixboot.c — NTFS 引导区写入（算法见头文件注释） */
#include "img_fixboot.h"
#include <string.h>
#include <stdio.h>

/* 扇区0 中【引导代码】的范围：BPB/EBPB 结束后到签名前
 *   0x00..0x0A  jump + OEM  "NTFS    "      （mkntfs 已写对）
 *   0x0B..0x53  BPB + 扩展 BPB + 卷序列号    （卷相关，mkntfs 写对）
 *   0x54..0x1FD 引导代码                      ← 只有这一段要从模板取
 *   0x1FE..0x1FF 55 AA                         （mkntfs 已写对）*/
#define CODE_START 0x54
#define CODE_END   0x1FE      /* 不含 */

int img_fixboot(img_qcow2 *dst, uint64_t dst_lba, img_qcow2 *tpl,
                uint64_t tpl_lba, img_prog *prog){
    if(!dst||!tpl) return IMG_EINVAL;
    uint8_t tsec[10*512], dsec[512];
    int rc = img_qcow2_read_full(tpl, tpl_lba*512ULL, tsec, sizeof tsec);
    if(rc) return rc;
    rc = img_qcow2_read_full(dst, dst_lba*512ULL, dsec, 512);
    if(rc) return rc;

    if(tsec[510]!=0x55 || tsec[511]!=0xAA) return IMG_ECORRUPT;   /* 模板不是引导扇区 */
    if(memcmp(tsec+3, "NTFS", 4)!=0)        return IMG_EFORMAT;   /* 模板不是 NTFS */

    if(prog){ img_prog_phase(prog, IMG_PH_FORMAT, "写入 NTFS 引导代码");
              img_prog_op(prog, IMG_OP_WRITE); img_prog_count(prog,0,10); }

    /* ① 扇区0 = 自己的 [0x00..0x53] + 模板的 [0x54..0x1FD] + 模板签名 */
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

int img_fixboot_verify(img_qcow2 *dst, uint64_t dst_lba, img_qcow2 *tpl,
                       uint64_t tpl_lba, char *out_detail, int detail_cap){
    if(!dst||!tpl) return IMG_EINVAL;
    uint8_t tsec[10*512], dsec[10*512];
    int rc = img_qcow2_read_full(tpl, tpl_lba*512ULL, tsec, sizeof tsec);
    if(rc) return rc;
    rc = img_qcow2_read_full(dst, dst_lba*512ULL, dsec, sizeof dsec);
    if(rc) return rc;

    int code_diff=0, sec19_diff=0;
    for(int i=CODE_START;i<CODE_END;i++) if(tsec[i]!=dsec[i]) code_diff++;
    for(int i=512;i<10*512;i++)         if(tsec[i]!=dsec[i]) sec19_diff++;
    if(out_detail && detail_cap>0)
        snprintf(out_detail,(size_t)detail_cap,
                 "扇区0引导代码差异 %d 字节 · 扇区1..9 差异 %d 字节", code_diff, sec19_diff);
    return (code_diff==0 && sec19_diff==0) ? IMG_OK : IMG_ECRC;
}
