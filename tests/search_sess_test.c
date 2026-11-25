/* search_sess_test — img.search 的会话隔离（同进程；CLI 跨进程测不了）
 *
 * 为什么必须同进程测：CLI 每条命令一个进程，token 一出门就 TOKEN-EXPIRED。
 * App 是常驻进程，这才是真实用法。
 *
 * 覆盖（用户 2026-11-19 明确担心的「搜索之后会不会污染当前目录、翻页重复/缺失/串目录」）：
 *   ① 对 NTFS 搜一次、对 zip 搜一次 → 两个 token、两个 total
 *   ② 交叉翻页：A 的页只有 /tree/…（NTFS），B 的页只有包内路径 —— 不串
 *   ③ 中间插一次【失败】搜索（坏 spec）→ A 的 token 仍能翻页（失败不污染已有会话）
 *   ④ 假 token → TOKEN-EXPIRED（错误码对）
 *   ⑤ 每次翻页报告的 total 恒等于首次搜索的 total（不会被别的会话改写）
 *
 * 用法: search_sess_test <qcow2> <lba> <zip>
 *       素材由 search-all-test.py 造好（NTFS 树 /tree 下含 TargetA_*，zip 内含 TargetA_*）。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "img_cmd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0, total_chk = 0;
static void chk(const char *w, int ok, const char *extra){
    total_chk++;
    printf("  %s %s%s%s\n", ok ? "✅" : "❌", w,
           (extra && extra[0]) ? "  " : "", (extra && extra[0]) ? extra : "");
    if(!ok) fails++;
}

static const char *res_str(const img_result *r, const char *k){
    for(int i = 0; i < r->nf; i++) if(!strcmp(r->f[i].k, k)) return r->f[i].s;
    return NULL;
}
static const char *res_raw(const img_result *r, const char *k){
    for(int i = 0; i < r->nf; i++) if(!strcmp(r->f[i].k, k)) return r->f[i].raw;
    return NULL;
}
static long long res_num(const img_result *r, const char *k){
    for(int i = 0; i < r->nf; i++) if(!strcmp(r->f[i].k, k)) return r->f[i].n;
    return -1;
}
static const char *res_code(const img_result *r){
    return r->ok ? "" : r->code;
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
    if(argc < 4){
        printf("用法: search_sess_test <qcow2> <lba> <zip>\n");
        return 77;
    }
    char spec_a[1200], spec_b[1200];
    snprintf(spec_a, sizeof spec_a, "%s!lba%s:/tree", argv[1], argv[2]);
    snprintf(spec_b, sizeof spec_b, "%s!:/", argv[3]);

    img_result r;
    char tok_a[40] = "", tok_b[40] = "";
    long long total_a = 0, total_b = 0;
    const char *s;

    /* ① 两次搜索，两个会话 */
    int rc = run_cmd("img.search", "spec", spec_a, "q", "targeta",
                     "limit", "10", NULL, NULL, &r);
    s = res_str(&r, "token");
    if(s) snprintf(tok_a, sizeof tok_a, "%s", s);      /* ★ 立刻拷出来：下一次 run_cmd 会 memset */
    total_a = res_num(&r, "total");
    chk("① NTFS 搜索拿到 token", rc == IMG_OK && tok_a[0] && total_a > 0, tok_a);

    rc = run_cmd("img.search", "spec", spec_b, "q", "targeta",
                 "limit", "10", NULL, NULL, &r);
    s = res_str(&r, "token");
    if(s) snprintf(tok_b, sizeof tok_b, "%s", s);
    total_b = res_num(&r, "total");
    chk("① zip 搜索拿到另一个 token", rc == IMG_OK && tok_b[0] && total_b > 0, tok_b);
    chk("① 两个 token 不同", strcmp(tok_a, tok_b) != 0, NULL);

    /* ② 交叉翻页：不串目录 */
    rc = run_cmd("img.search", "token", tok_a, "offset", "10", "limit", "10", NULL, NULL, &r);
    const char *ra = res_raw(&r, "entries");
    long long ta = res_num(&r, "total");
    /* ★ 按 path 特征判断：NTFS 条目的 name 本身也含 TargetA_，
     *   拿整个 JSON 找关键字会误判。zip 的路径形如 "path":"/d5/… */
    int a_ok = (rc == IMG_OK && ra && strstr(ra, "\"path\":\"/tree/")
                && !strstr(ra, "\"path\":\"/d"));
    char info[240];
    snprintf(info, sizeof info, "rc=%d total=%lld entries=%.100s", rc, ta, ra ? ra : "");
    chk("② 翻 A 的页：只有 NTFS 路径、没有包内路径", a_ok, info);
    chk("② 翻 A 的 total 仍等于首次搜索的 total", ta == total_a, NULL);

    rc = run_cmd("img.search", "token", tok_b, "offset", "10", "limit", "10", NULL, NULL, &r);
    const char *rb = res_raw(&r, "entries");
    long long tb = res_num(&r, "total");
    int b_ok = (rc == IMG_OK && rb && strstr(rb, "\"path\":\"/d")
                && !strstr(rb, "\"path\":\"/tree/"));
    chk("② 翻 B 的页：只有包内路径、没有 NTFS 路径", b_ok, rb ? rb : "");
    chk("② 翻 B 的 total 仍等于首次搜索的 total", tb == total_b, NULL);

    /* ③ 中间插一次失败搜索（坏 spec）→ 已有会话不受影响 */
    rc = run_cmd("img.search", "spec", "!!!", "q", "x", NULL, NULL, NULL, NULL, &r);
    chk("③ 坏 spec 报错（CMD-ARG-BAD）", rc != IMG_OK && !strcmp(res_code(&r), "CMD-ARG-BAD"),
        res_code(&r));

    rc = run_cmd("img.search", "token", tok_a, "offset", "20", "limit", "10", NULL, NULL, &r);
    const char *ra2 = res_raw(&r, "entries");
    chk("③ 失败搜索之后 A 的 token 仍能翻页", rc == IMG_OK && ra2 && strstr(ra2, "/tree/"), NULL);
    chk("③ 且 total 没被改写", res_num(&r, "total") == total_a, NULL);

    /* ④ 假 token */
    rc = run_cmd("img.search", "token", "ls-999999-99", "limit", "10", NULL, NULL, NULL, NULL, &r);
    chk("④ 假 token → TOKEN-EXPIRED", rc != IMG_OK && !strcmp(res_code(&r), "TOKEN-EXPIRED"),
        res_code(&r));

    printf("\n════════ search_sess_test：通过 %d · 失败 %d ════════\n",
           total_chk - fails, fails);
    return fails ? 1 : 0;
}