/* imggenbcd — 为新盘生成可引导的 BCD
 *  用法: imggenbcd <参考BCD> <输出BCD> <系统分区partGuid> <磁盘diskGuid> [WIM路径]
 *  做的事：拷参考 BCD 里 OS 加载器的 device 对象 → 换成新盘的分区/磁盘 GUID
 *         （换了盘不改的话 bootmgr 解析不到设备 → 0xc0000098）*/
#include "img_bcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char**argv){
    if(argc<5){ fprintf(stderr,"用法: %s <参考BCD> <输出BCD> <partGuid> <diskGuid>\n",argv[0]); return 2; }
    img_bcd *b = img_bcd_open_file(argv[1]);
    if(!b){ printf("❌ 打不开参考 BCD\n"); return 1; }
    char buf[64];
    if(img_bcd_get_display_order_first(b, buf, sizeof buf)!=IMG_OK){
        printf("❌ 参考 BCD 没有 DisplayOrder\n"); return 1;
    }
    printf("  参考加载器: %s\n", buf);
    char *id = buf; if(id[0]=='{') id++;
    char guid[64]; snprintf(guid,sizeof guid,"%s",id);
    char *c = strchr(guid, '}');
    if(c) *c = 0;

    /* OS 加载器的两个 device 对象都要换 GUID */
    int rc1 = img_bcd_patch_device_guids(b, guid, IMG_BCD_EL_WIM_DEVICE, argv[3], argv[4]);
    int rc2 = img_bcd_patch_device_guids(b, guid, IMG_BCD_EL_APP_DEVICE, argv[3], argv[4]);
    printf("  改 21000001(OSDevice): %s\n", img_strerror(rc1));
    printf("  改 11000001(AppDevice): %s\n", img_strerror(rc2));
    if(rc1!=IMG_OK && rc2!=IMG_OK){ img_bcd_close(b); return 1; }

    rc1 = img_bcd_save(b, argv[2]);
    printf("  写出: %s → %s\n", argv[2], img_strerror(rc1));
    img_bcd_close(b);
    return rc1==IMG_OK?0:1;
}
