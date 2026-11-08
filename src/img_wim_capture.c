/* img_wim_capture.c — 从 qcow2 NTFS 捕获成 WIM（P4）
 *
 * 流程（见 img_wim_capture.h）：
 *   ① img_xcopy_run：NTFS 整树 → 临时目录（imgtool-tmp-wimcap-<pid>-XXXXXX）
 *   ② wimlib：create_new_wim + add_empty_image + add_tree + write
 *   ③ 删临时目录（atexit 兜底）
 *
 * 注意：
 *   · wimlib_tchar 在 Windows 上是 wchar_t、POSIX 上是 char —— 本代码只在
 *     POSIX 编译，直接用 char。
 *   · add_flags 用 WIMLIB_ADD_FLAG_...（见 wimlib.h）。
 *   · 压缩类型：XPRESS / LZX / LZMS（ESD）。
 */
#include "img_wim_capture.h"
#include "img_cmd.h"
#include "img_xcopy.h"
#include "img_partition.h"
#include "img_du.h"
#include "img_path.h"
#include "img_ntfs_fs.h"

#include <wimlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

const char *img_wim_comp_name(img_wim_comp c){
    switch(c){
    case IMG_WIM_COMP_NONE:   return "none";
    case IMG_WIM_COMP_XPRESS: return "xpress";
    case IMG_WIM_COMP_LZX:    return "lzx";
    case IMG_WIM_COMP_LZMS:   return "lzms";
    default:                  return "?";
    }
}

int img_wim_capture_estimate(const char *ipath, uint64_t lba, uint64_t part_bytes,
                             const char *src_subdir, uint64_t *out_bytes,
                             uint64_t *out_files, char *err, size_t errcap){
    if(!ipath || !out_bytes || !out_files) return IMG_EINVAL;
    *out_bytes = 0; *out_files = 0;
    (void)part_bytes;                       /* img_du 内部按 lba 自己找分区 */

    /* ★ 用 img_du_run：现成的递归统计（du 命令就是它），不用自己遍历。
     *   一开始自己写 walk_est，把 img_ntfs_list_dir_sizes 的回调式签名
     *   当成"返回数组"，传了未初始化指针 → 段错误。 */
    img_path p;
    char why[256] = "";
    char spec[2400];
    snprintf(spec, sizeof spec, "%s!lba%llu:%s", ipath,
             (unsigned long long)lba, src_subdir && *src_subdir ? src_subdir : "/");
    int rc = img_path_parse(spec, &p, why, sizeof why);
    if(rc != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"路径解析失败：%s",why);
        return rc;
    }
    img_du_stats st;
    memset(&st,0,sizeof st);
    rc = img_du_run(&p, 0, 64, false, NULL, &st, why, sizeof why);
    img_path_cleanup(&p);
    if(rc != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"统计失败：%s",why);
        return rc;
    }
    *out_bytes = st.total_bytes;
    *out_files = st.files;
    return IMG_OK;
}

int img_wim_capture(const char *img_path, uint64_t lba, uint64_t part_bytes,
                    const char *src_subdir, const char *out_wim,
                    const img_wim_capture_opts_t *opts,
                    img_wim_capture_result_t *out,
                    img_prog *prog, char *err, size_t errcap){
    if(!img_path || !out_wim || !out || !opts) return IMG_EINVAL;
    memset(out, 0, sizeof *out);

    /* ① 估算 */
    uint64_t need_bytes = 0, need_files = 0;
    char why[256] = "";
    int rc = img_wim_capture_estimate(img_path, lba, part_bytes, src_subdir,
                                      &need_bytes, &need_files, why, sizeof why);
    if(rc != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"估算失败：%s",why);
        return rc;
    }
    out->tmpdir_need = need_bytes;

    /* ② 临时目录（P1-6 统一命名）*/
    const char *td = img_tmpdir();
    if(!td||!*td){ if(err&&errcap) snprintf(err,errcap,"没有可写临时目录"); return IMG_ESTATE; }
    char tmpbuf[1200];
    if(img_tmpfile_dir("wimcap", tmpbuf, sizeof tmpbuf) != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"建临时目录失败"); return IMG_EIO;
    }
    const char *tmpdir = tmpbuf;
    /* 失败路径：由调用方在 return 前删（或 atexit 兜底收 imgtool-tmp-*）*/

    /* ③ NTFS → 临时目录（img_xcopy 现成引擎）*/
    img_xloc src; memset(&src,0,sizeof src);
    src.kind = IMG_XK_NTFS;
    snprintf(src.img,sizeof src.img,"%s",img_path);
    src.lba = lba;
    snprintf(src.path,sizeof src.path,"%s", src_subdir && *src_subdir ? src_subdir : "/");

    img_xloc dst; memset(&dst,0,sizeof dst);
    dst.kind = IMG_XK_HOST;
    snprintf(dst.path,sizeof dst.path,"%s",tmpdir);

    img_xcopy_opts xo = { IMG_COPY_OVERWRITE, true, true, true, false, 64, false, {{0}}, 0 };
    img_xcopy_stats st; memset(&st,0,sizeof st);
    rc = img_xcopy_run(&src,&dst,&xo,prog,&st);
    if(rc != IMG_OK){
        char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
        (void)system(cmd);
        if(err&&errcap) snprintf(err,errcap,"抽取失败（%llu 文件后）：",
            (unsigned long long)st.files);
        return rc;
    }
    out->files = st.files; out->dirs = st.dirs; out->bytes_in = st.bytes;

    /* ④ wimlib capture */
    if(wimlib_global_init(0) != 0){
        char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
        (void)system(cmd);
        if(err&&errcap) snprintf(err,errcap,"wimlib_global_init 失败");
        return IMG_EIO;
    }
    WIMStruct *w = NULL;
    enum wimlib_compression_type ct;
    switch(opts->comp){
    case IMG_WIM_COMP_NONE:   ct = WIMLIB_COMPRESSION_TYPE_NONE;   break;
    case IMG_WIM_COMP_LZX:    ct = WIMLIB_COMPRESSION_TYPE_LZX;    break;
    case IMG_WIM_COMP_LZMS:   ct = WIMLIB_COMPRESSION_TYPE_LZMS;   break;
    default:                  ct = WIMLIB_COMPRESSION_TYPE_XPRESS; break;
    }
    if(wimlib_create_new_wim(ct, &w) != 0){
        char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
        (void)system(cmd);
        if(err&&errcap) snprintf(err,errcap,"wimlib_create_new_wim 失败");
        return IMG_EIO;
    }
    int new_idx = -1;
    if(wimlib_add_empty_image(w, opts->image_name ? opts->image_name : "Windows",
                              &new_idx) != 0){
        wimlib_free(w);
        char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
        (void)system(cmd);
        if(err&&errcap) snprintf(err,errcap,"wimlib_add_empty_image 失败");
        return IMG_EIO;
    }
    int add_flags = 0;
    if(!opts->capture_acl) add_flags |= WIMLIB_ADD_FLAG_NO_ACLS;   /* 默认带 ACL */

    if(wimlib_add_tree(w, new_idx, tmpdir, (wimlib_tchar*)"/", add_flags) != 0){
        wimlib_free(w);
        char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
        (void)system(cmd);
        if(err&&errcap) snprintf(err,errcap,"wimlib_add_tree 失败");
        return IMG_EIO;
    }
    int write_flags = 0;
    int wr = wimlib_write(w, (wimlib_tchar*)out_wim, new_idx, write_flags, 0);
    wimlib_free(w);
    if(wr != 0){
        unlink(out_wim);
        char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
        (void)system(cmd);
        if(err&&errcap) snprintf(err,errcap,"wimlib_write 失败（wimlib 错误码 %d）",wr);
        return IMG_EIO;
    }

    /* ⑤ 收尾 */
    struct stat sb;
    if(stat(out_wim,&sb)==0) out->bytes_out = (uint64_t)sb.st_size;
    char cmd[1400]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tmpdir);
    (void)system(cmd);
    return IMG_OK;
}
