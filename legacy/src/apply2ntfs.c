/*
 * apply2ntfs: 把 WIM/ESD 镜像的内容灌入 NTFS 卷文件
 *  - 元数据来自 libwim (ACL/属性/硬链接组/时间戳/8.3名)
 *  - 数据来自 wimlib-imagex 已解压的临时目录
 *  - 写入用 libntfs-3g
 *
 * 用法: apply2ntfs <esd> <image> <tempdir> <ntfs_volume>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <wimlib.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/security.h>
#include <ntfs-3g/layout.h>

#ifndef S_IFDIR
#define S_IFDIR 0040000
#define S_IFREG 0100000
#endif

/* ---------- 参数与状态 ---------- */
static const char *g_esd, *g_image, *g_tempdir, *g_ntfs;
static ntfs_volume *g_vol;
static ntfs_inode  *g_root;
static struct SECURITY_CONTEXT g_scx;

/* 目录路径 -> MFT 号（开放寻址哈希） */
struct pmap { uint64_t h; ntfs_inode *ni; char *path; int used; };
#define PM_SIZE (1u << 17)
static struct pmap g_pm[PM_SIZE];
static uint64_t str_hash(const char *s){ uint64_t h=1469598103934665603ULL; while(*s){h^=(unsigned char)*s++; h*=1099511628211ULL;} return h; }
static ntfs_inode *pm_lookup(const char *path){
    uint64_t h = str_hash(path), i = h & (PM_SIZE-1);
    while (g_pm[i].used) { if (g_pm[i].h==h && strcmp(g_pm[i].path,path)==0) return g_pm[i].ni; i=(i+1)&(PM_SIZE-1); }
    return NULL;
}
/* 取出并放弃某路径的句柄所有权（不清空调用方持有的指针责任） */
static void pm_insert(const char *path, ntfs_inode *ni){
    uint64_t h = str_hash(path), i = h & (PM_SIZE-1);
    while (g_pm[i].used) { if (g_pm[i].h==h && strcmp(g_pm[i].path,path)==0) { g_pm[i].ni = ni; return; } i=(i+1)&(PM_SIZE-1); }
    g_pm[i].used=1; g_pm[i].h=h; g_pm[i].ni=ni; g_pm[i].path=strdup(path);
}
static void pm_close_all(void){
    for (size_t i = 0; i < PM_SIZE; i++) if (g_pm[i].used && g_pm[i].ni && g_pm[i].ni != g_root) { ntfs_inode_close(g_pm[i].ni); g_pm[i].ni = NULL; }
}
static const char *g_prefix = NULL;
static int g_verbose = 0;
/* 目录句柄常驻：同一目录只 open 一次并一直持有。
 * 实测：每个文件都 open/close 父目录会让 libntfs-3g 回写陈旧索引 → 产生重复条目。 */
static ntfs_inode *get_dir(const char *path){
    if (!path || !*path) return g_root;
    return pm_lookup(path);
}
static void put_dir(ntfs_inode *ni){ (void)ni; }  /* 目录句柄常驻 */

/* 硬链接组 -> 已创建 inode 的 MFT 号 */
struct hl { uint64_t group; uint64_t mft; int used; };
#define HL_SIZE (1u << 20)
static struct hl g_hl[HL_SIZE];
static uint64_t hl_hash(uint64_t g){ g^=g>>33; g*=0xff51afd7ed558ccdULL; g^=g>>33; return g; }
static int64_t hl_lookup(uint64_t group){
    uint64_t i = hl_hash(group) & (HL_SIZE-1);
    while (g_hl[i].used) { if (g_hl[i].group == group) return g_hl[i].mft; i = (i+1) & (HL_SIZE-1); }
    return -1;
}
static void hl_insert(uint64_t group, uint64_t mft){
    uint64_t i = hl_hash(group) & (HL_SIZE-1);
    while (g_hl[i].used) { if (g_hl[i].group == group) return; i = (i+1) & (HL_SIZE-1); }
    g_hl[i].used = 1; g_hl[i].group = group; g_hl[i].mft = mft;
}

/* 统计 */
static struct {
    uint64_t dirs, files, bytes, hl_created, hl_linked, acl_ok, acl_fail,
             attr_ok, attr_fail, create_fail, data_fail, skipped, reparse, other;
} g_st;

static u8 to_ntfschar(ntfschar *dst, const char *s, int max){
    u8 n = 0; while (*s && n < max) dst[n++] = (ntfschar)(unsigned char)*s++; return n;
}

static const char *base_name(const char *p){ const char *s = strrchr(p, '/'); return s ? s+1 : p; }

/* ---------- 临时目录 -> NTFS ---------- */
static int copy_stream(const char *src, ntfs_attr *na){
    FILE *f = fopen(src, "rb");
    if (!f) return -errno;
    char buf[1<<20];
    int64_t off = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (ntfs_attr_pwrite(na, off, n, buf) != (s64)n) { fclose(f); return -EIO; }
        off += n;
    }
    int rc = ferror(f) ? -EIO : 0;
    fclose(f);
    return rc;
}

static void set_times(ntfs_inode *ni, const struct wimlib_timespec *c,
                      const struct wimlib_timespec *w, const struct wimlib_timespec *a){
    if (c) ni->creation_time = ((ntfs_time)c->tv_sec + 11644473600ULL) * 10000000ULL
                               + (c->tv_nsec ? c->tv_nsec/100 : 0);
    if (w) ni->last_data_change_time = ((ntfs_time)w->tv_sec + 11644473600ULL) * 10000000ULL
                               + (w->tv_nsec ? w->tv_nsec/100 : 0);
    if (a) ni->last_access_time = ((ntfs_time)a->tv_sec + 11644473600ULL) * 10000000ULL
                               + (a->tv_nsec ? a->tv_nsec/100 : 0);
}

/* ---------- 主回调 ---------- */
static int on_entry(const struct wimlib_dir_entry *de, void *ctx){
    (void)ctx;
    if (!de->full_path || !*de->full_path) return 0;

    const char *full = de->full_path;
    const char *base = de->filename ? de->filename : base_name(full);
    if (strncmp(base, ".l2s.", 5) == 0) { g_st.skipped++; return 0; }
    if (g_prefix) {                       /* 前缀必须精确匹配或后跟 '/'，避免 config vs configmanager */
        size_t plen = strlen(g_prefix);
        if (strncmp(full, g_prefix, plen) != 0 || (full[plen] != 0 && full[plen] != '/')) { g_st.skipped++; return 0; }
        if (full[plen] == 0) { g_st.skipped++; return 0; }   /* 前缀根本身已由预建创建 */
    }

    char parent[560];
    snprintf(parent, sizeof parent, "%s", full);
    char *slash = strrchr(parent, '/');
    if (!slash) { g_st.skipped++; return 0; }
    *slash = 0;
    ntfs_inode *pdir = get_dir(parent);
    if (!pdir) { g_st.skipped++; return 0; }

    ntfschar nm[256];
    u8 nlen = to_ntfschar(nm, base, 255);
    if (!nlen) { put_dir(pdir); g_st.skipped++; return 0; }

    int isdir = (de->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) != 0;
    ntfs_inode *self = NULL;
    int linked = 0;

    if (de->num_links > 1 && de->hard_link_group_id) {
        int64_t ex = hl_lookup(de->hard_link_group_id);
        if (ex >= 0) {
            ntfs_inode *first = ntfs_inode_open(g_vol, (MFT_REF)ex);
            if (first) { if (ntfs_link(first, pdir, nm, nlen) == 0) { linked = 1; g_st.hl_linked++; } ntfs_inode_close(first); }
        }
    }
    if (!linked) {
        if (g_verbose) fprintf(stderr, "[CREATE] %s dir=%d\n", full, isdir);
        self = ntfs_create(pdir, 0, nm, nlen, isdir ? S_IFDIR : S_IFREG);
        if (!self) {
            if (errno == EEXIST) {                       /* 已存在（预建过）→ 幂等继续 */
                self = ntfs_pathname_to_inode(g_vol, g_root, full);
                if (!self) { put_dir(pdir); return 0; }
            } else {
                g_st.create_fail++; fprintf(stderr, "[FAIL-create] %s (%s)\n", full, strerror(errno));
                put_dir(pdir); return 0;
            }
        }
        if (isdir) g_st.dirs++; else g_st.files++;
        if (de->num_links > 1 && de->hard_link_group_id) hl_insert(de->hard_link_group_id, self->mft_no);
    } else {
        self = ntfs_pathname_to_inode(g_vol, g_root, full);
        if (!self) { put_dir(pdir); return 0; }
    }

    if (!isdir && !linked) {
        char src[1200];
        snprintf(src, sizeof src, "%s%s", g_tempdir, full);
        ntfs_attr *na = ntfs_attr_open(self, AT_DATA, NULL, 0);
        if (na) { int cr = copy_stream(src, na); if (cr) { g_st.data_fail++; fprintf(stderr, "[FAIL-data] %s (%s)\n", src, strerror(-cr)); } else g_st.bytes += (de->streams ? de->streams[0].resource.uncompressed_size : 0); ntfs_attr_pclose(na); }
        else { g_st.data_fail++; fprintf(stderr, "[FAIL-attr] %s\n", full); }
    }

    char av[4]; uint32_t at = de->attributes;
    av[0]=at&0xff; av[1]=(at>>8)&0xff; av[2]=(at>>16)&0xff; av[3]=(at>>24)&0xff;
    if (ntfs_set_ntfs_attrib(self, av, 4, 0)) g_st.attr_fail++; else g_st.attr_ok++;

    if (de->security_descriptor && de->security_descriptor_size) {
        if (ntfs_set_ntfs_acl(&g_scx, self, de->security_descriptor, de->security_descriptor_size, 0)) g_st.acl_fail++;
        else g_st.acl_ok++;
    }
    set_times(self, &de->creation_time, &de->last_write_time, &de->last_access_time);
    if (de->attributes & WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT) g_st.reparse++;

    if (isdir) {
        pm_insert(full, self);   /* 目录句柄常驻 */
        put_dir(pdir);
        return 0;
    }
    /* 关键顺序：父目录先刷盘，文件后关闭
     * （文件 close 才会把 $FILE_NAME 尺寸写入父目录索引；若父目录随后
     *   用陈旧缓存刷盘，会把尺寸覆盖回 0） */
    ntfs_inode_close(self);   /* 目录句柄常驻，只关文件 */
    put_dir(pdir);
    return 0;
}

int main(int argc, char **argv){
    if (argc < 5) { fprintf(stderr, "用法: %s <esd> <image> <tempdir> <ntfs_volume>\n", argv[0]); return 2; }
    g_esd=argv[1]; g_image=argv[2]; g_tempdir=argv[3]; g_ntfs=argv[4];

    if (wimlib_global_init(0)) { fprintf(stderr,"wimlib_global_init 失败\n"); return 1; }
    WIMStruct *wim;
    int r = wimlib_open_wim(g_esd, 0, &wim);
    if (r) { fprintf(stderr,"open_wim: %s\n", wimlib_get_error_string(r)); return 1; }
    int img = wimlib_resolve_image(wim, g_image);
    if (img < 0) { fprintf(stderr,"找不到镜像 %s\n", g_image); return 1; }

    g_vol = ntfs_mount(g_ntfs, 0);
    if (!g_vol) { fprintf(stderr,"ntfs_mount 失败: %s\n", strerror(errno)); return 1; }
    g_root = ntfs_inode_open(g_vol, FILE_root);
    if (!g_root) { fprintf(stderr,"root 打开失败\n"); return 1; }

    memset(&g_scx, 0, sizeof g_scx);
    g_scx.vol = g_vol; g_scx.uid = 0; g_scx.gid = 0; g_scx.umask = 0;
    pm_insert("", g_root);
    if (getenv("A2N_VERBOSE")) g_verbose = 1;

    if (argc > 5 && argv[5] && argv[5][0]) {
        g_prefix = argv[5];
        char chain[600]; snprintf(chain, sizeof chain, "%s", g_prefix);
        char cur[600] = "/"; char *sp = strtok(chain, "/");
        while (sp) {
            if (cur[1]) strcat(cur, "/");
            strcat(cur, sp);
            if (!pm_lookup(cur)) {
                char par[600]; strcpy(par, cur);
                char *sl = strrchr(par, '/'); if (sl) *sl = 0; else par[0] = 0;
                ntfs_inode *pd = get_dir(par);
                ntfschar n2[256]; u8 l2 = to_ntfschar(n2, sp, 255);
                ntfs_inode *d = pd ? ntfs_create(pd, 0, n2, l2, S_IFDIR) : NULL;
                if (d) { pm_insert(cur, d); g_st.dirs++; }
                else fprintf(stderr, "[FAIL-dir] %s  (pd=%s errno=%d %s)\n", cur, pd?"ok":"NULL", errno, strerror(errno));
                put_dir(pd);
            }
            sp = strtok(NULL, "/");
        }
        printf("子树前缀 = %s\n", g_prefix);
    }


    printf("开始灌入 -> %s\n", g_ntfs);
    r = wimlib_iterate_dir_tree(wim, img, "", WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE, on_entry, NULL);
    if (r) fprintf(stderr, "iterate 警告/错误: %s\n", wimlib_get_error_string(r));

    pm_close_all();          /* 常驻句柄必须先关闭，否则脏索引不刷盘 */
    ntfs_inode_close(g_root);
    ntfs_umount(g_vol, 1);
    wimlib_free(wim);
    wimlib_global_cleanup();

    printf("\n=== 完成 ===\n");
    printf("目录      %llu\n", (unsigned long long)g_st.dirs);
    printf("文件      %llu\n", (unsigned long long)g_st.files);
    printf("数据      %.2f GiB\n", (double)g_st.bytes/(1<<30));
    printf("硬链接-建 %llu   硬链接-链 %llu\n", (unsigned long long)g_st.hl_created, (unsigned long long)g_st.hl_linked);
    printf("ACL 成功  %llu  失败 %llu\n", (unsigned long long)g_st.acl_ok, (unsigned long long)g_st.acl_fail);
    printf("属性 成功 %llu  失败 %llu\n", (unsigned long long)g_st.attr_ok, (unsigned long long)g_st.attr_fail);
    printf("创建失败  %llu   数据失败 %llu   跳过 %llu\n",
           (unsigned long long)g_st.create_fail, (unsigned long long)g_st.data_fail, (unsigned long long)g_st.skipped);
    printf("reparse   %llu\n", (unsigned long long)g_st.reparse);
    return (g_st.create_fail || g_st.data_fail) ? 1 : 0;
}
