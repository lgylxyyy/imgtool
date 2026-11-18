/* lscache_test — 目录会话缓存（img_lscache）的单元 + 命令层粘合测试
 *
 * 会话在【进程内存】里：CLI 每条命令一个进程，跨进程测不了 ——
 * 这里在同一进程里直接调库/调命令层验证。App（JNI 常驻进程）才是真实使用方。
 *
 * A 部分（纯逻辑）：open/page/search/拼接/无效 token/内存上限/LRU 淘汰
 * B 部分（可选，传 WIM 路径）：wim.ls --open → --token 翻页 → wim.search 命令层粘合
 *
 * 用法: lscache_test [<wim 路径>]
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "img_lscache.h"
#include "img_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static int total_chk = 0;
static void chk(const char *w, int ok, const char *extra){
    total_chk++;
    printf("  %s %s%s%s\n", ok ? "✅" : "❌", w,
           (extra && extra[0]) ? "  " : "", (extra && extra[0]) ? extra : "");
    if(!ok) fails++;
}

/* 从 img_result 取字段 */
static const char *res_str(const img_result *r, const char *k){
    for(int i = 0; i < r->nf; i++) if(!strcmp(r->f[i].k, k)) return r->f[i].s;
    return NULL;
}
static long long res_num(const img_result *r, const char *k){
    for(int i = 0; i < r->nf; i++) if(!strcmp(r->f[i].k, k)) return r->f[i].n;
    return -1;
}
static int res_bool(const img_result *r, const char *k){
    for(int i = 0; i < r->nf; i++) if(!strcmp(r->f[i].k, k)) return r->f[i].b ? 1 : 0;
    return -1;
}

/* 构造 n 条假条目（/many/f0001-aaa...bin） */
static img_lsc_ent *mk_ents(size_t n, size_t name_pad){
    img_lsc_ent *v = calloc(n ? n : 1, sizeof(img_lsc_ent));
    static char **keep = NULL;
    if(!v) return NULL;
    keep = calloc(n ? n : 1, sizeof(char*));
    if(!keep){ free(v); return NULL; }
    for(size_t i = 0; i < n; i++){
        size_t cap = 64 + name_pad;
        char *p = malloc(cap);
        if(!p) { free(v); free(keep); return NULL; }
        int w = 0;
        w += snprintf(p, cap, "/many/f%06zu-", i + 1);
        for(size_t k = 0; k < name_pad && (size_t)w < cap - 8; k++) p[w++] = 'a';
        snprintf(p + w, cap - (size_t)w, ".bin");
        keep[i] = p;
        v[i].path = p;
        v[i].size = 256;
        v[i].mtime = 1700000000u;
        v[i].attrs = 0x20;
        v[i].depth = 0;
        v[i].isdir = false;
    }
    return v;
}

static void free_ents(img_lsc_ent *v, size_t n){
    for(size_t i = 0; i < n; i++) free(v[i].path);
    free(v);
}

static int run_cmd(const char *name, const char *k1, const char *v1,
                   const char *k2, const char *v2, const char *k3, const char *v3,
                   const char *k4, const char *v4, img_result *out){
    img_args *a = img_args_new();
    if(!a) return -1;
    if(k1) img_args_set(a, k1, v1);
    if(k2) img_args_set(a, k2, v2);
    if(k3) img_args_set(a, k3, v3);
    if(k4) img_args_set(a, k4, v4);
    memset(out, 0, sizeof *out);
    int rc = img_cmd_exec(name, NULL, a, out);
    img_args_free(a);
    return rc;
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---------------- A. 纯逻辑 ---------------- */
    printf("════ A1. open + 翻页拼接 ════\n");
    const size_t N = 1200;
    img_lsc_ent *v = mk_ents(N, 16);
    if(!v){ printf("❌ 构造条目失败\n"); return 1; }
    char tok[40] = "";
    int rc = img_lsc_open(v, N, false, tok, sizeof tok);
    chk("open 成功且拿到 token", rc == IMG_OK && tok[0], tok);
    free_ents(v, N);              /* open 已拷贝 path —— 释放原数组验证无悬空 */

    static char json[600000];
    uint64_t total = 0, count = 0; bool more = false;
    int pages = 0; size_t all = 0; uint64_t off = 0;
    for(;;){
        rc = img_lsc_page(tok, off, 500, json, sizeof json, &total, &count, &more);
        if(rc != IMG_OK) break;
        pages++; all += count; off += count;
        if(!more || pages > 10) break;
    }
    char info[128];
    snprintf(info, sizeof info, "total=%llu pages=%d 拼接=%zu",
             (unsigned long long)total, pages, all);
    chk("翻页 3 页、拼接 1200、total 正确", rc == IMG_OK && total == N && pages == 3 && all == N, info);
    chk("json 以 [ 开头、] 结尾且非空",
        json[0] == '[' && json[strlen(json) - 1] == ']' && strlen(json) > 100,
        json[0] ? "" : "empty");

    printf("════ A2. 搜索 ════\n");
    rc = img_lsc_search(tok, "F000042", 0, 10, json, sizeof json, &total, &count, &more);
    chk("搜索大小写不敏感（F000042 → 1 条）", rc == IMG_OK && total == 1, NULL);
    rc = img_lsc_search(tok, "a", 0, 10, json, sizeof json, &total, &count, &more);
    chk("子串 'a' 命中全部 1200", rc == IMG_OK && total == N && count == 10 && more, NULL);
    rc = img_lsc_search(tok, "zzz-nope", 0, 10, json, sizeof json, &total, &count, &more);
    chk("无命中 total=0", rc == IMG_OK && total == 0 && !more, NULL);

    printf("════ A3. 无效 token / 内存上限 / LRU ════\n");
    chk("无效 token → IMG_ENOENT",
        img_lsc_page("ls-0-999", 0, 10, json, sizeof json, &total, &count, &more) == IMG_ENOENT, NULL);
    /* 内存上限：10 万条 × ~600B path ≈ 64MB+ → 应当拒绝 */
    {
        const size_t BIG = 100000;
        img_lsc_ent *bv = mk_ents(BIG, 700);
        char bt[40];
        int brc = bv ? img_lsc_open(bv, BIG, false, bt, sizeof bt) : IMG_ENOMEM;
        chk("超 64MB 的目录 → ENOMEM（不炸内存）", brc == IMG_ENOMEM, NULL);
        if(bv) free_ents(bv, BIG);
    }
    /* LRU：连开 9 个会话（上限 8）→ 第 1 个被淘汰 */
    {
        char t[9][40]; int all_ok = 1;
        img_lsc_ent *small = mk_ents(3, 4);
        for(int i = 0; i < 9; i++){
            if(img_lsc_open(small, 3, false, t[i], sizeof t[i]) != IMG_OK) all_ok = 0;
        }
        chk("连开 9 个会话都成功（LRU 淘汰最旧）", all_ok, NULL);
        /* 连开 9 个（上限 8）→ 淘汰的是 last_ms 最旧的会话（A1 那个）*/
        chk("最旧的会话（A1 的 token）已被淘汰",
            img_lsc_page(tok, 0, 3, json, sizeof json, &total, &count, &more) == IMG_ENOENT, tok);
        chk("最新 token 有效",
            img_lsc_page(t[8], 0, 3, json, sizeof json, &total, &count, &more) == IMG_OK, t[8]);
        free_ents(small, 3);
    }

    /* ---------------- B. 命令层粘合（需要 WIM） ---------------- */
    if(argc >= 2 && argv[1][0]){
        printf("════ B. 命令层：wim.ls --open → --token → wim.search ════\n");
        img_result r;
        rc = run_cmd("wim.ls", "open", "true", "wim", argv[1], "path", "/many", NULL, NULL, &r);
        /* ★ token 要立刻拷出来：res_str 指向 r 内部，下一次 run_cmd 会 memset 掉 */
        char ctokbuf[40] = "";
        const char *ct0 = res_str(&r, "token");
        if(ct0) snprintf(ctokbuf, sizeof ctokbuf, "%s", ct0);
        const char *ctok = ctokbuf;
        long long ctotal = res_num(&r, "total");
        chk("wim.ls --open 成功并返回 token", rc == IMG_OK && ctok && ctok[0] && ctotal > 0, ctok);

        char offbuf[32] = "0";
        static char big[600000];
        if(ctok && ctok[0]){
            /* 同进程翻两页（不带 --wim，证明脱离容器） */
            rc = run_cmd("wim.ls", "token", ctok, "path", "/many", "offset", "0", "limit", "500", &r);
            long long c0 = res_num(&r, "count");
            chk("token 翻页第 1 页（不传 --wim）", rc == IMG_OK && c0 == 500, NULL);
            chk("has_more=true", res_bool(&r, "has_more") == 1, NULL);

            rc = run_cmd("wim.search", "token", ctok, "q", "f0042", "limit", "10", NULL, NULL, &r);
            long long st = res_num(&r, "total");
            printf("     [dbg] search rc=%d total=%lld count=%lld nf=%d\n",
                   rc, st, res_num(&r, "count"), r.nf);
            char dbg[96];
            snprintf(dbg, sizeof dbg, "rc=%d total=%lld code=%s why=%s", rc, st,
                     r.ok ? "" : r.code, r.why);
            chk("wim.search 命令层（q=f0042 → 1 条）", rc == IMG_OK && st == 1, dbg);
        }
        (void)offbuf; (void)big;
    } else {
        printf("（B 部分跳过：未传 WIM 路径）\n");
    }

    printf("\n════════ lscache_test：通过 %d · 失败 %d ════════\n",
           total_chk - fails, fails);
    return fails ? 1 : 0;
}