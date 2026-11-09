/* img_vhd.c — VHD（Virtual PC / Hyper-V 旧格式）后端
 *
 * 支持动态 VHD（disk_type=3）；固定 VHD（disk_type=2）按 raw 数据读取。
 *
 * 文件布局（动态 VHD）：
 *   [0]                    副本 footer（512B，可选但 qemu 会写）
 *   [512]                  动态盘头（1024B，cookie="cxsparse"）
 *   [1536]                 BAT（块分配表，每项 4 字节大端；0xFFFFFFFF = 未分配）
 *   [BAT 后]               数据块（每块 block_size 字节 + 512B 位图）
 *   [文件尾-512]           footer（512B，cookie="conectix"）
 *
 * 关键常量（全部大端）：
 *   footer.data_offset  = 512（指向动态盘头；0xFFFFFFFFFFFFFFFF = 固定盘）
 *   footer.current_size = 虚拟磁盘大小
 *   footer.disk_type    = 3（动态）
 *   header.table_offset = BAT 的绝对偏移
 *   header.block_size   = 2 MiB（qemu/Hyper-V 默认）
 *   BAT[i]              = 第 i 块位图的扇区号（0xFFFFFFFF = 未分配）
 *
 * 校验和：footer/header 的 checksum 字段 = 整个 512/1024 字节求和后取反（不含 checksum 自身）。
 */
#include "img_qcow2_int.h"
#include "img_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#define VHD_BLOCK_SIZE   (2u*1024*1024)     /* 2 MiB */
#define VHD_SECTOR       512u
#define VHD_FOOTER_SIZE  512
#define VHD_HEADER_SIZE  1024
#define VHD_UNALLOC      0xFFFFFFFFu

typedef struct {
    uint32_t max_entries;      /* BAT 项数 */
    uint32_t block_size;       /* 固定 2MiB */
    uint64_t table_offset;     /* BAT 绝对偏移 */
    uint64_t data_start;       /* 数据区起点（BAT 末尾，512 对齐）*/
    uint32_t *bat;             /* 内存中的 BAT（大端已转本机）*/
    uint64_t next_block;       /* 尾 footer 起点，也是下一块分配位置 */
    uint8_t  footer[VHD_FOOTER_SIZE]; /* 新块追加时重写尾 footer */
} vhd_priv;

static uint32_t rd_be32u(const uint8_t *p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static uint64_t rd_be64u(const uint8_t *p){
    uint64_t v=0;
    for(int i=0;i<8;i++) v=(v<<8)|p[i];
    return v;
}
static void wr_be32u(uint8_t *p, uint32_t v){
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
static void wr_be64u(uint8_t *p, uint64_t v){
    for(int i=7;i>=0;i--){ p[i]=v&0xff; v>>=8; }
}

/* VHD 校验和：把结构各字节求和（checksum 字段为零），再按位取反。 */
static uint32_t vhd_checksum32(const uint8_t *buf, size_t len){
    uint32_t sum = 0;
    for(size_t i = 0; i < len; i++) sum += buf[i];
    return (~sum) & 0xFFFFFFFF;
}

/* 打开：读 footer + 动态盘头 + BAT */
int img_vhd_open(img_qcow2 *q){
    if(!q || !q->io) return IMG_EINVAL;
    uint64_t fsz = img_io_size(q->io);
    if(fsz < VHD_FOOTER_SIZE) return IMG_EFORMAT;

    uint8_t foot[VHD_FOOTER_SIZE];
    if(img_io_read_full(q->io, fsz - VHD_FOOTER_SIZE, foot, sizeof foot) != IMG_OK)
        return IMG_EIO;
    if(memcmp(foot, "conectix", 8)) return IMG_EFORMAT;

    uint64_t doff = rd_be64u(foot + 16);
    if(doff == 0xFFFFFFFFFFFFFFFFULL){
        /* 固定盘：按 raw 处理（数据区就是文件本体，末尾 512 是 footer）。 */
        q->fmt = DISK_FMT_RAW;
        q->raw_size = fsz - VHD_FOOTER_SIZE;
        q->h.size = q->raw_size;
        return IMG_OK;
    }
    if(fsz < VHD_HEADER_SIZE || doff > fsz - VHD_HEADER_SIZE) return IMG_EFORMAT;

    uint8_t hdr[VHD_HEADER_SIZE];
    if(img_io_read_full(q->io, doff, hdr, sizeof hdr) != IMG_OK) return IMG_EIO;
    if(memcmp(hdr, "cxsparse", 8)) return IMG_EFORMAT;

    vhd_priv *p = (vhd_priv*)calloc(1, sizeof *p);
    if(!p) return IMG_ENOMEM;
    p->table_offset = rd_be64u(hdr + 16);
    p->max_entries  = rd_be32u(hdr + 28);
    p->block_size   = rd_be32u(hdr + 32);
    if(!p->max_entries || !p->block_size || p->block_size % VHD_SECTOR ||
       p->block_size > (uint64_t)VHD_SECTOR * 8 * VHD_SECTOR ||
       p->table_offset > fsz - VHD_FOOTER_SIZE ||
       (uint64_t)p->max_entries * 4 > fsz - VHD_FOOTER_SIZE - p->table_offset){
        free(p); return IMG_EFORMAT;
    }
    /* 数据区起点 = BAT 末尾按 512 对齐 */
    p->data_start = (p->table_offset + (uint64_t)p->max_entries * 4 + VHD_SECTOR - 1)
                    / VHD_SECTOR * VHD_SECTOR;

    p->bat = (uint32_t*)calloc(p->max_entries ? p->max_entries : 1, sizeof(uint32_t));
    if(!p->bat){ free(p); return IMG_ENOMEM; }

    uint8_t *raw = (uint8_t*)malloc((size_t)p->max_entries * 4);
    if(!raw){ free(p->bat); free(p); return IMG_ENOMEM; }
    if(img_io_read_full(q->io, p->table_offset, raw, (size_t)p->max_entries * 4) != IMG_OK){
        free(raw); free(p->bat); free(p); return IMG_EIO;
    }
    for(uint32_t i=0;i<p->max_entries;i++) p->bat[i] = rd_be32u(raw + (size_t)i*4);
    free(raw);

    memcpy(p->footer, foot, VHD_FOOTER_SIZE);

    /* 仅接受数据块与尾 footer 不重叠的 BAT；新块从现有 footer 起追加。 */
    uint64_t footer_off = fsz - VHD_FOOTER_SIZE;
    if(p->data_start > footer_off || footer_off % VHD_SECTOR){
        free(p->bat); free(p); return IMG_EFORMAT;
    }
    for(uint32_t i = 0; i < p->max_entries; i++){
        if(p->bat[i] == VHD_UNALLOC) continue;
        uint64_t start = (uint64_t)p->bat[i] * VHD_SECTOR;
        if(start < p->data_start || start > footer_off ||
           (uint64_t)p->block_size + VHD_SECTOR > footer_off - start){
            free(p->bat); free(p); return IMG_EFORMAT;
        }
    }
    p->next_block = footer_off;

    q->fmt_priv = p;
    q->h.size   = rd_be64u(foot + 48);       /* current_size */
    q->h.cluster_size = p->block_size;
    q->h.cluster_bits = 21;                  /* 2MiB = 2^21 */
    return IMG_OK;
}

void img_vhd_close(img_qcow2 *q){
    if(!q || !q->fmt_priv) return;
    vhd_priv *p = (vhd_priv*)q->fmt_priv;
    free(p->bat);
    free(p);
    q->fmt_priv = NULL;
}

/* 读一个块（未分配 → 补 0） */
int img_vhd_read(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    if(!q || !q->fmt_priv || !buf) return IMG_EINVAL;
    vhd_priv *p = (vhd_priv*)q->fmt_priv;
    uint8_t *out = (uint8_t*)buf;
    memset(out, 0, len);

    size_t done = 0;
    while(done < len){
        uint64_t abs = off + done;
        if(abs >= q->h.size) break;
        uint32_t blk = (uint32_t)(abs / p->block_size);
        if(blk >= p->max_entries) break;
        uint64_t in_blk = abs % p->block_size;
        size_t n = (size_t)(len - done);
        if(n > p->block_size - in_blk) n = (size_t)(p->block_size - in_blk);

        uint32_t bat = p->bat[blk];
        if(bat == VHD_UNALLOC){ done += n; continue; }     /* 未分配 = 0 */

        /* ★ BAT 的值是【扇区号】，不是字节偏移（实测 qemu-img 写的 VHD：
         *   BAT[0]=8 → 块在 8*512=0x1000，前 512B 是位图，数据从 0x1200 开始）。 */
        uint64_t data_off = (uint64_t)bat * VHD_SECTOR + VHD_SECTOR + in_blk;
        size_t got = 0;
        int rc = img_io_read(q->io, data_off, out + done, n, &got);
        if(rc != IMG_OK) return rc;
        done += n;
    }
    return IMG_OK;
}

/* 写入动态 VHD：新块先写位图、数据、尾 footer，再发布 BAT 项。 */
int img_vhd_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
    if(!q || !q->fmt_priv || !buf) return IMG_EINVAL;
    if(q->read_only) return IMG_EREADONLY;
    vhd_priv *p = (vhd_priv*)q->fmt_priv;
    const uint8_t *in = (const uint8_t*)buf;

    size_t done = 0;
    while(done < len){
        uint64_t abs = off + done;
        if(abs >= q->h.size) break;
        uint32_t blk = (uint32_t)(abs / p->block_size);
        if(blk >= p->max_entries) break;
        uint64_t in_blk = abs % p->block_size;
        size_t n = (size_t)(len - done);
        if(n > p->block_size - in_blk) n = (size_t)(p->block_size - in_blk);

        if(p->bat[blk] == VHD_UNALLOC){
            uint64_t newblk = p->next_block; /* 原尾 footer 所在位置 */
            uint64_t next = newblk + VHD_SECTOR + p->block_size;
            if(next < newblk || next > (uint64_t)0xFFFFFFFEu * VHD_SECTOR ||
               img_io_size(q->io) != newblk + VHD_FOOTER_SIZE) return IMG_EFORMAT;

            /* 扩大文件并先写新 footer；进程被中断时，以末尾副本识别映像。 */
            int rc = img_io_truncate(q->io, next + VHD_FOOTER_SIZE);
            if(rc != IMG_OK) return rc;
            rc = img_io_write_full(q->io, next, p->footer, VHD_FOOTER_SIZE);
            if(rc != IMG_OK) return rc;
            uint8_t bitmap[VHD_SECTOR]; memset(bitmap, 0xFF, sizeof bitmap);
            rc = img_io_write_full(q->io, newblk, bitmap, sizeof bitmap);
            if(rc != IMG_OK) return rc;
            /* ftruncate 不会清除旧 footer；覆盖数据区起始扇区。 */
            uint8_t zero[VHD_FOOTER_SIZE] = {0};
            rc = img_io_write_full(q->io, newblk + VHD_SECTOR, zero, sizeof zero);
            if(rc != IMG_OK) return rc;
            rc = img_io_write_full(q->io, newblk + VHD_SECTOR + in_blk, in + done, n);
            if(rc != IMG_OK) return rc;
            /* 数据、位图和尾 footer 先持久化，再发布单项 BAT。 */
            rc = img_io_sync(q->io);
            if(rc != IMG_OK) return rc;
            uint8_t entry[4]; wr_be32u(entry, (uint32_t)(newblk / VHD_SECTOR));
            rc = img_io_write_full(q->io, p->table_offset + (uint64_t)blk * 4, entry, sizeof entry);
            if(rc != IMG_OK) return rc;
            p->bat[blk] = rd_be32u(entry);
            p->next_block = next;
        } else {
            uint64_t data_off = (uint64_t)p->bat[blk] * VHD_SECTOR + VHD_SECTOR + in_blk;
            int rc = img_io_write_full(q->io, data_off, in + done, n);
            if(rc != IMG_OK) return rc;
        }
        done += n;
    }
    return IMG_OK;
}

/* ---------------- 创建动态 VHD ----------------
 * 布局：[footer 副本][动态盘头][BAT][...数据区...] [footer]
 * qemu 兼容做法：footer 副本在最前（0x200 = data_offset 指向动态盘头）。
 * 所有"空盘"字段一次写齐；数据区是稀疏的（truncate 撑大）。
 */
int img_vhd_create(img_io *io, uint64_t bytes){
    if(!io || !bytes) return IMG_EINVAL;
    /* ★ 容量向上补齐到完整柱面（qemu 就是这么做的）。
     *   VHD 的 CHS 是权威字段：qemu 会用 cyl*heads*sectors*512 反推容量，
     *   容量不对齐就会报告成偏小的值（实测 1GiB 被报成 1073479680）。 */
    uint64_t vhd_cyl_units = 16ULL * 63ULL * VHD_SECTOR;          /* 一柱面字节数 */
    if(bytes > UINT64_MAX - vhd_cyl_units || bytes > 2040ULL * 1024 * 1024 * 1024)
        return IMG_ERANGE;
    uint64_t vsize = ((bytes + vhd_cyl_units - 1) / vhd_cyl_units) * vhd_cyl_units;
    /* ★ max_table_entries = 块数 + 1（qemu 1GiB 实测 513 = 512+1）。
     *   少算 1 项会让 BAT 末尾与 data_start 对不上。 */
    uint32_t max_entries = (uint32_t)((vsize + VHD_BLOCK_SIZE - 1) / VHD_BLOCK_SIZE + 1);

    uint8_t foot[VHD_FOOTER_SIZE]; memset(foot, 0, sizeof foot);
    memcpy(foot, "conectix", 8);
    wr_be32u(foot+8,  0x00000002);              /* features = reserved for Hyper-V */
    wr_be32u(foot+12, 0x00010000);              /* version 1.0 */
    wr_be64u(foot+16, VHD_FOOTER_SIZE);         /* data_offset → 动态盘头 */
    wr_be32u(foot+24, (uint32_t)time(NULL) - 946684800u);
    memcpy(foot+28, "vtxl", 4);                 /* creator_app（与 qemu 一致）*/
    wr_be32u(foot+32, 0x00040000);              /* creator_version 4.0 */
    memcpy(foot+36, "Wi2k", 4);                 /* creator_os */
    wr_be64u(foot+40, vsize);                   /* original_size */
    wr_be64u(foot+48, vsize);                   /* current_size */
    /* 几何：CHS 只作遗留字段，映像真实大小以 current_size 为准 */
    uint32_t heads = 16, sectors = 63;
    uint32_t cyl = (uint32_t)(vsize / vhd_cyl_units);
    if(cyl > 65535){ cyl = 65535; heads = 16; sectors = 255; }   /* 超 1020 GB：走 large */
    foot[56] = (uint8_t)(cyl >> 8); foot[57] = (uint8_t)cyl;
    foot[58] = (uint8_t)heads; foot[59] = (uint8_t)sectors;
    /* ★ disk_type 是【4 字节大端】@60，动态盘 = 3。
     *   原来写成 2 字节、值 2：宽度错（低位落到 62-63）值也错（2=固定盘语义） */
    wr_be32u(foot+60, 3);
    /* 从系统随机源获取 16 字节 UUID；失败时拒绝创建，避免重复标识符。 */
    {
        FILE *rng = fopen("/dev/urandom", "rb");
        if(!rng) return IMG_EIO;
        size_t got = fread(foot + 68, 1, 16, rng);
        fclose(rng);
        if(got != 16) return IMG_EIO;
        foot[68] = (uint8_t)((foot[68] & 0x0f) | 0x40);
        foot[74] = (uint8_t)((foot[74] & 0x3f) | 0x80);
    }

    uint8_t hdr[VHD_HEADER_SIZE]; memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "cxsparse", 8);
    /* hdr+8 = data_offset：无下一个头 → 全 1（qemu 实测同样写 0xFFFF...） */
    wr_be64u(hdr+8,  0xFFFFFFFFFFFFFFFFULL);
    wr_be64u(hdr+16, 1536);                     /* table_offset：动态头之后 */
    wr_be32u(hdr+24, 0x00010000);               /* header_version */
    wr_be32u(hdr+28, max_entries);
    wr_be32u(hdr+32, VHD_BLOCK_SIZE);
    /* hdr+36 checksum 先留 0，算完再回填 */

    /* BAT：★ 每项必须是 0xFFFFFFFF（未分配）。
     *   填 0 等于「第 0 块已分配到文件偏移 0」，qemu 会把 footer 当数据块读。 */
    uint64_t bat_bytes = ((uint64_t)max_entries * 4 + VHD_SECTOR - 1) / VHD_SECTOR * VHD_SECTOR;
    uint64_t data_start = 1536 + bat_bytes;

    /* 两个 checksum 依赖最终字节：先算 footer（校验位清零），再算 header */
    wr_be32u(foot+64, vhd_checksum32(foot, VHD_FOOTER_SIZE));
    wr_be32u(hdr+36, vhd_checksum32(hdr, VHD_HEADER_SIZE));

    /* 落盘顺序：文件定长 → footer 副本 → 动态头 → BAT → 尾 footer */
    if(img_io_truncate(io, data_start + VHD_FOOTER_SIZE) != IMG_OK) return IMG_EIO;
    if(img_io_write_full(io, 0, foot, sizeof foot) != IMG_OK) return IMG_EIO;
    if(img_io_write_full(io, VHD_FOOTER_SIZE, hdr, sizeof hdr) != IMG_OK) return IMG_EIO;
    {
        uint8_t *unalloc = (uint8_t*)malloc((size_t)bat_bytes);
        if(!unalloc) return IMG_ENOMEM;
        memset(unalloc, 0xFF, (size_t)bat_bytes); /* BAT 与扇区尾部均为未分配值 */
        int rc = img_io_write_full(io, 1536, unalloc, (size_t)bat_bytes);
        free(unalloc);
        if(rc != IMG_OK) return rc;
    }
    if(img_io_write_full(io, data_start, foot, sizeof foot) != IMG_OK) return IMG_EIO;
    return img_io_sync(io);
}
