#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_fat32_fs.c — FAT32 文件系统读写（全自研，见头文件说明） */
#include "img_fat32_fs.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define FAT32_EOC     0x0FFFFFF8u   /* >= 这个值就是链尾 */
#define FAT32_MASK    0x0FFFFFFFu

/* ---------- 链遍历保护 ---------- */
/* ★ 沿簇链走时必须有上界：FAT 被写坏成环（或对手给的镜像故意成环）时，
 *   只靠"走到 EOC 停"会【永久死循环】—— 实测表现为 ls/export 挂住不返回、
 *   CPU 跑满，只能杀进程。任何链条最多经过"簇总数"个簇，超过必为环。
 *   用法： uint64_t guard=0;
 *          while(c>=2 && c<FAT32_EOC){ if(FAT32_CHAIN_GUARD(fs,guard)) return IMG_ECORRUPT; ... } */
#define FAT32_CHAIN_GUARD(fs, cnt)  (++(cnt) > (uint64_t)(fs)->L.cluster_count)
#define ATTR_DIR      0x10
#define ATTR_LFN      0x0F
#define ATTR_VOLUME   0x08
#define ENTRY_SIZE    32

struct img_fat32_fs {
    img_qcow2 *q;
    uint64_t   base;              /* 分区字节起点 */
    img_fat32_layout_t L;
    uint64_t   fat_off;           /* FAT0 字节偏移 */
    uint64_t   data_off;          /* 数据区字节偏移 */
    uint32_t   alloc_hint;        /* 下次分配从这里往后找（加速）*/
    bool       writable;
    uint64_t   free_bytes;        /* 剩余空间缓存（见 img_fat32_free_bytes）*/
    bool       free_valid;
};

/* ---------- 底层读写 ---------- */
static int rd(img_fat32_fs *fs, uint64_t off, void *b, size_t n){
    return img_qcow2_read_full(fs->q, fs->base+off, b, n);
}
static int wr(img_fat32_fs *fs, uint64_t off, const void *b, size_t n){
    if(!fs->writable) return IMG_ENOTSUP;
    return img_qcow2_write(fs->q, fs->base+off, b, n);
}
static uint64_t clus_off(img_fat32_fs *fs, uint32_t c){
    return fs->data_off + (uint64_t)(c-2) * fs->L.sectors_per_cluster * 512;
}

/* ---------- FAT 表 ---------- */
static int fat_get(img_fat32_fs *fs, uint32_t c, uint32_t *out){
    uint8_t e[4];
    int rc = rd(fs, fs->fat_off + (uint64_t)c*4, e, 4);
    if(rc) return rc;
    *out = ((uint32_t)e[0]|((uint32_t)e[1]<<8)|((uint32_t)e[2]<<16)|((uint32_t)e[3]<<24)) & FAT32_MASK;
    return IMG_OK;
}
static int fat_set(img_fat32_fs *fs, uint32_t c, uint32_t v){
    fs->free_valid = false;            /* FAT 变了 → 剩余空间缓存失效 */
    v &= FAT32_MASK;
    uint8_t e[4];
    /* 保留高 4 位（通常是 0xF）*/
    uint32_t old=0;
    int rc = fat_get(fs, c, &old);
    if(rc) return rc;
    uint32_t merged = (old & 0xF0000000u) | (v & FAT32_MASK);
    e[0]=(uint8_t)(merged); e[1]=(uint8_t)(merged>>8);
    e[2]=(uint8_t)(merged>>16); e[3]=(uint8_t)(merged>>24);
    rc = wr(fs, fs->fat_off + (uint64_t)c*4, e, 4);
    if(rc) return rc;
    /* 第二份 FAT（若配置了 2 个）也要写，否则某些固件读到不一致 */
    if(fs->L.num_fats > 1){
        uint64_t off2 = fs->base + (uint64_t)(fs->L.reserved_sectors + fs->L.fat_size_sectors)*512
                        + (uint64_t)c*4;
        img_qcow2_write(fs->q, off2, e, 4);
    }
    return IMG_OK;
}

/* 分配一个空闲簇，返回簇号；失败返回 0 */
__attribute__((unused)) static uint32_t alloc_cluster(img_fat32_fs *fs){
    uint32_t start = fs->alloc_hint ? fs->alloc_hint : 3;
    for(uint32_t i=0;i<fs->L.cluster_count;i++){
        uint32_t c = start + i;
        if(c >= fs->L.cluster_count + 2) c = 3 + (c - (fs->L.cluster_count+2));
        if(c < 3) continue;
        uint32_t v=0;
        if(fat_get(fs, c, &v)) break;
        if(v == 0){
            if(fat_set(fs, c, FAT32_EOC)) break;
            fs->alloc_hint = c+1;
            return c;
        }
    }
    return 0;   /* 空间满 */
}

__attribute__((unused)) static int free_chain(img_fat32_fs *fs, uint32_t c){
    uint64_t guard=0;
    while(c >= 2 && c < FAT32_EOC){
        if(FAT32_CHAIN_GUARD(fs,guard)) return IMG_ECORRUPT;
        uint32_t n=0;
        if(fat_get(fs, c, &n)) return IMG_EIO;
        if(fat_set(fs, c, 0)) return IMG_EIO;
        c = n;
    }
    return IMG_OK;
}

/* 走簇链到第 n 个簇（0 起）；不存在返回 0 */
__attribute__((unused)) static uint32_t chain_nth(img_fat32_fs *fs, uint32_t start, uint32_t n){
    uint32_t c = start;
    for(uint32_t i=0;i<n;i++){
        if(c < 2 || c >= FAT32_EOC) return 0;
        uint32_t nx=0;
        if(fat_get(fs, c, &nx)) return 0;
        c = nx;
    }
    return (c>=2 && c<FAT32_EOC) ? c : 0;
}

/* ---------- 目录项辅助 ---------- */
typedef struct { uint32_t cluster; uint32_t size; bool is_dir; bool found;
                 uint64_t slot_off;     /* 8.3 项在盘上的偏移 */
                 uint64_t lfn_first;    /* 该条目的第一个 LFN 项偏移（0=无）*/
                 char     lfn[260]; } dir_ent;

/* FAT 规范里的 LFN 校验和：sum = ((sum&1)<<7) + (sum>>1) + name[i]，对 11 字节 8.3 名算 */
static uint8_t lfn_checksum(const uint8_t *name83){
    uint8_t sum=0;
    for(int i=0;i<11;i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name83[i]);
    return sum;
}
static void name_to_83(const char *nm, char out[12]){
    memset(out,' ',11); out[11]=0;
    const char *dot = strrchr(nm,'.');
    int base = dot ? (int)(dot-nm) : (int)strlen(nm);
    if(base>8) base=8;
    for(int i=0;i<base;i++) out[i] = (char)((nm[i]>='a'&&nm[i]<='z')? nm[i]-32 : nm[i]);
    if(dot){
        int e=(int)strlen(dot+1);
        if(e>3) e=3;
        for(int i=0;i<e;i++) out[8+i] = (char)((dot[1+i]>='a'&&dot[1+i]<='z')? dot[1+i]-32 : dot[1+i]);
    }
}
/* ★ 写盘用的是【补满 11 字节】的 8.3；比较时必须用【去尾空格】版，
 *   否则 "EFI" 与 "EFI        " 永不相等 —— 这个坑我踩过一次。*/
/* 比较用的规范形：去尾空格 + 基名/扩展名之间用点号（与 dir_find 里现拼的 n 一致）。
 * ★ 只去尾空格还不够 —— 'HELLO   TXT'（补空格形）与 'HELLO.TXT'（点号形）
 *   永远不相等。必须两边都规范成【点号形】。*/
static void name_to_83_dot(const char *nm, char out[12]){
    char pad[12]; name_to_83(nm,pad);
    int j=0;
    for(int i=0;i<8 && pad[i] && pad[i]!=' ';i++) out[j++]=pad[i];
    if(pad[8] && pad[8]!=' '){
        out[j++]='.';
        for(int i=8;i<11 && pad[i] && pad[i]!=' ';i++) out[j++]=pad[i];
    }
    out[j]=0;
}
__attribute__((unused)) static void name_to_83_trim(const char *nm, char out[12]){
    char pad[12]; name_to_83(nm, pad);
    int i=11; while(i>0 && pad[i-1]==' ') i--;
    pad[i]=0;
    memcpy(out,pad,(size_t)i+1);
}

/* 从一个 32 字节目录项里取元数据 */
static void ent_fill(dir_ent *o, const uint8_t *e, uint64_t slot_off){
    o->found    = true;
    o->is_dir   = (e[11] & ATTR_DIR)!=0;
    o->size     = (uint32_t)e[28]|((uint32_t)e[29]<<8)|((uint32_t)e[30]<<16)|((uint32_t)e[31]<<24);
    o->cluster  = ((uint32_t)e[20]<<16)|((uint32_t)e[21]<<24) | ((uint32_t)e[26]|((uint32_t)e[27]<<8));
    o->slot_off = slot_off;
}
/* 把 8.3 项名字拼成点号形（大写） */
static void ent_83_name(const uint8_t *e, char out[12]){
    int j=0;
    for(int k=0;k<8 && e[k]!=' ';k++) out[j++]=(char)((e[k]>='a'&&e[k]<='z')?e[k]-32:e[k]);
    if(e[8]!=' '){ out[j++]='.'; for(int k=8;k<11 && e[k]!=' ';k++)
        out[j++]=(char)((e[k]>='a'&&e[k]<='z')?e[k]-32:e[k]); }
    out[j]=0;
}

/* 目录里是否已经存在某个 11 字节 8.3 名（跳过 LFN 项、卷标项与已删除项）
 * 返回 1=已存在 · 0=不存在 · -1=镜像损坏（簇链成环/读失败） */
static int dir_has_83(img_fat32_fs *fs, uint32_t dirclus, const uint8_t n83[11]){
    size_t csz = (size_t)fs->L.sectors_per_cluster * 512;
    uint8_t *buf = malloc(csz);
    if(!buf) return -1;
    uint32_t c = dirclus; uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC){
        if(FAT32_CHAIN_GUARD(fs,guard)){ free(buf); return -1; }
        if(rd(fs, clus_off(fs,c), buf, csz)){ free(buf); return -1; }
        for(size_t i=0;i+ENTRY_SIZE<=csz;i+=ENTRY_SIZE){
            uint8_t *e = buf+i;
            if(e[0]==0x00){ free(buf); return 0; }              /* 目录结束 */
            if(e[0]==0xE5) continue;
            if((e[11]&0x0F)==0x0F) continue;
            if(e[11] & ATTR_VOLUME) continue;
            if(!memcmp(e,n83,11)){ free(buf); return 1; }
        }
        uint32_t nx=0; if(fat_get(fs,c,&nx)) break;
        c=nx;
    }
    free(buf);
    return 0;
}

/* 生成不与目录内已有项冲突的 11 字节 8.3 名（*aliased 返回是否被迫改名）。
 *
 * ★ Windows 规则：8.3 短名重名时用 '~N'（基名保留前几字符）。
 *   不做这一步的后果见 dir_find 顶部注释 —— 两个长名共用短名，
 *   后一个会把前一个"覆盖"掉，文件凭空消失。*/
static void make_short_83(img_fat32_fs *fs, uint32_t dirclus, const char *name,
                          uint8_t out83[11], bool *aliased){
    char pad[12]; name_to_83(name, pad);
    if(aliased) *aliased=false;
    {   int hv = dir_has_83(fs, dirclus, (const uint8_t*)pad);
        if(hv<0){ memcpy(out83,pad,11); return; }      /* 损坏：改名没意义，退回规范名 */
        if(!hv){ memcpy(out83,pad,11); return; }
    }
    int blen=0; while(blen<8 && pad[blen]!=' ') blen++;
    char ext[3]; for(int k=0;k<3;k++) ext[k]=pad[8+k];
    for(int n=1;n<1000000;n++){
        char num[12]; snprintf(num,sizeof num,"~%d",n);
        int nl=(int)strlen(num);
        int keep=8-nl; if(keep<1) keep=1; if(keep>blen) keep=blen;
        uint8_t cand[11]; memset(cand,' ',11);
        memcpy(cand, pad, (size_t)keep);
        memcpy(cand+keep, num, (size_t)nl);
        memcpy(cand+8, ext, 3);
        int cv = dir_has_83(fs, dirclus, cand);
        if(cv<0) break;                                 /* 损坏：放弃找唯一名 */
        if(!cv){ memcpy(out83,cand,11); if(aliased) *aliased=true; return; }
    }
    memcpy(out83, pad, 11);
}

/* 在目录簇里找 want（不区分大小写，支持 LFN 与 8.3）。目录会自动跨簇。
 *
 * ★ 长名/短名【别名】陷阱（实测踩到，症状是文件"写成功"却不见了）：
 *   "kd_02_1414.dll" 与 "kd_02_14e4.dll" 截断出的 8.3 短名都是
 *   "KD_02_14.DLL"。若只看短名判定，第二个会被当成"已存在"而
 *   【覆盖第一个的文件项】—— 目录里项数不增，文件凭空消失。
 *   规则：① 项上【有 LFN】就只比 LFN，绝不回退比短名；
 *        ② 短名匹配只作【兜底】，而且要扫完全目录 —— 万一后面还有
 *           一条 LFN 真正匹配的项（长名才是身份，短名只是别名）。*/
static int dir_find(img_fat32_fs *fs, uint32_t dirclus, const char *want, dir_ent *out){
    size_t csz = (size_t)fs->L.sectors_per_cluster * 512;
    uint8_t *buf = malloc(csz);
    if(!buf) return IMG_ENOMEM;
    memset(out,0,sizeof *out);
    char want83[12]; name_to_83_dot(want, want83);
    /* ★ 只有当【请求名本身就是 8.3 形状】时才允许用短名兜底匹配。
     *   否则 "xxxxxxxxx.dat"（9 字符基名）会拿截断后的 "XXXXXXXX.DAT"
     *   去撞已有的 "xxxxxxxx.dat" —— 又是同一个别名陷阱。*/
    bool want_is83 = false;
    {
        const char *dot = strrchr(want,'.');
        int base = dot ? (int)(dot-want) : (int)strlen(want);
        int ext  = dot ? (int)strlen(dot+1) : 0;
        want_is83 = (base>=1 && base<=8 && ext<=3);
    }
    char wantu[260]; snprintf(wantu,sizeof wantu,"%s",want);
    for(char *p=wantu;*p;p++) if(*p>='a'&&*p<='z') *p-=32;
    char lfn[260]; lfn[0]=0;
    bool have_lfn=false;
    dir_ent fb83; memset(&fb83,0,sizeof fb83);
    bool have_fb83=false;
    uint32_t c = dirclus;
    uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC){
        if(FAT32_CHAIN_GUARD(fs,guard)){ free(buf); return IMG_ECORRUPT; }
        uint64_t coff = clus_off(fs, c);
        if(rd(fs, coff, buf, csz)){ free(buf); return IMG_EIO; }
        for(size_t i=0;i+ENTRY_SIZE<=csz;i+=ENTRY_SIZE){
            uint8_t *e = buf+i;
            if(e[0]==0x00) goto done;                          /* 目录结束 */
            if(e[0]==0xE5){ lfn[0]=0; have_lfn=false; continue; }
            if((e[11] & 0x0F)==0x0F){                          /* LFN 项 */
                int ord = e[0] & 0x1F;
                if(ord<1 || ord>20){ lfn[0]=0; continue; }
                size_t off = (size_t)(ord-1)*13;
                static const int st[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
                for(int k=0;k<13;k++){
                    unsigned lo=e[st[k]], hi=e[st[k]+1];
                    unsigned ch = lo | (hi<<8);
                    if(off+(size_t)k >= sizeof lfn-1) break;
                    if(ch==0){ lfn[off+k]=0; have_lfn=true; break; }
                    lfn[off+k] = (ch<128) ? (char)ch : '?';
                    lfn[off+(size_t)k+1]=0;
                    have_lfn=true;
                }
                continue;
            }
            if(e[11] & ATTR_VOLUME){ lfn[0]=0; have_lfn=false; continue; }
            bool had_lfn = have_lfn && lfn[0];
            if(had_lfn){
                char a[260]; snprintf(a,sizeof a,"%s",lfn);
                for(char *p=a;*p;p++) if(*p>='a'&&*p<='z') *p-=32;
                if(!strcmp(a,wantu)){                    /* LFN 命中 → 立刻返回 */
                    ent_fill(out, e, coff+i);
                    free(buf);
                    return IMG_OK;
                }
            } else {
                char n[12]; ent_83_name(e, n);
                if(want_is83 && !strcmp(n, want83) && !have_fb83){   /* 先记账，扫完再定 */
                    ent_fill(&fb83, e, coff+i);
                    have_fb83=true;
                }
            }
            lfn[0]=0; have_lfn=false;
        }
        uint32_t nx=0; if(fat_get(fs,c,&nx)){ free(buf); return IMG_EIO; }
        c=nx;
    }
done:
    free(buf);
    if(have_fb83) *out = fb83;
    return IMG_OK;
}

/* ---------- 公开 API ---------- */
img_fat32_fs *img_fat32_mount(img_qcow2 *q, uint64_t part_lba, bool writable){
    if(!q) return NULL;
    img_fat32_fs *fs = calloc(1, sizeof *fs);
    if(!fs) return NULL;
    fs->q = q;
    if(img_fat32_probe(q, part_lba, &fs->L) != IMG_OK){ free(fs); return NULL; }
    fs->base = part_lba * 512;
    fs->fat_off  = (uint64_t)fs->L.reserved_sectors * 512;
    fs->data_off = ((uint64_t)fs->L.reserved_sectors + (uint64_t)fs->L.num_fats * fs->L.fat_size_sectors) * 512;
    fs->alloc_hint = 3;
    fs->writable = writable;
    return fs;
}
void img_fat32_unmount(img_fat32_fs *fs){ if(fs) free(fs); }
const img_fat32_layout_t *img_fat32_fs_layout(const img_fat32_fs *fs){
    return fs ? &fs->L : NULL;
}

/* 解析绝对路径到"父目录簇 + 末级名字"。
 * 用固定数组分词（strtok_r 的回退写法在多级路径下会出错）。 */
static int split_parent(img_fat32_fs *fs, const char *path,
                        uint32_t *out_dir, char *out_name, size_t namecap){
    char work[512]; snprintf(work,sizeof work,"%s",path);
    const char *parts[64]; int n=0;
    char *save=NULL;
    for(char *t=strtok_r(work,"/\\",&save); t && n<64; t=strtok_r(NULL,"/\\",&save))
        parts[n++] = t;
    if(n==0) return IMG_EINVAL;
    uint32_t dir = fs->L.root_cluster;
    for(int i=0;i<n-1;i++){
        dir_ent e;
        if(dir_find(fs, dir, parts[i], &e)!=IMG_OK || !e.found || !e.is_dir)
            return IMG_ENOENT;
        dir = e.cluster ? e.cluster : fs->L.root_cluster;
    }
    snprintf(out_name, namecap, "%s", parts[n-1]);
    *out_dir = dir;
    return IMG_OK;
}

bool img_fat32_path_exists(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path) return false;
    uint32_t dir; char name[256];
    if(split_parent(fs, abs_path, &dir, name, sizeof name)!=IMG_OK) return false;
    dir_ent e;
    return dir_find(fs, dir, name, &e)==IMG_OK && e.found;
}

int img_fat32_file_size(img_fat32_fs *fs, const char *abs_path, uint64_t *out){
    if(!fs||!abs_path||!out) return IMG_EINVAL;
    uint32_t dir; char name[256];
    if(split_parent(fs, abs_path, &dir, name, sizeof name)!=IMG_OK) return IMG_ENOENT;
    dir_ent e;
    if(dir_find(fs, dir, name, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    *out = e.size;
    return IMG_OK;
}

/* ---------- 写目录项（含 LFN） ---------- */
/* 在目录里插入一条：LFN 项（若干）+ 8.3 项。空间不够会扩展目录。 */
static int dir_add_entry(img_fat32_fs *fs, uint32_t dirclus, const char *name,
                         uint32_t clus, uint32_t size, bool is_dir);
/* 定位目录里第一个空槽（0x00 或 0xE5）。不够则扩展目录。 */
static int dir_make_room(img_fat32_fs *fs, uint32_t dirclus, int nslots,
                         uint64_t *out_off);

static int dir_make_room(img_fat32_fs *fs, uint32_t dirclus, int nslots, uint64_t *out_off){
    size_t csz = (size_t)fs->L.sectors_per_cluster * 512;
    uint8_t *buf = malloc(csz);
    if(!buf) return IMG_ENOMEM;
    uint32_t c = dirclus, prev=0;
    uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC){
        if(FAT32_CHAIN_GUARD(fs,guard)){ free(buf); return IMG_ECORRUPT; }
        uint64_t coff = clus_off(fs, c);
        if(rd(fs, coff, buf, csz)){ free(buf); return IMG_EIO; }
        int run=0; uint64_t run_start=0;
        for(size_t i=0;i+ENTRY_SIZE<=csz;i+=ENTRY_SIZE){
            uint8_t e0 = buf[i];
            if(e0==0x00 || e0==0xE5){
                if(run==0) run_start = coff + i;
                if(++run >= nslots){ *out_off = run_start; free(buf); return IMG_OK; }
            } else run=0;
        }
        prev=c;
        uint32_t nx=0; if(fat_get(fs,c,&nx)){ free(buf); return IMG_EIO; }
        c=nx;
    }
    /* 目录链已走完仍不够 → 给目录追加一个簇
     * ★ 追加【之前】必须把上一个簇尾部的空槽改成 0xE5（"已删除"标记）：
     *   那里的 0x00 会被读方当成【目录结束】—— 于是新簇里的项全都
     *   读不到（实测症状：目录项数卡在 16/每簇不再增长，文件"写成功"却消失）。
     *   0xE5 与 0x00 在读方眼里都是空槽，但不会终止目录。*/
    if(prev>=2 && prev<FAT32_EOC){
        uint64_t poff = clus_off(fs, prev);
        if(rd(fs, poff, buf, csz)){ free(buf); return IMG_EIO; }
        bool changed=false;
        for(size_t i=0;i+ENTRY_SIZE<=csz;i+=ENTRY_SIZE)
            if(buf[i]==0x00 || buf[i]==0xE5){ buf[i]=0xE5; changed=true; }
        if(changed && wr(fs, poff, buf, csz)){ free(buf); return IMG_EIO; }
    }
    uint32_t nc = alloc_cluster(fs);
    if(!nc){ free(buf); return IMG_EFULL; }
    if(prev>=2 && prev<FAT32_EOC){
        if(fat_set(fs, prev, nc)){ free(buf); return IMG_EIO; }
    }
    memset(buf,0,csz);
    if(wr(fs, clus_off(fs,nc), buf, csz)){ free(buf); return IMG_EIO; }
    *out_off = clus_off(fs, nc);
    free(buf);
    return IMG_OK;
}

static void put_u16(uint8_t *p, uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void put_u32(uint8_t *p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

static int dir_add_entry(img_fat32_fs *fs, uint32_t dirclus, const char *name,
                         uint32_t clus, uint32_t size, bool is_dir){
    int nchars = (int)strlen(name);
    /* ★ 需不需要 LFN 要看【是否符合 8.3 形状】，不是看长度：
     *   base ≤ 8 且（无扩展名 或 扩展名 ≤ 3）才算 8.3 可容纳。
     *   例："Microsoft"(9字符) 虽 < 11 但仍塞不进 8 位名，必须 LFN。*/
    bool fits83;
    {
        const char *dot = strrchr(name,'.');
        int base = dot ? (int)(dot-name) : nchars;
        int ext  = dot ? (int)strlen(dot+1) : 0;
        fits83 = (base>=1 && base<=8 && ext<=3);
    }
    /* ★ 短名必须目录内唯一：重名时退化成 '~N'（Windows 规则）。
     *   否则两个长名共用同一个短名 → 后写的会把先写的当成"已存在"覆盖掉。*/
    uint8_t n83[12]; bool aliased=false;
    make_short_83(fs, dirclus, name, n83, &aliased);
    /* ★ 名字里带小写字母也必须有 LFN —— 8.3 名只能是大写，
     *   不给 LFN 的话 "bootmgfw.efi" 在盘上就变成 "BOOTMGFW.EFI"（大小写丢失）。
     *   Windows 对这类名字一律写 LFN 保住原始大小写（真值 ESP 里就是这样）。*/
    bool has_lower=false;
    for(const char *p=name; *p; p++) if(*p>='a'&&*p<='z'){ has_lower=true; break; }
    bool need_lfn = !fits83 || aliased || has_lower;
    int lfn_slots = need_lfn ? ((nchars + 12) / 13) : 0;
    int total = lfn_slots + 1;
    uint64_t off=0;
    int rc = dir_make_room(fs, dirclus, total, &off);
    if(rc) return rc;

    /* ★ LFN 项的字节13 必须是 8.3 名的校验和 —— mtools 等严格实现会校验，
     *   不对就【整条 LFN 丢弃】（表现为名字退化成截断的 8.3，如 MICROSOF）。
     *   （我自己踩过：实现宽松能读，但外部工具读不到）*/
    uint8_t sum = lfn_checksum(n83);
    /* 写 LFN（按【倒序】：磁盘上是 ord N..1，紧跟 8.3 之前的是 ord1）*/
    for(int ord=lfn_slots; ord>=1; ord--){
        /* ★ 必须先清零：type(字节12) 与 FstClusLO(字节26..27) 不在下面的
         *   字符槽里，原先 memset 0xFF 会把它们留成 0xFF/0xFFFF ——
         *   type≠0 的 LFN 项被 Windows 的 FAT 驱动直接丢弃，
         *   长名退化成截断的 8.3 名（"Microsoft"→"MICROSOF"）→ 路径解析失败
         *   → bootmgr 报 0xc000000f「BCD 丢失」。
         *   （mtools 与自己的读取器都不看这两个字段，所以自测一直是绿的；
         *     padding 字符槽仍由下面按 0xFFFF 写，不受清零影响。）*/
        uint8_t e[ENTRY_SIZE]; memset(e,0,ENTRY_SIZE);
        e[0] = (uint8_t)ord;
        e[11] = ATTR_LFN;
        e[13] = sum;
        static const int st[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
        for(int k=0;k<13;k++){
            size_t idx = (size_t)(ord-1)*13 + (size_t)k;
            unsigned ch = 0;
            if(idx < (size_t)nchars) ch = (unsigned char)name[idx];
            else if(idx == (size_t)nchars) ch = 0;      /* 终止符 */
            else ch = 0xFFFF;
            put_u16(e + st[k], (uint16_t)ch);
        }
        if(ord==lfn_slots) e[0] |= 0x40;                 /* 最后一个标记 */
        if(wr(fs, off, e, ENTRY_SIZE)) return IMG_EIO;
        off += ENTRY_SIZE;
    }
    /* 8.3 项（用 make_short_83 定下的唯一短名，而不是再算一遍） */
    uint8_t d[ENTRY_SIZE]; memset(d,0,ENTRY_SIZE);
    memcpy(d, n83, 11);
    /* ★ 目录项字节明细（debug 档）：这一行就是为了"写成功但外部不认"那类问题
     *   准备的 —— LFN type/簇号、attr、短名别名，一眼能看出异常。*/
    {
        img_ev e;
        if(img_ev_init(&e,IMG_LV_DEBUG,"fat32","entry",IMG_OK,"写入目录项")){
            char n83s[13]; memcpy(n83s,n83,11); n83s[11]=0;
            img_ev_str(&e,"name",name);
            img_ev_str(&e,"short",n83s);
            img_ev_u64(&e,"attr",is_dir?ATTR_DIR:(d[11]|0));
            img_ev_u64(&e,"lfn",(uint64_t)lfn_slots);
            img_ev_u64(&e,"alias",aliased?1u:0u);
            img_ev_u64(&e,"cluster",clus);
            img_ev_u64(&e,"size_bytes",size);
            img_ev_emit(&e);
        }
    }
    d[11] = is_dir ? ATTR_DIR : 0x00;
    put_u16(d+20, (uint16_t)(clus >> 16));   /* FstClusHI */
    put_u16(d+26, (uint16_t)(clus & 0xFFFF));/* FstClusLO */
    put_u32(d+28, is_dir ? 0 : size);
    return wr(fs, off, d, ENTRY_SIZE);
}

/* 在新建子目录的簇头写入 '.' 与 '..' 两项 —— FAT 规范强制要求。
 *
 * ★ 少了它们会怎样（实测，2×2 交叉实验定罪）：
 *   我们的卷 + 我们建的目录        → bootmgr 报 0xc000000f「BCD 丢失」
 *   我们的卷 + mtools 建的目录     → 正常读出 BCD、走到 winload
 *   diskA 的卷 + 我们写的目录项    → 正常读出 BCD
 *   同一个文件、同一份 BCD，唯一差别就是目录里缺不缺这两项。
 *   （mmd/mkfs 都会写点号项；自己写目录时漏了。）
 * 格式（取自 diskA 真值逐字节）：名字 '.'+10空格 / '..'+9空格，attr=0x10，
 * size=0，`.` 指向自身簇，`..` 指向父簇（父是根目录时写 0）。 */
static int dir_write_dots(img_fat32_fs *fs, uint32_t self, uint32_t parent){
    uint8_t e[ENTRY_SIZE];
    memset(e,0,ENTRY_SIZE);
    memset(e,' ',11); e[0]='.'; e[11]=ATTR_DIR;
    put_u16(e+20,(uint16_t)(self>>16));
    put_u16(e+26,(uint16_t)(self&0xFFFF));
    if(wr(fs, clus_off(fs,self), e, ENTRY_SIZE)) return IMG_EIO;
    memset(e,0,ENTRY_SIZE);
    memset(e,' ',11); e[0]='.'; e[1]='.'; e[11]=ATTR_DIR;
    uint32_t p = (parent==fs->L.root_cluster) ? 0 : parent;
    put_u16(e+20,(uint16_t)(p>>16));
    put_u16(e+26,(uint16_t)(p&0xFFFF));
    if(wr(fs, clus_off(fs,self)+ENTRY_SIZE, e, ENTRY_SIZE)) return IMG_EIO;
    return IMG_OK;
}

int img_fat32_mkdir_p(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path) return IMG_EINVAL;
    char work[512]; snprintf(work,sizeof work,"%s",abs_path);
    uint32_t dir = fs->L.root_cluster;
    char *save=NULL;
    for(char *t=strtok_r(work,"/\\\\",&save); t; t=strtok_r(NULL,"/\\\\",&save)){
        dir_ent e;
        if(dir_find(fs, dir, t, &e)==IMG_OK && e.found){
            if(!e.is_dir) return IMG_ENOTSUP;
            dir = e.cluster ? e.cluster : fs->L.root_cluster;
            continue;
        }
        uint32_t nc = alloc_cluster(fs);
        if(!nc) return IMG_EFULL;
        /* 清空新簇 */
        size_t csz=(size_t)fs->L.sectors_per_cluster*512;
        uint8_t *z=calloc(1,csz); if(!z) return IMG_ENOMEM;
        int rc=wr(fs, clus_off(fs,nc), z, csz);
        free(z);
        if(rc) return rc;
        rc = dir_add_entry(fs, dir, t, nc, 0, true);
        if(rc) return rc;
        /* ★ 新子目录里必须有 . 与 ..（见 dir_write_dots 注释） */
        rc = dir_write_dots(fs, nc, dir);
        if(rc) return rc;
        dir = nc;
    }
    return IMG_OK;
}

/* ---------- 写入三件套（内存路径与流式路径共用，避免两处逻辑分叉） ---------- */
/* ① 准备：定位父目录（父目录不存在就自动建）→ 找旧项 → 释放旧簇链 */
static int put_prepare(img_fat32_fs *fs, const char *dst_abs_path,
                       uint32_t *dir, char *nm, size_t nmcap,
                       bool *existed, dir_ent *old){
    if(split_parent(fs, dst_abs_path, dir, nm, nmcap)!=IMG_OK){
        /* ★ 父目录不存在时按头文件承诺【自动建出来】。
         *   原先直接返回 ENOENT —— 头文件写着"自动建父目录"、实现却没有，
         *   于是往空 ESP 放文件必须调用方先 mkdir（实测 142 个文件全失败）。*/
        char par[512]; snprintf(par,sizeof par,"%s",dst_abs_path);
        char *sl=strrchr(par,'/');
        if(!sl) return IMG_ENOENT;
        if(sl==par) par[1]='\0'; else *sl='\0';
        int mrc=img_fat32_mkdir_p(fs, par);
        if(mrc!=IMG_OK) return mrc;
        if(split_parent(fs, dst_abs_path, dir, nm, nmcap)!=IMG_OK) return IMG_ENOENT;
    }
    dir_ent e;
    int rc=dir_find(fs, *dir, nm, &e);
    if(rc) return rc;
    *existed=e.found;
    if(old) *old=e;
    /* ★ 覆盖写【不再先删旧链】：先保留旧数据，等新数据写完、目录项改成功后再释放。
     *   这样中途取消/失败时旧文件完好无损（原先先删旧链 → 取消后留下悬空的目录项）。*/
    if(e.is_dir) return IMG_ENOTSUP;
    return IMG_OK;
}
/* ② 分配 need 个簇并连成链（失败时自行回收已分配的） */
static int put_alloc_chain(img_fat32_fs *fs, uint32_t need, uint32_t *first_out){
    uint32_t first=0, prev=0;
    for(uint32_t i=0;i<need;i++){
        uint32_t c=alloc_cluster(fs);
        if(!c){ if(first) free_chain(fs,first); return IMG_EFULL; }
        if(!first) first=c;
        if(prev) fat_set(fs,prev,c);
        prev=c;
    }
    *first_out=first;
    return IMG_OK;
}
/* ③ 落地目录项（已存在则原地改，否则新增） */
static int put_finalize(img_fat32_fs *fs, uint32_t dir, const char *nm,
                        uint32_t first, uint32_t size, bool existed, const dir_ent *old){
    if(existed && old){
        uint8_t d[ENTRY_SIZE];
        if(rd(fs, old->slot_off, d, ENTRY_SIZE)) return IMG_EIO;
        put_u16(d+20,(uint16_t)(first>>16));
        put_u16(d+26,(uint16_t)(first&0xFFFF));
        put_u32(d+28, size);
        int w = wr(fs, old->slot_off, d, ENTRY_SIZE);
        if(w) return w;
        /* 新数据已就位 → 才释放旧链（顺序反了会让失败/取消变成悬空指针）*/
        if(old->cluster>=2) free_chain(fs, old->cluster);
        return IMG_OK;
    }
    return dir_add_entry(fs, dir, nm, first, size, false);
}

/* 往 dst 写一段【内存里】的数据（供 put_data 用；大文件请走 put_file 流式路径） */
static int write_data(img_fat32_fs *fs, const void *data, size_t len,
                      const char *dst_abs_path){
    uint32_t dir; char nm[256]; bool ex=false; dir_ent old;
    int rc=put_prepare(fs,dst_abs_path,&dir,nm,sizeof nm,&ex,&old);
    if(rc) return rc;
    uint32_t csz=(uint32_t)fs->L.sectors_per_cluster*512;
    uint32_t need = len? (uint32_t)((len + csz - 1)/csz) : 0;
    uint32_t first=0;
    if(need){ rc=put_alloc_chain(fs,need,&first); if(rc) return rc; }
    if(need){
        uint8_t *tmp=calloc(1,csz);
        if(!tmp){ if(first) free_chain(fs,first); return IMG_ENOMEM; }
        uint32_t c=first;
        for(uint32_t i=0;i<need;i++){
            size_t off=(size_t)i*csz;
            size_t n=(len-off<csz)?(len-off):csz;
            memcpy(tmp,(const uint8_t*)data+off,n);
            if(i+1<need) memset(tmp+n,0,csz-n);          /* 非末簇要写满 */
            int w=wr(fs, clus_off(fs,c), tmp, csz);
            if(w){ free(tmp); if(first) free_chain(fs,first); return w; }
            if(i+1<need){ uint32_t nx=0; if(fat_get(fs,c,&nx)){ free(tmp); return IMG_EIO; } c=nx; }
        }
        free(tmp);
    }
    return put_finalize(fs,dir,nm,first,(uint32_t)len,ex,&old);
}

int img_fat32_put_data(img_fat32_fs *fs, const void *data, size_t len, const char *dst_abs_path){
    if(!fs||!data||!dst_abs_path) return IMG_EINVAL;
    return write_data(fs, data, len, dst_abs_path);
}

int img_fat32_put_file(img_fat32_fs *fs, const char *local_path,
                       const char *dst_abs_path, img_prog *prog){
    if(!fs||!local_path||!dst_abs_path) return IMG_EINVAL;
    img_span sp;
    img_span_prep(&sp,"fat32","put","写入 FAT32");
    img_span_kv_str(&sp,"path",dst_abs_path);
    int fd = open(local_path, O_RDONLY);
    if(fd<0) return IMG_ENOENT;
    struct stat st; if(fstat(fd,&st)<0){ close(fd); return IMG_EIO; }
    size_t len=(size_t)st.st_size;
    img_span_kv_u64(&sp,"size_bytes",len);
    img_span_begin(&sp);

    uint32_t dir; char nm[256]; bool ex=false; dir_ent old;
    int rc=put_prepare(fs,dst_abs_path,&dir,nm,sizeof nm,&ex,&old);
    if(rc){ close(fd); img_span_end(&sp,rc); return rc; }
    uint32_t csz=(uint32_t)fs->L.sectors_per_cluster*512;
    uint32_t need = len? (uint32_t)((len + csz - 1)/csz) : 0;
    uint32_t first=0;
    if(need){ rc=put_alloc_chain(fs,need,&first); if(rc){ close(fd); img_span_end(&sp,rc); return rc; } }

    /* ★ 流式写入：内存峰值 = 1MiB（原先整个文件进内存 —— 415MB 的 winre.wim
     *   在 Android 上直接把 App 顶到 OOM）。顺带也让进度变成流式的。*/
    enum { CHUNK = 1u<<20 };
    uint8_t *buf=malloc(CHUNK);
    if(!buf){ close(fd); if(first) free_chain(fs,first); img_span_end(&sp,IMG_ENOMEM); return IMG_ENOMEM; }
    uint32_t c=first; size_t coff=0, done=0;
    while(done<len){
        /* ★ 取消检查点：每个 1MiB 查一次。取消时【不写目录项】并释放已分配簇链，
         *   所以不会留下"半成品文件"（见 DESIGN-OUTPUT.md 第四节）*/
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        size_t want=len-done; if(want>CHUNK) want=CHUNK;
        size_t got=0;
        while(got<want){ ssize_t r=read(fd,buf+got,want-got); if(r<=0) break; got+=(size_t)r; }
        if(!got){ rc=IMG_EIO; break; }                    /* 源文件被截短/读失败 */
        size_t p=0;
        while(p<got){
            size_t room=csz-coff, n=(got-p<room)?(got-p):room;
            int w=wr(fs, clus_off(fs,c)+coff, buf+p, n);
            if(w){ rc=w; break; }
            p+=n; coff+=n; done+=n;
            if(coff==csz){ coff=0; uint32_t nx=0; if(fat_get(fs,c,&nx)){ rc=IMG_ECORRUPT; break; } c=nx; }
        }
        if(rc) break;
        if(prog) img_prog_bytes(prog,done,len);
        if(done<len && (c<2 || c>=FAT32_EOC)){ rc=IMG_ECORRUPT; break; }   /* 链比数据短 */
    }
    free(buf); close(fd);
    if(rc==IMG_OK && coff>0 && c>=2 && c<FAT32_EOC){       /* 尾簇补零：不把上一位住户的字节留在缝隙里 */
        size_t rest=csz-coff;
        uint8_t *z=calloc(1,rest);
        if(z){ wr(fs, clus_off(fs,c)+coff, z, rest); free(z); }
    }
    if(rc){
        if(first) free_chain(fs,first);                    /* 失败/取消：回收，不留半成品 */
        img_span_end(&sp,rc);
        return rc;
    }
    rc=put_finalize(fs,dir,nm,first,(uint32_t)len,ex,&old);
    img_span_kv_u64(&sp,"written_bytes",(uint64_t)len);
    img_span_end(&sp,rc);
    if(rc==IMG_OK && prog){
        img_prog_phase(prog, IMG_PH_COPY, "写入 FAT32");
        img_prog_op(prog, IMG_OP_WRITE_FILE);
        img_prog_item(prog, dst_abs_path);
        img_prog_bytes(prog, (uint64_t)len, (uint64_t)len);
    }
    return rc;
}


int img_fat32_read_file(img_fat32_fs *fs, const char *abs_path, void **out, size_t *out_len){
    if(!fs||!abs_path||!out||!out_len) return IMG_EINVAL;
    *out=NULL; *out_len=0;
    uint32_t dir; char nm[256];
    if(split_parent(fs, abs_path, &dir, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e;
    if(dir_find(fs, dir, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    if(e.is_dir) return IMG_ENOTSUP;
    void *buf = e.size? malloc(e.size) : malloc(1);
    if(!buf) return IMG_ENOMEM;
    size_t csz=(size_t)fs->L.sectors_per_cluster*512;
    uint32_t c=e.cluster, done=0;
    uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC && done<e.size){
        if(FAT32_CHAIN_GUARD(fs,guard)){ free(buf); return IMG_ECORRUPT; }
        size_t n = (e.size-done < csz)? (e.size-done) : csz;
        if(rd(fs, clus_off(fs,c), (uint8_t*)buf+done, n)){ free(buf); return IMG_EIO; }
        done += n;
        uint32_t nx=0; if(fat_get(fs,c,&nx)) { free(buf); return IMG_EIO; }
        c=nx;
    }
    *out=buf; *out_len=e.size;
    return IMG_OK;
}

int img_fat32_list_dir(img_fat32_fs *fs, const char *abs_path, img_fat32_list_cb cb, void *ud){
    if(!fs||!cb) return IMG_EINVAL;
    uint32_t dir = fs->L.root_cluster;
    if(abs_path && abs_path[0] && strcmp(abs_path,"/")){   /* 任何非根路径都要查 */
        uint32_t d; char nm[256];
        if(split_parent(fs, abs_path, &d, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
        dir_ent e;
        if(dir_find(fs, d, nm, &e)!=IMG_OK || !e.found || !e.is_dir) return IMG_ENOTSUP;
        dir = e.cluster ? e.cluster : fs->L.root_cluster;
    }
    size_t csz=(size_t)fs->L.sectors_per_cluster*512;
    uint8_t *buf=malloc(csz);
    if(!buf) return IMG_ENOMEM;
    char lfn[260]; lfn[0]=0; bool have=false;
    uint32_t c=dir; uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC){
        if(FAT32_CHAIN_GUARD(fs,guard)){ free(buf); return IMG_ECORRUPT; }
        if(rd(fs, clus_off(fs,c), buf, csz)){ free(buf); return IMG_EIO; }
        bool stop=false;
        for(size_t i=0;i+ENTRY_SIZE<=csz;i+=ENTRY_SIZE){
            uint8_t *e=buf+i;
            if(e[0]==0x00){ stop=true; break; }
            if(e[0]==0xE5){ lfn[0]=0; have=false; continue; }
            if((e[11]&0x0F)==0x0F){
                int ord=e[0]&0x1F;
                if(ord<1||ord>20){ lfn[0]=0; continue; }
                size_t off=(size_t)(ord-1)*13;
                static const int st[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
                for(int k=0;k<13;k++){
                    unsigned ch=e[st[k]]|((unsigned)e[st[k]+1]<<8);
                    if(off+(size_t)k>=sizeof lfn-1) break;
                    if(ch==0){ lfn[off+k]=0; have=true; break; }
                    lfn[off+k]=(ch<128)?(char)ch:'?';
                    lfn[off+(size_t)k+1]=0; have=true;
                }
                continue;
            }
            if(e[11] & (ATTR_VOLUME)){ lfn[0]=0; have=false; continue; }
            char n[12]; int j=0;
            for(int k=0;k<8 && e[k]!=' ';k++) n[j++]=(char)e[k];
            if(e[8]!=' '){ n[j++]='.'; for(int k=8;k<11 && e[k]!=' ';k++) n[j++]=(char)e[k]; }
            n[j]=0;
            uint32_t clus=((uint32_t)e[20]<<16)|((uint32_t)e[21]<<24)|(uint32_t)e[26]|((uint32_t)e[27]<<8);
            uint32_t sz  =(uint32_t)e[28]|((uint32_t)e[29]<<8)|((uint32_t)e[30]<<16)|((uint32_t)e[31]<<24);
            cb(ud, (have&&lfn[0])?lfn:n, (e[11]&ATTR_DIR)!=0, sz);
            (void)clus;
            lfn[0]=0; have=false;
        }
        if(stop) break;
        uint32_t nx=0; if(fat_get(fs,c,&nx)) break;
        c=nx;
    }
    free(buf);
    return IMG_OK;
}

/* 只数条目（用于判断目录是否为空）*/
static void count_cb(void *ud, const char *nm, bool d, uint64_t sz){
    (void)nm;(void)d;(void)sz; (*(int*)ud)++;
}

int img_fat32_rm(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path) return IMG_EINVAL;
    uint32_t dir; char nm[256];
    if(split_parent(fs, abs_path, &dir, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e;
    if(dir_find(fs, dir, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    if(e.is_dir){
        /* 只允许删空目录：非空时保守返回错误。
         * ★ 原先这里给 list_dir 传的是 NULL 回调（死代码），改成明确计数。*/
        uint32_t d2; char nm2[256]; int cnt=0;
        if(split_parent(fs, abs_path, &d2, nm2, sizeof nm2)==IMG_OK){
            dir_ent sub;
            if(dir_find(fs, d2, nm2, &sub)==IMG_OK && sub.found)
                img_fat32_list_dir(fs, abs_path, count_cb, &cnt);
        }
        if(cnt>2) return IMG_ENOTSUP;      /* 只有 . 与 .. 才算空 */
    }
    if(e.cluster>=2){
        /* ★ 必须检查返回值：链损坏时 free_chain 会返回 IMG_ECORRUPT，
         *   原先丢掉返回值 = 以为释放成功、把目录项标删，留下断链的半删除状态。*/
        int frc = free_chain(fs, e.cluster);
        if(frc!=IMG_OK) return frc;
    }
    uint8_t d[ENTRY_SIZE];
    if(rd(fs, e.slot_off, d, ENTRY_SIZE)) return IMG_EIO;
    d[0]=0xE5;
    /* 同时把该条目的 LFN 项也标删（往前扫）*/
    uint64_t off=e.slot_off;
    while(off >= fs->data_off + ENTRY_SIZE){
        uint8_t prev[ENTRY_SIZE];
        if(rd(fs, off-ENTRY_SIZE, prev, ENTRY_SIZE)) break;
        if((prev[11]&0x0F)==0x0F){ prev[0]=0xE5; wr(fs, off-ENTRY_SIZE, prev, ENTRY_SIZE); off-=ENTRY_SIZE; }
        else break;
    }
    return wr(fs, e.slot_off, d, ENTRY_SIZE);
}

/* 剩余空间（字节）：扫一遍 FAT0 数空簇。首次调用后缓存，直到 FAT 被修改。
 * 用途：App 想在写大文件前校验空间（默认不校验 —— 见 DESIGN-OUTPUT.md 第五节） */
uint64_t img_fat32_free_bytes(img_fat32_fs *fs){
    if(!fs) return 0;
    if(fs->free_valid) return fs->free_bytes;
    size_t fat_bytes=(size_t)fs->L.fat_size_sectors*512;
    uint8_t *fat=malloc(fat_bytes);
    if(!fat) return 0;
    if(rd(fs, fs->fat_off, fat, fat_bytes)){ free(fat); return 0; }
    uint64_t free_c=0;
    for(uint32_t c=2;c<fs->L.cluster_count+2;c++){
        size_t o=(size_t)c*4;
        if(o+4>fat_bytes) break;
        uint32_t v=((uint32_t)fat[o]|((uint32_t)fat[o+1]<<8)|((uint32_t)fat[o+2]<<16)|((uint32_t)fat[o+3]<<24)) & FAT32_MASK;
        if(v==0) free_c++;
    }
    free(fat);
    fs->free_bytes = free_c*(uint64_t)fs->L.sectors_per_cluster*512;
    fs->free_valid = true;
    return fs->free_bytes;
}
