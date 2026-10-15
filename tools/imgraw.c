/* imgraw — 原始区段读写
 *  imgraw read  <img> <offset> <len> > out.bin
 *  imgraw write <img> <offset> in.bin
 *  imgraw seek  <img> <offset> <len>   （打印前64字节 hex 用于核对）*/
#include "img_qcow2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"用法: %s read|write|seek <img> <offset> <len|file>\n",argv[0]); return 2; }
    const char *mode=argv[1];
    img_io_opts_t io={0}; io.writable = strcmp(mode,"read")&&strcmp(mode,"seek") ? true : true;
    img_io *f=img_io_open_path(argv[2],&io);
    if(!f){ printf("❌ 打不开\n"); return 1; }
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK){ printf("❌ qcow2\n"); return 1; }
    uint64_t off=strtoull(argv[3],NULL,0);
    if(!strcmp(mode,"read")){
        size_t len=(size_t)strtoull(argv[4],NULL,0);
        unsigned char *b=malloc(len);
        if(!b||img_qcow2_read_full(q,off,b,len)!=IMG_OK){ printf("❌ 读失败\n"); return 1; }
        fwrite(b,1,len,stdout); free(b);
    } else if(!strcmp(mode,"write")){
        FILE *fp=fopen(argv[4],"rb");
        if(!fp){ printf("❌ 打不开输入文件\n"); return 1; }
        fseek(fp,0,SEEK_END); long n=ftell(fp); fseek(fp,0,SEEK_SET);
        unsigned char *b=malloc((size_t)n);
        if(!b||fread(b,1,(size_t)n,fp)!=(size_t)n){ printf("❌ 读输入失败\n"); return 1; }
        fclose(fp);
        int rc=img_qcow2_write(q,off,b,(size_t)n);
        if(rc){ printf("❌ 写失败: %s\n",img_strerror(rc)); return 1; }
        printf("  ✅ 已写入 %ld 字节 @ offset %llu\n", n,(unsigned long long)off);
        free(b);
    } else {
        size_t len=(size_t)strtoull(argv[4],NULL,0);
        if(len>256) len=256;
        unsigned char b[256];
        if(img_qcow2_read_full(q,off,b,len)!=IMG_OK){ printf("❌\n"); return 1; }
        for(size_t i=0;i<len;i++){ printf("%02x",b[i]); if(i%32==31) printf("\n"); else printf(" "); }
        printf("\n");
    }
    return 0;
}
