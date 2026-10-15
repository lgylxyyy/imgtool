/* ntfs_bridge.h — 内部：ntfs_device_operations 桥接（需 libntfs-3g 头）
 * 由 img_bootcheck.c 与 img_ntfs_fs.c 共用，避免两份相同回调。 */
#ifndef NTFS_BRIDGE_H
#define NTFS_BRIDGE_H
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdarg.h>     /* ntfs logging.h 用 va_list */
#include <time.h>       /* 必须在 ntfs 头之前（ntfstime.h 会重定义 timespec）*/
#include <sys/types.h>
#include <sys/stat.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/layout.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/unistr.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>

/* ★ mkntfs 用 ntfs_device_alloc(name,0,&ops,NULL) 建设备 → d_private 是 NULL，
 *   所以回调一律从我们自己的全局上下文取（img_ntfs_get_active_ctx）。 */
extern struct ntfs_device_operations *img_ntfs_device_ops(void);
#endif
