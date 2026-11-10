/* img_qcow2.c — qcow2 读取（+ 结构/open/close/info）
 * 翻译自已用 sha256 交叉验证过的 reader.mjs
 */
#include "img_qcow2_int.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>
#ifdef IMG_HAVE_ZSTD
#include <zstd.h>
#endif

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
    /* ★ 2026-11-10：装了 libzstd 就默认允许 zstd 压缩簇 —— 以前默认拒绝是因为
     *   没有解码器，硬读会写出坏数据；现在有解码器，拒绝反而让用户打不开正常映像。
     *   显式传 opts->allow_zstd=false 仍然可以强制拒绝（测试用）。*/
#ifdef IMG_HAVE_ZSTD
    q->allow_zstd = o ? (o->allow_zstd || true) : true;
#else
    q->allow_zstd = false;
#endif
    q->cb = o ? o->cb : NULL; q->cb_ud = o ? o->cb_ud : NULL;

    /* ★ P3（2026-11-09）：先探测格式。qcow2 / raw / vhd / vmdk 共用本类型。
     *   raw 只需把 guest 偏移直通给 img_io —— 零额外结构。
     *   VHD/VMDK 各自有自己的解析器（img_vhd.c / img_vmdk.c）。 */
    q->fmt = img_diskfmt_probe(io);
    if(q->fmt == DISK_FMT__COUNT){ free(q); return NULL; }

    if(q->fmt == DISK_FMT_RAW){
        q->raw_size = img_io_size(io);
        q->h.magic  = 0;                  /* raw 没有 magic */
        q->h.size   = q->raw_size;        /* guest 大小 == 文件大小 */
        q->h.cluster_bits = 0;
        q->h.cluster_size = 0;
        q->writable = !q->read_only;
        return q;                          /* raw 不需要任何解析 */
    }
    if(q->fmt == DISK_FMT_VHD || q->fmt == DISK_FMT_VMDK){
        int vrc = img_fmt_open(q);         /* 见 img_vhd.c / img_vmdk.c */
        if(vrc != IMG_OK){ free(q); return NULL; }
        q->writable = !q->read_only;
        return q;
    }

    /* ---- 以下是原 qcow2 路径 ---- */
    uint8_t hdr[112];
    memset(hdr,0,sizeof hdr);
    int rc = img_io_read_full(io, 0, hdr, 104);
    if(rc != IMG_OK){ free(q); return NULL; }

    q->h.magic = be32(hdr);
    if(q->h.magic != IMG_QCOW2_MAGIC){ free(q); return NULL; }   /* 不是 qcow2 */

    q->h.version                = be32(hdr+4);
    /* ★ 2026-11-10（REQ-66）：规范里 backing_file_offset 是 **u64**（8..15），
     *   backing_file_size 是 u32（16..19）。以前按 be32(hdr+8) 读，读到的是
     *   这个 u64 的【高 32 位】—— 4 GiB 以内的镜像恒为 0，于是所有 backing
     *   镜像都被当成"没有 backing"。在我们开始按它做门禁（img.trim）之前必须修。*/
    q->h.backing_file_offset    = be64(hdr+8);
    q->h.backing_file_size      = be32(hdr+16);
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
            q->h.refcount_order = 4;              /* 无法读取就按默认 16 位 */
            q->h.header_length  = 104;
        }
        /* ★ compression_type 是【1 字节】，在 header_length 之后（offset 104）：
         *     0 = deflate(zlib)，1 = zstd
         *   以前按 2 字节大端读 → 拿到的是高半部分（zstd 镜像读到 0x0100），
         *   于是恒被当成 zlib，解压出来是错的（一律 ECORRUPT）。
         *   出处：QEMU qcow2 规范「104: compression_type」。*/
        if(q->h.header_length > 104){
            uint8_t ct=0; if(img_io_read_full(io,104,&ct,1)==IMG_OK)
                q->h.compression_type = ct;
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
    /* ★ 关盘前落盘：UI 说"完成"的时候，数据必须真的在介质上。
     *   手机上进程被杀是常态，不做这步，"done" 就只是"写进了页缓存"。*/
    if(q->writable && q->io) img_io_sync(q->io);
    free(q->l1); free(q->l2_cache); free(q);
}
const img_qcow2_header_t *img_qcow2_header(const img_qcow2 *q){ return q?&q->h:NULL; }
uint64_t img_qcow2_size(const img_qcow2 *q){
    if(!q) return 0;
    /* ★ P3：raw = 宿主文件大小 */
    if(q->fmt == DISK_FMT_RAW) return q->raw_size;
    return q->h.size;
}

/* ---------- 一致性扫描（qc.verify / 写前自检） ---------- */
int img_qcow2_scan(img_qcow2 *q, img_qcow2_scan_t *out){
    if(!q || !out || !q->io) return IMG_EINVAL;
    if(q->fmt == DISK_FMT_RAW){   /* raw: 无 L1/L2 可扫，直接成功 */
        memset(out,0,sizeof *out);
        return IMG_OK;
    }
    memset(out, 0, sizeof *out);
    uint64_t cs = q->h.cluster_size;
    if(!cs) return IMG_EFORMAT;
    uint64_t fsz = img_io_size(q->io);
    out->guest_bytes = q->h.size;
    out->guest_clusters = (q->h.size + cs - 1) / cs;

    uint64_t ncl = fsz / cs + 2;
    uint8_t *seen = calloc(1, (size_t)((ncl + 7) / 8 ? (ncl + 7) / 8 : 1));
    uint8_t *tbl  = malloc((size_t)cs);
    if(!seen || !tbl){ free(seen); free(tbl); return IMG_ENOMEM; }

    uint64_t l2n = cs / 8;
    for(uint64_t i = 0; i < q->h.l1_size; i++){
        uint64_t e = be64(q->l1 + i * 8);
        uint64_t l2 = e & QCOW2_OFFSET_MASK;
        if(!l2) continue;
        out->l2_tables++;
        /* 记一个 host 簇（L2 表本身也算），顺带查别名/越界 */
        #define MARK_CLUSTER(off) do{                                       \
            uint64_t _o = (off);                                            \
            if((_o % cs) != 0 || _o + cs > fsz){ out->out_of_range++; }     \
            else {                                                          \
                uint64_t _i = _o / cs;                                      \
                if(_i < ncl){                                               \
                    if(seen[_i >> 3] & (uint8_t)(1u << (_i & 7))) out->duplicate_refs++; \
                    else seen[_i >> 3] |= (uint8_t)(1u << (_i & 7));         \
                    out->referenced++;                                      \
                    if(_o + cs > out->max_ref_end) out->max_ref_end = _o + cs; \
                } else out->out_of_range++;                                 \
            }                                                               \
        }while(0)
        MARK_CLUSTER(l2);
        if((l2 % cs) || l2 + cs > fsz){ out->bad_l2++; continue; }
        if(img_io_read_full(q->io, l2, tbl, (size_t)cs) != IMG_OK){ out->bad_l2++; continue; }
        for(uint64_t k = 0; k < l2n; k++){
            uint64_t le = be64(tbl + k * 8);
            if(!le) continue;
            if(le & QCOW2_COMPRESSED_BIT) continue;   /* 压缩簇：偏移字段另有含义 */
            uint64_t d = le & QCOW2_OFFSET_MASK;
            if(!d) continue;
            MARK_CLUSTER(d);
        }
        #undef MARK_CLUSTER
    }
    free(seen); free(tbl);
    return IMG_OK;
}

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
#if defined(IMG_HAVE_ZLIB) || defined(IMG_HAVE_ZSTD)
    /* 压缩簇的地址描述（两种算法共用）：
     *   低 62 位 = 主机偏移 + 压缩字节数以 512 字节扇区计。 */
    int x = 62 - ((int)q->h.cluster_bits - 8);
    uint64_t desc = l2e & ((1ULL<<62) - 1);
    uint64_t host_off = desc & (( (x>=64) ? ~0ULL : ((1ULL<<x)-1) ));
    uint64_t nb_sectors = (desc >> x) + 1;
    size_t nraw = (size_t)nb_sectors * 512;
    if(!nraw) return IMG_ECORRUPT;

    uint8_t *raw = malloc(nraw);
    if(!raw) return IMG_ENOMEM;
    int rc = img_io_read_full(q->io, host_off, raw, nraw);
    if(rc){ free(raw); return rc; }

    if(q->h.compression_type == IMG_QCOW2_COMP_ZSTD){
#if defined(IMG_HAVE_ZSTD)
        /* ★ 压缩簇按扇区对齐存放，帧后面可能跟着填充字节 —— ZSTD_decompress
         *   要求 srcSize 恰好是【帧】的长度，多给会报 "Src size is incorrect"。
         *   先用 findFrameCompressedSize 量出真正的帧长，再按帧解。*/
        size_t flen = ZSTD_findFrameCompressedSize(raw, nraw);
        if(ZSTD_isError(flen)){ free(raw); return IMG_ECORRUPT; }
        size_t got = ZSTD_decompress(out, (size_t)q->h.cluster_size, raw, flen);
        free(raw);
        if(ZSTD_isError(got)) return IMG_ECORRUPT;
        if(got < (size_t)q->h.cluster_size)
            memset(out + got, 0, (size_t)q->h.cluster_size - got);
        return IMG_OK;
#else
        free(raw);
        return IMG_ENOTSUP;   /* 本构建没有 libzstd（open 时已按 allow_zstd 拦过） */
#endif
    }

    /* 默认按 zlib（raw deflate，无 zlib 头）解 */
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
    return IMG_ENOTSUP;      /* 未编入 zlib/zstd */
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
    /* ★ P3：非 qcow2 格式走各自后端 */
    if(q->fmt == DISK_FMT_RAW)  return img_raw_read(q, off, buf, len);
    if(q->fmt == DISK_FMT_VHD || q->fmt == DISK_FMT_VMDK)
        return img_fmt_read(q, off, buf, len);
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
    if(!q||(!buf&&len)) return IMG_EINVAL;
    /* ★ 越界必须【先判后读】且【一个字节都不写】。
     *   原实现是"把 len 裁到剩余长度"再读 —— 调用方把 len 传错时（实测 len=SIZE_MAX）
     *   会往调用方的小缓冲区里写满整文件 → 踩坏内存/崩溃，而且调用方还看不出错。
     *   下面的减法写法不会溢出（不写 off+len）。*/
    uint64_t sz = q->h.size;
    if(off > sz || (uint64_t)len > sz - off) return IMG_ERANGE;
    return img_qcow2_read(q, off, buf, len);
}

/* ---------- 紧凑另存用：这段 guest 区间有没有已分配的簇 ---------- */
int img_qcow2_any_allocated(img_qcow2 *q, uint64_t off, uint64_t len, bool *any){
    if(q->fmt == DISK_FMT_RAW){   /* raw: 区间在文件内就当已分配（没有稀疏位图） */
        if(any) *any = (off < q->raw_size);
        return IMG_OK;
    }
    if(!q||!any) return IMG_EINVAL;
    *any=false;
    if(!len || off>=q->h.size) return IMG_OK;
    if(len > q->h.size - off) len = q->h.size - off;
    uint64_t c=q->h.cluster_size;
    uint64_t end=off+len;
    uint64_t a=off-(off%c);
    for(; a<end; a+=c){
        uint64_t l2_addr=0, e=0;
        int rc=load_l2(q,a,&l2_addr);
        if(rc==IMG_ENOENT) continue;              /* 整张 L2 都不存在 → 纯稀疏 */
        if(rc!=IMG_OK) return rc;
        rc=read_l2_entry(q,l2_addr,a,&e);
        if(rc!=IMG_OK) return rc;
        if(e!=0){ *any=true; return IMG_OK; }     /* 非零 = 有映射（含压缩簇） */
    }
    return IMG_OK;
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
