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
 *
 * ★ 2026-11-10（REQ-67）两处改动：
 *   ① 三段进度：extract（抽取）/ pack（扫描建索引）/ write（压缩落盘）。
 *      以前后两段是黑的，128 秒里用户只能看 logcat。
 *   ② --exclude：按 basename 通配，estimate 与实捕走【同一个】名单。
 */
#include "img_wim_capture.h"
#include "img_cmd.h"
#include "img_xcopy.h"
#include "img_partition.h"
#include "img_du.h"
#include "img_path.h"
#include "img_ntfs_fs.h"
#include "img_types.h"
#include "img_event.h"

#include <wimlib.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* 内置默认可省文件（REQ-67 Q3）：
 *   pagefile.sys / hiberfil.sys / swapfile.sys 开机由 Windows 自己重建，
 *   写进 WIM 只是白读 3.85 GB、白压一遍。名单写死在这里，CLI/App 都能用
 *   --exclude-default=false 关掉；关掉时要在结果里说清楚（见命令层）。 */
static const char *const IMG_WIM_DEF_EXCL[] = {
    "pagefile.sys", "hiberfil.sys", "swapfile.sys"
};

const char *const *img_wim_default_exclude(int *n){
    if(n) *n = (int)(sizeof IMG_WIM_DEF_EXCL / sizeof IMG_WIM_DEF_EXCL[0]);
    return IMG_WIM_DEF_EXCL;
}

/* 生效名单 = 默认名单（可选）+ 用户名单。返回条数。 */
static int build_patterns(const img_wim_capture_opts_t *o, const char *out[], int cap){
    int np = 0;
    if(o && o->use_default_exclude){
        int dn = 0;
        const char *const *d = img_wim_default_exclude(&dn);
        for(int i = 0; i < dn && np < cap; i++) out[np++] = d[i];
    }
    if(o){
        for(int i = 0; i < o->n_exclude && np < cap; i++)
            if(o->exclude[i][0]) out[np++] = o->exclude[i];
    }
    return np;
}

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
                             const char *src_subdir,
                             const img_wim_capture_opts_t *opts,
                             uint64_t *out_bytes, uint64_t *out_files,
                             uint64_t *out_excluded,
                             char *err, size_t errcap){
    if(!ipath || !out_bytes || !out_files) return IMG_EINVAL;
    *out_bytes = 0; *out_files = 0;
    if(out_excluded) *out_excluded = 0;
    (void)part_bytes;                       /* img_du 内部按 lba 自己找分区 */

    /* ★ 用 img_du_run_ex：现成的递归统计（du 命令就是它），不用自己遍历。
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
    const char *pat[IMG_WIM_EXCL_MAX + 8];
    img_du_opts dopt;
    dopt.exclude = pat;
    dopt.n_exclude = build_patterns(opts, pat,
                                    (int)(sizeof pat / sizeof pat[0]));
    img_du_stats st;
    memset(&st,0,sizeof st);
    rc = img_du_run_ex(&p, 0, 64, false, &dopt, NULL, &st, why, sizeof why);
    img_path_cleanup(&p);
    if(rc != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"统计失败：%s",why);
        return rc;
    }
    *out_bytes = st.total_bytes;
    *out_files = st.files;
    if(out_excluded) *out_excluded = st.excluded;
    return IMG_OK;
}

/* ---------- 打包/写出阶段的进度（REQ-67 §2.1） ---------- */
typedef struct {
    img_prog *p;
    uint64_t  files_total;   /* 抽取阶段的实际文件数：PACK 阶段的分母 */
    uint64_t  bytes_total;   /* 抽取阶段的实际字节数 */
} cap_cb;

static enum wimlib_progress_status cap_on_progress(enum wimlib_progress_msg msg,
                                                   union wimlib_progress_info *info,
                                                   void *ctx){
    /* 喂 I/O 看门狗：压缩整块数据时可能几十秒不写盘，进度回调就是"还活着"。 */
    img_io_touch();
    cap_cb *c = (cap_cb*)ctx;
    /* 取消：wimlib 只认这种返回码（应用层没法自己中断 add_tree/write）。
     * ★ REQ-68 §3.3：两个标志都查 —— App 的取消走的是全局 img_cancel()。
     *   这段必须在"没有进度器"时也生效（--progress=none / prog==NULL）。*/
    if(img_cancelled() || (c && c->p && img_prog_cancelled(c->p)))
        return WIMLIB_PROGRESS_STATUS_ABORT;
    if(!c || !c->p) return WIMLIB_PROGRESS_STATUS_CONTINUE;

    switch(msg){
    case WIMLIB_PROGRESS_MSG_SCAN_BEGIN:
        img_prog_phase(c->p, IMG_PH_PACK, "建立 WIM 索引");
        img_prog_op(c->p, IMG_OP_SCAN);
        img_prog_count(c->p, 0, c->files_total);
        break;
    case WIMLIB_PROGRESS_MSG_SCAN_DENTRY:
        /* num_nondirs/num_bytes 是"扫到现在累计"，正好当 cur（单调）。
         * ★ REQ-68 §3.1：pack 阶段也要 item —— 用户看到路径在滚才知道没卡死。
         *   wimlib 给的是 WIM 内路径（/Windows/...），正是 App 要的样子。*/
        if(info->scan.cur_path && info->scan.cur_path[0])
            img_prog_item(c->p, (const char*)info->scan.cur_path);
        img_prog_count(c->p, info->scan.num_nondirs_scanned, c->files_total);
        img_prog_bytes(c->p, info->scan.num_bytes_scanned, c->bytes_total);
        break;
    case WIMLIB_PROGRESS_MSG_SCAN_END:
        img_prog_count(c->p, info->scan.num_nondirs_scanned, c->files_total);
        break;
    case WIMLIB_PROGRESS_MSG_WRITE_STREAMS:
        img_prog_phase(c->p, IMG_PH_WRITE, "压缩写出 WIM");
        img_prog_op(c->p, IMG_OP_COMPRESS);
        {
            /* ★ REQ-69 ①：这里【不能冻结】分母。wimlib 的 total_bytes 是
             *   "未压缩上界"，发现重复数据后会往下调；冻结第一次的值会让
             *   cur/total 永远到不了 100%（实测 8.43 GB 的分母、4.57 GB 收尾 = 54%）。
             *   直接用它给的值，pct 由引擎夹成单调不减即可。*/
            uint64_t tot = info->write_streams.total_bytes;
            uint64_t cur = info->write_streams.completed_bytes;
            if(tot && cur > tot) cur = tot;
            img_prog_bytes(c->p, cur, tot);
        }
        img_prog_count(c->p, info->write_streams.completed_streams,
                             info->write_streams.total_streams);
        break;
    case WIMLIB_PROGRESS_MSG_WRITE_METADATA_BEGIN:
        img_prog_phase(c->p, IMG_PH_WRITE, "写入元数据");
        img_prog_op(c->p, IMG_OP_WRITE_INDEX);
        break;
    default:
        break;
    }
    return WIMLIB_PROGRESS_STATUS_CONTINUE;
}

int img_wim_capture(const char *img_path, uint64_t lba, uint64_t part_bytes,
                    const char *src_subdir, const char *out_wim,
                    const img_wim_capture_opts_t *opts,
                    img_wim_capture_result_t *out,
                    img_prog *prog, char *err, size_t errcap){
    if(!img_path || !out_wim || !out || !opts) return IMG_EINVAL;
    memset(out, 0, sizeof *out);

    /* ① 估算（与实捕同一套排除规则） */
    uint64_t need_bytes = 0, need_files = 0, need_excluded = 0;
    char why[256] = "";
    int rc = img_wim_capture_estimate(img_path, lba, part_bytes, src_subdir, opts,
                                      &need_bytes, &need_files, &need_excluded,
                                      why, sizeof why);
    if(rc != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"估算失败：%s",why);
        return rc;
    }
    out->tmpdir_need = need_bytes;

    /* ★ REQ-69 ④：先把上次"取消后待清"的 -garbage 收走，免得占着空间不让这次跑 */
    (void)img_tmpdir_sweep();

    /* ② 临时目录（P1-6 统一命名）*/
    const char *td = img_tmpdir();
    if(!td||!*td){ if(err&&errcap) snprintf(err,errcap,"没有可写临时目录"); return IMG_ESTATE; }
    char tmpbuf[1200];
    if(img_tmpfile_dir("wimcap", tmpbuf, sizeof tmpbuf) != IMG_OK){
        if(err&&errcap) snprintf(err,errcap,"建临时目录失败"); return IMG_EIO;
    }
    const char *tmpdir = tmpbuf;
    /* 失败路径：由调用方在 return 前删（或 atexit 兜底收 imgtool-tmp-*）*/

    /* ③ NTFS → 临时目录（img_xcopy 现成引擎） */
    img_xloc src; memset(&src,0,sizeof src);
    src.kind = IMG_XK_NTFS;
    snprintf(src.img,sizeof src.img,"%s",img_path);
    src.lba = lba;
    snprintf(src.path,sizeof src.path,"%s", src_subdir && *src_subdir ? src_subdir : "/");

    img_xloc dst; memset(&dst,0,sizeof dst);
    dst.kind = IMG_XK_HOST;
    snprintf(dst.path,sizeof dst.path,"%s",tmpdir);

    img_xcopy_opts xo = { IMG_COPY_OVERWRITE, true, true, true, false, 64, false, {{0}}, 0 };
    /* ★ REQ-67：把【同一份】名单灌进 xcopy，不然 estimate 与实捕又会分叉。 */
    {
        const char *pat[IMG_WIM_EXCL_MAX + 8];
        int np = build_patterns(opts, pat, (int)(sizeof pat / sizeof pat[0]));
        for(int i = 0; i < np && i < IMG_XCOPY_EXCL_MAX; i++){
            snprintf(xo.exclude[xo.n_exclude], sizeof xo.exclude[0], "%s", pat[i]);
            xo.n_exclude++;
        }
    }
    /* ★ REQ-68 §2：totalBytes 全程不变，App 的 ETA 才稳 */
    xo.agg_bytes_total = need_bytes;
    xo.agg_files_total = need_files;
    /* ★ REQ-69 ⑥：先把段定成 extract —— 否则 xcopy 会按"独立 copy 命令"
     *   先发一条 phase=copy（App 没这个文案，界面上会出现两帧不明阶段）。*/
    if(prog) img_prog_phase(prog, IMG_PH_EXTRACT, "抽取文件");
    img_xcopy_stats st; memset(&st,0,sizeof st);
    rc = img_xcopy_run(&src,&dst,&xo,prog,&st);
    if(rc != IMG_OK){
        /* ★ REQ-69 ④：取消 ≠ 失败。取消时把整棵树改个名字（O(1)）交给下次 sweep；
         *   失败时（数据可能有用）照旧同步删。*/
        if(rc == IMG_ECANCELLED) (void)img_tmpdir_discard(tmpdir);
        else                     (void)img_tmpdir_remove(tmpdir);
        if(err&&errcap){
            if(rc == IMG_ECANCELLED)
                snprintf(err,errcap,"已取消（已抽取 %llu 个文件）",(unsigned long long)st.files);
            else
            /* ★ 2026-11-10（REQ-65 §5）：以前末尾只有一个冒号，用户与 App 都看不到原因。
             *   first_error 现在由 copy_dir 填（出错那条源路径），这里如实带出去。*/
                snprintf(err,errcap,"抽取失败（%llu 文件后）：%s（rc=%d side=%d）",
                    (unsigned long long)st.files,
                    st.first_error[0]?st.first_error:"未记录原因", rc, st.side);
        }
        return rc;
    }
    out->files = st.files; out->dirs = st.dirs; out->bytes_in = st.bytes;
    out->skipped = st.skipped;
    out->excluded = st.skipped_excluded;
    /* ★ REQ-68 §3.2：extract 收尾必须是 pct=100，圆环才不会停在中途 */
    if(prog) img_prog_seal(prog, "抽取完成");

    /* ④ wimlib capture */
    if(wimlib_global_init(0) != 0){
        (void)img_tmpdir_remove(tmpdir);
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
        (void)img_tmpdir_remove(tmpdir);
        if(err&&errcap) snprintf(err,errcap,"wimlib_create_new_wim 失败");
        return IMG_EIO;
    }
    int new_idx = -1;
    if(wimlib_add_empty_image(w, opts->image_name ? opts->image_name : "Windows",
                              &new_idx) != 0){
        wimlib_free(w);
        (void)img_tmpdir_remove(tmpdir);
        if(err&&errcap) snprintf(err,errcap,"wimlib_add_empty_image 失败");
        return IMG_EIO;
    }
    /* ★ REQ-69 ②：WIMLIB_PROGRESS_MSG_SCAN_DENTRY【只在 VERBOSE 下发送】
     *   （wimlib.h 原文：This message is only sent if WIMLIB_ADD_FLAG_VERBOSE
     *   has been specified）。不加这个标志，pack 段就只有 BEGIN/END 两条、
     *   item 永远是空 —— 用户看到圆盘下方全空、环还停在 100%。*/
    int add_flags = WIMLIB_ADD_FLAG_VERBOSE;
    if(!opts->capture_acl) add_flags |= WIMLIB_ADD_FLAG_NO_ACLS;   /* 默认带 ACL */

    /* ★ REQ-67：add_tree 与 write 都通过这个注册的回调上报进度 */
    cap_cb cc;
    cc.p = prog; cc.files_total = st.files; cc.bytes_total = st.bytes;
    /* 无条件注册：即使没有进度器，也要靠这个回调把取消送进 add_tree/write */
    wimlib_register_progress_function(w, cap_on_progress, &cc);

    if(wimlib_add_tree(w, new_idx, tmpdir, (wimlib_tchar*)"/", add_flags) != 0){
        int canc = img_cancelled();
        wimlib_free(w);
        (void)img_tmpdir_remove(tmpdir);
        if(err&&errcap) snprintf(err,errcap, canc ? "已取消" : "wimlib_add_tree 失败");
        return canc ? IMG_ECANCELLED : IMG_EIO;
    }
    if(prog) img_prog_seal(prog, "打包完成");     /* REQ-68 §3.2：pack 收尾 */
    int write_flags = 0;
    int wr = wimlib_write(w, (wimlib_tchar*)out_wim, new_idx, write_flags, 0);
    wimlib_free(w);
    if(wr != 0){
        int canc = img_cancelled();
        unlink(out_wim);
        (void)img_tmpdir_remove(tmpdir);
        if(err&&errcap) snprintf(err,errcap, canc ? "已取消" : "wimlib_write 失败（wimlib 错误码 %d）", wr);
        return canc ? IMG_ECANCELLED : IMG_EIO;
    }

    /* ⑤ 收尾（REQ-69 ①：write 段末必须有 pct=100，否则圆环停在半路）*/
    if(prog) img_prog_done(prog, "写入完成");
    struct stat sb;
    if(stat(out_wim,&sb)==0) out->bytes_out = (uint64_t)sb.st_size;
    (void)img_tmpdir_remove(tmpdir);
    return IMG_OK;
}