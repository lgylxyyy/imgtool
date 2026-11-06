/* winre_chain — REQ-25 ①：同一进程里按 App 的真实顺序连跑，看 WinRE 检测会不会"第二次就找不到"
 *
 * 顺序（App 是这么调的）：
 *   wim.identify --scan=true  →  wim.size --inner  →  winre.find --inner
 *   →  winre.find（index=0，App 的兜底）  →  wim.apply（真灌，可选）  →  winre.find 再探
 *
 * 为什么需要它：imgcmd 一个进程一条命令，所以"前一条命令留下的状态"永远测不出来。
 *   最可疑的是 img_vfd 的槽位（4 个）和 ISO 句柄 —— 漏一个，后面就静默降级。
 *
 * 用法：winre_chain <ISO> [index]
 * 退出码：0 全过 · 1 有失败
 */
#include "img_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_bad;

static img_result *run(const char *name, img_args *a){
    img_result *r=(img_result*)malloc(sizeof *r);
    memset(r,0,sizeof *r);
    int rc=img_cmd_exec(name,NULL,a,r);
    char *js=(char*)malloc(16*1024);
    if(js) img_result_json(r,js,16*1024);
    printf("  %s %s（rc=%d）%s\n", rc==IMG_OK?"✅":"❌", name, rc,
           rc==IMG_OK?"":(js?js:""));
    free(js);
    return r;
}

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"用法：%s <ISO> [index]\n",argv[0]); return 2; }
    const char *iso=argv[1];
    char idx[16]="1"; if(argc>2) snprintf(idx,sizeof idx,"%s",argv[2]);
    img_args *a;

    printf("════ ① wim.identify --scan=true ════\n");
    a=img_args_new(); img_args_set(a,"src",iso); img_args_set(a,"scan","true");
    img_result *r1=run("wim.identify",a); img_args_free(a);
    if(!r1->ok) g_bad++;

    /* 从 scan 结果里取第一条候选的 inner_path（App 就是这么拿的）—— 这里用固定值 + 参数兜底 */
    char inner[256]="/sources/install.wim";
    const char *ip=getenv("INNER"); if(ip&&*ip) snprintf(inner,sizeof inner,"%s",ip);

    printf("════ ② wim.size --inner=%s ════\n", inner);
    a=img_args_new(); img_args_set(a,"wim",iso); img_args_set(a,"inner",inner);
    img_args_set(a,"index",idx);
    img_result *r2=run("wim.size",a); img_args_free(a);
    if(!r2->ok) g_bad++;

    printf("════ ③ winre.find --inner=%s --index=%s ════\n", inner, idx);
    a=img_args_new(); img_args_set(a,"src",iso); img_args_set(a,"inner",inner);
    img_args_set(a,"index",idx);
    img_result *r3=run("winre.find",a); img_args_free(a);
    if(!r3->ok) g_bad++;

    printf("════ ④ winre.find --index=0（App 的兜底：扫全部映像）════\n");
    a=img_args_new(); img_args_set(a,"src",iso); img_args_set(a,"inner",inner);
    img_args_set(a,"index","0");
    img_result *r4=run("winre.find",a); img_args_free(a);
    if(!r4->ok) g_bad++;

    printf("════ ⑤ winre.find 不带 --inner（走自动定位）════\n");
    a=img_args_new(); img_args_set(a,"src",iso); img_args_set(a,"index",idx);
    img_result *r5=run("winre.find",a); img_args_free(a);
    if(!r5->ok) g_bad++;

    printf("════ ⑥ 再来一轮（第 6~8 次打开同一个 ISO）════\n");
    for(int i=0;i<3;i++){
        a=img_args_new(); img_args_set(a,"src",iso); img_args_set(a,"inner",inner);
        img_args_set(a,"index",idx);
        img_result *rn=run("winre.find",a); img_args_free(a);
        if(!rn->ok) g_bad++;
        free(rn);
    }

    free(r1); free(r2); free(r3); free(r4); free(r5);
    printf("\n════ 结果：%s ════\n", g_bad?"有失败":"全过");
    return g_bad?1:0;
}
