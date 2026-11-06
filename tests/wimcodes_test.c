/* wimcodes_test.c — REQ-48 §1：wimlib 错误码 → 枚举名/中文含义
 * 只守两件事：① 46 必须叫 WIMLIB_ERR_NTFS_3G 并且有中文解释（App 按码分流）；
 *             ② 未知码返回 NULL（不许编一个名字出来）。*/
#include <stdio.h>
#include <string.h>
#include "img_wim_apply.h"

int main(void){
    int bad=0;
    struct { int code; const char *name; } want[] = {
        {41,"WIMLIB_ERR_NOTEMPTY"},
        {46,"WIMLIB_ERR_NTFS_3G"},
        {47,"WIMLIB_ERR_OPEN"},
        {49,"WIMLIB_ERR_PATH_DOES_NOT_EXIST"},
        {18,"WIMLIB_ERR_INVALID_IMAGE"},
    };
    for(unsigned i=0;i<sizeof want/sizeof want[0];i++){
        const char *n=img_wimlib_err_name(want[i].code);
        if(!n || strcmp(n,want[i].name)){
            printf("FAIL code %d -> %s（要 %s）\n",want[i].code,n?n:"(null)",want[i].name);
            bad=1;
        }
    }
    if(!img_wimlib_err_zh(46) || !strstr(img_wimlib_err_zh(46),"ntfs-3g")){
        printf("FAIL 46 中文含义缺失/不对：%s\n",img_wimlib_err_zh(46)?img_wimlib_err_zh(46):"(null)");
        bad=1;
    }
    if(img_wimlib_err_name(999)!=NULL){ printf("FAIL 未知码应返回 NULL\n"); bad=1; }
    if(img_wimlib_err_zh(999)!=NULL){ printf("FAIL 未知码中文应返回 NULL\n"); bad=1; }
    printf(bad?"wimcodes_test：FAIL\n":"wimcodes_test：OK（5 个码 + 未知码）\n");
    return bad;
}