/* img_lscache.c — 目录会话缓存（listing session，2026-11-18）
 *
 * 翻页 O(1)：一次遍历把目录条目全部缓存进进程内存；之后每页只是切片渲染。
 * 见 include/img_lscache.h 的设计说明。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "img_lscache.h"
#include "img_types.h"   /* IMG_OK/IMG_ENOENT/IMG_ENOMEM */

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static struct {
    int          used;
    char         token[40];
    long long    last_ms;
    img_lsc_ent *v;
    size_t       n;
    bool         trunc;
    size_t       mem;          /* 近似字节：结构 + path（用于总量上限）*/
} g_sess[IMG_LSC_MAX_SESS];

static unsigned long g_seq = 0;
static bool g_atexit_done = false;

static long long now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- 与 wim.ls 完全一致的条目渲染（App 解析依赖字段名） ---------- */

/* UTF-8 序列校验（与 img_cmds.c 的 wj_esc 同规则：非法字节 → U+FFFD） */
static int u8_seq(const unsigned char *p){
    unsigned char c = p[0];
    if(c < 0x80) return 1;
    if(c >= 0xC2 && c <= 0xDF) return (p[1] >= 0x80 && p[1] <= 0xBF) ? 2 : 0;
    if(c >= 0xE0 && c <= 0xEF){
        if(!(p[1] >= 0x80 && p[1] <= 0xBF) || !(p[2] >= 0x80 && p[2] <= 0xBF)) return 0;
        if(c == 0xE0 && p[1] < 0xA0) return 0;
        if(c == 0xED && p[1] > 0x9F) return 0;
        return 3;
    }
    if(c >= 0xF0 && c <= 0xF4){
        if(!(p[1] >= 0x80 && p[1] <= 0xBF) || !(p[2] >= 0x80 && p[2] <= 0xBF) ||
           !(p[3] >= 0x80 && p[3] <= 0xBF)) return 0;
        if(c == 0xF0 && p[1] < 0x90) return 0;
        if(c == 0xF4 && p[1] > 0x8F) return 0;
        return 4;
    }
    return 0;
}

/* JSON 字符串转义（不含首尾引号；截断时按字节停，宁可少一个字符也不出非法 JSON） */
static void esc(char *d, size_t cap, const char *s){
    size_t j = 0;
    for(; s && *s && j + 7 < cap; ){
        unsigned char c = (unsigned char)*s;
        if(c >= 0x80){
            int n = u8_seq((const unsigned char*)s);
            if(n > 0){
                if(j + (size_t)n < cap){ memcpy(d + j, s, (size_t)n); j += (size_t)n; }
                s += n;
                continue;
            }
            memcpy(d + j, "\xef\xbf\xbd", 3); j += 3; s++;      /* U+FFFD */
            continue;
        }
        switch(c){
        case '"':  memcpy(d + j, "\\\"", 2); j += 2; break;
        case '\\': memcpy(d + j, "\\\\", 2); j += 2; break;
        case '\n': memcpy(d + j, "\\n", 2);  j += 2; break;
        case '\r': memcpy(d + j, "\\r", 2);  j += 2; break;
        case '\t': memcpy(d + j, "\\t", 2);  j += 2; break;
        default:
            if(c < 0x20) j += (size_t)snprintf(d + j, cap - j, "\\u%04x", c);
            else d[j++] = (char)c;
            break;
        }
        s++;
    }
    d[j] = 0;
}

static const char *basename_of(const char *path){
    const char *sl = path ? strrchr(path, '/') : NULL;
    return (sl && sl[1]) ? sl + 1 : (path ? path : "");
}

static int emit_item(char *p, size_t cap, const img_lsc_ent *e, int first){
    char nm[512], fp[2048];
    esc(nm, sizeof nm, basename_of(e->path));
    esc(fp, sizeof fp, e->path ? e->path : "");
    return snprintf(p, cap,
        "%s{\"name\":\"%s\",\"path\":\"%s\",\"dir\":%s,\"size\":%llu,\"mtime\":%llu,\"attrs\":%u,\"depth\":%u}",
        first ? "" : ",", nm, fp,
        e->isdir ? "true" : "false",
        (unsigned long long)e->size, (unsigned long long)e->mtime,
        (unsigned)e->attrs, (unsigned)e->depth);
}

/* ---------- 会话表 ---------- */

static void sess_free(int i){
    if(!g_sess[i].used) return;
    for(size_t k = 0; k < g_sess[i].n; k++) free(g_sess[i].v[k].path);
    free(g_sess[i].v);
    g_sess[i].v = NULL;
    g_sess[i].n = 0;
    g_sess[i].used = 0;
    g_sess[i].token[0] = 0;
    g_sess[i].mem = 0;
}

void img_lsc_shutdown(void){
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < IMG_LSC_MAX_SESS; i++) sess_free(i);
    pthread_mutex_unlock(&g_mu);
}

void img_lsc_gc(void){
    long long t = now_ms();
    pthread_mutex_lock(&g_mu);
    for(int i = 0; i < IMG_LSC_MAX_SESS; i++)
        if(g_sess[i].used && t - g_sess[i].last_ms > IMG_LSC_TTL_MS) sess_free(i);
    pthread_mutex_unlock(&g_mu);
}

int img_lsc_open(const img_lsc_ent *ents, size_t n, bool truncated,
                 char *token, size_t tokcap){
    if((!ents && n) || !token || tokcap < 16) return IMG_EINVAL;

    /* 内存预算：结构 + 每条 path */
    size_t mem = n * sizeof(img_lsc_ent);
    for(size_t i = 0; i < n; i++) mem += (ents[i].path ? strlen(ents[i].path) : 0) + 1;
    if(mem > IMG_LSC_MEM_CAP) return IMG_ENOMEM;

    img_lsc_ent *v = calloc(n ? n : 1, sizeof(img_lsc_ent));
    if(!v) return IMG_ENOMEM;
    for(size_t i = 0; i < n; i++){
        v[i] = ents[i];
        v[i].path = strdup(ents[i].path ? ents[i].path : "");
        if(!v[i].path){
            for(size_t k = 0; k < i; k++) free(v[k].path);
            free(v);
            return IMG_ENOMEM;
        }
    }

    pthread_mutex_lock(&g_mu);
    int slot = -1, oldest = -1;
    long long oldest_t = 0;
    for(int i = 0; i < IMG_LSC_MAX_SESS; i++){
        if(!g_sess[i].used){ slot = i; break; }
        if(oldest < 0 || g_sess[i].last_ms < oldest_t){ oldest = i; oldest_t = g_sess[i].last_ms; }
    }
    if(slot < 0) slot = oldest;
    if(slot < 0){ pthread_mutex_unlock(&g_mu); for(size_t k = 0; k < n; k++) free(v[k].path); free(v); return IMG_ENOMEM; }
    sess_free(slot);
    snprintf(token, tokcap, "ls-%ld-%lu", (long)getpid(), ++g_seq);
    g_sess[slot].used = 1;
    snprintf(g_sess[slot].token, sizeof g_sess[slot].token, "%s", token);
    g_sess[slot].last_ms = now_ms();
    g_sess[slot].v = v;
    g_sess[slot].n = n;
    g_sess[slot].trunc = truncated;
    g_sess[slot].mem = mem;
    if(!g_atexit_done){ g_atexit_done = true; atexit(img_lsc_shutdown); }
    pthread_mutex_unlock(&g_mu);
    return IMG_OK;
}

static int find_locked(const char *token){
    if(!token || !*token) return -1;
    for(int i = 0; i < IMG_LSC_MAX_SESS; i++)
        if(g_sess[i].used && !strcmp(g_sess[i].token, token)) return i;
    return -1;
}

int img_lsc_page(const char *token, uint64_t offset, uint64_t limit,
                 char *json, size_t cap,
                 uint64_t *out_total, uint64_t *out_count, bool *out_has_more){
    if(!token || !json || cap < 8) return IMG_EINVAL;

    pthread_mutex_lock(&g_mu);
    int i = find_locked(token);
    if(i < 0){ pthread_mutex_unlock(&g_mu); return IMG_ENOENT; }
    g_sess[i].last_ms = now_ms();

    size_t total = g_sess[i].n;
    size_t p = 0, count = 0;
    json[p++] = '[';
    for(uint64_t k = offset; k < total && count < (size_t)limit; k++){
        int n = emit_item(json + p, cap - p - 2, &g_sess[i].v[k], count == 0);
        if(n <= 0 || (size_t)n >= cap - p - 2) break;   /* 装不下就停（正常不会发生）*/
        p += (size_t)n;
        count++;
    }
    json[p++] = ']';
    json[p] = 0;

    if(out_total) *out_total = total;
    if(out_count) *out_count = count;
    if(out_has_more) *out_has_more = (offset + count) < total;
    pthread_mutex_unlock(&g_mu);
    return IMG_OK;
}

/* 不区分大小写的子串匹配（"输入即搜"用；q 为空 = 全匹配） */
static bool ci_contains(const char *h, const char *q){
    if(!q || !*q) return true;
    size_t nq = strlen(q);
    for(; h && *h; h++){
        size_t k = 0;
        while(k < nq && h[k] &&
              tolower((unsigned char)h[k]) == tolower((unsigned char)q[k])) k++;
        if(k == nq) return true;
    }
    return false;
}

int img_lsc_search(const char *token, const char *q, uint64_t offset, uint64_t limit,
                   char *json, size_t cap,
                   uint64_t *out_total, uint64_t *out_count, bool *out_has_more){
    if(!token || !json || cap < 8) return IMG_EINVAL;

    pthread_mutex_lock(&g_mu);
    int i = find_locked(token);
    if(i < 0){ pthread_mutex_unlock(&g_mu); return IMG_ENOENT; }
    g_sess[i].last_ms = now_ms();

    size_t p = 0, rendered = 0;
    uint64_t matched = 0;
    json[p++] = '[';
    for(size_t k = 0; k < g_sess[i].n; k++){
        if(!ci_contains(g_sess[i].v[k].path, q)) continue;
        if(matched >= offset && rendered < (size_t)limit){
            int n = emit_item(json + p, cap - p - 2, &g_sess[i].v[k], rendered == 0);
            if(n <= 0 || (size_t)n >= cap - p - 2) break;
            p += (size_t)n;
            rendered++;
        }
        matched++;
    }
    json[p++] = ']';
    json[p] = 0;

    if(out_total) *out_total = matched;
    if(out_count) *out_count = rendered;
    if(out_has_more) *out_has_more = (offset + rendered) < matched;
    pthread_mutex_unlock(&g_mu);
    return IMG_OK;
}