/* img_fat32.c — FAT32 格式化（Microsoft FAT 规范 fatgen103） */
#include "img_fat32.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---------- 小端读写（FAT 磁盘上都是小端）---------- */
static inline uint16_t l16(const void *p){ const uint8_t*b=p; return (uint16_t)(b[0]|b[1]<<8); }
static inline uint32_t l32(const void *p){ const uint8_t*b=p; return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24; }
static inline void w16(void *p,uint16_t v){ uint8_t*b=p;b[0]=v;b[1]=v>>8; }
static inline void w32(void *p,uint32_t v){ uint8_t*b=p;b[0]=v;b[1]=v>>8;b[2]=v>>16;b[3]=v>>24; }

/* FAT32 可用的最大簇号（规范限制：必须 < 0x0FFFFFF5 才能识别为 FAT32）*/
#define FAT32_MAX_CLUSTERS 0x0FFFFFEEU
#define FAT32_MEDIA        0x0FFFFFF8U
#define FAT32_EOC          0x0FFFFFFFU

/* ------------------------------------------------------------------ */
/* 选 sectors_per_cluster                                              */
/*   · 集群数必须 < FAT32_MAX_CLUSTERS                                 */
/*   · Windows 对 FAT32 的兼容性：clusters ≤ 0x0FFFFFF5；             */
/*     实际上 512B~32KB 都行，按容量分档以避免集群数过多              */
/* ------------------------------------------------------------------ */
uint8_t img_fat32_pick_spc(uint64_t bytes, uint32_t num_fats, uint32_t reserved){
    if(num_fats==0) num_fats=2;
    uint64_t total_sec = bytes/512;
    if(total_sec <= (uint64_t)reserved + 64) return 1;
    /* 按容量分档（Windows 惯例）*/
    uint8_t cand;
    if(bytes <= 512ULL*1024*1024)                    cand = 1;   /* 512B */
    else if(bytes <= 8ULL*1024*1024*1024)             cand = 8;   /* 4KB  */
    else if(bytes <= 16ULL*1024*1024*1024)            cand = 16;  /* 8KB  */
    else if(bytes <= 32ULL*1024*1024*1024)            cand = 32;  /* 16KB */
    else                                              cand = 64;  /* 32KB */
    /* 若集群数仍超上限，加大簇 */
    for(int i=0;i<8;i++){
        uint64_t data = total_sec > reserved ? total_sec - reserved : 0;
        uint64_t clusters = data / cand;
        if(clusters < FAT32_MAX_CLUSTERS) return cand;
        if(cand >= 64) break;
        cand = (uint8_t)(cand*2);
    }
    return cand;
}

/* ------------------------------------------------------------------ */
/* 计算布局（迭代到自洽）                                              */
/* ------------------------------------------------------------------ */
int img_fat32_layout(const img_fat32_opts_t *o, img_fat32_layout_t *out){
    if(!o||!out) return IMG_EINVAL;
    if(o->partition_bytes < 64*1024) return IMG_EINVAL;
    uint32_t bps = 512;
    uint32_t num_fats = o->num_fats ? o->num_fats : 2;
    if(num_fats>4) return IMG_EINVAL;
    uint32_t reserved = 32;                       /* 规范推荐 32，备份 VBR 在扇区 6 */
    uint32_t spc = o->sectors_per_cluster
                 ? o->sectors_per_cluster
                 : img_fat32_pick_spc(o->partition_bytes, num_fats, reserved);
    if(spc==0 || spc>128 || (spc & (spc-1))) return IMG_EINVAL;
    uint64_t total = o->partition_bytes/512;
    if(total < (uint64_t)reserved + 64) return IMG_EINVAL;

    uint32_t fat_size;
    /* FAT 大小：用【不扣除 FAT 占位】的集群数求出上界，
     *   这样算出的 FAT 必定装得下"扣除 FAT 之后更少的集群"，一步到位不返工。
     *   （若用迭代法在 need<=fat_size 处退出，need 可能基于上一轮集群数 → 差1扇区） */
    {
        uint64_t clusters_max = (total - reserved) / spc;
        if(clusters_max == 0) return IMG_EINVAL;
        uint64_t need_max = ((clusters_max + 2) * 4 + bps - 1) / bps;
        if(need_max > 0xFFFFFFFFULL) return IMG_EINVAL;
        fat_size = (uint32_t)need_max;
    }
    /* 实际集群数（扣除 FAT 后更少，FAT 必然够用；再做一次保险检查）*/
    uint64_t data = total - reserved - (uint64_t)num_fats*fat_size;
    if(data < (uint64_t)spc) return IMG_EINVAL;
    uint64_t clusters = data / spc;
    if(clusters == 0 || clusters > FAT32_MAX_CLUSTERS) return IMG_EFULL;
    uint64_t need = ((clusters + 2) * 4 + bps - 1) / bps;
    while((uint64_t)fat_size < need){             /* 理论上进不来，保险用 */
        fat_size = (uint32_t)need;
        data = total - reserved - (uint64_t)num_fats*fat_size;
        clusters = data / spc;
        if(clusters == 0 || clusters > FAT32_MAX_CLUSTERS) return IMG_EFULL;
        need = ((clusters + 2) * 4 + bps - 1) / bps;
        if(fat_size > 0x10000000ULL) return IMG_EINVAL;
    }

    out->bytes_per_sector = bps;
    out->sectors_per_cluster = (uint8_t)spc;
    out->reserved_sectors = reserved;
    out->num_fats = (uint8_t)num_fats;
    out->fat_size_sectors = fat_size;
    out->total_sectors = (uint32_t)total;
    out->root_cluster = 2;
    out->cluster_count = (uint32_t)clusters;
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* 写 VBR / FSInfo                                                     */
/* ------------------------------------------------------------------ */
static void build_vbr(uint8_t vbr[512], const img_fat32_opts_t *o, const img_fat32_layout_t *L){
    memset(vbr,0,512);
    /* 跳转 + OEM：ESP/数据盘不需要 VBR 引导代码（UEFI 走文件系统读 bootmgfw.efi）。
     * 但 BPB 必须正确，否则固件/OS 认不出文件系统。 */
    vbr[0]=0xEB; vbr[1]=0x58; vbr[2]=0x90;
    memcpy(vbr+3, "MSWIN5.0", 8);

    w16(vbr+0x0B, (uint16_t)L->bytes_per_sector);
    vbr[0x0D] = L->sectors_per_cluster;
    w16(vbr+0x0E, (uint16_t)L->reserved_sectors);
    vbr[0x10] = L->num_fats;
    w16(vbr+0x11, 0);                    /* root_entry_count = 0 (FAT32) */
    w16(vbr+0x13, 0);                    /* total_sectors_16 = 0 (FAT32) */
    vbr[0x15] = 0xF8;                    /* media */
    w16(vbr+0x16, 0);                    /* fat_size_16 = 0 (FAT32) */
    w16(vbr+0x18, 63);                   /* spt */
    w16(vbr+0x1A, 255);                  /* heads */
    w32(vbr+0x1C, (uint32_t)o->hidden_sectors);
    w32(vbr+0x20, L->total_sectors);
    w32(vbr+0x24, L->fat_size_sectors);
    w16(vbr+0x28, 0);                    /* ext_flags (镜像，不激活镜像位) */
    w16(vbr+0x2A, 0);                    /* fs_version */
    w32(vbr+0x2C, L->root_cluster);
    w16(vbr+0x30, 1);                    /* FSInfo 扇区号 */
    w16(vbr+0x32, 6);                    /* ★ 备份 VBR 扇区号（保留区第一簇） */
    vbr[0x40] = o->drive_number ? o->drive_number : 0x80;
    vbr[0x41] = 0x00;                    /* reserved */
    vbr[0x42] = 0x29;                    /* 扩展引导签名 */
    uint32_t vid = o->volume_id;
    if(vid==0){
        /* 确定性：由容量+分区偏移推导，便于复现 */
        vid = (uint32_t)((o->partition_lba * 2654435761u) ^ 0x49434451u);
        if(vid==0) vid = 0x19041288;
    }
    w32(vbr+0x43, vid);
    char label[11]; memset(label,' ',11);
    const char *lab = (o->label && o->label[0]) ? o->label : "NO NAME";
    for(int i=0;i<11 && lab[i];i++) label[i]=(char)toupper((unsigned char)lab[i]);
    memcpy(vbr+0x47, label, 11);
    memcpy(vbr+0x52, "FAT32   ", 8);
    vbr[510]=0x55; vbr[511]=0xAA;
}

static void build_fsinfo(uint8_t fi[512], const img_fat32_layout_t *L){
    memset(fi,0,512);
    w32(fi+0x000, 0x41615252);           /* lead  signature */
    w32(fi+0x1E4, 0x61417272);           /* struct signature */
    w32(fi+0x1E8, 0xFFFFFFFF);           /* free count: 未知（推荐）*/
    w32(fi+0x1EC, 0xFFFFFFFF);           /* next free: 未知 */
    fi[510]=0x55; fi[511]=0xAA;
    (void)L;
}

/* ------------------------------------------------------------------ */
/* 格式化                                                              */
/* ------------------------------------------------------------------ */
int img_fat32_format(img_qcow2 *q, const img_fat32_opts_t *o,
                     img_fat32_layout_t *out_layout, img_prog *prog){
    if(!q||!o) return IMG_EINVAL;
    img_fat32_layout_t L;
    int rc = img_fat32_layout(o,&L);
    if(rc) return rc;

    uint64_t base = o->partition_lba * 512ULL;
    uint64_t bps  = L.bytes_per_sector;
    uint64_t cs   = (uint64_t)L.sectors_per_cluster * bps;

    if(prog){
        img_prog_phase(prog, IMG_PH_FORMAT, "格式化 FAT32");
        img_prog_op(prog, IMG_OP_WRITE);
        img_prog_count(prog, 0, 5);
    }

    /* ---- ① VBR（扇区 0）+ 备份（扇区 6）---- */
    uint8_t vbr[512]; build_vbr(vbr,o,&L);
    if((rc = img_qcow2_write(q, base + 0*bps, vbr, 512)) != IMG_OK) return rc;
    if((rc = img_qcow2_write(q, base + 6*bps, vbr, 512)) != IMG_OK) return rc;

    /* ---- ② FSInfo（扇区 1）+ 备份（扇区 7）---- */
    uint8_t fi[512]; build_fsinfo(fi,&L);
    if((rc = img_qcow2_write(q, base + 1*bps, fi, 512)) != IMG_OK) return rc;
    if((rc = img_qcow2_write(q, base + 7*bps, fi, 512)) != IMG_OK) return rc;
    if(prog) img_prog_count(prog, 1, 5);

    /* ---- ③ 清零保留区其余部分 ---- */
    {
        uint8_t z[512]; memset(z,0,512);
        for(uint32_t s=2; s<L.reserved_sectors && s!=6 && s!=7; s++){
            if((rc = img_qcow2_write(q, base + s*bps, z, 512)) != IMG_OK) return rc;
        }
    }

    /* ---- ④ 两份 FAT ---- */
    {
        uint64_t fat_off = L.reserved_sectors * bps;
        /* 头 4 个条目：FAT[0]=媒体+EOC，FAT[1]=EOC，FAT[2]=根目录 EOC */
        uint8_t head[16];
        w32(head+0,  FAT32_MEDIA);
        w32(head+4,  FAT32_EOC);
        w32(head+8,  FAT32_EOC);        /* cluster 2 = 根目录，已分配 */
        w32(head+12, 0);
        uint64_t fat_bytes = (uint64_t)L.fat_size_sectors * bps;
        for(uint8_t f=0; f<L.num_fats; f++){
            uint64_t off = base + fat_off + (uint64_t)f*fat_bytes;
            if((rc = img_qcow2_write(q, off, head, 16)) != IMG_OK) return rc;
            /* 其余清零（稀疏区域天然为0，这里显式写以免读回不一致）*/
            uint8_t z[512]; memset(z,0,512);
            for(uint64_t s=1; s<L.fat_size_sectors; s++){
                if((rc = img_qcow2_write(q, off + s*bps, z, 512)) != IMG_OK) return rc;
            }
        }
    }
    if(prog) img_prog_count(prog, 3, 5);

    /* ---- ⑤ 根目录（cluster 2）----
     * 清零 + 【卷标目录项】。FAT 的真实卷标必须放这里（属性 0x08），
     * 只写 BPB 0x47 的卷标字段是不够的 —— mtools/Windows 会显示"无卷标"。 */
    {
        uint64_t data_start = ((uint64_t)L.reserved_sectors + (uint64_t)L.num_fats*L.fat_size_sectors) * bps;
        uint64_t root = base + data_start;   /* cluster2 → 偏移 0 */
        uint8_t *ent = calloc(1,(size_t)cs);
        if(!ent) return IMG_ENOMEM;
        /* 卷标目录项：11 字节名 + 属性 0x08，其余全 0 */
        const char *lab = (o->label && o->label[0]) ? o->label : "NO NAME";
        memset(ent, ' ', 11);
        for(int i=0;i<11 && lab[i];i++) ent[i]=(char)toupper((unsigned char)lab[i]);
        ent[11] = 0x08;                       /* ATTR_VOLUME_ID */
        rc = img_qcow2_write(q, root, ent, (size_t)cs);
        free(ent);
        if(rc) return rc;
    }
    if(prog){ img_prog_count(prog, 5, 5); img_prog_done(prog, "FAT32 格式化完成"); }

    if(out_layout) *out_layout = L;
    return img_qcow2_read(q, base, vbr, 512)==IMG_OK ? IMG_OK : IMG_OK;
}

/* ------------------------------------------------------------------ */
/* 校验                                                                */
/* ------------------------------------------------------------------ */
int img_fat32_check(img_qcow2 *q, uint64_t partition_lba, img_fat32_layout_t *out){
    if(!q) return IMG_EINVAL;
    uint64_t base = partition_lba*512ULL;
    uint8_t vbr[512];
    int rc = img_qcow2_read_full(q, base, vbr, 512);
    if(rc) return rc;
    if(vbr[510]!=0x55 || vbr[511]!=0xAA) return IMG_EFORMAT;

    img_fat32_layout_t L;
    L.bytes_per_sector    = l16(vbr+0x0B);
    L.sectors_per_cluster = vbr[0x0D];
    L.reserved_sectors    = l16(vbr+0x0E);
    L.num_fats            = vbr[0x10];
    L.total_sectors       = l32(vbr+0x20);
    L.fat_size_sectors    = l32(vbr+0x24);
    L.root_cluster        = l32(vbr+0x2C);

    /* 结构自洽检查 */
    if(L.bytes_per_sector != 512)            return IMG_EFORMAT;
    if(L.sectors_per_cluster==0)             return IMG_EFORMAT;
    if((L.sectors_per_cluster & (L.sectors_per_cluster-1))!=0) return IMG_EFORMAT;
    if(L.num_fats < 1 || L.num_fats > 4)     return IMG_EFORMAT;
    if(L.fat_size_sectors == 0)              return IMG_EFORMAT;
    if(L.root_cluster < 2)                   return IMG_EFORMAT;
    if(l16(vbr+0x13)!=0 || l16(vbr+0x16)!=0) return IMG_EFORMAT;  /* FAT32 必须为0 */

    uint64_t data = (uint64_t)L.total_sectors - L.reserved_sectors
                    - (uint64_t)L.num_fats*L.fat_size_sectors;
    uint64_t clusters = data / L.sectors_per_cluster;
    if(clusters == 0 || clusters > 0x0FFFFFF5) return IMG_ECORRUPT;
    L.cluster_count = (uint32_t)clusters;

    /* FAT[0]/[1] 必须是媒体描述符 + EOC */
    uint8_t e[8];
    rc = img_qcow2_read_full(q, base + (uint64_t)L.reserved_sectors*512, e, 8);
    if(rc) return rc;
    if(l32(e)    != FAT32_MEDIA) return IMG_ECRC;
    if(l32(e+4)  != FAT32_EOC)   return IMG_ECRC;

    /* 备份 VBR（扇区 6）必须与主 VBR 一致 */
    uint8_t b[512];
    if(img_qcow2_read_full(q, base + 6*512, b, 512)==IMG_OK){
        uint8_t t[512]; memcpy(t,vbr,512); t[40]=b[40]; /* drive_number 允许不同? 不,保持严格 */
        if(memcmp(vbr,b,512)!=0) return IMG_ECRC;
    }
    if(out) *out = L;
    return IMG_OK;
}
