/* img_qcow2_int.h — 内部共享结构（不对外暴露） */
#ifndef IMG_QCOW2_INT_H
#define IMG_QCOW2_INT_H
#include "img_qcow2.h"

/* 地址掩码与压缩位 */
#define QCOW2_OFFSET_MASK  0x00fffffffffffe00ULL
#define QCOW2_COMPRESSED_BIT (1ULL<<62)
/* 规范：bit63 = QCOW_OFLAG_COPIED —— 当该簇 refcount==1 时必须置位，
   否则 qemu-img check 报 "ERROR OFLAG_COPIED ... refcount=1"。
   我们的分配器是追加式、每个簇恒为 refcount=1 → 写入时总是置位。 */
#define QCOW2_COPIED_BIT   (1ULL<<63)
#define QCOW2_L2_FLAG_MASK 0x00000000000003ffULL   /* L2 低 10 bit flags（含 bit62 在上面） */

/* refcount 条目宽度（默认 16 位） */
#define QCOW2_DEFAULT_REFCOUNT_BITS 16

/* ★ P3（2026-11-09）：磁盘格式分发。
 *   img_qcow2 原来是"qcow2 专用"类型，但全代码 186 处调用它做"块设备读写"。
 *   加一个 fmt 字段做内部分发，**不改任何调用点** —— 这是最小侵入方案。
 *   新增格式只需实现 3 个函数：fmt_read / fmt_write / fmt_size。 */
typedef enum {
    DISK_FMT_QCOW2 = 0,
    DISK_FMT_RAW,          /* 裸字节直通 */
    DISK_FMT_VHD,          /* Virtual PC / Hyper-V 旧格式（footer + 动态 BAT）*/
    DISK_FMT_VMDK,         /* VMware（描述符 + sparse extent）*/
    DISK_FMT__COUNT
} disk_fmt;

struct img_qcow2 {
    img_io  *io;
    bool     own_io;
    disk_fmt fmt;                    /* ★ P3：默认 QCOW2，open 时探测 */
    uint64_t raw_size;               /* ★ raw: 宿主文件大小（guest 大小）*/
    void    *fmt_priv;               /* ★ VHD/VMDK 私有结构（BAT / grain 表）*/
    img_qcow2_header_t h;
    uint8_t *l1;
    size_t   l1_bytes;
    img_progress_cb cb; void *cb_ud;
    bool     read_only;
    bool     allow_zstd;
    uint8_t *l2_cache;
    uint64_t l2_cache_off;
    bool     l2_cache_valid;

    /* ---- 写入状态 ---- */
    bool     writable;
    uint32_t refcount_bits;         /* 默认 16 */
    uint64_t refcount_table_bytes;  /* refcount_table_clusters * cluster_size */
    uint64_t entries_per_rc_block;  /* cluster_size / (refcount_bits/8) */
    uint64_t alloc_next;            /* 下一个可分配 host 偏移（cluster 对齐，= 追加式分配水位）*/
    uint64_t total_clusters;        /* 已用 host 簇数（用于 refcount 布局推算） */
    /* 写前自检结果（img_qcow2_write_init 里扫一遍 L1/L2 得到）*/
    bool     scan_done;
    uint64_t scan_dup, scan_oor, scan_max_end;
};

/* ---- img.trim 入口（img_qcow2_write.c 实现；外部别用）---- */
int qc2_l2_load(img_qcow2 *q, uint64_t l1i, uint64_t *l2_addr, uint8_t **out);
int qc2_l2_store(img_qcow2 *q, uint64_t l2_addr, const uint8_t *b);
int qc2_refcount_get(img_qcow2 *q, uint64_t host_idx, uint32_t *out);
int qc2_refcount_set(img_qcow2 *q, uint64_t host_idx, uint32_t v);

/* ---- P3 格式分发（img_diskfmt.c / img_vhd.c / img_vmdk.c 提供）---- */
disk_fmt    img_diskfmt_probe(img_io *io);
const char *img_diskfmt_name(disk_fmt f);
int         img_fmt_open(img_qcow2 *q);       /* VHD/VMDK 打开（按 q->fmt 分派）*/
void        img_fmt_close(img_qcow2 *q);
int         img_fmt_read(img_qcow2 *q, uint64_t off, void *buf, size_t len);
int         img_fmt_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len);
int         img_vhd_create(img_io *io, uint64_t bytes);
int         img_vmdk_create(img_io *io, uint64_t bytes);
int         img_vhd_open(img_qcow2 *q);
void        img_vhd_close(img_qcow2 *q);
int         img_vhd_read(img_qcow2 *q, uint64_t off, void *buf, size_t len);
int         img_vhd_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len);
int         img_vmdk_open(img_qcow2 *q);
void        img_vmdk_close(img_qcow2 *q);
int         img_vmdk_read(img_qcow2 *q, uint64_t off, void *buf, size_t len);
int         img_vmdk_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len);
int         img_raw_read(img_qcow2 *q, uint64_t off, void *buf, size_t len);
int         img_raw_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len);

/* 内部工具（img_qcow2.c 提供） */
void wr_be16_i(void *p, uint16_t v);
void wr_be32_i(void *p, uint32_t v);
void wr_be64_i(void *p, uint64_t v);
uint16_t rd_be16_i(const void *p);
uint32_t rd_be32_i(const void *p);
uint64_t rd_be64_i(const void *p);
#endif
