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
#include <time.h>

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

static int fat_unique_name(img_fat32_fs *fs, const char *dir, const char *base,
                           char *out, size_t cap);   /* 定义在后 */

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
                 uint16_t crt_time, crt_date, acc_date, wrt_time, wrt_date;  /* FAT 原始时间字段 */
                 char     lfn[260]; } dir_ent;

/* FAT 规范里的 LFN 校验和：sum = ((sum&1)<<7) + (sum>>1) + name[i]，对 11 字节 8.3 名算 */
static uint8_t lfn_checksum(const uint8_t *name83){
    uint8_t sum=0;
    for(int i=0;i<11;i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name83[i]);
    return sum;
}
/* ---------- UTF-8 ↔ UTF-16LE（不依赖 iconv；Android/Windows 都能编） ----------
 * 为什么必须做：FAT 的长文件名在盘上是 UTF-16LE。旧实现把 UTF-8 的每个字节
 * 当成一个 UTF-16 码元写进去、读回来再把 >=128 的码元换成 '?' ——
 * 中文名在 ESP 上会变成 "?????? ??????"，而且写完自己都找不到（路径解析失败）。
 * 2026-10-24 真机复现后补上。*/
static int u8_to_u16(const char *s, uint16_t *out, int cap){
    int n=0;
    const unsigned char *p=(const unsigned char*)s;
    while(*p){
        uint32_t cp; int adv;
        if(p[0]<0x80){ cp=p[0]; adv=1; }
        else if((p[0]&0xE0)==0xC0 && p[1] && (p[1]&0xC0)==0x80){
            cp=((uint32_t)(p[0]&0x1F)<<6)|(p[1]&0x3F); adv=2;
        } else if((p[0]&0xF0)==0xE0 && p[1] && p[2] && (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80){
            cp=((uint32_t)(p[0]&0x0F)<<12)|((uint32_t)(p[1]&0x3F)<<6)|(p[2]&0x3F); adv=3;
        } else if((p[0]&0xF8)==0xF0 && p[1] && p[2] && p[3] &&
                  (p[1]&0xC0)==0x80 && (p[2]&0xC0)==0x80 && (p[3]&0xC0)==0x80){
            cp=((uint32_t)(p[0]&0x07)<<18)|((uint32_t)(p[1]&0x3F)<<12)|
               ((uint32_t)(p[2]&0x3F)<<6)|(p[3]&0x3F); adv=4;
        } else { cp='?'; adv=1; }
        if(cp>=0x10000){
            if(n+2>cap) return -1;
            cp-=0x10000;
            out[n++]=(uint16_t)(0xD800+(cp>>10));
            out[n++]=(uint16_t)(0xDC00+(cp&0x3FF));
        } else {
            if(n+1>cap) return -1;
            out[n++]=(uint16_t)cp;
        }
        p+=adv;
    }
    return n;
}
static int u16_to_u8(const uint16_t *u, int n, char *out, size_t cap){
    size_t o=0;
    for(int i=0;i<n;i++){
        uint32_t cp=u[i];
        if(cp>=0xD800 && cp<=0xDBFF && i+1<n && u[i+1]>=0xDC00 && u[i+1]<=0xDFFF){
            cp=0x10000+((cp-0xD800)<<10)+(u[i+1]-0xDC00); i++;
        } else if(cp>=0xD800 && cp<=0xDFFF) cp='?';
        if(cp<0x80){ if(o+1>=cap) break; out[o++]=(char)cp; }
        else if(cp<0x800){ if(o+2>=cap) break;
            out[o++]=(char)(0xC0|(cp>>6)); out[o++]=(char)(0x80|(cp&0x3F)); }
        else if(cp<0x10000){ if(o+3>=cap) break;
            out[o++]=(char)(0xE0|(cp>>12)); out[o++]=(char)(0x80|((cp>>6)&0x3F));
            out[o++]=(char)(0x80|(cp&0x3F)); }
        else { if(o+4>=cap) break;
            out[o++]=(char)(0xF0|(cp>>18)); out[o++]=(char)(0x80|((cp>>12)&0x3F));
            out[o++]=(char)(0x80|((cp>>6)&0x3F)); out[o++]=(char)(0x80|(cp&0x3F)); }
    }
    if(cap) out[o]=0;
    return (int)o;
}

static void name_to_83(const char *nm, char out[12]){
    memset(out,' ',11); out[11]=0;
    const char *dot = strrchr(nm,'.');
    int base = dot ? (int)(dot-nm) : (int)strlen(nm);
    if(base>8) base=8;
    /* 8.3 名只能是 ASCII：非 ASCII 字节一律 '_'（Windows 的短名也是这样糊出来的）*/
    for(int i=0;i<base;i++){
        unsigned char c=(unsigned char)nm[i];
        out[i] = (c>=0x80 || c<0x20) ? '_'
               : (char)((c>='a'&&c<='z')? c-32 : c);
    }
    if(dot){
        int e=(int)strlen(dot+1);
        if(e>3) e=3;
        for(int i=0;i<e;i++){
            unsigned char c=(unsigned char)dot[1+i];
            out[8+i] = (c>=0x80 || c<0x20) ? '_'
                     : (char)((c>='a'&&c<='z')? c-32 : c);
        }
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
    o->crt_time = (uint16_t)(e[14] | (e[15] << 8));
    o->crt_date = (uint16_t)(e[16] | (e[17] << 8));
    o->acc_date = (uint16_t)(e[18] | (e[19] << 8));
    o->wrt_time = (uint16_t)(e[22] | (e[23] << 8));
    o->wrt_date = (uint16_t)(e[24] | (e[25] << 8));
}
/* 把 8.3 项名字拼成点号形（大写） */
static void ent_83_name(const uint8_t *e, char out[12]){
    int j=0;
    for(int k=0;k<8 && e[k]!=' ';k++) out[j++]=(char)((e[k]>='a'&&e[k]<='z')?e[k]-32:e[k]);
    if(e[8]!=' '){ out[j++]='.'; for(int k=8;k<11 && e[k]!=' ';k++)
        out[j++]=(char)((e[k]>='a'&&e[k]<='z')?e[k]-32:e[k]); }
    out[j]=0;
}

/* ---------- LFN 累加器（dir_find 与 list_dir 共用） ----------
 * 为什么单独抽出来（2026-10-23 在真 Windows 的 ESP 上实测出的 bug）：
 *   LFN 在盘上是【倒序】存放的（ord=N…1），名字超过 13 字符就要占多个槽。
 *   旧写法每写一个字符顺手在下一个字节补结束符，于是最后处理的 ord=1
 *   把结束符补在了 buf[13] —— 正好【截断】已经填好的后半截。
 *   症状：fat.ls 里 "Renamed-Long.stl" 显示成 "Renamed-Long."；
 *        fat.exists / fat.export 找不到 >13 字符的文件；
 *        而 mtools 能正常读同一个目录 —— 说明盘是对的，是我们读错了。
 *   现在：见到带 0x40 的那片就重置；结束符统一由 lfn_value() 补。*/
typedef struct { uint16_t u[261]; char buf[4*261+4]; size_t len; bool active; bool bad; } lfn_acc;
static void lfn_reset(lfn_acc *a){ a->buf[0]=0; a->len=0; a->active=false; a->bad=false; }
/* 喂一条目录项。返回 1=这是 LFN 项（已消费），0=不是。*/
static int lfn_feed(lfn_acc *a, const uint8_t *e){
    if((e[11] & 0x0F) != 0x0F) return 0;                 /* 不是 LFN 项 */
    int ord = e[0] & 0x1F;
    if(ord < 1 || ord > 20){ lfn_reset(a); return 1; }   /* 乱序/超限：重新开始 */
    if(e[0] & 0x40) lfn_reset(a);                        /* 磁盘上最先读到的那片 */
    size_t off = (size_t)(ord - 1) * 13;
    if(off >= (sizeof a->u/sizeof a->u[0]) - 1){ a->bad = true; return 1; }
    static const int st[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    for(int k = 0; k < 13; k++){
        unsigned ch = (unsigned)e[st[k]] | ((unsigned)e[st[k]+1] << 8);
        size_t i = off + (size_t)k;
        if(i >= (sizeof a->u/sizeof a->u[0]) - 1){ a->bad = true; break; }
        if(ch == 0){ if(i > a->len) a->len = i; a->active = true; break; }   /* 名字结束 */
        if(ch == 0xFFFF) break;                                              /* 填充位 */
        a->u[i] = (uint16_t)ch;
        if(i + 1 > a->len) a->len = i + 1;
        a->active = true;
    }
    return 1;
}
static const char *lfn_value(lfn_acc *a){
    if(!a->active || a->bad || a->len == 0){ a->buf[0]=0; return a->buf; }
    u16_to_u8(a->u, (int)a->len, a->buf, sizeof a->buf);
    return a->buf;
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
    char wantu[1024]; snprintf(wantu,sizeof wantu,"%s",want);
    for(char *p=wantu;*p;p++) if(*p>='a'&&*p<='z') *p-=32;
    lfn_acc la; lfn_reset(&la);
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
            if(e[0]==0xE5){ lfn_reset(&la); continue; }
            if(lfn_feed(&la, e)) continue;                     /* LFN 项交给累加器 */
            if(e[11] & ATTR_VOLUME){ lfn_reset(&la); continue; }
            const char *lfn = lfn_value(&la);
            bool had_lfn = la.active && lfn[0];
            if(had_lfn){
                char a[1024]; snprintf(a,sizeof a,"%s",lfn);
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
            lfn_reset(&la);
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
    char work[4096]; snprintf(work,sizeof work,"%s",path);
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

bool img_fat32_dir_exists(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path||!*abs_path) return false;
    if(!strcmp(abs_path,"/")) return true;
    uint32_t dir; char nm[1024];
    if(split_parent(fs, abs_path, &dir, nm, sizeof nm)!=IMG_OK) return false;
    dir_ent e;
    return dir_find(fs, dir, nm, &e)==IMG_OK && e.found && e.is_dir;
}

bool img_fat32_path_exists(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path) return false;
    uint32_t dir; char name[1024];
    if(split_parent(fs, abs_path, &dir, name, sizeof name)!=IMG_OK) return false;
    dir_ent e;
    return dir_find(fs, dir, name, &e)==IMG_OK && e.found;
}

int img_fat32_file_size(img_fat32_fs *fs, const char *abs_path, uint64_t *out){
    if(!fs||!abs_path||!out) return IMG_EINVAL;
    uint32_t dir; char name[1024];
    if(split_parent(fs, abs_path, &dir, name, sizeof name)!=IMG_OK) return IMG_ENOENT;
    dir_ent e;
    if(dir_find(fs, dir, name, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    *out = e.size;
    return IMG_OK;
}

/* ---------- 写目录项（含 LFN） ---------- */
/* 在目录里插入一条：LFN 项（若干）+ 8.3 项。空间不够会扩展目录。 */
static int dir_add_entry(img_fat32_fs *fs, uint32_t dirclus, const char *name,
                         uint32_t clus, uint32_t size, bool is_dir, uint64_t *out_off);
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
     *   无法读取（实测症状：目录项数卡在 16/每簇不再增长，文件"写成功"却消失）。
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

/* 当前时间的 FAT/DOS 编码（日期：年-1980 占 7 位 / 月 4 位 / 日 5 位；时间：时 5 位 / 分 6 位 / 秒÷2 5 位）。
 * ★ 以前新目录项的时间戳全是 0（mtools 显示 "1980-00-00 0:00"，月/日都是 0 不是合法 DOS 日期）。
 *   Windows 虽容忍，但资源管理器里日期是空的，CHKDSK 也可能报"时间戳异常"。
 *   新建项一律盖当前时间；改名则保留原项的时间（见 img_fat32_rename）。*/
static void dos_now(uint16_t *out_date, uint16_t *out_time){
    time_t now=time(NULL); struct tm tmv; memset(&tmv,0,sizeof tmv);
#if defined(_WIN32)
    localtime_s(&tmv,&now);
#else
    localtime_r(&now,&tmv);
#endif
    int y=tmv.tm_year+1900; if(y<1980) y=1980; if(y>2107) y=2107;
    *out_date=(uint16_t)(((y-1980)<<9)|((tmv.tm_mon+1)<<5)|tmv.tm_mday);
    *out_time=(uint16_t)((tmv.tm_hour<<11)|(tmv.tm_min<<5)|(tmv.tm_sec/2));
}

static int dir_add_entry(img_fat32_fs *fs, uint32_t dirclus, const char *name,
                         uint32_t clus, uint32_t size, bool is_dir, uint64_t *out_off){
    uint16_t uname[256];
    int nu = u8_to_u16(name, uname, 255);
    if(nu <= 0) return IMG_EINVAL;
    if(nu > 255) return IMG_ENAMETOOLONG;
    int nchars = nu;
    /* ★ 需不需要 LFN 要看【是否符合 8.3 形状】，不是看长度：
     *   base ≤ 8 且（无扩展名 或 扩展名 ≤ 3）才算 8.3 可容纳。
     *   例："Microsoft"(9字符) 虽 < 11 但仍塞不进 8 位名，必须 LFN。*/
    bool fits83, has_nonascii=false;
    {
        const char *dot = strrchr(name,'.');
        int base = dot ? (int)(dot-name) : (int)strlen(name);
        int ext  = dot ? (int)strlen(dot+1) : 0;
        fits83 = (base>=1 && base<=8 && ext<=3);
        for(const unsigned char *q=(const unsigned char*)name; *q; q++)
            if(*q>=0x80){ has_nonascii=true; break; }
        if(has_nonascii) fits83=false;      /* 8.3 只能 ASCII */
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
     *   （我自己踩过：实现宽松能读，但外部工具无法读取）*/
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
            if(idx < (size_t)nchars) ch = uname[idx];
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
    {   uint16_t dt=0, tm=0; dos_now(&dt,&tm);
        put_u16(d+14, tm);   /* CrtTime    */
        put_u16(d+16, dt);   /* CrtDate    */
        put_u16(d+18, dt);   /* LstAccDate */
        put_u16(d+22, tm);   /* WrtTime    */
        put_u16(d+24, dt);   /* WrtDate    */
    }
    put_u16(d+20, (uint16_t)(clus >> 16));   /* FstClusHI */
    put_u16(d+26, (uint16_t)(clus & 0xFFFF));/* FstClusLO */
    put_u32(d+28, is_dir ? 0 : size);
    int wrc=wr(fs, off, d, ENTRY_SIZE);
    if(wrc==IMG_OK && out_off) *out_off=off;
    return wrc;
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
    uint16_t dt=0, tm=0; dos_now(&dt,&tm);
    uint8_t e[ENTRY_SIZE];
    memset(e,0,ENTRY_SIZE);
    memset(e,' ',11); e[0]='.'; e[11]=ATTR_DIR;
    put_u16(e+14,tm); put_u16(e+16,dt); put_u16(e+18,dt); put_u16(e+22,tm); put_u16(e+24,dt);
    put_u16(e+20,(uint16_t)(self>>16));
    put_u16(e+26,(uint16_t)(self&0xFFFF));
    if(wr(fs, clus_off(fs,self), e, ENTRY_SIZE)) return IMG_EIO;
    memset(e,0,ENTRY_SIZE);
    memset(e,' ',11); e[0]='.'; e[1]='.'; e[11]=ATTR_DIR;
    put_u16(e+14,tm); put_u16(e+16,dt); put_u16(e+18,dt); put_u16(e+22,tm); put_u16(e+24,dt);
    uint32_t p = (parent==fs->L.root_cluster) ? 0 : parent;
    put_u16(e+20,(uint16_t)(p>>16));
    put_u16(e+26,(uint16_t)(p&0xFFFF));
    if(wr(fs, clus_off(fs,self)+ENTRY_SIZE, e, ENTRY_SIZE)) return IMG_EIO;
    return IMG_OK;
}

int img_fat32_mkdir_p(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path) return IMG_EINVAL;
    char work[4096]; snprintf(work,sizeof work,"%s",abs_path);
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
        rc = dir_add_entry(fs, dir, t, nc, 0, true, NULL);
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
        char par[2048]; snprintf(par,sizeof par,"%s",dst_abs_path);
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
    return dir_add_entry(fs, dir, nm, first, size, false, NULL);
}

/* 往 dst 写一段【内存里】的数据（供 put_data 用；大文件请走 put_file 流式路径） */
static int write_data(img_fat32_fs *fs, const void *data, size_t len,
                      const char *dst_abs_path){
    uint32_t dir; char nm[1024]; bool ex=false; dir_ent old;
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

/* ---------- 宿主 fd → img_stream（put_file 与复制引擎共用） ---------- */
typedef struct { int fd; } hostfd_src;
static int hostfd_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    hostfd_src *h = (hostfd_src*)ud;
    if(got) *got = 0;
    ssize_t r = pread(h->fd, buf, want, (off_t)off);
    if(r < 0) return IMG_EIO;
    *got = (size_t)r;
    return IMG_OK;
}

/* ---------- 流式写（任意源 → FAT32 文件）----------
 * 语义与 NTFS 侧 img_ntfs_write_stream 对齐：
 *   · 先落到同目录临时名 .imgtmp-<pid>-<n>，全部写完再改名；
 *   · 失败/取消 → 释放已分配簇链 + 删临时项，不留半成品；
 *   · 覆盖时也先写新数据，成功后才释放旧链（取消不会毁掉旧文件）。
 * FAT32 单文件上限 4 GiB-1（目录项 size 是 32 位），超出直接拒绝而不是写坏。*/
int img_fat32_unique_name(img_fat32_fs *fs, const char *dir_abs, const char *base,
                          char *out, size_t cap){
    if(!fs||!dir_abs||!base||!out) return -1;
    return fat_unique_name(fs, dir_abs, base, out, cap);
}

int img_fat32_write_stream(img_fat32_fs *fs, const char *dst_abs,
                           const img_stream *src, const img_src_meta *meta,
                           int conflict, bool preserve_times, img_prog *prog,
                           char *out_path, size_t out_cap,
                           uint64_t *out_size, bool *out_replaced){
    if(!fs || !dst_abs || !src || !src->read || !meta) return IMG_EINVAL;
    if(out_size) *out_size = 0;
    if(out_replaced) *out_replaced = false;
    uint64_t len64 = meta->size;
    if(len64 > 0xFFFFFFFFULL) return IMG_ERANGE;         /* FAT32 单文件上限 */

    char ddir[2048], dname[1024];
    {   /* 先确保父目录存在（mkdir_p 已存在则成功） */
        char par[2048]; snprintf(par,sizeof par,"%s",dst_abs);
        char *sl=strrchr(par,'/');
        if(!sl) return IMG_ENOENT;
        if(sl==par) par[1]='\0'; else *sl='\0';
        if(!img_fat32_dir_exists(fs, par)){
            int mrc=img_fat32_mkdir_p(fs, par);
            if(mrc!=IMG_OK) return mrc;
        }
    }
    /* 父目录绝对路径 + 名字 */
    {
        char tmp[2048]; snprintf(tmp,sizeof tmp,"%s",dst_abs);
        char *sl=strrchr(tmp,'/');
        if(!sl) return IMG_EINVAL;
        if(sl==tmp) snprintf(ddir,sizeof ddir,"/");
        else { *sl=0; snprintf(ddir,sizeof ddir,"%s",tmp); }
        snprintf(dname,sizeof dname,"%s",sl+1);
        if(!dname[0]) return IMG_EINVAL;
        /* 字符数检查在 dir_add_entry（按 UTF-16 码元）里做；这里不按字节误杀中文名 */
    }
    uint32_t dirclus=0; char nm[1024];
    if(split_parent(fs, dst_abs, &dirclus, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;

    /* 冲突策略 */
    char final_name[1024]; snprintf(final_name,sizeof final_name,"%s",dname);
    char full[4096];
    snprintf(full,sizeof full,"%s%s%s",ddir,strcmp(ddir,"/")?"/":"",final_name);
    bool replaced=false;
    if(img_fat32_path_exists(fs, full)){
        if(conflict==0) return IMG_EEXISTS;
        if(img_fat32_dir_exists(fs, full)) return IMG_ESTATE;
        if(conflict==2){
            if(fat_unique_name(fs, ddir, dname, final_name, sizeof final_name)!=0) return IMG_EEXISTS;
            snprintf(full,sizeof full,"%s%s%s",ddir,strcmp(ddir,"/")?"/":"",final_name);
        } else replaced=true;
    }

    /* 临时名（探测到确实不存在） */
    char tmp_name[64], tmp_full[4096];
    bool have=false;
    for(int i=0;i<64 && !have;i++){
        snprintf(tmp_name,sizeof tmp_name,".imgtmp-%d-%d",(int)getpid(),i);
        snprintf(tmp_full,sizeof tmp_full,"%s%s%s",ddir,strcmp(ddir,"/")?"/":"",tmp_name);
        if(!img_fat32_path_exists(fs, tmp_full)) have=true;
    }
    if(!have) return IMG_EEXISTS;

    uint32_t csz=(uint32_t)fs->L.sectors_per_cluster*512;
    uint32_t need = len64? (uint32_t)((len64 + csz - 1)/csz) : 0;
    uint32_t first=0;
    int rc=IMG_OK;
    if(need){ rc=put_alloc_chain(fs,need,&first); if(rc) return rc; }

    enum { CHUNK = 1u<<20 };
    uint8_t *buf=malloc(CHUNK);
    if(!buf){ if(first) free_chain(fs,first); return IMG_ENOMEM; }
    if(prog){ img_prog_phase(prog,IMG_PH_EXTRACT,"复制文件");
              img_prog_op(prog,IMG_OP_WRITE); img_prog_bytes(prog,0,len64); }
    uint32_t c=first; size_t coff=0; uint64_t done=0;
    while(done<len64){
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        size_t want=(size_t)((len64-done<CHUNK)?(len64-done):CHUNK);
        size_t got=0;
        rc=src->read(src->ud, done, buf, want, &got);
        if(rc!=IMG_OK) break;
        if(got==0){ rc=IMG_EIO; break; }
        size_t p=0;
        while(p<got){
            size_t room=csz-coff, n=(got-p<room)?(got-p):room;
            int w=wr(fs, clus_off(fs,c)+coff, buf+p, n);
            if(w){ rc=w; break; }
            p+=n; coff+=n; done+=n;
            if(coff==csz){
                coff=0; uint32_t nx=0;
                if(fat_get(fs,c,&nx)){ rc=IMG_ECORRUPT; break; }
                c=nx;
            }
        }
        if(rc) break;
        if(prog) img_prog_bytes(prog,done,len64);
        if(done<len64 && (c<2 || c>=FAT32_EOC)){ rc=IMG_ECORRUPT; break; }
    }
    free(buf);
    if(rc==IMG_OK && coff>0 && c>=2 && c<FAT32_EOC){       /* 尾簇补零 */
        size_t rest=csz-coff;
        uint8_t *z=calloc(1,rest);
        if(z){ wr(fs, clus_off(fs,c)+coff, z, rest); free(z); }
    }
    if(rc){
        if(first) free_chain(fs,first);
        return rc;
    }

    /* 数据已就位 → 建目录项（名字是临时名，最后再改名） */
    rc=dir_add_entry(fs, dirclus, tmp_name, first, (uint32_t)len64, false, NULL);
    if(rc){ if(first) free_chain(fs,first); return rc; }

    if(preserve_times && meta->has_times){
        uint16_t t5[5]={0,0,0,0,0};
        uint16_t tm=0, dt=0;
        img_fat32_epoch_to_dos((int64_t)meta->times[0], &t5[0], &t5[1]);   /* 创建 */
        img_fat32_epoch_to_dos((int64_t)meta->times[2], &tm, &t5[2]);     /* 访问(只存日期) */
        img_fat32_epoch_to_dos((int64_t)meta->times[1], &tm, &dt);        /* 修改 */
        t5[3]=tm; t5[4]=dt;
        (void)img_fat32_file_set_times(fs,tmp_full,t5);
    }

    if(replaced){
        int rr=img_fat32_rm(fs, full);
        if(rr!=IMG_OK && rr!=IMG_ENOENT){ (void)img_fat32_rm(fs,tmp_full); return rr; }
    }
    rc=img_fat32_rename(fs, tmp_full, full);
    if(rc!=IMG_OK){ (void)img_fat32_rm(fs,tmp_full); return rc; }
    if(!img_fat32_path_exists(fs, full)) return IMG_ECORRUPT;   /* 写后自证 */
    if(out_path) snprintf(out_path,out_cap,"%.*s",(int)(out_cap-1),full);
    if(out_size) *out_size=len64;
    if(out_replaced) *out_replaced=replaced;
    if(prog) img_prog_done(prog,"复制完成");
    return IMG_OK;
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
    img_src_meta meta; memset(&meta,0,sizeof meta);
    meta.size=(uint64_t)st.st_size;
    meta.has_times=true;
    meta.times[0]=(uint64_t)st.st_ctime;
    meta.times[1]=(uint64_t)st.st_mtime;
    meta.times[2]=(uint64_t)st.st_atime;
    meta.times[3]=(uint64_t)st.st_ctime;
    img_span_kv_u64(&sp,"size_bytes",meta.size);
    img_span_begin(&sp);
    hostfd_src h; h.fd=fd;
    img_stream s; s.ud=&h; s.read=hostfd_read; s.close=NULL;
    /* put 的老语义：覆盖已存在（conflict=1）；时间由调用方的 preserve_times 决定，
     * 这里保持"不继承宿主时间"（与旧行为一致，App 显式要才传） */
    int rc=img_fat32_write_stream(fs,dst_abs_path,&s,&meta,1,false,prog,
                                  NULL,0,NULL,NULL);
    close(fd);
    img_span_kv_u64(&sp,"written_bytes",meta.size);
    img_span_end(&sp,rc);
    return rc;
}

/* ---------- 流式读句柄 ---------- */
struct img_fat32_rd {
    img_fat32_fs *fs;
    uint32_t start;      /* 首簇 */
    uint32_t cur;        /* 当前簇 */
    uint64_t cur_idx;    /* 当前簇序号 */
    uint64_t size;
    uint32_t csz;
};
int img_fat32_rd_open(img_fat32_fs *fs, const char *abs_path, img_fat32_rd **out,
                      img_src_meta *meta){
    if(out) *out=NULL;
    if(!fs||!abs_path||!out) return IMG_EINVAL;
    uint32_t dir; char nm[1024];
    if(split_parent(fs, abs_path, &dir, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e; memset(&e,0,sizeof e);
    if(dir_find(fs, dir, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    if(e.is_dir) return IMG_ENOTSUP;
    img_fat32_rd *rd=(img_fat32_rd*)calloc(1,sizeof *rd);
    if(!rd) return IMG_ENOMEM;
    rd->fs=fs; rd->start=e.cluster; rd->cur=e.cluster; rd->cur_idx=0;
    rd->size=e.size; rd->csz=(uint32_t)fs->L.sectors_per_cluster*512;
    if(meta){
        memset(meta,0,sizeof *meta);
        meta->size=e.size;
        meta->has_times=true;
        meta->times[0]=(uint64_t)img_fat32_dos_to_epoch(e.crt_time,e.crt_date);
        meta->times[1]=(uint64_t)img_fat32_dos_to_epoch(e.wrt_time,e.wrt_date);
        meta->times[2]=(uint64_t)img_fat32_dos_to_epoch(0,e.acc_date);   /* 只有日期 */
        meta->times[3]=meta->times[1];
        meta->has_flags=false;
    }
    *out=rd;
    return IMG_OK;
}
int img_fat32_rd_read(img_fat32_rd *h, uint64_t off, void *buf, size_t want, size_t *got){
    if(got) *got=0;
    if(!h||!buf||!got) return IMG_EINVAL;
    if(off>=h->size) return IMG_OK;
    uint64_t idx=off/h->csz;
    if(idx<h->cur_idx){ h->cur=h->start; h->cur_idx=0; }
    uint64_t guard=0;
    while(h->cur_idx<idx){
        uint32_t nx=0;
        if(FAT32_CHAIN_GUARD(h->fs,guard)) return IMG_ECORRUPT;
        if(fat_get(h->fs,h->cur,&nx)) return IMG_EIO;
        if(nx<2||nx>=FAT32_EOC) return IMG_ECORRUPT;
        h->cur=nx; h->cur_idx++;
    }
    size_t done=0;
    uint64_t pos=off;
    while(done<want && pos<h->size){
        size_t coff=(size_t)(pos%h->csz);
        size_t room=h->csz-coff;
        size_t n=(size_t)(h->size-pos<room?(h->size-pos):room);
        if(n>want-done) n=want-done;
        if(rd(h->fs, clus_off(h->fs,h->cur)+coff, (uint8_t*)buf+done, n)) return IMG_EIO;
        done+=n; pos+=n;
        if(coff+n==h->csz && pos<h->size){
            uint32_t nx=0;
            if(FAT32_CHAIN_GUARD(h->fs,guard)) return IMG_ECORRUPT;
            if(fat_get(h->fs,h->cur,&nx)) return IMG_EIO;
            if(nx<2||nx>=FAT32_EOC) return IMG_ECORRUPT;
            h->cur=nx; h->cur_idx++;
        }
    }
    *got=done;
    return IMG_OK;
}
void img_fat32_rd_close(img_fat32_rd *rd){ free(rd); }

typedef struct { img_fat32_rd *rd; } fat_rd_wrap;
static int fat_wrap_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    return img_fat32_rd_read(((fat_rd_wrap*)ud)->rd, off, buf, want, got);
}

int img_fat32_read_file(img_fat32_fs *fs, const char *abs_path, void **out, size_t *out_len){
    if(!fs||!abs_path||!out||!out_len) return IMG_EINVAL;
    *out=NULL; *out_len=0;
    uint32_t dir; char nm[1024];
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

int img_fat32_copy_out(img_fat32_fs *fs, const char *abs_path, const char *dst_path,
                       img_prog *prog, uint64_t *out_bytes){
    if(out_bytes) *out_bytes=0;
    if(!fs||!abs_path||!dst_path) return IMG_EINVAL;
    uint32_t dir; char nm[1024];
    if(split_parent(fs, abs_path, &dir, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e;
    if(dir_find(fs, dir, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    if(e.is_dir) return IMG_ENOTSUP;
    FILE *o=fopen(dst_path,"wb");
    if(!o) return IMG_EIO;
    if(prog){ img_prog_op(prog,IMG_OP_READ);
              img_prog_phase(prog,IMG_PH_EXTRACT,"导出 FAT32 文件");
              img_prog_bytes(prog,0,e.size); }
    size_t csz=(size_t)fs->L.sectors_per_cluster*512;
    uint8_t *buf=malloc(csz?csz:512);
    int rc=IMG_OK;
    if(!buf){ fclose(o); return IMG_ENOMEM; }
    uint32_t c=e.cluster, done=0; uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC && done<e.size){
        if(FAT32_CHAIN_GUARD(fs,guard)){ rc=IMG_ECORRUPT; break; }
        size_t n=(e.size-done<csz)?(size_t)(e.size-done):csz;
        if(rd(fs,clus_off(fs,c),buf,n)){ rc=IMG_EIO; break; }
        if(fwrite(buf,1,n,o)!=n){ rc=IMG_EIO; break; }
        done+=(uint32_t)n;
        if(prog) img_prog_bytes(prog,done,e.size);
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
        uint32_t nx=0; if(fat_get(fs,c,&nx)){ rc=IMG_EIO; break; }
        c=nx;
    }
    free(buf); fclose(o);
    if(rc!=IMG_OK){ unlink(dst_path); return rc; }
    if(out_bytes) *out_bytes=done;
    return IMG_OK;
}

int img_fat32_list_dir(img_fat32_fs *fs, const char *abs_path, img_fat32_list_cb cb, void *ud){
    if(!fs||!cb) return IMG_EINVAL;
    uint32_t dir = fs->L.root_cluster;
    if(abs_path && abs_path[0] && strcmp(abs_path,"/")){   /* 任何非根路径都要查 */
        uint32_t d; char nm[1024];
        if(split_parent(fs, abs_path, &d, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
        dir_ent e;
        if(dir_find(fs, d, nm, &e)!=IMG_OK || !e.found || !e.is_dir) return IMG_ENOTSUP;
        dir = e.cluster ? e.cluster : fs->L.root_cluster;
    }
    size_t csz=(size_t)fs->L.sectors_per_cluster*512;
    uint8_t *buf=malloc(csz);
    if(!buf) return IMG_ENOMEM;
    lfn_acc la; lfn_reset(&la);
    uint32_t c=dir; uint64_t guard=0;
    while(c>=2 && c<FAT32_EOC){
        if(FAT32_CHAIN_GUARD(fs,guard)){ free(buf); return IMG_ECORRUPT; }
        if(rd(fs, clus_off(fs,c), buf, csz)){ free(buf); return IMG_EIO; }
        bool stop=false;
        for(size_t i=0;i+ENTRY_SIZE<=csz;i+=ENTRY_SIZE){
            uint8_t *e=buf+i;
            if(e[0]==0x00){ stop=true; break; }
            if(e[0]==0xE5){ lfn_reset(&la); continue; }
            if(lfn_feed(&la, e)) continue;
            if(e[11] & (ATTR_VOLUME)){ lfn_reset(&la); continue; }
            char n[12]; int j=0;
            for(int k=0;k<8 && e[k]!=' ';k++) n[j++]=(char)e[k];
            if(e[8]!=' '){ n[j++]='.'; for(int k=8;k<11 && e[k]!=' ';k++) n[j++]=(char)e[k]; }
            n[j]=0;
            uint32_t clus=((uint32_t)e[20]<<16)|((uint32_t)e[21]<<24)|(uint32_t)e[26]|((uint32_t)e[27]<<8);
            uint32_t sz  =(uint32_t)e[28]|((uint32_t)e[29]<<8)|((uint32_t)e[30]<<16)|((uint32_t)e[31]<<24);
            const char *lfnv = lfn_value(&la);
            const char *nm_out=(la.active && lfnv[0])?lfnv:n;
            /* ★ 不把 "." / ".." 抛给上层：与 ntfs.list 保持一致，
             *   否则 App 要给两套目录渲染逻辑（实测 fat.ls 会多出这两行）。*/
            if(strcmp(nm_out,".") && strcmp(nm_out,".."))
                cb(ud, nm_out, (e[11]&ATTR_DIR)!=0, sz);
            (void)clus;
            lfn_reset(&la);
        }
        if(stop) break;
        uint32_t nx=0; if(fat_get(fs,c,&nx)) break;
        c=nx;
    }
    free(buf);
    return IMG_OK;
}

/* 只数条目（用于判断目录是否为空）
 * ★ list_dir 已经把 "." / ".." 过滤掉了 —— 所以这里数到的就是【真实条目数】，
 *   判空条件是 cnt==0，不是 cnt>2（旧写法把「3 个文件的目录」当空目录删掉，
 *   簇链被释放但目录项还在子簇里 → 整棵子树脱链且空间泄漏）。*/
static void count_cb(void *ud, const char *nm, bool d, uint64_t sz){
    (void)nm;(void)d;(void)sz; (*(int*)ud)++;
}

/* 把一条目录项（连同紧挨在它前面的 LFN 项）标记为已删除（首字节 0xE5）。
 * rm 与 rename 共用：两处各写一遍必然分叉（一份修好另一份还会犯）。
 * ★ 只置 0xE5，不写 0x00：0x00 会让读方认为"目录到此结束"，
 *   后面所有项集体消失（dir_make_room 里记过同一个坑）。*/
static int dir_slot_delete(img_fat32_fs *fs, uint64_t slot_off){
    uint8_t d[ENTRY_SIZE];
    if(rd(fs, slot_off, d, ENTRY_SIZE)) return IMG_EIO;
    d[0]=0xE5;
    /* 往前扫，把属于它的 LFN 项也标删 */
    uint64_t off=slot_off;
    while(off >= fs->data_off + ENTRY_SIZE){
        uint8_t prev[ENTRY_SIZE];
        if(rd(fs, off-ENTRY_SIZE, prev, ENTRY_SIZE)) break;
        if((prev[11]&0x0F)==0x0F){
            prev[0]=0xE5;
            if(wr(fs, off-ENTRY_SIZE, prev, ENTRY_SIZE)) return IMG_EIO;
            off-=ENTRY_SIZE;
        }
        else break;
    }
    return wr(fs, slot_off, d, ENTRY_SIZE);
}

/* 读子目录首簇里的 ".." 项 → 父目录簇号（根目录记作 0）。
 * FAT 的父引用【只有这一处】，所以目录移动必须同步它。*/
static int dir_dotdot_parent(img_fat32_fs *fs, uint32_t dirclus, uint32_t *out){
    uint8_t e[ENTRY_SIZE];
    if(rd(fs, clus_off(fs,dirclus)+ENTRY_SIZE, e, ENTRY_SIZE)) return IMG_EIO;
    if(e[0]!='.' || e[1]!='.') return IMG_ECORRUPT;
    *out = ((uint32_t)e[20]<<16)|((uint32_t)e[21]<<24)
         | (uint32_t)e[26]|((uint32_t)e[27]<<8);
    return IMG_OK;
}
static int dir_set_dotdot_parent(img_fat32_fs *fs, uint32_t dirclus, uint32_t parent){
    uint64_t off = clus_off(fs,dirclus)+ENTRY_SIZE;
    uint8_t e[ENTRY_SIZE];
    if(rd(fs, off, e, ENTRY_SIZE)) return IMG_EIO;
    if(e[0]!='.' || e[1]!='.') return IMG_ECORRUPT;
    uint32_t p = (parent==fs->L.root_cluster) ? 0 : parent;   /* 根目录的父记作 0 */
    e[20]=(uint8_t)(p>>16); e[21]=(uint8_t)(p>>24);
    e[26]=(uint8_t)(p&0xFF); e[27]=(uint8_t)((p>>8)&0xFF);
    return wr(fs, off, e, ENTRY_SIZE);
}

int img_fat32_rm(img_fat32_fs *fs, const char *abs_path){
    if(!fs||!abs_path) return IMG_EINVAL;
    uint32_t dir; char nm[1024];
    if(split_parent(fs, abs_path, &dir, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e;
    if(dir_find(fs, dir, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    if(e.is_dir){
        /* 只允许删空目录：非空时保守返回错误（不递归）。*/
        int cnt=0;
        if(img_fat32_list_dir(fs, abs_path, count_cb, &cnt)!=IMG_OK) return IMG_EIO;
        if(cnt>0) return IMG_ENOTSUP;
    }
    if(e.cluster>=2){
        /* ★ 必须检查返回值：链损坏时 free_chain 会返回 IMG_ECORRUPT，
         *   原先丢掉返回值 = 以为释放成功、把目录项标删，留下断链的半删除状态。*/
        int frc = free_chain(fs, e.cluster);
        if(frc!=IMG_OK) return frc;
    }
    return dir_slot_delete(fs, e.slot_off);
}

/* 改名 / 移动（同一分区内）。目录也可移动：FAT 的父引用只有子目录里的 ".." 一项，
 * 搬完同步它就行（NTFS 侧不是这样，那边还有 $I30 索引 —— 见 img_ntfs_fs.c）。
 * 目标已存在 → IMG_EEXISTS（是否覆盖是政策，留给命令层/App）。
 * 顺序：① 先建新项（指向同一簇）② 目录换了父就改 ".." ③ 再删旧项。
 *   任何一步失败，数据都还在（最坏情况是出现两条同名项，方向可查、可恢复）。*/
/* ---------- 盘内复制（FAT32） ---------- */
static void fat_split(const char *abs, char *dir, size_t dcap, char *name, size_t ncap){
    const char *sl = strrchr(abs, '/');
    if (!sl) { snprintf(dir, dcap, "/"); snprintf(name, ncap, "%s", abs); return; }
    size_t dl = (size_t)(sl - abs);
    if (dl == 0) snprintf(dir, dcap, "/");
    else { if (dl >= dcap) dl = dcap - 1; memcpy(dir, abs, dl); dir[dl] = 0; }
    snprintf(name, ncap, "%s", sl + 1);
}

static int fat_unique_name(img_fat32_fs *fs, const char *dir, const char *base,
                           char *out, size_t cap){
    for (int i = 0; i < 1000; i++) {
        char cand[1024]; int n;
        if (i == 0) n = snprintf(cand, sizeof cand, "%s", base);
        else {
            /* 同 NTFS 侧：扩展名之前插 " (N)"，目录则 "名 (N)" */
            const char *dot = strrchr(base, '.');
            bool has_ext = dot && dot != base && dot[1];
            if (has_ext) n = snprintf(cand, sizeof cand, "%.*s (%d)%s", (int)(dot - base), base, i, dot);
            else         n = snprintf(cand, sizeof cand, "%s (%d)", base, i);
        }
        if (n < 0 || (size_t)n >= sizeof cand) continue;
        char full[4096];
        snprintf(full, sizeof full, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", cand);
        if (!img_fat32_path_exists(fs, full)) { snprintf(out, cap, "%.*s", (int)(cap - 1), cand); return 0; }
    }
    return -1;
}

/* ---------- FAT 递归删除 ---------- */
typedef struct { char (*nm)[1024]; bool *isdir; int n, cap, overflow; } fat_rm_bag;
static void fat_rm_collect(void *ud, const char *name, bool is_dir, uint64_t size){
    fat_rm_bag *b=(fat_rm_bag*)ud; (void)size;
    if(b->n>=b->cap){ b->overflow=1; return; }
    snprintf(b->nm[b->n],1024,"%s",name); b->isdir[b->n]=is_dir; b->n++;
}
static int fat_rm_tree_rec(img_fat32_fs *fs,const char *abs,int depth,int *removed){
    if(depth>32) return IMG_ESTATE;
    if(!img_fat32_path_exists(fs,abs)) return IMG_ENOENT;
    if(!img_fat32_dir_exists(fs,abs)){
        int r=img_fat32_rm(fs,abs);
        if(r==IMG_OK && removed) (*removed)++;
        return r;
    }
    fat_rm_bag *b=(fat_rm_bag*)calloc(1,sizeof *b);
    if(!b) return IMG_ENOMEM;
    b->cap=512;
    b->nm=(char (*)[1024])calloc((size_t)b->cap,1024);
    b->isdir=(bool*)calloc((size_t)b->cap,sizeof(bool));
    if(!b->nm||!b->isdir){ free(b->nm); free(b->isdir); free(b); return IMG_ENOMEM; }
    int rc=img_fat32_list_dir(fs,abs,fat_rm_collect,b);
    if(rc==IMG_OK && b->overflow) rc=IMG_EFULL;
    if(rc==IMG_OK){
        for(int i=0;i<b->n && rc==IMG_OK;i++){
            char child[1200];
            int n=snprintf(child,sizeof child,"%s%s%s",abs,
                (abs[0] && abs[strlen(abs)-1]=='/')?"":"/", b->nm[i]);
            if(n<0||(size_t)n>=sizeof child){ rc=IMG_EINVAL; break; }
            rc=fat_rm_tree_rec(fs,child,depth+1,removed);
        }
    }
    free(b->nm); free(b->isdir); free(b);
    if(rc!=IMG_OK) return rc;
    rc=img_fat32_rm(fs,abs);                 /* 此时已空 */
    if(rc==IMG_OK && removed) (*removed)++;
    return rc;
}
int img_fat32_rm_tree(img_fat32_fs *fs,const char *abs_path,int *removed){
    if(removed) *removed=0;
    if(!fs||!abs_path||!abs_path[0]||!strcmp(abs_path,"/")) return IMG_EINVAL;
    return fat_rm_tree_rec(fs,abs_path,0,removed);
}

/* ---------- FAT 目录项时间（本地时间，2 秒粒度，1980 起） ---------- */
/* 读某个路径的 5 个原始字段：crt_time/crt_date/acc_date/wrt_time/wrt_date */
/* FAT 的 (时间,日期) 两个 u16 → unix 秒（★ 本地时间，FAT 规范如此）。
 * 旧实现按 UTC 解释 → UTC+8 设备上「文件夹显示的时间早 8 小时」。
 * 注意：这依赖进程时区（Android 上由 App 启动时 tzset 决定）；
 * 同一张卡拿到别的时区看，墙上时间不变 —— 这正是 FAT 的设计。*/
int64_t img_fat32_dos_to_epoch(uint16_t tm, uint16_t dt){
    if (!dt) return 0;
    struct tm g; memset(&g, 0, sizeof g);
    g.tm_year = 1980 + ((dt >> 9) & 0x7f) - 1900;
    g.tm_mon  = ((dt >> 5) & 0x0f) - 1;
    g.tm_mday = dt & 0x1f;
    g.tm_hour = (tm >> 11) & 0x1f;
    g.tm_min  = (tm >> 5) & 0x3f;
    g.tm_sec  = (tm & 0x1f) * 2;
    g.tm_isdst = -1;
    if (g.tm_mon < 0 || g.tm_mday < 1) return 0;
    time_t t = mktime(&g);
    return (t == (time_t)-1) ? 0 : (int64_t)t;
}
int img_fat32_file_times(img_fat32_fs *fs, const char *abs_path, uint16_t out[5]){
    if(!fs||!abs_path||!out) return IMG_EINVAL;
    uint32_t dclus=0; char nm[1024];
    if(split_parent(fs, abs_path, &dclus, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e; memset(&e,0,sizeof e);
    if(dir_find(fs, dclus, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    out[0]=e.crt_time; out[1]=e.crt_date; out[2]=e.acc_date;
    out[3]=e.wrt_time; out[4]=e.wrt_date;
    return IMG_OK;
}
/* 把同样的 5 个字段写回某个路径的目录项（其它字段不动） */
int img_fat32_file_set_times(img_fat32_fs *fs, const char *abs_path, const uint16_t t[5]){
    if(!fs||!abs_path||!t) return IMG_EINVAL;
    uint32_t dclus=0; char nm[1024];
    if(split_parent(fs, abs_path, &dclus, nm, sizeof nm)!=IMG_OK) return IMG_ENOENT;
    dir_ent e; memset(&e,0,sizeof e);
    if(dir_find(fs, dclus, nm, &e)!=IMG_OK || !e.found) return IMG_ENOENT;
    uint8_t d[ENTRY_SIZE];
    if(rd(fs, e.slot_off, d, ENTRY_SIZE)) return IMG_EIO;
    put_u16(d+14,t[0]); put_u16(d+16,t[1]); put_u16(d+18,t[2]);
    put_u16(d+22,t[3]); put_u16(d+24,t[4]);
    return wr(fs, e.slot_off, d, ENTRY_SIZE) ? IMG_EIO : IMG_OK;
}
/* 宿主 epoch 秒 → FAT 的 (时间,日期) 两个 u16（★ 本地时间，与 dos_to_epoch 对称） */
void img_fat32_epoch_to_dos(int64_t t, uint16_t *tm, uint16_t *dt){
    struct tm g; time_t tt=(time_t)t;
    if(!localtime_r(&tt,&g)){ *tm=0; *dt=0; return; }
    int y=g.tm_year+1900; if(y<1980){ *tm=0; *dt=0; return; }
    *tm=(uint16_t)((g.tm_hour<<11)|(g.tm_min<<5)|(g.tm_sec/2));
    *dt=(uint16_t)(((y-1980)<<9)|((g.tm_mon+1)<<5)|g.tm_mday);
}
int img_fat32_copy(img_fat32_fs *fs, const char *src_abs, const char *dst_abs,
                   int conflict, img_prog *prog,
                   char *out_path, size_t out_cap, uint64_t *out_size, bool *out_replaced){
    if (!fs || !src_abs || !dst_abs) return IMG_EINVAL;
    img_fat32_rd *rd=NULL; img_src_meta meta; memset(&meta,0,sizeof meta);
    int rc=img_fat32_rd_open(fs, src_abs, &rd, &meta);
    if(rc!=IMG_OK) return rc;

    /* 目标目录语义：dst 以 / 结尾或已是目录 ⇒ 目标名取源末段 */
    char final[4096];
    char sdir[2048], sname[1024];
    fat_split(src_abs, sdir, sizeof sdir, sname, sizeof sname);
    size_t dl=strlen(dst_abs);
    bool dst_is_dir=(dl && dst_abs[dl-1]=='/') || img_fat32_dir_exists(fs,dst_abs);
    if(dst_is_dir) snprintf(final,sizeof final,"%s%s%s",dst_abs,
                            (dst_abs[0]&&dst_abs[strlen(dst_abs)-1]=='/')?"":"/",sname);
    else snprintf(final,sizeof final,"%s",dst_abs);

    fat_rd_wrap wr; wr.rd=rd;
    img_stream st; st.ud=&wr; st.read=fat_wrap_read; st.close=NULL;
    rc=img_fat32_write_stream(fs, final, &st, &meta, conflict, true, prog,
                              out_path, out_cap, out_size, out_replaced);
    img_fat32_rd_close(rd);
    return rc;
}

int img_fat32_rename(img_fat32_fs *fs, const char *src_abs, const char *dst_abs){
    if(!fs||!src_abs||!dst_abs||!*src_abs||!*dst_abs) return IMG_EINVAL;
    if(!fs->writable) return IMG_ENOTSUP;
    uint32_t sdir, ddir; char sname[1024], dname[1024];
    if(split_parent(fs, src_abs, &sdir, sname, sizeof sname)!=IMG_OK) return IMG_ENOENT;
    if(split_parent(fs, dst_abs, &ddir, dname, sizeof dname)!=IMG_OK) return IMG_ENOENT;
    dir_ent se, de;
    int rc=dir_find(fs, sdir, sname, &se);
    if(rc!=IMG_OK) return rc;
    if(!se.found) return IMG_ENOENT;
    if(sdir==ddir && !strcmp(sname, dname)) return IMG_EINVAL;   /* 完全同名：自己改自己 */
    /* 只改大小写（a.txt → A.txt）：FAT 目录项查找不区分大小写，
     * 直接 dir_add_entry 会撞上"自己已存在"。做法：先改到临时名，再改到目标名；
     * 第二步失败就改回原名（回滚到原样，不留半个状态）。目录同样适用。
     * 说明：NTFS 那边是 img_ntfs_move_path 自带的 case-only 改名，不走这里。*/
    if(sdir==ddir && !strcasecmp(sname, dname)){
        char parent[4096], tmp[4096];
        snprintf(parent,sizeof parent,"%s",src_abs);
        char *sl=strrchr(parent,'/');
        if(!sl) return IMG_EINVAL;
        if(sl==parent) parent[1]=0; else *sl=0;      /* "/a.txt" → "/" */
        bool got=false;
        for(int n=0;n<1000 && !got;n++){
            char nm[64]; snprintf(nm,sizeof nm,"~ics%d.tmp",n);
            dir_ent te;
            if(dir_find(fs,sdir,nm,&te)!=IMG_OK) return IMG_EIO;
            if(te.found) continue;                   /* 撞上真文件：换下一个 */
            if(snprintf(tmp,sizeof tmp,"%s%s%s",parent,
                        (parent[0]&&parent[strlen(parent)-1]=='/')?"":"/",nm) >= (int)sizeof tmp)
                return IMG_ENAMETOOLONG;
            got=true;
        }
        if(!got) return IMG_EEXISTS;
        rc=img_fat32_rename(fs,src_abs,tmp);
        if(rc!=IMG_OK) return rc;
        rc=img_fat32_rename(fs,tmp,dst_abs);
        if(rc!=IMG_OK) (void)img_fat32_rename(fs,tmp,src_abs);   /* 回滚 */
        return rc;
    }
    rc=dir_find(fs, ddir, dname, &de);
    if(rc!=IMG_OK) return rc;
    if(de.found) return IMG_EEXISTS;
    if(se.is_dir && se.cluster>=2){
        /* 目录不能move进自己的子树（否则整棵子树从根上脱链）：
         * 从目标父目录沿 ".." 往上走，撞见源目录簇就拒绝。*/
        uint32_t c=ddir; uint64_t guard=0;
        while(c>=2 && c<FAT32_EOC){
            if(c==se.cluster) return IMG_EINVAL;
            if(FAT32_CHAIN_GUARD(fs,guard)) return IMG_ECORRUPT;
            uint32_t p=0;
            if(dir_dotdot_parent(fs,c,&p)!=IMG_OK) break;   /* ".." 坏了：只靠上层路径检查 */
            if(p==0 || p==c) break;
            c=p;
        }
    }
    uint64_t new_off=0;
    rc=dir_add_entry(fs, ddir, dname, se.cluster, se.size, se.is_dir, &new_off);
    if(rc) return rc;
    /* 继承源项的属性与时间戳：Windows 的"改名"是同一对象换名字，不是"新建 + 删旧"。
     * 不继承的后果：archive/readonly/hidden 位丢失、创建/修改时间变成"现在"，
     * 备份工具与增量复制会因此认错（改名后整文件被重新备份）。*/
    {   uint8_t s8[ENTRY_SIZE], n8[ENTRY_SIZE];
        if(rd(fs, se.slot_off, s8, ENTRY_SIZE)==IMG_OK &&
           rd(fs, new_off,    n8, ENTRY_SIZE)==IMG_OK){
            memcpy(n8+11, s8+11, 9);    /* attr · NTRes · CrtTimeTenth · CrtTime · CrtDate · LstAccDate */
            memcpy(n8+22, s8+22, 4);    /* WrtTime · WrtDate */
            wr(fs, new_off, n8, ENTRY_SIZE);
        }
    }
    if(se.is_dir && se.cluster>=2 && sdir!=ddir){
        int prc=dir_set_dotdot_parent(fs, se.cluster, ddir);
        if(prc!=IMG_OK){
            /* 回滚刚建的新项，回到"原样"而不是"两条项" */
            dir_ent ne;
            if(dir_find(fs, ddir, dname, &ne)==IMG_OK && ne.found) dir_slot_delete(fs, ne.slot_off);
            return prc;
        }
    }
    return dir_slot_delete(fs, se.slot_off);
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
