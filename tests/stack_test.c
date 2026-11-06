/* stack_test — REQ-20261026-21：在【1 MB 栈】的线程里跑"会吃栈"的真命令
 *
 * 为什么要有它：App 的工作线程（ART DefaultDispatch）只有 ~1 MB 栈。
 *   fs.format → mkntfs（bitmap_allocate）、recovery.populate → wimlib
 *   （open_wim_as_WIMStruct）各吃过一次 SIGSEGV —— 整个进程闪退，宿主 catch 不住。
 *   imgcmd 是主线程 8 MB 栈，所以自测一直是绿的：必须【在 1 MB 线程里】跑才算数。
 *
 * 用法：stack_test <qcow2> <sys_lba> <rec_lba> <ISO>
 *   前置（由 stack-test.sh 准备）：盘已建好、分区对；ISO 里有 install.wim 且内含 WinRE。
 * 退出码：0 全过 · 1 有失败 · 2 参数/环境不对
 */
#include "img_cmd.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_img, *g_iso;
static long long g_sys, g_rec;
static int g_pass, g_fail;

static char *u64s(char *b, size_t cap, long long v){ snprintf(b,cap,"%lld",v); return b; }

/* 单条命令：ctx 由外面传（同一条命令序列共用一个句柄，和 App 一样）*/
static void step(img_ctx *ctx, const char *name, img_args *a, int want_ok){
    img_result *r=(img_result*)malloc(sizeof *r);      /* ★ 不占本线程栈 */
    if(!r){ printf("  ❌ %s: malloc 失败\n",name); g_fail++; return; }
    memset(r,0,sizeof *r);
    int rc=img_cmd_exec(name,ctx,a,r);
    char *js=(char*)malloc(64*1024);
    if(js) img_result_json(r,js,64*1024);
    int ok = (rc==IMG_OK) == (want_ok!=0);
    if(ok){ g_pass++; printf("  ✅ %s（rc=%d）\n",name,rc); }
    else  { g_fail++; printf("  ❌ %s（rc=%d）%s\n",name,rc,js?js:""); }
    free(js); free(r);
}

static void *worker(void *ud){
    (void)ud;
    char sb[32], rb[32];
    /* ★ 0：先在 1 MB 栈里确认一下（这条只是热身，失败也不致命）*/
    img_args *a;

    /* ① fs.format：mkntfs 在 8 GiB 分区上建位图（App 崩过的那条）*/
    img_ctx *ctx=img_ctx_open(g_img,true,false,NULL,0);
    if(!ctx){ printf("  ❌ 打不开镜像 %s\n",g_img); g_fail++; return NULL; }
    a=img_args_new();
    img_args_set(a,"img",g_img); img_args_set(a,"lba",u64s(sb,sizeof sb,g_sys));
    img_args_set(a,"fs","ntfs"); img_args_set(a,"label","Windows");
    step(ctx,"fs.format",a,1);
    img_args_free(a);

    /* ② wim.apply：ISO 直读 + wimlib 解压（wimlib 打开路径也吃栈）*/
    a=img_args_new();
    img_args_set(a,"img",g_img); img_args_set(a,"lba",u64s(sb,sizeof sb,g_sys));
    img_args_set(a,"wim",g_iso); img_args_set(a,"index","1"); img_args_set(a,"format","on");
    step(ctx,"wim.apply",a,1);
    img_args_free(a);

    /* ③ recovery.populate：winre.wim 从 ISO 直读（tombstone 那条）
     *    ★ 这条不许 format=true 之外的写法：App 就是这么调的 */
    a=img_args_new();
    img_args_set(a,"img",g_img); img_args_set(a,"rec-lba",u64s(rb,sizeof rb,g_rec));
    img_args_set(a,"winre-src",g_iso); img_args_set(a,"winre-index","1");
    img_args_set(a,"format","true"); img_args_set(a,"label","Recovery");
    step(ctx,"recovery.populate",a,1);
    img_args_free(a);

    /* ④ boot.populate：连带着 boot.check 也在这条栈上跑 */
    a=img_args_new();
    img_args_set(a,"img",g_img); img_args_set(a,"sys-lba",u64s(sb,sizeof sb,g_sys));
    step(ctx,"boot.populate",a,1);
    img_args_free(a);

    /* ⑤ 再跑一遍 recovery.populate（覆盖已存在文件的路径，栈深度不同）*/
    a=img_args_new();
    img_args_set(a,"img",g_img); img_args_set(a,"rec-lba",u64s(rb,sizeof rb,g_rec));
    img_args_set(a,"winre-src",g_iso); img_args_set(a,"winre-index","1");
    img_args_set(a,"format","true"); img_args_set(a,"label","Recovery");
    step(ctx,"recovery.populate",a,1);
    img_args_free(a);

    img_ctx_close(ctx);
    return NULL;
}

int main(int argc, char **argv){
    if(argc<5){
        fprintf(stderr,"用法：%s <qcow2> <sys_lba> <rec_lba> <ISO>\n",argv[0]);
        return 2;
    }
    g_img=argv[1]; g_sys=atoll(argv[2]); g_rec=atoll(argv[3]); g_iso=argv[4];

    printf("════ 1 MB 栈线程里跑真命令（模拟 App 的 ART worker）════\n");
    pthread_attr_t at;
    pthread_t th;
    if(pthread_attr_init(&at)!=0){ perror("attr_init"); return 2; }
    if(pthread_attr_setstacksize(&at,1024u*1024u)!=0){ perror("stacksize"); return 2; }
    if(pthread_create(&th,&at,worker,NULL)!=0){ perror("pthread_create"); return 2; }
    pthread_attr_destroy(&at);
    pthread_join(th,NULL);

    printf("\n════ 结果：通过 %d · 失败 %d ════\n",g_pass,g_fail);
    return g_fail?1:0;
}
