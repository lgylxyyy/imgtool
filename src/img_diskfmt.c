/* img_diskfmt.c — 磁盘格式探测与 raw 后端（P3）
 *
 * 设计：img_qcow2 是"块设备读写"的统一类型（历史原因叫 qcow2），
 *       186 处调用点不动。这里只加格式探测 + 各格式的 read/write/size。
 *
 * 探测顺序（按魔数，不靠扩展名）：
 *   ① qcow2  magic "QFI\xfb" @ 0
 *   ② VHD    footer "conectix" @ 文件尾 -512
 *   ③ VMDK   sparse magic "KDMV" @ 0（小端 0x564D444B）
 *   ④ raw    以上都不是 → 裸字节
 *
 * raw 后端的实现就是 img_io 的直通 —— raw 是"块设备语义"的零成本实现。
 */
#include "img_qcow2_int.h"
#include "img_qcow2.h"
#include "img_io.h"
#include "img_types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* qcow2 魔数（大端 0x514649fb） */
#define QCOW2_MAGIC_BE 0x514649fbU
/* VMDK sparse 魔数（小端 "KDMV"） */
#define VMDK_SPARSE_MAGIC 0x564d444bU
/* VHD footer cookie */
static const char VHD_COOKIE[8] = {'c','o','n','e','c','t','i','x'};

/* 探测格式。返回 DISK_FMT_*；io 必须已打开且可读。 */
disk_fmt img_diskfmt_probe(img_io *io){
    if(!io) return DISK_FMT__COUNT;

    uint8_t b[8] = {0};
    if(img_io_read_full(io, 0, b, 8) != IMG_OK) return DISK_FMT__COUNT;

    /* ① qcow2 */
    if(((uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3]) == QCOW2_MAGIC_BE)
        return DISK_FMT_QCOW2;

    /* ② VMDK sparse */
    if(((uint32_t)b[3]<<24 | (uint32_t)b[2]<<16 | (uint32_t)b[1]<<8 | b[0]) == VMDK_SPARSE_MAGIC)
        return DISK_FMT_VMDK;

    /* ③ VHD：footer 在文件尾 -512 */
    uint64_t sz = img_io_size(io);
    if(sz >= 512){
        uint8_t footer[8] = {0};
        if(img_io_read_full(io, sz - 512, footer, 8) == IMG_OK &&
           !memcmp(footer, VHD_COOKIE, 8))
            return DISK_FMT_VHD;
    }

    /* ④ 都不是 → raw */
    return DISK_FMT_RAW;
}

const char *img_diskfmt_name(disk_fmt f){
    switch(f){
    case DISK_FMT_QCOW2: return "qcow2";
    case DISK_FMT_RAW:   return "raw";
    case DISK_FMT_VHD:   return "vhd";
    case DISK_FMT_VMDK:  return "vmdk";
    default:             return "?";
    }
}

/* ---------------- raw 后端 ---------------- */
/* raw 的"guest 偏移" == "宿主文件偏移"，直接转发给 img_io。
 * 稀疏：raw 文件短于请求范围时，超出部分补 0（与 qcow2 语义一致）。 */
int img_raw_read(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    if(!q || !q->io || !buf) return IMG_EINVAL;
    uint8_t *out = (uint8_t*)buf;
    uint64_t fsz = q->raw_size;

    size_t done = 0;
    if(off < fsz){
        size_t want = len;
        if(off + want > fsz) want = (size_t)(fsz - off);
        size_t got = 0;
        int rc = img_io_read(q->io, off, out, want, &got);
        if(rc != IMG_OK) return rc;
        done = got;
    }
    if(done < len) memset(out + done, 0, len - done);
    return IMG_OK;
}

int img_raw_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
    if(!q || !q->io || !buf) return IMG_EINVAL;
    if(q->read_only) return IMG_EREADONLY;
    size_t done = 0;
    int rc = img_io_write_full(q->io, off, buf, len);
    if(rc != IMG_OK) return rc;
    (void)done;
    /* 写到文件尾之外 → 文件变长 */
    if(off + len > q->raw_size) q->raw_size = off + len;
    return IMG_OK;
}

/* ---------------- VHD / VMDK 后端（P3-4/P3-5，本文件先给桩）----------------
 * 后续补 img_vhd.c / img_vmdk.c 时把这几个转发过去。
 * 现在先返回 ENOTSUP，保证链接通过 + 明确报错（宁可报错不静默写坏）。 */
int img_fmt_open(img_qcow2 *q){
    if(q->fmt == DISK_FMT_VHD)  return img_vhd_open(q);
    if(q->fmt == DISK_FMT_VMDK) return img_vmdk_open(q);
    return IMG_ENOTSUP;
}
void img_fmt_close(img_qcow2 *q){
    if(q->fmt == DISK_FMT_VHD)  img_vhd_close(q);
    if(q->fmt == DISK_FMT_VMDK) img_vmdk_close(q);
}
int img_fmt_read(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    if(q->fmt == DISK_FMT_VHD)  return img_vhd_read(q, off, buf, len);
    if(q->fmt == DISK_FMT_VMDK) return img_vmdk_read(q, off, buf, len);
    return IMG_ENOTSUP;
}
int img_fmt_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
    if(q->fmt == DISK_FMT_VHD)  return img_vhd_write(q, off, buf, len);
    if(q->fmt == DISK_FMT_VMDK) return img_vmdk_write(q, off, buf, len);
    return IMG_ENOTSUP;
}
