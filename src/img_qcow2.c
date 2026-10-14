/* img_qcow2.c — qcow2 读取（+ 结构/open/close/info）
 * 翻译自已用 sha256 交叉验证过的 reader.mjs
 */
#include "img_qcow2_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>

/* ---------- 大端读取（qcow2 全镜像大端）---------- */
static inline uint16_t be16(const void *p){
    const uint8_t *b=(const uint8_t*)p; return (uint16_t)((b[0]<<8)|b[1]);
}
static inline uint32_t be32(const void *p){
    const uint8_t *b=(const uint8_t*)p;
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}
static inline uint64_t be64(const void *p){
    const uint8_t *b=(const uint8_t*)p;
    return ((uint64_t)be32(b)<<32)|be32(b+4);
}
static inline void wr_be16(void *p,uint16_t v){ uint8_t*b=p;b[0]=v>>8;b[1]=v; }
static inline void wr_be32(void *p,uint32_t v){ uint8_t*b=p;b[0]=v>>24;b[1]=v>>16;b[2]=v>>8;b[3]=v; }
static inline void wr_be64(void *p,uint64_t v){ wr_be32(p,(uint32_t)(v>>32)); wr_be32((uint8_t*)p+4,(uint32_t)v); }

/* 导出字节序工具（供写入模块使用） */
void    wr_be16_i(void *p, uint16_t v){ wr_be16(p,v); }
void    wr_be32_i(void *p, uint32_t v){ wr_be32(p,v); }
void    wr_be64_i(void *p, uint64_t v){ wr_be64(p,v); }
uint16_t rd_be16_i(const void *p){ return be16(p); }
uint32_t rd_be32_i(const void *p){ return be32(p); }
uint64_t rd_be64_i(const void *p){ return be64(p); }

/* ---------- 打开 ---------- */
img_qcow2 *img_qcow2_open(img_io *io, const img_qcow2_opts_t *o){
    if(!io) return NULL;
    img_qcow2 *q = calloc(1, sizeof *q);
    if(!q) return NULL;
    q->io = io; q->own_io = false;
    q->read_only = o ? o->read_only : false;
    q->allow_zstd = o ? o->allow_zstd : false;
    q->cb = o ? o->cb : NULL; q->cb_ud = o ? o->cb_ud : NULL;

    uint8_t hdr[112];
    memset(hdr,0,sizeof hdr);
    int rc = img_io_read_full(io, 0, hdr, 104);
    if(rc != IMG_OK){ free(q); return NULL; }

    q->h.magic = be32(hdr);
    if(q->h.magic != IMG_QCOW2_MAGIC){ free(q); return NULL; }   /* 不是 qcow2 */

    q->h.version                = be32(hdr+4);
    q->h.backing_file_offset    = be32(hdr+8);
    q->h.backing_file_size      = be32(hdr+12);
    q->h.cluster_bits           = be32(hdr+20);
    q->h.size                   = be64(hdr+24);
    q->h.crypt_method           = be64(hdr+32);
    q->h.l1_size                = be32(hdr+36);
    q->h.l1_table_offset        = be64(hdr+40);
    q->h.refcount_table_offset  = be64(hdr+48);
    q->h.refcount_table_clusters= be32(hdr+56);
    q->h.nb_snapshots           = be32(hdr+60);
    q->h.snapshots_offset       = be64(hdr+64);

    if(q->h.version < 2 || q->h.version > 3){ free(q); return NULL; }
    if(q->h.cluster_bits < 9 || q->h.cluster_bits > 21){ free(q); return NULL; }

    q->h.cluster_size  = 1ULL << q->h.cluster_bits;
    q->h.l2_entries    = q->h.cluster_size / 8;
    q->h.l1_span_bytes = q->h.cluster_size * q->h.l2_entries;   /* 单张 L2 覆盖量 */

    if(q->h.version >= 3){
        /* 72..103 = 32 字节（规范原文）：
         *   72-79 incompatible | 80-87 compatible | 88-95 ro
         *   96-99 refcount_order | 100-103 header_length          */
        uint8_t ext[32]; memset(ext,0,sizeof ext);
        if(img_io_read_full(io, 72, ext, 32) == IMG_OK){
            q->h.incompatible   = be64(ext);
            q->h.compatible     = be64(ext+8);
            q->h.read_only      = be64(ext+16);
            q->h.refcount_order = be32(ext+24);   /* 96-99  */
            q->h.header_length  = be32(ext+28);   /* 100-103 */
        } else {
            q->h.refcount_order = 4;              /* 读不到就按默认 16 位 */
            q->h.header_length  = 104;
        }
        if(q->h.header_length > 104){             /* 104: compression_type */
            uint8_t ct[2]; if(img_io_read_full(io,104,ct,2)==IMG_OK)
                q->h.compression_type = be16(ct);
        }
    } else {
        q->h.refcount_order = 4;                  /* v2 恒为 4 (16 位) */
        q->h.header_length  = 72;
    }
    /* refcount_bits = 1 << refcount_order，上限 6 (64 位) */
    if(q->h.refcount_order > 6){ free(q); return NULL; }

    /* ★ 特性位校验：超出能力范围就明确报错，绝不静默出错 */
    if(q->h.incompatible & IMG_QCOW2_INCOMPAT_ENCRYPTED){ free(q); return NULL; }
    if(q->h.incompatible & IMG_QCOW2_INCOMPAT_DATA_FILE){ free(q); return NULL; }
    if(q->h.incompatible & IMG_QCOW2_INCOMPAT_BITMASK)   { free(q); return NULL; }
    if(q->h.compression_type == IMG_QCOW2_COMP_ZSTD && !q->allow_zstd){ free(q); return NULL; }
    if((q->h.incompatible & IMG_QCOW2_INCOMPAT_RAW_CLUSTERS) && q->h.compression_type != 0
       && q->h.compression_type != 1){ free(q); return NULL; }

    /* 载入 L1 */
    q->l1_bytes = (size_t)q->h.l1_size * 8;
    q->l1 = calloc(1, q->l1_bytes ? q->l1_bytes : 1);
    if(!q->l1){ free(q); return NULL; }
    if(q->l1_bytes && img_io_read_full(io, q->h.l1_table_offset, q->l1, q->l1_bytes) != IMG_OK){
        free(q->l1); free(q); return NULL;
    }

    q->l2_cache = malloc((size_t)q->h.cluster_size);
    if(!q->l2_cache){ free(q->l1); free(q); return NULL; }
    return q;
}

void img_qcow2_close(img_qcow2 *q){
    if(!q) return;
    free(q->l1); free(q->l2_cache); free(q);
}
const img_qcow2_header_t *img_qcow2_header(const img_qcow2 *q){ return q?&q->h:NULL; }
uint64_t img_qcow2_size(const img_qcow2 *q){ return q?q->h.size:0; }

/* ---------- 读取 ---------- */

/* 取 offset 所在 guest 块对应的 L2 表；成功则 *l2 表就位 */
static int load_l2(img_qcow2 *q, uint64_t abs, uint64_t *l2_out){
    uint64_t l1i = abs / q->h.l1_span_bytes;
    if(l1i >= q->h.l1_size) return IMG_ENOENT;              /* 超出 L1 覆盖 */
    uint64_t l1e = be64(q->l1 + l1i*8);
    uint64_t l2_addr = l1e & QCOW2_OFFSET_MASK;
    if(l2_addr == 0) return IMG_ENOENT;                     /* 稀疏 */
    *l2_out = l2_addr;
    return IMG_OK;
}

static int read_l2_entry(img_qcow2 *q, uint64_t l2_addr, uint64_t abs, uint64_t *entry_out){
    /* L2 缓存 */
    if(!q->l2_cache_valid || q->l2_cache_off != l2_addr){
        int rc = img_io_read_full(q->io, l2_addr, q->l2_cache, (size_t)q->h.cluster_size);
        if(rc != IMG_OK) return rc;
        q->l2_cache_off = l2_addr; q->l2_cache_valid = true;
    }
    uint64_t l2i = (abs % q->h.l1_span_bytes) / q->h.cluster_size;
    *entry_out = be64(q->l2_cache + l2i*8);
    return IMG_OK;
}

/* 解压压缩簇：描述符 x = 62 - (cluster_bits - 8) */
static int decompress_cluster(img_qcow2 *q, uint64_t l2e, uint8_t *out){
#ifdef IMG_HAVE_ZLIB
    int x = 62 - ((int)q->h.cluster_bits - 8);
    uint64_t desc = l2e & ((1ULL<<62) - 1);
    uint64_t host_off = desc & (( (x>=64) ? ~0ULL : ((1ULL<<x)-1) ));
    uint64_t nb_sectors = (desc >> x) + 1;

    size_t nraw = (size_t)nb_sectors * 512;
    uint8_t *raw = malloc(nraw);
    if(!raw) return IMG_ENOMEM;
    int rc = img_io_read_full(q->io, host_off, raw, nraw);
    if(rc){ free(raw); return rc; }

    /* qcow2 压缩簇用【raw deflate】（无 zlib 头） */
    z_stream zs; memset(&zs,0,sizeof zs);
    if(inflateInit2(&zs, -15) != Z_OK){ free(raw); return IMG_EIO; }
    zs.next_in = raw;  zs.avail_in  = (uInt)nraw;
    zs.next_out = out; zs.avail_out = (uInt)q->h.cluster_size;
    int zr = inflate(&zs, Z_FINISH);
    size_t produced = q->h.cluster_size - zs.avail_out;
    inflateEnd(&zs);
    free(raw);

    if(zr != Z_STREAM_END && zr != Z_OK){ return IMG_ECORRUPT; }
    if(produced < q->h.cluster_size)
        memset(out + produced, 0, (size_t)q->h.cluster_size - produced);
    return IMG_OK;
#else
    (void)q; (void)l2e; (void)out;
    return IMG_ENOTSUP;      /* 未编入 zlib */
#endif
}

/* 读 offset 所在的整簇到 out（长度须 >= cluster_size）；全稀疏返回 IMG_ENOENT */
static int read_cluster(img_qcow2 *q, uint64_t abs, uint8_t *out){
    uint64_t l2_addr = 0;
    int rc = load_l2(q, abs, &l2_addr);
    if(rc != IMG_OK) return rc;

    uint64_t l2e = 0;
    rc = read_l2_entry(q, l2_addr, abs, &l2e);
    if(rc != IMG_OK) return rc;

    if(l2e & QCOW2_COMPRESSED_BIT)
        return decompress_cluster(q, l2e, out);

    uint64_t host = l2e & QCOW2_OFFSET_MASK;
    if(host == 0) return IMG_ENOENT;                        /* 稀疏 */

    return img_io_read_full(q->io, host, out, (size_t)q->h.cluster_size);
}

int img_qcow2_read(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    if(!q||!buf) return IMG_EINVAL;
    uint8_t *out = (uint8_t*)buf;
    memset(out, 0, len);                                    /* 稀疏/越界 = 0 */
    uint8_t *scratch = NULL;

    size_t done = 0;
    while(done < len){
        uint64_t abs = off + done;
        if(abs >= q->h.size) break;                         /* 虚拟盘尾外 → 保持 0 */
        uint64_t in_cluster = abs % q->h.cluster_size;
        size_t n = (size_t)(len - done);
        if(n > q->h.cluster_size - in_cluster) n = (size_t)(q->h.cluster_size - in_cluster);

        uint64_t cluster_base = abs - in_cluster;
        if(!scratch) scratch = malloc((size_t)q->h.cluster_size);
        if(!scratch) return IMG_ENOMEM;

        int rc = read_cluster(q, cluster_base, scratch);
        if(rc == IMG_OK){
            memcpy(out + done, scratch + in_cluster, n);
        } else if(rc == IMG_ENOENT){
            /* 稀疏 → 保持 0 */
        } else {
            free(scratch);
            return rc;
        }
        done += n;
    }
    free(scratch);
    return IMG_OK;
}

int img_qcow2_read_full(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    if(!q||!buf) return IMG_EINVAL;
    if(off >= q->h.size) return IMG_ETRUNC;
    uint64_t avail = q->h.size - off;
    return img_qcow2_read(q, off, buf, (len <= avail) ? len : (size_t)avail);
}

/* ---------- 信息 JSON ---------- */
int img_qcow2_info_json(const img_qcow2 *q, char *buf, int cap){
    if(!q||!buf||cap<16) return 0;
    const img_qcow2_header_t *h = &q->h;
    char feat[256]={0};
    int n=0;
    #define ADD(f) do{ if(n<(int)sizeof(feat)-1) n+=snprintf(feat+n,sizeof(feat)-n,"%s%s", n?",":"", f); }while(0)
    if(h->incompatible & IMG_QCOW2_INCOMPAT_CORRUPT) ADD("corrupt");
    if(h->incompatible & IMG_QCOW2_INCOMPAT_DIRTY)   ADD("dirty");
    if(h->incompatible & IMG_QCOW2_INCOMPAT_ACTIVE_L2) ADD("active-l2");
    if(h->incompatible & IMG_QCOW2_INCOMPAT_EXT_L2)  ADD("extended-l2");
    if(h->compatible   & IMG_QCOW2_COMPAT_DIRTY_BITMAPS) ADD("dirty-bitmaps");
    if(h->read_only    & IMG_QCOW2_RO_INCONSIST_SNAP)    ADD("inconsistent-snapshots");
    #undef ADD
    return snprintf(buf, cap,
        "{\"magic\":\"QFI\\xfb\",\"version\":%u,"
        "\"virtualSize\":%llu,\"clusterBits\":%u,\"clusterSize\":%llu,"
        "\"l1Size\":%u,\"l1Offset\":%llu,"
        "\"refcountClusters\":%u,\"snapshots\":%u,"
        "\"compression\":\"%s\","
        "\"incompatible\":%llu,\"compatible\":%llu,\"readOnly\":%llu,"
        "\"features\":[%s%s%s]}",
        h->version,
        (unsigned long long)h->size,
        h->cluster_bits, (unsigned long long)h->cluster_size,
        h->l1_size, (unsigned long long)h->l1_table_offset,
        h->refcount_table_clusters, h->nb_snapshots,
        h->compression_type==IMG_QCOW2_COMP_ZSTD ? "zstd":"zlib",
        (unsigned long long)h->incompatible,
        (unsigned long long)h->compatible,
        (unsigned long long)h->read_only,
        feat[0] ? "\"" : "",
        feat,
        feat[0] ? "\"" : "");
}
