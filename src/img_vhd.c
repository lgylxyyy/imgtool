/* img_vhd.c — VHD（Virtual PC / Hyper-V 旧格式）后端（P3-4）
 * 完整实现见 P3-4。当前是链接桩：所有操作返回 ENOTSUP。
 * 探测已可用：img_diskfmt_probe 认 footer 的 "conectix"。 */
#include "img_qcow2_int.h"
#include "img_types.h"

int img_vhd_open(img_qcow2 *q){ (void)q; return IMG_ENOTSUP; }
void img_vhd_close(img_qcow2 *q){ (void)q; }
int img_vhd_read(img_qcow2 *q, uint64_t off, void *buf, size_t len){
    (void)q;(void)off;(void)buf;(void)len; return IMG_ENOTSUP; }
int img_vhd_write(img_qcow2 *q, uint64_t off, const void *buf, size_t len){
    (void)q;(void)off;(void)buf;(void)len; return IMG_ENOTSUP; }
