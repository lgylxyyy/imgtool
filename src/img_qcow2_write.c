/* img_qcow2_write.c — qcow2 写入
 *
 * 分配策略：【追加式】—— 新簇一律从文件尾部拿。
 *   理由：简单、绝不会覆盖已用数据、无需空闲链表。
 *   代价：不复用已释放空间（阶段1可接受；阶段中后期再做回收）。
 *
 * refcount：按 host 簇索引，默认 16 位/条。
 *   refcount_block 覆盖 entries_per_rc_block 个 host 簇，按需分配。
 *
 * 验证路径（分步，绝不写坏用户数据）：
 *   create → qemu-img check → 转回 raw 逐字节比对
 *   append → qemu-img check → 读回 sha256
 *   overwrite → sha256 应恰好变成新值
 */
#include "img_qcow2_int.h"
#include "img_event.h"
#include "img_host.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef IMG_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef IMG_HAVE_ZSTD
#include <zstd.h>
#endif

/* ------------------------------------------------------------------ */
/* 内部小工具                                                          */
/* ------------------------------------------------------------------ */
static inline uint64_t align_up(uint64_t v, uint64_t a){ return (v + a - 1) & ~(a - 1); }

static inline int wr64(img_io *io, uint64_t off, uint64_t v){
    uint8_t b[8]; wr_be64_i(b, v);
    return img_io_write_full(io, off, b, 8);
}
static inline int rd64(img_io *io, uint64_t off, uint64_t *v){
    uint8_t b[8];
    int rc = img_io_read_full(io, off, b, 8);
    if(rc) return rc;
    *v = rd_be64_i(b);
    return IMG_OK;
}
static inline int wr32(img_io *io, uint64_t off, uint32_t v){
    uint8_t b[4]; wr_be32_i(b, v);
    return img_io_write_full(io, off, b, 4);
}
static inline int wr16(img_io *io, uint64_t off, uint16_t v){
    uint8_t b[2]; wr_be16_i(b, v);
    return img_io_write_full(io, off, b, 2);
}

/* ------------------------------------------------------------------ */
/* refcount                                                            */
/* ------------------------------------------------------------------ */

/* 不设 refcount 的裸分配（用于 refcount block 自身等） */
static int alloc_cluster_raw(img_qcow2 *q, uint64_t *out_off){
    /* ★ T6 之后 alloc_next 可能停在 512 对齐（压缩簇只占几扇区），
     *   普通簇必须重新对齐到整簇，否则会写进同一个簇的后半段。*/
    uint64_t off = align_up(q->alloc_next, q->h.cluster_size);
    if(off == 0){
        off = align_up(img_io_size(q->io), q->h.cluster_size);
        if(off == 0) off = q->h.cluster_size;   /* 至少从簇1开始 */
    }
    q->alloc_next = off;
    /* 延伸文件（写一个 0 簇保证真正占位；qcow2 要求文件包含真实字节） */
    uint8_t *z = calloc(1, (size_t)q->h.cluster_size);
    if(!z) return IMG_ENOMEM;
    int rc = img_io_write_full(q->io, off, z, (size_t)q->h.cluster_size);
    free(z);
    if(rc) return rc;
    q->alloc_next = off + q->h.cluster_size;
    if(q->alloc_next > q->total_clusters * q->h.cluster_size)
        q->total_clusters = q->alloc_next / q->h.cluster_size;
    *out_off = off;
    return IMG_OK;
}

/* 设置 host 簇 idx 的 refcount 值；必要时按需分配 refcount block */
static int set_refcount(img_qcow2 *q, uint64_t host_idx, uint32_t value){
    if(q->refcount_bits == 0 || q->refcount_bits > 32) return IMG_ESTATE;
    if(q->refcount_bits == 16){
        uint64_t block_i  = host_idx / q->entries_per_rc_block;
        uint64_t entry_i  = host_idx % q->entries_per_rc_block;
        if(block_i * 8 >= q->refcount_table_bytes) return IMG_EFULL;

        /* 1) 确保该 refcount block 存在 */
        uint64_t table_off = q->h.refcount_table_offset;
        uint64_t blk_off = 0;
        int rc = rd64(q->io, table_off + block_i*8, &blk_off);
        if(rc) return rc;
        if(blk_off == 0){
            uint64_t a = 0;
            if((rc = alloc_cluster_raw(q, &a)) != IMG_OK) return rc;
            /* 写回 table 条目 */
            if((rc = wr64(q->io, table_off + block_i*8, a)) != IMG_OK) return rc;
            /* 给这个新 refcount block 自己记 refcount（table 条目已就位，不会递归死循环） */
            if((rc = set_refcount(q, a / q->h.cluster_size, 1)) != IMG_OK) return rc;
            blk_off = a;
        }
        /* 2) 写条目 */
        uint64_t pos = blk_off + entry_i * 2;
        uint16_t v = (uint16_t)value;
        return wr16(q->io, pos, v);
    }
    /* 32 位 refcount（少见，但按同法处理） */
    uint64_t eprb = q->entries_per_rc_block;   /* entries_per_rc_block 是按 bits 算过的 */
    uint64_t block_i = host_idx / eprb, entry_i = host_idx % eprb;
    uint64_t table_off = q->h.refcount_table_offset;
    if(block_i * 8 >= q->refcount_table_bytes) return IMG_EFULL;
    uint64_t blk_off = 0;
    int rc = rd64(q->io, table_off + block_i*8, &blk_off);
    if(rc) return rc;
    if(blk_off == 0){
        uint64_t a=0;
        if((rc = alloc_cluster_raw(q,&a))!=IMG_OK) return rc;
        if((rc = wr64(q->io, table_off + block_i*8, a))!=IMG_OK) return rc;
        if((rc = set_refcount(q, a/q->h.cluster_size, 1))!=IMG_OK) return rc;
        blk_off = a;
    }
    uint64_t pos = blk_off + entry_i * 4;
    uint8_t b[4]; wr_be32_i(b, value);
    return img_io_write_full(q->io, pos, b, 4);
}

/* 带 refcount 的簇分配 */
static int alloc_cluster(img_qcow2 *q, uint64_t *out_off){
    uint64_t a=0;
    int rc = alloc_cluster_raw(q, &a);
    if(rc) return rc;
    if((rc = set_refcount(q, a / q->h.cluster_size, 1)) != IMG_OK) return rc;
    *out_off = a;
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* L2 表维护                                                           */
/* ------------------------------------------------------------------ */

/* 读出 L2 表所在簇的完整内容（alloc/free/写条目都要用） */
static int read_l2_cluster(img_qcow2 *q, uint64_t l2_addr, uint8_t **out){
    uint8_t *b = malloc((size_t)q->h.cluster_size);
    if(!b) return IMG_ENOMEM;
    int rc = img_io_read_full(q->io, l2_addr, b, (size_t)q->h.cluster_size);
    if(rc){ free(b); return rc; }
    *out = b;
    return IMG_OK;
}

/* 把 L2 表写回（同时失效读缓存） */
static int write_l2_cluster(img_qcow2 *q, uint64_t l2_addr, const uint8_t *b){
    int rc = img_io_write_full(q->io, l2_addr, b, (size_t)q->h.cluster_size);
    if(rc) return rc;
    q->l2_cache_valid = false;                /* 读缓存必须失效 */
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* L1 表维护                                                           */
/* ------------------------------------------------------------------ */

static int l1_get(img_qcow2 *q, uint64_t l1i, uint64_t *val){
    if(l1i >= q->h.l1_size) return IMG_ENOENT;
    *val = rd_be64_i(q->l1 + l1i*8);
    return IMG_OK;
}
static int l1_set(img_qcow2 *q, uint64_t l1i, uint64_t val){
    if(l1i >= q->h.l1_size) return IMG_EFULL;
    wr_be64_i(q->l1 + l1i*8, val);
    return img_io_write_full(q->io, q->h.l1_table_offset + l1i*8, q->l1 + l1i*8, 8);
}

/* ------------------------------------------------------------------ */
/* img.trim 要用的内部入口（img_trim.c 只拿这几个，不碰 static）          */
/* ------------------------------------------------------------------ */

/* 读 l1i 指向的 L2 表。返回 IMG_ENOENT = 这一层还没分配（正常）。*/
int qc2_l2_load(img_qcow2 *q, uint64_t l1i, uint64_t *l2_addr, uint8_t **out){
    if(!q || !l2_addr || !out) return IMG_EINVAL;
    *out = NULL; *l2_addr = 0;
    if(l1i >= q->h.l1_size) return IMG_ENOENT;
    uint64_t a = 0;
    int rc = l1_get(q, l1i, &a);
    if(rc) return rc;
    a &= QCOW2_OFFSET_MASK;
    *l2_addr = a;
    if(!a) return IMG_ENOENT;
    return read_l2_cluster(q, a, out);
}

int qc2_l2_store(img_qcow2 *q, uint64_t l2_addr, const uint8_t *b){
    if(!q || !l2_addr || !b) return IMG_EINVAL;
    return write_l2_cluster(q, l2_addr, b);
}

/* 读 host 簇的 refcount（16/32 位都认；表项为 0 → 0）*/
int qc2_refcount_get(img_qcow2 *q, uint64_t host_idx, uint32_t *out){
    if(!q || !out) return IMG_EINVAL;
    *out = 0;
    if(q->refcount_bits != 16 && q->refcount_bits != 32) return IMG_ENOTSUP;
    uint64_t eprb = q->entries_per_rc_block;
    if(!eprb) return IMG_ESTATE;
    uint64_t block_i = host_idx / eprb;
    if(block_i * 8 >= q->refcount_table_bytes) return IMG_ERANGE;
    uint64_t blk = 0;
    int rc = rd64(q->io, q->h.refcount_table_offset + block_i*8, &blk);
    if(rc) return rc;
    if(!blk) return IMG_OK;                    /* 没分配过 → refcount 0 */
    uint64_t pos = blk + (host_idx % eprb) * (q->refcount_bits/8);
    uint8_t b[4] = {0,0,0,0};
    rc = img_io_read_full(q->io, pos, b, q->refcount_bits/8);
    if(rc) return rc;
    *out = (q->refcount_bits == 16) ? (uint32_t)((b[0]<<8)|b[1])
                                    : (uint32_t)((b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]);
    return IMG_OK;
}

int qc2_refcount_set(img_qcow2 *q, uint64_t host_idx, uint32_t v){
    if(!q) return IMG_EINVAL;
    return set_refcount(q, host_idx, v);
}

/* ------------------------------------------------------------------ */
/* 创建全新 qcow2                                                      */
/* ------------------------------------------------------------------ */
/*
 * 布局（与 QEMU 一致）：
 *   cluster 0 : header
 *   cluster 1 : refcount 表
 *   cluster 2 : refcount block 0
 *   cluster 3 : L1 表
 *   cluster 4+: 数据 / L2 / 后续 refcount block
 */
int img_qcow2_create(img_io *dest, const img_qcow2_create_opts_t *o){
    if(!dest || !o || o->size == 0) return IMG_EINVAL;
    if(!img_io_writable(dest)) return IMG_EINVAL;

    uint32_t cb = o->cluster_bits ? o->cluster_bits : 16;   /* 默认 64KB */
    if(cb < 9 || cb > 21) return IMG_EINVAL;
    uint64_t cs = 1ULL << cb;
    uint64_t rcb = o->refcount_bits ? o->refcount_bits : 16;
    if(rcb != 16 && rcb != 32) return IMG_ENOTSUP;
    /* 压缩算法：0=zlib（默认）、1=zstd。zstd 是 qcow2 v3 的 incompatible 特性，
     * 老工具（QEMU < 2.10）会拒绝打开整个映像 —— 所以必须显式要才写。
     * 本构建没编 libzstd 时直接拒绝（写了也解不开，不能造出自己读不了的盘）。 */
    uint8_t ctype = o ? o->compression_type : 0;
    if(ctype == IMG_QCOW2_COMP_ZSTD){
#ifndef IMG_HAVE_ZSTD
        return IMG_ENOTSUP;      /* 本构建没有 zstd：不能造自己读不了的映像 */
#else
        /* ok：zstd 只允许 v3（本函数恒写 v3 头，天然满足） */
#endif
    } else if(ctype != IMG_QCOW2_COMP_ZLIB){
        return IMG_EINVAL;       /* 未知的 compression_type */
    }
    /* incompatible 特性位（offset 72..79）：zstd = bit20「compression type」 */
    uint64_t incompat = (ctype == IMG_QCOW2_COMP_ZSTD)
                      ? IMG_QCOW2_INCOMPAT_COMP_TYPE : 0;

    uint64_t l2_entries = cs / 8;
    uint64_t l1_span    = cs * l2_entries;          /* 单张 L2 覆盖量 */
    uint64_t l1_size    = (o->size + l1_span - 1) / l1_span;
    if(l1_size == 0) l1_size = 1;
    if(l1_size * 8 > cs) return IMG_ENOTSUP;        /* 单簇放不下 L1（>4TB），暂不支持 */

    const uint64_t OFF_RCT = cs * 1;                /* refcount 表 */
    const uint64_t OFF_RCB = cs * 2;                /* refcount block 0 */
    const uint64_t OFF_L1  = cs * 3;                /* L1 表 */

    /* ---- 写头 ---- */
    /* 112 字节：104..103 是标准 v3 头，104 是 compression_type（header_length
     * 必须 >104 qemu 才会去读它），105..111 是保留的 0 填充（8 字节对齐）。 */
    uint8_t hdr[112];
    memset(hdr, 0, sizeof hdr);
    hdr[0]='Q'; hdr[1]='F'; hdr[2]='I'; hdr[3]=0xfb;
    wr_be32_i(hdr+4, 3);                            /* version 3 */
    wr_be32_i(hdr+20, cb);                          /* cluster_bits (规范 20-23) */
    wr_be64_i(hdr+24, o->size);                     /* 虚拟大小 */
    wr_be32_i(hdr+36, (uint32_t)l1_size);           /* l1_size */
    wr_be64_i(hdr+40, OFF_L1);                      /* l1_table_offset */
    wr_be64_i(hdr+48, OFF_RCT);                     /* refcount_table_offset */
    wr_be32_i(hdr+56, 1);                           /* refcount_table_clusters = 1 */
    wr_be32_i(hdr+60, 0);                           /* nb_snapshots */
    wr_be64_i(hdr+64, 0);                           /* snapshots_offset */
    /* v3 特性区（72..95）：incompatible 位里把「compression type」置上；
     * 其余（加密/外部文件/脏位/扩展 L2）保持 0。 */
    wr_be64_i(hdr+72, incompat);
    wr_be32_i(hdr+96,  (uint32_t)(rcb==16 ? 4 : 5));  /* 96-99  refcount_order: 16位→4, 32位→5 */
    wr_be32_i(hdr+100, 112);                         /* 100-103 header_length (>104：含 compression_type) */
    hdr[104] = ctype;                                /* 104     compression_type（0=zlib 1=zstd） */
    int rc = img_io_write_full(dest, 0, hdr, sizeof hdr);
    if(rc) return rc;

    /* ---- refcount 表（1 簇） ---- */
    uint8_t *z = calloc(1,(size_t)cs);
    if(!z) return IMG_ENOMEM;
    rc = img_io_write_full(dest, OFF_RCT, z, (size_t)cs);
    if(rc){ free(z); return rc; }

    /* ---- L1 表 ---- */
    memset(z, 0, (size_t)cs);
    rc = img_io_write_full(dest, OFF_L1, z, (size_t)cs);
    if(rc){ free(z); return rc; }

    /* ---- refcount block 0（覆盖 host 簇 0..entries-1）---- */
    memset(z, 0, (size_t)cs);
    /* cluster 0..3 已被占用 → refcount = 1 */
    if(rcb == 16){
        for(int i=0;i<4;i++){
            z[i*2]   = 0; z[i*2+1] = 1;             /* BE16 = 1 */
        }
    } else {
        for(int i=0;i<4;i++) wr_be32_i(z+i*4, 1);
    }
    rc = img_io_write_full(dest, OFF_RCB, z, (size_t)cs);
    free(z);
    if(rc) return rc;

    /* refcount 表指向 block 0 */
    rc = wr64(dest, OFF_RCT, OFF_RCB);
    if(rc) return rc;

    /* 预置文件大小（稀疏占位，不占真实空间） */
    rc = img_io_truncate(dest, cs*4);
    if(rc) return rc;

    return img_io_sync(dest);
}

/* ------------------------------------------------------------------ */
/* 打开后为写模式准备状态                                              */
/* ------------------------------------------------------------------ */
int img_qcow2_write_init(img_qcow2 *q){
    if(!q) return IMG_EINVAL;
    /* ★ P3：raw / VHD / VMDK 无 qcow2 结构（无 L1/L2/refcount），不需要准备 */
    if(q->fmt == DISK_FMT_RAW || q->fmt == DISK_FMT_VHD || q->fmt == DISK_FMT_VMDK)
        return IMG_OK;
    uint64_t cs = q->h.cluster_size;
    uint32_t rcb = q->refcount_bits ? q->refcount_bits : 16;
    q->refcount_bits = rcb;
    q->entries_per_rc_block = cs / (rcb/8);
    q->refcount_table_bytes = (uint64_t)q->h.refcount_table_clusters * cs;
    /* 追加水位 = 文件尾（cluster 对齐） */
    q->alloc_next = align_up(img_io_size(q->io), cs);
    if(q->alloc_next == 0) q->alloc_next = cs;
    q->total_clusters = (img_io_size(q->io) + cs - 1) / cs;

    /* ★ 写前自检：扫一遍 L1/L2。
     *   ① 分配水位不能只看文件尾 —— 如果进程被杀时"数据簇写了、文件却短了一截"，
     *      继续从文件尾追加就会覆盖别处引用到的簇（别名）→ 上层看到"索引块签名错"。
     *      改成 max(文件尾, 已引用簇末尾)。
     *   ② 已经有别名/越界的镜像，先在日志里喊一声，别让它静默扩散。*/
    img_qcow2_scan_t sc;
    if(img_qcow2_scan(q, &sc) == IMG_OK){
        q->scan_done = true;
        q->scan_dup = sc.duplicate_refs;
        q->scan_oor = sc.out_of_range + sc.bad_l2;
        q->scan_max_end = sc.max_ref_end;
        if(sc.max_ref_end > q->alloc_next) q->alloc_next = align_up(sc.max_ref_end, cs);
        if(sc.duplicate_refs || q->scan_oor){
            img_ev e;
            if(img_ev_init(&e, IMG_LV_WARN, "qcow2", "inconsistent", IMG_OK,
                           "映像里有别名/越界簇：写入前已把分配水位抬高，避免继续写坏"))
            {
                img_ev_u64(&e, "duplicate_refs", sc.duplicate_refs);
                img_ev_u64(&e, "out_of_range", q->scan_oor);
                img_ev_u64(&e, "l2_tables", sc.l2_tables);
                img_ev_emit(&e);
            }
        }
    }
    q->writable = true;
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* 写 guest 空间                                                       */
/* ------------------------------------------------------------------ */
int img_qcow2_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
    /* ★ P3：非 qcow2 格式走各自后端 */
    if(q && q->fmt == DISK_FMT_RAW) return img_raw_write(q, off, buf, len);
    if(q && (q->fmt == DISK_FMT_VHD || q->fmt == DISK_FMT_VMDK))
        return img_fmt_write(q, off, buf, len);
    if(!q||(!buf&&len)) return IMG_EINVAL;
    if(!q->writable) return IMG_ESTATE;
    /* ★ 宿主空间底线（默认开、不可关 —— 见 img_host.h）：
     *   每写满 128MB 查一次 fstatfs；低于 IMG_HOST_MIN_FREE 就【停止写】，
     *   不让"宿主满"变成"写到一半留下半成品镜像"。开销：fstatfs 每 128MB 一次。*/
    static uint64_t s_host_written;
    (void)s_host_written;
    {
        static uint64_t written_since_check = 0;
        if(written_since_check == 0 && len){
            uint64_t freeb = img_host_free_bytes(img_io_is_fd(q->io)?q->io:q->io);
            if(freeb && freeb < IMG_HOST_MIN_FREE) return IMG_EFULL;
        }
        written_since_check += len;
        if(written_since_check >= (128ull<<20)) written_since_check = 0;
    }
    /* ★ 越界判定必须防加法溢出：原写法 `off + len > size` 在
     *   off=UINT64_MAX-4096/len=4096 时会回绕成 0 → 判定通过 → 真的按巨大偏移写，
     *   把 L2/头部写坏（实测把镜像 size 字段搞成 UINT64_MAX，qemu-img 从此无法打开）。*/
    if(off > q->h.size || (uint64_t)len > q->h.size - off) return IMG_EFULL;   /* 越界 → 需先 expand */

    const uint8_t *src = (const uint8_t*)buf;
    uint64_t cs = q->h.cluster_size;
    uint8_t *scratch = malloc((size_t)cs);
    if(!scratch) return IMG_ENOMEM;

    int rc = IMG_OK;
    size_t done = 0;
    while(done < len){
        uint64_t abs = off + done;
        uint64_t in_cl = abs % cs;
        size_t n = len - done;
        if(n > cs - in_cl) n = (size_t)(cs - in_cl);
        uint64_t cl_base = abs - in_cl;

        /* ---- ① 拿到 L2 表 ---- */
        uint64_t l1i = abs / q->h.l1_span_bytes;
        if(l1i >= q->h.l1_size){ rc = IMG_EFULL; break; }

        uint64_t l2_addr = 0;
        if((rc = l1_get(q, l1i, &l2_addr)) != IMG_OK) break;
        l2_addr &= QCOW2_OFFSET_MASK;

        uint8_t *l2 = NULL;
        if(l2_addr == 0){
            if((rc = alloc_cluster(q, &l2_addr)) != IMG_OK) break;
            l2 = calloc(1,(size_t)cs);
            if(!l2){ rc = IMG_ENOMEM; break; }
            if((rc = l1_set(q, l1i, l2_addr | QCOW2_COPIED_BIT)) != IMG_OK){ free(l2); break; }
            if((rc = write_l2_cluster(q, l2_addr, l2)) != IMG_OK){ free(l2); break; }
        } else {
            if((rc = read_l2_cluster(q, l2_addr, &l2)) != IMG_OK) break;
        }

        /* ---- ② 拿到数据簇 ---- */
        uint64_t l2i = (abs % q->h.l1_span_bytes) / cs;
        uint64_t l2e = rd_be64_i(l2 + l2i*8);
        uint64_t host = l2e & QCOW2_OFFSET_MASK;
        bool compressed = (l2e & QCOW2_COMPRESSED_BIT) != 0;

        bool need_new = (host == 0);
        if(compressed){
            /* 压缩簇 → 解出来，改成未压缩重写（简单、必然有效） */
            if((rc = img_qcow2_read(q, cl_base, scratch, (size_t)cs)) != IMG_OK) break;
            need_new = true;
            host = 0;
        } else if(host == 0){
            memset(scratch, 0, (size_t)cs);
        } else {
            if((rc = img_io_read_full(q->io, host, scratch, (size_t)cs)) != IMG_OK) break;
        }

        if(need_new){
            uint64_t a = 0;
            if((rc = alloc_cluster(q, &a)) != IMG_OK) break;
            host = a;
        }

        /* ---- ③ 覆盖本次要写的字节 ---- */
        memcpy(scratch + in_cl, src + done, n);

        /* ---- ④ 落盘 + 更新 L2 ---- */
        if((rc = img_io_write_full(q->io, host, scratch, (size_t)cs)) != IMG_OK) break;
        if(need_new || compressed){
            /* host 簇 refcount==1 → 必须置 OFLAG_COPIED(bit63)；同时清掉压缩位 */
            wr_be64_i(l2 + l2i*8, host | QCOW2_COPIED_BIT);
            if((rc = write_l2_cluster(q, l2_addr, l2)) != IMG_OK) break;
        }
        free(l2); l2 = NULL;
        done += n;
    }
    free(scratch);
    return rc;
}


/* ---------- T6：压缩簇 ----------
 * 布局（与 QEMU 一致，实测 qemu-img 造的 zstd 簇同样没有长度前缀）：
 *   L2 entry bit62=1（压缩）、bit63=COPIED（refcount==1 时必须置）
 *   低 csize_shift(54，cluster_bits=16) 位 = host 字节偏移（512 对齐）
 *   bit 54 起 (cluster_bits-8)=8 位 = 占用扇区数 - 1
 *   host 处放【裸 deflate 流】（不是 zlib 格式：deflateInit2(-15)）
 *
 * 注意：不走 alloc_cluster_raw —— 它会先写一个完整 0 簇，那等于把压缩省下的
 * 空间又写回去了。这里只 truncate 撑到簇末尾，实际只写压缩数据那几扇区，
 * 其余天然是洞（宿主按 st_blocks 记账，文件看起来还是簇对齐的长度）。 */
static int alloc_cluster_sparse(img_qcow2 *q, uint64_t nbytes, uint64_t *out_off){
    uint64_t cs = q->h.cluster_size;
    uint64_t off = q->alloc_next;
    if(off == 0) off = align_up(img_io_size(q->io), 512);
    /* 压缩数据不能跨簇：放不下就换到下一簇起点（尾部那点浪费无所谓）。
     * 这样 refcount 永远只涉及【一个】簇，语义最简单。*/
    if((off % cs) && (off % cs) + nbytes > cs) off = align_up(off, cs);
    if(img_io_truncate(q->io, off + nbytes) != IMG_OK) return IMG_EIO;
    q->alloc_next = off + nbytes;
    if(q->alloc_next > q->total_clusters * cs)
        q->total_clusters = (q->alloc_next + cs - 1) / cs;
    *out_off = off;
    return IMG_OK;
}

/* 压缩簇共享同一个宿主簇时 refcount 要累加（QEMU 的 check 会核对引用次数）。*/
static int refcount_inc(img_qcow2 *q, uint64_t host_cluster_idx){
    uint32_t v = 0;
    int rc = qc2_refcount_get(q, host_cluster_idx, &v);
    if(rc != IMG_OK) return rc;
    return qc2_refcount_set(q, host_cluster_idx, v + 1);
}

int img_qcow2_write_compressed(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
#if defined(IMG_HAVE_ZLIB) || defined(IMG_HAVE_ZSTD)
    if(!q || !buf) return IMG_EINVAL;
    if(!q->writable) return IMG_ESTATE;
    if(q->fmt != DISK_FMT_QCOW2) return IMG_ENOTSUP;
    uint64_t cs = q->h.cluster_size;
    if(!cs || len != (size_t)cs || (off % cs)) return IMG_ENOTSUP;
    if(off > q->h.size || cs > q->h.size - off) return IMG_EFULL;

    /* ① 压缩：至少省出一个 512 扇区才值得。
     *   算法跟映像头走（compression_type）：0=zlib 裸 deflate、1=zstd。
     *   盘是 zstd 而本构建没编 libzstd（或反之）→ 如实报 ENOTSUP，
     *   让调用方（img_clone）退回普通写 —— 绝不把 deflate 流写进 zstd 盘。 */
    uint8_t *out = (uint8_t*)malloc((size_t)cs);
    if(!out) return IMG_ENOMEM;
    size_t clen = 0;
    if(q->h.compression_type == IMG_QCOW2_COMP_ZSTD){
#ifdef IMG_HAVE_ZSTD
        /* 压不动（放不下 cs 字节的输出缓冲）→ ZSTD_compress 报错 → ENOTSUP 退普通写 */
        size_t zr = ZSTD_compress(out, (size_t)cs, buf, (size_t)cs, 3);
        if(ZSTD_isError(zr)){ free(out); return IMG_ENOTSUP; }
        clen = zr;
#else
        free(out); return IMG_ENOTSUP;      /* zstd 盘 + 无解码器/编码器 */
#endif
    } else {
#ifdef IMG_HAVE_ZLIB
        z_stream zs; memset(&zs, 0, sizeof zs);
        if(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 9,
                        Z_DEFAULT_STRATEGY) != Z_OK){ free(out); return IMG_EIO; }
        zs.next_in  = (Bytef*)buf; zs.avail_in  = (uInt)cs;
        zs.next_out = out;         zs.avail_out = (uInt)cs;
        int zr = deflate(&zs, Z_FINISH);
        clen = (size_t)cs - zs.avail_out;
        deflateEnd(&zs);
        if(zr != Z_STREAM_END){ free(out); return IMG_ENOTSUP; }
#else
        free(out); return IMG_ENOTSUP;      /* zlib 盘 + 本构建没编 zlib */
#endif
    }
    uint64_t nsec = (clen + 511) / 512;
    if(nsec == 0) nsec = 1;
    if(nsec * 512 >= cs){ free(out); return IMG_ENOTSUP; }   /* 压不动：交给普通写 */

    /* ② 拿 L2（没有就建） */
    uint64_t l1i = off / q->h.l1_span_bytes;
    if(l1i >= q->h.l1_size){ free(out); return IMG_EFULL; }
    uint64_t l2_addr = 0;
    int rc = l1_get(q, l1i, &l2_addr);
    if(rc != IMG_OK){ free(out); return rc; }
    l2_addr &= QCOW2_OFFSET_MASK;
    uint8_t *l2 = NULL;
    if(l2_addr == 0){
        if((rc = alloc_cluster(q, &l2_addr)) != IMG_OK){ free(out); return rc; }
        l2 = (uint8_t*)calloc(1, (size_t)cs);
        if(!l2){ free(out); return IMG_ENOMEM; }
        if((rc = l1_set(q, l1i, l2_addr | QCOW2_COPIED_BIT)) != IMG_OK){ free(l2); free(out); return rc; }
        if((rc = write_l2_cluster(q, l2_addr, l2)) != IMG_OK){ free(l2); free(out); return rc; }
    } else {
        if((rc = read_l2_cluster(q, l2_addr, &l2)) != IMG_OK){ free(out); return rc; }
    }

    uint64_t l2i = (off % q->h.l1_span_bytes) / cs;
    uint64_t old = rd_be64_i(l2 + l2i*8);
    /* 目标簇已有普通映射：refcount 1 → 0（追加式分配器不回收，但至少不泄漏 refcount）。
     * 压缩簇（旧）没有独立 refcount 语义之外的占用，这里不动。 */
    if(old && !(old & QCOW2_COMPRESSED_BIT)){
        uint64_t oh = old & QCOW2_OFFSET_MASK;
        uint32_t rn = 0;
        if(oh && qc2_refcount_get(q, oh / cs, &rn) == IMG_OK && rn == 1)
            (void)qc2_refcount_set(q, oh / cs, 0);
    }

    /* ③ 分配 + 只写压缩数据 */
    uint64_t h = 0;
    if((rc = alloc_cluster_sparse(q, nsec*512, &h)) != IMG_OK){ free(l2); free(out); return rc; }
    if((rc = refcount_inc(q, h / cs)) != IMG_OK){ free(l2); free(out); return rc; }
    memset(out + clen, 0, (size_t)(nsec*512 - clen));
    if((rc = img_io_write_full(q->io, h, out, (size_t)nsec*512)) != IMG_OK){ free(l2); free(out); return rc; }

    /* ④ L2 entry */
    int x = 62 - ((int)q->h.cluster_bits - 8);
    uint64_t mask = (x >= 64) ? ~0ULL : ((1ULL << x) - 1);
    uint64_t desc = (h & mask) | ((nsec - 1) << x);
    /* ★ 压缩簇【绝不能】置 COPIED 位：qemu-img check 的原话是
     *   "copied flag must never be set for compressed clusters"。
     *   （普通簇 refcount==1 时必须置，两种簇规则相反，别想当然。）*/
    wr_be64_i(l2 + l2i*8, desc | QCOW2_COMPRESSED_BIT);
    rc = write_l2_cluster(q, l2_addr, l2);
    free(l2); free(out);
    return rc;
#else
    (void)q; (void)off; (void)buf; (void)len;
    return IMG_ENOTSUP;      /* 未编入 zlib/zstd */
#endif
}

/* ------------------------------------------------------------------ */
/* 扩大虚拟磁盘                                                        */
/* ------------------------------------------------------------------ */
int img_qcow2_expand(img_qcow2 *q, uint64_t new_size, img_prog *prog){
    if(q && q->fmt == DISK_FMT_RAW){   /* raw: 扩容 = truncate 宿主文件 */
        if(new_size < q->raw_size) return IMG_EINVAL;
        int rc = img_io_truncate(q->io, new_size);
        if(rc != IMG_OK) return rc;
        q->raw_size = new_size;
        q->h.size = new_size;
        return IMG_OK;
    }
    if(!q) return IMG_EINVAL;
    if(!q->writable) return IMG_ESTATE;
    if(new_size == q->h.size) return IMG_OK;           /* 同尺寸：幂等成功 */
    if(new_size <  q->h.size) return IMG_EINVAL;       /* 阶段1 不支持缩容 */
    /* ★ 硬上限 + 防乘法溢出。不设限的后果实测：expand(UINT64_MAX) 会被照做，
     *   头部 size 变成 UINT64_MAX → 镜像彻底损坏（qemu-img 都无法打开）——
     *   一次坏参数就毁掉用户数据。上限依据我们的布局：L1 表放在一个簇里，
     *   每项 8 字节 → 最多 cluster_size/8 项；每项覆盖 l1_span_bytes。*/
    {
        uint64_t max_l1 = q->h.cluster_size / 8;
        if(max_l1 && q->h.l1_span_bytes){
            uint64_t need_l1 = new_size / q->h.l1_span_bytes +
                               (new_size % q->h.l1_span_bytes ? 1 : 0);   /* 不写加法，避免回绕 */
            if(need_l1 > max_l1) return IMG_ERANGE;
        }
    }

    uint64_t new_l1 = new_size / q->h.l1_span_bytes +
                      (new_size % q->h.l1_span_bytes ? 1 : 0);
    if(new_l1 == 0) new_l1 = 1;

    if(new_l1 > q->h.l1_size){
        uint64_t need = new_l1 * 8;
        /* L1 表在簇内还有位置吗？（我们的布局：cluster 对齐 → 一簇可放 8192 条 = 4TB）*/
        uint64_t in_cluster = q->h.l1_table_offset % q->h.cluster_size;
        uint64_t room = q->h.cluster_size - in_cluster;
        uint8_t *z = calloc(1, (size_t)(need > room ? room : need));
        if(!z) return IMG_ENOMEM;
        int rc = IMG_OK;
        if(need > room){
            /* 需要搬迁 L1 表到文件尾（>4TB 才会发生） */
            uint64_t a = 0;
            if((rc = alloc_cluster(q, &a)) == IMG_OK){
                rc = img_io_write_full(q->io, a, q->l1, (size_t)q->l1_bytes);
                if(rc == IMG_OK){
                    memset((uint8_t*)z,0,(size_t)need);
                    memcpy(z, q->l1, (size_t)q->l1_bytes);
                    free(q->l1);
                    q->l1 = malloc((size_t)need);
                    if(!q->l1){ free(z); return IMG_ENOMEM; }
                    memcpy(q->l1, z, (size_t)need);
                    q->l1_bytes = (size_t)need;
                    wr_be64_i(((uint8_t[]){0}), 0); /* no-op */
                    {
                        uint8_t b[8]; wr_be64_i(b, a);
                        rc = img_io_write_full(q->io, 40, b, 8);
                    }
                    q->h.l1_table_offset = a;
                    if(rc == IMG_OK) rc = img_io_write_full(q->io, a, q->l1, (size_t)need);
                }
            }
        } else {
            /* 原地扩展（清零新增部分） */
            rc = img_io_write_full(q->io, q->h.l1_table_offset + q->h.l1_size*8,
                                   z, (size_t)(need - q->h.l1_size*8));
            if(rc == IMG_OK){
                uint8_t *nl = realloc(q->l1, (size_t)need);
                if(!nl){ free(z); return IMG_ENOMEM; }
                memset(nl + q->l1_bytes, 0, (size_t)(need - q->l1_bytes));
                q->l1 = nl; q->l1_bytes = (size_t)need;
            }
        }
        free(z);
        if(rc) return rc;
        q->h.l1_size = (uint32_t)new_l1;
        int rc2 = wr32(q->io, 36, q->h.l1_size);     /* 头里 l1_size */
        if(rc2) return rc2;
    }

    if(prog) img_prog_count(prog, 1, 2);
    /* 改虚拟大小（头偏移 24） */
    {
        uint8_t b[8]; wr_be64_i(b, new_size);
        int rc = img_io_write_full(q->io, 24, b, 8);
        if(rc) return rc;
    }
    q->h.size = new_size;
    if(prog){ img_prog_count(prog,2,2); img_prog_done(prog, "扩盘完成"); }
    return img_io_sync(q->io);
}
