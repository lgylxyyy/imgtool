/* img_clone.h —— 扇区级克隆（换系统的主路径，PARTITION-POLICY #6）
 *
 *   把【给定的保留分区】拷进一张全新盘；没列进来的分区在新盘上【留空】。
 *   换系统流程：系统分区不列 → 旧系统数据根本不进新文件 → 新盘天然更小。
 *
 * ★ 层归属（规范第 0 条）：这是【机制不是政策】——
 *     保留哪些分区、要不要重新生成 GUID、旧文件叫什么名字，
 *     全部由调用方（App/CLI 命令层）决定，库里一个默认值都不设。
 *
 * 铁律（DESIGN-OUTPUT.md 第十二节·结构类）：
 *   ① 源文件【全程只读】—— 出任何错，旧盘还是完好的
 *   ② GPT 主表+备份表双写、CRC 重算（img_parttable_write 负责）
 *   ③ 保持稀疏：全零块跳过不写（否则拷空分区会把新文件写大）
 *   ④ 新盘虚拟大小 = 源盘虚拟大小（分区表原样保留，布局不变）
 */
#ifndef IMG_CLONE_H
#define IMG_CLONE_H
#include "img_types.h"
#include "img_progress.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t bytes_written;  /* 实际写进新盘的字节（跳零之后；小于保留分区总容量是正常的）*/
    int      parts_copied;   /* 拷了几个分区 */
    uint64_t clusters_compressed;   /* 按压缩簇写的簇数（T6）*/
} img_clone_stats_t;

/* 目标盘压缩算法（qcow2 头 compression_type）。
 *   AUTO：跟源盘走（源是 raw / zlib → zlib；源是 zstd → 保持 zstd）。
 *   ZSTD：qcow2 v3 + incompatible「compression type」位 —— QEMU 2.10 起能读、
 *         5.1 起能写；更老的 QEMU / 其它工具会拒绝打开【整个映像】。 */
typedef enum {
    IMG_CLONE_CTYPE_AUTO = 0,
    IMG_CLONE_CTYPE_ZLIB = 1,
    IMG_CLONE_CTYPE_ZSTD = 2,
} img_clone_ctype_t;

/* 克隆。keep_lbas/nkeep = 要保留的分区起始 LBA 列表（不在此列表的分区 → 新盘留空）。
 * guid_random = true 重新生成磁盘/分区 GUID（推荐：避免同机挂两块同 GUID 盘），
 *               false 逐字节保持（完全相同的克隆）。
 * compress = true 时能压的簇按 qcow2 压缩簇写（「压不动」的簇自动退回普通簇，
 *               不报错）；压缩算法由 ctype 决定。
 * 成功返回 IMG_OK；失败返回 IMG_E* 且【源文件未被修改】。*/
int img_clone(const char *src_path, const char *dst_path,
              const uint64_t *keep_lbas, int nkeep,
              bool guid_random,
              bool compress,
              img_clone_ctype_t ctype,
              img_clone_stats_t *out, img_prog *prog);

#ifdef __cplusplus
}
#endif
#endif
