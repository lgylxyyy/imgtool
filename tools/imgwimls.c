/* imgwimls — 浏览 WIM/ESD 内部（镜像清单 + 目录树）
 *  imgwimls <wim|esd>                 列镜像清单
 *  imgwimls <wim|esd> <index> [路径]   列镜像里的目录（默认根 "/"，递归）
 *  ISO 里的 WIM 先用 7z 抽出再来
 *
 * 用 wimlib_iterate_dir_tree 回调遍历（extract_xml_data 只给统计信息，
 * 完整文件列表必须走这个 API）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wimlib.h>
#include <sys/stat.h>
/* 简易：ISO 内容用 7z 列（ISO 是 ISO9660/UDF，本层不自己解析，
 * 交给成熟的 7z —— App 侧可换成内置解析器）*/
static int cmd_out(const char *cmd){
    FILE *p = popen(cmd, "r");
    if(!p){ printf("  ❌ 命令失败: %s\n", cmd); return -1; }
    char line[1024];
    while(fgets(line,sizeof line,p)) printf("  %s", line);
    pclose(p);
    return 0;
}

static int g_count=0, g_limit=400;
static const char *g_filter=NULL;      /* 只显示名字含此子串的 */
static bool g_recursive=false;
static size_t g_base_depth=1;   /* 目标路径的深度；直接子项 = depth == base_depth+1 */

static int on_entry(const struct wimlib_dir_entry *de, void *ctx){
    (void)ctx;
    if(!de || !de->full_path) return 0;
    if(getenv("WIMLS_DEBUG") && g_count<5)
        fprintf(stderr,"  [dbg] depth=%zu path=%s attr=0x%x\n", de->depth, de->full_path, de->attributes);
    if(de->depth == 0) return 0;                    /* 跳过根自身 */
    bool isdir = (de->attributes & WIMLIB_FILE_ATTRIBUTE_DIRECTORY)!=0;
    /* ★ depth 是相对【镜像根】的绝对深度，不是相对查询路径的。
     *   查 /Windows/System32 时子项 depth=3 —— 按 1 判断会全部滤掉。*/
    if(!g_recursive && de->depth != g_base_depth + 1) return 0;
    if(g_filter && !strstr(de->full_path, g_filter)) return 0;
    if(g_count >= g_limit){ g_count++; return 0; }
    const char *nm = de->filename ? de->filename : de->full_path;
    printf("      %-*s %s\n", isdir?32:12, isdir?"[目录]":"[文件]", nm);
    g_count++;
    return 0;
}

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"用法: %s <wim|esd> [index] [路径] [--all] [--filter 子串] [--limit N]\n",argv[0]); return 2; }
    int idx = (argc>2)?atoi(argv[2]):0;
    const char *path = (argc>3 && argv[3][0]!='-')?argv[3]:"";  /* wimlib 根路径是空串 */
    for(int i=2;i<argc;i++){
        if(!strcmp(argv[i],"--all")) g_recursive=true;
        else if(!strcmp(argv[i],"--filter") && i+1<argc) g_filter=argv[++i];
        else if(!strcmp(argv[i],"--limit") && i+1<argc) g_limit=atoi(argv[++i]);
    }
    /* ISO：列出内部的 wim/esd 与 boot 素材 */
    { size_t n=strlen(argv[1]); 
      if(n>4 && (!strcasecmp(argv[1]+n-4,".iso") || !strcasecmp(argv[1]+n-4,".img"))){
          printf("  ── ISO 内容（7z 列举）──\n");
          char cmd[1024];
          snprintf(cmd,sizeof cmd, "7z l \"%s\" 2>/dev/null | grep -iE '\\.(wim|esd|efi|sdi)$' | head -40", argv[1]);
          if(cmd_out(cmd)!=0) return 1;
          return 0;
      } }
    if(wimlib_global_init(0)!=0){ printf("❌ wimlib init\n"); return 1; }
    WIMStruct *w=NULL;
    int r = wimlib_open_wim(argv[1], 0, &w);
    if(r){ printf("❌ 打不开: %s\n", wimlib_get_error_string(r)); return 1; }
    struct wimlib_wim_info wi;
    wimlib_get_wim_info(w,&wi);
    printf("  镜像数 %u · 压缩 %s\n", wi.image_count,
           wimlib_get_compression_type_string(wi.compression_type));
    for(unsigned i=1;i<=wi.image_count;i++){
        const char *nm=wimlib_get_image_name(w,(int)i);
        printf("    [%u] %s\n", i, nm?nm:"(无名)");
    }
    if(idx>0){
        { size_t d=0; for(const char *p=path; *p; p++) if(*p=='/') d++;
          g_base_depth = d; }
        printf("  ── 路径 %s 的条目%s（基线深度 %zu）──\n", path, g_recursive?"（递归）":"", g_base_depth);
        g_count=0;
        /* ★ 必须传 CHILDREN/RECURSIVE 标志 —— 不传只回调【路径节点自身】，
         *   子项一个都不会给（我踩过：不传 → 永远 0 项）。*/
        int fl = g_recursive ? WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE
                             : WIMLIB_ITERATE_DIR_TREE_FLAG_CHILDREN;
        r = wimlib_iterate_dir_tree(w, idx, path, fl, on_entry, NULL);
        if(r) printf("  ❌ 遍历失败: %s\n", wimlib_get_error_string(r));
        else if(g_count>g_limit) printf("      …还有 %d 项未显示（用 --limit 调整）\n", g_count-g_limit);
        else printf("      （共 %d 项）\n", g_count);
    }
    wimlib_free(w);
    return 0;
}
