/*
 * qcow2.h —— 自研 qcow2 读写库（P1）
 *
 * 依据：qemu/docs/interop/qcow2.txt（存于 qcow2-viewer/docs/qcow2-spec.txt）
 *
 * 设计约束（有意简化，见 docs/实现方案）：
 *   - 只支持 cluster_bits = 16（64KB），与我们产出的镜像一致
 *   - 只支持 refcount_bits = 16（v2 固定；v3 读头部）
 *   - 拒绝：加密、外部数据文件、bitmask、extended L2、zstd
 *   - P1 阶段不写压缩簇（读支持）
 */
#ifndef QCOW2_H
#define QCOW2_H

#include <stdint.h>
#include <stddef.h>

/* 打开模式 */
#define QCOW2_RDONLY 0
#define QCOW2_RDWR   1

typedef struct Qcow2Image Qcow2Image;

typedef struct {
    uint32_t version;
    uint32_t cluster_bits;
    uint64_t cluster_size;
    uint64_t size;               /* 虚拟磁盘字节数 */
    uint64_t l1_offset;
    uint32_t l1_size;            /* L1 项数 */
    uint64_t refcount_offset;
    uint32_t refcount_clusters;
    uint32_t refcount_order;
    uint32_t refcount_bits;
    uint64_t incompatible;
    uint64_t compatible;
    uint64_t autoclear;
    uint8_t  compression_type;   /* 0=zlib 1=zstd */
} Qcow2Info;

/* ---------- 生命周期 ---------- */
/* 打开镜像。失败返回 NULL，错误信息写入 errbuf。 */
Qcow2Image *qcow2_open(const char *path, int mode, char *errbuf, size_t errlen);

/* 创建新镜像。cluster_bits 通常 16，refcount_order 通常 4（=16 位）。 */
Qcow2Image *qcow2_create(const char *path, uint64_t virtual_size,
                         uint32_t cluster_bits, uint32_t refcount_order,
                         char *errbuf, size_t errlen);

/* 落盘并释放。返回 0 表示成功。 */
int qcow2_close(Qcow2Image *img, char *errbuf, size_t errlen);

/* 立即把元数据写回文件。 */
int qcow2_sync(Qcow2Image *img, char *errbuf, size_t errlen);

/* 查询信息 */
const Qcow2Info *qcow2_info(const Qcow2Image *img);

/* ---------- 读写 ---------- */
/* 读 count 字节到 buf，返回实际读到的字节数（稀疏区域补零）。失败 -1。 */
int64_t qcow2_read(Qcow2Image *img, uint64_t offset, void *buf, uint64_t count);

/* 写 count 字节。自动分配簇。返回实际写入字节数。失败 -1。 */
int64_t qcow2_write(Qcow2Image *img, uint64_t offset, const void *buf, uint64_t count);

/* ---------- 工具 ---------- */
/* 统计：已分配簇数 / 已用字节。用于校验与报告。 */
typedef struct {
    uint64_t allocated_clusters;
    uint64_t host_bytes;         /* 已分配簇占用的物理字节 */
    uint64_t compressed_clusters;
} Qcow2Stats;
int qcow2_stats(Qcow2Image *img, Qcow2Stats *st, char *errbuf, size_t errlen);

/* 最后一次错误（内部静态缓冲） */
const char *qcow2_last_error(void);

#endif /* QCOW2_H */
