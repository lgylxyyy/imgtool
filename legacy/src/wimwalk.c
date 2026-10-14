/* 遍历 WIM/ESD，收集元数据统计：证明 libwim C API 能拿到 ACL/硬链接/属性 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wimlib.h>

static const char *g_dbg = NULL;
struct stats {
    uint64_t dirs, files, bytes, named_streams;
    uint64_t hardlinked;      /* num_links > 1 */
    uint64_t reparse;
    uint64_t with_acl, no_acl;
    uint32_t max_links;
    uint64_t sys_hidden_ro;   /* SYSTEM|HIDDEN|READONLY */
    uint64_t by_attr[16];     /* 属性位分布 */
    char acl_sample_path[400];
    size_t acl_sample_size;
    unsigned acl_sample_ver;
    int samples;
};

static int cb(const struct wimlib_dir_entry *de, void *ctx) {
    struct stats *s = ctx;
    uint32_t a = de->attributes;
    int isdir = (a & WIMLIB_FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (isdir) s->dirs++;
    else { s->files++; if (de->streams) s->bytes += de->streams[0].resource.uncompressed_size; }
    if (de->num_links > 1) { s->hardlinked++; if (de->num_links > s->max_links) s->max_links = de->num_links; }
    if (a & WIMLIB_FILE_ATTRIBUTE_REPARSE_POINT) s->reparse++;
    if (a & (WIMLIB_FILE_ATTRIBUTE_SYSTEM|WIMLIB_FILE_ATTRIBUTE_HIDDEN|WIMLIB_FILE_ATTRIBUTE_READONLY)) s->sys_hidden_ro++;
    for (int b = 0; b < 16; b++) if (a & (1u << b)) s->by_attr[b]++;
    s->named_streams += de->num_named_streams;
    if (de->security_descriptor && de->security_descriptor_size) {
        s->with_acl++;
        if (!s->acl_sample_path[0] && de->full_path) {
            snprintf(s->acl_sample_path, sizeof s->acl_sample_path, "%s", de->full_path);
            s->acl_sample_size = de->security_descriptor_size;
            s->acl_sample_ver = de->security_descriptor[0];
        }
    } else s->no_acl++;
    s->samples++;
    if (g_dbg && de->full_path && strstr(de->full_path, g_dbg)) {
        printf("  PATH=%s\n     file=%s  links=%u group=%llu size=%llu attrs=0x%x\n",
               de->full_path, de->filename ? de->filename : "(null)",
               de->num_links, (unsigned long long)de->hard_link_group_id,
               (unsigned long long)(de->streams ? de->streams[0].resource.uncompressed_size : 0),
               de->attributes);
    }
    return 0;
}



int main(int argc, char **argv) {
    const char *wim_path = "/workspace/_build/SOURCES/INSTALL.ESD";
    const char *imgname = "1";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i+1 < argc) { g_dbg = argv[++i]; continue; }
        if (!strcmp(argv[i], "-i") && i+1 < argc) { imgname = argv[++i]; continue; }
        wim_path = argv[i];
    }
    if (wimlib_global_init(0)) { fprintf(stderr, "global_init 失败\n"); return 1; }
    WIMStruct *wim;
    int r = wimlib_open_wim(wim_path, 0, &wim);
    if (r) { fprintf(stderr, "open_wim: %s\n", wimlib_get_error_string(r)); return 1; }
    int img = wimlib_resolve_image(wim, imgname);
    if (img < 0) { fprintf(stderr, "找不到镜像 %s\n", imgname); return 1; }
    printf("镜像索引 = %d\n", img);

    struct stats s; memset(&s, 0, sizeof s);
    r = wimlib_iterate_dir_tree(wim, img, "", WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE, cb, &s);
    if (r) { fprintf(stderr, "iterate: %s\n", wimlib_get_error_string(r)); return 1; }

    printf("\n=== 遍历完成 ===\n");
    printf("条目总数      %llu\n", (unsigned long long)s.samples);
    printf("目录(正确)    %llu\n", (unsigned long long)s.dirs);
    printf("文件          %llu\n", (unsigned long long)s.files);
    printf("数据总量      %.2f GiB\n", (double)s.bytes / (1<<30));
    printf("硬链接文件    %llu  (最大链接数 %u)\n", (unsigned long long)s.hardlinked, s.max_links);
    printf("reparse point  %llu\n", (unsigned long long)s.reparse);
    printf("具 ACL         %llu\n", (unsigned long long)s.with_acl);
    printf("无 ACL         %llu\n", (unsigned long long)s.no_acl);
    printf("系统/隐藏/只读 %llu\n", (unsigned long long)s.sys_hidden_ro);
    printf("命名数据流     %llu\n", (unsigned long long)s.named_streams);
    if (s.acl_sample_path[0])
        printf("ACL 样例       %s (%u 字节, revision=%u)\n", s.acl_sample_path, (unsigned)s.acl_sample_size, s.acl_sample_ver);
    static const char *an[] = {"READONLY","HIDDEN","SYSTEM","NONE","ARCHIVE","DEVICE","NORMAL","TEMP","SPARSE","REPARSE","COMPRESSED","OFFLINE","NOIDX","ENCRYPTED"};
    printf("属性分布: ");
    for (int i = 0; i < 14; i++) if (s.by_attr[i]) printf("%s=%llu ", an[i], (unsigned long long)s.by_attr[i]);
    printf("\n");
    wimlib_free(wim);
    wimlib_global_cleanup();
    return 0;
}
