#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_wim_apply.c — Stage 5：WIM → qcow2 分区（原理见头文件）
 *
 * 关键接线：wimlib 用 WIMLIB_EXTRACT_FLAG_NTFS 走 libntfs-3g 直写模式，
 * 其 ntfs_mount() 经宏 ntfs_device_default_io_ops → ntfs_device_unix_io_ops
 * → 我们在 img_ntfs_fmt.c 定义的同名符号 → 10 个回调 → qcow2 分区。 */
#include "img_wim_apply.h"
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

/* wimlib 进度 → img_prog */
struct cb_ctx { img_prog *p; };
static enum wimlib_progress_status on_progress(enum wimlib_progress_msg msg,
                                               union wimlib_progress_info *info, void *ctx){
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
        img_prog_count(c->p, info->extract.completed_streams, info->extract.total_streams);
        break;
    case WIMLIB_PROGRESS_MSG_EXTRACT_IMAGE_END:
        if(info->extract.total_bytes){
            img_prog_bytes(c->p, info->extract.total_bytes, info->extract.total_bytes);
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
    const char *dummy = "imgtool-dev";
    int fd = open(dummy, O_RDWR|O_CREAT, 0666);
    if(fd<0) return IMG_EIO;
    close(fd);

    img_ntfs_ctx_activate(q, part_lba, pb);     /* 让回调指向这个分区 */

    if(wimlib_global_init(0)!=0){ img_ntfs_ctx_deactivate(); return IMG_EIO; }
    WIMStruct *w=NULL;
    int r = wimlib_open_wim(wim_path, 0, &w);
    if(r){ img_ntfs_ctx_deactivate(); return IMG_ENOENT; }

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
    wimlib_free(w);
    img_ntfs_ctx_deactivate();
    if(r){
        if(prog) img_prog_done(prog, wimlib_get_error_string(r));
        return IMG_EIO;
    }
    if(prog) img_prog_done(prog, "WIM 灌入完成");
    return IMG_OK;
}
