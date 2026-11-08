/* img_vhd.c — VHD（Virtual PC / Hyper-V 旧格式）后端（P3-4）
 *
 * 只支持【动态】 VHD（disk_type=2）。固定盘（type=2 之外）本工具不处理 ——
 * 固定盘就是 raw + 512 字节 footer，用户用 raw 后端即可。
 *
 * 文件布局（动态 VHD）：
 *   [0]                    副本 footer（512B，可选但 qemu 会写）
 *   [512]                  动态盘头（1024B，cookie="cxsparse"）
 *   [1536]                 BAT（块分配表，每项 4 字节大端；0xFFFFFFFF = 未分配）
 *   [2048]                 数据块（每块 block_size 字节 + 512B 位图）
 *   [文件尾-512]           footer（512B，cookie="conectix"）
 *
 * 关键常量（全部大端）：
 *   footer.data_offset  = 512（指向动态盘头；0xFFFFFFFFFFFFFFFF = 固定盘）
 *   footer.current_size = 虚拟磁盘大小
 *   footer.disk_type    = 2（动态）
 *   header.table_offset = BAT 的绝对偏移
 *   header.block_size   = 2 MiB（qemu/Hyper-V 默认）
 *   BAT[i]              = 第 i 块数据的绝对偏移（0xFFFFFFFF = 未分配）
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
    bool     dirty;            /* BAT 有改动，close 时写回 */
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
static void wr_be16u(uint8_t *p, uint16_t v){
    p[0]=v>>8; p[1]=v;
}
static void wr_be64u(uint8_t *p, uint64_t v){
    for(int i=7;i>=0;i--){ p[i]=v&0xff; v>>=8; }
}

/* VHD 校验和：把 512/1024 字节全部求和，checksum 字段当 0 算，然后取反 */
/* VHD 校验和（★ 不是字节求和！）：
 *   把结构当成【大端 16 位字的数组】求和，进位折回低 16 位，再取反。
 *   实测 qemu-img 造的 VHD：footer 全 0 时 checksum=0xffff（sum=0 取反）。
 *   一开始写成"逐字节求和"→ qemu 报 Incorrect header checksum。
 *   32 位结构（动态盘头）同样按 16 位字算，只是结果放 32 位。
 */
static uint32_t vhd_sum16(const uint8_t *buf, size_t len){
    uint32_t sum = 0;
    for(size_t i = 0; i + 1 < len; i += 2)
        sum += ((uint32_t)buf[i] << 8) | buf[i+1];
    sum = (sum & 0xFFFF) + (sum >> 16);      /* 进位折叠 */
    sum = (sum & 0xFFFF) + (sum >> 16);
    return sum & 0xFFFF;
}
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
        /* 固定盘：当成 raw 处理（数据区就是文件本体，末尾 512 是 footer） */
        q->fmt = DISK_FMT_RAW;
        q->raw_size = fsz - VHD_FOOTER_SIZE;
        q->h.size = q->raw_size;
        return IMG_OK;
    }
    if(doff + VHD_HEADER_SIZE > fsz) return IMG_EFORMAT;

    uint8_t hdr[VHD_HEADER_SIZE];
    if(img_io_read_full(q->io, doff, hdr, sizeof hdr) != IMG_OK) return IMG_EIO;
    if(memcmp(hdr, "cxsparse", 8)) return IMG_EFORMAT;

    vhd_priv *p = (vhd_priv*)calloc(1, sizeof *p);
    if(!p) return IMG_ENOMEM;
    p->table_offset = rd_be64u(hdr + 16);
    p->max_entries  = rd_be32u(hdr + 28);
    p->block_size   = rd_be32u(hdr + 32);
    if(!p->block_size) p->block_size = VHD_BLOCK_SIZE;
    /* 数据区起点 = BAT 末尾按 512 对齐 */
    p->data_start   = (p->table_offset + (uint64_t)p->max_entries * 4 + VHD_SECTOR - 1)
                      / VHD_SECTOR * VHD_SECTOR;

    uint64_t blk = (uint64_t)p->block_size + VHD_SECTOR;   /* 数据 + 位图 */
    (void)blk;

    p->bat = (uint32_t*)calloc(p->max_entries ? p->max_entries : 1, sizeof(uint32_t));
    if(!p->bat){ free(p); return IMG_ENOMEM; }

    uint8_t *raw = (uint8_t*)malloc((size_t)p->max_entries * 4);
    if(!raw){ free(p->bat); free(p); return IMG_ENOMEM; }
    if(img_io_read_full(q->io, p->table_offset, raw, (size_t)p->max_entries * 4) != IMG_OK){
        free(raw); free(p->bat); free(p); return IMG_EIO;
    }
    for(uint32_t i=0;i<p->max_entries;i++) p->bat[i] = rd_be32u(raw + (size_t)i*4);
    free(raw);

    q->fmt_priv = p;
    q->h.size   = rd_be64u(foot + 48);       /* current_size */
    q->h.cluster_size = p->block_size;
    q->h.cluster_bits = 21;                  /* 2MiB = 2^21 */
    return IMG_OK;
}

void img_vhd_close(img_qcow2 *q){
    if(!q || !q->fmt_priv) return;
    vhd_priv *p = (vhd_priv*)q->fmt_priv;
    if(p->dirty && q->io){
        /* 写回 BAT */
        uint8_t *raw = (uint8_t*)malloc((size_t)p->max_entries * 4);
        if(raw){
            for(uint32_t i=0;i<p->max_entries;i++) wr_be32u(raw + (size_t)i*4, p->bat[i]);
            (void)img_io_write_full(q->io, p->table_offset, raw, (size_t)p->max_entries * 4);
            free(raw);
        }
        /* footer 的原始大小没变；但 BAT 变了要更新 footer 校验和？
         * 不需要 —— footer 里没有 BAT 相关信息。*/
    }
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

/* 写一个块：未分配就在文件尾追加一块（数据 + 512B 零位图），更新 BAT */
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

        /* 未分配 → 追加一块 */
        if(p->bat[blk] == VHD_UNALLOC){
            /* ★ 新块追加到【数据区末尾】，不能从文件尾算 ——
             *   文件尾是 footer（512B），从那里追加会把它盖掉。
             *   数据区末尾 = max(现有块末尾, data_start)。*/
            uint64_t fsz = img_io_size(q->io);
            uint64_t footer_off = (fsz >= VHD_SECTOR) ? (fsz - VHD_SECTOR) : 0;
            uint64_t newblk = footer_off;
            if(newblk < p->data_start) newblk = p->data_start;
            newblk = (newblk + VHD_SECTOR - 1) / VHD_SECTOR * VHD_SECTOR;
            /* 512B 位图（全 0 = 全部已用，VHD 语义：1=已用）——
             * 实际语义相反：位图里 1 = 该扇区【已分配】。qemu 造盘时是全 0xFF。
             * 我们写全 0xFF 表示整块都有效。*/
            uint8_t bmp[VHD_SECTOR]; memset(bmp, 0xFF, sizeof bmp);
            if(img_io_write_full(q->io, newblk, bmp, sizeof bmp) != IMG_OK) return IMG_EIO;
            /* 数据区补零（稀疏文件天然 0，但显式写一次更稳）*/
            p->bat[blk] = (uint32_t)(newblk / VHD_SECTOR);   /* 存扇区号 */
            p->dirty = true;
        }
        uint64_t data_off = (uint64_t)p->bat[blk] * VHD_SECTOR + VHD_SECTOR + in_blk;
        if(img_io_write_full(q->io, data_off, in + done, n) != IMG_OK) return IMG_EIO;
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
    /* VHD 要求大小按 512 对齐（qemu 是这样做的） */
    uint64_t vsize = ((bytes + VHD_SECTOR - 1) / VHD_SECTOR) * VHD_SECTOR;
    /* ★ max_table_entries = 块数 + 1（qemu 实测：64MB → 33 = 32+1）*/
    uint64_t max_entries = (vsize + VHD_BLOCK_SIZE - 1) / VHD_BLOCK_SIZE + 1;

    /* footer（512B） */
    uint8_t foot[VHD_FOOTER_SIZE]; memset(foot, 0, sizeof foot);
    memcpy(foot, "conectix", 8);
    wr_be32u(foot+8,  0x00000002);              /* features */
    wr_be32u(foot+12, 0x00010000);              /* version 1.0 */
    wr_be64u(foot+16, VHD_FOOTER_SIZE);         /* data_offset → 动态盘头 */
    /* timestamp: 当前时间（VHD 用秒，从 2000-01-01 起） */
    wr_be32u(foot+24, (uint32_t)time(NULL) - 946684800u);
    memcpy(foot+28, "imgt", 4);                 /* creator_app */
    wr_be32u(foot+32, 0x00010000);              /* creator_version 1.0 */
    memcpy(foot+36, "Wi2k", 4);                 /* creator_os */
    wr_be64u(foot+40, vsize);                   /* original_size */
    wr_be64u(foot+48, vsize);                   /* current_size */
    /* geometry: CHS 由大小推（简单用 LBA-assist，不太重要）*/
    uint32_t sectors_total = (uint32_t)(vsize / VHD_SECTOR);
    uint16_t cyl = 1, heads = 16, sect = 63;
    if(sectors_total > heads*sect){ cyl = (uint16_t)(sectors_total / (heads*sect)); if(cyl>65535) cyl=65535; }
    foot[56] = (uint8_t)(cyl>>8); foot[57] = (uint8_t)cyl;    /* 大端 cylinder */
    foot[58] = (uint8_t)heads; foot[59] = (uint8_t)sect;
    wr_be16u(foot+60, 2);                        /* disk_type = dynamic */
    /* checksum 在最后算 */

    /* 动态盘头（1024B） */
    uint8_t hdr[VHD_HEADER_SIZE]; memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "cxsparse", 8);
    /* hdr+8 = free_data_block_offset（下一个可写位置）。
     * ★ 不能写 0xFFFF...（那是固定盘语义，qemu 报 points after end of file）。
     *   正确 = 数据区起点（BAT 末尾）。BAT 大小下面才知道，先占位、最后回填。 */
    wr_be64u(hdr+8,  0xFFFFFFFFFFFFFFFFULL);   /* 无下一个头（照抄 qemu）*/
    wr_be64u(hdr+16, 1536);                     /* table_offset（BAT 紧跟动态盘头）*/
    wr_be32u(hdr+24, 0x00010000);               /* header_version 1.0 */
    wr_be32u(hdr+28, (uint32_t)max_entries);    /* max_table_entries */
    wr_be32u(hdr+32, VHD_BLOCK_SIZE);           /* block_size */
    wr_be32u(hdr+36, 0);                        /* checksum 后算 */

    /* footer 校验和 */
    /* ★ footer checksum @64 是【4 字节】，逐字节求和取反（实测 qemu 0xffffed9d 完全吻合）。
       一开始写成 2 字节 + 16 位字求和，全是错的。 */
    wr_be32u(foot+64, vhd_checksum32(foot, VHD_FOOTER_SIZE));
    wr_be32u(hdr+36, vhd_checksum32(hdr, VHD_HEADER_SIZE));

    /* 写：footer 副本 @0，header @512，BAT @2048，footer @文件尾 */
    int w1=img_io_write_full(io, 0, foot, sizeof foot);
    int w2=img_io_write_full(io, VHD_FOOTER_SIZE, hdr, sizeof hdr);
    if(w1 != IMG_OK) return w1;
    if(w2 != IMG_OK) return w2;
    uint64_t bat_bytes = ((uint64_t)max_entries * 4 + VHD_SECTOR - 1) / VHD_SECTOR * VHD_SECTOR;
    uint64_t data_start0 = 1536 + bat_bytes;   /* 512(footer)+1024(hdr)+BAT */
    /* 回填 free_data_block_offset 并重算 header checksum，然后重写动态头 */
    uint8_t *zero = (uint8_t*)calloc(1, (size_t)bat_bytes);
    if(!zero) return IMG_ENOMEM;
    if(img_io_write_full(io, 1536, zero, (size_t)bat_bytes) != IMG_OK){ free(zero); return IMG_EIO; }
    free(zero);
    /* ★ data_start 必须与 data_start0（free_data_block_offset）一致，
     *   否则 qemu 报 "free_data_block_offset points after the end of file"。*/
    uint64_t data_start = data_start0;
    /* 文件大小 = free + 512（与 qemu 一致：data_start 之后只放 1 个扇区的 footer 位）*/
    int trc = img_io_truncate(io, data_start + VHD_SECTOR);
    if(getenv("IMG_VHD_DEBUG")) fprintf(stderr,
        "[vhd] max_entries=%llu bat_bytes=%llu data_start=%llu truncate_to=%llu rc=%d\n",
        (unsigned long long)max_entries,(unsigned long long)bat_bytes,
        (unsigned long long)data_start,(unsigned long long)(data_start+VHD_SECTOR),trc);
    if(trc != IMG_OK) return trc;
    /* 尾 footer */
    /* ★ footer 必须写在【文件末尾】（qemu 读最后 512 字节当 footer）*/
    uint64_t fsz_now = img_io_size(io);
    if(fsz_now > 0 && fsz_now >= VHD_FOOTER_SIZE){
        if(img_io_write_full(io, fsz_now - VHD_FOOTER_SIZE, foot, sizeof foot) != IMG_OK) return IMG_EIO;
    }
    (void)img_io_sync(io);                    /* 确保四段都落盘（FUSE 上尤甚） */
    return IMG_OK;
}
