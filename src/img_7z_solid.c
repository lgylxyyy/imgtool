/* img_7z_solid.c — 7z 固实标志（REQ-51 A），实现说明见 img_7z_solid.h
 *
 * 只依赖 liblzma（本来就跟 libarchive 一起静态链接）。
 * 解析器是"够用就好"：只走 MainStreamsInfo 那条主干，遇到没见过的结构就放弃
 * （返回 false），绝不让 archive.ls 因此失败 —— 这个字段只是给 UI 的提示。
 */
#include "img_7z_solid.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lzma.h>

static int DBG(void){ static int v=-1; if(v<0) v=getenv("IMG_7Z_DEBUG")?1:0; return v; }
#define D(...) do{ if(DBG()) { fprintf(stderr,"[7z] " __VA_ARGS__); fputc(10,stderr);} }while(0)

typedef struct { const uint8_t *p; size_t n, i; } cur_t;

/* 7z 的数字编码（与 libarchive parse_7zip_uint64 同语义）*/
static int rd_num(cur_t *c, uint64_t *out){
    if(c->i>=c->n) return -1;
    unsigned char avail=c->p[c->i++];
    unsigned char mask=0x80;
    uint64_t v=0;
    for(int i=0;i<8;i++){
        if(avail & mask){
            if(c->i>=c->n) return -1;
            v |= ((uint64_t)c->p[c->i++]) << (8*i);
            mask >>= 1;
            continue;
        }
        v += ((uint64_t)(avail & (mask-1))) << (8*i);
        break;
    }
    *out=v;
    return 0;
}
static int rd_byte(cur_t *c, unsigned char *b){
    if(c->i>=c->n) return -1;
    *b=c->p[c->i++];
    return 0;
}
static int skip(cur_t *c, size_t n){
    if(c->i+n > c->n) return -1;
    c->i+=n;
    return 0;
}

/* PackInfo：packPos, numPackStreams, [kSize sizes...], kEnd */
static int skip_packinfo(cur_t *c, uint64_t *out_pack_pos, uint64_t *sizes, int maxsizes, int *nsizes){
    uint64_t v;
    if(rd_num(c,&v)) return -1;                 /* packPos */
    if(out_pack_pos) *out_pack_pos=v;
    if(rd_num(c,&v)) return -1;                 /* numPackStreams */
    uint64_t n=v; if(n>64) return -1;
    if(sizes && nsizes) *nsizes=(int)n;
    for(;;){
        unsigned char t;
        if(rd_byte(c,&t)) return -1;
        if(t==0x00) return 0;
        if(t==0x09){
            for(uint64_t i=0;i<n;i++){
                if(rd_num(c,&v)) return -1;
                if(sizes && (int)i<maxsizes) sizes[i]=v;
            }
        } else if(t==0x0A){                     /* kCRC */
            unsigned char all;
            if(rd_byte(c,&all)) return -1;
            if(all){
                if(skip(c,(size_t)4)) return -1;
            } else {
                for(uint64_t i=0;i<n;i++){
                    if(rd_num(c,&v)) return -1;
                    if(v&1){ if(skip(c,4)) return -1; }
                }
            }
        } else {
            return -1;                          /* 没见过：放弃 */
        }
    }
}

/* UnpackInfo：kFolder（folders）→ kCodersUnpackSize → kEnd。
 * 只关心【第一个 folder 的第一个 coder 的 id/props】和【解压总大小】。*/
typedef struct {
    uint64_t codec_id;
    uint8_t  props[64];
    size_t   props_len;
    uint64_t unpack_size;
    uint64_t num_folders;
    int      n_unpack_sizes;
} unpack_t;

static int parse_unpackinfo(cur_t *c, unpack_t *u){
    unsigned char t;
    if(rd_byte(c,&t)) return -1;
    if(t!=0x0B) return -1;                      /* kFolder */
    uint64_t nf;
    if(rd_num(c,&nf)) return -1;
    if(nf==0 || nf>4096) return -1;
    u->num_folders=nf;
    unsigned char ext;
    if(rd_byte(c,&ext)) return -1;
    if(ext) return -1;                          /* 外部 folder 数据：不支持 */
    for(uint64_t f=0; f<nf; f++){
        uint64_t nc;
        if(rd_num(c,&nc)) return -1;
        if(nc==0 || nc>64) return -1;
        for(uint64_t k=0;k<nc;k++){
            unsigned char flags;
            if(rd_byte(c,&flags)) return -1;
            size_t idsz=flags & 0x0F;
            bool simple=(flags & 0x10)==0;
            bool has_attr=(flags & 0x20)!=0;
            if(flags & 0x80) return -1;
            if(idsz==0 || idsz>8) return -1;
            uint64_t numIn=1, numOut=1;
            uint64_t id=0;
            for(size_t z=0; z<idsz; z++){
                unsigned char b;
                if(rd_byte(c,&b)) return -1;
                id=(id<<8)|b;
            }
            if(!simple){
                if(rd_num(c,&numIn) || rd_num(c,&numOut)) return -1;
            }
            (void)numIn; (void)numOut;
            uint8_t props[64]; size_t plen=0;
            if(has_attr){
                uint64_t ps;
                if(rd_num(c,&ps)) return -1;
                if(ps>sizeof props) return -1;
                if(skip(c,(size_t)ps)) return -1;
                memcpy(props, c->p + c->i - ps, (size_t)ps);
                plen=(size_t)ps;
            }
            if(f==0 && k==0){
                u->codec_id=id;
                u->props_len=plen;
                memcpy(u->props, props, plen);
            }
        }
        /* ★ numBindPairs = totalOut-1、numPackedStreams = totalIn-numBindPairs 都是
         *   【推导】出来的，不占字节（libarchive 同样）；只有 numPackedStreams>1 时才
         *   跟一串索引。以前在这里多读了一个 num，把后面的 kCodersUnpackSize 吃掉了。*/
    }
    if(rd_byte(c,&t)) return -1;
    if(t!=0x0C) return -1;                      /* kCodersUnpackSize */
    uint64_t v; u->n_unpack_sizes=0;
    for(uint64_t f=0; f<nf; f++){
        if(rd_num(c,&v)) return -1;
        if(f==0) u->unpack_size=v;
        u->n_unpack_sizes++;
    }
    for(;;){
        unsigned char tt;
        if(rd_byte(c,&tt)) return -1;
        if(tt==0x00) return 0;
        if(tt==0x0A){                            /* kCRC */
            unsigned char all;
            if(rd_byte(c,&all)) return -1;
            if(all){ if(skip(c,4)) return -1; }
            else { uint64_t d; for(int i=0;i<u->n_unpack_sizes;i++){ if(rd_num(c,&d)) return -1; if(d&1){ if(skip(c,4)) return -1; } } }
        } else return -1;
    }
}

/* StreamsInfo（简化）：只要 PackInfo + UnpackInfo；SubStreamsInfo 的 kNumUnPackStream 用于判固实 */
static int parse_streamsinfo(cur_t *c, unpack_t *u, uint64_t *pack_pos, uint64_t *pack_sizes, int *n_pack, bool *solid){
    unsigned char t;
    for(;;){
        if(rd_byte(c,&t)) return -1;
        if(t==0x00) return 0;
        if(t==0x06){
            if(skip_packinfo(c, pack_pos, pack_sizes, 8, n_pack)) return -1;
        } else if(t==0x07){
            if(parse_unpackinfo(c, u)) return -1;
        } else if(t==0x08){
            /* SubStreamsInfo：第一个元素若是 kNumUnPackStream(0x0D) → 每个 folder 的流数 */
            unsigned char tt;
            if(rd_byte(c,&tt)) return -1;
            if(tt==0x0D){
                for(uint64_t f=0; f<u->num_folders; f++){
                    uint64_t n;
                    if(rd_num(c,&n)) return -1;
                    if(n>1 && solid) *solid=true;
                }
                return 0;                        /* 要的信息已经拿到（后面的 kSize/kCRC 不关心）*/
            }
            return -1;                           /* 没有 kNumUnPackStream：每个 folder 1 个流 → 非固实 */
        } else {
            return -1;
        }
    }
}

static int decode_header(const char *path, uint64_t pack_pos, uint64_t pack_size,
                         const unpack_t *u, uint8_t **out, size_t *out_len){
    if(pack_size==0 || pack_size>64u*1024*1024) return -1;
    if(u->unpack_size==0 || u->unpack_size>16u*1024*1024) return -1;   /* 头不该这么大 */
    FILE *f=fopen(path,"rb");
    if(!f) return -1;
    if(fseeko(f,(off_t)(32+pack_pos),SEEK_SET)!=0){ fclose(f); return -1; }
    uint8_t *in=malloc((size_t)pack_size);
    /* ★ 输出缓冲多给 4 KB：7z 记的 unpackSize 可能比实际解出来的少 1 字节
     *   （实测这张包 154 vs 155），给足就不会 LZMA_BUF_ERROR。*/
    uint8_t *buf=malloc((size_t)u->unpack_size + 4096);
    if(!in || !buf){ free(in); free(buf); fclose(f); return -1; }
    if(fread(in,1,(size_t)pack_size,f)!=(size_t)pack_size){ free(in); free(buf); fclose(f); return -1; }
    fclose(f);

    lzma_filter filt[2]; memset(filt,0,sizeof filt);
    filt[1].id=LZMA_VLI_UNKNOWN;                 /* ★ 链尾必须显式写 UNKNOWN，否则 0 会被当成又一个 LZMA1 */
    if(u->codec_id==0x030101)       filt[0].id=LZMA_FILTER_LZMA1;
    else if(u->codec_id==0x21)      filt[0].id=LZMA_FILTER_LZMA2;
    else { free(in); free(buf); return -1; }
    if(lzma_properties_decode(&filt[0], NULL, u->props, u->props_len)!=LZMA_OK){
        free(in); free(buf); return -1;
    }
    /* ★ 用【流式】raw 解码器：.7z 的 LZMA1 流没有结束标记，buffer_decode 会在
     *   收尾时（甚至开头）报 DATA_ERROR；流式解码只要已经吐出数据就算成功。*/
    size_t cap=(size_t)u->unpack_size + 4096, out_pos=0;
    lzma_stream strm=LZMA_STREAM_INIT;
    lzma_ret r=lzma_raw_decoder(&strm, filt);
    if(r==LZMA_OK){
        strm.next_in=in;  strm.avail_in=(size_t)pack_size;
        strm.next_out=buf; strm.avail_out=cap;
        r=lzma_code(&strm, LZMA_RUN);
        while(r==LZMA_OK && strm.avail_in>0 && strm.avail_out>0)
            r=lzma_code(&strm, LZMA_RUN);
        out_pos=cap-strm.avail_out;
        lzma_end(&strm);
    }
    free(filt[0].options);
    free(in);
    if(out_pos==0){ free(buf); return -1; }
    *out=buf; *out_len=out_pos;
    return 0;
}

int img_7z_is_solid(const char *path, bool *out_solid){
    if(out_solid) *out_solid=false;
    if(!path) return IMG_EINVAL;
    FILE *f=fopen(path,"rb");
    if(!f) return IMG_ENOENT;
    uint8_t sig[32];
    if(fread(sig,1,sizeof sig,f)!=sizeof sig){ fclose(f); return IMG_EINVAL; }
    static const uint8_t MAGIC[6]={0x37,0x7A,0xBC,0xAF,0x27,0x1C};
    if(memcmp(sig,MAGIC,6)!=0){ fclose(f); return IMG_EINVAL; }
    uint64_t next_off=0, next_size=0;
    for(int i=0;i<8;i++) next_off |= (uint64_t)sig[12+i] << (8*i);
    for(int i=0;i<8;i++) next_size |= (uint64_t)sig[20+i] << (8*i);
    if(next_size==0 || next_size>64u*1024*1024){ fclose(f); return IMG_OK; }  /* 空包/离谱：按非固实 */
    uint8_t *nh=malloc((size_t)next_size);
    if(!nh){ fclose(f); return IMG_ENOMEM; }
    if(fseeko(f,(off_t)(32+next_off),SEEK_SET)!=0 || fread(nh,1,(size_t)next_size,f)!=(size_t)next_size){
        free(nh); fclose(f); return IMG_OK;
    }
    fclose(f);

    cur_t c={nh,(size_t)next_size,0};
    uint8_t *hdr=nh; size_t hdr_len=(size_t)next_size;
    uint8_t *decoded=NULL;
    bool solid=false;

    unsigned char t;
    if(rd_byte(&c,&t)!=0){ free(nh); return IMG_OK; }
    if(t==0x17){                                   /* kEncodedHeader：先解压头 */
        unpack_t u; memset(&u,0,sizeof u);
        uint64_t pack_pos=0, sizes[8]; int n_pack=0;
        if(parse_streamsinfo(&c,&u,&pack_pos,sizes,&n_pack,&solid)!=0){
            free(nh); return IMG_OK;
        }
        uint64_t psz = n_pack>0 ? sizes[0] : 0;
        D("encoded hdr: pack_pos=%llu psz=%llu unpack=%llu codec=%llx props=%zu",
          (unsigned long long)pack_pos,(unsigned long long)psz,
          (unsigned long long)u.unpack_size,(unsigned long long)u.codec_id,u.props_len);
        if(decode_header(path,pack_pos,psz,&u,&decoded,&hdr_len)!=0){
            free(decoded); free(nh); return IMG_OK;      /* 解不开就不下结论 */
        }
        free(nh);
        hdr=decoded;
        c.p=decoded; c.n=hdr_len; c.i=0;
        if(rd_byte(&c,&t)!=0){ free(decoded); return IMG_OK; }
        solid=false;                                   /* 头部里的标志才算数 */
    }
    if(t==0x01){                                   /* kHeader */
        for(;;){
            unsigned char tag;
            if(rd_byte(&c,&tag)!=0) break;
            if(tag==0x00) break;
            if(tag==0x02){                         /* ArchiveProperties */
                for(;;){
                    unsigned char p;
                    if(rd_byte(&c,&p)!=0 || p==0x00) break;
                    uint64_t sz;
                    if(rd_num(&c,&sz)!=0 || skip(&c,(size_t)sz)!=0) break;
                }
                continue;
            }
            if(tag==0x03){                         /* AdditionalStreamsInfo */
                unpack_t u; memset(&u,0,sizeof u);
                uint64_t pp=0, ss[8]; int np=0;
                if(parse_streamsinfo(&c,&u,&pp,ss,&np,&solid)!=0) break;
                continue;
            }
            if(tag==0x04){                         /* MainStreamsInfo */
                D("MainStreamsInfo at %zu", c.i);
                unpack_t u; memset(&u,0,sizeof u);
                uint64_t pp=0, ss[8]; int np=0;
                bool s2=false;
                if(parse_streamsinfo(&c,&u,&pp,ss,&np,&s2)!=0) break;
                D("MainStreamsInfo parsed, solid=%d", (int)s2);
                solid=s2;
                continue;
            }
            break;                                 /* 0x05 FilesInfo：不用看 */
        }
    }
    free(hdr);
    if(out_solid) *out_solid=solid;
    return IMG_OK;
}