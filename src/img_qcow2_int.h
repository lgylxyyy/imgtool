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

struct img_qcow2 {
    img_io  *io;
    bool     own_io;
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
};

/* 内部工具（img_qcow2.c 提供） */
void wr_be16_i(void *p, uint16_t v);
void wr_be32_i(void *p, uint32_t v);
void wr_be64_i(void *p, uint64_t v);
uint16_t rd_be16_i(const void *p);
uint32_t rd_be32_i(const void *p);
uint64_t rd_be64_i(const void *p);
#endif
