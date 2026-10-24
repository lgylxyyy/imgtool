/* bcdcreate_test — bcd.create（内置模板 → 按目标盘重写）的回归
 *
 * 钉住的都是"看着能过、真机起不来"的坑：
 *   1) MBR 设备 [52..55] 必须是 1（漏了 → bootmgr 报 0xc000000e「设备不存在」，
 *      实测 Win7 20 GiB 盘就卡在这里，补上后一路走进"安装程序正在安装设备 100%"）
 *   2) MBR 设备 [32..39] 是【字节】偏移，[56..59] 是磁盘签名
 *   3) GPT 设备是 88 字节：分区 GUID@32、磁盘 GUID@56
 *   4) default(23000003) 必须带花括号；displayorder(24000001) 必须是 GUID 列表(t=7)
 *   5) 默认删掉 WinRE/ramdisk 对象（我们不建恢复分区）；keep_recovery=true 才留
 *   6) SystemRoot 必须写 \Windows（大小写；小写曾 0xc000000f）
 */
#include "img_bcd.h"
#include <hivex.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int fails = 0;
#define OK(c, m) do{ if(c) printf("  ✅ %s\n", m); else { printf("  ❌ %s\n", m); fails++; } }while(0)

static int count_objects(img_bcd *b){
    char **g = NULL; size_t n = 0;
    if(img_bcd_list_objects(b, &g, &n) != IMG_OK) return -1;
    img_bcd_free_object_list(g, n);
    return (int)n;
}
static size_t elem(img_bcd *b, const char *guid, const char *eid, void *out, size_t cap){
    size_t n = 0;
    if(img_bcd_get_element_binary(b, guid, eid, out, cap, &n) != IMG_OK) return 0;
    return n;
}
static uint32_t u32at(const unsigned char *p, int off){ return (uint32_t)p[off] | ((uint32_t)p[off+1]<<8) | ((uint32_t)p[off+2]<<16) | ((uint32_t)p[off+3]<<24); }
static uint64_t u64at(const unsigned char *p, int off){ return (uint64_t)u32at(p,off) | ((uint64_t)u32at(p,off+4)<<32); }

static void test_mbr(void){
    printf("\n── MBR/BIOS（legacy 模板）──\n");
    img_bcd_create_req_t rq; memset(&rq, 0, sizeof rq);
    rq.uefi = false; rq.family = IMG_BCD_FAM_LEGACY;
    rq.desc = "Windows 7 Professional"; rq.timeout = 7;
    rq.mbr_sig = 0xAABBCCDDu;
    rq.sys_off = 206848ull * 512; rq.sr_off = 2048ull * 512;

    void *m = NULL; size_t n = 0; char err[256] = "";
    OK(img_bcd_create_mem(&rq, &m, &n, err, sizeof err) == IMG_OK, "生成 BCD（legacy/MBR）");
    img_bcd *b = img_bcd_open_mem(m, n);
    OK(b != NULL, "生成物是合法 hive");
    if(!b) return;

    unsigned char d[256];
    size_t dn = elem(b, IMG_BCD_GUID_BOOTMGR, "11000001", d, sizeof d);
    OK(dn == 88, "bootmgr 设备 88 字节");
    OK(u32at(d, 16) == 6, "设备类型=6（分区设备）");
    OK(u32at(d, 24) == 72, "结构长度=72");
    OK(u32at(d, 52) == 1, "★ [52..55]=1（漏了会 0xc000000e）");
    OK(u64at(d, 32) == rq.sr_off, "bootmgr 指向系统保留分区（字节偏移）");
    OK(u32at(d, 56) == rq.mbr_sig, "bootmgr 带磁盘签名");

    char *def = img_bcd_get_string(b, IMG_BCD_GUID_BOOTMGR, "23000003");
    OK(def && def[0] == '{' && strchr(def, '}'), "default 是带花括号的 GUID");
    char dup[80] = "";
    if(def){ snprintf(dup, sizeof dup, "%s", def + 1); dup[strcspn(dup, "}")] = 0; }
    OK(dup[0] && img_bcd_has_element(b, dup, "22000002"), "default 指向真实存在的加载器对象");
    OK(dup[0] && img_bcd_has_element(b, dup, "12000002"), "加载器有 ApplicationPath");
    free(def);

    char *sr = dup[0] ? img_bcd_get_string(b, dup, "22000002") : NULL;
    OK(sr && !strcmp(sr, "\\Windows"), "SystemRoot = \\Windows（大小写）");
    free(sr);
    char *ldr = dup[0] ? img_bcd_get_string(b, dup, "12000002") : NULL;
    OK(ldr && strstr(ldr, "winload.exe"), "加载器路径 = winload.exe");
    free(ldr);

    /* displayorder 必须是 GUID 列表（t=7）；写成 REG_SZ 会 0xc0000098。
     * img_bcd 没有暴露"读元素类型"，所以直接拿生成物用 hivex 看。 */
    { const char *td = img_tmpdir(); int fd = -1; char tp[528] = ""; if(td){ snprintf(tp, sizeof tp, "%s/bcdcreateXXXXXX", td); fd = mkstemp(tp); }
      if(fd >= 0){ ssize_t wr = write(fd, m, n); (void)wr; close(fd);
          hive_h *h = hivex_open(tp, 0);
          int ty = -1;
          if(h){
              hive_node_h root = hivex_root(h);
              hive_node_h objs = hivex_node_get_child(h, root, "Objects");
              hive_node_h bm = hivex_node_get_child(h, objs, "{9dea862c-5cdd-4e70-acc1-f32b344d4795}");
              hive_node_h els = hivex_node_get_child(h, bm, "Elements");
              hive_node_h el = hivex_node_get_child(h, els, "24000001");
              hive_value_h *vs = el ? hivex_node_values(h, el) : NULL;
              if(vs && vs[0]){ hive_type t2; size_t l2; char *dv = hivex_value_value(h, vs[0], &t2, &l2); ty = (int)t2; free(dv); }
              if(vs) free(vs);
              hivex_close(h);
          }
          unlink(tp);
          OK(ty == 7, "displayorder 是 GUID 列表（t=7）");
      }
    }

    OK(count_objects(b) < 13, "默认删掉了 WinRE/ramdisk 对象（对象数变少）");
    OK(!img_bcd_has_element(b, IMG_BCD_GUID_BOOTMGR, "14000008"), "bootmgr 不再有 recoverysequence");
    img_bcd_close(b);
    free(m);
}

static void test_mbr_keep_recovery(void){
    printf("\n── MBR 保留恢复对象（keep_recovery=true）──\n");
    img_bcd_create_req_t rq; memset(&rq, 0, sizeof rq);
    rq.family = IMG_BCD_FAM_LEGACY; rq.mbr_sig = 0x11223344;
    rq.sys_off = 206848ull * 512; rq.sr_off = 2048ull * 512;
    rq.keep_recovery = true;
    void *m = NULL; size_t n = 0; char err[256] = "";
    OK(img_bcd_create_mem(&rq, &m, &n, err, sizeof err) == IMG_OK, "生成（keep_recovery）");
    img_bcd *b = img_bcd_open_mem(m, n);
    OK(b && count_objects(b) >= 12, "保留了 WinRE/ramdisk 对象");
    img_bcd_close(b); free(m);
}

static void test_gpt(void){
    printf("\n── GPT/UEFI（modern 模板）──\n");
    const char *sysg = "11111111-2222-3333-4444-555555555555";
    const char *diskg = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    const char *espg = "99999999-8888-7777-6666-555555555555";
    img_bcd_create_req_t rq; memset(&rq, 0, sizeof rq);
    rq.uefi = true; rq.family = IMG_BCD_FAM_MODERN;
    rq.sys_guid = sysg; rq.disk_guid = diskg; rq.esp_guid = espg;
    rq.desc = "Windows 10"; rq.locale = "en-US";
    void *m = NULL; size_t n = 0; char err[256] = "";
    OK(img_bcd_create_mem(&rq, &m, &n, err, sizeof err) == IMG_OK, "生成 BCD（modern/GPT）");
    img_bcd *b = img_bcd_open_mem(m, n);
    if(!b) return;

    unsigned char d[256];
    size_t dn = elem(b, IMG_BCD_GUID_BOOTMGR, "11000001", d, sizeof d);
    OK(dn == 88 && u32at(d, 16) == 6, "bootmgr 是分区设备（88 字节）");
    char pg[40] = "", dg[40] = "";
    OK(img_bcd_get_device_guids(b, IMG_BCD_GUID_BOOTMGR, "11000001", pg, dg) == IMG_OK, "读得回 GUID");
    OK(!strcasecmp(pg, espg), "bootmgr 指向 ESP");
    OK(!strcasecmp(dg, diskg), "磁盘 GUID 正确");

    char *def = img_bcd_get_string(b, IMG_BCD_GUID_BOOTMGR, "23000003");
    char dup[80] = "";
    if(def){ snprintf(dup, sizeof dup, "%s", def + 1); dup[strcspn(dup, "}")] = 0; }
    OK(dup[0] != 0, "UEFI 也有 default");
    char *ldr = dup[0] ? img_bcd_get_string(b, dup, "12000002") : NULL;
    OK(ldr && strstr(ldr, "winload.efi"), "加载器路径 = winload.efi");
    free(ldr);
    char pg2[40] = "", dg2[40] = "";
    if(dup[0] && img_bcd_get_device_guids(b, dup, "11000001", pg2, dg2) == IMG_OK)
        OK(!strcasecmp(pg2, sysg), "加载器指向系统分区");
    free(def);
    img_bcd_close(b); free(m);
}

static void test_bad_args(void){
    printf("\n── 参数缺口 ──\n");
    img_bcd_create_req_t rq; memset(&rq, 0, sizeof rq);
    void *m = NULL; size_t n = 0; char err[256] = "";
    rq.uefi = false;   /* 没签名/没偏移 */
    OK(img_bcd_create_mem(&rq, &m, &n, err, sizeof err) == IMG_EINVAL, "MBR 缺签名/偏移 → EINVAL");
    rq.uefi = true;    /* 没 GUID */
    OK(img_bcd_create_mem(&rq, &m, &n, err, sizeof err) == IMG_EINVAL, "UEFI 缺 GUID → EINVAL");
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("bcdcreate_test — 内置模板建 BCD\n");
    test_mbr();
    test_mbr_keep_recovery();
    test_gpt();
    test_bad_args();
    printf(fails ? "\n❌ bcdcreate_test 失败 %d 项\n" : "\n✅ bcdcreate_test 全过\n", fails);
    return fails ? 1 : 0;
}
