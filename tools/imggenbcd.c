/* imggenbcd — 为新盘生成可引导的 BCD
 *  用法: imggenbcd <参考BCD> <输出BCD> <系统分区partGuid> <磁盘diskGuid> [ESP分区partGuid]
 *  做的事：拷参考 BCD 里 OS 加载器的 device 对象 → 换成新盘的分区/磁盘 GUID
 *         （换了盘不改的话 bootmgr 解析不到设备 → 0xc0000098）
 *         给了第 5 参（ESP 分区 GUID）时再做一遍【全对象重映射】：
 *         参考盘 BCD 里每个「分区设备」都写死了源盘的分区 GUID ——
 *           bootmgr 自身的 device 指 ESP、内存测试项指 ESP、
 *           OS 加载器/恢复项指系统分区；
 *         漏改任何一处，bootmgr 就找不到设备（0xc000000e / 0xc0000098）。
 *         本工具按【参考盘原 GUID → 新盘 GUID】的映射逐对象改，未知分区不动。
 *         参考 BCD 没有 DisplayOrder（例如 ESP 上那份精简的 Recovery\BCD）时
 *         自动退化为「只做 ESP 重映射」。 */
#include "img_bcd.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imggenbcd");
    if(argc<5){ fprintf(stderr,"用法: %s <参考BCD> <输出BCD> <partGuid> <diskGuid> [espPartGuid]\n",argv[0]); return 2; }
    img_bcd *b = img_bcd_open_file(argv[1]);
    if(!b){ printf("❌ 打不开参考 BCD\n"); return 1; }

    char buf[64]; char guid[64]="";
    bool have_do = (img_bcd_get_display_order_first(b, buf, sizeof buf)==IMG_OK);
    if(have_do){
        printf("  参考加载器: %s\n", buf);
        char *id = buf; if(id[0]=='{') id++;
        snprintf(guid,sizeof guid,"%s",id);
        char *c = strchr(guid, '}');
        if(c) *c = 0;
    } else if(argc<6){
        printf("❌ 参考 BCD 没有 DisplayOrder\n"); return 1;
    } else {
        printf("  参考 BCD 没有 DisplayOrder —— 只做 ESP 分区重映射\n");
    }

    /* ★ 必须【改之前】先记下参考盘的原始 GUID —— 改完就读不到旧值了 */
    char old_os[40]={0}, old_esp[40]={0}, tp[40], td[40];
    if(guid[0] && img_bcd_get_device_guids(b, guid, IMG_BCD_EL_WIM_DEVICE, old_os, td)!=IMG_OK)
        old_os[0]=0;
    if(guid[0] && !old_os[0])
        img_bcd_get_device_guids(b, guid, IMG_BCD_EL_APP_DEVICE, old_os, td);
    if(argc>=6)
        img_bcd_get_device_guids(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_APP_DEVICE, old_esp, td);

    if(guid[0]){
        /* OS 加载器的两个 device 对象都要换 GUID */
        int rc1 = img_bcd_patch_device_guids(b, guid, IMG_BCD_EL_WIM_DEVICE, argv[3], argv[4]);
        int rc2 = img_bcd_patch_device_guids(b, guid, IMG_BCD_EL_APP_DEVICE, argv[3], argv[4]);
        printf("  改 21000001(OSDevice): %s\n", img_strerror(rc1));
        printf("  改 11000001(AppDevice): %s\n", img_strerror(rc2));
        if(rc1!=IMG_OK && rc2!=IMG_OK && argc<6){ img_bcd_close(b); return 1; }
    }

    if(argc>=6){
        printf("  参考盘 系统分区=%s\n", old_os[0]?old_os:"(无)");
        printf("  参考盘 ESP 分区=%s\n", old_esp[0]?old_esp:"(无)");
        printf("  新盘   系统分区=%s / ESP=%s\n", argv[3], argv[5]);
        char **objs=NULL; size_t nobj=0;
        if(img_bcd_list_objects(b,&objs,&nobj)==IMG_OK){
            static const char *EIDS[]={ IMG_BCD_EL_WIM_DEVICE, IMG_BCD_EL_APP_DEVICE, NULL };
            int hit=0;
            for(size_t i=0;i<nobj;i++){
                for(int e=0;EIDS[e];e++){
                    if(img_bcd_get_device_guids(b,objs[i],EIDS[e],tp,td)!=IMG_OK) continue;
                    const char *np=NULL;
                    if(old_os[0] && !strcmp(tp,old_os)) np=argv[3];
                    else if(old_esp[0] && !strcmp(tp,old_esp)) np=argv[5];
                    if(!np) continue;
                    if(img_bcd_patch_device_guids(b,objs[i],EIDS[e],np,argv[4])==IMG_OK){
                        printf("    %s %s: %s → %s\n", objs[i], EIDS[e], tp, np);
                        hit++;
                    }
                }
            }
            printf("  全对象重映射: 共 %d 处\n", hit);
            img_bcd_free_object_list(objs,nobj);
        } else printf("  ⚠ 对象枚举失败，跳过全对象重映射\n");
    }

    int rc = img_bcd_save(b, argv[2]);
    printf("  写出: %s → %s\n", argv[2], img_strerror(rc));
    img_bcd_close(b);
    return rc==IMG_OK?0:1;
}
