/* thread_test — 并发契约验证
 *   ① 多线程同时写日志 → 日志必须【每行仍合法 JSON】（说明内部有锁）
 *   ② 多线程同时写环形缓冲 → 不崩
 *   ③ 不同镜像并行操作 → 都成功（各自句柄状态私有）
 *   ④ 同一 ctx 并发跑命令 → ctx 锁串行化，都成功
 * 用法: thread_test <镜像A> <镜像B> <ESP_LBA_A> <ESP_LBA_B> [日志路径]
 */
#define _POSIX_C_SOURCE 200809L
#include "img_cmd.h"
#include "img_event.h"
#include "img_io.h"
#include "img_qcow2.h"
#include "img_fat32_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails=0;
static void chk(const char *what, int cond){
    printf("  %s %s\n", cond?"✅":"❌", what);
    if(!cond) fails++;
}

#define NTH 4
#define NEV 250
static const char *g_log;

static void *th_log(void *arg){
    long id=(long)arg;
    for(int i=0;i<NEV;i++){
        img_ev e;
        if(img_ev_init(&e,IMG_LV_INFO,"test","spam",IMG_OK,"多线程日志")){
            img_ev_u64(&e,"tid",(uint64_t)id);
            img_ev_u64(&e,"i",(uint64_t)i);
            img_ev_emit(&e);
        }
        img_ring_add("{\"ev\":\"ring\",\"x\":1}");
    }
    return NULL;
}

/* 不同镜像并行：各自 img_qcow2 + img_fat32_fs，句柄状态私有 */
static const char *g_img[2]; static uint64_t g_lba[2];
static void *th_img(void *arg){
    long id=(long)arg;
    char src[256]; snprintf(src,sizeof src,"/tmp/tt_src_%ld.bin",id);
    FILE *f=fopen(src,"wb"); if(!f) return (void*)1;
    char buf[4096]; memset(buf,(int)('a'+id),sizeof buf);
    for(int i=0;i<16;i++) fwrite(buf,1,sizeof buf,f);
    fclose(f);

    img_io_opts_t io={0}; io.writable=true;
    img_io *fh=img_io_open_path(g_img[id],&io); if(!fh) return (void*)1;
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(fh,&qo);
    if(!q){ img_io_close(fh); return (void*)1; }
    img_qcow2_write_init(q);
    img_fat32_fs *fs=img_fat32_mount(q,g_lba[id],true);
    int bad=0;
    if(fs){
        char dst[128];
        for(int i=0;i<8;i++){
            snprintf(dst,sizeof dst,"/EFI/t%ld_%d.bin",id,i);
            if(img_fat32_put_file(fs,src,dst,NULL)!=IMG_OK) bad++;
        }
        img_fat32_unmount(fs);
    } else bad++;
    img_qcow2_close(q); img_io_close(fh);
    unlink(src);
    return (void*)(long)bad;
}

/* 同一 ctx 并发跑命令：img_cmd_exec 内建锁串行化 */
static img_ctx *g_ctx; static uint64_t g_lba_c; static int g_cmd_bad;
static void *th_cmd(void *arg){
    long id=(long)arg;
    char src[128]; snprintf(src,sizeof src,"/tmp/tt_cmd_%ld.bin",id);
    FILE *f=fopen(src,"wb"); if(f){ fwrite("hello",1,5,f); fclose(f); }
    for(int i=0;i<6;i++){
        img_args *a=img_args_new();
        char dst[96]; snprintf(dst,sizeof dst,"/EFI/c%ld_%d.bin",id,i);
        img_args_set(a,"lba","2048");
        img_args_set(a,"src",src);
        img_args_set(a,"dst",dst);
        img_result r; int rc=img_cmd_exec("fat.put",g_ctx,a,&r);
        if(rc!=IMG_OK || !r.ok){
            g_cmd_bad++;
            if(g_cmd_bad<=3)                       /* 打印前几条失败原因，便于定位 */
                printf("     ↳ 失败 tid=%ld i=%d rc=%d code=%s why=%s\n",
                       id,i,rc,r.ok?"(ok)":r.code, r.ok?"":r.why);
        }
        img_args_free(a);
    }
    unlink(src);
    return NULL;
}

int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"用法: %s <镜像A> <镜像B> <ESP_LBA_A> <ESP_LBA_B> [日志]\n",argv[0]); return 2; }
    g_img[0]=argv[1]; g_img[1]=argv[2];
    g_lba[0]=strtoull(argv[3],NULL,0); g_lba[1]=strtoull(argv[4],NULL,0);
    g_log = argc>5?argv[5]:"/tmp/thread_test.log";
    g_lba_c=g_lba[0];

    printf("════ ① 多线程并发写日志（%d 线程 × %d 条）════\n",NTH,NEV);
    unlink(g_log);
    if(img_log_open(g_log,IMG_LV_INFO,32u*1024*1024,1)!=IMG_OK){ printf("日志打不开\n"); return 1; }
    pthread_t t[NTH];
    for(long i=0;i<NTH;i++) pthread_create(&t[i],NULL,th_log,(void*)i);
    for(int i=0;i<NTH;i++) pthread_join(t[i],NULL);
    img_log_close();

    /* 逐行解析日志：并发写如果没锁，一定会出现半行/交错的坏 JSON */
    FILE *f=fopen(g_log,"r"); int lines=0,bad=0;
    char line[2048];
    while(f && fgets(line,sizeof line,f)){
        lines++;
        size_t L=strlen(line);
        if(!L || line[L-1]!='\n'){ bad++; continue; }          /* 半行 = 交错 */
        if(line[0]!='{' || line[L-2]!='}'){
            /* 简单合法性检查：首尾必须是 { }，且引号成对 */
            int q=0; for(size_t i=0;i<L;i++) if(line[i]=='"') q++;
            if(line[0]!='{'||q%2){ bad++; continue; }
        }
    }
    if(f) fclose(f);
    printf("     日志 %d 行，坏行 %d\n",lines,bad);
    chk("日志无交错坏行", bad==0);
    chk("日志行数 = 会话头 + 线程数×条数", lines>=NTH*NEV);

    printf("════ ② 环形缓冲并发写（随机 dump）════\n");
    {
        img_log_set_level(IMG_LV_DEBUG);   /* 让环形缓冲也收过程事件 */
        img_log_open(g_log,IMG_LV_DEBUG,32u*1024*1024,1);
        for(long i=0;i<NTH;i++) pthread_create(&t[i],NULL,th_log,(void*)i);
        for(int i=0;i<NTH;i++) pthread_join(t[i],NULL);
        chk("并发写环形缓冲未崩", 1);
        printf("     环形缓冲现有 %d 条\n", img_ring_count());
        img_log_close();
    }

    printf("════ ③ 不同镜像并行操作（各自句柄）════\n");
    {
        pthread_t t2[2]; long bad=0;
        for(long i=0;i<2;i++) pthread_create(&t2[i],NULL,th_img,(void*)i);
        for(int i=0;i<2;i++){ void*r; pthread_join(t2[i],&r); bad+=(long)r; }
        chk("两张盘并行各写 8 个文件都成功", bad==0);
    }

    printf("════ ④ 同一 ctx 并发跑命令（应被串行化）════\n");
    {
        char err[128];
        g_ctx=img_ctx_open(g_img[0],true,false,err,sizeof err);
        chk("ctx 打开", g_ctx!=NULL);
        if(g_ctx){
            pthread_t t3[NTH];
            for(long i=0;i<NTH;i++) pthread_create(&t3[i],NULL,th_cmd,(void*)i);
            for(int i=0;i<NTH;i++) pthread_join(t3[i],NULL);
            chk("并发命令全部成功（ctx 锁生效）", g_cmd_bad==0);
            img_ctx_close(g_ctx);
        }
    }
    printf("\n════ 结果：%s（失败 %d）════\n", fails?"❌ 有问题":"✅ 并发契约通过", fails);
    return fails?1:0;
}
