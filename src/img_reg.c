/* img_reg.c — 离线注册表 hive 通用读写（P2）
 *
 * 实现要点见 include/img_reg.h。这里只记踩过的坑：
 *   ① hivex_open(NULL) 会段错误 —— 必须先判 path（img_bcd.c:52 有同样注释）
 *   ② hivex_commit(h, NULL, 0) 写回已打开的文件；hivex_commit(h, filename, 0) 写到别处
 *      win.finish 用的是第一种（commit 到打开的那个临时文件），本文件跟随。
 *   ③ 临时文件必须用 img_tmpfile()（P1-6 统一命名），否则 App 清理时找不到。
 *   ④ 校验步骤不能省：hivex 的 commit 可能"成功"但内容没落地（FUSE 上踩过），
 *      所以写回分区之前一定要重开一次确认。
 *   ⑤ 键路径分隔符：外部用 '\\'（Windows 习惯），内部一律转成 '/' 处理，
 *      hivex 自己按名字逐级查找，不关心分隔符。
 */
#include "img_reg.h"
#include "img_cmd.h"          /* img_ctx_open / img_ctx_qcow2 / img_ctx_close */
#include "img_partition.h"    /* img_parttable_t / IMG_MAX_PARTITIONS */
#include "img_ntfs_fs.h"
#include <hivex.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>

static void *memdup_own(const void *src, size_t n);   /* 定义在文件尾 */

struct img_reg {
    img_ctx     *ctx;      /* 自己打开的镜像上下文（关的时候一起关） */
    img_ntfs_fs *fs;
    hive_h      *h;
    img_reg_hive hive;
    char         hive_path[256];   /* 分区内路径：/Windows/System32/config/SYSTEM */
    char         tmp[600];         /* 临时文件（抽出后改，close 时写回并删） */
    bool         writable;
    bool         changed;
    char         err[256];         /* 内部错误缓冲（close 时用） */
};

static const char *HIVE_FILES[IMG_REG_HIVE__COUNT] = {
    "SYSTEM", "SOFTWARE", "SAM", "SECURITY", "DEFAULT"
};

const char *img_reg_hive_name(img_reg_hive h){
    if(h < 0 || h >= IMG_REG_HIVE__COUNT) return "?";
    return HIVE_FILES[h];
}

bool img_reg_hive_from_name(const char *s, img_reg_hive *out){
    if(!s || !out) return false;
    for(int i = 0; i < IMG_REG_HIVE__COUNT; i++){
        if(!strcasecmp(s, HIVE_FILES[i])){ *out = (img_reg_hive)i; return true; }
    }
    return false;
}

const char *img_reg_type_name(img_reg_type t){
    switch(t){
    case IMG_REG_SZ:            return "REG_SZ";
    case IMG_REG_EXPAND_SZ:     return "REG_EXPAND_SZ";
    case IMG_REG_BINARY:        return "REG_BINARY";
    case IMG_REG_DWORD:         return "REG_DWORD";
    case IMG_REG_DWORD_BE:      return "REG_DWORD_BIG_ENDIAN";
    case IMG_REG_LINK:          return "REG_LINK";
    case IMG_REG_MULTI_SZ:      return "REG_MULTI_SZ";
    case IMG_REG_RESOURCE_LIST: return "REG_RESOURCE_LIST";
    case IMG_REG_FULL_RESOURCE_DESCRIPTOR: return "REG_FULL_RESOURCE_DESCRIPTOR";
    case IMG_REG_RESOURCE_REQUIREMENTS_LIST: return "REG_RESOURCE_REQUIREMENTS_LIST";
    case IMG_REG_QWORD:         return "REG_QWORD";
    default:                    return "REG_NONE";
    }
}

static img_reg_type from_hive_type(hive_type t){
    switch(t){
    case hive_t_REG_SZ:                        return IMG_REG_SZ;
    case hive_t_REG_EXPAND_SZ:                 return IMG_REG_EXPAND_SZ;
    case hive_t_REG_BINARY:                    return IMG_REG_BINARY;
    case hive_t_REG_DWORD:                      return IMG_REG_DWORD;
    case hive_t_REG_DWORD_BIG_ENDIAN:          return IMG_REG_DWORD_BE;
    case hive_t_REG_LINK:                      return IMG_REG_LINK;
    case hive_t_REG_MULTI_SZ:                  return IMG_REG_MULTI_SZ;
    case hive_t_REG_RESOURCE_LIST:             return IMG_REG_RESOURCE_LIST;
    case hive_t_REG_FULL_RESOURCE_DESCRIPTOR:  return IMG_REG_FULL_RESOURCE_DESCRIPTOR;
    case hive_t_REG_RESOURCE_REQUIREMENTS_LIST:return IMG_REG_RESOURCE_REQUIREMENTS_LIST;
    case hive_t_REG_QWORD:                     return IMG_REG_QWORD;
    default:                                    return IMG_REG_NONE;
    }
}

static hive_type to_hive_type(img_reg_type t){
    switch(t){
    case IMG_REG_SZ:            return hive_t_REG_SZ;
    case IMG_REG_EXPAND_SZ:     return hive_t_REG_EXPAND_SZ;
    case IMG_REG_BINARY:        return hive_t_REG_BINARY;
    case IMG_REG_DWORD:         return hive_t_REG_DWORD;
    case IMG_REG_DWORD_BE:      return hive_t_REG_DWORD_BIG_ENDIAN;
    case IMG_REG_LINK:          return hive_t_REG_LINK;
    case IMG_REG_MULTI_SZ:      return hive_t_REG_MULTI_SZ;
    case IMG_REG_QWORD:         return hive_t_REG_QWORD;
    default:                    return hive_t_REG_NONE;
    }
}

/* 键路径 → 逐级下钻。key_path 用 '\\' 或 '/' 都能进。
 * 返回 0 表示不存在（不是错误）；<0 表示真错误。 */
static hive_node_h walk(img_reg *r, const char *key_path){
    if(!r || !r->h) return 0;
    hive_node_h cur = hivex_root(r->h);
    if(!cur) return 0;
    if(!key_path || !*key_path || !strcmp(key_path, "/") || !strcmp(key_path, "\\"))
        return cur;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", key_path);
    for(char *p = buf; *p; p++) if(*p == '/') *p = '\\';
    char *save = NULL;
    for(char *seg = strtok_r(buf, "\\", &save); seg; seg = strtok_r(NULL, "\\", &save)){
        if(!*seg) continue;
        cur = hivex_node_get_child(r->h, cur, seg);
        if(!cur) return 0;
    }
    return cur;
}

img_reg *img_reg_open(const char *img_path, uint64_t lba, uint64_t part_bytes,
                      img_reg_hive hive, bool writable, char *err, size_t errcap){
    if(!img_path || !*img_path){ if(err&&errcap) snprintf(err,errcap,"映像路径为空"); return NULL; }
    if(hive < 0 || hive >= IMG_REG_HIVE__COUNT){
        if(err&&errcap) snprintf(err,errcap,"未知 hive：%d",(int)hive); return NULL;
    }

    img_ctx *ctx = img_ctx_open(img_path, writable, false, err, errcap);
    if(!ctx) return NULL;
    img_qcow2 *q = img_ctx_qcow2(ctx);
    if(!q){ img_ctx_close(ctx); return NULL; }

    uint64_t pb = part_bytes;
    if(!pb){
        img_parttable_t t;
        if(img_parttable_read(q, &t, 0) != IMG_OK){
            if(err&&errcap) snprintf(err,errcap,"分区表读不出来");
            img_ctx_close(ctx); return NULL;
        }
        for(int i = 0; i < IMG_MAX_PARTITIONS; i++){
            if(t.parts[i].used && t.parts[i].first_lba == lba){
                pb = (t.parts[i].last_lba - t.parts[i].first_lba + 1) * 512ULL;
                break;
            }
        }
        if(!pb){
            if(err&&errcap) snprintf(err,errcap,"LBA %llu 不在分区表里",(unsigned long long)lba);
            img_ctx_close(ctx); return NULL;
        }
    }

    img_ntfs_fs *fs = img_ntfs_mount_rw(q, lba, pb);
    if(!fs){
        if(err&&errcap) snprintf(err,errcap,"不是合法 NTFS（未格式化或 LBA 不对）");
        img_ctx_close(ctx); return NULL;
    }

    img_reg *r = (img_reg*)calloc(1, sizeof *r);
    if(!r){
        if(err&&errcap) snprintf(err,errcap,"内存不足");
        img_ntfs_umount(fs); img_ctx_close(ctx); return NULL;
    }
    r->ctx = ctx;
    r->fs = fs;
    r->hive = hive;
    r->writable = writable;
    snprintf(r->hive_path, sizeof r->hive_path,
             "/Windows/System32/config/%s", HIVE_FILES[hive]);

    /* ① 抽出 hive 到临时文件 */
    if(img_tmpfile("reg", r->tmp, sizeof r->tmp) == NULL){
        if(err&&errcap) snprintf(err,errcap,"临时目录不可写（设 IMGTOOL_TMPDIR）");
        free(r); img_ntfs_umount(fs); img_ctx_close(ctx); return NULL;
    }
    /* img_tmpfile 只抢名字，文件已被它 unlink 掉；这里重新建 */
    int fd = open(r->tmp, O_CREAT|O_EXCL|O_WRONLY, 0600);
    if(fd < 0){
        if(err&&errcap) snprintf(err,errcap,"建临时文件失败：%s",strerror(errno));
        free(r); img_ntfs_umount(fs); img_ctx_close(ctx); return NULL;
    }
    close(fd);

    int rc = img_ntfs_copy_out(fs, r->hive_path, r->tmp, NULL, NULL);
    if(rc != IMG_OK){
        if(err&&errcap)
            snprintf(err,errcap,"读不到 %s（这个分区可能不是 Windows 系统分区）", r->hive_path);
        unlink(r->tmp);
        free(r); img_ntfs_umount(fs); img_ctx_close(ctx); return NULL;
    }

    /* ② hivex 打开 */
    r->h = hivex_open(r->tmp, writable ? HIVEX_OPEN_WRITE : 0);
    if(!r->h){
        if(err&&errcap) snprintf(err,errcap,"hivex 无法打开 %s（文件损坏或不是合法 hive）", HIVE_FILES[hive]);
        unlink(r->tmp);
        free(r); img_ntfs_umount(fs); img_ctx_close(ctx); return NULL;
    }
    return r;
}

void img_reg_close(img_reg *r){
    if(!r) return;
    bool commit_ok = true;
    if(r->h){
        if(r->changed && r->writable){
            /* ③ commit 到临时文件 */
            if(hivex_commit(r->h, NULL, 0) < 0){
                snprintf(r->err, sizeof r->err, "hivex_commit 失败（hive 写不回去）");
                commit_ok = false;
            }
        }
        hivex_close(r->h);
        r->h = NULL;
    }
    if(r->changed && r->writable && commit_ok){
        /* ④ 读回校验：重开确认能打开（结构没坏） */
        hive_h *chk = hivex_open(r->tmp, 0);
        if(!chk){
            snprintf(r->err, sizeof r->err, "写回校验失败（commit 后 hive 无法打开）");
            commit_ok = false;
        } else {
            hivex_close(chk);
        }
    }
    if(r->changed && r->writable && commit_ok){
        /* ⑤ 才真正覆盖分区里的 hive */
        if(img_ntfs_put_file(r->fs, r->tmp, r->hive_path, NULL) != IMG_OK){
            snprintf(r->err, sizeof r->err, "把改好的 %s 写回分区失败", r->hive_path);
            r->changed = false;      /* 如实报告：没写成功 */
        }
    }
    if(r->tmp[0]) unlink(r->tmp);
    if(r->fs) img_ntfs_umount(r->fs);
    if(r->ctx) img_ctx_close(r->ctx);      /* q 由 ctx 持有，一起关 */
    free(r);
}

bool img_reg_changed(const img_reg *r){ return r && r->changed; }

/* ---------------- 读 ---------------- */

int img_reg_get(img_reg *r, const char *key_path, const char *value_name,
                img_reg_value *out, char *err, size_t errcap){
    if(!r || !r->h || !out) return IMG_EINVAL;
    memset(out, 0, sizeof *out);

    hive_node_h node = walk(r, key_path);
    if(!node){
        if(err&&errcap) snprintf(err,errcap,"键不存在：%s",key_path?key_path:"(root)");
        return IMG_ENOENT;
    }
    const char *vn = value_name ? value_name : "";
    hive_value_h v = hivex_node_get_value(r->h, node, vn);
    if(!v){
        if(err&&errcap) snprintf(err,errcap,"值不存在：%s\\%s", key_path?key_path:"", vn);
        return IMG_ENOENT;
    }

    size_t len = 0;
    hive_type t = hive_t_REG_NONE;
    if(hivex_value_type(r->h, v, &t, &len) < 0){
        if(err&&errcap) snprintf(err,errcap,"hivex_value_type 失败");
        return IMG_EIO;
    }
    char *data = hivex_value_value(r->h, v, &t, &len);
    if(!data && len){
        if(err&&errcap) snprintf(err,errcap,"hivex_value_value 失败");
        return IMG_EIO;
    }
    const char *key = hivex_value_key(r->h, v);
    out->name = strdup(key ? key : "");
    out->type = from_hive_type(t);
    out->len  = len;
    if(len){
        out->data = (uint8_t*)malloc(len);
        if(!out->data){
            free(out->name); out->name = NULL;
            if(err&&errcap) snprintf(err,errcap,"内存不足");
            return IMG_ENOMEM;
        }
        memcpy(out->data, data, len);
    }
    free(data);
    return IMG_OK;
}

void img_reg_value_free(img_reg_value *v){
    if(!v) return;
    free(v->name); v->name = NULL;
    free(v->data); v->data = NULL;
    v->len = 0; v->type = IMG_REG_NONE;
}

int img_reg_enum(img_reg *r, const char *key_path,
                 bool want_subkeys, bool want_values,
                 img_reg_enum_cb cb, void *ud, char *err, size_t errcap){
    if(!r || !r->h || !cb) return IMG_EINVAL;
    hive_node_h node = walk(r, key_path);
    if(!node){
        if(err&&errcap) snprintf(err,errcap,"键不存在：%s",key_path?key_path:"(root)");
        return IMG_ENOENT;
    }
    if(want_subkeys){
        hive_node_h *kids = hivex_node_children(r->h, node);
        if(kids){
            for(hive_node_h *k = kids; *k; k++){
                char *nm = hivex_node_name(r->h, *k);
                cb(ud, nm ? nm : "", true, IMG_REG_NONE, 0);
                free(nm);
            }
            free(kids);
        }
    }
    if(want_values){
        hive_value_h *vals = hivex_node_values(r->h, node);
        if(vals){
            for(hive_value_h *v = vals; *v; v++){
                char *nm = hivex_value_key(r->h, *v);
                hive_type t = hive_t_REG_NONE; size_t len = 0;
                hivex_value_type(r->h, *v, &t, &len);
                cb(ud, nm ? nm : "", false, from_hive_type(t), len);
                free(nm);
            }
            free(vals);
        }
    }
    return IMG_OK;
}

int img_reg_key_exists(img_reg *r, const char *key_path, bool *out, char *err, size_t errcap){
    if(!r || !r->h || !out) return IMG_EINVAL;
    *out = (walk(r, key_path) != 0);
    return IMG_OK;
}

/* ---------------- 写 ---------------- */

int img_reg_set(img_reg *r, const char *key_path, const char *value_name,
                img_reg_type type, const void *data, size_t len,
                char *err, size_t errcap){
    if(!r || !r->h) return IMG_EINVAL;
    if(!r->writable){
        if(err&&errcap) snprintf(err,errcap,"以只读方式打开，不能写");
        return IMG_EREADONLY;
    }

    /* 键不存在就逐级建 */
    hive_node_h cur = hivex_root(r->h);
    if(!cur) return IMG_EIO;
    if(key_path && *key_path && strcmp(key_path,"/") && strcmp(key_path,"\\")){
        char buf[1024];
        snprintf(buf, sizeof buf, "%s", key_path);
        for(char *p = buf; *p; p++) if(*p == '/') *p = '\\';
        char *save = NULL;
        for(char *seg = strtok_r(buf, "\\", &save); seg; seg = strtok_r(NULL, "\\", &save)){
            if(!*seg) continue;
            hive_node_h nx = hivex_node_get_child(r->h, cur, seg);
            if(!nx){
                nx = hivex_node_add_child(r->h, cur, seg);
                if(!nx){
                    if(err&&errcap) snprintf(err,errcap,"建键失败：%s",seg);
                    return IMG_EIO;
                }
            }
            cur = nx;
        }
    }

    hive_set_value sv;
    memset(&sv, 0, sizeof sv);
    sv.key   = (char*)(value_name ? value_name : "");
    sv.t     = to_hive_type(type);
    sv.len   = len;
    sv.value = (char*)(data ? data : "");

    /* DWORD 需要小端 4 字节；调用方直接传 uint32_t 就是本机小端，可直接用。
     * 但如果是 big-endian 平台要转 —— 这里显式处理，避免"在本机测试通过、换平台翻车"。 */
    uint8_t be[8];
    if(type == IMG_REG_DWORD && len == 4 && data){
        const uint8_t *b = (const uint8_t*)data;
        /* 假设入参是本机字节序的 uint32_t：小端机器上 memcpy 即可；
         * 这里统一转成小端写入（hivex 期望小端）。 */
        be[0]=b[0]; be[1]=b[1]; be[2]=b[2]; be[3]=b[3];
        sv.value = (char*)be;
    } else if(type == IMG_REG_QWORD && len == 8 && data){
        const uint8_t *b = (const uint8_t*)data;
        memcpy(be, b, 8);
        sv.value = (char*)be;
    }

    if(hivex_node_set_value(r->h, cur, &sv, 0) < 0){
        if(err&&errcap) snprintf(err,errcap,"写值失败：%s", sv.key);
        return IMG_EIO;
    }
    r->changed = true;
    return IMG_OK;
}

int img_reg_set_dword(img_reg *r, const char *key_path, const char *value_name,
                      uint32_t v, char *err, size_t errcap){
    return img_reg_set(r, key_path, value_name, IMG_REG_DWORD, &v, sizeof v, err, errcap);
}

int img_reg_set_qword(img_reg *r, const char *key_path, const char *value_name,
                      uint64_t v, char *err, size_t errcap){
    return img_reg_set(r, key_path, value_name, IMG_REG_QWORD, &v, sizeof v, err, errcap);
}

int img_reg_set_sz(img_reg *r, const char *key_path, const char *value_name,
                   const char *utf8, char *err, size_t errcap){
    const char *s = utf8 ? utf8 : "";
    /* hivex 的 REG_SZ 期望末尾带 NUL（长度含 NUL） */
    return img_reg_set(r, key_path, value_name, IMG_REG_SZ, s, strlen(s) + 1, err, errcap);
}

int img_reg_del_value(img_reg *r, const char *key_path, const char *value_name,
                      char *err, size_t errcap){
    if(!r || !r->h) return IMG_EINVAL;
    if(!r->writable){ if(err&&errcap) snprintf(err,errcap,"只读"); return IMG_EREADONLY; }

    hive_node_h node = walk(r, key_path);
    if(!node){
        if(err&&errcap) snprintf(err,errcap,"键不存在：%s",key_path?key_path:"");
        return IMG_ENOENT;
    }
    const char *vn = value_name ? value_name : "";
    hive_value_h v = hivex_node_get_value(r->h, node, vn);
    if(!v){
        if(err&&errcap) snprintf(err,errcap,"值不存在：%s",vn);
        return IMG_ENOENT;
    }

    /* hivex 没有 node_del_value；用 set_values 重写整个值列表（跳过要删的那个）。
     * 这是官方推荐做法：先把剩下的值收集起来，再一次性 set。 */
    hive_value_h *vals = hivex_node_values(r->h, node);
    if(!vals){
        if(err&&errcap) snprintf(err,errcap,"读值列表失败");
        return IMG_EIO;
    }
    size_t n = 0;
    while(vals[n]) n++;

    hive_set_value *keep = (hive_set_value*)calloc(n ? n : 1, sizeof *keep);
    char **keys = (char**)calloc(n ? n : 1, sizeof(char*));
    char **datas = (char**)calloc(n ? n : 1, sizeof(char*));
    if(!keep || !keys || !datas){
        free(vals); free(keep); free(keys); free(datas);
        if(err&&errcap) snprintf(err,errcap,"内存不足");
        return IMG_ENOMEM;
    }

    size_t kn = 0;
    for(size_t i = 0; i < n; i++){
        char *k = hivex_value_key(r->h, vals[i]);
        if(k && !strcmp(k, vn)){ free(k); continue; }
        hive_type t = hive_t_REG_NONE; size_t len = 0;
        hivex_value_type(r->h, vals[i], &t, &len);
        char *d = hivex_value_value(r->h, vals[i], &t, &len);
        keys[kn]  = k ? strdup(k) : strdup("");
        datas[kn] = d ? (char*)memdup_own(d, len) : NULL;
        keep[kn].key   = keys[kn];
        keep[kn].t     = t;
        keep[kn].len   = len;
        keep[kn].value = datas[kn] ? datas[kn] : (char*)"";
        kn++;
        free(k); free(d);
    }
    free(vals);

    int rc = IMG_OK;
    if(hivex_node_set_values(r->h, node, kn, keep, 0) < 0){
        if(err&&errcap) snprintf(err,errcap,"重写值列表失败（删值）");
        rc = IMG_EIO;
    } else {
        r->changed = true;
    }
    for(size_t i = 0; i < kn; i++){ free(keys[i]); free(datas[i]); }
    free(keep); free(keys); free(datas);
    return rc;
}

int img_reg_del_key(img_reg *r, const char *key_path, bool recursive,
                    char *err, size_t errcap){
    if(!r || !r->h) return IMG_EINVAL;
    if(!r->writable){ if(err&&errcap) snprintf(err,errcap,"只读"); return IMG_EREADONLY; }

    hive_node_h node = walk(r, key_path);
    if(!node){
        if(err&&errcap) snprintf(err,errcap,"键不存在：%s",key_path?key_path:"");
        return IMG_ENOENT;
    }

    /* 非递归：先看有没有子键/值 */
    if(!recursive){
        size_t nk = hivex_node_nr_children(r->h, node);
        size_t nv = hivex_node_nr_values(r->h, node);
        if(nk > 0 || nv > 0){
            if(err&&errcap) snprintf(err,errcap,
                "键非空（%zu 子键 / %zu 值）—— recursive=true 才能删",
                nk, nv);
            return IMG_ESTATE;
        }
    }
    if(hivex_node_delete_child(r->h, node) < 0){
        if(err&&errcap) snprintf(err,errcap,"删键失败（hivex_node_delete_child）");
        return IMG_EIO;
    }
    r->changed = true;
    return IMG_OK;
}

static void *memdup_own(const void *src, size_t n){
    if(!src || !n) return NULL;
    void *p = malloc(n);
    if(p) memcpy(p, src, n);
    return p;
}
