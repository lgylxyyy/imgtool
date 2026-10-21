#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_bcd.c — BCD 语义层（hive 底层全部交给 libhivex）
 * 容器格式与元素 ID 规律见 reference/BCD-FORMAT.md */
#include "img_bcd.h"
#include "img_event.h"
#include <hivex.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
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
    if(!path) return NULL;                  /* ★ 实测：NULL 传进 hivex_open 会段错误 */
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
    img_span sp;                        /* 里程碑：BCD 写入（含 hivex 的 bin 重建/重平衡）*/
    img_span_prep(&sp,"bcd","save","写入 BCD");
    img_span_kv_str(&sp,"path",path);
    img_span_begin(&sp);
    int rc = hivex_commit(b->h, path, 0)==0 ? IMG_OK : IMG_EIO;
    img_span_end(&sp, rc);
    return rc;
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
    /* ★ 目标已有同 GUID 对象时先删再拷（模板覆盖语义）。
     *   否则 hivex_node_add_child 会因重名失败 —— 实测：Win11 的 BCD 自带
     *   ISO 的 ramdisk/WinPE 对象，恢复条目构建会在这里断掉。 */
    hive_node_h old = hivex_node_get_child(dst->h, dobj, want);
    if(old) hivex_node_delete_child(dst->h, old);
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

/* 把某个 device 对象的【分区 GUID】与【磁盘 GUID】替换成新盘的值
 *
 * 场景：参考镜像 BCD 的 OS 加载器里，device 对象写死了源盘的
 *   分区唯一 GUID 与 GPT 磁盘 GUID；换盘/重建分区后必须改，
 *   否则 bootmgr 解析不到设备 → 0xc0000098。
 *
 * 88 字节结构（见 img_bcd_make_partition_device 注释）：
 *   [32..47] 分区 GUID   [56..71] 磁盘 GUID
 * 只改这两段，其余字节原样保留。 */
int img_bcd_patch_device_guids(img_bcd *b, const char *obj_guid, const char *eid,
                               const char *part_guid, const char *disk_guid){
    if(!b||!obj_guid||!eid||!part_guid||!disk_guid) return IMG_EINVAL;
    hive_node_h o=find_object(b,obj_guid); if(!o) return IMG_ENOENT;
    hive_node_h els=hivex_node_get_child(b->h,o,"Elements");
    hive_node_h el =els?hivex_node_get_child(b->h,els,eid):0;
    if(!el||hivex_node_nr_values(b->h,el)==0) return IMG_ENOENT;
    hive_value_h *vals=hivex_node_values(b->h,el);
    hive_type t; size_t len=0;
    char *d=hivex_value_value(b->h,vals[0],&t,&len);
    free(vals);
    if(!d) return IMG_EIO;
    if(t!=hive_t_binary || len<88){ free(d); return IMG_ECORRUPT; }

    uint8_t pg[16], dg[16];
    /* 复用 make_partition_device 里的 GUID 解析：先造一份再取它的两段 */
    uint8_t tmp[88];
    if(img_bcd_make_partition_device(part_guid,disk_guid,tmp)!=IMG_OK){ free(d); return IMG_EINVAL; }
    memcpy(pg, tmp+32, 16);
    memcpy(dg, tmp+56, 16);

    memcpy(d+32, pg, 16);
    memcpy(d+56, dg, 16);
    hive_set_value sv={ .key=(char*)"element", .t=hive_t_binary, .len=len, .value=d };
    int rc=hivex_node_set_value(b->h, el, &sv, 0);
    free(d);
    return rc==0 ? IMG_OK : IMG_EIO;
}

/* ★ MBR/BIOS 设备对象的命门字节（2026-10-18 字节级实测：Win11 BCD @13560 区）
 *   blob[0..3]   = 0
 *   blob[4..11]  = 分区【字节偏移】LE64（= LBA × 512；bootmgr 找分区靠它）
 *   blob[24..27] = 类型标记 01 00 00 00
 *   blob[28..31] = 磁盘签名 LE32（MBR @0x1B8 的 4 字节；bootmgr 找盘靠它）
 * 与 BCD-SYS update_device.sh 改的两个字段完全对应（它是 Linux 版 bcdboot 的做法）。
 * GPT/UEFI 版设备对象是另一布局（88 字节 GUID 版），见 patch_device_guids。 */
/* ★ 修正（2026-10-18 实测 blob 后）：MBR 设备对象【复用 GPT 的 88 字节 type-6 布局】：
 *   [16..19] = 6（类型标记，与 GPT 分区设备相同）
 *   [32..39] = 分区【字节偏移】LE64（GPT 的"分区GUID槽"前8字节放偏移）
 *   [56..59] = 磁盘签名 LE32     （GPT 的"磁盘GUID槽"前4字节放签名）
 * 与 BCD-SYS update_device.sh 改的两个字段完全对应。 */
#define MBR_OFF 32
#define MBR_SIG 56
static int mbr_el_node(img_bcd *b,const char*guid,const char*eid,hive_node_h*el_out){
    if(!b||!guid||!eid||!el_out) return IMG_EINVAL;
    hive_node_h o=find_object(b,guid); if(!o) return IMG_ENOENT;
    hive_node_h els=hivex_node_get_child(b->h,o,"Elements");
    hive_node_h el =els?hivex_node_get_child(b->h,els,eid):0;
    if(!el||hivex_node_nr_values(b->h,el)==0) return IMG_ENOENT;
    *el_out=el; return IMG_OK;
}
int img_bcd_get_mbr_binding(img_bcd *b,const char*guid,const char*eid,
                            uint32_t *sig_out,uint64_t *off_out){
    hive_node_h el; int rc=mbr_el_node(b,guid,eid,&el); if(rc) return rc;
    hive_value_h *vs=hivex_node_values(b->h,el);
    hive_type t; size_t len=0;
    char *d=hivex_value_value(b->h,vs[0],&t,&len); free(vs);
    if(!d) return IMG_EIO;
    if(t!=hive_t_binary||len<32){ free(d); return IMG_EFORMAT; }
    unsigned ty; memcpy(&ty,d+16,4);
    if(ty!=6){ free(d); return IMG_EFORMAT; }   /* 不是「分区设备」 */
    uint64_t off; uint32_t sig;
    memcpy(&off, d+MBR_OFF, 8);
    memcpy(&sig, d+MBR_SIG, 4);
    free(d);
    if(off_out) *off_out=off;
    if(sig_out) *sig_out=sig;
    return IMG_OK;
}
int img_bcd_set_mbr_binding(img_bcd *b,const char*guid,const char*eid,
                            uint32_t sig,uint64_t off){
    hive_node_h el; int rc=mbr_el_node(b,guid,eid,&el); if(rc) return rc;
    hive_value_h *vs=hivex_node_values(b->h,el);
    hive_type t; size_t len=0;
    char *d=hivex_value_value(b->h,vs[0],&t,&len); free(vs);
    if(!d) return IMG_EIO;
    if(t!=hive_t_binary||len<32){ free(d); return IMG_EFORMAT; }
    unsigned ty2; memcpy(&ty2,d+16,4);
    if(ty2!=6){ free(d); return IMG_EFORMAT; }
    memcpy(d+MBR_OFF, &off, 8);
    memcpy(d+MBR_SIG, &sig, 4);
    hive_set_value sv={ .key=(char*)"element", .t=hive_t_binary, .len=len, .value=d };
    rc=hivex_node_set_value(b->h, el, &sv, 0);
    free(d);
    return rc==0?IMG_OK:IMG_EIO;
}

/* ---------- 全对象扫描（见 img_bcd.h 注释） ----------
 * 为什么按 hive 结构扫、而不是在整个文件里裸替换 4 字节签名：
 *   ① 裸替换会误伤恰好等于签名的其它数据（概率低但不是零）；
 *   ② 参考件里通常有 5 个 type=6 设备（bootmgr/memtest→SR，
 *      resume/OSLoader/OSDevice→系统分区），只改已知三处会漏网；
 *   ③ hivex 重写会把旧 cell 留在空闲区，裸替换会把"垃圾副本"也改掉。
 * 这里只认 88 字节分区设备结构（offset 16 = 6），只改 [32..39]/[56..59]。 */
typedef int (*mbr_scan_cb)(void *ud, const char *obj_guid, const char *eid,
                           hive_node_h el, const char *vkey,
                           char *blob, size_t len);
static int scan_mbr_devices(img_bcd *b, mbr_scan_cb cb, void *ud){
    if(!b||!cb) return IMG_EINVAL;
    hive_node_h objs=hivex_node_get_child(b->h, hivex_root(b->h), "Objects");
    if(!objs) return IMG_ENOENT;
    hive_node_h *oc=hivex_node_children(b->h, objs);
    if(!oc) return IMG_EIO;
    int rc=IMG_OK;
    for(size_t i=0; oc[i] && rc==IMG_OK; i++){
        char *nm=hivex_node_name(b->h, oc[i]);
        if(!nm) continue;
        hive_node_h els=hivex_node_get_child(b->h, oc[i], "Elements");
        hive_node_h *ec=els?hivex_node_children(b->h, els):NULL;
        if(ec){
            for(size_t j=0; ec[j] && rc==IMG_OK; j++){
                char *eid=hivex_node_name(b->h, ec[j]);
                if(!eid) continue;
                hive_value_h *vs=hivex_node_values(b->h, ec[j]);
                if(vs && vs[0]){
                    hive_type t; size_t len=0;
                    char *d=hivex_value_value(b->h, vs[0], &t, &len);
                    char *vk=hivex_value_key(b->h, vs[0]);
                    if(d && vk && t==hive_t_binary && len>=64){
                        unsigned ty; memcpy(&ty, d+16, 4);
                        if(ty==6){
                            char g[64]; snprintf(g,sizeof g,"%s",nm);
                            char *p=g; if(p[0]=='{') p++;
                            char *c=strchr(p,'}'); if(c) *c=0;
                            rc=cb(ud, p, eid, ec[j], vk, d, len);
                        }
                    }
                    free(vk); free(d);
                }
                free(vs);
                free(eid);
            }
            free(ec);
        }
        free(nm);
    }
    free(oc);
    return rc;
}
typedef struct {
    uint32_t old_sig, new_sig;
    const img_bcd_off_map *maps; size_t n_maps;
    img_bcd *b; int patched;
} mbr_patch_ud;
static int mbr_patch_cb(void *ud, const char *obj_guid, const char *eid,
                        hive_node_h el, const char *vkey, char *blob, size_t len){
    (void)obj_guid; (void)eid;
    mbr_patch_ud *p=(mbr_patch_ud*)ud;
    uint32_t sig; uint64_t off;
    memcpy(&off, blob+MBR_OFF, 8);
    memcpy(&sig, blob+MBR_SIG, 4);
    uint32_t nsig=sig; uint64_t noff=off;
    if(p->new_sig && (p->old_sig==0 || sig==p->old_sig)) nsig=p->new_sig;
    for(size_t i=0;i<p->n_maps;i++)
        if(off==p->maps[i].old_off){ noff=p->maps[i].new_off; break; }
    if(nsig==sig && noff==off) return IMG_OK;   /* 已经是目标值：不重写 */
    memcpy(blob+MBR_SIG, &nsig, 4);
    memcpy(blob+MBR_OFF, &noff, 8);
    hive_set_value sv={ .key=(char*)vkey, .t=hive_t_binary, .len=len, .value=blob };
    if(hivex_node_set_value(p->b->h, el, &sv, 0)!=0) return IMG_EIO;
    p->patched++;
    return IMG_OK;
}
int img_bcd_patch_all_mbr_bindings(img_bcd *b,
        uint32_t old_sig, uint32_t new_sig,
        const img_bcd_off_map *maps, size_t n_maps, int *patched){
    if(!b) return IMG_EINVAL;
    if(n_maps && !maps) return IMG_EINVAL;
    mbr_patch_ud ud={ .old_sig=old_sig, .new_sig=new_sig,
                      .maps=maps, .n_maps=n_maps, .b=b, .patched=0 };
    int rc=scan_mbr_devices(b, mbr_patch_cb, &ud);
    if(patched) *patched=ud.patched;
    return rc;
}
typedef struct { img_bcd_mbr_visit_fn fn; void *ud; } mbr_visit_ud;
static int mbr_visit_cb(void *ud, const char *obj_guid, const char *eid,
                        hive_node_h el, const char *vkey, char *blob, size_t len){
    (void)el; (void)vkey; (void)len;
    mbr_visit_ud *v=(mbr_visit_ud*)ud;
    uint32_t sig; uint64_t off;
    memcpy(&off, blob+MBR_OFF, 8);
    memcpy(&sig, blob+MBR_SIG, 4);
    return v->fn(v->ud, obj_guid, eid, sig, off);
}
int img_bcd_foreach_mbr_binding(img_bcd *b, img_bcd_mbr_visit_fn fn, void *ud){
    if(!b||!fn) return IMG_EINVAL;
    mbr_visit_ud v={ .fn=fn, .ud=ud };
    return scan_mbr_devices(b, mbr_visit_cb, &v);
}


/* 内存 GUID 布局 → 文本（guid_text_to_bytes 的逆运算） */
static void guid_bytes_to_text(const uint8_t *b, char *out /* ≥37 字节 */){
    snprintf(out, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[3],b[2],b[1],b[0], b[5],b[4], b[7],b[6], b[8],b[9],
        b[10],b[11],b[12],b[13],b[14],b[15]);
}

/* 读 device 元素里的分区/磁盘 GUID（文本）。非「分区设备」(type≠6) → IMG_EFORMAT */
int img_bcd_get_device_guids(img_bcd *b, const char *obj_guid, const char *eid,
                             char *part_out, char *disk_out){
    if(!b||!obj_guid||!eid||!part_out||!disk_out) return IMG_EINVAL;
    part_out[0]='\0'; disk_out[0]='\0';
    hive_node_h o=find_object(b,obj_guid); if(!o) return IMG_ENOENT;
    hive_node_h els=hivex_node_get_child(b->h,o,"Elements");
    hive_node_h el =els?hivex_node_get_child(b->h,els,eid):0;
    if(!el||hivex_node_nr_values(b->h,el)==0) return IMG_ENOENT;
    hive_value_h *vs=hivex_node_values(b->h,el);
    hive_type t; size_t len=0;
    char *d=hivex_value_value(b->h,vs[0],&t,&len);
    free(vs);
    if(!d) return IMG_EIO;
    if(t!=hive_t_binary || len<88){ free(d); return IMG_ECORRUPT; }
    unsigned ty; memcpy(&ty,d+16,4);
    if(ty!=6){ free(d); return IMG_EFORMAT; }   /* 只要「分区设备」 */
    guid_bytes_to_text((const uint8_t*)d+32, part_out);
    guid_bytes_to_text((const uint8_t*)d+56, disk_out);
    free(d);
    return IMG_OK;
}

/* ---------- 全对象 GUID 批量映射（换盘必做） ---------- */
typedef struct {
    const char *old_os, *new_os, *old_esp, *new_esp;
    const char *old_rec, *new_rec, *old_disk, *new_disk;
    img_bcd *b; int patched;
} guid_patch_ud;
static int guid_patch_cb(void *ud, const char *obj_guid, const char *eid,
                         hive_node_h el, const char *vkey, char *blob, size_t len){
    (void)obj_guid; (void)eid;
    guid_patch_ud *p=(guid_patch_ud*)ud;
    if(len < 88) return IMG_OK;
    char pg[40], dg[40];
    guid_bytes_to_text((const uint8_t*)blob+32, pg);
    guid_bytes_to_text((const uint8_t*)blob+56, dg);
    const char *np=NULL, *nd=NULL;
    if(p->old_os  && !strcmp(pg,p->old_os))  np=p->new_os;
    else if(p->old_esp && !strcmp(pg,p->old_esp)) np=p->new_esp;
    else if(p->old_rec && !strcmp(pg,p->old_rec)) np=p->new_rec;
    if(p->old_disk && !strcmp(dg,p->old_disk)) nd=p->new_disk;
    if(!np && !nd) return IMG_OK;                    /* 不属于本盘：不动 */
    uint8_t tmp[88];
    if(img_bcd_make_partition_device(np?np:pg, nd?nd:dg, tmp)!=IMG_OK) return IMG_EINVAL;
    memcpy(blob+32, tmp+32, 16);
    memcpy(blob+56, tmp+56, 16);
    hive_set_value sv={ .key=(char*)vkey, .t=hive_t_binary, .len=len, .value=blob };
    if(hivex_node_set_value(p->b->h, el, &sv, 0)!=0) return IMG_EIO;
    p->patched++;
    return IMG_OK;
}
int img_bcd_patch_all_guid_bindings(img_bcd *b,
        const char *old_os,  const char *new_os,
        const char *old_esp, const char *new_esp,
        const char *old_rec, const char *new_rec,
        const char *old_disk, const char *new_disk,
        int *patched){
    if(!b) return IMG_EINVAL;
    if(patched) *patched=0;
    guid_patch_ud ud={ .old_os=old_os, .new_os=new_os, .old_esp=old_esp, .new_esp=new_esp,
                       .old_rec=old_rec, .new_rec=new_rec, .old_disk=old_disk,
                       .new_disk=new_disk, .b=b, .patched=0 };
    int rc=scan_mbr_devices(b, guid_patch_cb, &ud);
    if(patched) *patched=ud.patched;
    return rc;
}

/* 枚举 Objects 下所有对象 GUID —— 换盘重建时要遍历全部 device 对象 */
int img_bcd_list_objects(img_bcd *b, char ***out_guids, size_t *out_n){
    if(!b||!out_guids||!out_n) return IMG_EINVAL;
    *out_guids=NULL; *out_n=0;
    hive_node_h objs=hivex_node_get_child(b->h, hivex_root(b->h), "Objects");
    if(!objs) return IMG_ENOENT;
    hive_node_h *ch=hivex_node_children(b->h, objs);
    if(!ch) return IMG_EIO;
    size_t n=0; while(ch[n]) n++;
    char **arr=calloc(n?n:1, sizeof *arr);
    if(!arr){ free(ch); return IMG_ENOMEM; }
    size_t k=0;
    for(size_t i=0;i<n;i++){
        char *nm=hivex_node_name(b->h, ch[i]);
        if(!nm) continue;
        const char *s=nm; if(s[0]=='{') s++;
        size_t L=strlen(s);
        char *c=malloc(L+1);
        if(c){ memcpy(c,s,L+1); if(L && c[L-1]=='}') c[L-1]='\0'; arr[k++]=c; }
        free(nm);
    }
    free(ch);
    *out_guids=arr; *out_n=k;
    return IMG_OK;
}
void img_bcd_free_object_list(char **guids, size_t n){
    if(!guids) return;
    for(size_t i=0;i<n;i++) free(guids[i]);
    free(guids);
}

/* ================================================================== */
/* 一站式换盘：参考 BCD → 新盘 GUID 的可引导 BCD（CLI 与命令表共用）   */
/* ================================================================== */
static void remap_trim_guid(const char *in, char *out, size_t cap){
    if(!out||!cap) return;
    out[0]=0;
    if(!in) return;
    const char *p=in;
    while(*p==' '||*p=='{') p++;
    size_t n=0;
    while(p[n] && p[n]!='}' && n+1<cap){ out[n]=p[n]; n++; }
    out[n]=0;
}

int img_bcd_remap(img_bcd *b, const img_bcd_remap_req_t *req,
                  int *hits, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(hits) *hits=0;
    if(!b || !req || !req->sys_guid || !req->disk_guid) return IMG_EINVAL;

    /* GUID 先校验：非法就别开工（旧 imggenbcd 会"改了个寂寞还返回 0"）*/
    unsigned char probe[88];
    if(img_bcd_make_partition_device(req->sys_guid, req->disk_guid, probe)!=IMG_OK){
        if(err) snprintf(err,errcap,"系统分区 GUID 或磁盘 GUID 非法");
        return IMG_EINVAL;
    }
    if(req->esp_guid && img_bcd_make_partition_device(req->esp_guid, req->disk_guid, probe)!=IMG_OK){
        if(err) snprintf(err,errcap,"ESP 分区 GUID 非法");
        return IMG_EINVAL;
    }
    if(req->new_rec_guid && img_bcd_make_partition_device(req->new_rec_guid, req->disk_guid, probe)!=IMG_OK){
        if(err) snprintf(err,errcap,"恢复分区 GUID 非法");
        return IMG_EINVAL;
    }

    /* 参考件里的加载器 GUID（DisplayOrder 第一条）；没有就只能做 ESP 重映射 */
    char order[96]="", obj[80]="";
    bool have_do = (img_bcd_get_display_order_first(b, order, sizeof order)==IMG_OK);
    if(have_do) remap_trim_guid(order, obj, sizeof obj);
    if(!have_do && !req->esp_guid){
        if(err) snprintf(err,errcap,"参考 BCD 没有 DisplayOrder，且没给 ESP GUID —— 无从下手");
        return IMG_EFORMAT;
    }

    /* ★ 必须【改之前】先记下参考盘的旧 GUID：改完就读不到了 */
    char old_os[40]={0}, old_esp[40]={0}, old_disk[40]={0}, td[40]={0};
    if(obj[0]){
        if(img_bcd_get_device_guids(b, obj, IMG_BCD_EL_WIM_DEVICE, old_os, old_disk)!=IMG_OK)
            old_os[0]=0;
        if(!old_os[0])
            img_bcd_get_device_guids(b, obj, IMG_BCD_EL_APP_DEVICE, old_os, old_disk);
    }
    if(req->esp_guid){
        img_bcd_get_device_guids(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_APP_DEVICE, old_esp, td);
        if(!old_disk[0] && td[0]) snprintf(old_disk,sizeof old_disk,"%s",td);
    }
    if(!old_os[0] && !old_esp[0]){
        if(err) snprintf(err,errcap,"参考件里找不到可映射的分区设备（不是 UEFI BCD？）");
        return IMG_EFORMAT;
    }

    int hit=0;
    int rc=img_bcd_patch_all_guid_bindings(b,
            old_os[0]?old_os:NULL, req->sys_guid,
            (req->esp_guid && old_esp[0])?old_esp:NULL, req->esp_guid,
            req->old_rec_guid, req->new_rec_guid,
            old_disk[0]?old_disk:NULL, req->disk_guid, &hit);
    if(rc!=IMG_OK){
        if(err) snprintf(err,errcap,"全对象映射失败：%s", img_strerror(rc));
        return rc;
    }
    /* 兜底：参考件结构不认识时，至少把两个加载器设备改对 */
    if(hit==0 && obj[0]){
        int rc1=img_bcd_patch_device_guids(b,obj,IMG_BCD_EL_WIM_DEVICE,req->sys_guid,req->disk_guid);
        int rc2=img_bcd_patch_device_guids(b,obj,IMG_BCD_EL_APP_DEVICE,req->sys_guid,req->disk_guid);
        if(rc1==IMG_OK) hit++;
        if(rc2==IMG_OK) hit++;
    }
    if(hits) *hits=hit;
    if(hit==0){
        if(err) snprintf(err,errcap,"一处也没改到（参考件 GUID 与本盘不匹配？）");
        return IMG_ENOENT;
    }
    return IMG_OK;
}

/* ================================================================== */
/* 从模板构建 BCD（不依赖"参考盘旧 GUID"）                              */
/* ================================================================== */
/* 为什么需要它：Win7/8 光盘里的 BCD-Template 没有任何分区绑定（实测 total=0），
 * 而 boot.repair / imggenbcd 都要"先读旧值再映射" —— 对模板无从下手。
 * 这里改成"直接写"：
 *   · 设备对象：GPT 用 img_bcd_make_partition_device() 现造；
 *               MBR 先借一份模板里的设备 blob（保证类型字段正确），再写签名/偏移。
 *   · 路径/描述：12000002 / 12000004 字符串。
 *   · DisplayOrder：指到默认项。
 * 调用方负责告诉我"sys/sr 在盘上的字节偏移"（= 分区起始 LBA × 512）。*/
int img_bcd_build(const img_bcd_build_req_t *rq, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(!rq || !rq->template_path || !rq->out_path) return IMG_EINVAL;
    img_bcd *b=img_bcd_open_file(rq->template_path);
    if(!b){ if(err) snprintf(err,errcap,"模板 BCD 打不开或不是合法 hive"); return IMG_EFORMAT; }

    /* ★ 选"要填的那个启动项"：优先 {bootmgr} 的 default（23000003），
     *   再退 DisplayOrder。Win7 的 BCD-Template 没有 DisplayOrder，
     *   但 bootmgr 的 default 指向一个只有 locale 的空壳对象 —— bcdboot 就是填它。*/
    char os_guid[80]="";
    { char *v=img_bcd_get_string(b,IMG_BCD_GUID_BOOTMGR,"23000003");
      if(v){ if(v[0]) remap_trim_guid(v,os_guid,sizeof os_guid); free(v); }
    }
    if(!os_guid[0]){
        char order[96]="";
        if(img_bcd_get_display_order_first(b,order,sizeof order)==IMG_OK)
            remap_trim_guid(order,os_guid,sizeof os_guid);
    }
    if(!os_guid[0]){
        img_bcd_close(b);
        if(err) snprintf(err,errcap,"模板里既没有 bootmgr 的 default 也没有 DisplayOrder —— 换个模板");
        return IMG_EFORMAT;
    }

    const char *desc = (rq->desc && *rq->desc) ? rq->desc : "Windows";
    const char *bootmgr_path = rq->uefi ? "\\EFI\\Microsoft\\Boot\\bootmgfw.efi" : "\\bootmgr";
    const char *loader_rel   = rq->uefi ? "\\system32\\winload.efi" : "\\system32\\winload.exe";
    const char *loader_abs   = rq->uefi ? "\\Windows\\system32\\winload.efi"
                                        : "\\Windows\\system32\\winload.exe";

    /* ---- 设备元素 ---- */
    if(rq->uefi){
        unsigned char blob[88];
        if(!rq->sys_guid || !rq->disk_guid){
            img_bcd_close(b);
            if(err) snprintf(err,errcap,"UEFI 构建需要 sys-guid 与 disk-guid");
            return IMG_EINVAL;
        }
        if(img_bcd_make_partition_device(rq->sys_guid,rq->disk_guid,blob)!=IMG_OK){
            img_bcd_close(b);
            if(err) snprintf(err,errcap,"系统分区/磁盘 GUID 非法");
            return IMG_EINVAL;
        }
        img_bcd_set_element_binary(b,os_guid,"11000001",blob,sizeof blob);
        img_bcd_set_element_binary(b,os_guid,"21000001",blob,sizeof blob);
        if(rq->esp_guid){
            unsigned char eb[88];
            if(img_bcd_make_partition_device(rq->esp_guid,rq->disk_guid,eb)==IMG_OK)
                img_bcd_set_element_binary(b,IMG_BCD_GUID_BOOTMGR,"11000001",eb,sizeof eb);
        }
    } else {
        /* MBR 分区设备：type=6@16 / size=72@24 / 字节偏移@32 / flag=1@52 / 签名@56 */
        unsigned char dev[88];
        img_bcd_make_mbr_device(rq->mbr_sig, rq->sys_off, dev);
        img_bcd_set_element_binary(b,os_guid,"11000001",dev,sizeof dev);
        img_bcd_set_element_binary(b,os_guid,"21000001",dev,sizeof dev);
        if(rq->sr_off){
            unsigned char bm[88]; memcpy(bm,dev,sizeof bm);
            memcpy(bm+32,&rq->sr_off,8);
            img_bcd_set_element_binary(b,IMG_BCD_GUID_BOOTMGR,"11000001",bm,sizeof bm);
        }
    }

    /* ---- 字符串 / 继承 / 默认项 ---- */
    img_bcd_set_string(b,os_guid,"12000002",loader_abs);      /* ApplicationPath */
    img_bcd_set_string(b,os_guid,"42000002",loader_rel);      /* Win7 用的相对 Path */
    img_bcd_set_string(b,os_guid,"12000004",desc);
    img_bcd_set_string(b,os_guid,"12000005","zh-CN");
    img_bcd_set_string(b,os_guid,"22000002","\\windows");     /* systemroot */
    img_bcd_set_string(b,os_guid,"14000006","{6efb52bf-1766-41db-a6b3-0ee5eff72bd7}");

    img_bcd_set_string(b,IMG_BCD_GUID_BOOTMGR,"12000002",bootmgr_path);
    img_bcd_set_string(b,IMG_BCD_GUID_BOOTMGR,"12000004","Windows Boot Manager");
    img_bcd_set_string(b,IMG_BCD_GUID_BOOTMGR,"12000005","zh-CN");
    img_bcd_set_string(b,IMG_BCD_GUID_BOOTMGR,"14000006","{7ea2e1ac-2e61-4728-aaa3-896d9d0a9f0e}");
    img_bcd_set_string(b,IMG_BCD_GUID_BOOTMGR,"23000003",os_guid);   /* default */
    img_bcd_displayorder_add(b,os_guid);
    { static const unsigned char tmo=5;
      img_bcd_set_element_binary(b,IMG_BCD_GUID_BOOTMGR,"25000004",&tmo,1); }

    int rc=img_bcd_save(b,rq->out_path);
    img_bcd_close(b);
    if(rc!=IMG_OK){ if(err) snprintf(err,errcap,"BCD 落盘失败"); return rc; }
    return IMG_OK;
}

/* ============================================================
 * bcd.create —— 用【内置模板】建一张能启动的 BCD（不需要参考盘）
 *
 * 为什么是"模板 + 全量重写"而不是"从零拼元素"：
 *   元素表（geoffchappell / BCD-FORMAT.md）只告诉你 ID 与格式，不告诉你
 *   bootmgr 认哪些组合。我试过自己拼，得到 0xc0000034（元素非法）/
 *   0xc000000f（找不到 winload）。所以底座用真实能启动的 BCD，
 *   把【每一处与盘有关的东西】都按目标盘重写：
 *     设备（磁盘签名/分区偏移 或 分区GUID/磁盘GUID）、加载器路径、
 *     SystemRoot、菜单描述、locale、默认项、displayorder、超时。
 *   模板里机器专属的部分在 reference/bcd-tpl-*.bin 生成时已被清零。
 *
 * 三代三包（用户要"一招鲜"，但不同代的 bootmgr 差异真实存在）：
 *   legacy_bios  = Vista/7 BIOS     modern_bios = 8/8.1/10/11 BIOS
 *   modern_uefi  = 8/8.1/10/11 UEFI（带 FirmwareVariable，BIOS 模板没有）
 *   32/64 位不分开：BCD 只记 \Windows\system32\winload.exe|.efi，与位数无关。
 * ============================================================ */
#include "img_bcd_tpl.h"

static const unsigned char *tpl_pick(const img_bcd_create_req_t *rq, size_t *len){
    int fam = (int)rq->family;
    if(fam == IMG_BCD_FAM_AUTO) fam = IMG_BCD_FAM_MODERN;    /* 调用方通常已按 win.version 定好 */
    if(rq->uefi){
        /* 6.x 的 UEFI 没有官方支持（Win7 的 bootmgfw.efi 走 UGA，现代 OVMF 只给 GOP）。
         * 仍给一个包：legacy 模板改 UEFI 路径 —— 明确标注"实验，未验证"。*/
        if(fam == IMG_BCD_FAM_LEGACY){ *len = sizeof IMG_BCD_TPL_LEGACY_BIOS; return IMG_BCD_TPL_LEGACY_BIOS; }
        *len = sizeof IMG_BCD_TPL_MODERN_UEFI; return IMG_BCD_TPL_MODERN_UEFI;
    }
    if(fam == IMG_BCD_FAM_LEGACY){ *len = sizeof IMG_BCD_TPL_LEGACY_BIOS; return IMG_BCD_TPL_LEGACY_BIOS; }
    *len = sizeof IMG_BCD_TPL_MODERN_BIOS; return IMG_BCD_TPL_MODERN_BIOS;
}

int img_bcd_template_size(int family, bool uefi){
    img_bcd_create_req_t r; memset(&r, 0, sizeof r);
    r.family = (img_bcd_family)family; r.uefi = uefi;
    size_t n = 0; tpl_pick(&r, &n);
    return (int)n;
}

/* 去掉花括号与填充，只留 GUID 文本（hive 里常带 {..}\0 尾巴）*/
static void guid_trim(const char *in, char *out, size_t cap){
    if(cap) out[0] = 0;
    if(!in) return;
    while(*in == '{' || *in == ' ' || *in == '\t') in++;
    size_t i = 0;
    for(; in[i] && in[i] != '}' && i + 1 < cap; i++){
        unsigned char c = (unsigned char)in[i];
        if(c <= 0x20) break;                 /* NUL/空格 之后都是垃圾 */
        out[i] = (char)c;
    }
    out[i] = 0;
}

static int del_element(img_bcd *b, const char *guid, const char *eid){
    hive_node_h o = find_object(b, guid);
    if(!o) return IMG_ENOENT;
    hive_node_h els = hivex_node_get_child(b->h, o, "Elements");
    if(!els) return IMG_ENOENT;
    hive_node_h el = hivex_node_get_child(b->h, els, eid);
    if(!el) return IMG_OK;
    return hivex_node_delete_child(b->h, el) == 0 ? IMG_OK : IMG_EIO;
}

static int del_object(img_bcd *b, const char *guid){
    hive_node_h o = find_object(b, guid);
    if(!o) return IMG_OK;
    return hivex_node_delete_child(b->h, o) == 0 ? IMG_OK : IMG_EIO;
}

/* 恢复相关对象：WinRE 加载器（26000022=1 / 路径含 winre）、ramdisk options（含 32000004）*/
static bool obj_is_recovery(img_bcd *b, const char *guid){
    if(img_bcd_has_element(b, guid, "32000004")) return true;     /* ramdisk options */
    char *p = img_bcd_get_string(b, guid, "12000002");
    bool hit = false;
    if(p){
        for(char *s = p; *s; s++) *s = (char)tolower((unsigned char)*s);
        if(strstr(p, "winre") || strstr(p, "boot.sdi")) hit = true;
        free(p);
    }
    if(img_bcd_has_element(b, guid, "26000022")) hit = true;      /* PE / recovery 标志 */
    return hit;
}

/* 88 字节分区设备（MBR：签名@56、字节偏移@32；GPT：GUID 对）*/
int img_bcd_make_mbr_device(uint32_t sig, uint64_t off, unsigned char blob[88]){
    memset(blob, 0, 88);
    uint32_t ty = 6, sz = 72, one = 1;
    memcpy(blob + 16, &ty, 4);      /* device type = 6 分区设备 */
    memcpy(blob + 24, &sz, 4);      /* 结构长度 = 72（88-16）*/
    memcpy(blob + 32, &off, 8);     /* 分区【字节】偏移 */
    /* ★ [52..55] = 1：真机 BCD 全都有这一位，我漏了它 →
     *    bootmgr 认得这个分区设备，回车就报 0xc000000e「指定的设备不存在」。 */
    memcpy(blob + 52, &one, 4);
    memcpy(blob + 56, &sig, 4);     /* NT 磁盘签名 */
    return IMG_OK;
}

static int make_dev(const img_bcd_create_req_t *rq, bool use_sr, unsigned char blob[88]){
    if(rq->uefi){
        memset(blob, 0, 88);
        const char *part = use_sr ? (rq->esp_guid && *rq->esp_guid ? rq->esp_guid : rq->sys_guid)
                                  : rq->sys_guid;
        if(!part || !rq->disk_guid) return IMG_EINVAL;
        return img_bcd_make_partition_device(part, rq->disk_guid, blob);
    }
    return img_bcd_make_mbr_device(rq->mbr_sig, use_sr && rq->sr_off ? rq->sr_off : rq->sys_off, blob);
}

static int set_dev(img_bcd *b, const img_bcd_create_req_t *rq, const char *guid,
                   const char *eid, bool use_sr){
    unsigned char dev[88];
    int rc = make_dev(rq, use_sr, dev);
    if(rc != IMG_OK) return rc;
    return img_bcd_set_element_binary(b, guid, eid, dev, sizeof dev);
}

/* 找"要启动的那个 Windows"：bootmgr 的 default 优先；其次扫 12000002 含 winload 的对象 */
static void pick_osloader(img_bcd *b, char *out, size_t cap){
    out[0] = 0;
    char *v = img_bcd_get_string(b, IMG_BCD_GUID_BOOTMGR, "23000003");
    if(v){ guid_trim(v, out, cap); free(v); }
    if(out[0] && find_object(b, out)) return;
    out[0] = 0;
    char **guids = NULL; size_t n = 0;
    if(img_bcd_list_objects(b, &guids, &n) == IMG_OK){
        for(size_t i = 0; i < n && !out[0]; i++){
            char *p = img_bcd_get_string(b, guids[i], "12000002");
            if(!p) continue;
            bool loader = strstr(p, "winload") != NULL;
            free(p);
            if(loader) snprintf(out, cap, "%s", guids[i]);
        }
        img_bcd_free_object_list(guids, n);
    }
}


/* 把某个 GUID 写成「GUID 列表」元素（t=7）。displayorder/recoverysequence 必须是这个类型，
 * 写成 REG_SZ 或 RAW(t=3) 都会让 bootmgr 判定"没有有效操作系统"→ 0xc0000098。 */
static int set_list_element(img_bcd *b, const char *guid, const char *eid, const char *item){
    hive_node_h o = find_object(b, guid);
    if(!o) return IMG_ENOENT;
    hive_node_h el = ensure_element(b, o, eid);
    if(!el) return IMG_EIO;
    return set_guid_list_element(b, el, item);
}
static bool blob_is_zero(const void *p, size_t n){
    const unsigned char *q = (const unsigned char*)p;
    for(size_t i = 0; i < n; i++) if(q[i]) return false;
    return true;
}

int img_bcd_create_mem(const img_bcd_create_req_t *rq, void **out, size_t *out_len,
                       char *err, size_t errcap){
    if(err && errcap) err[0] = 0;
    if(!rq || !out || !out_len) return IMG_EINVAL;
    if(rq->uefi && (!rq->sys_guid || !rq->disk_guid)){
        if(err) snprintf(err, errcap, "UEFI 需要 sys-guid 与 disk-guid");
        return IMG_EINVAL;
    }
    if(!rq->uefi && (!rq->sys_off || !rq->mbr_sig)){
        if(err) snprintf(err, errcap, "BIOS 需要磁盘签名与系统分区字节偏移");
        return IMG_EINVAL;
    }

    size_t tlen = 0;
    const unsigned char *tpl = tpl_pick(rq, &tlen);
    img_bcd *b = img_bcd_open_mem(tpl, tlen);
    if(!b){ if(err) snprintf(err, errcap, "内置模板打不开（内存不足或模板损坏）"); return IMG_EIO; }

    const char *desc   = (rq->desc && *rq->desc) ? rq->desc : "Windows";
    const char *locale = (rq->locale && *rq->locale) ? rq->locale : "zh-CN";
    int timeout = rq->timeout > 0 ? rq->timeout : 5;

    char osldr[80] = "", resume[80] = "";
    pick_osloader(b, osldr, sizeof osldr);
    { char *v = img_bcd_get_string(b, IMG_BCD_GUID_BOOTMGR, "23000006");
      if(v){ guid_trim(v, resume, sizeof resume); free(v); } }

    /* ---- 1. 删掉恢复相关对象（我们的政策：不建恢复分区）---- */
    int dropped = 0;
    char **guids = NULL; size_t n = 0;
    if(img_bcd_list_objects(b, &guids, &n) == IMG_OK){
        for(size_t i = 0; i < n; i++){
            if(!strcmp(guids[i], IMG_BCD_GUID_BOOTMGR)) continue;
            if(osldr[0] && !strcmp(guids[i], osldr)) continue;
            if(resume[0] && !strcmp(guids[i], resume)) continue;
            if(rq->keep_recovery) continue;
            if(obj_is_recovery(b, guids[i])){ del_object(b, guids[i]); dropped++; }
        }
        img_bcd_free_object_list(guids, n);
    }
    if(!rq->keep_recovery){
        del_element(b, IMG_BCD_GUID_BOOTMGR, "14000008");    /* recoverysequence */
        if(osldr[0]) del_element(b, osldr, "14000008");
    }

    if(!osldr[0]){ img_bcd_close(b); if(err) snprintf(err, errcap, "模板里找不到 Windows 加载器对象"); return IMG_EFORMAT; }

    /* ---- 2. 设备：bootmgr → 引导分区(SR/ESP)，加载器/resume → 系统分区 ---- */
    set_dev(b, rq, IMG_BCD_GUID_BOOTMGR, "11000001", true);
    if(resume[0]){ set_dev(b, rq, resume, "11000001", false); set_dev(b, rq, resume, "21000001", false); }
    set_dev(b, rq, osldr, "11000001", false);
    set_dev(b, rq, osldr, "21000001", false);
    /* 模板里被清零的设备（memtest 等）按"库对象→引导分区"补上，
     * 否则 bootmgr 读到一个全零设备会当成"没有分区"，菜单少项/报错。 */
    { char **gs = NULL; size_t gn = 0;
      if(img_bcd_list_objects(b, &gs, &gn) == IMG_OK){
          for(size_t i = 0; i < gn; i++){
              if(!strcmp(gs[i], IMG_BCD_GUID_BOOTMGR)) continue;
              if(osldr[0] && !strcmp(gs[i], osldr)) continue;
              if(resume[0] && !strcmp(gs[i], resume)) continue;
              unsigned char cur[256]; size_t curlen = 0;
              if(img_bcd_get_element_binary(b, gs[i], "11000001", cur, sizeof cur, &curlen) != IMG_OK) continue;
              if(curlen != 88 || !blob_is_zero(cur, curlen)) continue;
              set_dev(b, rq, gs[i], "11000001", true);
          }
          img_bcd_free_object_list(gs, gn);
      }
    }

    /* ---- 3. 路径 / 描述 / locale / 默认项 / 超时 ---- */
    const char *ldr_abs = rq->uefi ? "\\Windows\\system32\\winload.efi"
                                   : "\\Windows\\system32\\winload.exe";
    const char *bm_path = rq->uefi ? "\\EFI\\Microsoft\\Boot\\bootmgfw.efi" : "\\bootmgr";
    img_bcd_set_string(b, osldr, "12000002", ldr_abs);
    if(rq->family != IMG_BCD_FAM_MODERN) img_bcd_set_string(b, osldr, "42000002", ldr_abs + 8);  /* 6.x 的相对 Path */
    img_bcd_set_string(b, osldr, "22000002", "\\Windows");     /* ★ 真机是大写；小写曾导致 0xc000000f */
    img_bcd_set_string(b, osldr, "12000004", desc);
    img_bcd_set_string(b, osldr, "12000005", locale);

    img_bcd_set_string(b, IMG_BCD_GUID_BOOTMGR, "12000002", bm_path);
    img_bcd_set_string(b, IMG_BCD_GUID_BOOTMGR, "12000004", "Windows Boot Manager");
    img_bcd_set_string(b, IMG_BCD_GUID_BOOTMGR, "12000005", locale);
    { char os_ref[72]; snprintf(os_ref, sizeof os_ref, "{%s}", osldr);
      img_bcd_set_string(b, IMG_BCD_GUID_BOOTMGR, "23000003", os_ref); }   /* ★ 必须带花括号 */

    set_list_element(b, IMG_BCD_GUID_BOOTMGR, "24000001", osldr);   /* displayorder 只留它 */
    { uint64_t t = (uint64_t)timeout;
      img_bcd_set_element_binary(b, IMG_BCD_GUID_BOOTMGR, "25000004", &t, 8); }

    int rc = img_bcd_save_mem(b, out, out_len);
    img_bcd_close(b);
    if(rc != IMG_OK){ if(err) snprintf(err, errcap, "BCD 生成失败（hive 落盘）"); return rc; }
    (void)dropped;
    return IMG_OK;
}

int img_bcd_create_file(const img_bcd_create_req_t *rq, const char *out_path,
                        char *err, size_t errcap){
    if(!rq || !out_path) return IMG_EINVAL;
    void *m = NULL; size_t n = 0;
    int rc = img_bcd_create_mem(rq, &m, &n, err, errcap);
    if(rc != IMG_OK) return rc;
    FILE *fp = fopen(out_path, "wb");
    if(!fp){ free(m); if(err) snprintf(err, errcap, "写不了 %s", out_path); return IMG_EIO; }
    size_t w = fwrite(m, 1, n, fp);
    fclose(fp);
    free(m);
    if(w != n){ if(err) snprintf(err, errcap, "写入不完整（%zu/%zu 字节）", w, n); return IMG_EIO; }
    return IMG_OK;
}