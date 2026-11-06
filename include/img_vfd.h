/* img_vfd.h — 虚拟文件描述符：把"ISO 里的一个成员"伪装成本地可 seek 文件（REQ-16）
 *
 * 要解决的问题：用户不想为了灌 4~5 GB 的 install.wim 先在手机上多拷一份。
 *   · wimlib 只接受【真实文件路径】（wimlib_open_wim → open()/fstat()/lseek()/pread()），
 *     没有"自定义读取器"的公开 API；pipable 只对 pipable WIM 有效，微软的 install.wim/esd 不是。
 *   · 于是我们把 libc 的那几个 fd 操作在自己的 .so 里改道（-Wl,--wrap=…）：
 *     传一个不存在的假路径 /imgtool-vfd/<n>，open() 换成 /dev/null 的 fd 并登记，
 *     之后 read/pread/lseek/fstat/close 全部翻译成"对 ISO 的 img_iso_read_range"。
 *     一个字节都不落盘，也不需要 root/FUSE。
 *   · 前提：wimlib 的 IO 调用必须在本 .so 内（= 静态链接 libwim.a）。
 *     共享库版 wimlib 调用的是系统 libc 的 read/pread，--wrap 拦不住 → IMG_WIM_VFD=0，
 *     调用方自动退回"先抽到临时文件"的老路。
 */
#ifndef IMG_VFD_H
#define IMG_VFD_H
#include "img_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 本构建能不能用（1=静态 wimlib，直灌可用；0=只能用抽取退路）*/
bool img_vfd_ready(void);

/* 挂一个 ISO 成员，成功时 vpath 里是假路径（形如 /imgtool-vfd/0），size 是该成员字节数。
 * 用法：把 vpath 直接喂给 wimlib_open_wim()；用完调 img_vfd_unmount(vpath)。*/
int  img_vfd_mount(const char *iso_path, const char *inner_path,
                   char *vpath, size_t cap, uint64_t *size,
                   char *err, size_t errcap);
void img_vfd_unmount(const char *vpath);

/* 只给测试用：当前挂了几个 */
int  img_vfd_active(void);

#ifdef __cplusplus
}
#endif
#endif