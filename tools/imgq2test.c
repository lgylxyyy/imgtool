/* 写入器测试：
 *   imgq2test create <out.qcow2> <size>
 *   imgq2test write  <qcow2>     （读回校验）
 */
#include "img_qcow2.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void on_prog(void *ud,const img_progress_t *p){ (void)ud;
    char b[2048]; if(img_prog_json(p,b,sizeof b)>0) fprintf(stderr,"%s\n",b); }

/* 确定性伪随机，保证两侧可复现 */
static void fill_pat(uint8_t *b, size_t n, uint64_t seed){
    uint64_t x = seed*2654435761ULL + 0x9e3779b97f4a7c15ULL;
    for(size_t i=0;i<n;i++){ x ^= x<<13; x^=x>>7; x^=x<<17; b[i]=(uint8_t)(x>>24); }
}

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imgq2test");
    if(argc<3){ fprintf(stderr,
        "用法:\n  %s create <out.qcow2> <bytes>\n  %s write  <in.qcow2>\n  %s expand <qcow2> <new_bytes>\n",
        argv[0],argv[0],argv[0]); return 2; }

    if(!strcmp(argv[1],"create")){
        uint64_t sz = strtoull(argv[3],NULL,0);
        img_io_opts_t io={0}; io.writable=true; io.initial_size=0;
        img_io *f = img_io_open_path(argv[2],&io);
        if(!f){ fprintf(stderr,"❌ 无法打开输出文件\n"); return 1; }
        img_qcow2_create_opts_t co={0};
        co.size = sz; co.cluster_bits = 16; co.compressed=false;
        int rc = img_qcow2_create(f,&co);
        if(rc){ fprintf(stderr,"❌ create: %s\n",img_strerror(rc)); img_io_close(f); return 1; }
        img_io_close(f);
        printf("✅ 已创建 %llu 字节的 qcow2\n",(unsigned long long)sz);
        return 0;
    }

    if(!strcmp(argv[1],"write")||!strcmp(argv[1],"roundtrip")){
        img_io_opts_t io={0}; io.writable=true;
        img_io *f = img_io_open_path(argv[2],&io);
        if(!f){ fprintf(stderr,"❌ 无法打开\n"); return 1; }
        img_qcow2_opts_t qo={0}; qo.cb=on_prog;
        img_qcow2 *q = img_qcow2_open(f,&qo);
        if(!q){ fprintf(stderr,"❌ 不是 qcow2\n"); img_io_close(f); return 1; }
        if(img_qcow2_write_init(q)!=IMG_OK){ fprintf(stderr,"❌ write_init\n"); return 1; }

        uint64_t vs = img_qcow2_size(q);
        printf("  虚拟大小 %llu 字节\n",(unsigned long long)vs);

        /* 写若干处：0 / 簇边界 / 盘中部 / 尾部 / 非对齐 */
        struct { uint64_t off; size_t len; uint64_t seed; const char*name; } cases[] = {
            {0,               4096,  1, "MBR区(0)"},
            {65536,           1024,  2, "簇边界(64KB)"},
            {1048576,         70000, 3, "跨簇写(1MB, 70KB)"},
            {vs/2 + 20971520, 4096,  4, "盘中部"},
            {vs-4096,         4096,  5, "盘尾"},
            {12345,            333,  6, "非对齐偏移"},
            {2147483648ULL,   16384, 7, "2GB 处(可能要新建L2)"},
        };
        int ncase = (int)(sizeof cases/sizeof cases[0]);
        int ok=0;
        uint8_t *wbuf=NULL; uint8_t *rbuf=NULL; size_t wcap=0,rcap=0;
        for(int i=0;i<ncase;i++){
            if(cases[i].off + cases[i].len > vs){ printf("  ⏭  %s — 超出虚拟盘，跳过\n",cases[i].name); continue; }
            if(cases[i].len>wcap){ free(wbuf); wbuf=malloc(cases[i].len); wcap=cases[i].len; }
            if(cases[i].len>rcap){ free(rbuf); rbuf=malloc(cases[i].len); rcap=cases[i].len; }
            if(!wbuf||!rbuf){ printf("  ❌ 内存\n"); break; }
            fill_pat(wbuf, cases[i].len, cases[i].seed);

            int rc = img_qcow2_write(q, cases[i].off, wbuf, cases[i].len);
            if(rc){ printf("  ❌ %s — 写失败: %s\n", cases[i].name, img_strerror(rc)); continue; }
            memset(rbuf,0xAA,cases[i].len);
            rc = img_qcow2_read(q, cases[i].off, rbuf, cases[i].len);
            if(rc){ printf("  ❌ %s — 读失败: %s\n", cases[i].name, img_strerror(rc)); continue; }
            if(memcmp(wbuf,rbuf,cases[i].len)==0){ printf("  ✅ %s  读回一致 (%zu 字节)\n",cases[i].name,cases[i].len); ok++; }
            else {
                size_t bad=0; for(size_t k=0;k<cases[i].len;k++) if(wbuf[k]!=rbuf[k]) bad++;
                printf("  ❌ %s  有 %zu 字节不符\n",cases[i].name,bad);
            }
        }
        free(wbuf); free(rbuf);
        img_io_sync(f);
        printf("\n结果: %d/%d 项读回一致\n", ok, ncase);
        printf("文件大小: %llu 字节 (含 refcount/L1/L2/数据)\n",(unsigned long long)img_io_size(f));
        img_qcow2_close(q); img_io_close(f);
        return ok==ncase ? 0 : 1;
    }

    if(!strcmp(argv[1],"expand")){
        img_io_opts_t io={0}; io.writable=true;
        img_io *f = img_io_open_path(argv[2],&io);
        if(!f){ fprintf(stderr,"❌ 无法打开\n"); return 1; }
        img_qcow2_opts_t qo={0};
        img_qcow2 *q = img_qcow2_open(f,&qo);
        if(!q){ fprintf(stderr,"❌ 不是 qcow2\n"); return 1; }
        if(img_qcow2_write_init(q)!=IMG_OK){ fprintf(stderr,"❌ init\n"); return 1; }
        uint64_t before = img_qcow2_size(q);
        uint64_t want = strtoull(argv[3],NULL,0);
        img_prog *pr = img_prog_new(on_prog,NULL,0);
        img_prog_phase(pr, IMG_PH_EXPAND, "扩展磁盘");
        int rc = img_qcow2_expand(q, want, pr);
        if(rc){ fprintf(stderr,"❌ expand: %s\n",img_strerror(rc)); return 1; }
        printf("✅ %llu → %llu 字节\n",(unsigned long long)before,(unsigned long long)img_qcow2_size(q));
        img_prog_free(pr);
        img_qcow2_close(q); img_io_close(f);
        return 0;
    }
    fprintf(stderr,"未知子命令 %s\n",argv[1]);
    return 2;
}
