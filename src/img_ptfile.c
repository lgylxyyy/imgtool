/* img_ptfile.c — 分区表文件（.json）的读写（REQ-20261026-14）
 *
 * 只做"文件 ↔ img_parttable_t"的翻译 + 结构校验；能不能装进目标盘由命令层判。
 * 解析器是**专用的最小 JSON**（不引入第三方库）：够读我们自己导出的文件，
 * 也能容忍 App 侧手写的同名文件（未知键直接跳过）。
 */
#define _POSIX_C_SOURCE 200809L
#include "img_ptfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ============================ 最小 JSON 读取 ============================ */
typedef struct { const char *p; } jp;

static void jws(jp *j){ while(*j->p && strchr(" \t\r\n",*j->p)) j->p++; }
static bool jlit(jp *j, const char *lit){
    size_t n=strlen(lit);
    if(strncmp(j->p,lit,n)!=0) return false;
    j->p+=n; return true;
}
static void jput_utf8(char *out, size_t cap, size_t *k, unsigned cp){
    if(cp<0x80){ if(*k+1<cap) out[(*k)++]=(char)cp; }
    else if(cp<0x800){ if(*k+2<cap){ out[(*k)++]=(char)(0xC0|(cp>>6)); out[(*k)++]=(char)(0x80|(cp&0x3F)); } }
    else { if(*k+3<cap){ out[(*k)++]=(char)(0xE0|(cp>>12));
                        out[(*k)++]=(char)(0x80|((cp>>6)&0x3F));
                        out[(*k)++]=(char)(0x80|(cp&0x3F)); } }
}
/* 读一个 JSON 字符串；out 可为 NULL（只跳过）。成功返回 true */
static bool jstr(jp *j, char *out, size_t cap){
    jws(j);
    if(*j->p!='"') return false;
    j->p++;
    size_t k=0;
    if(out && cap) out[0]=0;
    while(*j->p && *j->p!='"'){
        unsigned char c=(unsigned char)*j->p;
        if(c=='\\'){
            j->p++;
            char e=*j->p++;
            switch(e){
                case 'n': c='\n'; break;  case 't': c='\t'; break;
                case 'r': c='\r'; break;  case 'b': c='\b'; break;
                case 'f': c='\f'; break;  case '/': c='/';  break;
                case '"': c='"';  break;  case '\\': c='\\'; break;
                case 'u': {
                    unsigned cp=0; int i=0;
                    for(;i<4 && isxdigit((unsigned char)j->p[i]);i++)
                        cp=cp*16+(unsigned)(isdigit((unsigned char)j->p[i])
                            ? j->p[i]-'0' : (tolower((unsigned char)j->p[i])-'a'+10));
                    j->p+=i;
                    if(out) jput_utf8(out,cap,&k,cp);
                    continue;
                }
                default: if(out && k+1<cap) out[k++]=(char)e; continue;
            }
        } else j->p++;
        if(out && k+1<cap) out[k++]=(char)c;
    }
    if(out && cap) out[k<cap?k:cap-1]=0;
    if(*j->p=='"'){ j->p++; return true; }
    return false;
}
/* 数字（也接受带引号的数字字符串："128"） */
static bool jnum(jp *j, long long *out){
    jws(j);
    if(*j->p=='"'){
        char tmp[64];
        if(!jstr(j,tmp,sizeof tmp)) return false;
        char *end=NULL; long long v=strtoll(tmp,&end,0);
        if(!end||*end) return false;
        if(out) *out=v;
        return true;
    }
    const char *s=j->p;
    if(*s=='-'||*s=='+') s++;
    if(!isdigit((unsigned char)*s)) return false;
    char *end=NULL; long long v=strtoll(j->p,&end,0);
    if(!end||end==j->p) return false;
    j->p=end;
    if(out) *out=v;
    return true;
}
/* true/false/1/0/"true" */
static bool jbool(jp *j, bool *out){
    jws(j);
    if(jlit(j,"true")){ if(out)*out=true; return true; }
    if(jlit(j,"false")){ if(out)*out=false; return true; }
    if(*j->p=='"'){
        char tmp[16];
        if(!jstr(j,tmp,sizeof tmp)) return false;
        if(!strcasecmp(tmp,"true")||!strcmp(tmp,"1")){ if(out)*out=true; return true; }
        if(!strcasecmp(tmp,"false")||!strcmp(tmp,"0")){ if(out)*out=false; return true; }
        return false;
    }
    long long n;
    if(jnum(j,&n)){ if(out)*out=(n!=0); return true; }
    return false;
}
static bool jeat(jp *j, char c){ jws(j); if(*j->p==c){ j->p++; return true; } return false; }
static bool jisnull(jp *j){ jws(j); return jlit(j,"null"); }

/* 读十六进制字符串（接受 "0x1f" / "1f" / 数字 / null）；返回字节数，失败 -1 */
static int jhex_bytes(jp *j, uint8_t *out, int cap){
    jws(j);
    if(jisnull(j)){ if(out&&cap>0) memset(out,0,(size_t)cap); return 0; }
    char tmp[1200];
    if(*j->p=='"'){
        if(!jstr(j,tmp,sizeof tmp)) return -1;
    } else {
        long long n;
        if(!jnum(j,&n)) return -1;
        snprintf(tmp,sizeof tmp,"%llx",(unsigned long long)n);
    }
    const char *s=tmp;
    if(s[0]=='0' && (s[1]=='x'||s[1]=='X')) s+=2;
    size_t L=strlen(s);
    if(L%2) return -1;
    int nbytes=(int)(L/2);
    if(nbytes>cap) return -1;
    for(int i=0;i<nbytes;i++){
        char b[3]={s[i*2],s[i*2+1],0};
        if(!isxdigit((unsigned char)b[0])||!isxdigit((unsigned char)b[1])) return -1;
        if(out) out[i]=(uint8_t)strtoul(b,NULL,16);
    }
    return nbytes;
}

/* ============================ 表 → 文件 JSON ============================ */
static void jesc_to(char *buf, int cap, int *k, const char *s){
    for(; s && *s; s++){
        unsigned char c=(unsigned char)*s;
        if(c=='"'||c=='\\'){
            if(*k+2<cap){ buf[(*k)++]='\\'; buf[(*k)++]=(char)c; }
        } else if(c<0x20){
            if(*k+6<cap) *k+=snprintf(buf+*k,(size_t)(cap-*k),"\\u%04x",c);
        } else if(*k+1<cap) buf[(*k)++]=(char)c;
    }
}

int img_ptfile_dump(const img_parttable_t *t, const uint8_t *mbr_code,
                    char *buf, int cap){
    if(!t||!buf||cap<256) return -1;
    int k=0;
    k+=snprintf(buf+k,(size_t)(cap-k),
        "{\"kind\":\"%s\",\"v\":%d,\"table\":\"%s\",\"sectorSize\":512,"
        "\"diskSectors\":%llu,\"diskGuid\":\"%s\",\"diskSignature\":\"0x%08x\",",
        IMG_PTFILE_KIND, IMG_PTFILE_V,
        t->table==IMG_TABLE_GPT?"GPT":"MBR",
        (unsigned long long)t->disk_sectors, t->disk_guid,
        (unsigned)t->disk_signature);
    /* mbrCode：MBR 且真的有引导码才写（否则 null，省 900 字节）*/
    bool have_code=false;
    if(t->table==IMG_TABLE_MBR && mbr_code)
        for(int i=0;i<440;i++) if(mbr_code[i]){ have_code=true; break; }
    if(have_code){
        k+=snprintf(buf+k,(size_t)(cap-k),"\"mbrCode\":\"");
        for(int i=0;i<446 && k+2<cap;i++) k+=snprintf(buf+k,(size_t)(cap-k),"%02x",mbr_code[i]);
        k+=snprintf(buf+k,(size_t)(cap-k),"\",");
    } else {
        k+=snprintf(buf+k,(size_t)(cap-k),"\"mbrCode\":null,");
    }
    k+=snprintf(buf+k,(size_t)(cap-k),"\"parts\":[");
    bool first=true;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        if(k>cap-600) return -1;                 /* 装不下就报错，绝不吐半截 JSON */
        if(!first) k+=snprintf(buf+k,(size_t)(cap-k),",");
        first=false;
        k+=snprintf(buf+k,(size_t)(cap-k),
            "{\"first\":%llu,\"last\":%llu,\"sectors\":%llu,\"mbri\":%u,"
            "\"active\":%s,\"logical\":%s,\"typeGuid\":\"",
            (unsigned long long)p->first_lba,(unsigned long long)p->last_lba,
            (unsigned long long)(p->last_lba-p->first_lba+1),
            p->type, p->status==0x80?"true":"false", p->logical?"true":"false");
        jesc_to(buf,cap,&k,p->type_guid);
        k+=snprintf(buf+k,(size_t)(cap-k),"\",\"partGuid\":\"");
        jesc_to(buf,cap,&k,p->part_guid);
        k+=snprintf(buf+k,(size_t)(cap-k),"\",\"name\":\"");
        jesc_to(buf,cap,&k,p->name);
        k+=snprintf(buf+k,(size_t)(cap-k),"\",\"attributes\":\"0x%016llx\"}",
                    (unsigned long long)p->attributes);
    }
    k+=snprintf(buf+k,(size_t)(cap-k),"]}");   /* ★ 不带结尾换行：要嵌进信封 */
    if(k>=cap) return -1;
    return k;
}

/* ============================ 文件 → 表 ============================ */
static int bad(char *code,size_t codecap,char *at,size_t atcap,char *why,size_t whycap,
               const char *a, const char *w){
    if(code&&codecap) snprintf(code,codecap,"PT-FILE-BAD");
    if(at&&atcap) snprintf(at,atcap,"%s",a?a:"");
    if(why&&whycap) snprintf(why,whycap,"%s",w?w:"");
    return IMG_EINVAL;
}

bool img_ptfile_fits(const img_parttable_t *t, uint64_t disk_sectors,
                     uint64_t *need_sectors, uint64_t *need_gb){
    if(!t) return false;
    uint64_t end=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t->parts[i].used && t->parts[i].last_lba+1>end) end=t->parts[i].last_lba+1;
    /* GPT 要在盘尾留 33 个扇区（备份表 32 + 备份头 1）；MBR 留 0 */
    uint64_t need = end + (t->table==IMG_TABLE_GPT ? 34u : 1u);
    if(need_sectors) *need_sectors=need;
    if(need_gb) *need_gb=(need*512ull + (1ull<<30) - 1) >> 30;
    return disk_sectors >= need && disk_sectors>0;
}

int img_ptfile_load(const char *path, img_ptfile_t *out,
                    char *code, size_t codecap,
                    char *at, size_t atcap,
                    char *why, size_t whycap){
    if(code&&codecap) code[0]=0;
    if(at&&atcap) at[0]=0;
    if(why&&whycap) why[0]=0;
    if(!path||!out) return bad(code,codecap,at,atcap,why,whycap,"path","没有文件路径");

    FILE *f=fopen(path,"rb");
    if(!f) return bad(code,codecap,at,atcap,why,whycap,"in","打不开这个文件（不存在/没权限）");
    if(fseek(f,0,SEEK_END)!=0){ fclose(f); return bad(code,codecap,at,atcap,why,whycap,"in","读不了文件大小"); }
    long sz=ftell(f);
    /* 128 条分区 × 约 300 字节 + 900 字节 mbrCode ≈ 40 KB；给 1 MiB 富余 */
    if(sz<=0 || sz>(1024*1024)){ fclose(f); return bad(code,codecap,at,atcap,why,whycap,"in",
        sz<=0?"文件是空的":"文件太大（>1 MiB，不像是分区表）"); }
    rewind(f);
    char *txt=(char*)malloc((size_t)sz+1);
    if(!txt){ fclose(f); return bad(code,codecap,at,atcap,why,whycap,"in","内存不足"); }
    if(fread(txt,1,(size_t)sz,f)!=(size_t)sz){ free(txt); fclose(f);
        return bad(code,codecap,at,atcap,why,whycap,"in","读文件失败"); }
    txt[sz]=0;
    fclose(f);

    memset(out,0,sizeof *out);
    img_parttable_t *t=&out->t;

    jp j={txt};
    bool got_kind=false, got_table=false, got_parts=false;
    long long ver=0;
    int  nparts=0;
    bool logicals=false, have_container=false;
    int  nprim=0;

    jws(&j);
    if(!jeat(&j,'{')){ free(txt); return bad(code,codecap,at,atcap,why,whycap,NULL,"文件不是 JSON 对象"); }

    while(true){
        jws(&j);
        if(*j.p=='}'){ j.p++; break; }
        char key[64];
        if(!jstr(&j,key,sizeof key)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,NULL,"JSON 键解析失败"); }
        if(!jeat(&j,':')){ free(txt); return bad(code,codecap,at,atcap,why,whycap,key,"JSON 少了冒号"); }

        if(!strcmp(key,"kind")){
            char v[64];
            if(!jstr(&j,v,sizeof v)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"kind","kind 不是字符串"); }
            if(strcmp(v,IMG_PTFILE_KIND)){ free(txt);
                return bad(code,codecap,at,atcap,why,whycap,"kind","这不是分区表文件（kind 不对）"); }
            got_kind=true;
        } else if(!strcmp(key,"v")){
            if(!jnum(&j,&ver)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"v","v 不是数字"); }
            if(ver>IMG_PTFILE_V){ free(txt);
                return bad(code,codecap,at,atcap,why,whycap,"v","文件版本比工具新，先升级工具"); }
        } else if(!strcmp(key,"table")){
            char v[16];
            if(!jstr(&j,v,sizeof v)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"table","table 不是字符串"); }
            if(!strcasecmp(v,"GPT"))      t->table=IMG_TABLE_GPT;
            else if(!strcasecmp(v,"MBR")) t->table=IMG_TABLE_MBR;
            else { free(txt); return bad(code,codecap,at,atcap,why,whycap,"table","table 只接受 GPT | MBR"); }
            got_table=true;
        } else if(!strcmp(key,"sectorSize")){
            long long ss=0;
            if(!jnum(&j,&ss)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"sectorSize","sectorSize 不是数字"); }
            if(ss!=512){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"sectorSize",
                ss==4096?"本版只支持 512 字节扇区（4K 盘还没做）":"sectorSize 只支持 512"); }
        } else if(!strcmp(key,"diskSectors")){
            long long ds=0;
            if(jnum(&j,&ds) && ds>0) out->disk_sectors=(uint64_t)ds;
        } else if(!strcmp(key,"diskGuid")){
            if(!jstr(&j,t->disk_guid,sizeof t->disk_guid)){ free(txt);
                return bad(code,codecap,at,atcap,why,whycap,"diskGuid","diskGuid 不是字符串"); }
        } else if(!strcmp(key,"diskSignature")){
            uint8_t b[8];
            int n=jhex_bytes(&j,b,sizeof b);
            if(n<0){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"diskSignature","diskSignature 不是十六进制"); }
            t->disk_signature=0;
            if(n==4) t->disk_signature=((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|(uint32_t)b[3];
            else if(n>0){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"diskSignature","diskSignature 必须是 4 字节"); }
        } else if(!strcmp(key,"mbrCode")){
            int n=jhex_bytes(&j,out->mbr_code,446);
            if(n<0||(n!=0&&n!=446)){ free(txt);
                return bad(code,codecap,at,atcap,why,whycap,"mbrCode","mbrCode 要么 null，要么 446 字节的十六进制"); }
            if(n==446){
                out->has_mbr_code=true;
                memcpy(t->mbr_code,out->mbr_code,446);
            }
        } else if(!strcmp(key,"parts")){
            if(!jeat(&j,'[')){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"parts","parts 不是数组"); }
            got_parts=true;
            while(true){
                jws(&j);
                if(*j.p==']'){ j.p++; break; }
                if(*j.p!='{'){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"parts","parts 里不是对象"); }
                j.p++;
                img_partition_t p; memset(&p,0,sizeof p); p.used=true;
                bool got_first=false, got_sectors=false;
                unsigned long long sec_v=0, last_v=0; bool got_last=false;
                while(true){
                    jws(&j);
                    if(*j.p=='}'){ j.p++; break; }
                    char k2[48];
                    if(!jstr(&j,k2,sizeof k2)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"parts","分区对象里的键解析失败"); }
                    if(!jeat(&j,':')){ free(txt); return bad(code,codecap,at,atcap,why,whycap,k2,"JSON 少了冒号"); }
                    if(!strcmp(k2,"first")){
                        long long v; if(!jnum(&j,&v)||v<1){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.first","first 必须是 ≥1 的整数"); }
                        p.first_lba=(uint64_t)v; got_first=true;
                    } else if(!strcmp(k2,"sectors")){
                        long long v; if(!jnum(&j,&v)||v<1){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.sectors","sectors 必须是 ≥1 的整数"); }
                        sec_v=(unsigned long long)v; got_sectors=true;
                    } else if(!strcmp(k2,"last")){
                        long long v; if(!jnum(&j,&v)||v<1){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.last","last 必须是 ≥1 的整数"); }
                        last_v=(unsigned long long)v; got_last=true;
                    } else if(!strcmp(k2,"mbri")){
                        long long v; if(!jnum(&j,&v)||v<0||v>255){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.mbri","mbri 必须是 0~255"); }
                        p.type=(uint8_t)v;
                    } else if(!strcmp(k2,"active")){
                        bool b; if(!jbool(&j,&b)){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.active","active 不是布尔"); }
                        p.status=b?0x80:0x00;
                    } else if(!strcmp(k2,"logical")){
                        bool b; if(!jbool(&j,&b)){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.logical","logical 不是布尔"); }
                        p.logical=b;
                    } else if(!strcmp(k2,"typeGuid")){
                        if(!jstr(&j,p.type_guid,sizeof p.type_guid)){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.typeGuid","typeGuid 不是字符串"); }
                    } else if(!strcmp(k2,"partGuid")){
                        if(!jstr(&j,p.part_guid,sizeof p.part_guid)){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.partGuid","partGuid 不是字符串"); }
                    } else if(!strcmp(k2,"name")){
                        if(!jstr(&j,p.name,sizeof p.name)){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.name","name 不是字符串"); }
                    } else if(!strcmp(k2,"attributes")){
                        uint8_t b[8];
                        if(*j.p=='"'||isdigit((unsigned char)*j.p)){
                            int n=jhex_bytes(&j,b,sizeof b);
                            if(n<0){ free(txt); return bad(code,codecap,at,atcap,why,whycap,"parts.attributes","attributes 不是十六进制"); }
                            uint64_t v=0;
                            for(int i=0;i<n;i++) v=(v<<8)|(uint64_t)b[i];   /* 大端：0x80…00 == bit63 */
                            p.attributes=v;
                        } else if(!jisnull(&j)){
                            free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,"parts.attributes","attributes 不是十六进制");
                        }
                    } else {
                        /* 未知键：跳过值（字符串/数字/布尔/null/嵌套都覆盖到） */
                        jws(&j);
                        if(*j.p=='"') { if(!jstr(&j,NULL,0)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,k2,"值解析失败"); } }
                        else if(*j.p=='{'){ int depth=0;
                            while(*j.p){ if(*j.p=='{')depth++; else if(*j.p=='}'&&!--depth){j.p++;break;} j.p++; } }
                        else if(*j.p=='['){ int depth=0;
                            while(*j.p){ if(*j.p=='[')depth++; else if(*j.p==']'&&!--depth){j.p++;break;} j.p++; } }
                        else { long long v; bool b; if(!jnum(&j,&v)&&!jbool(&j,&b)&&!jisnull(&j)){ free(txt);
                            return bad(code,codecap,at,atcap,why,whycap,k2,"值解析失败"); } }
                    }
                    /* ★ 成员之间要有逗号处理：漏了这一步会在 "first":2048 之后直接卡住 */
                    jws(&j);
                    if(*j.p==','){ j.p++; continue; }
                    if(*j.p=='}'){ j.p++; break; }
                    { free(txt); return bad(code,codecap,at,atcap,why,whycap,k2,"JSON 结构不完整（少了 , 或 }）"); }
                }
                if(!got_first||!got_sectors){ free(txt);
                    return bad(code,codecap,at,atcap,why,whycap,"parts","每条分区必须有 first 和 sectors"); }
                p.last_lba=p.first_lba+sec_v-1;
                if(got_last && last_v!=(unsigned long long)p.last_lba){ free(txt);
                    return bad(code,codecap,at,atcap,why,whycap,"parts.last",
                               "last 与 first+sectors-1 对不上"); }
                if(nparts>=IMG_MAX_PARTITIONS){ free(txt);
                    return bad(code,codecap,at,atcap,why,whycap,"parts","分区条数超过上限"); }
                t->parts[nparts++]=p;
                t->count=nparts;
                /* ★ 同理：分区之间的逗号要在这里吃掉，否则第二个分区就"不是对象"了 */
                jws(&j);
                if(*j.p==','){ j.p++; continue; }
                if(*j.p==']'){ j.p++; break; }
                { free(txt); return bad(code,codecap,at,atcap,why,whycap,"parts","parts 数组结构不完整（少了 , 或 ]）"); }
            }
        } else {
            /* 顶层未知键：跳过值 */
            jws(&j);
            if(*j.p=='"'){ if(!jstr(&j,NULL,0)){ free(txt); return bad(code,codecap,at,atcap,why,whycap,key,"值解析失败"); } }
            else if(*j.p=='{'){ int depth=0;
                while(*j.p){ if(*j.p=='{')depth++; else if(*j.p=='}'&&!--depth){j.p++;break;} j.p++; } }
            else if(*j.p=='['){ int depth=0;
                while(*j.p){ if(*j.p=='[')depth++; else if(*j.p==']'&&!--depth){j.p++;break;} j.p++; } }
            else { long long v; bool b; if(!jnum(&j,&v)&&!jbool(&j,&b)&&!jisnull(&j)){ free(txt);
                return bad(code,codecap,at,atcap,why,whycap,key,"值解析失败"); } }
        }
        jws(&j);
        if(*j.p==','){ j.p++; continue; }
        if(*j.p=='}'){ j.p++; break; }
        free(txt);
        return bad(code,codecap,at,atcap,why,whycap,key,"JSON 结构不完整（少了 , 或 }）");
    }
    free(txt);

    if(!got_kind)  return bad(code,codecap,at,atcap,why,whycap,"kind","缺 kind（这不像是分区表文件）");
    if(!got_table) return bad(code,codecap,at,atcap,why,whycap,"table","缺 table（GPT | MBR）");
    if(!got_parts) return bad(code,codecap,at,atcap,why,whycap,"parts","缺 parts 数组");

    /* ---- 结构校验 ---- */
    for(int i=0;i<nparts;i++){
        img_partition_t *p=&t->parts[i];
        if(p->last_lba < p->first_lba) return bad(code,codecap,at,atcap,why,whycap,"parts.sectors","sectors 必须 ≥1");
        char atb[48]; snprintf(atb,sizeof atb,"parts[%d]",i);
        if(t->table==IMG_TABLE_GPT){
            if(!p->part_guid[0]) return bad(code,codecap,at,atcap,why,whycap,"parts.partGuid","GPT 每条分区都要 partGuid");
            if(img_partition_guid_valid(p->part_guid)!=IMG_OK)
                return bad(code,codecap,at,atcap,why,whycap,"parts.partGuid","partGuid 不是合法 GUID");
            if(!p->type_guid[0] || img_partition_guid_valid(p->type_guid)!=IMG_OK)
                return bad(code,codecap,at,atcap,why,whycap,"parts.typeGuid","typeGuid 不是合法 GUID");
            for(int k=i+1;k<nparts;k++)
                if(!strcmp(p->part_guid,t->parts[k].part_guid)){
                    if(code&&codecap) snprintf(code,codecap,"PT-DUP-GUID");
                    if(at&&atcap) snprintf(at,atcap,"parts.partGuid");
                    if(why&&whycap) snprintf(why,whycap,"两条分区的 partGuid 相同：%s",p->part_guid);
                    return IMG_EINVAL;
                }
        }
        if(p->logical) logicals=true;
        else {
            nprim++;
            if(t->table==IMG_TABLE_MBR && (p->type==IMG_MBR_EXTENDED||p->type==IMG_MBR_EXTENDED_L))
                have_container=true;
        }
        (void)atb;
    }
    if(t->table==IMG_TABLE_MBR){
        if(nprim>4){
            if(code&&codecap) snprintf(code,codecap,"PT-TOO-MANY");
            if(at&&atcap) snprintf(at,atcap,"parts");
            if(why&&whycap) snprintf(why,whycap,"MBR 最多 4 个主分区（含扩展容器），文件里有 %d 个",nprim);
            return IMG_EINVAL;
        }
        if(logicals && !have_container) return bad(code,codecap,at,atcap,why,whycap,"parts",
            "有逻辑分区，但没有 0x0F 扩展容器（MBR 的逻辑分区必须住在容器里）");
        if(have_container){
            int ci=-1;
            for(int i=0;i<nparts;i++)
                if(!t->parts[i].logical &&
                   (t->parts[i].type==IMG_MBR_EXTENDED||t->parts[i].type==IMG_MBR_EXTENDED_L)){ ci=i; break; }
            for(int i=0;i<nparts;i++){
                if(!t->parts[i].logical) continue;
                const img_partition_t *c=&t->parts[ci];
                if(t->parts[i].first_lba<=c->first_lba || t->parts[i].last_lba>c->last_lba)
                    return bad(code,codecap,at,atcap,why,whycap,"parts.logical",
                        "逻辑分区跑到扩展容器外面了（文件里的 LBA 不一致）");
            }
        }
    }
    /* 重叠检查（容器 vs 它自己的逻辑分区不算重叠） */
    for(int i=0;i<nparts;i++){
        const img_partition_t *a=&t->parts[i];
        for(int k=i+1;k<nparts;k++){
            const img_partition_t *b=&t->parts[k];
            bool a_ext=(a->type==IMG_MBR_EXTENDED||a->type==IMG_MBR_EXTENDED_L);
            bool b_ext=(b->type==IMG_MBR_EXTENDED||b->type==IMG_MBR_EXTENDED_L);
            if((a->logical&&b_ext)||(b->logical&&a_ext)) continue;
            if(a->first_lba<=b->last_lba && b->first_lba<=a->last_lba){
                if(code&&codecap) snprintf(code,codecap,"PT-OVERLAP");
                if(at&&atcap) snprintf(at,atcap,"parts");
                if(why&&whycap) snprintf(why,whycap,"两条分区重叠：first=%llu 与 first=%llu",
                    (unsigned long long)a->first_lba,(unsigned long long)b->first_lba);
                return IMG_EINVAL;
            }
        }
    }
    t->part_entry_count=128;
    t->part_entry_size=128;
    if(!t->disk_guid[0] && t->table==IMG_TABLE_GPT) img_guid_new(t->disk_guid);
    return IMG_OK;
}