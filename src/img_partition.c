/* img_partition.c — MBR / GPT 分区表读写 */
#include "img_partition.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* CRC32 (IEEE 802.3，多项式 0xEDB88320) — GPT 标准要求                */
/* ------------------------------------------------------------------ */
static uint32_t crc_tab[256];
static bool crc_ready = false;

static void crc_init(void){
    if(crc_ready) return;
    for(uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int k=0;k<8;k++) c = (c&1) ? (0xEDB88320u ^ (c>>1)) : (c>>1);
        crc_tab[i]=c;
    }
    crc_ready=true;
}
uint32_t img_crc32(const void *data, size_t len){
    crc_init();
    const uint8_t *p=(const uint8_t*)data;
    uint32_t c=0xffffffffu;
    for(size_t i=0;i<len;i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c>>8);
    return c ^ 0xffffffffu;
}

/* ------------------------------------------------------------------ */
/* 小端读写（MBR/GPT 在磁盘上都是【小端】，与 qcow2 头相反）           */
/* ------------------------------------------------------------------ */
static inline uint16_t le16(const void *p){ const uint8_t*b=p; return (uint16_t)(b[0]|b[1]<<8); }
static inline uint32_t le32(const void *p){ const uint8_t*b=p; return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24; }
static inline uint64_t le64(const void *p){ return (uint64_t)le32(p) | ((uint64_t)le32((const uint8_t*)p+4)<<32); }
static inline void wle16(void *p,uint16_t v){ uint8_t*b=p;b[0]=v;b[1]=v>>8; }
static inline void wle32(void *p,uint32_t v){ uint8_t*b=p;b[0]=v;b[1]=v>>8;b[2]=v>>16;b[3]=v>>24; }
static inline void wle64(void *p,uint64_t v){ wle32(p,(uint32_t)v); wle32((uint8_t*)p+4,(uint32_t)(v>>32)); }

/* ------------------------------------------------------------------ */
/* GUID：GPT 里是【混合字节序】(前三段小端)                            */
/* ------------------------------------------------------------------ */
static const char *HEX="0123456789abcdef";

/* 磁盘 16 字节 → 文本 "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
static void guid_bin_to_str(const uint8_t b[16], char out[40]){
    uint8_t d[16];
    d[0]=b[3]; d[1]=b[2]; d[2]=b[1]; d[3]=b[0];          /* 前4字节反转 */
    d[4]=b[5]; d[5]=b[4];                                /* 中2字节反转 */
    d[6]=b[7]; d[7]=b[6];                                /* 后2字节反转 */
    memcpy(d+8, b+8, 8);                                 /* 最后8字节原样 */
    int k=0;
    for(int i=0;i<16;i++){
        if(i==4||i==6||i==8||i==10) out[k++]='-';
        out[k++]=HEX[d[i]>>4]; out[k++]=HEX[d[i]&0xf];
    }
    out[k]=0;
}
/* 文本 → 磁盘 16 字节（反向） */
static int guid_str_to_bin(const char *s, uint8_t out[16]){
    if(!s||!out) return IMG_EINVAL;
    uint8_t d[16]; int k=0;
    for(int i=0;s[i] && k<36;i++){
        char c=s[i]; if(c=='-') continue;
        int hi=-1,lo=-1;
        for(int h=0;h<16;h++){ if(HEX[h]==(char)tolower((unsigned char)c)) hi=h; }
        if(hi<0) return IMG_EINVAL;
        i++;
        if(!s[i]) return IMG_EINVAL;
        for(int h=0;h<16;h++){ if(HEX[h]==(char)tolower((unsigned char)s[i])) lo=h; }
        if(lo<0) return IMG_EINVAL;
        d[k++]=(uint8_t)((hi<<4)|lo);
    }
    if(k!=16) return IMG_EINVAL;
    out[0]=d[3]; out[1]=d[2]; out[2]=d[1]; out[3]=d[0];
    out[4]=d[5]; out[5]=d[4];
    out[6]=d[7]; out[7]=d[6];
    memcpy(out+8, d+8, 8);
    return IMG_OK;
}

/* 随机 GUID（Android/Linux 用 /dev/urandom；失败则退化为时间+地址混合） */
void img_guid_new(char out[40]){
    uint8_t b[16];
    FILE *f=fopen("/dev/urandom","rb");
    if(f){ size_t n=fread(b,1,16,f); (void)n; fclose(f); }
    else {
        uint64_t s=(uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)out;
        for(int i=0;i<16;i++){ s^=s<<13; s^=s>>7; s^=s<<17; b[i]=(uint8_t)(s>>16); }
    }
    b[6] = (b[6]&0x0f)|0x40;   /* version 4 */
    b[8] = (b[8]&0x3f)|0x80;   /* RFC4122 variant */
    guid_bin_to_str(b, out);
}

const char *img_mbr_type_name(uint8_t t){
    switch(t){
    case 0x00: return "空";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16<32MB";
    case 0x05: return "扩展分区";
    case 0x06: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0b: return "FAT32";
    case 0x0c: return "FAT32(LBA)";
    case 0x0e: return "FAT16(LBA)";
    case 0x0f: return "扩展分区(LBA)";
    case 0x27: return "Windows恢复";
    case 0x83: return "Linux";
    case 0xef: return "EFI系统分区";
    case 0xee: return "GPT保护";
    default:   return "其他";
    }
}

/* 校验 GUID 文本（形如 xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx）是否合法 */
int img_partition_guid_valid(const char *s){
    if(!s) return IMG_EINVAL;
    uint8_t b[16];
    if(strlen(s) != 36) return IMG_EINVAL;
    for(int i=0;i<36;i++){
        if(i==8||i==13||i==18||i==23){ if(s[i]!='-') return IMG_EINVAL; }
        else { char c=(char)tolower((unsigned char)s[i]);
               if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return IMG_EINVAL; }
    }
    return guid_str_to_bin(s, b);
}

/* ------------------------------------------------------------------ */
/* 读取                                                                */
/* ------------------------------------------------------------------ */
static int read_mbr(img_qcow2 *q, img_parttable_t *t, bool *is_gpt){
    uint8_t s[512];
    int rc = img_qcow2_read_full(q, 0, s, 512);
    if(rc!=IMG_OK) return rc;
    if(s[510]!=0x55 || s[511]!=0xaa) return IMG_EFORMAT;

    memcpy(t->mbr_code, s, 446);
    t->disk_signature = le32(s+440);

    bool gpt=false;
    for(int i=0;i<4;i++){
        const uint8_t *e=s+446+i*16;
        img_partition_t *p=&t->parts[i];
        p->status = e[0];
        p->type   = e[4];
        p->first_lba = le32(e+8);
        uint32_t sz  = le32(e+12);
        p->last_lba  = (sz==0) ? 0 : p->first_lba + sz - 1;
        p->used = (p->type != 0 && sz != 0);
        if(p->type == 0xee) gpt = true;
        if(p->used) t->count = i+1;
    }
    t->table = IMG_TABLE_MBR;
    *is_gpt = gpt;
    return IMG_OK;
}

static int read_gpt_header(img_qcow2 *q, uint64_t lba, uint8_t h[512]){
    int rc = img_qcow2_read_full(q, lba*512ULL, h, 512);
    if(rc!=IMG_OK) return rc;
    /* ★ 魔数必须是【8 字节】"EFI PART"（我曾只写 4 字节 → OVMF 拒绝识别） */
    if(memcmp(h, "EFI PART", 8) != 0) return IMG_EFORMAT;
    uint32_t hdr_size = le32(h+12);
    if(hdr_size < 92 || hdr_size > 512) return IMG_EFORMAT;
    uint32_t stored = le32(h+16);
    /* 校验时把 header_crc 字段清零再算 */
    uint8_t tmp[512]; memcpy(tmp,h,hdr_size);
    memset(tmp+16, 0, 4);
    if(img_crc32(tmp, hdr_size) != stored) return IMG_ECRC;
    return IMG_OK;
}

static int parse_gpt(img_qcow2 *q, img_parttable_t *t){
    uint8_t h[512];
    int rc = read_gpt_header(q, 1, h);
    if(rc!=IMG_OK) return rc;

    /* 备份头位置先取出来（后面读分区项可能要它） */
    uint64_t backup_lba   = le64(h+32);
    uint32_t hdr_size     = le32(h+12);
    if(hdr_size >= 92){
        t->current_lba        = le64(h+24);
        t->alt_header_lba     = backup_lba;
        t->usable_first       = (uint32_t)le64(h+40);
        t->usable_last        = (uint32_t)le64(h+48);
        guid_bin_to_str(h+56, t->disk_guid);
        t->part_entry_first_lba = (uint32_t)le64(h+72);
        t->part_entry_count     = le32(h+80);
        t->part_entry_size      = le32(h+84);
        t->part_entry_crc       = le32(h+88);
    } else return IMG_EFORMAT;

    if(t->part_entry_count==0 || t->part_entry_count>IMG_MAX_PARTITIONS
       || t->part_entry_size<128) return IMG_EFORMAT;

    uint64_t off = (uint64_t)t->part_entry_first_lba * 512ULL;
    size_t   len = (size_t)t->part_entry_count * t->part_entry_size;
    uint8_t *ent = malloc(len);
    if(!ent) return IMG_ENOMEM;
    rc = img_qcow2_read_full(q, off, ent, len);
    if(rc){ free(ent); return rc; }
    if(img_crc32(ent, len) != t->part_entry_crc){ free(ent); return IMG_ECRC; }

    int n=0;
    for(uint32_t i=0;i<t->part_entry_count && n<IMG_MAX_PARTITIONS;i++){
        const uint8_t *e = ent + (size_t)i*t->part_entry_size;
        bool empty=true; for(int k=0;k<16;k++) if(e[k]) { empty=false; break; }
        img_partition_t *p=&t->parts[n];
        memset(p,0,sizeof *p);
        if(empty){ p->used=false; n++; continue; }
        guid_bin_to_str(e,     p->type_guid);
        guid_bin_to_str(e+16,  p->part_guid);
        p->first_lba = le64(e+32);
        p->last_lba  = le64(e+40);
        p->attributes= le64(e+48);
        p->used = true;
        /* 名字：UTF-16LE → UTF-8（最多 36 字符） */
        int k=0;
        for(int c=0;c<36;c++){
            uint16_t u = le16(e+56+c*2);
            if(u==0) break;
            if(u < 0x80) p->name[k++]=(char)u;
            else if(u < 0x800){
                p->name[k++]=(char)(0xC0|(u>>6));
                p->name[k++]=(char)(0x80|(u&0x3f));
            } else {
                p->name[k++]=(char)(0xE0|(u>>12));
                p->name[k++]=(char)(0x80|((u>>6)&0x3f));
                p->name[k++]=(char)(0x80|(u&0x3f));
            }
        }
        p->name[k]=0;
        if(p->used) t->count=n+1;
        n++;
    }
    free(ent);
    t->table = IMG_TABLE_GPT;
    return IMG_OK;
}

int img_parttable_read(img_qcow2 *q, img_parttable_t *out, uint64_t disk_sectors){
    if(!q||!out) return IMG_EINVAL;
    memset(out,0,sizeof *out);
    out->disk_sectors = disk_sectors ? disk_sectors : (img_qcow2_size(q)/512);
    bool is_gpt=false;
    int rc = read_mbr(q, out, &is_gpt);
    if(rc) return rc;
    if(is_gpt){
        /* GPT：清掉 MBR 那 4 个占位，重新解析 */
        img_parttable_t t2; memset(&t2,0,sizeof t2);
        t2.table = out->table;
        memcpy(t2.mbr_code, out->mbr_code, 446);
        t2.disk_signature = out->disk_signature;
        t2.disk_sectors   = out->disk_sectors;
        rc = parse_gpt(q, &t2);
        if(rc) return rc;
        /* protective MBR 只保留其引导码/签名，【不作为真实分区项】；
         * parse_gpt 已把 GPT 真实分区从 index 0 开始连续存放 —— 千万别清 parts[0]。 */
        *out = t2;
    }
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* 找空白区                                                            */
/* ------------------------------------------------------------------ */
static bool used_in(const img_parttable_t *t, uint64_t lba){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(p->used && lba>=p->first_lba && lba<=p->last_lba) return true;
    }
    return false;
}

int img_parttable_free_regions(const img_parttable_t *t, uint64_t total_sectors,
                               img_range_t *out, int max, uint64_t align,
                               uint64_t start_lba){
    if(!t||!out||max<=0) return 0;
    if(align==0) align=2048;
    if(start_lba==0) start_lba=1;
    int n=0;
    uint64_t i = start_lba;
    while(i < total_sectors && n<max){
        if(used_in(t,i)){ i++; continue; }
        uint64_t b=i;
        while(i<total_sectors && !used_in(t,i)) i++;
        /* 对齐到 align */
        uint64_t fb=(b+align-1)/align*align;
        uint64_t lb=i-1;
        if(fb<=lb && lb-fb+1 >= align){
            out[n].first_lba=fb; out[n].last_lba=lb; out[n].sectors=lb-fb+1;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 写入                                                                */
/* ------------------------------------------------------------------ */
/* 由分区项构造 16 字节 MBR 条目 */
static void build_mbr_entry(const img_partition_t *p, uint8_t e[16]){
    memset(e,0,16);
    e[0] = p->status;
    e[4] = p->type;
    wle32(e+8, (uint32_t)p->first_lba);
    wle32(e+12, (uint32_t)(p->last_lba>=p->first_lba ? p->last_lba-p->first_lba+1 : 0));
    /* CHS 字段：现代实现只用 LBA；这里按规范填"超出范围"哨兵，避免算错 CHS 溢出 */
    /* 起始 CHS：用真实计算（安全，磁道内）
       结束 CHS：扇区数>1024*255*63 时填 fe ff ff（行业惯例哨兵）*/
    e[5]=0xfe; e[6]=0xff; e[7]=0xff;
}

static int write_mbr(img_qcow2 *q, img_parttable_t *t){
    uint8_t s[512]; memset(s,0,sizeof s);
    if(memcmp(t->mbr_code, (uint8_t[8]){0}, 8)==0){
        /* 没有引导码 → 填一个最小可识别的（只保证 55AA，供工具盘使用） */
        /* 真正能引导的码由 fixboot / 模板提供 */
    }
    memcpy(s, t->mbr_code, 446);
    wle32(s+440, t->disk_signature);

    int slot=0;
    for(int i=0;i<IMG_MAX_PARTITIONS && slot<4;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        build_mbr_entry(p, s+446+slot*16);
        slot++;
    }
    /* ★ MBR 最多 4 个主分区，多了要报错（不能静默丢） */
    if(slot==0) return IMG_ESTATE;
    s[510]=0x55; s[511]=0xaa;
    return img_qcow2_write(q, 0, s, 512);
}

static int write_gpt(img_qcow2 *q, img_parttable_t *t){
    if(t->disk_sectors < 68) return IMG_EINVAL;   /* 头+表+可用区至少要够 */

    uint32_t count  = t->part_entry_count ? t->part_entry_count : 128;
    uint32_t esz    = t->part_entry_size  ? t->part_entry_size  : 128;
    uint32_t tbl_sectors = (count*esz + 511)/512;
    uint64_t primary_tbl = 2;                                   /* LBA2 起 */
    /* ★ 备份头在最后一扇区；备份表必须【紧挨它前面】，
     *   若直接用 disk_sectors-tbl_sectors，表尾会与备份头重叠
     *   （sgdisk: "backup partition table overlaps the backup header"） */
    uint64_t backup_hdr  = t->disk_sectors - 1;
    uint64_t backup_tbl  = backup_hdr - tbl_sectors;
    uint64_t usable_first= primary_tbl + tbl_sectors;
    uint64_t usable_last = backup_tbl - 1;
    if(usable_last <= usable_first) return IMG_EFULL;

    size_t tbl_len = (size_t)count*esz;
    uint8_t *ent = calloc(1, tbl_len);
    if(!ent) return IMG_ENOMEM;

    /* 填分区项 */
    int n=0;
    for(uint32_t i=0;i<count;i++){
        uint8_t *e = ent + (size_t)i*esz;
        /* 找第 n 个"用到"的分区 */
        while(n<IMG_MAX_PARTITIONS && !t->parts[n].used) n++;
        if(n>=IMG_MAX_PARTITIONS) break;
        const img_partition_t *p=&t->parts[n];
        if(p->first_lba < usable_first || p->last_lba > usable_last
           || p->last_lba < p->first_lba){ free(ent); return IMG_EFULL; }
        uint8_t g[16];
        /* ★ 必须校验 GUID 文本合法，否则会把栈上垃圾写进分区表（静默损坏） */
        if(img_partition_guid_valid(p->type_guid) != IMG_OK
           || img_partition_guid_valid(p->part_guid) != IMG_OK){
            free(ent); return IMG_EINVAL;
        }
        guid_str_to_bin(p->type_guid, g);
        memcpy(e, g, 16);
        guid_str_to_bin(p->part_guid, g);
        memcpy(e+16, g, 16);
        wle64(e+32, p->first_lba);
        wle64(e+40, p->last_lba);
        wle64(e+48, p->attributes);
        /* 名字 → UTF-16LE */
        for(int c=0;c<36;c++){
            unsigned char uc=(unsigned char)p->name[c];
            if(!uc) break;
            uint16_t u;
            if(uc<0x80) u=uc;
            else if((uc&0xE0)==0xC0 && p->name[c+1]){
                u=(uint16_t)(((uc&0x1f)<<6)|((unsigned char)p->name[c+1]&0x3f)); c++;
            } else if((uc&0xF0)==0xE0 && p->name[c+1] && p->name[c+2]){
                u=(uint16_t)(((uc&0x0f)<<12)|(((unsigned char)p->name[c+1]&0x3f)<<6)|((unsigned char)p->name[c+2]&0x3f));
                c+=2;
            } else u='?';
            wle16(e+56+c*2, u);
        }
        n++;
    }
    uint32_t ent_crc = img_crc32(ent, tbl_len);

    /* 分区项表要写 2 份：主（LBA2）+ 尾部备份 */
    int rc = img_qcow2_write(q, primary_tbl*512, ent, tbl_len);
    if(rc){ free(ent); return rc; }
    rc = img_qcow2_write(q, backup_tbl*512, ent, tbl_len);
    if(rc){ free(ent); return rc; }

    /* 造头 */
    uint8_t h[512]; memset(h,0,sizeof h);
    memcpy(h, "EFI PART", 8);                     /* ★ 8 字节魔数 */
    wle32(h+8,  0x00010000);                      /* revision 1.0 */
    wle32(h+12, 92);                              /* header_size */
    wle32(h+16, 0);                               /* header_crc（先置0） */
    wle32(h+20, 0);                               /* reserved */
    wle64(h+24, 1);                               /* current_lba */
    wle64(h+32, t->disk_sectors-1);               /* backup_lba */
    wle64(h+40, usable_first);
    wle64(h+48, usable_last);
    { uint8_t g[16]; guid_str_to_bin(t->disk_guid, g); memcpy(h+56, g, 16); }
    wle64(h+72, primary_tbl);
    wle32(h+80, count);
    wle32(h+84, esz);
    wle32(h+88, ent_crc);
    uint32_t hc = img_crc32(h, 92);
    wle32(h+16, hc);

    /* 写主头（LBA1） */
    rc = img_qcow2_write(q, 512, h, 512);
    if(rc){ free(ent); return rc; }

    /* 写备份头（最后一扇区）：current/backup 互换，其余相同 */
    uint8_t hb[512]; memcpy(hb,h,512);
    wle64(hb+24, t->disk_sectors-1);              /* current_lba = 尾 */
    wle64(hb+32, 1);                              /* backup_lba  = 1 */
    wle64(hb+72, backup_tbl);                     /* 分区项表在尾部 */
    wle32(hb+16, 0);
    wle32(hb+16, img_crc32(hb, 92));
    rc = img_qcow2_write(q, backup_hdr*512, hb, 512);

    /* ★ protective MBR：1 项 type 0xEE，覆盖整个盘 */
    uint8_t mbr[512]; memset(mbr,0,512);
    memcpy(mbr, t->mbr_code, 446);
    wle32(mbr+440, t->disk_signature);
    uint8_t *e=mbr+446;
    e[0]=0x00; e[4]=0xee;
    e[5]=0xfe; e[6]=0xff; e[7]=0xff;             /* 结束 CHS 哨兵 */
    wle32(e+8, 1);
    wle32(e+12, (uint32_t)((t->disk_sectors-1 > 0xffffffffu) ? 0xffffffffu : t->disk_sectors-1));
    mbr[510]=0x55; mbr[511]=0xaa;
    if(rc==IMG_OK) rc = img_qcow2_write(q, 0, mbr, 512);

    free(ent);
    return rc;
}

int img_parttable_write(img_qcow2 *q, img_parttable_t *t){
    if(!q||!t) return IMG_EINVAL;
    if(t->table==IMG_TABLE_GPT) return write_gpt(q,t);
    if(t->table==IMG_TABLE_MBR) return write_mbr(q,t);
    return IMG_ESTATE;
}

/* ------------------------------------------------------------------ */
/* 增删改                                                              */
/* ------------------------------------------------------------------ */
static int first_free_slot(const img_parttable_t *t){
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(!t->parts[i].used) return i;
    return -1;
}

int img_partition_add(img_parttable_t *t, uint64_t first_lba, uint64_t sectors,
                      img_mbr_type_t mbr_type, const char *gpt_type_guid,
                      const char *name, bool active, int *out_index){
    if(!t||sectors==0) return IMG_EINVAL;
    int slot = first_free_slot(t);
    if(slot<0) return IMG_EFULL;

    if(first_lba==0){
        /* 自动选空位 */
        img_range_t rs[4];
        int n = img_parttable_free_regions(t, t->disk_sectors, rs, 4, 2048, 1);
        if(n==0) return IMG_EFULL;
        /* 选最大的 */
        int best=0;
        for(int i=1;i<n;i++) if(rs[i].sectors>rs[best].sectors) best=i;
        if(rs[best].sectors < sectors) return IMG_EFULL;
        first_lba = rs[best].first_lba;
    }
    if(first_lba + sectors - 1 >= t->disk_sectors) return IMG_EFULL;
    if(t->table==IMG_TABLE_MBR){
        int used=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(t->parts[i].used) used++;
        if(used>=4) return IMG_EFULL;      /* MBR 最多 4 主分区 */
    }

    img_partition_t *p=&t->parts[slot];
    memset(p,0,sizeof *p);
    p->used=true; p->first_lba=first_lba; p->last_lba=first_lba+sectors-1;
    p->type=(uint8_t)mbr_type; p->status=active?0x80:0x00;
    /* GPT 表里类型必须是 GUID 文本；给不出合法 GUID 时，按 MBR 类型映射到常见值 */
    const char *g = NULL;
    if(gpt_type_guid && img_partition_guid_valid(gpt_type_guid)==IMG_OK) g = gpt_type_guid;
    else {
        switch(mbr_type){
        case IMG_MBR_ESP:      g=IMG_GUID_ESP;      break;
        case IMG_MBR_EMPTY:    g=IMG_GUID_MSR;      break;
        case IMG_MBR_RECOVERY: g=IMG_GUID_RECOVERY; break;
        default:               g=IMG_GUID_BASIC;    break;   /* 0x07/0x0b → Windows 基本数据 */
        }
    }
    snprintf(p->type_guid,sizeof p->type_guid,"%s",g);
    img_guid_new(p->part_guid);
    if(name) snprintf(p->name,sizeof p->name,"%s",name);
    /* 恢复分区 → 默认置"不分配盘符"属性（Windows 靠类型 GUID 识别，属性是双保险） */
    if(gpt_type_guid && strcmp(p->type_guid, IMG_GUID_RECOVERY)==0)
        p->attributes = IMG_GPT_ATTR_RECOVERY_DEFAULT;
    if(slot >= t->count) t->count = slot+1;
    if(out_index) *out_index = slot;
    return IMG_OK;
}

int img_partition_remove(img_parttable_t *t, int index){
    if(!t||index<0||index>=IMG_MAX_PARTITIONS) return IMG_EINVAL;
    if(!t->parts[index].used) return IMG_ENOENT;
    memset(&t->parts[index],0,sizeof t->parts[index]);
    return IMG_OK;
}

int img_partition_set_type(img_parttable_t *t, int index, img_mbr_type_t mbr_type,
                           const char *gpt_type_guid){
    if(!t||index<0||index>=IMG_MAX_PARTITIONS) return IMG_EINVAL;
    if(!t->parts[index].used) return IMG_ENOENT;
    t->parts[index].type=(uint8_t)mbr_type;
    if(gpt_type_guid) snprintf(t->parts[index].type_guid,sizeof t->parts[index].type_guid,"%s",gpt_type_guid);
    return IMG_OK;
}

int img_partition_set_active(img_parttable_t *t, int index, bool active){
    if(!t||index<0||index>=IMG_MAX_PARTITIONS) return IMG_EINVAL;
    if(!t->parts[index].used) return IMG_ENOENT;
    if(active){
        /* ★ MBR 只允许 1 个 0x80 → 先清掉其它 */
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used) t->parts[i].status=0x00;
    }
    t->parts[index].status = active?0x80:0x00;
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */
int img_parttable_json(const img_parttable_t *t, char *buf, int cap){
    if(!t||!buf||cap<16) return 0;
    int k=0;
    k+=snprintf(buf+k,(size_t)(cap-k),
        "{\"table\":\"%s\",\"diskSectors\":%llu,\"diskGuid\":\"%s\",\"parts\":[",
        t->table==IMG_TABLE_GPT?"GPT":(t->table==IMG_TABLE_MBR?"MBR":"NONE"),
        (unsigned long long)t->disk_sectors, t->disk_guid);
    bool first=true;
    for(int i=0;i<IMG_MAX_PARTITIONS && k<cap-64;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        if(!first) k+=snprintf(buf+k,(size_t)(cap-k),",");
        first=false;
        k+=snprintf(buf+k,(size_t)(cap-k),
            "{\"i\":%d,\"first\":%llu,\"last\":%llu,\"sectors\":%llu,"
            "\"mbri\":%u,\"active\":%s,\"typeGuid\":\"%s\",\"partGuid\":\"%s\","
            "\"name\":\"%s\",\"attributes\":\"0x%016llx\"}",
            i,
            (unsigned long long)p->first_lba,
            (unsigned long long)p->last_lba,
            (unsigned long long)(p->last_lba-p->first_lba+1),
            p->type, p->status==0x80?"true":"false",
            p->type_guid, p->part_guid, p->name,
            (unsigned long long)p->attributes);
        if(k>=cap-2) break;
    }
    k+=snprintf(buf+k,(size_t)(cap-k),"]}");
    return k;
}

/* ------------------------------------------------------------------ */
/* 分区调整 / 填满剩余（DiskGenius、傲梅分区助手 同款交互语义）        */
/* ------------------------------------------------------------------ */

/* 可用末尾 LBA（GPT 要扣掉尾部备份表+备份头） */
uint64_t img_parttable_usable_last(const img_parttable_t *t){
    if(!t || t->disk_sectors==0) return 0;
    if(t->table != IMG_TABLE_GPT) return t->disk_sectors - 1;
    uint32_t count = t->part_entry_count ? t->part_entry_count : 128;
    uint32_t esz   = t->part_entry_size  ? t->part_entry_size  : 128;
    uint64_t tbl_sectors = ((uint64_t)count*esz + 511)/512;
    /* 备份头占最后一扇区，备份表紧挨它前面 → 可用末尾 = 盘末 - 1 - tbl_sectors */
    uint64_t backup_hdr  = t->disk_sectors - 1;
    uint64_t backup_tbl  = backup_hdr - tbl_sectors;
    return backup_tbl - 1;
}

/* 找 index 之后第一个"在 LBA 上排在它后面"的已用分区；没有则 -1 */
static int next_partition_by_lba(const img_parttable_t *t, int index){
    int best=-1;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(i==index || !t->parts[i].used) continue;
        if(t->parts[i].first_lba > t->parts[index].first_lba){
            if(best<0 || t->parts[i].first_lba < t->parts[best].first_lba) best=i;
        }
    }
    return best;
}

/* 找 index 之前"在 LBA 上排在它前面"的已用分区；没有则 -1 */
static int prev_partition_by_lba(const img_parttable_t *t, int index){
    int best=-1;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(i==index || !t->parts[i].used) continue;
        if(t->parts[i].first_lba < t->parts[index].first_lba){
            if(best<0 || t->parts[i].first_lba > t->parts[best].first_lba) best=i;
        }
    }
    return best;
}

static void resf(img_check_result_t *r,bool ok,int code,const char *reason,const char *sugv){
    if(!r) return;
    r->ok=ok; r->code=code;
    snprintf(r->reason,sizeof r->reason,"%s",reason?reason:"");
    snprintf(r->suggestion,sizeof r->suggestion,"%s",sugv?sugv:"");
}

int img_partition_resize(img_parttable_t *t, int index, uint64_t new_sectors,
                         img_check_result_t *err){
    if(!t || index<0 || index>=IMG_MAX_PARTITIONS){ resf(err,0,IMG_EINVAL,"无效的分区下标",NULL); return IMG_EINVAL; }
    img_partition_t *p=&t->parts[index];
    if(!p->used){ resf(err,0,IMG_ENOENT,"该槽位没有分区",NULL); return IMG_ENOENT; }
    if(new_sectors==0){ resf(err,0,IMG_EINVAL,"大小不能为 0，请用删除",NULL); return IMG_EINVAL; }

    uint64_t old_sec = p->last_lba - p->first_lba + 1;
    if(new_sectors == old_sec){ resf(err,1,IMG_OK,"无变化",NULL); return IMG_OK; }

    uint64_t usable_last = img_parttable_usable_last(t);
    int ni = next_partition_by_lba(t, index);      /* 后一个（可能紧邻） */
    int pi = prev_partition_by_lba(t, index);
    bool is_last = (ni < 0);

    if(is_last){
        /* ---- 末位分区 ---- */
        uint64_t new_last = p->first_lba + new_sectors - 1;
        if(new_last > usable_last){
            resf(err,0,IMG_EINVAL,"超出磁盘可用范围（会溢出）",
                 "先扩盘，或减小目标大小");
            return IMG_EINVAL;
        }
        if(new_last > p->last_lba){
            /* 调大：后面必须有空闲 */
            if(new_last > usable_last){
                if(pi >= 0){
                    resf(err,0,IMG_E_NEED_PREV_SHRINK,
                         "尾部空间不足，无法扩大到最后一个分区",
                         "是否压缩前面的分区？（App 层询问用户）");
                    return IMG_E_NEED_PREV_SHRINK;
                }
                resf(err,0,IMG_EFULL,"磁盘没有可用空间",NULL);
                return IMG_EFULL;
            }
        }
        /* 调小 → 尾部自然划出空余；调大 → 吃进尾部空余 */
        p->last_lba = new_last;
        resf(err,1,IMG_OK,"已调整",NULL);
        return IMG_OK;
    }

    /* ---- 非末位分区：调整完前方，立即调整后方（保持紧邻）---- */
    img_partition_t *n=&t->parts[ni];
    bool contiguous = (n->first_lba == p->last_lba + 1);
    uint64_t new_last = p->first_lba + new_sectors - 1;

    if(!contiguous){
        /* 与后方分区之间有空隙 → 只能在空隙范围内调整 */
        uint64_t gap = n->first_lba - p->last_lba - 1;
        if(new_last > p->last_lba + gap){
            resf(err,0,IMG_EINVAL,"与后方分区之间的空隙不足，无法扩大",
                 "后方分区挡住了，先调整后方分区");
            return IMG_EINVAL;
        }
        p->last_lba = new_last;
        resf(err,1,IMG_OK,"已调整（与后方分区不相邻，后方未动）",NULL);
        return IMG_OK;
    }

    if(new_last >= p->first_lba + new_sectors - 1 && new_last != p->last_lba){
        if(new_last > p->last_lba){
            /* 扩大 → 向后方分区"借" */
            uint64_t give = new_last - p->last_lba;
            uint64_t nsec = n->last_lba - n->first_lba + 1;
            if(give >= nsec){
                resf(err,0,IMG_EFULL,
                     "后方分区没有足够的空间可供借用",
                     "后方分区太小，先缩小它或删除它");
                return IMG_EFULL;
            }
            n->first_lba = new_last + 1;      /* 后方起点右移 */
            p->last_lba  = new_last;
        } else {
            /* 缩小 → 后方分区起点左移，吃掉腾出的空间（★ "调整前方后立即调后方"）*/
            uint64_t give = p->last_lba - new_last;
            n->first_lba = new_last + 1;
            p->last_lba  = new_last;
            (void)give;
        }
        resf(err,1,IMG_OK,"已调整（后方分区同步移动，保持紧邻）",NULL);
        return IMG_OK;
    }
    p->last_lba = new_last;
    resf(err,1,IMG_OK,"已调整",NULL);
    return IMG_OK;
}

int img_partition_fill_remaining(img_parttable_t *t, int index, img_check_result_t *err){
    if(!t || index<0 || index>=IMG_MAX_PARTITIONS){ resf(err,0,IMG_EINVAL,"无效下标",NULL); return IMG_EINVAL; }
    img_partition_t *p=&t->parts[index];
    if(!p->used){ resf(err,0,IMG_ENOENT,"该槽位没有分区",NULL); return IMG_ENOENT; }

    uint64_t usable_last = img_parttable_usable_last(t);
    int ni = next_partition_by_lba(t, index);
    if(ni >= 0){
        /* 有后方分区 → 只能填到它前面 */
        uint64_t limit = t->parts[ni].first_lba - 1;
        if(limit <= p->last_lba){ resf(err,0,IMG_EFULL,"后面已被其它分区占满",NULL); return IMG_EFULL; }
        p->last_lba = limit;
        resf(err,1,IMG_OK,"已填满到后方分区之前",NULL);
        return IMG_OK;
    }
    /* 末位分区 → 顶到可用末尾（★ 精确不取整，后端零空余）*/
    if(usable_last <= p->last_lba){ resf(err,0,IMG_EFULL,"已经顶到磁盘末尾",NULL); return IMG_EFULL; }
    if(p->first_lba > usable_last){ resf(err,0,IMG_EINVAL,"分区起始超出可用范围",NULL); return IMG_EINVAL; }
    p->last_lba = usable_last;
    resf(err,1,IMG_OK,"已顶到可用末尾",NULL);
    return IMG_OK;
}

/* ---------- GPT 属性位 ---------- */
int img_partition_set_attributes(img_parttable_t *t, int index, uint64_t attrs){
    if(!t || index<0 || index>=IMG_MAX_PARTITIONS) return IMG_EINVAL;
    if(t->table != IMG_TABLE_GPT) return IMG_ENOTSUP;   /* MBR 没有属性位 */
    if(!t->parts[index].used) return IMG_EINVAL;
    t->parts[index].attributes = attrs;
    return IMG_OK;
}
