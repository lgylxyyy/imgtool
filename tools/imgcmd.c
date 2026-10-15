/* imgcmd — 命令表的统一入口（App 走 JNI 调同一个 img_cmd_exec，这里只是 CLI 外壳）
 *
 *   imgcmd --list                 列出所有命令（含参数规格/帮助）
 *   imgcmd --help=<命令>          单个命令用法（从表自动生成）
 *   imgcmd <命令> [--参数=值 ...]  执行
 *   全局：--log=路径|--log=-   --log-level=error|info|debug|trace   --log-max=4M   --ro
 *
 * 结果：stdout 一行 JSON 信封
 *   {"ok":true,"cmd":"fat.put","v":1,"ms":41,"data":{...}}
 *   {"ok":false,"cmd":..,"err":{"rc":-10,"code":"FAT-ENOSPC","at":"..","why":"..","suggest":".."}}
 * 退出码：0=成功 1=失败 2=用法错
 */
#include "img_cmd.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_help(const img_cmd *c){
    if(!c){ printf("没有这个命令\n"); return; }
    printf("%s  (since %s)\n  %s\n", c->name, c->since?c->since:"", c->help?c->help:"");
    if(c->nargs){
        printf("  参数：\n");
        for(int i=0;i<c->nargs;i++){
            const img_arg *a=&c->args[i];
            const char *ty="字符串";
            switch(a->ty){ case IMG_A_U64: ty="数字"; break; case IMG_A_I64: ty="整数"; break;
                           case IMG_A_BOOL: ty="布尔"; break; case IMG_A_ENUM: ty="枚举"; break; default: break; }
            printf("    --%-12s %-6s %s%*s%s\n", a->name, ty,
                   a->required?"必填  ":"可选  ", a->def?8:0, a->def?a->def:"",
                   a->help? a->help :"");
        }
    }
}

int main(int argc, char **argv){
    const char *cmd=NULL, *log=NULL, *lvl=NULL, *helpcmd=NULL, *logmax=NULL, *progress=NULL;
    bool list=false, ro=false;
    img_args *args=img_args_new();
    if(!args) return 1;

    for(int i=1;i<argc;i++){
        const char *a=argv[i];
        if(a[0]!='-'){
            if(!cmd){ cmd=a; continue; }
            fprintf(stderr,"多余的位置参数：%s\n",a);
            img_args_free(args); return 2;
        }
        const char *eq=strchr(a,'=');
        char key[64]; const char *val=eq?eq+1:"";
        if(eq){ size_t n=(size_t)(eq-a-2); if(n>=sizeof key) n=sizeof key-1; memcpy(key,a+2,n); key[n]=0; }
        else   snprintf(key,sizeof key,"%s",a+2);

        if(!strcmp(key,"list")){ list=true; continue; }
        if(!strcmp(key,"help")){ helpcmd=eq?val:NULL; if(!eq) list=true; continue; }
        if(!strcmp(key,"ro")){ ro=true; continue; }
        if(!strcmp(key,"log")){ log=val; continue; }
        if(!strcmp(key,"log-level")){ lvl=val; continue; }
        if(!strcmp(key,"log-max")){ logmax=val; continue; }
        if(!strcmp(key,"lang")){ img_set_lang(val); continue; }
        if(!strcmp(key,"progress")){ progress=val; continue; }
        if(!eq){ fprintf(stderr,"参数 %s 缺少 =值（例如 --lba=2048）\n",a); img_args_free(args); return 2; }
        img_args_set(args,key,val);
    }

    if(list){
        static char buf[16384];
        if(img_cmd_list_json(buf,sizeof buf)>0) fputs(buf,stdout);
        img_args_free(args); return 0;
    }
    if(helpcmd){ print_help(img_cmd_find(helpcmd)); img_args_free(args); return 0; }
    if(!cmd){ fprintf(stderr,"用法: imgcmd <命令> [--参数=值 ...] | --list | --help=<命令>\n"); img_args_free(args); return 2; }

    { const char *el=getenv("IMGTOOL_LANG"); if(el&&*el) img_set_lang(el); }   /* env 兜底 */
    /* 屏幕输出形态：默认 tty → bar（人话行），非 tty → json；可用 --progress 强制 */
    {
        bool human;
        if(progress) human = !strcmp(progress,"bar");
        else         human = isatty(2);
        img_out_open_fd(2, human);
        img_log_set_level(progress && !strcmp(progress,"none") ? IMG_LV_ERROR : IMG_LV_INFO);
    }
    if(lvl){
        img_lv_t l=IMG_LV_INFO;
        if(!strcmp(lvl,"error")) l=IMG_LV_ERROR;
        else if(!strcmp(lvl,"warn")) l=IMG_LV_WARN;
        else if(!strcmp(lvl,"debug")) l=IMG_LV_DEBUG;
        else if(!strcmp(lvl,"trace")) l=IMG_LV_TRACE;
        img_log_set_level(l);
    }
    if(log){
        size_t maxb=4u*1024*1024;
        if(logmax){ uint64_t v=strtoull(logmax,NULL,0); if(v>0) maxb=(size_t)v; }
        int rc = !strcmp(log,"-") ? img_log_open_fd(2,img_log_level(),maxb,1)
                                 : img_log_open(log,img_log_level(),maxb,1);
        if(rc!=IMG_OK) fprintf(stderr,"警告：日志打不开（%s），继续执行\n",log);
        else {
            char line[512]; int k=0;
            k+=snprintf(line+k,sizeof line-(size_t)k,"imgcmd %s",cmd);
            for(int i=0;i<img_args_count(args);i++){ }   /* 参数在 ctx 里统一打印 */
            img_log_session("imgcmd",cmd,img_args_get(args,"img"),0);
            char ctxline[512]; snprintf(ctxline,sizeof ctxline,"imgcmd %s",cmd);
            img_card_set_context(ctxline);
        }
    }

    const char *img=img_args_get(args,"img");
    char err[256]="";
    img_ctx *ctx=NULL;
    if(img){
        ctx=img_ctx_open(img, !ro, img_args_bool(args,"check_size",false), err, sizeof err);
        if(!ctx){
            img_result r; img_result_begin(&r,cmd);
            img_result_fail(&r,IMG_ENOENT,"IMG-OPEN-FAIL",img,err,"检查路径/权限，确认文件存在");
            char b[1024]; int n=img_result_json(&r,b,sizeof b);
            if(n>0) fwrite(b,1,(size_t)n,stdout);
            img_args_free(args); return 1;
        }
    }

    img_result res;
    int rc=img_cmd_exec(cmd, ctx, args, &res);
    static char out[128*1024];
    int n=img_result_json(&res,out,sizeof out);
    if(n>0) fwrite(out,1,(size_t)n,stdout);
    if(ctx) img_ctx_close(ctx);
    img_log_close();
    img_args_free(args);
    return rc==IMG_OK?0:1;
}
