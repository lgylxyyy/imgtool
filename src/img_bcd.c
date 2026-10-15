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

int img_bcd_set_recovery_sequence(img_bcd *b, const char *guid){
    if(!b||!guid) return IMG_EINVAL;
    hive_node_h bm = find_object(b, IMG_BCD_GUID_BOOTMGR);
    if(!bm) return IMG_EFORMAT;
    hive_node_h el = ensure_element(b, bm, IMG_BCD_EL_RECOVERYSEQ);
    if(!el) return IMG_EIO;
    char buf[64]; snprintf(buf,sizeof buf,"{%s}",guid);
    return set_str_element(b, el, buf);
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
    char *s=u16_to_utf8(d,len); free(d); return s;
}
