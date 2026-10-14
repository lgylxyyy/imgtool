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
    bool        allow_zstd;      /* 默认 false → zstd 镜像拒绝（宁可报错不静默写坏） */
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

#ifdef __cplusplus
}
#endif
#endif
