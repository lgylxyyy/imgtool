#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_bootcheck.c — 引导链自检 */
#include "img_bootcheck.h"
#include "img_text.h"
#include "img_fat32.h"
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include "img_ntfs_fs.h"      /* ★ REQ-54：img_ntfs_read_file */
#include "img_identify.h"     /* ★ REQ-54：img_pe_version() 读 PE machine 判位数 */
#include "ntfs_bridge.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>        /* 必须在 ntfs 头之前（ntfstime.h 会重定义 timespec）*/
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

/* 设备桥接统一在 ntfs_bridge.c（本文件用 img_ntfs_device_ops()）*/

/* ---------- 用 libntfs-3g 查 NTFS 里某个路径是否存在 ---------- */
static bool ntfs_path_exists(img_qcow2 *q, uint64_t lba, uint64_t bytes, const char *path){
    if(!q) return false;
    if(img_ntfs_probe(q,lba,NULL)!=IMG_OK) return false;
    img_ntfs_ctx_activate(q,lba,bytes);
    const char *dn = img_dev_placeholder(); if(!dn) dn = "imgtool-dev";
    struct ntfs_device *dev = ntfs_device_alloc(dn,0,img_ntfs_device_ops(),NULL);
    if(dev) dev->d_private = img_ntfs_ctx_current();
    if(!dev){ img_ntfs_ctx_deactivate(); return false; }

    ntfs_volume *vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY);
    if(!vol){
        if(getenv("IMG_BC_DEBUG")) fprintf(stderr,"  [dbg] mount fail errno=%d\n", errno);
        ntfs_device_free(dev);
        img_ntfs_ctx_deactivate();
        return false;
    }
    bool found = false;
    char work[512];
    snprintf(work,sizeof work,"%s",path);
    ntfs_inode *cur = ntfs_inode_open(vol, FILE_root);
    if(cur){
        char *save = NULL;
        for(char *tok = strtok_r(work,"/\\",&save); tok; tok = strtok_r(NULL,"/\\",&save)){
            ntfschar *uname = NULL;
            size_t ulen = strlen(tok);
            int rc = ntfs_mbstoucs((const char*)tok, &uname);
            if(getenv("IMG_BC_DEBUG"))
                fprintf(stderr,"  [dbg]   tok='%s' mbstoucs=%d\n", tok, rc);
            if(rc < 0 || !uname){ ntfs_inode_close(cur); cur = NULL; break; }
            MFT_REF ref = ntfs_inode_lookup_by_name(cur, uname, ulen);
            free(uname);
            if(!ref){ ntfs_inode_close(cur); cur = NULL; break; }
            ntfs_inode *nxt = ntfs_inode_open(vol, ref);
            ntfs_inode_close(cur);
            cur = nxt;
            if(!cur) break;
        }
        if(cur){ found = true; ntfs_inode_close(cur); }
    }
    /* ★ ntfs_umount() 内部已调用 ntfs_device_free(dev)，这里不可再调（double free）*/
    ntfs_umount(vol, false);
    img_ntfs_ctx_deactivate();
    return found;
}

/* ---------- FAT32 目录项查找（简单逐级） ---------- */
typedef struct { uint32_t cluster; uint32_t size; bool is_dir; bool found; } fat_ent;

static fat_ent fat32_find(img_qcow2 *q, uint64_t lba, uint64_t bytes, const char *path);

static void fat32_read_cluster(img_qcow2 *q, uint64_t base, const img_fat32_layout_t *L,
                               uint32_t clus, uint8_t *buf){
    uint64_t data_start = ((uint64_t)L->reserved_sectors + (uint64_t)L->num_fats*L->fat_size_sectors) * 512;
    uint64_t off = base + data_start + (uint64_t)(clus - 2) * (uint64_t)L->sectors_per_cluster * 512;
    img_qcow2_read_full(q, off, buf, (size_t)L->sectors_per_cluster * 512);
}
static uint32_t fat32_next(img_qcow2 *q, uint64_t base, const img_fat32_layout_t *L, uint32_t clus){
    uint8_t e[4];
    uint64_t off = base + (uint64_t)L->reserved_sectors*512 + (uint64_t)clus*4;
    if(img_qcow2_read_full(q, off, e, 4)!=IMG_OK) return 0;
    return (uint32_t)e[0]|((uint32_t)e[1]<<8)|((uint32_t)e[2]<<16)|(uint32_t)e[3];
}
/* 组装 LFN：FAT 目录里 0x0F 属性的条目按【序号倒序】排列在 8.3 条目前面。
 * 每个 LFN 项提供 13 个 UTF-16 字符。我们按序号把它拼起来。 */
typedef struct { char name[260]; bool valid; } lfn_buf;
static void lfn_clear(lfn_buf *b){ b->name[0]=0; b->valid=false; }
static void lfn_add_entry(lfn_buf *b, const uint8_t *e){
    int ord = e[0] & 0x1F;
    if(ord==0 || ord>20) { lfn_clear(b); return; }
    size_t off = (size_t)(ord-1)*13;
    /* 5 个 UTF-16 字段：1..10, 14..25, 28..31 */
    static const int starts[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    for(int i=0;i<13;i++){
        unsigned c = e[starts[i]] | ((unsigned)e[starts[i]+1]<<8);
        if(off + (size_t)i >= sizeof b->name - 1) break;
        if(c==0){ b->name[off+i]=0; b->valid=true; return; }
        /* 仅存 ASCII 子集即可满足比对需求；非 ASCII 用 ? 代替 */
        b->name[off+i] = (c<128) ? (char)c : '?';
        b->name[off+ (size_t)i + 1] = 0;
    }
    b->valid = true;
}
/* 大小写不敏感的字符串比较（ASCII） */
static int ascii_ieq(const char *a, const char *b){
    while(*a && *b){
        char x=*a, y=*b;
        if(x>='a'&&x<='z') x-=32;
        if(y>='a'&&y<='z') y-=32;
        if(x!=y) return 0;
        a++; b++;
    }
    return *a==0 && *b==0;
}
static bool fat32_name_match(const uint8_t *ent, const char *want, const lfn_buf *lfn){
    /* ① 优先比 LFN（长名）——真实镜像里 Microsoft / bootmgfw.efi 全靠它 */
    if(lfn && lfn->valid && lfn->name[0]){
        if(ascii_ieq(lfn->name, want)) return true;
    }
    /* ② 回退 8.3：名(8) + 空格 + 扩展(3)，比对时去掉空格 */
    char n[13]; int j=0;
    for(int i=0;i<8 && ent[i]!=' ';i++) n[j++]=(char)ent[i];
    if(ent[8]!=' '){ n[j++]='.'; for(int i=8;i<11 && ent[i]!=' ';i++) n[j++]=(char)ent[i]; }
    n[j]=0;
    for(int i=0;n[i];i++) if(n[i]>='a'&&n[i]<='z') n[i]-=32;
    char w[64]; snprintf(w,sizeof w,"%s",want);
    for(int i=0;w[i];i++) if(w[i]>='a'&&w[i]<='z') w[i]-=32;
    return strcmp(n,w)==0;
}
static fat_ent fat32_in_dir(img_qcow2 *q, uint64_t base, const img_fat32_layout_t *L,
                            uint32_t dirclus, const char *want){
    fat_ent r={0};
    size_t csz = (size_t)L->sectors_per_cluster * 512;
    uint8_t *buf = malloc(csz);
    if(!buf) return r;
    lfn_buf lfn; lfn_clear(&lfn);
    uint32_t c = dirclus;
    while(c>=2 && c<0x0FFFFFF8){
        fat32_read_cluster(q, base, L, c, buf);
        bool stop=false;
        for(size_t i=0;i+32<=csz;i+=32){
            uint8_t *e = buf+i;
            if(e[0]==0x00){ stop=true; break; }
            if(e[0]==0xE5){ lfn_clear(&lfn); continue; }
            if((e[11] & 0x0F)==0x0F){ lfn_add_entry(&lfn, e); continue; }  /* LFN */
            if(e[11] & 0x08){ lfn_clear(&lfn); continue; }                 /* 卷标 */
            if(fat32_name_match(e, want, &lfn)){
                r.found=true;
                r.is_dir=(e[11]&0x10)!=0;
                /* FAT32 目录项（32 字节）字段偏移：
                 *   26..27 = 起始簇号【低 16 位】
                 *   20..21 = 起始簇号【高 16 位】  ← 原实现漏了，簇号被读成垃圾
                 *   28..31 = 文件大小
                 * 原代码把 28..29 当成簇号高位 → clus=2480930822 这种天文数字。 */
                r.cluster = ((uint32_t)e[26] | ((uint32_t)e[27]<<8))
                          | (((uint32_t)e[20] | ((uint32_t)e[21]<<8))<<16);
                r.size = (uint32_t)e[28] | ((uint32_t)e[29]<<8)
                       | ((uint32_t)e[30]<<16) | ((uint32_t)e[31]<<24);
                free(buf);
                return r;
            }
            lfn_clear(&lfn);
        }
        if(stop) break;
        c = fat32_next(q, base, L, c);
    }
    free(buf);
    return r;
}
static fat_ent fat32_find(img_qcow2 *q, uint64_t lba, uint64_t bytes, const char *path){
    (void)bytes;
    fat_ent r={0};
    uint64_t base=lba*512;
    img_fat32_layout_t L;
    if(img_fat32_check(q,lba,&L)!=IMG_OK) return r;
    uint32_t cur = L.root_cluster;
    char work[512]; snprintf(work,sizeof work,"%s",path);
    char *save=NULL;
    for(char *tok=strtok_r(work,"/\\",&save); tok; tok=strtok_r(NULL,"/\\",&save)){
        r = fat32_in_dir(q, base, &L, cur, tok);
        if(!r.found){ r.found=false; return r; }
        if(r.is_dir) cur = r.cluster;
    }
    return r;
}

/* ★ REQ-54：从 FAT32 读文件头若干字节（只为判 PE machine，读 4KB 足够）。
 *   返回实际读到的字节数；失败返回 0。
 *   实现：沿 cluster 链读，最多读 want 字节。 */
static size_t fat32_read_head(img_qcow2 *q, uint64_t lba, uint32_t start_clus,
                              uint32_t file_size, uint8_t *out, size_t want){
    img_fat32_layout_t L;
    if(img_fat32_check(q,lba,&L)!=IMG_OK) return 0;
    uint64_t base = lba*512;
    size_t clen = (size_t)L.sectors_per_cluster*512;
    uint64_t data_start = ((uint64_t)L.reserved_sectors + (uint64_t)L.num_fats*L.fat_size_sectors)*512;
    size_t need = file_size < want ? (size_t)file_size : want;
    size_t got = 0;
    uint32_t clus = start_clus;
    uint8_t *cb = malloc(clen);
    if(!cb) return 0;
    while(got < need && clus >= 2 && clus < 0x0FFFFFF8u){
        uint64_t off = base + data_start + (uint64_t)(clus-2)*clen;
        if(img_qcow2_read_full(q,off,cb,clen)!=IMG_OK) break;
        size_t n = need-got; if(n > clen) n = clen;
        memcpy(out+got, cb, n); got += n;
        if(got >= need) break;
        clus = fat32_next(q,base,&L,clus);
    }
    free(cb);
    return got;
}
/* ★ REQ-54：只读 PE machine 字段（不解析版本资源、不要求可选头完整）。
 *   支持"前面有引导壳"的情况（Win11 的 bootmgr 前置 24KB 16 位壳），
 *   在前 64KB 里找第一个合法的 MZ+PE\0\0。返回 0 = 判不出。 */
static uint16_t pe_machine_from_buf(const uint8_t *buf, size_t len){
    if(!buf || len < 0x40) return 0;
    #define R16(p) ((uint16_t)((p)[0] | ((uint16_t)(p)[1]<<8)))
    #define R32(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1]<<8) | ((uint32_t)(p)[2]<<16) | ((uint32_t)(p)[3]<<24))
    size_t limit = len < 65536 ? len : 65536;
    /* 快路径：MZ 在偏移 0 */
    if(buf[0]=='M' && buf[1]=='Z'){
        uint32_t pe = R32(buf+0x3C);
        if((size_t)pe+6 <= limit && memcmp(buf+pe,"PE\0\0",4)==0) return R16(buf+pe+4);
    }
    /* 慢路径：扫前置壳 */
    for(size_t o=0; o+0x40 < limit; o++){
        if(buf[o]!='M' || buf[o+1]!='Z') continue;
        uint32_t pe = R32(buf+o+0x3C);
        if((size_t)o+pe+6 > limit) continue;
        if(memcmp(buf+o+pe,"PE\0\0",4)!=0) continue;
        uint16_t m = R16(buf+o+pe+4);
        if(m==0x14c||m==0x8664||m==0xAA64||m==0x1C4||m==0x200) return m;
    }
    #undef R16
    #undef R32
    return 0;
}
/* ★ REQ-54：PE machine → 架构名。读不出返回 NULL（不出结论，不猜）。 */
static const char *pe_arch_name(uint16_t mach){
    switch(mach){
        case 0x014c: return "x86";
        case 0x8664: return "x64";
        case 0xAA64: return "arm64";
        case 0x01c4: return "arm32";
        default:     return NULL;
    }
}
/* ★ REQ-54：读 FAT32 上某文件的 PE machine（只读头 4KB，不加载整文件）。 */
static const char *fat32_file_arch(img_qcow2 *q, uint64_t lba, const char *path){
    fat_ent fe = fat32_find(q, lba, 0, path);
    if(!fe.found || fe.is_dir || fe.size < 0x40) return NULL;
    size_t want = fe.size < 8192 ? (size_t)fe.size : 8192;
    uint8_t *buf = malloc(want);
    if(!buf) return NULL;
    size_t got = fat32_read_head(q, lba, fe.cluster, fe.size, buf, want);
    if(getenv("IMG_BC_DEBUG"))
        fprintf(stderr,"[dbg arch] path=%s found=%d clus=%u size=%u got=%zu first=%02x%02x\n",
                path, fe.found, fe.cluster, fe.size, got,
                got>0?buf[0]:0, got>1?buf[1]:0);
    const char *res = NULL;
    if(got >= 0x40) res = pe_arch_name(pe_machine_from_buf(buf, got));
    free(buf);
    return res;
}
/* ★ REQ-54：读 NTFS 上某文件的 PE machine（同源逻辑，只读头）。 */
static const char *ntfs_file_arch(img_qcow2 *q, uint64_t lba, uint64_t bytes, const char *path){
    img_ntfs_fs *fs = img_ntfs_mount_rw(q, lba, bytes);
    if(!fs) return NULL;
    void *data=NULL; size_t n=0;
    const char *res = NULL;
    if(img_ntfs_read_file(fs, path, &data, &n)==IMG_OK && n>=0x40){
        size_t use = n < 8192 ? n : 8192;
        res = pe_arch_name(pe_machine_from_buf((const uint8_t*)data, use));
        free(data);
    }
    img_ntfs_umount(fs);
    return res;
}

/* ---------- 工具：找到各分区 ---------- */
typedef struct { int esp, bootp, os, rec; } parts_t;static bool is_esp(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_ESP, 8)==0;
    return p->type == IMG_MBR_ESP;
}
static bool is_recovery(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_RECOVERY, 8)==0;
    /* ★ MBR 的 0x27 有两个用途：Windows 的系统保留(SR) 和 恢复分区。
     *   SR 一定是【活动】分区，恢复分区一定不是 —— 用这个区分。
     *   否则 BIOS 布局下 boot.check 会对着 SR 报四条 rec.* 警告
     *   （缺 winre.wim / 缺 ReAgent.xml / 缺 boot.sdi …），全是假警报。*/
    if(p->status & 0x80) return false;
    return p->type == IMG_MBR_RECOVERY;
}
/* NTFS 分区：GPT 用基本数据 GUID；MBR 用 0x07（或 0x27 恢复）。
 * 需要>4GiB 才当作系统分区候选，避免把 ESP 之类误判。 */
static bool is_ntfs_like(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_BASIC, 8)==0;
    return p->type == IMG_MBR_NTFS_EXFAT || p->type == IMG_MBR_RECOVERY;
}
static bool is_fat_like(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_ESP, 8)==0;
    return p->type==IMG_MBR_FAT32 || p->type==IMG_MBR_FAT32_LBA || p->type==IMG_MBR_ESP;
}
static parts_t locate_parts(img_qcow2 *q, const img_parttable_t *t){
    (void)q;
    parts_t p = {-1,-1,-1,-1};
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(!t->parts[i].used) continue;
        uint64_t sec = t->parts[i].last_lba - t->parts[i].first_lba + 1;
        if(p.esp<0 && is_esp(&t->parts[i])) p.esp=i;
        if(p.esp<0 && is_fat_like(&t->parts[i]) && sec>=64*2048 && sec<=4096*2048) p.esp=i;
        if(p.rec<0 && is_recovery(&t->parts[i])) p.rec=i;
        if(is_ntfs_like(&t->parts[i]) && !(is_recovery(&t->parts[i]))){
            if(p.os<0 || sec > (t->parts[p.os].last_lba - t->parts[p.os].first_lba))
                p.os = i;
        }
    }
    /* BIOS 常见布局：活动分区就是引导分区 */
    for(int i=0;i<4;i++) if(t->parts[i].used && t->parts[i].status==0x80 && p.os<0) p.os=i;
    if(p.os<0) for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && is_ntfs_like(&t->parts[i])){ p.os=i; break; }
    return p;
}

/* ★ 不能只按"最大的 NTFS 分区"认定系统分区 —— 真实镜像常有多个同容量数据分区。
 *   改为：逐个 NTFS 分区试找 \Windows\System32\winload，命中才算系统分区。 */
static int find_os_partition(img_qcow2 *q, const img_parttable_t *t, int guess){
    if(guess>=0){
        uint64_t lba=t->parts[guess].first_lba;
        uint64_t sz=(t->parts[guess].last_lba-t->parts[guess].first_lba+1)*512;
        if(ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.exe") ||
           ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.efi") ||
           ntfs_path_exists(q,lba,sz,"/Windows/System32/boot/winload.efi"))
            return guess;
    }
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(i==guess || !t->parts[i].used || !is_ntfs_like(&t->parts[i])) continue;
        uint64_t lba=t->parts[i].first_lba;
        uint64_t sz=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512;
        if(ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.exe") ||
           ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.efi"))
            return i;
    }
    return guess;   /* 都未找到就保持原判断，让检查项自己报红 */
}

/* ---------- 主检查 ---------- */
static img_check_t *add(img_bootcheck_result_t *r, const char *id, const char *name,
                        img_ck_level_t lvl, bool pass, const char *fmt, ...){
    if(r->count>=64) return &r->items[63];
    img_check_t *c = &r->items[r->count++];
    c->id=id; c->name=name; c->level=lvl; c->pass=pass;
    va_list ap; va_start(ap,fmt);
    /* ★ fmt 先过 img_t()：中文格式串即 key，英文环境换成英文模板再填数据 */
    vsnprintf(c->detail,sizeof c->detail,img_t(fmt),ap);
    va_end(ap);
    if(lvl==IMG_CK_REQUIRED && !pass) r->n_required_fail++;
    if(lvl==IMG_CK_WARN && !pass) r->n_warn_fail++;
    return c;
}

int img_bootcheck(img_qcow2 *q, img_boot_mode_t mode, img_bootcheck_result_t *out, img_prog *prog){
    return img_bootcheck_ex(q, mode, 0, out, prog);
}
int img_bootcheck_ex(img_qcow2 *q, img_boot_mode_t mode, unsigned flags,
                     img_bootcheck_result_t *out, img_prog *prog){
    if(!q||!out) return IMG_EINVAL;
    memset(out,0,sizeof *out);

    img_parttable_t t;
    int rc = img_parttable_read(q,&t,0);
    if(rc){ add(out,"gpt.read","读分区表",IMG_CK_REQUIRED,false,"img_parttable_read: %s",img_strerror(rc)); return rc; }

    bool has_gpt = (t.table == IMG_TABLE_GPT);
    bool has_mbr = (t.table == IMG_TABLE_MBR) || has_gpt; /* GPT 也有保护 MBR */
    out->is_uefi = (mode==IMG_BOOT_UEFI) || (mode==IMG_BOOT_AUTO && has_gpt);
    const char *modename = out->is_uefi ? "UEFI/GPT" : "BIOS/MBR";

    add(out,"mode","引导模式",IMG_CK_INFO,true,"%s（%s，%d 个分区）",
        modename, has_gpt?"GPT":(has_mbr?"MBR":"?"), t.count);

    parts_t P = locate_parts(q,&t);
    P.os = find_os_partition(q,&t,P.os);   /* 用 \Windows 存在性确认，而非只看容量 */
    int actp = -1;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].status==0x80){ actp=i; break; }
    /* ★ $Boot 引导代码的检查对象要分模式：
     *   UEFI 用系统分区（其实 ESP 不是 NTFS，此项仅作参考）
     *   BIOS 必须查【活动分区】—— bootsect.exe 把引导代码写在那里 */

    /* ── ① 分区表层 ── */
    if(out->is_uefi){
        add(out,"gpt.present","GPT 结构有效",IMG_CK_REQUIRED,has_gpt,
            "table=%s backup_lba=%llu", has_gpt?"GPT":"非GPT",(unsigned long long)t.backup_lba);
        bool esp_ok = P.esp>=0 && is_fat_like(&t.parts[P.esp]);
        add(out,"gpt.esp","存在 ESP (FAT32)",IMG_CK_REQUIRED,esp_ok,
            esp_ok ? "LBA %llu..%llu" : "未找到 FAT32 小分区",
            esp_ok?(unsigned long long)t.parts[P.esp].first_lba:0ULL,
            esp_ok?(unsigned long long)t.parts[P.esp].last_lba:0ULL);
        bool efi_type = P.esp>=0 && is_esp(&t.parts[P.esp]);
        add(out,"gpt.esp_type","ESP 类型 GUID c12a7328",IMG_CK_REQUIRED,efi_type,
            efi_type?"正确":"类型不是 EFI System Partition");
    } else {
        add(out,"mbr.present","MBR 结构有效",IMG_CK_REQUIRED,has_mbr,
            "sig55aa=%s",has_mbr?"OK":"BAD");
        bool act=false; uint64_t actlba=0;
        for(int i=0;i<4;i++) if(t.parts[i].used && t.parts[i].status==0x80){ act=true; actlba=t.parts[i].first_lba; }
        add(out,"mbr.active","有活动分区标记",IMG_CK_REQUIRED,act,
            act? "活动分区 LBA %llu":"没有任何活动分区 — BIOS 无法选择引导盘",
            act?(unsigned long long)actlba:0ULL);
        bool has07=false;
        for(int i=0;i<4;i++) if(t.parts[i].used && is_ntfs_like(&t.parts[i])) has07=true;
        add(out,"mbr.ntfs","存在 NTFS 分区(类型0x07)",IMG_CK_REQUIRED,has07,
            has07?"有":"引导分区不是 0x07");
    {   /* ★ mbr.chs —— Windows 的硬规则：起始 CHS 扇区号 1-based，0 是非法值
         * （2026-10-16 用户 PE 实测：全零 CHS → Windows 判分区表损坏 →
         *  "磁盘管理：没有初始化/未分配"、"搜索分区只认出两个"；sfdisk 却认 —— 两套标准）
         * 合法形态：真实 CHS（与 LBA 对应）或哨兵 fe ff ff（越界，段=63≥1）*/
        uint8_t mb[512]; int chsbad=0;
        if(img_qcow2_read_full(q,0,mb,512)==IMG_OK){
            for(int e=0;e<4;e++){
                const uint8_t *en=mb+446+e*16;
                if(en[4]==0) continue;                        /* 空项跳过 */
                if((en[2] & 0x3F)==0) chsbad++;               /* ★ 扇区号=0 → 非法（Windows 拒）*/
            }
            add(out,"mbr.chs","MBR 分区项 CHS 合法性(Windows:扇区号1-based)",IMG_CK_REQUIRED, chsbad==0,
                chsbad? "起始CHS扇区号=0 — Windows 判分区表损坏(磁盘管理显示未初始化/只认部分分区)" : "全部合法(真实CHS或fe ff ff哨兵)");
        }
    }
    }

    /* ── ② NTFS 层 ── */
    int ntp = -1;
    if(out->is_uefi) ntp = P.os;
    else ntp = P.os>=0?P.os:P.bootp;
    int bcp = out->is_uefi ? ntp : (actp>=0 ? actp : ntp);   /* 引导代码所在分区 */
    if(bcp>=0){
        /* ---- $Boot 扇区与引导代码：查 bcp ---- */
        uint64_t lbaB=t.parts[bcp].first_lba;
        img_ntfs_info_t niB;
        int nrcB = img_ntfs_probe(q,lbaB,&niB);
        add(out,"ntfs.boot","NTFS $Boot 扇区",IMG_CK_REQUIRED,nrcB==IMG_OK,
            nrcB==IMG_OK? "OEM=NTFS spc=%u 总扇区=%llu (LBA %llu)":"无法读取或签名不对 (%s)",
            nrcB==IMG_OK?(unsigned)niB.sectors_per_cluster:0,
            nrcB==IMG_OK?(unsigned long long)niB.total_sectors:0ULL,
            (unsigned long long)lbaB, nrcB!=IMG_OK?img_strerror(nrcB):"");
        uint8_t b10[10*512];
        if(img_qcow2_read_full(q,lbaB*512,b10,sizeof b10)==IMG_OK){
            bool allnz=false;
            for(size_t i=512;i<sizeof b10 && !allnz;i++) if(b10[i]){ allnz=true; break; }
            /* 升级为硬指标：BIOS 必须有引导代码（mkntfs 不写，得跑 img_fixboot）
             * 依据：实测 mkntfs 格式化后扇区1..9 差 3825 字节全零；
             *       真实 Windows 安装与 Xlite 的该段【逐字节相同】= 官方机器码 */
            add(out,"ntfs.code","NTFS $Boot 扇区 0..9 有引导器代码",
                out->is_uefi?IMG_CK_INFO:IMG_CK_REQUIRED, allnz,
                out->is_uefi
                  ? (allnz?"检测到引导代码":"扇区1-9 全零（UEFI 不依赖此处，仅参考）")
                  : (allnz?"检测到引导代码"
                          :"扇区1-9 全零 — mkntfs 不写引导代码，必须跑 imgfixboot 补上"));
        }
    }
    if(ntp>=0){   /* ---- 系统文件：查 ntp ---- */
        uint64_t lba=t.parts[ntp].first_lba;
        uint64_t sz=(t.parts[ntp].last_lba-t.parts[ntp].first_lba+1)*512;
        add(out,"ntfs.winload","系统文件 \\Windows\\system32\\winload",
            IMG_CK_REQUIRED,
            ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.exe"),
            "winload.exe 是否存在（.efi 分支未在此查）");
        {
            bool has = ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.efi");
            const char *wa = has ? ntfs_file_arch(q,lba,sz,"/Windows/System32/winload.efi") : NULL;
            img_check_t *ck = add(out,"ntfs.winload_efi","系统文件 winload.efi",
                IMG_CK_INFO, has, "UEFI 分支需要（不存在则只支持 BIOS）");
            ck->arch = wa ? strdup(wa) : NULL;   /* ★ REQ-54 */
        }
    }

    /* ── ③ ESP/FAT 层 ── */
    if(P.esp>=0 && out->is_uefi){
        uint64_t lba=t.parts[P.esp].first_lba;
        uint64_t sz=(t.parts[P.esp].last_lba-t.parts[P.esp].first_lba+1)*512;
        img_fat32_layout_t FL;
        add(out,"esp.fat32","ESP 可被 FAT32 解析",IMG_CK_REQUIRED,
            img_fat32_probe(q,lba,&FL)==IMG_OK,
            "spc=%u 簇数=%u",(unsigned)FL.sectors_per_cluster,(unsigned)FL.cluster_count);
        fat_ent e1 = fat32_find(q,lba,sz,"EFI/Microsoft/Boot/bootmgfw.efi");
        {
            const char *ba = e1.found ? fat32_file_arch(q,lba,"EFI/Microsoft/Boot/bootmgfw.efi") : NULL;
            img_check_t *ck = add(out,"esp.bootmgfw","\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
                IMG_CK_REQUIRED, e1.found,
                e1.found? "存在，%u 字节": "不存在 — UEFI 未找到 Windows 启动管理器",
                e1.size);
            ck->arch = ba ? strdup(ba) : NULL;    /* ★ REQ-54 */
            ck->size = e1.found ? e1.size : 0;
        }
        /* ★ 2026-11-05：兜底启动器按系统位数自适应（以前写死 bootx64.efi，
         *   x86 盘实际写的是 bootia32.efi，检查项误报"不存在"）。
         *   位数从 esp.bootmgfw 同源的 \Windows\Boot\EFI\bootmgfw.efi 判不出，
         *   直接看正式路径文件大小不可靠 —— 用 PE machine 字段最硬，但这里
         *   用更省的办法：两个名字都查，存在哪个就是哪个（不会都存在：
         *   boot.populate 按位数只写一个）。 */
        fat_ent e2  = fat32_find(q,lba,sz,"EFI/Boot/bootx64.efi");
        fat_ent e2a = fat32_find(q,lba,sz,"EFI/Boot/bootia32.efi");
        fat_ent e2r = e2.found ? e2 : e2a;
        {
            /* ★ REQ-54：hint 里不再猜 x64/x86（曾误导），位数结论由 arch 字段给。
             *   两个名字都查；命中的那个读 PE machine 得真实位数。 */
            const char *fbpath = e2.found ? "EFI/Boot/bootx64.efi" : "EFI/Boot/bootia32.efi";
            const char *fa = e2r.found ? fat32_file_arch(q,lba,fbpath) : NULL;
            img_check_t *ck = add(out,"esp.fallback",
                e2.found? "\\EFI\\Boot\\bootx64.efi": "\\EFI\\Boot\\bootia32.efi",
                IMG_CK_WARN,e2r.found,
                e2r.found? "存在（通用回退入口，%u 字节）"
                         : "不存在（bootx64.efi / bootia32.efi 都没有）— 只能从固件菜单选 Windows Boot Manager",
                e2r.found? e2r.size : 0);
            ck->arch = fa ? strdup(fa) : NULL;    /* ★ REQ-54：真实 PE 位数 */
            ck->size = e2r.found ? e2r.size : 0;
        }
        fat_ent e3 = fat32_find(q,lba,sz,"EFI/Microsoft/Boot/BCD");
        add(out,"esp.bcd","\\EFI\\Microsoft\\Boot\\BCD",IMG_CK_REQUIRED,e3.found,
            e3.found ? "存在，%u 字节" : "ESP 上没有 BCD", e3.size);
    }

    /* ── ④ BIOS 特有：bootmgr 与 \Boot\BCD ──
     * ★ 不能用系统分区（装 \Windows 的那个）——真实镜像常把 BIOS 引导文件
     *   放在【活动分区】(status=0x80) 上，那是另一个小 NTFS 分区（如 Xlite 的 50MB）。 */
    if(!out->is_uefi){
        int actp = -1;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t.parts[i].used && t.parts[i].status==0x80){ actp=i; break; }
        /* 活动分区未找到引导文件就退到系统分区再试一次 */
        int bp = actp>=0 ? actp : ntp;
        uint64_t lba = bp>=0 ? t.parts[bp].first_lba : 0;
        uint64_t sz  = bp>=0 ? (t.parts[bp].last_lba-t.parts[bp].first_lba+1)*512 : 0;
        bool have_bm = bp>=0 && ntfs_path_exists(q,lba,sz,"/bootmgr");
        if(!have_bm && actp>=0 && ntp>=0 && ntp!=actp){
            lba = t.parts[ntp].first_lba;
            sz  = (t.parts[ntp].last_lba-t.parts[ntp].first_lba+1)*512;
            have_bm = ntfs_path_exists(q,lba,sz,"/bootmgr");
        }
        add(out,"bios.bootmgr","bootmgr 在活动分区",IMG_CK_REQUIRED, have_bm,
            have_bm ? "在 LBA %llu 找到" : "活动分区与系统分区都没有 bootmgr",
            (unsigned long long)lba);
        /* ★ bugfix（2026-11-04）：上面 411~414 行为"找 bootmgr"把 lba 切到了系统分区，
         *   而 BCD 必须查【引导分区】（活动分区 bp）—— 否则活动分区明明有 BCD 却报
         *   "BIOS 引导分区上没有 /Boot/BCD"（实测 15 个真模板全中招，内置模板也中招）。*/
        uint64_t bcd_lba = bp>=0 ? t.parts[bp].first_lba : lba;
        uint64_t bcd_sz  = bp>=0 ? (t.parts[bp].last_lba-t.parts[bp].first_lba+1)*512 : sz;
        bool have_bcd = bp>=0 && ntfs_path_exists(q,bcd_lba,bcd_sz,"/Boot/BCD");
        add(out,"bios.bootmgr_bcd","\\Boot\\BCD",IMG_CK_REQUIRED, have_bcd,
            have_bcd ? "存在" : "BIOS 引导分区上没有 /Boot/BCD");
        (void)ntp;
    }
    /* ── ⑤ 恢复分区：★ 2026-11-04 起彻底移走 ──
     * 恢复分区属性（0x8000000000000001）与 Winre.wim 的检测/修复全部搬到
     * WinRE 独立模块（命令 recovery.status，src/img_winre.c）。
     * 引导链（MBR→VBR→bootmgr→BCD→winload）与恢复环境本来就是两回事，
     * 以前混在这里，App 的"引导修复"卡片总被恢复分区的警告搅和。 */
    (void)flags;

    if(prog) img_prog_done(prog,"检查完成");
    return IMG_OK;
}

/* ---------- 输出 ---------- */
/* suggest 里的 <img> 占位符 → 真实镜像路径（REQ-38：App 拿来直接跑）*/
static void bc_fill_img(char *dst, size_t cap, const char *src, const char *img){
    if(!dst||!cap) return;
    if(!src) { dst[0]=0; return; }
    if(!img || !*img){ snprintf(dst,cap,"%s",src); return; }
    const char *p=strstr(src,"<img>");
    if(!p){ snprintf(dst,cap,"%s",src); return; }
    snprintf(dst,cap,"%.*s%s%s",(int)(p-src),src,img,p+5);
}

int img_bootcheck_json(const img_bootcheck_result_t *r, char *buf, int cap){
    if(!r||!buf||cap<=0) return 0;
    /* JSON 字符串里反斜杠与引号必须转义 —— 本模块的 name 里就有
     *   \EFI\Microsoft\Boot\bootmgfw.efi
     * 不转义会导致整行 JSON 非法、下游解析直接丢弃该检查项。 */
    int n=0;
    for(int i=0;i<r->count;i++){
        const img_check_t *c=&r->items[i];
        char line[1024];
        const char *label = img_t(c->name);   /* ★ 中英双份：中文原文即 key */
        const char *lvl = c->level==IMG_CK_REQUIRED?"required":
                          c->level==IMG_CK_WARN?"warn":"info";
        /* ⚠ c->id / c->name 是【指针】，sizeof 只有 8；必须用固定大缓冲。
         *   用 sizeof(指针) 会截断 UTF-8 中文，产出非法 JSON。 */
        char id[128], nm[256], det[sizeof c->detail*2], sg[sizeof c->suggest*2];
        #define ESC(dst,src) do{ int j=0; for(const char *p=(src); *p && j<(int)sizeof(dst)-2; p++){ \
            if(*p=='"'||*p=='\\'){ (dst)[j++]='\\'; } (dst)[j++]=*p; } (dst)[j]=0; }while(0)
        ESC(id, c->id); ESC(nm, label); ESC(det, c->detail);
        if(c->suggest[0]){ char raw[sizeof c->suggest*2]; bc_fill_img(raw,sizeof raw,c->suggest,r->img); ESC(sg, raw); }
        #undef ESC
        int m=snprintf(line,sizeof line,
            "{\"id\":\"%s\",\"name\":\"%s\",\"level\":\"%s\",\"pass\":%s,\"detail\":\"%s\"%s%s%s}\n",
            id, nm, lvl, c->pass?"true":"false", det,
            c->suggest[0]?",\"suggest\":\"":"",
            c->suggest[0]?sg:"",
            c->suggest[0]?"\"":"");
        if(m<0) break;
        if(n+m >= cap-1) break;
        memcpy(buf+n,line,(size_t)m);
        n+=m;
    }
    if(n<cap) buf[n]=0;
    return n;
}

int img_bootcheck_json_array(const img_bootcheck_result_t *r, char *buf, int cap){
    if(!r||!buf||cap<16) return 0;
    int n=0;
    buf[n++]='[';
    for(int i=0;i<r->count;i++){
        const img_check_t *c=&r->items[i];
        char line[1024];
        const char *label = img_t(c->name);   /* ★ 中英双份：中文原文即 key */
        const char *lvl = c->level==IMG_CK_REQUIRED?"required":
                          c->level==IMG_CK_WARN?"warn":"info";
        char id[128], nm[256], det[sizeof c->detail*2], sg[sizeof c->suggest*2];
        #define ESC2(dst,src) do{ int j=0; for(const char *p=(src); *p && j<(int)sizeof(dst)-2; p++){ \
            if(*p=='"'||*p=='\\'){ (dst)[j++]='\\'; } (dst)[j++]=*p; } (dst)[j]=0; }while(0)
        ESC2(id, c->id); ESC2(nm, label); ESC2(det, c->detail);
        /* ★ REQ-54：UEFI 分支输出 arch / size（x86|x64|arm64，字节数） */
        char arch_buf[24] = "", size_buf[40] = "";
        if(c->arch && c->arch[0]) snprintf(arch_buf,sizeof arch_buf,",\"arch\":\"%s\"", c->arch);
        if(c->size > 0) snprintf(size_buf,sizeof size_buf,",\"size\":%llu", (unsigned long long)c->size);
        if(c->suggest[0]){ char raw[sizeof c->suggest*2]; bc_fill_img(raw,sizeof raw,c->suggest,r->img); ESC2(sg, raw); }
        #undef ESC2
        int m=snprintf(line,sizeof line,
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"level\":\"%s\",\"pass\":%s,\"detail\":\"%s\"%s%s%s%s%s}",
            i?",":"", id, nm, lvl, c->pass?"true":"false", det,
            c->suggest[0]?",\"suggest\":\"":"",
            c->suggest[0]?sg:"",
            c->suggest[0]?"\"":"",
            arch_buf, size_buf);
        if(m<0) break;
        if(n+m >= cap-2) break;
        memcpy(buf+n,line,(size_t)m); n+=m;
    }
    if(n<cap-1) buf[n++]=']';
    buf[n]=0;
    return n;
}

int img_bootcheck_summary(const img_bootcheck_result_t *r, char *buf, int cap){
    if(!r||!buf||cap<=0) return 0;
    int req_ok=0, req_n=0, warn_ok=0, warn_n=0;
    for(int i=0;i<r->count;i++){
        switch(r->items[i].level){
        case IMG_CK_REQUIRED: req_n++; if(r->items[i].pass) req_ok++; break;
        case IMG_CK_WARN:     warn_n++; if(r->items[i].pass) warn_ok++; break;
        default: break;   /* 信息项不进摘要计数，总数用 r->count */
        }
    }
    if(img_lang_en())
        return snprintf(buf,(size_t)cap,"%s · %s · required %d/%d · warnings %d/%d · %d checks",
            r->is_uefi?"UEFI/GPT":"BIOS/MBR",
            (req_n && r->n_required_fail==0) ? "boot chain complete" : "required checks failed - may fail to start",
            req_ok, req_n, warn_ok, warn_n, r->count);
    return snprintf(buf,(size_t)cap,"%s · %s · 必需 %d/%d 通过 · 警告 %d/%d · 共 %d 项",
        r->is_uefi?"UEFI/GPT":"BIOS/MBR",
        req_n && r->n_required_fail==0 ? "引导链完整" : "★ 有必需项未通过 — 很可能无法启动",
        req_ok, req_n, warn_ok, warn_n, r->count);
}
