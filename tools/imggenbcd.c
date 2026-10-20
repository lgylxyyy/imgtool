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

    /* ★ 共享实现：img_bcd_remap()（库里那份，命令表 bcd.gen-uefi 用的是同一段代码）。
     *   它内部做：GUID 合法性前置校验 → 读参考件旧 GUID（系统/ESP/磁盘/恢复）
     *   → 按 hive 结构全对象映射 → 兜底改两个加载器 → 返回改动处数。*/
    img_bcd *b = img_bcd_open_file(argv[1]);
    if(!b){ printf("❌ 打不开参考 BCD（%s）\n", argv[1]); return 1; }

    img_bcd_remap_req_t rq;
    rq.sys_guid     = argv[3];
    rq.disk_guid    = argv[4];
    rq.esp_guid     = (argc>=6) ? argv[5] : NULL;
    rq.old_rec_guid = (argc>=8) ? argv[6] : NULL;
    rq.new_rec_guid = (argc>=8) ? argv[7] : NULL;

    char err[256]=""; int hit=0;
    int rc = img_bcd_remap(b, &rq, &hit, err, sizeof err);
    if(rc != IMG_OK){
        printf("❌ 换盘重映射失败：%s（rc=%s）\n", err[0]?err:"?", img_strerror(rc));
        img_bcd_close(b);
        return 1;
    }
    printf("  全对象重映射: 共 %d 处（系统=%s%s%s / 磁盘=%s）\n", hit, argv[3],
           rq.esp_guid?", ESP=":"", rq.esp_guid?rq.esp_guid:"", argv[4]);

    rc = img_bcd_save(b, argv[2]);
    printf("  写出: %s → %s\n", argv[2], img_strerror(rc));
    img_bcd_close(b);
    return rc==IMG_OK ? 0 : 1;
}
