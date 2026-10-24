/* mbr_bcd_test — BIOS/MBR BCD 设备命门的批量改写验证
 *
 * 用法：
 *   mbr_bcd_test --list <bcd>          列出所有 type=6 设备对象的 (sig, off)
 *   mbr_bcd_test <参考BCD> <输出BCD>   自测：签名全改 + 每个偏移按映射改 + 其它字节不动
 *
 * 为什么需要它：
 *   boot.repair 过去只改 3 处（bootmgr / OSLoader 11000001 / 21000001），
 *   参考件实测有 5 处（memtest、resume 也在内）；只改 3 处会让 memtest/resume
 *   仍指旧盘。旧的补丁是在整个文件里裸替换 4 字节签名 —— 会误伤恰好等于签名的
 *   其它数据，也改不到漏网对象的偏移。本测试锁住新实现（按 hive 结构遍历）。
 *
 * 退出码：0 通过 · 1 断言失败 · 2 打不开/参数错
 */
#include "img_bcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXB 32
typedef struct {
    int n;                         /* 实际找到的数量（可能 > MAXB） */
    char obj[MAXB][64], eid[MAXB][16];
    uint32_t sig[MAXB];
    uint64_t off[MAXB];
} bind_list;

static int on_bind(void *ud, const char *obj, const char *eid,
                   uint32_t sig, uint64_t off){
    bind_list *L = (bind_list*)ud;
    if(L->n < MAXB){
        snprintf(L->obj[L->n], sizeof L->obj[0], "%s", obj);
        snprintf(L->eid[L->n], sizeof L->eid[0], "%s", eid);
        L->sig[L->n] = sig;
        L->off[L->n] = off;
    }
    L->n++;
    return 0;
}
static int list_bind(img_bcd *b, bind_list *L){
    memset(L, 0, sizeof *L);
    return img_bcd_foreach_mbr_binding(b, on_bind, L);
}
static unsigned char *slurp(const char *path, size_t *n){
    FILE *f = fopen(path, "rb"); if(!f) return NULL;
    if(fseek(f, 0, SEEK_END) != 0){ fclose(f); return NULL; }
    long sz = ftell(f); if(sz <= 0){ fclose(f); return NULL; }
    rewind(f);
    unsigned char *p = malloc((size_t)sz);
    if(p && fread(p, 1, (size_t)sz, f) != (size_t)sz){ free(p); p = NULL; }
    fclose(f);
    if(p) *n = (size_t)sz;
    return p;
}

int main(int argc, char **argv){
    if(argc == 3 && (!strcmp(argv[1], "--list") || !strcmp(argv[1], "--list-guid"))){
        int as_guid = !strcmp(argv[1], "--list-guid");
        img_bcd *b = img_bcd_open_file(argv[2]);
        if(!b){ fprintf(stderr, "打不开 %s\n", argv[2]); return 2; }
        bind_list L = {0};
        int rc = list_bind(b, &L);
        if(rc != IMG_OK){ fprintf(stderr, "遍历失败 rc=%d\n", rc); img_bcd_close(b); return 1; }
        for(int i = 0; i < L.n && i < MAXB; i++){
            if(as_guid){
                char pg[40]="", dg[40]="";
                int grc = img_bcd_get_device_guids(b, L.obj[i], L.eid[i], pg, dg);
                if(grc == IMG_OK) printf("%s %s part=%s disk=%s\n", L.obj[i], L.eid[i], pg, dg);
                else              printf("%s %s part=? disk=? (rc=%d)\n", L.obj[i], L.eid[i], grc);
            } else {
                printf("%s %s sig=%08x off=%llu\n", L.obj[i], L.eid[i],
                       L.sig[i], (unsigned long long)L.off[i]);
            }
        }
        img_bcd_close(b);
        printf("total=%d\n", L.n);
        return 0;
    }
    if(argc != 3){
        fprintf(stderr, "用法: %s --list|--list-guid <bcd> | %s <参考BCD> <输出BCD>\n",
                argv[0], argv[0]);
        return 2;
    }

    size_t rn = 0;
    unsigned char *raw = slurp(argv[1], &rn);
    if(!raw){ fprintf(stderr, "读不了 %s\n", argv[1]); return 2; }
    img_bcd *b = img_bcd_open_mem(raw, rn);
    free(raw);
    if(!b){ fprintf(stderr, "解析失败（不是 BCD/hive）\n"); return 2; }

    bind_list before = {0};
    int rc = list_bind(b, &before);
    if(rc != IMG_OK){ fprintf(stderr, "遍历失败 rc=%d\n", rc); img_bcd_close(b); return 1; }
    printf("找到 MBR 设备对象 %d 处\n", before.n);
    if(before.n < 3){ fprintf(stderr, "❌ 参考件里 type=6 设备对象太少（%d）\n", before.n); img_bcd_close(b); return 1; }

    uint32_t oldsig = before.sig[0];
    for(int i = 0; i < before.n && i < MAXB; i++)
        if(before.sig[i] != oldsig){
            fprintf(stderr, "❌ 参考件签名不一致（%08x vs %08x）\n", before.sig[i], oldsig);
            img_bcd_close(b); return 1;
        }

    /* 记一个元素的原 blob，用来断言"除命门 12 字节外一字未动" */
    unsigned char blob0[256]; size_t blob0n = 0;
    if(img_bcd_get_element_binary(b, before.obj[0], before.eid[0],
                                  blob0, sizeof blob0, &blob0n) != IMG_OK || blob0n < 60){
        fprintf(stderr, "❌ 读不到样本元素 blob\n"); img_bcd_close(b); return 1;
    }

    /* 每个不同的旧偏移都给一个新目标（只改两个字段，其它字节必须原样） */
    img_bcd_off_map maps[8]; int nmaps = 0;
    uint64_t new_off_for[MAXB];
    for(int i = 0; i < before.n && i < MAXB; i++){
        int found = -1;
        for(int k = 0; k < nmaps; k++) if(maps[k].old_off == before.off[i]){ found = k; break; }
        if(found < 0){
            maps[nmaps].old_off = before.off[i];
            maps[nmaps].new_off = 0x100000000ULL + (uint64_t)nmaps * 0x1000000ULL;
            found = nmaps++;
        }
        new_off_for[i] = maps[found].new_off;
    }
    const uint32_t newsig = 0x11223344u;

    int patched = 0;
    rc = img_bcd_patch_all_mbr_bindings(b, oldsig, newsig, maps, (size_t)nmaps, &patched);
    if(rc != IMG_OK){ fprintf(stderr, "❌ patch rc=%d\n", rc); img_bcd_close(b); return 1; }
    printf("批量改写 %d 处（预期 %d）\n", patched, before.n);
    if(patched != before.n){ fprintf(stderr, "❌ 改写数量不符\n"); img_bcd_close(b); return 1; }

    bind_list after = {0};
    if(list_bind(b, &after) != IMG_OK || after.n != before.n){
        fprintf(stderr, "❌ 改写后遍历异常\n"); img_bcd_close(b); return 1;
    }
    for(int i = 0; i < after.n && i < MAXB; i++){
        if(after.sig[i] != newsig){
            fprintf(stderr, "❌ %s %s 签名仍是 %08x\n", after.obj[i], after.eid[i], after.sig[i]);
            img_bcd_close(b); return 1;
        }
        if(after.off[i] != new_off_for[i]){
            fprintf(stderr, "❌ %s %s 偏移 %llu，预期 %llu\n", after.obj[i], after.eid[i],
                    (unsigned long long)after.off[i], (unsigned long long)new_off_for[i]);
            img_bcd_close(b); return 1;
        }
    }
    /* 除 [32..39]（偏移）和 [56..59]（签名）外必须逐字节相同 */
    unsigned char blob1[256]; size_t blob1n = 0;
    if(img_bcd_get_element_binary(b, before.obj[0], before.eid[0],
                                  blob1, sizeof blob1, &blob1n) != IMG_OK || blob1n != blob0n){
        fprintf(stderr, "❌ 样本元素 blob 长度变了\n"); img_bcd_close(b); return 1;
    }
    for(size_t i = 0; i < blob0n; i++){
        if((i >= 32 && i < 40) || (i >= 56 && i < 60)) continue;
        if(blob0[i] != blob1[i]){
            fprintf(stderr, "❌ 样本元素第 %zu 字节被误改（%02x → %02x）\n", i, blob0[i], blob1[i]);
            img_bcd_close(b); return 1;
        }
    }
    /* 幂等：再跑一次不应再改任何东西 */
    int again = 0;
    rc = img_bcd_patch_all_mbr_bindings(b, 0, newsig, maps, (size_t)nmaps, &again);
    if(rc != IMG_OK || again != 0){
        fprintf(stderr, "❌ 幂等失败（rc=%d again=%d）\n", rc, again); img_bcd_close(b); return 1;
    }

    /* 落盘 → 重新打开 → 再验一次（hivex commit 后仍成立） */
    void *ob = NULL; size_t on = 0;
    if(img_bcd_save_mem(b, &ob, &on) != IMG_OK || !ob){
        fprintf(stderr, "❌ save_mem 失败\n"); img_bcd_close(b); return 1;
    }
    img_bcd_close(b);
    FILE *of = fopen(argv[2], "wb");
    if(!of || fwrite(ob, 1, on, of) != on){ free(ob); if(of) fclose(of);
        fprintf(stderr, "❌ 写不了 %s\n", argv[2]); return 1; }
    fclose(of); free(ob);

    img_bcd *b2 = img_bcd_open_file(argv[2]);
    if(!b2){ fprintf(stderr, "❌ 落盘后打不开\n"); return 1; }
    bind_list saved = {0};
    rc = list_bind(b2, &saved);
    img_bcd_close(b2);
    if(rc != IMG_OK || saved.n != before.n){
        fprintf(stderr, "❌ 落盘后遍历异常（n=%d）\n", saved.n); return 1;
    }
    for(int i = 0; i < saved.n && i < MAXB; i++)
        if(saved.sig[i] != newsig){
            fprintf(stderr, "❌ 落盘后签名没持久化（%s %s = %08x）\n",
                    saved.obj[i], saved.eid[i], saved.sig[i]); return 1; }

    printf("✅ MBR BCD 批量改写通过：%d 处签名 %08x→%08x，%d 组偏移映射，其它字节未动\n",
           before.n, oldsig, newsig, nmaps);
    return 0;
}
