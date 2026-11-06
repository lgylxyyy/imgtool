/* dl_cmd — 用 dlopen 加载 libimgtool.so 再跑一条命令（REQ-27）
 *
 * 为什么必须单独有这个壳：`imgcmd` 是**可执行文件**，`-Wl,--wrap=` 在它的最终链接里生效；
 * 而 App 用的是预编译的 .so —— 很多"CLI 好、App 坏"的问题只在 .so 里出现：
 *   · REQ-18：__real_* 未定义 → 整库 dlopen 失败；
 *   · REQ-27：--wrap= 挂在 INTERFACE 上 → .so 内部没人改道 → VFD 假路径全废。
 * 所以：**凡是 ISO 直读相关的改动，都要先过这个壳。**
 *
 * 用法：dl_cmd <libimgtool.so> <命令> [--参数=值 ...]
 * 退出码：0 命令 ok · 1 命令失败 · 2 用法/加载错误
 */
#include "img_cmd.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef img_args *(*fn_args_new)(void);
typedef int       (*fn_args_set)(img_args*, const char*, const char*);
typedef void      (*fn_args_free)(img_args*);
typedef int       (*fn_exec)(const char*, img_ctx*, const img_args*, img_result*);
typedef int       (*fn_result_json)(const img_result*, char*, int);
/* ★ 带 --img 的命令（part.show / wim.apply / fs.* …）必须由调用方先 img_ctx_open，
 *   否则库内部 img_ctx_path(ctx) 拿到 NULL → 一律 PART-READ-FAIL「参数非法」。
 *   以前这个壳没做，等于"只测了 --src/--wim 那半边"。*/
typedef img_ctx  *(*fn_ctx_open)(const char*, bool, bool, char*, size_t);
typedef void      (*fn_ctx_close)(img_ctx*);

/* dlsym 返回 void*，转函数指针要过 memcpy（-Wpedantic 下直接强转会报警）*/
#define SYM(dst, type, sym) do{ void *p_=dlsym(h,sym); memcpy(&(dst),&p_,sizeof p_); }while(0)

int main(int argc, char **argv){
    if(argc<3){
        fprintf(stderr,"用法：%s <libimgtool.so> <命令> [--参数=值 ...]\n",argv[0]);
        return 2;
    }
    void *h=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!h){ fprintf(stderr,"dlopen 失败：%s\n",dlerror()); return 2; }

    fn_args_new    args_new   =NULL;
    fn_args_set    args_set   =NULL;
    fn_args_free   args_free  =NULL;
    fn_exec        exec       =NULL;
    fn_result_json result_json=NULL;
    fn_ctx_open    ctx_open   =NULL;
    fn_ctx_close   ctx_close  =NULL;
    SYM(args_new,   fn_args_new,   "img_args_new");
    SYM(args_set,   fn_args_set,   "img_args_set");
    SYM(args_free,  fn_args_free,  "img_args_free");
    SYM(exec,       fn_exec,       "img_cmd_exec");
    SYM(result_json,fn_result_json,"img_result_json");
    SYM(ctx_open,   fn_ctx_open,   "img_ctx_open");
    SYM(ctx_close,  fn_ctx_close,  "img_ctx_close");
    if(!args_new||!args_set||!exec||!result_json){
        fprintf(stderr,"缺符号（so 版本太旧？缺 img_args_new/img_args_set/img_cmd_exec/img_result_json 之一）\n");
        return 2;
    }

    img_args *a=args_new();
    const char *imgpath=NULL;
    for(int i=3;i<argc;i++){
        const char *kv=argv[i];
        while(*kv=='-') kv++;
        const char *eq=strchr(kv,'=');
        char k[64];
        if(eq){
            size_t n=(size_t)(eq-kv); if(n>=sizeof k) n=sizeof k-1;
            memcpy(k,kv,n); k[n]=0;
            args_set(a,k,eq+1);
        } else args_set(a,kv,"true");
        if(!strcmp(k,"img") && eq) imgpath=eq+1;
    }

    img_ctx *ctx=NULL;
    if(imgpath && ctx_open && ctx_close){
        char cerr[256]="";
        ctx=ctx_open(imgpath,true,false,cerr,sizeof cerr);   /* 可写：wim.apply/fs.format 要用 */
        if(!ctx){
            fprintf(stderr,"img_ctx_open 失败：%s\n",cerr);
            return 2;
        }
    }
    img_result *r=(img_result*)calloc(1,sizeof *r);
    int rc=exec(argv[2],ctx,a,r);
    char *js=(char*)malloc(256*1024);
    if(js){ js[0]=0; result_json(r,js,256*1024); fputs(js,stdout); fputc('\n',stdout); }
    free(js); free(r);
    if(ctx && ctx_close) ctx_close(ctx);
    if(args_free) args_free(a);
    return rc==0?0:1;
}
