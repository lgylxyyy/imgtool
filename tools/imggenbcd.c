/* imggenbcd — 为新盘生成可引导的 BCD
 *  用法: imggenbcd <参考BCD> <输出BCD> <系统分区partGuid> <磁盘diskGuid> [ESP分区partGuid]
 *
 *  做的事：把参考 BCD 里所有「分区设备对象」写死的源盘 GUID，
 *         按【旧系统分区 → 新系统分区】【旧 ESP → 新 ESP】逐对象替换。
 *         漏改任何一处，bootmgr 就找不到设备（0xc000000e / 0xc0000098）。
 *
 *  ★ 为什么要遍历【全部对象】而不是只改 OS 加载器：
 *    参考件实测有 5 个分区设备对象 —— bootmgr/memtest 指 ESP，
 *    resume/OSLoader(11000001)/OSDevice(21000001) 指系统分区。
 *    只改加载器会让 resume 仍指旧系统分区；本工具按旧 GUID 映射逐个改，
 *    未知分区不动。
 *  ★ 不给 ESP GUID（4 参）时：只映射系统分区（resume + 两个加载器设备），
 *    ESP 上的对象保持原样（用于只换系统分区的场景）。
 *  ★ 参考 BCD 没有 DisplayOrder（例如 ESP 上那份精简的 Recovery\BCD）时，
 *    自动退化为「只做 ESP 重映射」。
 */
#include "img_bcd.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 从 "{guid}" 里取 guid（去花括号），结果写入 out（≥40 字节） */
static void guid_trim(const char *in, char *out, size_t cap){
    const char *p = in;
    if(p[0] == '{') p++;
    snprintf(out, cap, "%s", p);
    char *c = strchr(out, '}');
    if(c) *c = 0;
}

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imggenbcd");
    if(argc<5 || argc==7){
        fprintf(stderr,"用法: %s <参考BCD> <输出BCD> <sysGuid> <diskGuid> [espGuid] [oldRecGuid newRecGuid]\n",argv[0]);
        return 2; }

    /* ★ 先验 GUID：非法 GUID 绝不许"改了个寂寞还返回 0" */
    unsigned char probe[88];
    if(img_bcd_make_partition_device(argv[3], argv[4], probe) != IMG_OK){
        printf("❌ 系统分区 GUID 或磁盘 GUID 非法：%s / %s\n", argv[3], argv[4]);
        return 1;
    }
    if(argc>=6 && img_bcd_make_partition_device(argv[5], argv[4], probe) != IMG_OK){
        printf("❌ ESP 分区 GUID 非法：%s\n", argv[5]);
        return 1;
    }
    if(argc>=8 && img_bcd_make_partition_device(argv[7], argv[4], probe) != IMG_OK){
        printf("❌ 恢复分区 GUID 非法：%s\n", argv[7]);
        return 1;
    }

    img_bcd *b = img_bcd_open_file(argv[1]);
    if(!b){ printf("❌ 打不开参考 BCD\n"); return 1; }

    char buf[80]; char guid[80]="";
    bool have_do = (img_bcd_get_display_order_first(b, buf, sizeof buf)==IMG_OK);
    if(have_do){
        printf("  参考加载器: %s\n", buf);
        guid_trim(buf, guid, sizeof guid);
    } else if(argc<6){
        printf("❌ 参考 BCD 没有 DisplayOrder\n"); img_bcd_close(b); return 1;
    } else {
        printf("  参考 BCD 没有 DisplayOrder —— 只做 ESP 分区重映射\n");
    }

    /* ★ 必须【改之前】先记下参考盘的原始 GUID —— 改完就读不到旧值了 */
    char old_os[40]={0}, old_esp[40]={0}, old_disk[40]={0}, td[40];
    if(guid[0] && img_bcd_get_device_guids(b, guid, IMG_BCD_EL_WIM_DEVICE, old_os, old_disk)!=IMG_OK)
        old_os[0]=0;
    if(guid[0] && !old_os[0])
        img_bcd_get_device_guids(b, guid, IMG_BCD_EL_APP_DEVICE, old_os, old_disk);
    if(argc>=6){
        img_bcd_get_device_guids(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_APP_DEVICE, old_esp, td);
        if(!old_disk[0]) snprintf(old_disk,sizeof old_disk,"%s",td);
    }
    const char *old_rec = argc>=8 ? argv[6] : NULL;
    const char *new_rec = argc>=8 ? argv[7] : NULL;

    printf("  参考盘 系统分区=%s / ESP=%s / 恢复=%s / 磁盘=%s\n",
           old_os[0]?old_os:"(无)", old_esp[0]?old_esp:"(无)",
           old_rec?old_rec:"(无)", old_disk[0]?old_disk:"(无)");
    printf("  新盘   系统分区=%s", argv[3]);
    if(argc>=6) printf(" / ESP=%s", argv[5]);
    if(argc>=8) printf(" / 恢复=%s", argv[7]);
    printf(" / 磁盘=%s\n", argv[4]);
    if(!old_os[0] && !old_esp[0]){
        printf("❌ 参考件里找不到可映射的分区设备（不是 UEFI BCD？）\n");
        img_bcd_close(b); return 1;
    }

    /* ★ 全对象映射：不止 11000001/21000001，还有 21000026/31000003 等；
     *   只改已知元素 ID 会让 bootmgr 仍去找旧盘（随机 GUID 启动失败）。*/
    int hit=0;
    int prc=img_bcd_patch_all_guid_bindings(b,
            old_os[0]?old_os:NULL, argv[3],
            (argc>=6 && old_esp[0])?old_esp:NULL, (argc>=6)?argv[5]:NULL,
            old_rec, new_rec,
            old_disk[0]?old_disk:NULL, argv[4], &hit);
    if(prc!=IMG_OK){
        printf("❌ 全对象映射失败：%s\n", img_strerror(prc));
        img_bcd_close(b); return 1;
    }
    /* 兜底：万一参考件结构不认识，至少把 OS 加载器的两个设备对象改掉 */
    if(hit==0 && guid[0]){
        int rc1=img_bcd_patch_device_guids(b,guid,IMG_BCD_EL_WIM_DEVICE,argv[3],argv[4]);
        int rc2=img_bcd_patch_device_guids(b,guid,IMG_BCD_EL_APP_DEVICE,argv[3],argv[4]);
        printf("  兜底改加载器 21000001: %s / 11000001: %s\n", img_strerror(rc1), img_strerror(rc2));
        if(rc1==IMG_OK) hit++;
        if(rc2==IMG_OK) hit++;
    }
    printf("  全对象重映射: 共 %d 处\n", hit);
    if(hit==0){ printf("❌ 一处也没改到（参考件 GUID 与参数不匹配？）\n"); img_bcd_close(b); return 1; }

    int rc = img_bcd_save(b, argv[2]);
    printf("  写出: %s → %s\n", argv[2], img_strerror(rc));
    img_bcd_close(b);
    return rc==IMG_OK?0:1;
}