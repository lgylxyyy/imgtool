/* format_twice_test — 同一进程里连续两次 fs.format（NTFS）不许崩
 *
 * 为什么单独测：imgcmd 是"一条命令一个进程"，CLI 永远复现不出来这个 bug。
 * 上游 mkntfs 是【一次性 CLI】，模块级静态状态（g_allocation 等）靠"进程退出"归零；
 * 我们是进程内复用（objcopy 把 main 改名成 mkntfs_main），所以第二次格式化时
 * 野指针会让 bitmap_allocate() 读到已释放内存 → SIGSEGV（真机 bitmap_allocate+84）。
 *
 * 用法：./format_twice_test [镜像路径]
 *   不给路径就在 /tmp 里自己造一张 8 GiB 的盘（gpt: ESP 200 / MSR 16 / Windows 4096）。
 * 退出码：0 = 两次都成功；1 = 有一步失败；崩掉当然也是失败（shell 会看到 signal）。
 */
#include "img_types.h"
#include "img_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails = 0;

/* 跑一条命令；ctx 可为 NULL（img.create 这种无 ctx 的） */
static int call(img_ctx *c, const char *cmd,
                const char **k, const char **v, int n, img_result *r){
    img_args *a = img_args_new();
    if(!a) return IMG_ENOMEM;
    for(int i = 0; i < n; i++) img_args_set(a, k[i], v[i]);
    int rc = img_cmd_exec(cmd, c, a, r);
    img_args_free(a);
    return rc;
}

static void show(const char *what, img_result *r, int rc){
    static char buf[4096];
    img_result_json(r, buf, (int)sizeof buf);
    printf("  %-22s rc=%-3d %s", what, rc, buf);
    if(buf[0] && buf[strlen(buf) - 1] != '\n') printf("\n");
}

int main(int argc, char **argv){
    char path[512];
    if(argc > 1) snprintf(path, sizeof path, "%s", argv[1]);
    else         snprintf(path, sizeof path, "/tmp/format-twice-%d.qcow2", (int)getpid());

    img_result r;
    char err[512];

    /* ① 建一张 8 GiB GPT 盘（ESP 200 / MSR 16 / Windows 4096；测试盘故意不合装机口径） */
    {
        const char *k[] = {"out","size-gb","table","esp-mb","msr-mb","sys-mb","limits","force"};
        const char *v[] = {path,"8","gpt","200","16","4096","off","true"};
        int rc = call(NULL, "img.create", k, v, 8, &r);
        show("img.create", &r, rc);
        if(rc != IMG_OK){ printf("!! 建盘失败\n"); return 1; }
    }

    /* Windows 分区起始 LBA = 444416（ESP 200 MiB 从 2048 起，MSR 16 MiB 紧随其后） */

    /* ② ★ 同一进程里连续格式化两次 —— 第二次以前必崩 */
    for(int i = 1; i <= 2; i++){
        img_ctx *c = img_ctx_open(path, true, false, err, sizeof err);
        if(!c){ printf("!! 打不开镜像：%s\n", err); return 1; }
        const char *k[] = {"lba","fs","label","quick"};
        char lab[16]; snprintf(lab, sizeof lab, "TWICE%d", i);
        const char *v[] = {"444416","ntfs",lab,"true"};
        int rc = call(c, "fs.format", k, v, 4, &r);   /* img 由 ctx 自动注入 */
        img_ctx_close(c);
        char what[32]; snprintf(what, sizeof what, "fs.format #%d", i);
        show(what, &r, rc);
        if(rc != IMG_OK){
            if(r.code[0] && !strcmp(r.code, "FEATURE-UNAVAILABLE")){
                printf("（这个构建没开 IMG_WITH_NTFS，跳过）\n");
                unlink(path);
                return 0;
            }
            fails++;
        }
    }

    /* ④ FAT32 也连打两次（我们自研实现，没有 mkntfs 那种静态链表；
     *    这条是"确认没有同类问题"的反向证明） */
    for(int i = 1; i <= 2; i++){
        img_ctx *c = img_ctx_open(path, true, false, err, sizeof err);
        if(!c) break;
        const char *k[] = {"lba","fs","label","quick"};
        const char *v[] = {"2048","fat32","FATX","true"};
        int rc = call(c, "fs.format", k, v, 4, &r);
        img_ctx_close(c);
        if(rc != IMG_OK){ fails++; printf("  ❌ FAT32 第 %d 次失败\n", i); }
    }

    /* ③ 复验：分区上确实是 NTFS，而且卷标是第二次写的那个 */
    {
        img_ctx *c = img_ctx_open(path, false, false, err, sizeof err);
        if(!c){ printf("!! 复验时打不开镜像\n"); return 1; }
        const char *k[] = {"lba"};
        const char *v[] = {"444416"};
        int rc = call(c, "part.scan", k, v, 1, &r);
        img_ctx_close(c);
        show("part.scan", &r, rc);
        if(rc != IMG_OK) fails++;
        else if(!strstr(r.ok ? "NTFS" : "", "NTFS")){
            /* img_result 的字段是私有的，用 JSON 里找最省事 */
        }
    }
    {
        /* 用 JSON 输出核一下 fs/label（img_result 结构体字段不公开，走渲染） */
        img_ctx *c = img_ctx_open(path, false, false, err, sizeof err);
        if(c){
            const char *k[] = {"lba"};
            const char *v[] = {"444416"};
            if(call(c, "part.scan", k, v, 1, &r) == IMG_OK){
                static char buf[4096];
                img_result_json(&r, buf, (int)sizeof buf);
                if(!strstr(buf, "\"fs\":\"NTFS\"")){
                    printf("  ❌ part.scan 没报 NTFS：%s", buf);
                    fails++;
                } else if(!strstr(buf, "TWICE2")){
                    printf("  ❌ 卷标不是第二次写的 TWICE2：%s", buf);
                    fails++;
                } else {
                    printf("  ✅ 两次格式化都成功，卷标 = TWICE2，fs = NTFS\n");
                }
            }
            img_ctx_close(c);
        }
    }

    if(argc <= 1) unlink(path);      /* 自带路径才删（传进来的路径别动） */
    printf("%s\n", fails ? "❌ 失败" : "✅ format_twice_test 全过");
    return fails ? 1 : 0;
}