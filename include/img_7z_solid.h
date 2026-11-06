/* img_7z_solid.h — 判断 7z 是否"固实"（REQ-51 A）
 *
 * 为什么需要：7z 的固实块（7z a -ms=on）解第 N 个文件要从前面的数据顺序解，
 * 用户看到的是"解压很慢但看不出原因"。libarchive 不暴露这个标志，只能自己读 7z 头。
 *
 * 7z 的头默认是【压缩】的（kEncodedHeader，LZMA/LZMA2），所以这里：
 *   读 32 字节签名头 → 取 next header → 若是压缩头就用 liblzma 解开 → 解析
 *   MainStreamsInfo/SubStreamsInfo 的 kNumUnPackStream：某个 folder 的解压流 > 1 = 固实。
 *
 * 解析不了（罕见编码/损坏）返回 IMG_OK + *out_solid=false：宁可说"不是固实"（只是提示少一条），
 * 也不能因此让 archive.ls 失败。
 */
#ifndef IMG_7Z_SOLID_H
#define IMG_7Z_SOLID_H
#include "img_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 只认 7z 魔数；不是 7z 返回 IMG_EINVAL。 */
int img_7z_is_solid(const char *path, bool *out_solid);

#ifdef __cplusplus
}
#endif
#endif