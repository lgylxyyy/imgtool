#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_bootcheck.c — 引导链自检 */
#include "img_bootcheck.h"
#include "img_fat32.h"
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>        /* 必须在 ntfs 头之前（ntfstime.h 会重定义 timespec）*/
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/dir.h>

/* ---------- ntfs_device_operations 桥接（复用同一套回调） ---------- */
static s64  b_seek (struct ntfs_device *d, s64 o, int w){ (void)d; return img_ntfs_cb_seek((void*)img_ntfs_get_active_ctx(),(long long)o,w); }
static s64  b_read (struct ntfs_device *d, void *p, s64 c){ (void)d; return img_ntfs_cb_read((void*)img_ntfs_get_active_ctx(),p,(long long)c); }
static s64  b_pread(struct ntfs_device *d, void *p, s64 c, s64 o){ (void)d; return img_ntfs_cb_pread((void*)img_ntfs_get_active_ctx(),p,(long long)c,(long long)o); }
static s64  b_write(struct ntfs_device *d, const void *p, s64 c){ (void)d; return img_ntfs_cb_write((void*)img_ntfs_get_active_ctx(),p,(long long)c); }
static s64  b_pwrite(struct ntfs_device *d, const void *p, s64 c, s64 o){ (void)d; return img_ntfs_cb_pwrite((void*)img_ntfs_get_active_ctx(),p,(long long)c,(long long)o); }
static int  b_open (struct ntfs_device *d, int f){ (void)d;(void)f; return img_ntfs_cb_open((void*)img_ntfs_get_active_ctx(),0); }
static int  b_close(struct ntfs_device *d){ (void)d; return img_ntfs_cb_close((void*)img_ntfs_get_active_ctx()); }
static int  b_sync (struct ntfs_device *d){ (void)d; return img_ntfs_cb_sync((void*)img_ntfs_get_active_ctx()); }
static int  b_stat (struct ntfs_device *d, struct stat *s){ (void)d; return img_ntfs_cb_stat((void*)img_ntfs_get_active_ctx(),s); }
static int  b_ioctl(struct ntfs_device *d, unsigned long r, void *a){ (void)d; return img_ntfs_cb_ioctl((void*)img_ntfs_get_active_ctx(),r,a); }

static struct ntfs_device_operations ROPS = { b_open,b_close,b_seek,b_read,b_write,
                                               b_pread,b_pwrite,b_sync,b_stat,b_ioctl };

/* ---------- 用 libntfs-3g 查 NTFS 里某个路径是否存在 ---------- */
static bool ntfs_path_exists(img_qcow2 *q, uint64_t lba, uint64_t bytes, const char *path){
    if(!q || img_ntfs_probe(q,lba,NULL)!=IMG_OK) return false;
    img_ntfs_ctx_activate(q,lba,bytes);
    int dfd = open("imgtool-dev", O_RDWR|O_CREAT|O_TRUNC, 0666);
    if(dfd>=0) close(dfd);
    struct ntfs_device *dev = ntfs_device_alloc("imgtool-dev",0,&ROPS,NULL);
    if(!dev){ img_ntfs_ctx_deactivate(); return false; }
    ntfs_volume *vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY|NTFS_MNT_FORENSIC);
    if(!vol){
        /* 再试不带 forensics（旧选项可能不存在）*/
        vol = ntfs_device_mount(dev, NTFS_MNT_RDONLY);
    }
    if(!vol){ ntfs_device_free(dev); img_ntfs_ctx_deactivate(); return false; }

    bool found = false;
    char work[512];
    snprintf(work,sizeof work,"%s",path);
    /* 逐级解析 */
    ntfs_inode *cur = ntfs_inode_open(vol, FILE_root);
    if(cur){
        char *save=NULL;
        for(char *tok=strtok_r(work,"/\\",&save); tok; tok=strtok_r(NULL,"/\\",&save)){
            ntfschar *uname = NULL;
            size_t ulen = 0;
            ulen = strlen(tok);
            if(ntfs_mbstoucs((const char*)tok, &uname) != 0){ ntfs_inode_close(cur); cur=NULL; break; }
            MFT_REF ref = ntfs_inode_lookup_by_name(cur, uname, ulen);
            free(uname);
            if(!ref){ ntfs_inode_close(cur); cur=NULL; break; }
            ntfs_inode *nxt = ntfs_inode_open(vol, ref);
            ntfs_inode_close(cur);
            cur = nxt;
            if(!cur) break;
        }
        if(cur){ found = true; ntfs_inode_close(cur); }
    }
    ntfs_umount(vol,false);
    ntfs_device_free(dev);
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
static bool fat32_name_match(const uint8_t *ent, const char *name){
    char n[64]; int j=0;
    for(int i=0;i<11;i++) n[j++]= (char)ent[i];
    while(j>0 && n[j-1]==' ') j--;
    n[j]=0;
    /* 去扩展名空格 */
    char comp[64]; snprintf(comp,sizeof comp,"%s",name);
    char *dot=strrchr(comp,'.'); int lfnlen = dot? (int)(dot-comp):(int)strlen(comp);
    for(int i=0;i<lfnlen && i<8;i++){
        char c=comp[i];
        if(c>='a'&&c<='z') c-=32;
        if(n[i]!=c) return false;
    }
    if(dot){
        for(int i=0;i<3;i++){
            char c=dot[1+i];
            if(!c) c=' ';
            if(c>='a'&&c<='z') c-=32;
            if(n[8+i]!=c) return false;
        }
    }
    return true;
}
static fat_ent fat32_in_dir(img_qcow2 *q, uint64_t base, const img_fat32_layout_t *L,
                            uint32_t dirclus, const char *want){
    fat_ent r={0};
    size_t csz = (size_t)L->sectors_per_cluster * 512;
    uint8_t *buf = malloc(csz);
    if(!buf) return r;
    uint32_t c = dirclus;
    while(c>=2 && c<0x0FFFFFF8){
        fat32_read_cluster(q, base, L, c, buf);
        for(size_t i=0;i+32<=csz;i+=32){
            uint8_t *e = buf+i;
            if(e[0]==0x00){ c=0xFFFFFFFF; break; }        /* 目录结束 */
            if(e[0]==0xE5) continue;
            if(e[11]==0x0F) continue;                       /* LFN 项，用 8.3 匹配即可 */
            if(e[11] & 0x08) continue;                      /* 卷标 */
            if(fat32_name_match(e, want)){
                r.found = true;
                r.is_dir = (e[11] & 0x10)!=0;
                r.cluster = (uint32_t)e[26]|((uint32_t)e[27]<<8)|((uint32_t)e[28]<<16)|((uint32_t)e[29]<<24);
                r.size = (uint32_t)e[28]|((uint32_t)e[29]<<8)|((uint32_t)e[30]<<16)|((uint32_t)e[31]<<24);
                r.size = (uint32_t)e[28] | ((uint32_t)e[29]<<8) | ((uint32_t)e[30]<<16) | ((uint32_t)e[31]<<24);
                r.size = (uint32_t)(e[28]) | ((uint32_t)e[29]<<8) | ((uint32_t)e[30]<<16) | ((uint32_t)e[31]<<24);
                r.size = (uint32_t)e[28]|((uint32_t)e[29]<<8)|((uint32_t)e[30]<<16)|((uint32_t)e[31]<<24);
                free(buf);
                return r;
            }
        }
        if(c==0xFFFFFFFF) break;
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

/* ---------- 工具：找到各分区 ---------- */
typedef struct { int esp, bootp, os, rec; } parts_t;
static bool is_esp(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_ESP, 8)==0;
    return p->type == IMG_MBR_ESP;
}
static bool is_recovery(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_RECOVERY, 8)==0;
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

/* ---------- 主检查 ---------- */
static img_check_t *add(img_bootcheck_result_t *r, const char *id, const char *name,
                        img_ck_level_t lvl, bool pass, const char *fmt, ...){
    if(r->count>=64) return &r->items[63];
    img_check_t *c = &r->items[r->count++];
    c->id=id; c->name=name; c->level=lvl; c->pass=pass;
    va_list ap; va_start(ap,fmt);
    vsnprintf(c->detail,sizeof c->detail,fmt,ap);
    va_end(ap);
    if(lvl==IMG_CK_REQUIRED && !pass) r->n_required_fail++;
    if(lvl==IMG_CK_WARN && !pass) r->n_warn_fail++;
    return c;
}

int img_bootcheck(img_qcow2 *q, img_boot_mode_t mode, img_bootcheck_result_t *out, img_prog *prog){
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
    }

    /* ── ② NTFS 层 ── */
    int ntp = -1;
    if(out->is_uefi) ntp = P.os;
    else ntp = P.os>=0?P.os:P.bootp;
    if(ntp>=0){
        uint64_t lba=t.parts[ntp].first_lba;
        img_ntfs_info_t ni;
        int nrc = img_ntfs_probe(q,lba,&ni);
        add(out,"ntfs.boot","NTFS $Boot 扇区",IMG_CK_REQUIRED,nrc==IMG_OK,
            nrc==IMG_OK? "OEM=NTFS spc=%u 总扇区=%llu":"读不到或签名不对 (%s)",
            nrc==IMG_OK?(unsigned)ni.sectors_per_cluster:0,
            nrc==IMG_OK?(unsigned long long)ni.total_sectors:0ULL,
            nrc!=IMG_OK?img_strerror(nrc):"");
        uint8_t b10[10*512];
        if(img_qcow2_read_full(q,lba*512,b10,sizeof b10)==IMG_OK){
            bool allnz = false;
            /* 扇区1..9 应有引导器代码（至少非全零）*/
            for(size_t i=512;i<sizeof b10 && !allnz;i++) if(b10[i]){ allnz=true; break; }
            add(out,"ntfs.code","NTFS $Boot 扇区 0..9 有引导器代码",IMG_CK_WARN,allnz,
                allnz?"检测到引导代码":"扇区1-9 全零 — mkntfs 只写0，Windows 就是靠这里启动");
        }
        uint64_t sz=(t.parts[ntp].last_lba-t.parts[ntp].first_lba+1)*512;
        add(out,"ntfs.winload","系统文件 \\Windows\\system32\\winload",
            IMG_CK_REQUIRED,
            ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.exe"),
            "winload.exe 是否存在（.efi 分支未在此查）");
        add(out,"ntfs.winload_efi","系统文件 winload.efi",
            IMG_CK_INFO,
            ntfs_path_exists(q,lba,sz,"/Windows/System32/winload.efi"),
            "UEFI 分支需要（不存在则只支持 BIOS）");
    }

    /* ── ③ ESP/FAT 层 ── */
    if(P.esp>=0 && out->is_uefi){
        uint64_t lba=t.parts[P.esp].first_lba;
        uint64_t sz=(t.parts[P.esp].last_lba-t.parts[P.esp].first_lba+1)*512;
        img_fat32_layout_t FL;
        add(out,"esp.fat32","ESP 可被 FAT32 解析",IMG_CK_REQUIRED,
            img_fat32_check(q,lba,&FL)==IMG_OK,
            "spc=%u 簇数=%u",(unsigned)FL.sectors_per_cluster,(unsigned)FL.cluster_count);
        fat_ent e1 = fat32_find(q,lba,sz,"EFI/Microsoft/Boot/bootmgfw.efi");
        add(out,"esp.bootmgfw","\\EFI\\Microsoft\\Boot\\bootmgfw.efi",
            IMG_CK_REQUIRED, e1.found,
            e1.found? "存在，%u 字节": "不存在 — UEFI 找不到 Windows 启动管理器",
            e1.size);
        fat_ent e2 = fat32_find(q,lba,sz,"EFI/Boot/bootx64.efi");
        add(out,"esp.fallback","\\EFI\\Boot\\bootx64.efi",IMG_CK_WARN,e2.found,
            e2.found? "存在（通用回退入口）":"不存在 — 只能从固件菜单选 Windows Boot Manager");
        fat_ent e3 = fat32_find(q,lba,sz,"EFI/Microsoft/Boot/BCD");
        add(out,"esp.bcd","\\EFI\\Microsoft\\Boot\\BCD",IMG_CK_REQUIRED,e3.found,
            e3.found ? "存在，%u 字节" : "ESP 上没有 BCD", e3.size);
    }

    /* ── ④ BIOS 特有：$Boot 前10扇区 bootmgr ── */
    if(!out->is_uefi && ntp>=0){
        uint64_t lba=t.parts[ntp].first_lba;
        uint64_t sz=(t.parts[ntp].last_lba-t.parts[ntp].first_lba+1)*512;
        add(out,"bios.bootmgr","bootmgr 在活动分区",IMG_CK_REQUIRED,
            ntfs_path_exists(q,lba,sz,"/bootmgr"),
            "bootmgr 是否存在");
        add(out,"bios.bootmgr_bcd","\\Boot\\BCD",IMG_CK_REQUIRED,
            ntfs_path_exists(q,lba,sz,"/Boot/BCD"),
            "BIOS 的 BCD 是否存在");
    }

    /* ── ⑤ 恢复分区 ── */
    if(P.rec>=0){
        uint64_t lba=t.parts[P.rec].first_lba;
        uint64_t sz=(t.parts[P.rec].last_lba-t.parts[P.rec].first_lba+1)*512;
        add(out,"rec.type","恢复分区类型 GUID de94bba4",IMG_CK_INFO,true,
            "LBA %llu",(unsigned long long)lba);
        bool has_wim = ntfs_path_exists(q,lba,sz,"/Recovery/WindowsRE/winre.wim");
        add(out,"rec.winre","winre.wim 存在",IMG_CK_WARN,has_wim,
            has_wim?"找到恢复映像":"恢复分区内没有 winre.wim");
    }

    if(prog) img_prog_done(prog,"检查完成");
    return IMG_OK;
}

/* ---------- 输出 ---------- */
int img_bootcheck_json(const img_bootcheck_result_t *r, char *buf, int cap){
    if(!r||!buf||cap<=0) return 0;
    int n=0;
    for(int i=0;i<r->count;i++){
        const img_check_t *c=&r->items[i];
        char line[512];
        const char *lvl = c->level==IMG_CK_REQUIRED?"required":
                          c->level==IMG_CK_WARN?"warn":"info";
        /* 转义详情里的引号与反斜杠 */
        char det[sizeof c->detail*2]; int dj=0;
        for(const char *p=c->detail; *p && dj<(int)sizeof det-2; p++){
            if(*p=='"'||*p=='\\') det[dj++]='\\';
            det[dj++]=*p;
        }
        det[dj]=0;
        int m=snprintf(line,sizeof line,
            "{\"id\":\"%s\",\"name\":\"%s\",\"level\":\"%s\",\"pass\":%s,\"detail\":\"%s\"}\n",
            c->id, c->name, lvl, c->pass?"true":"false", det);
        if(m<0) break;
        if(n+m >= cap-1) break;
        memcpy(buf+n,line,(size_t)m);
        n+=m;
    }
    if(n<cap) buf[n]=0;
    return n;
}

int img_bootcheck_summary(const img_bootcheck_result_t *r, char *buf, int cap){
    if(!r||!buf||cap<=0) return 0;
    int req_ok=0, req_n=0, warn_ok=0, warn_n=0, info_n=0;
    for(int i=0;i<r->count;i++){
        switch(r->items[i].level){
        case IMG_CK_REQUIRED: req_n++; if(r->items[i].pass) req_ok++; break;
        case IMG_CK_WARN:     warn_n++; if(r->items[i].pass) warn_ok++; break;
        default: info_n++; break;
        }
    }
    return snprintf(buf,(size_t)cap,"%s · %s · 必需 %d/%d 通过 · 警告 %d/%d · 共 %d 项",
        r->is_uefi?"UEFI/GPT":"BIOS/MBR",
        req_n && r->n_required_fail==0 ? "引导链完整" : "★ 有必需项未通过 — 很可能无法启动",
        req_ok, req_n, warn_ok, warn_n, r->count);
}
