#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_bcd.c — BCD 语义层（hive 底层全部交给 libhivex）
 * 容器格式与元素 ID 规律见 reference/BCD-FORMAT.md */
#include "img_bcd.h"
#include <hivex.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

struct img_bcd { hive_h *h; };

/* ---------- 编码辅助 ---------- */
static char *u16_to_utf8(const void *p, size_t n){
    const char *q=(const char*)p; size_t chars=n/2;
    char *o=malloc(chars*3+4); if(!o) return NULL;
    size_t j=0;
    /* 上界必须允许写满 chars 字节；原写法 j+4<chars+1 会提前截断最后1~2字符 */
    for(size_t i=0;i+1<n;i+=2){
        unsigned c=(unsigned char)q[i]|((unsigned char)q[i+1]<<8);
        if(c==0) break;
        if(c<0x80) o[j++]=(char)c;
        else if(c<0x800){ o[j++]=(char)(0xC0|(c>>6)); o[j++]=(char)(0x80|(c&0x3F)); }
        else { o[j++]=(char)(0xE0|(c>>12)); o[j++]=(char)(0x80|((c>>6)&0x3F)); o[j++]=(char)(0x80|(c&0x3F)); }
    }
    o[j]=0; return o;
}
static char *utf8_to_u16(const char *s, size_t *out_len){
    size_t len=strlen(s), n=0;
    for(size_t i=0;i<len;){ unsigned c=(unsigned char)s[i];
        if(c<0x80){i++;n+=2;} else if((c&0xE0)==0xC0){i+=2;n+=2;} else {i+=3;n+=2;} }
    char *o=calloc(1,n+2); if(!o) return NULL;
    size_t j=0;
    for(size_t i=0;i<len;){
        unsigned c=(unsigned char)s[i], cp;
        if(c<0x80){cp=c;i++;}
        else if((c&0xE0)==0xC0){cp=((c&0x1F)<<6)|((unsigned char)s[i+1]&0x3F);i+=2;}
        else {cp=((c&0x0F)<<12)|(((unsigned char)s[i+1]&0x3F)<<6)|((unsigned char)s[i+2]&0x3F);i+=3;}
        o[j++]=(char)(cp&0xFF); o[j++]=(char)(cp>>8);
    }
    if(out_len) *out_len=j;
    return o;
}

/* ---------- 生命周期 ---------- */
img_bcd *img_bcd_open_file(const char *path){
    hive_h *h = hivex_open(path, HIVEX_OPEN_WRITE);
    if(!h) return NULL;
    img_bcd *b=calloc(1,sizeof *b);
    if(!b){ hivex_close(h); return NULL; }
    b->h=h; return b;
}
img_bcd *img_bcd_open_mem(const void *data, size_t len){
    char tmpl[]="/tmp/imgbcdXXXXXX";
    int fd=mkstemp(tmpl); if(fd<0) return NULL;
    size_t off=0; const char *p=(const char*)data;
    while(off<len){ ssize_t w=write(fd,p+off,len-off);
        if(w<=0){ close(fd); unlink(tmpl); return NULL; } off+=(size_t)w; }
    close(fd);
    img_bcd *b=img_bcd_open_file(tmpl);
    unlink(tmpl);
    return b;
}
int img_bcd_save(img_bcd *b, const char *path){
    if(!b||!path) return IMG_EINVAL;
    return hivex_commit(b->h, path, 0)==0 ? IMG_OK : IMG_EIO;
}
int img_bcd_save_mem(img_bcd *b, void **out, size_t *out_len){
    if(!b||!out||!out_len) return IMG_EINVAL;
    char tmpl[]="/tmp/imgbcdoutXXXXXX";
    int fd=mkstemp(tmpl); if(fd<0) return IMG_EIO;
    close(fd);
    if(hivex_commit(b->h, tmpl, 0)!=0){ unlink(tmpl); return IMG_EIO; }
    FILE *fp=fopen(tmpl,"rb"); if(!fp){ unlink(tmpl); return IMG_EIO; }
    fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
    void *buf=malloc((size_t)n);
    if(!buf || n<0 || fread(buf,1,(size_t)n,fp)!=(size_t)n){ free(buf); fclose(fp); unlink(tmpl); return IMG_EIO; }
    fclose(fp); unlink(tmpl);
    *out=buf; *out_len=(size_t)n; return IMG_OK;
}
void img_bcd_close(img_bcd *b){ if(b){ if(b->h) hivex_close(b->h); free(b); } }

/* ---------- 内部 ---------- */
static hive_node_h find_object(img_bcd *b, const char *guid){
    hive_node_h objs = hivex_node_get_child(b->h, hivex_root(b->h), "Objects");
    if(!objs) return 0;
    char want[64]; snprintf(want,sizeof want,"{%s}",guid);
    return hivex_node_get_child(b->h, objs, want);
}
static hive_node_h ensure_element(img_bcd *b, hive_node_h obj, const char *eid){
    hive_node_h els = hivex_node_get_child(b->h, obj, "Elements");
    if(!els) els = hivex_node_add_child(b->h, obj, "Elements");
    if(!els) return 0;
    hive_node_h el = hivex_node_get_child(b->h, els, eid);
    if(!el) el = hivex_node_add_child(b->h, els, eid);
    return el;
}
static int set_str_element(img_bcd *b, hive_node_h el, const char *s){
    size_t n=0; char *u=utf8_to_u16(s,&n);
    if(!u) return IMG_ENOMEM;
    hive_set_value v = { .key=(char*)"element", .t=hive_t_string, .len=n+2, .value=u };
    int rc = hivex_node_set_value(b->h, el, &v, 0);
    free(u);
    return rc==0 ? IMG_OK : IMG_EIO;
}

/* ---------- 语义层 API ---------- */
int img_bcd_set_bootmgr_name(img_bcd *b, const char *name){
    if(!b||!name) return IMG_EINVAL;
    hive_node_h bm = find_object(b, IMG_BCD_GUID_BOOTMGR);
    if(!bm) return IMG_EFORMAT;
    hive_node_h el = ensure_element(b, bm, IMG_BCD_EL_OSDEVICE);
    if(!el) return IMG_EIO;
    return set_str_element(b, el, name);
}

/* 写「GUID 列表」元素（REG_MULTI_SZ, hive_t=7）
 *
 * ★ 血泪教训：recoverysequence 的 format 段是 4 = GUID list，必须存成
 *   REG_MULTI_SZ（双 NUL 结尾），与 DisplayOrder/ToolsDisplayOrder 一样。
 *   我最初用 set_str_element 写成了 REG_SZ(t=1) 单串 —— bootmgr 按列表格式
 *   去读，发现类型不符 → 判定"BCD 不含有效操作系统信息" → 0xc0000098。
 *   实测对比：
 *     DisplayOrder     24000001  t=7  len=80（尾部 00 00）
 *     RecoverySequence 14000008  t=1  len=78（尾部单 00）← 错
 */
static int set_guid_list_element(img_bcd *b, hive_node_h el, const char *guid){
    char txt[64]; snprintf(txt,sizeof txt,"{%s}",guid);
    /* UTF-16LE + 双 NUL 终止符 */
    size_t n = (strlen(txt)+1)*2 + 2;
    char *buf = calloc(1,n);
    if(!buf) return IMG_ENOMEM;
    for(size_t i=0;i<strlen(txt);i++){
        buf[i*2]=(char)(unsigned char)txt[i];
        buf[i*2+1]=0;
    }
    hive_set_value sv = { .key=(char*)"element", .t=hive_t_multiple_strings,
                          .len=n, .value=buf };
    int rc = hivex_node_set_value(b->h, el, &sv, 0);
    free(buf);
    return rc==0 ? IMG_OK : IMG_EIO;
}

int img_bcd_set_recovery_sequence(img_bcd *b, const char *guid){
    if(!b||!guid) return IMG_EINVAL;
    hive_node_h bm = find_object(b, IMG_BCD_GUID_BOOTMGR);
    if(!bm) return IMG_EFORMAT;
    hive_node_h el = ensure_element(b, bm, IMG_BCD_EL_RECOVERYSEQ);
    if(!el) return IMG_EIO;
    return set_guid_list_element(b, el, guid);
}

int img_bcd_get_display_order_first(img_bcd *b, char *out, size_t out_len){
    if(!b||!out||out_len<40) return IMG_EINVAL;
    out[0]=0;
    hive_node_h bm = find_object(b, IMG_BCD_GUID_BOOTMGR);
    if(!bm) return IMG_EFORMAT;
    hive_node_h els = hivex_node_get_child(b->h,bm,"Elements");
    if(!els) return IMG_EFORMAT;
    hive_node_h el = hivex_node_get_child(b->h,els,IMG_BCD_EL_DISPLAYORDER);
    if(!el || hivex_node_nr_values(b->h,el)==0) return IMG_ENOENT;
    hive_value_h *vals = hivex_node_values(b->h, el);
    if(!vals) return IMG_EIO;
    hive_type t; size_t len=0;
    char *d = hivex_value_value(b->h, vals[0], &t, &len);
    free(vals);
    if(!d) return IMG_EIO;
    char *s = u16_to_utf8(d, len);
    free(d);
    if(!s) return IMG_ENOMEM;
    snprintf(out, out_len, "%s", s);
    free(s);
    return IMG_OK;
}

bool img_bcd_has_element(img_bcd *b, const char *guid, const char *eid){
    if(!b||!guid||!eid) return false;
    hive_node_h o = find_object(b,guid);
    if(!o) return false;
    hive_node_h els = hivex_node_get_child(b->h,o,"Elements");
    if(!els) return false;
    return hivex_node_get_child(b->h,els,eid)!=0;
}

char *img_bcd_get_string(img_bcd *b, const char *guid, const char *eid){
    if(!b||!guid||!eid) return NULL;
    hive_node_h o=find_object(b,guid); if(!o) return NULL;
    hive_node_h els=hivex_node_get_child(b->h,o,"Elements"); if(!els) return NULL;
    hive_node_h el=hivex_node_get_child(b->h,els,eid); if(!el) return NULL;
    if(hivex_node_nr_values(b->h,el)==0) return NULL;
    hive_value_h *vals=hivex_node_values(b->h,el);
    if(!vals) return NULL;
    hive_type t; size_t len=0;
    char *d=hivex_value_value(b->h,vals[0],&t,&len);
    free(vals);
    if(!d) return NULL;
    /* ★ 必须校验类型 —— 25000004(Timeout) 之类是 REG_BINARY(integer)，
     *   硬当 UTF-16 解码会产出  这类垃圾，调用方误以为"读到了字符串"。*/
    if(t != hive_t_string && t != hive_t_expand_string && t != hive_t_multiple_strings){
        free(d);
        return NULL;
    }
    char *s=u16_to_utf8(d,len); free(d); return s;
}

/* ============================================================
 * 对象复制 / 恢复条目构建
 *
 * ★ 策略：不手写 device 对象的 176 字节二进制（含 subtype/偏移/长度，
 *   错一个字段 Windows 就静默忽略），而是【从官方真值复制 + 改 WIM 路径】。
 *   官方 LTSC ISO 的 {7619dcc8}=ramdiskoptions、{7619dcc9}=WinPE loader
 *   本质就是"从 wim 启动"，把 WIM 路径换成 winre.wim 即为恢复条目。
 * ============================================================ */

/* 递归复制一个节点（名字+值+子节点）到目标 hive；返回新节点，0=失败 */
static hive_node_h copy_node_recursive(img_bcd *dst, hive_h *sh, hive_node_h sn,
                                       hive_node_h dparent){
    char *nm = hivex_node_name(sh, sn);
    if(!nm) return 0;
    hive_node_h dn = hivex_node_add_child(dst->h, dparent, nm);
    free(nm);
    if(!dn) return 0;

    /* 复制值 */
    hive_value_h *vals = hivex_node_values(sh, sn);
    if(vals){
        for(size_t i=0; vals[i]; i++){
            hive_type t; size_t len=0;
            char *data = hivex_value_value(sh, vals[i], &t, &len);
            char *key  = hivex_value_key(sh, vals[i]);
            if(data && key){
                hive_set_value sv = { .key=key, .t=t, .len=len, .value=data };
                hivex_node_set_value(dst->h, dn, &sv, 0);
            }
            free(data); free(key);
        }
        free(vals);
    }
    /* 递归子节点 */
    hive_node_h *kids = hivex_node_children(sh, sn);
    if(kids){
        for(size_t i=0; kids[i]; i++)
            copy_node_recursive(dst, sh, kids[i], dn);
        free(kids);
    }
    return dn;
}

/* 把 src 源里的一个对象（按 GUID 名）整体复制到 dst */
int img_bcd_copy_object(img_bcd *dst, const char *src_path, const char *guid){
    if(!dst||!src_path||!guid) return IMG_EINVAL;
    hive_h *sh = hivex_open(src_path, HIVEX_OPEN_VERBOSE);
    if(!sh) return IMG_ENOENT;
    hive_node_h sobjs = hivex_node_get_child(sh, hivex_root(sh), "Objects");
    char want[64]; snprintf(want,sizeof want,"{%s}",guid);
    hive_node_h so = sobjs ? hivex_node_get_child(sh, sobjs, want) : 0;
    if(!so){ hivex_close(sh); return IMG_ENOENT; }

    hive_node_h dobj = hivex_node_get_child(dst->h, hivex_root(dst->h), "Objects");
    if(!dobj) dobj = hivex_node_add_child(dst->h, hivex_root(dst->h), "Objects");
    hive_node_h dn = copy_node_recursive(dst, sh, so, dobj);
    hivex_close(sh);
    return dn ? IMG_OK : IMG_EIO;
}

/* 在指定对象下新增/覆盖一个字符串元素（UTF-8 入参） */
int img_bcd_set_string(img_bcd *b, const char *guid, const char *eid, const char *value){
    if(!b||!guid||!eid||!value) return IMG_EINVAL;
    hive_node_h o = find_object(b, guid);
    if(!o) return IMG_ENOENT;
    hive_node_h el = ensure_element(b, o, eid);
    if(!el) return IMG_EIO;
    return set_str_element(b, el, value);
}

/* 新建一个空对象（GUID 自由），返回 0 表示失败 */
int img_bcd_create_object(img_bcd *b, const char *guid){
    if(!b||!guid) return IMG_EINVAL;
    hive_node_h objs = hivex_node_get_child(b->h, hivex_root(b->h), "Objects");
    if(!objs) objs = hivex_node_add_child(b->h, hivex_root(b->h), "Objects");
    if(!objs) return IMG_EIO;
    char want[64]; snprintf(want,sizeof want,"{%s}",guid);
    if(hivex_node_get_child(b->h, objs, want)) return IMG_OK;   /* 已存在 */
    hive_node_h o = hivex_node_add_child(b->h, objs, want);
    return o ? IMG_OK : IMG_EIO;
}

/* 把对象加入 DisplayOrder 列表（幂等） */
int img_bcd_displayorder_add(img_bcd *b, const char *guid){
    if(!b||!guid) return IMG_EINVAL;
    hive_node_h bm = find_object(b, IMG_BCD_GUID_BOOTMGR);
    if(!bm) return IMG_ENOENT;
    hive_node_h el = ensure_element(b, bm, IMG_BCD_EL_DISPLAYORDER);
    if(!el) return IMG_EIO;
    /* 读现有列表 */
    char cur[2048]; cur[0]=0;
    hive_value_h *vals = hivex_node_values(b->h, el);
    if(vals && vals[0]){
        hive_type t; size_t len=0;
        char *d = hivex_value_value(b->h, vals[0], &t, &len);
        if(d){
            char *s = u16_to_utf8(d, len);
            free(d);
            if(s){ snprintf(cur,sizeof cur,"%s",s); free(s); }
        }
    }
    if(vals) free(vals);
    char me[64]; snprintf(me,sizeof me,"{%s}",guid);
    if(strstr(cur, me)) return IMG_OK;      /* 已在列表 */
    /* 拼接（多串用 \0 分隔，末尾再加一个 \0）*/
    size_t n=0; char *u=utf8_to_u16(cur[0]?cur:me, &n);
    if(!u) return IMG_ENOMEM;
    /* 若已有旧项，需要把新项接到后面 —— 这里简化：两段分别转码再拼 */
    size_t n1=0,n2=0;
    char *ua = utf8_to_u16(cur, &n1);         /* 旧（可能带尾 \0） */
    char *ub = utf8_to_u16(me, &n2);          /* 新 */
    free(u);
    if(!ua||!ub){ free(ua); free(ub); return IMG_ENOMEM; }
    size_t total = n1 + n2 + 2;
    char *buf = calloc(1, total);
    if(!buf){ free(ua); free(ub); return IMG_ENOMEM; }
    memcpy(buf, ua, n1);
    memcpy(buf+n1, ub, n2);
    free(ua); free(ub);
    hive_set_value sv = { .key=(char*)"element", .t=hive_t_multiple_strings,
                          .len=total, .value=buf };
    int rc = hivex_node_set_value(b->h, el, &sv, 0);
    free(buf);
    return rc==0 ? IMG_OK : IMG_EIO;
}

/* ============================================================
 * device 对象里的 WIM 路径补丁
 *
 * 实测结构（176 字节的 ramdisk device 对象）：
 *   [0..15]   被引用对象 GUID（RAMDisk options）
 *   [20..23]  类型（1=ramdisk带路径, 6=本地设备）
 *   [24..27]  ★ blob总长 - 16  （ISO 176→160；Xlite 88→72，两个真值均吻合）
 *   [56..59]  ★ 字符串偏移（恒 120）
 *   [120..]   UTF-16LE 路径 + NUL
 *
 * 改路径时必须同步 [24..27]，否则引导器会按旧长度截断/越界读。
 * ============================================================ */
static int u16len(const char *s){ int n=0; while(s[n]) n++; return n; }

/* 在 blob 里扫描 UTF-16 ASCII 串（我们的路径都是 ASCII），返回起始偏移，0=没找到 */
static size_t find_u16_ascii(const unsigned char *d, size_t len){
    /* 从后往前找最长的连续可打印 UTF-16（低字节可打印、高字节0）段 */
    size_t best=0, bestlen=0, i=0;
    while(i+1 < len){
        size_t start=i;
        while(i+1 < len){
            unsigned lo=d[i], hi=d[i+1];
            bool ch = (lo>=0x20 && lo<0x7f && hi==0);
            if(!ch) break;
            i+=2;
        }
        size_t seg = i - start;
        if(seg > bestlen && seg >= 4){ bestlen=seg; best=start; }
        i += 2;   /* 跳过该段（含其后字节），继续找 */
        if(bestlen>0 && i<=best) break;
    }
    return best;
}

int img_bcd_patch_device_wimpath(img_bcd *b, const char *guid, const char *eid,
                                 const char *new_path){
    if(!b||!guid||!eid||!new_path) return IMG_EINVAL;
    hive_node_h o = find_object(b, guid);
    if(!o) return IMG_ENOENT;
    hive_node_h els = hivex_node_get_child(b->h, o, "Elements");
    hive_node_h el  = els ? hivex_node_get_child(b->h, els, eid) : 0;
    if(!el || hivex_node_nr_values(b->h, el)==0) return IMG_ENOENT;

    hive_value_h *vals = hivex_node_values(b->h, el);
    hive_type t; size_t len=0;
    char *d = hivex_value_value(b->h, vals[0], &t, &len);
    free(vals);
    if(!d || t != hive_t_binary){ free(d); return IMG_EFORMAT; }
    if(len < 64 || len > 4096){ free(d); return IMG_ECORRUPT; }

    /* 结构守卫：[24] 必须等于 blob长-16 —— 两个真值（176/160、88/72）均吻合 */
    uint32_t sz_field; memcpy(&sz_field, d+24, 4);
    if((size_t)sz_field + 16 != len){ free(d); return IMG_ECORRUPT; }

    /* ★ 定位原串：实测它在 blob【末尾】，前面有一段(120..139)是零填充，
     *   偏移字段 [56]=1 并【不是】串偏移（我曾误判并因此破坏过结构）。 */
    size_t soff = find_u16_ascii((const unsigned char*)d, len);
    if(soff == 0){ free(d); return IMG_ECORRUPT; }   /* 结构不符就不动它 */

    int chars = u16len(new_path);
    size_t newstr = (size_t)chars*2 + 2;             /* +NUL */
    size_t newlen = soff + newstr;
    if(newlen < len) newlen = len;                   /* 只增不减，保尾部未知字段 */
    size_t pad = (4 - (newlen % 4)) % 4; newlen += pad;

    char *nd = calloc(1, newlen);
    if(!nd){ free(d); return IMG_ENOMEM; }
    memcpy(nd, d, len < newlen ? len : newlen);
    free(d);

    /* 只覆盖字符串区，其余字节保持原样 */
    memset(nd + soff, 0, newlen - soff);
    for(int i=0;i<chars;i++){
        nd[soff + i*2]   = (char)(new_path[i] & 0xFF);
        nd[soff + i*2+1] = 0;
    }
    /* 同步 [24] = 总长-16（唯一必须改的结构字段）*/
    uint32_t nsz = (uint32_t)(newlen - 16);
    memcpy(nd+24, &nsz, 4);

    hive_set_value sv = { .key=(char*)"element", .t=hive_t_binary,
                          .len=newlen, .value=nd };
    int rc = hivex_node_set_value(b->h, el, &sv, 0);
    free(nd);
    return rc==0 ? IMG_OK : IMG_EIO;
}

/* 读出 device 对象里的路径（校验结构用） */
int img_bcd_get_device_wimpath(img_bcd *b, const char *guid, const char *eid,
                               char *out, size_t outlen){
    if(!b||!guid||!eid||!out) return IMG_EINVAL;
    out[0]=0;
    hive_node_h o=find_object(b,guid); if(!o) return IMG_ENOENT;
    hive_node_h els=hivex_node_get_child(b->h,o,"Elements");
    hive_node_h el =els?hivex_node_get_child(b->h,els,eid):0;
    if(!el||hivex_node_nr_values(b->h,el)==0) return IMG_ENOENT;
    hive_value_h *vals=hivex_node_values(b->h,el);
    hive_type t; size_t len=0;
    char *d=hivex_value_value(b->h,vals[0],&t,&len);
    free(vals);
    if(!d||t!=hive_t_binary||len<124){ free(d); return IMG_EFORMAT; }
    size_t off = find_u16_ascii((const unsigned char*)d, len);
    if(off==0){ free(d); return IMG_ECORRUPT; }
    char tmp[512]; int j=0;
    for(size_t i=off; i+1<len && j<(int)sizeof tmp-1; i+=2){
        unsigned c = (unsigned char)d[i] | ((unsigned char)d[i+1]<<8);
        if(c==0) break;
        tmp[j++] = c<128 ? (char)c : '?';
    }
    tmp[j]=0; free(d);
    snprintf(out,outlen,"%s",tmp);
    return IMG_OK;
}

/* ============================================================
 * 构造「分区设备」对象（88 字节）
 *
 * 格式（实测：与 diskA 的 GPT 磁盘 GUID 逐字节吻合）：
 *   [0..15]  16B  GUID = 0                （partition 设备没有顶层对象 GUID）
 *   [16..19] 4B   type  = 6
 *   [20..23] 4B   0
 *   [24..27] 4B   size  = 72
 *   [28..31] 4B   0
 *   [32..47] 16B  分区唯一 GUID（LE 混合端序：前3段小端，后2段按字节）
 *   [48..55] 8B   0
 *   [56..71] 16B  磁盘 GUID    （同上编码）
 *   [72..87] 16B  0
 *
 * ★ 为什么需要它：ramdiskoptions 的 ramdisksdidevice(31000003) 在官方 ISO 里是
 *   type=5「本地设备」(GUID 全零)，含义是"跟着 BCD 所在分区"。ISO 的 BCD/boot.sdi/
 *   boot.wim 全在同一分区所以能用；而我们 winre.wim 在【独立的恢复分区】上，
 *   用「本地设备」会让 bootmgr 去 ESP 找 → 找不到 → 0xc0000098。
 * ============================================================ */
static bool guid_text_to_bytes(const char *txt, uint8_t out[16]){
    /* 去掉横线取 32 个十六进制字符，再按 GUID 内存布局重排：
     *   文本 AABBCCDD-EEFF-GGHH-IIJJ-KKLLMMNNOOPP
     *   内存 DD CC BB AA FF EE HH GG II JJ KK LL MM NN OO PP
     *   （前 3 段小端，后 2 段按原字节序）*/
    if(!txt) return false;
    char hex[64]; int n=0;
    for(const char *p=txt; *p; p++){
        if(*p=='-') continue;
        if(n>=(int)sizeof hex-1) return false;
        hex[n++]=*p;
    }
    hex[n]=0;
    if(n!=32) return false;
    uint8_t b[16];
    for(int i=0;i<16;i++){
        char t[3]={hex[i*2],hex[i*2+1],0};
        char *end=NULL;
        long v=strtol(t,&end,16);
        if(!end || *end) return false;
        b[i]=(uint8_t)v;
    }
    out[0]=b[3]; out[1]=b[2]; out[2]=b[1]; out[3]=b[0];
    out[4]=b[5]; out[5]=b[4];
    out[6]=b[7]; out[7]=b[6];
    for(int i=8;i<16;i++) out[i]=b[i];
    return true;
}

int img_bcd_make_partition_device(const char *part_guid, const char *disk_guid,
                                  uint8_t out[88]){
    if(!part_guid||!disk_guid||!out) return IMG_EINVAL;
    memset(out,0,88);
    uint8_t pg[16], dg[16];
    if(!guid_text_to_bytes(part_guid,pg)) return IMG_EINVAL;
    if(!guid_text_to_bytes(disk_guid,dg)) return IMG_EINVAL;
    uint32_t type=6, size=72;
    memcpy(out+16,&type,4);
    memcpy(out+24,&size,4);
    memcpy(out+32,pg,16);
    memcpy(out+56,dg,16);
    return IMG_OK;
}

int img_bcd_set_element_binary(img_bcd *b, const char *guid, const char *eid,
                               const void *data, size_t len){
    if(!b||!guid||!eid||!data) return IMG_EINVAL;
    hive_node_h o=find_object(b,guid); if(!o) return IMG_ENOENT;
    hive_node_h el=ensure_element(b,o,eid); if(!el) return IMG_EIO;
    hive_set_value sv={ .key=(char*)"element", .t=hive_t_binary, .len=len, .value=(void*)data };
    return hivex_node_set_value(b->h, el, &sv, 0)==0 ? IMG_OK : IMG_EIO;
}

/* 把 ramdiskoptions 的 ramdisksdidevice 指到指定分区（而不是"本地设备"）*/
int img_bcd_set_ramdisksdi_partition(img_bcd *b, const char *ramdisk_guid,
                                     const char *part_guid, const char *disk_guid){
    uint8_t dev[88];
    int rc=img_bcd_make_partition_device(part_guid,disk_guid,dev);
    if(rc) return rc;
    return img_bcd_set_element_binary(b, ramdisk_guid, "31000003", dev, sizeof dev);
}

/* 读二进制元素（调用方提供的缓冲；*out_len 返回实际长度）*/
int img_bcd_get_element_binary(img_bcd *b, const char *guid, const char *eid,
                               void *out, size_t cap, size_t *out_len){
    if(!b||!guid||!eid||!out||!out_len) return IMG_EINVAL;
    *out_len=0;
    hive_node_h o=find_object(b,guid); if(!o) return IMG_ENOENT;
    hive_node_h els=hivex_node_get_child(b->h,o,"Elements");
    hive_node_h el =els?hivex_node_get_child(b->h,els,eid):0;
    if(!el||hivex_node_nr_values(b->h,el)==0) return IMG_ENOENT;
    hive_value_h *vs=hivex_node_values(b->h,el);
    hive_type t; size_t ln=0;
    char *d=hivex_value_value(b->h, vs[0], &t, &ln);
    free(vs);
    if(!d) return IMG_EIO;
    if(t!=hive_t_binary){ free(d); return IMG_EFORMAT; }
    if(ln>cap){ free(d); return IMG_EFULL; }
    memcpy(out,d,ln); *out_len=ln;
    free(d);
    return IMG_OK;
}
