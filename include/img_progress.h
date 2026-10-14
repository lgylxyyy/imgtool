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

/* 设置当前阶段/操作（变化时会【立即】触发一次上报） */
void img_prog_phase(img_prog *p, img_phase_t ph, const char *msg);
void img_prog_op(img_prog *p, img_op_t op);

/* 设置进度数据（内部按节流决定是否真正上报） */
void img_prog_count(img_prog *p, uint64_t cur, uint64_t total);
void img_prog_bytes(img_prog *p, uint64_t cur_bytes, uint64_t total_bytes);
void img_prog_item(img_prog *p, const char *item);   /* item 换了就立即上报 */

/* 手动强制上报（如里程碑） */
void img_prog_emit(img_prog *p);

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
