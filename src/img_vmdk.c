/* img_vmdk.c — VMDK（VMware）sparse 后端（P3-5）
 *
 * 只支持【sparse】格式（magic "KDMV"）。不支持：
 *   - streamOptimized（压缩 + 不同的 grain 标记）
 *   - 平面盘（flat：那个是 raw + 描述符，用 raw 后端即可）
 *   - 差分盘（parentCID != ffffffff）
 *
 * ★ 关键：VMDK 里所有「偏移」字段都是【扇区号】（512B），不是字节。
 *   实测 qemu-img 造的 vmdk：
 *     descriptorOffset=1  → 描述符在 512 字节处
 *     gdOffset=30         → GD 在 30*512=0x3C00
 *     GD[0]=0x1f          → 第 0 个 GT 在 31*512=0x3E00
 *   一开始按字节算 → 读到的是 sparse 头本身，全是垃圾。
 *
 * 布局（sparse）：
 *   [0]      sparse 头（512B，magic "KDMV"）
 *   [512]    描述符（embedded，文本）
 *   [overHead*512]  GD / RGD / GT 等元数据
 *   [grain 区]  实际数据（grainSize 个扇区 = 一个 grain）
 */
#include "img_qcow2_int.h"
#include "img_types.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define VMDK_SECTOR      512u
#define VMDK_UNALLOC     0u

typedef struct {
    uint32_t grain_size;        /* 扇区数（典型 128 = 64KB）*/
    uint32_t gtes_per_gt;       /* 每个 GT 的项数（典型 512）*/
    uint64_t gd_offset;         /* GD 字节偏移（已从扇区换算）*/
    uint32_t num_gts;           /* GD 项数 */
    uint32_t *gts;              /* GT 的扇区号数组（0=未分配）*/
    bool     dirty;
} vmdk_priv;

static uint32_t rd_le32(const uint8_t *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static uint64_t rd_le64(const uint8_t *p){
    uint64_t v=0; for(int i=7;i>=0;i--) v=(v<<8)|p[i]; return v;
}
static void wr_le32(uint8_t *p, uint32_t v){
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}

int img_vmdk_open(img_qcow2 *q){
    if(!q || !q->io) return IMG_EINVAL;
    uint8_t hdr[512];
    if(img_io_read_full(q->io, 0, hdr, sizeof hdr) != IMG_OK) return IMG_EIO;
    if(memcmp(hdr, "KDMV", 4)) return IMG_EFORMAT;

    uint32_t flags     = rd_le32(hdr + 8);
    uint64_t capacity  = rd_le64(hdr + 12);      /* 扇区 */
    uint32_t grain     = rd_le32(hdr + 20);      /* 扇区 */
    uint64_t gd_sect   = rd_le64(hdr + 56);
    uint32_t gtes      = rd_le32(hdr + 44);
    uint64_t overHead  = rd_le64(hdr + 64);

    if(flags & 0x10000) return IMG_ENOTSUP;      /* streamOptimized */
    if(!grain) grain = 128;
    if(!gtes) gtes = 512;

    vmdk_priv *p = (vmdk_priv*)calloc(1, sizeof *p);
    if(!p) return IMG_ENOMEM;
    p->grain_size  = grain;
    p->gtes_per_gt = gtes;
    p->gd_offset   = gd_sect * VMDK_SECTOR;      /* ★ 扇区号 → 字节 */

    /* GD 项数 = capacity 扇区 / (grain * gtes) */
    uint64_t grains_total = (capacity + grain - 1) / grain;
    p->num_gts = (uint32_t)((grains_total + gtes - 1) / gtes);
    if(p->num_gts == 0) p->num_gts = 1;

    p->gts = (uint32_t*)calloc(p->num_gts, sizeof(uint32_t));
    if(!p->gts){ free(p); return IMG_ENOMEM; }

    /* 读 GD（每项 4 字节，存 GT 的扇区号）*/
    uint8_t *gd = (uint8_t*)malloc((size_t)p->num_gts * 4);
    if(!gd){ free(p->gts); free(p); return IMG_ENOMEM; }
    if(img_io_read_full(q->io, p->gd_offset, gd, (size_t)p->num_gts * 4) != IMG_OK){
        free(gd); free(p->gts); free(p); return IMG_EIO;
    }
    for(uint32_t i=0;i<p->num_gts;i++) p->gts[i] = rd_le32(gd + (size_t)i*4);
    free(gd);

    q->fmt_priv = p;
    q->h.size = capacity * VMDK_SECTOR;
    q->h.cluster_size = (uint64_t)grain * VMDK_SECTOR;
    q->h.cluster_bits = 0;
    (void)overHead;
    return IMG_OK;
}

void img_vmdk_close(img_qcow2 *q){
    if(!q || !q->fmt_priv) return;
    vmdk_priv *p = (vmdk_priv*)q->fmt_priv;
    if(p->dirty && q->io){
        uint8_t *gd = (uint8_t*)malloc((size_t)p->num_gts * 4);
        if(gd){
            for(uint32_t i=0;i<p->num_gts;i++) wr_le32(gd + (size_t)i*4, p->gts[i]);
            (void)img_io_write_full(q->io, p->gd_offset, gd, (size_t)p->num_gts * 4);
            free(gd);
        }
    }
    free(p->gts);
    free(p);
    q->fmt_priv = NULL;
}

/* 读一个 grain：GD → GT → GTE（扇区号）→ 数据 */
int img_vmdk_read(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    if(!q || !q->fmt_priv || !buf) return IMG_EINVAL;
    vmdk_priv *p = (vmdk_priv*)q->fmt_priv;
    uint8_t *out = (uint8_t*)buf;
    memset(out, 0, len);

    uint64_t grain_bytes = (uint64_t)p->grain_size * VMDK_SECTOR;
    size_t done = 0;
    while(done < len){
        uint64_t abs = off + done;
        if(abs >= q->h.size) break;
        uint64_t grain_idx = abs / grain_bytes;
        uint64_t in_grain  = abs % grain_bytes;
        size_t n = (size_t)(len - done);
        if(n > grain_bytes - in_grain) n = (size_t)(grain_bytes - in_grain);

        uint32_t gt_idx = (uint32_t)(grain_idx / p->gtes_per_gt);
        uint32_t gte_idx = (uint32_t)(grain_idx % p->gtes_per_gt);
        if(gt_idx >= p->num_gts) break;

        uint32_t gt_sect = p->gts[gt_idx];
        if(gt_sect == VMDK_UNALLOC){ done += n; continue; }   /* 未分配 = 0 */

        /* 读这个 GTE（4 字节，扇区号）*/
        uint64_t gte_off = (uint64_t)gt_sect * VMDK_SECTOR + (uint64_t)gte_idx * 4;
        uint8_t gte[4];
        if(img_io_read_full(q->io, gte_off, gte, 4) != IMG_OK) return IMG_EIO;
        uint32_t data_sect = rd_le32(gte);
        if(data_sect == VMDK_UNALLOC){ done += n; continue; }

        uint64_t data_off = (uint64_t)data_sect * VMDK_SECTOR + in_grain;
        size_t got = 0;
        int rc = img_io_read(q->io, data_off, out + done, n, &got);
        if(rc != IMG_OK) return rc;
        done += n;
    }
    return IMG_OK;
}

/* 写一个 grain：未分配就在文件尾追加，更新 GT（必要时先建 GT）*/
int img_vmdk_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
    if(!q || !q->fmt_priv || !buf) return IMG_EINVAL;
    if(q->read_only) return IMG_EREADONLY;
    vmdk_priv *p = (vmdk_priv*)q->fmt_priv;
    const uint8_t *in = (const uint8_t*)buf;

    uint64_t grain_bytes = (uint64_t)p->grain_size * VMDK_SECTOR;
    size_t done = 0;
    while(done < len){
        uint64_t abs = off + done;
        if(abs >= q->h.size) break;
        uint64_t grain_idx = abs / grain_bytes;
        uint64_t in_grain  = abs % grain_bytes;
        size_t n = (size_t)(len - done);
        if(n > grain_bytes - in_grain) n = (size_t)(grain_bytes - in_grain);

        uint32_t gt_idx = (uint32_t)(grain_idx / p->gtes_per_gt);
        uint32_t gte_idx = (uint32_t)(grain_idx % p->gtes_per_gt);
        if(gt_idx >= p->num_gts) break;

        uint64_t fsz = img_io_size(q->io);

        /* ① GT 不存在 → 追加一个全 0 的 GT */
        if(p->gts[gt_idx] == VMDK_UNALLOC){
            uint64_t gt_off = (fsz + VMDK_SECTOR - 1) / VMDK_SECTOR * VMDK_SECTOR;
            size_t gt_bytes = (size_t)p->gtes_per_gt * 4;
            uint8_t *zero = (uint8_t*)calloc(1, gt_bytes);
            if(!zero) return IMG_ENOMEM;
            if(img_io_write_full(q->io, gt_off, zero, gt_bytes) != IMG_OK){ free(zero); return IMG_EIO; }
            free(zero);
            p->gts[gt_idx] = (uint32_t)(gt_off / VMDK_SECTOR);
            p->dirty = true;
        }

        /* ② GTE 不存在 → 追加一个 grain（全 0 数据）*/
        uint64_t gt_off = (uint64_t)p->gts[gt_idx] * VMDK_SECTOR;
        uint64_t gte_off = gt_off + (uint64_t)gte_idx * 4;
        uint8_t gte[4];
        if(img_io_read_full(q->io, gte_off, gte, 4) != IMG_OK) return IMG_EIO;
        uint32_t data_sect = rd_le32(gte);
        if(data_sect == VMDK_UNALLOC){
            fsz = img_io_size(q->io);
            uint64_t g_off = (fsz + VMDK_SECTOR - 1) / VMDK_SECTOR * VMDK_SECTOR;
            /* 稀疏：不真写 0，只把文件"撑"到 grain 末尾 */
            if(img_io_truncate(q->io, g_off + grain_bytes) != IMG_OK) return IMG_EIO;
            data_sect = (uint32_t)(g_off / VMDK_SECTOR);
            wr_le32(gte, data_sect);
            if(img_io_write_full(q->io, gte_off, gte, 4) != IMG_OK) return IMG_EIO;
        }

        /* ③ 写数据 */
        uint64_t data_off = (uint64_t)data_sect * VMDK_SECTOR + in_grain;
        if(img_io_write_full(q->io, data_off, in + done, n) != IMG_OK) return IMG_EIO;
        done += n;
    }
    return IMG_OK;
}
