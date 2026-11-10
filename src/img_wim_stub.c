/* img_wim_stub.c —— WIM 功能关闭时的占位符号
 *
 * 为什么单独一个文件（2026-11-10）：
 *   CMakeLists 在 IMG_WITH_WIM=OFF 时会从源码列表里移除 img_wim_apply.c /
 *   img_wim_fs.c / img_wim_capture.c，但命令层和 img_recovery.c 仍然引用
 *   img_wim_probe_arch / img_wim_stream_consume 这两个函数 —— 不占位就是
 *   "undefined reference"，也就是 degraded-build-test 里「只关 WIM、保留 NTFS」
 *   那一组编不过的原因。
 *
 *   以前这两个占位写在 img_ntfs_stub.c 里，而那个文件只在 NTFS=OFF 时才编，
 *   于是「WIM 关、NTFS 开」这组漏了。现在按开关拆开：NTFS 的占位归
 *   img_ntfs_stub.c，WIM 的占位归本文件。
 *
 * 约定：一律返回 IMG_ENOTSUP，绝不假装成功（README「不静默降级」）。
 */
#include <stdio.h>
#include <stddef.h>
#include "img_types.h"
#include "img_wim_fs.h"

int img_wim_probe_arch(const char *wim_path, int index, char *out, size_t cap,
                       char *src, size_t srccap, char *why, size_t whycap){
    (void)wim_path; (void)index;
    if(out && cap) out[0]=0;
    if(src && srccap) src[0]=0;
    if(why && whycap) snprintf(why,whycap,"本构建未启用 WIM（wimlib 缺失）");
    return IMG_ENOTSUP;
}

int img_wim_stream_consume(const char *wim_path,int index,const char *inner_path,
                           img_wim_consume_fn consume, void *ud,
                           uint64_t *out_size, img_prog *prog,
                           char *err, size_t errcap){
    (void)wim_path; (void)index; (void)inner_path; (void)consume; (void)ud; (void)prog;
    if(out_size) *out_size=0;
    if(err && errcap) snprintf(err,errcap,"本构建未启用 WIM（wimlib 缺失）");
    return IMG_ENOTSUP;
}