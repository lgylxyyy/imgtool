/* recovery_bcd_test — 用官方真值构建恢复条目
 *  流程：参考 BCD → 复制 WinPE loader + ramdiskoptions → 改 WIM 路径
 *        → 挂到 bootmgr 的 14000008(RecoverySequence) → 落盘重读校验 */
#include "img_bcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENSURE(c,m) do{ if(!(c)){ printf("❌ %s\n", m); return 1; } printf("  ✅ %s\n", m); }while(0)
static const char *ISO_BCD_DEFAULT = "/tmp/iso/bcd";

int main(int argc, char **argv){
    const char *src = argc>1?argv[1]:"reference/bcd-reference-uefi.bin";
    const char *dst = argc>2?argv[2]:"/tmp/bcd_recovery.bin";
    /* ★ BCD 里的设备路径用反斜杠（Windows 惯例）*/
    const char *WIM = argc>3?argv[3]:"\\Recovery\\WindowsRE\\Winre.wim";
    /* 官方 ISO 的 BCD 作模板来源（第4参），缺省回退到 /tmp/iso/bcd */
    const char *ISO_BCD = argc>4 ? argv[4] : ISO_BCD_DEFAULT;

    FILE *fp=fopen(ISO_BCD,"rb");
    if(!fp){ printf("❌ 缺少官方 ISO BCD 参考 %s（先解 ISO）\n", ISO_BCD); return 2; }
    fclose(fp);

    printf("① 打开参考 BCD\n");
    img_bcd *b = img_bcd_open_file(src);
    ENSURE(b!=NULL,"打开成功");

    printf("② 从官方 ISO BCD 复制 ramdiskoptions 对象\n");
    ENSURE(img_bcd_copy_object(b, ISO_BCD, IMG_BCD_GUID_ISO_RAMDISK)==IMG_OK, "复制成功");

    printf("③ 从官方 ISO BCD 复制 WinPE loader 对象\n");
    ENSURE(img_bcd_copy_object(b, ISO_BCD, IMG_BCD_GUID_ISO_WINPE)==IMG_OK, "复制成功");

    printf("④ 改 WIM 路径 %s\n", WIM);
    char before[256];
    ENSURE(img_bcd_get_device_wimpath(b, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_WIM_DEVICE,
                                      before, sizeof before)==IMG_OK, "读到原路径");
    printf("     原(hex): ");
    for(size_t i=0;i<strlen(before) && i<24;i++) printf("%02x ", (unsigned char)before[i]);
    printf(" (len=%zu)\n", strlen(before));
    /* ★ 官方 WinPE loader 上有【两个】device 对象且内容相同：
     *   11000001 = BcdLibraryDevice_ApplicationDevice（bootmgr 主要读这个）
     *   21000001 = BcdOSLoaderDevice_OSDevice
     * 只改一个的话另一个仍指向 \sources\boot.wim → WinRE 加载失败（F1 无效）。
     * 实测：只改 21000001 时 QEMU 里按 F1 返回原错误屏。 */
    ENSURE(img_bcd_patch_device_wimpath(b, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_WIM_DEVICE,
                                        WIM)==IMG_OK, "改 21000001(OSDevice) 返回 OK");
    ENSURE(img_bcd_patch_device_wimpath(b, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_APP_DEVICE,
                                        WIM)==IMG_OK, "改 11000001(ApplicationDevice) 返回 OK");

    printf("⑤ 改 ramdiskoptions 的 boot.sdi 路径\n");
    /* boot.sdi 路径跟随 WIM 同目录推导不了就用同级约定：与 WIM 同目录 */
    { char sdi[256]; snprintf(sdi,sizeof sdi, "%s", WIM);
      char *slash=strrchr(sdi,'\\');   /* BCD 路径用反斜杠 */
      if(slash){ snprintf(slash+1, sizeof sdi-(size_t)(slash+1-sdi), "boot.sdi"); }
      else { snprintf(sdi,sizeof sdi, "\\boot.sdi"); }
      ENSURE(img_bcd_set_string(b, IMG_BCD_GUID_ISO_RAMDISK, IMG_BCD_EL_BOOTSDI_PATH, sdi)==IMG_OK, "写入 boot.sdi 路径"); }

    printf("⑤·改 loader 显示名（原为 'Windows Setup'，恢复条目应显示为恢复环境）\n");
    ENSURE(img_bcd_set_string(b, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_OSDEVICE,
                              "Windows Recovery Environment")==IMG_OK, "显示名已改");

    printf("⑥ 挂到 bootmgr 的 RecoverySequence(14000008)\n");
    ENSURE(img_bcd_set_recovery_sequence(b, IMG_BCD_GUID_ISO_WINPE)==IMG_OK, "写入 OK");

    printf("⑦ 落盘\n");
    ENSURE(img_bcd_save(b, dst)==IMG_OK, "保存成功");
    img_bcd_close(b);

    printf("⑧ 重开校验\n");
    img_bcd *c = img_bcd_open_file(dst);
    ENSURE(c!=NULL,"重开成功");
    char p1[256];
    ENSURE(img_bcd_get_device_wimpath(c, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_WIM_DEVICE,
                                      p1,sizeof p1)==IMG_OK, "OSDevice 路径可读");
    printf("     OSDevice(21000001): %s\n", p1);
    ENSURE(strcmp(p1,WIM)==0, "OSDevice 已改为 winre.wim");
    { char p3[256];
      ENSURE(img_bcd_get_device_wimpath(c, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_APP_DEVICE,
                                        p3,sizeof p3)==IMG_OK, "AppDevice 路径可读");
      printf("     AppDevice(11000001): %s\n", p3);
      ENSURE(strcmp(p3,WIM)==0, "AppDevice 也已改为 winre.wim（两者必须一致）"); }

    char *sd = img_bcd_get_string(c, IMG_BCD_GUID_ISO_RAMDISK, IMG_BCD_EL_BOOTSDI_PATH);
    printf("     boot.sdi: %s\n", sd?sd:"(null)");
    ENSURE(sd && strstr(sd,"boot.sdi")!=NULL, "boot.sdi 路径可读");
    free(sd);

    char *rs = img_bcd_get_string(c, IMG_BCD_GUID_BOOTMGR, IMG_BCD_EL_RECOVERYSEQ);
    printf("     RecoverySequence → %s\n", rs?rs:"(null)");
    if(rs) printf("     (len=%zu) 期望=%s\n", strlen(rs), IMG_BCD_GUID_ISO_WINPE);
    ENSURE(rs && strstr(rs, IMG_BCD_GUID_ISO_WINPE)!=NULL, "RecoverySequence 指向 WinPE loader");
    free(rs);

    char *desc = img_bcd_get_string(c, IMG_BCD_GUID_ISO_WINPE, IMG_BCD_EL_OSDEVICE);
    printf("     loader 描述: %s\n", desc?desc:"(null)");
    ENSURE(desc!=NULL, "loader 对象的显示名保留");
    free(desc);

    /* 原有条目未被破坏 */
    char g[64];
    ENSURE(img_bcd_get_display_order_first(c,g,sizeof g)==IMG_OK, "DisplayOrder 仍在");
    printf("     默认项: %s\n", g);
    img_bcd_close(c);

    printf("\n✅ 恢复条目构建通过：官方真值复制 → 改路径 → 挂 14000008 → 重读校验\n");
    return 0;
}
