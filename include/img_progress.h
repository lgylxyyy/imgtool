/* img_progress.h — 进度上报（节流）+ JSON 行输出 */
#ifndef IMG_PROGRESS_H
#define IMG_PROGRESS_H
#include "img_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct img_prog img_prog;

/* 创建进度器。cb 可为 NULL（则调用者用 img_prog_print() 手动取）
 * throttle_ms: 最小间隔，默认 200 */
img_prog *img_prog_new(img_progress_cb cb, void *ud, int throttle_ms);
void      img_prog_free(img_prog *p);

/* 设置当前阶段/操作（变化时会【立即】触发一次上报）
 * ★ 2026-11-10（REQ-67 + 释放进度）：phase 变化会把 cur/total/cur_bytes/total_bytes
 *   【归零】并重置速率窗口。原因：以前换阶段只换名字，数值留着 ——
 *   wim.apply 里格式化阶段留下的 3/3 被 extract 拿去用，第一帧就报 pct=100
 *   （App 显示成"直接跳到 95%"），随后又掉回 2%。调用方约定：先 phase，再设数值。 */
void img_prog_phase(img_prog *p, img_phase_t ph, const char *msg);
void img_prog_op(img_prog *p, img_op_t op);

/* 设置进度数据（内部按节流决定是否真正上报） */
void img_prog_count(img_prog *p, uint64_t cur, uint64_t total);
void img_prog_bytes(img_prog *p, uint64_t cur_bytes, uint64_t total_bytes);
void img_prog_item(img_prog *p, const char *item);   /* item 换了就上报（走节流，≤5 条/秒）*/
/* ★ REQ-77 Q1：文件计数（逐文件处理的任务用）。total=0 = 总数未知/不适用。
 *   单独给是因为 cur/total 的口径在不同命令里不统一（字节/簇/条目混用）。*/
void img_prog_files(img_prog *p, uint64_t cur, uint64_t total);
/* ★ REQ-77 Q2：当前写入的目标路径（item 是源路径，这个是目标）。传 NULL 清空。*/
void img_prog_dst(img_prog *p, const char *dst);
/* 当前 phase（IMG_PH_NONE = 还没设）。xcopy 用它判断"调用方已经定好段了"。*/
img_phase_t img_prog_phase_of(const img_prog *p);
/* 全局进度（REQ-49 §3）：cur 单调（传小值会被忽略），pct 单调不减。
 * total=0 表示未知（此时 overall_pct=-1）。*/
void img_prog_overall(img_prog *p, uint64_t cur, uint64_t total);
/* 字节口径声明（REQ-51 B）：kind="extracted" 时 cur/total_bytes 的解压后大小；
 * archive_* 是压缩包口径（能算就给，算不了给 0）。*/
void img_prog_bytes_kind(img_prog *p, const char *kind,
                         uint64_t archive_cur, uint64_t archive_total);

/* 手动强制上报（如里程碑） */
void img_prog_emit(img_prog *p);

/* 阶段收尾：把当前 phase 的 pct 补到 100 并发一条（REQ-68 §3.2）。
 * 在"换 phase"之前调；不改变 total 口径，只把 cur 顶到 total。 */
void img_prog_seal(img_prog *p, const char *msg);

/* 必发的 100% 完成事件（模块退出时调用） */
void img_prog_done(img_prog *p, const char *msg);

/* 取消支持：置 1 后各模块应在下一个检查点返回 IMG_ECANCELLED */
void img_prog_cancel(img_prog *p);
int  img_prog_cancelled(const img_prog *p);

/* 直接把进度渲染成一行 JSON（无换行），调用者传足长 buffer 返回长度 */
int img_prog_json(const img_progress_t *pr, char *buf, int cap);

/* 简单的“标准输出 JSON”回调（开发期/CLI 用） */
void img_progress_to_stdout(void *ud, const img_progress_t *p);

#ifdef __cplusplus
}
#endif
#endif
