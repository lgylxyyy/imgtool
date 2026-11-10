/* img_qcow2.h — qcow2 镜像读写接口
 *
 * 规范: qemu/docs/interop/qcow2.txt
 *   - 全镜像【大端序】
 *   - 三级映射: header → L1 → L2 → host cluster
 *   - L2 bit62 = 1 表示【压缩簇】(raw deflate)
 *   - L1/L2 地址掩码 0x00fffffffffffe00（低位 9 bit 是 flags）
 */
#ifndef IMG_QCOW2_H
#define IMG_QCOW2_H
#include "img_types.h"
#include "img_io.h"
#include "img_progress.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 头部魔数 'Q','F','I',0xfb */
#define IMG_QCOW2_MAGIC 0x514649fbU

/* incompatible 特性位 */
#define IMG_QCOW2_INCOMPAT_ENCRYPTED   (1ULL<<0)  /* 加密 → 拒绝 */
#define IMG_QCOW2_INCOMPAT_DATA_FILE   (1ULL<<1)  /* 外部数据文件 → 拒绝 */
#define IMG_QCOW2_INCOMPAT_CORRUPT     (1ULL<<2)
#define IMG_QCOW2_INCOMPAT_DIRTY       (1ULL<<3)
#define IMG_QCOW2_INCOMPAT_ACTIVE_L2   (1ULL<<4)
#define IMG_QCOW2_INCOMPAT_BITMASK     (1ULL<<5)
#define IMG_QCOW2_INCOMPAT_RAW_CLUSTERS (1ULL<<6)  /* compression_type 要看值 */
#define IMG_QCOW2_INCOMPAT_EXT_L2      (1ULL<<7)

/* compatible / ro 特性位 */
#define IMG_QCOW2_COMPAT_DIRTY_BITMAPS (1ULL<<0)
#define IMG_QCOW2_RO_INCONSIST_SNAP    (1ULL<<0)

typedef enum {
    IMG_QCOW2_COMP_ZLIB = 0,
    IMG_QCOW2_COMP_ZSTD = 1,
} img_qcow2_compression_t;

typedef struct {
    uint32_t magic;
    uint32_t version;            /* 2 或 3 */
    uint32_t backing_file_offset;
    uint32_t backing_file_size;
    uint32_t cluster_bits;       /* e.g. 16 → 64KB 簇 */
    uint64_t cluster_size;       /* 1<<cluster_bits */
    uint64_t size;               /* 虚拟(guest)总大小 */
    uint64_t crypt_method;
    uint32_t l1_size;
    uint64_t l1_table_offset;
    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;
    uint32_t nb_snapshots;
    uint64_t snapshots_offset;
    uint64_t incompatible;
    uint64_t compatible;
    uint64_t read_only;
    uint32_t refcount_order;     /* 96-99: refcount_bits = 1<<order (v2 恒为 4) */
    uint32_t header_length;      /* 100-103: v3 >=104 且是 8 的倍数 */
    uint16_t compression_type;   /* 104: 仅当 header_length>104 */
    uint64_t l2_entries;         /* cluster_size/8 */
    uint64_t l1_span_bytes;      /* 一张 L2 覆盖的 guest 字节数 */
} img_qcow2_header_t;

typedef struct img_qcow2 img_qcow2;

typedef struct {
    bool        read_only;
    bool        allow_zstd;      /* 编入 libzstd 时默认允许；没有解码器则一律拒绝 */
    img_progress_cb cb;          /* 进度回调，可为 NULL */
    void       *cb_ud;
} img_qcow2_opts_t;

/* ---- 打开/关闭 ---- */
img_qcow2 *img_qcow2_open(img_io *io, const img_qcow2_opts_t *o);   /* 接管 io 所有权 */
void       img_qcow2_close(img_qcow2 *q);
const img_qcow2_header_t *img_qcow2_header(const img_qcow2 *q);
uint64_t   img_qcow2_size(const img_qcow2 *q);

/* ---- 读 ---- */
/* 读 guest 空间 [off, off+len)；稀疏区域补 0；返回 IMG_OK 或错误 */
int img_qcow2_read(img_qcow2 *q, uint64_t off, void *buf, size_t len);
int img_qcow2_read_full(img_qcow2 *q, uint64_t off, void *buf, size_t len);
/* ★ 紧凑另存（img.compact）用：guest 区间 [off, off+len) 里有没有【已分配】的簇。
 *   未分配的簇读出来必然是零 —— 连读都不用读，直接跳过（这才是"只写已分配簇"）。
 *   不分配内存、不做 I/O 以外的副作用；出错返回错误码（*any 保持 false）。*/
int img_qcow2_any_allocated(img_qcow2 *q, uint64_t off, uint64_t len, bool *any);

/* ---- 写（阶段1 拆步实现）---- */
int img_qcow2_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len);

/* 为写入做准备：计算 refcount 布局、追加分配水位。
 * 打开镜像后若要写入，必须先调用一次（否则返回 IMG_ESTATE）。 */
int img_qcow2_write_init(img_qcow2 *q);

/* ---- 创建 / 扩盘 ---- */
typedef struct {
    uint64_t size;               /* 虚拟磁盘大小（字节） */
    uint32_t cluster_bits;       /* 0 → 默认 16 (64KB) */
    bool     compressed;         /* 写入时是否压缩簇 */
    uint32_t refcount_bits;      /* 0 → 默认 16 */
} img_qcow2_create_opts_t;

int img_qcow2_create(img_io *dest, const img_qcow2_create_opts_t *o);
/* 扩大虚拟磁盘（只改头 size；L1 不够时才追加 L1 簇） */
int img_qcow2_expand(img_qcow2 *q, uint64_t new_size, img_prog *prog);

/* ---- 信息 ---- */
/* 把信息渲染成 JSON（前端面板直接用） */
int img_qcow2_info_json(const img_qcow2 *q, char *buf, int cap);

/* ------------------------------------------------------------------ */
/* qcow2 一致性扫描（qc.verify / 写前自检）                             */
/* ------------------------------------------------------------------ */
/* 为什么要有它：进程被杀（手机上必然发生）时，qcow2 的 L1/L2/refcount 可能
 * 只写了一半。下次打开再往里写，如果分配水位算错、或 L2 里已经有指向新簇的
 * 条目，就会出现"两个 guest 区指向同一个 host 簇"（别名）——
 * 表现是 NTFS 说"索引块签名错误"（实测 Win8 x86 灌 WIM 撞过一次）。
 * 我们不猜，直接扫一遍 L1/L2：别名、越界、L2 表本身不可读，都能查出来。
 *
 * scan 的 O(#L2 表)：128 GiB 镜像 ≈ 256 张 L2 表 ≈ 16 MB 读 —— 打开一次可接受。*/
typedef struct {
    uint64_t guest_bytes;        /* 虚拟大小 */
    uint64_t guest_clusters;     /* 虚拟簇数 */
    uint64_t l2_tables;          /* 实际存在的 L2 表数 */
    uint64_t referenced;         /* 被引用的 host 簇数（含 L2 表自身）*/
    uint64_t max_ref_end;        /* 被引用簇的最大结束偏移 → 分配水位下限 */
    uint64_t duplicate_refs;     /* ★两个 guest 簇指向同一 host 簇（别名）*/
    uint64_t out_of_range;       /* 指向文件外 / 未对齐 */
    uint64_t bad_l2;             /* L2 表无法读取来（偏移非法/读失败）*/
} img_qcow2_scan_t;

/* 扫描（只读，不改文件）。返回 IMG_OK 表示扫完了；扫描结果里的计数才是结论。*/
int img_qcow2_scan(img_qcow2 *q, img_qcow2_scan_t *out);



#ifdef __cplusplus
}
#endif
#endif
