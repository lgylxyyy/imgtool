/* bcd_test — BCD 读写验证（用参考真值） */
#include "img_bcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENSURE(c,msg) do{ if(!(c)){ printf("❌ %s\n", msg); return 1; } printf("  ✅ %s\n", msg); }while(0)

int main(int argc, char **argv){
    const char *src = argc>1 ? argv[1] : "reference/bcd-reference-uefi.bin";
    const char *dst = argc>2 ? argv[2] : "/tmp/bcd_out.bin";

    printf("① 打开参考 BCD\n");
    img_bcd *b = img_bcd_open_file(src);
    ENSURE(b!=NULL, "打开成功");

    printf("② 读取 DisplayOrder 第一项（默认启动项）\n");
    char guid[64];
    int rc = img_bcd_get_display_order_first(b, guid, sizeof guid);
    ENSURE(rc==IMG_OK, "读取成功");
    printf("     默认启动项 = %s\n", guid);

    printf("③ 检查关键元素存在\n");
    ENSURE(img_bcd_has_element(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_DEVICE),
           "bootmgr 有 Device(12000002)");
    ENSURE(img_bcd_has_element(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_DISPLAYORDER),
           "bootmgr 有 DisplayOrder(24000001)");
    ENSURE(!img_bcd_has_element(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_RECOVERYSEQ),
           "参考 BCD 原本没有 RecoverySequence（符合预期，4 份真值都是如此）");

    printf("④ 读 bootmgr 的 device 路径（应为相对路径，可移植）\n");
    char *dev = img_bcd_get_string(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_DEVICE);
    ENSURE(dev!=NULL, "读到 device");
    printf("     device = %s\n", dev);
    ENSURE(strncmp(dev,"\\EFI\\Microsoft\\Boot",19)==0 || dev[0]=='\\', "是相对路径");
    free(dev);

    printf("⑤ 修改 bootmgr 显示名\n");
    rc = img_bcd_set_bootmgr_name(b, "Icdqcow2 Windows");
    ENSURE(rc==IMG_OK, "修改返回 OK");

    printf("⑥ 写 RecoverySequence（权威 ID = 14000008，GUID list）\n");
    rc = img_bcd_set_recovery_sequence(b, "b0b53260-3b27-4f0a-8d0f-6b3e0c1a2b3c");
    ENSURE(rc==IMG_OK, "写入返回 OK");
    ENSURE(img_bcd_has_element(b, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_RECOVERYSEQ),
           "bootmgr 下出现 14000008 元素");

    printf("⑦ 落盘\n");
    rc = img_bcd_save(b, dst);
    ENSURE(rc==IMG_OK, "保存成功");
    img_bcd_close(b);

    printf("⑧ 重新打开校验\n");
    img_bcd *c = img_bcd_open_file(dst);
    ENSURE(c!=NULL, "重开成功");
    char *nm = img_bcd_get_string(c, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_OSDEVICE);
    printf("     显示名 = %s\n", nm?nm:"(null)");
    ENSURE(nm && strcmp(nm,"Icdqcow2 Windows")==0, "显示名已生效");
    free(nm);
    char *rs = img_bcd_get_string(c, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_RECOVERYSEQ);
    printf("     RecoverySequence = %s\n", rs?rs:"(null)");
    ENSURE(rs && strstr(rs,"b0b53260")!=NULL, "RecoverySequence 重读校验生效");
    free(rs);
    /* 同时确认没写坏 Timeout(25000004) —— 一旦写错元素就会在这里露馅 */
    char *to = img_bcd_get_string(c, IMG_BCD_GUID_BOOTMGR, "25000004");
    ENSURE(to==NULL, "Timeout(25000004) 是整数，get_string 正确返回 NULL（不产出垃圾）");
    free(to);
    char g2[64];
    ENSURE(img_bcd_get_display_order_first(c,g2,sizeof g2)==IMG_OK && strcmp(g2,guid)==0,
           "DisplayOrder 未被破坏");
    img_bcd_close(c);

    printf("\n✅ BCD 读写全部通过（读原值 → 改 → 落盘 → 重读校验）\n");
    return 0;
}
