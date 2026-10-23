/* img_stream.h — 单向字节流（复制引擎的"源"抽象）
 *
 * 为什么要有它：跨分区/跨镜像/宿主↔镜像 的复制，源可能是 NTFS 属性、FAT 簇链、
 * 宿主 fd、ISO 的 extent、WIM 解出来的临时文件。目标只关心"从 off 读 n 字节"，
 * 不该为每种源各写一遍目标侧逻辑（那是 4×3 = 12 份实现，必分叉）。
 *
 * 契约：
 *   read(ud, off, buf, want, *got)
 *     - 成功返回 IMG_OK，*got ∈ [0, want]；*got==0 表示 EOF（off 已到文件尾）
 *     - 失败返回 IMG_E*，*got 无意义
 *     - 必须支持任意 off（随机读），不要求顺序
 *   close(ud) 可为 NULL；引擎在复制结束（成功/失败/取消）时调用一次。
 */
#ifndef IMG_STREAM_H
#define IMG_STREAM_H
#include "img_types.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_stream {
    void *ud;
    int (*read)(void *ud, uint64_t off, void *buf, size_t want, size_t *got);
    int (*close)(void *ud);
} img_stream;

/* 源文件的元信息（目标侧按需取用：时间/属性/大小）。
 * has_* = false 表示源没有这个概念（宿主 FAT/ISO 没有 NTFS 四时间），
 * 目标侧要按自己的默认规则处理，不能把 0 当成 1970 年写进盘。 */
typedef struct {
    uint64_t size;          /* 必须准确（目标先按它分配/校验） */
    uint64_t times[4];      /* create, modify, access, mft_change（unix 秒 UTC） */
    bool     has_times;
    uint32_t flags;         /* NTFS FILE_ATTR_*；其他源用 0 */
    bool     has_flags;
} img_src_meta;

#ifdef __cplusplus
}
#endif
#endif
