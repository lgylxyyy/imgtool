/* identify_test — 版本识别的"多源投票"逻辑（不需要真盘）
 *   钉住三件事：
 *     1) build 分"代"：19041/19045 同代（Win10 启用包升级是正常的）；
 *     2) 跨代才叫 mismatch（7601 vs 19041）；
 *     3) 平票时取注册表那一票。
 *   真实盘的端到端验证用 tools/winver.sh（需用户镜像，不进回归）。*/
#include "img_identify.h"
#include <stdio.h>
#include <string.h>
static int fails=0;
#define OK(c,m) do{ if(c) printf("  ✅ %s\n", m); else { printf("  ❌ %s\n", m); fails++; } }while(0)
static void put(img_winver_t *v,const char *src,int build){
    snprintf(v->vote[v->n_vote].src,sizeof v->vote[0].src,"%s",src);
    v->vote[v->n_vote].build=build; v->n_vote++;
}
int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("identify_test — 多源版本投票\n");

    OK(img_build_family(7600)==img_build_family(7601), "7600 与 7601 同代（Win7 RTM/SP1）");
    OK(img_build_family(19041)==img_build_family(19045), "19041 与 19045 同代（Win10 启用包）");
    OK(img_build_family(22000)==img_build_family(22631), "22000 与 22631 同代（都是 Win11）");
    OK(img_build_family(26100)==img_build_family(26300), "★ 26100 与 26300 同代（实测误报过）");
    OK(img_build_family(19045)!=img_build_family(22000), "Win10(19045) 与 Win11(22000) 不同代");
    OK(img_build_family(9200)!=img_build_family(9600), "Win8(9200) 与 Win8.1(9600) 不同代");
    OK(img_build_family(6000)!=img_build_family(7601), "Vista(6000) 与 Win7(7601) 不同代");
    OK(img_build_family(0)==0, "0 → 0（没有证据）");

    { img_winver_t v; memset(&v,0,sizeof v);
      put(&v,"registry",7601); put(&v,"ntoskrnl",7601); put(&v,"ntdll",7601);
      put(&v,"winload.exe",7601); put(&v,"winload.efi",7601); put(&v,"servicing",7601);
      img_winver_vote(&v);
      OK(v.consensus_build==7601 && v.mismatch==0, "Win7：6 票一致 → 7601，不报 mismatch"); }

    { img_winver_t v; memset(&v,0,sizeof v);
      put(&v,"registry",19045); put(&v,"winload.exe",19041); put(&v,"winload.efi",19041);
      put(&v,"servicing",19041);
      img_winver_vote(&v);
      OK(v.mismatch==0, "Win10 22H2：注册表 19045 + 文件 19041 → 同代，不算被改");
      OK(v.consensus_build==19045, "平票取注册表 → 19045（用户看到的版本号）"); }

    { img_winver_t v; memset(&v,0,sizeof v);
      put(&v,"registry",7601); put(&v,"ntoskrnl",7601);
      put(&v,"winload.exe",19041); put(&v,"winload.efi",19041);
      img_winver_vote(&v);
      OK(v.mismatch==1, "注册表说 Win7、引导器是 Win10 → 跨代，判为被改过");
      OK(v.consensus_build==7601, "平票时仍取注册表那一票"); }

    { img_winver_t v; memset(&v,0,sizeof v);
      put(&v,"winload.exe",19041);
      img_winver_vote(&v);
      OK(v.consensus_build==19041 && v.mismatch==0, "只有 1 票：给出结论但不报矛盾"); }

    { img_winver_t v; memset(&v,0,sizeof v);
      img_winver_vote(&v);
      OK(v.consensus_build==0 && v.mismatch==0, "0 票：consensus_build=0，不崩"); }

    printf(fails? "\n❌ identify_test 失败 %d 项\n" : "\n✅ identify_test 全过\n", fails);
    return fails?1:0;
}
