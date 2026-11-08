#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_wim_apply.c — Stage 5：WIM → qcow2 分区（原理见头文件）
 *
 * 关键接线：wimlib 用 WIMLIB_EXTRACT_FLAG_NTFS 走 libntfs-3g 直写模式，
 * 其 ntfs_mount() 经宏 ntfs_device_default_io_ops → ntfs_device_unix_io_ops
 * → 我们在 img_ntfs_fmt.c 定义的同名符号 → 10 个回调 → qcow2 分区。 */
#include "img_wim_apply.h"
#include "img_event.h"
#include "img_partition.h"
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wimlib.h>
#include "img_wimlib_codes.h"

/* wimlib 进度 → img_prog */
struct cb_ctx { img_prog *p; };
static enum wimlib_progress_status on_progress(enum wimlib_progress_msg msg,
                                               union wimlib_progress_info *info, void *ctx){
    /* ★ 第一件事：喂看门狗。
     * 澄清（2026-10-20）：灌入是【随读随写】—— WIMLIB_EXTRACT_FLAG_NTFS 让 wimlib
     * 经 libntfs-3g 直写进分区，**不落任何临时文件、不整盘先解压**，ACL/属性默认保留。
     * 空窗的真正原因是【解压】：这张 ESD 是 LZMS 压缩（1.87 GB → 6.86 GB，3.7×），
     * wimlib 解某个固实 resource 的那段时间是纯 CPU 活，**期间不产生任何一次 qcow2 写**
     * ——实测连续 260 秒。而 I/O 看门狗按"完成过一次读写"计时，于是把"在解压"
     * 误判成"无响应"，发出 IO-TIMEOUT(gap=260886ms) 并取消（rc=-13 CMD-CANCELLED），
     * 6.86 GB 的盘根本灌不完。
     * 进度回调本身就是"还活着"的证据：这里 touch 之后，看门狗才真正等价于
     * "完全没有任何进展"（而不是"这一会儿未写入盘"）。*/
    img_io_touch();
    struct cb_ctx *c = ctx;
    if(!c || !c->p) return 0;
    switch(msg){
    case WIMLIB_PROGRESS_MSG_EXTRACT_FILE_STRUCTURE:
    case WIMLIB_PROGRESS_MSG_EXTRACT_METADATA:
        img_prog_phase(c->p, IMG_PH_EXTRACT, "建立文件结构");
        img_prog_op(c->p, IMG_OP_WRITE_INDEX);
        break;
    case WIMLIB_PROGRESS_MSG_EXTRACT_STREAMS:
        img_prog_phase(c->p, IMG_PH_EXTRACT, "解压文件数据");
        img_prog_op(c->p, IMG_OP_DECOMPRESS);
        img_prog_bytes(c->p, info->extract.completed_bytes, info->extract.total_bytes);
        /* REQ-49 §3：这就是"解压+写入"的总工作量 → 直接当全局进度（单调） */
        img_prog_overall(c->p, info->extract.completed_bytes, info->extract.total_bytes);
        img_prog_count(c->p, info->extract.completed_streams, info->extract.total_streams);
        break;
    case WIMLIB_PROGRESS_MSG_EXTRACT_IMAGE_END:
        if(info->extract.total_bytes){
            img_prog_bytes(c->p, info->extract.total_bytes, info->extract.total_bytes);
            img_prog_overall(c->p, info->extract.total_bytes, info->extract.total_bytes);
            img_prog_count(c->p, info->extract.total_streams, info->extract.total_streams);
        }
        break;
    default: break;
    }
    return 0;
}

void img_wim_apply_defaults(img_wim_apply_opts_t *o){
    if(o) memset(o, 0, sizeof *o);
}

int img_wim_list_images(const char *wim_path, char *buf, int cap){
    if(!wim_path||!buf||cap<8) return IMG_EINVAL;
    buf[0]=0;
    if(wimlib_global_init(0)!=0) return IMG_EIO;
    WIMStruct *w=NULL;
    int r = wimlib_open_wim(wim_path, 0, &w);
    if(r) return IMG_ENOENT;
    struct wimlib_wim_info wi;
    wimlib_get_wim_info(w, &wi);
    int k = snprintf(buf,(size_t)cap,
        "{\"imageCount\":%u,\"compression\":\"%s\",\"images\":[",
        wi.image_count, wimlib_get_compression_type_string(wi.compression_type));
    for(unsigned i=1;i<=wi.image_count && k<cap-4;i++){
        const char *nm = wimlib_get_image_name(w,(int)i);
        const char *ds = wimlib_get_image_description(w,(int)i);
        k += snprintf(buf+k,(size_t)cap-k,
            "%s{\"index\":%u,\"name\":\"%s\",\"desc\":\"%s\"}",
            (i>1)?",":"", i, nm?nm:"", ds?ds:"");
    }
    snprintf(buf+k,(size_t)cap-k,"]}");
    wimlib_free(w);
    return IMG_OK;
}

/* ★ wimlib 1.14 的头文件声明了 wimlib_get_error_string()，但发行版的 libwim.so
 *   并没有导出这个符号（链接期 undefined reference）。我们只需要一句说明 + 错误码，
 *   自己给一条，避免依赖一个"声明存在但符号缺失"的 API。*/
static const img_wimlib_code_t *wimlib_code_lookup(int code){
    for(int i=0;i<IMG_WIMLIB_CODES_N;i++) if(IMG_WIMLIB_CODES[i].code==code) return &IMG_WIMLIB_CODES[i];
    return NULL;
}
const char *img_wimlib_err_name(int code){
    const img_wimlib_code_t *e=wimlib_code_lookup(code);
    return e? e->name : NULL;
}
const char *img_wimlib_err_zh(int code){
    const img_wimlib_code_t *e=wimlib_code_lookup(code);
    return (e && e->zh && e->zh[0])? e->zh : NULL;
}

/* 估算：数据量 + 元数据开销 + 余量 → 建议的目标分区最小可用量 */
#define IMG_NTFS_META_PER_FILE  2048u     /* 每文件 MFT 记录 + 目录项 + 索引，经验值 */
#define IMG_NTFS_SLACK_PCT      8         /* 安全余量（宁可高估，别写到一半不够）*/
int img_wim_estimate(const char *wim_path, int image_idx, img_wim_size_t *out){
    if(!wim_path||!out||image_idx<1) return IMG_EINVAL;
    memset(out,0,sizeof *out);
    if(wimlib_global_init(0)!=0) return IMG_EIO;
    WIMStruct *w=NULL;
    if(wimlib_global_init(0)!=0) return IMG_EIO;
    if(wimlib_open_wim(wim_path,0,&w)!=0) return IMG_ENOENT;
    struct wimlib_wim_info wi; wimlib_get_wim_info(w,&wi);
    if((unsigned)image_idx > wi.image_count){ wimlib_free(w); return IMG_ERANGE; }
    out->index=image_idx;
    const char *nm=wimlib_get_image_name(w,image_idx);
    snprintf(out->name,sizeof out->name,"%s",nm?nm:"");
    snprintf(out->compression,sizeof out->compression,"%s",
             wimlib_get_compression_type_string(wi.compression_type));
    /* 属性名与值都是字符串（WIM XML 原样）*/
    const char *p;
    p=wimlib_get_image_property(w,image_idx,"TOTALBYTES");
    if(p) out->data_bytes=strtoull(p,NULL,10);
    p=wimlib_get_image_property(w,image_idx,"FILECOUNT");
    if(p) out->file_count=strtoull(p,NULL,10);
    p=wimlib_get_image_property(w,image_idx,"DIRCOUNT");
    if(p) out->dir_count=strtoull(p,NULL,10);
    wimlib_free(w);
    uint64_t meta = out->file_count * IMG_NTFS_META_PER_FILE;
    uint64_t sum  = out->data_bytes + meta;
    out->est_ntfs_bytes = sum + sum*IMG_NTFS_SLACK_PCT/100;
    return IMG_OK;
}

static int g_last_wrc = 0;
/* 上一次 img_wim_apply 里 wimlib 的原始返回码（0=成功）。诊断用，见命令层 WIM-APPLY-FAIL 的 hint。*/
int img_wim_last_rc(void){ return g_last_wrc; }

int img_wim_apply(const char *wim_path, int image_idx,
                  img_qcow2 *q, uint64_t part_lba,
                  const img_wim_apply_opts_t *opts, img_prog *prog){
    if(!wim_path||!q||image_idx<1) return IMG_EINVAL;
    img_parttable_t t;
    int rc = img_parttable_read(q,&t,0);
    if(rc) return rc;
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==part_lba){
            pb = (t.parts[i].last_lba - t.parts[i].first_lba + 1) * 512;
            break;
        }
    if(!pb) return IMG_EINVAL;

    /* wimlib 先 stat/mkdir 探测目标类型 —— 必须是【已存在的普通文件】才进 NTFS 卷模式
     * （不存在 → 它 mkdir 当目录；是目录 → 目录模式）。
     * 这个占位文件本身【从不被真正读写】，I/O 全在我们的回调里。 */
    const char *dummy = img_dev_placeholder();
    if(!dummy) return IMG_ENOTSUP;   /* 没有可写临时目录：调用方出 TMPDIR-UNAVAILABLE */

    img_ntfs_ctx_activate(q, part_lba, pb);     /* 让回调指向这个分区 */

    if(wimlib_global_init(0)!=0){ img_ntfs_ctx_deactivate(); return IMG_EIO; }
    WIMStruct *w=NULL;
    int r = wimlib_open_wim(wim_path, 0, &w);
    if(r){ g_last_wrc=r; img_ntfs_ctx_deactivate(); return IMG_ENOENT; }

    /* 里程碑：灌 WIM 是全流程最慢的一步，日志里必须能看到"开始/完成"和耗时，
     *   否则"卡在灌系统这步"和"灌到一半死了"在日志上分不出来。 */
    img_span wsp;
    img_span_prep(&wsp,"wim","apply","灌入 WIM 到分区");
    img_span_kv_str(&wsp,"wim",wim_path);
    img_span_kv_u64(&wsp,"index",(uint64_t)image_idx);
    img_span_kv_u64(&wsp,"part_lba",part_lba);
    img_span_begin(&wsp);
    if(prog){
        struct wimlib_wim_info wi; wimlib_get_wim_info(w,&wi);
        img_prog_phase(prog, IMG_PH_EXTRACT, "灌入 WIM 到分区");
        img_prog_op(prog, IMG_OP_WRITE);
        (void)wi;
    }
    int ef = WIMLIB_EXTRACT_FLAG_NTFS;
    if(opts){
        if(opts->no_acls)  ef |= WIMLIB_EXTRACT_FLAG_NO_ACLS;
        if(opts->no_attrs) ef |= WIMLIB_EXTRACT_FLAG_NO_ATTRIBUTES;
        if(opts->wimboot)  ef |= WIMLIB_EXTRACT_FLAG_WIMBOOT;
    }
    struct cb_ctx cc = { .p = prog };
    wimlib_register_progress_function(w, on_progress, &cc);

    r = wimlib_extract_image(w, image_idx, dummy, ef);
    /* ★ 把 wimlib 的原始错误码留痕：我们把失败统一压成 IMG_EIO 便于调用方处理，
     *   但真因（WIMLIB_ERR_*）不能丢 —— 否则"中断后重跑失败"这类问题只能靠猜。*/
    g_last_wrc = r;
    wimlib_free(w);
    img_ntfs_ctx_deactivate();
    /* ★ 是我们主动取消导致的失败 → 报 IMG_ECANCELLED（不要报 I/O 失败）：
     *   取消是通过 ntfs_device 回调返回 ECANCELED 穿透 wimlib 的，
     *   wimlib 只会笼统报"读写失败"，调用方无法区分"磁盘坏了"和"用户点了停止"。*/
    int frc = r ? (img_cancelled() ? IMG_ECANCELLED : IMG_EIO) : IMG_OK;
    img_span_end(&wsp, frc);
    if(frc!=IMG_OK){
        if(prog) img_prog_done(prog, img_strerror(frc));
        return frc;
    }
    if(prog) img_prog_done(prog, "WIM 灌入完成");
    return IMG_OK;
}
