/* img_tbuf.h — 线程级可增长缓冲（REQ-20261026-21）
 *
 * 为什么不是 `_Thread_local char buf[N]`：
 *   静态 TLS 是【计入线程栈预算】的（glibc 把静态 TLS 放在线程栈顶；bionic 同理）。
 *   本库原来有 9 个 IMG_THREAD_LOCAL 缓冲、合计 1.53 MB —— 宿主只要开一条 1 MB 栈的
 *   工作线程（Android 的 ART worker 就是），pthread_create 直接失败，或者一跑就爆栈
 *   （真机 tombstone：mkntfs 的 bitmap_allocate / wimlib 的 open_wim_as_WIMStruct）。
 * 现在的做法：堆上分配 + pthread_key 析构。
 *   生命周期与原来的 TLS 完全一致（活到线程结束）—— 这一点很重要，
 *   因为 img_result_raw() 只存指针，缓冲必须活到调用方渲染完；
 *   TLS 只留 8 字节的 arena 指针，不再占栈。
 */
#ifndef IMG_TBUF_H
#define IMG_TBUF_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_TB_LS = 0,      /* fat.ls 的条目 JSON */
    IMG_TB_NTFS_LIST,   /* ntfs.list 的条目 JSON */
    IMG_TB_CMD_LIST,    /* cmd.list 全文 */
    IMG_TB_ISO_LIST,    /* iso.ls / iso.find */
    IMG_TB_ID_SCAN,     /* wim.identify --scan=true 的候选 */
    IMG_TB_ID_IMAGES,   /* wim.identify 的映像清单 */
    IMG_TB_WIM_LS,      /* wim.ls 的条目（最大 512 KB）*/
    IMG_TB_ITEMS1,      /* 批量复制明细（file.copy）*/
    IMG_TB_ITEMS2,      /* 批量复制明细（xcopy）*/
    /* ★ 接力槽：命令在大栈线程里跑完后，raw JSON 的指针还要继续有效
     *   （那个线程的 arena 会随线程退出释放），所以由调用方线程把内容拷进这里。
     *   数量 = img_result 里 raw 字段的上限。*/
    IMG_TB_RELAY0, IMG_TB_RELAY1, IMG_TB_RELAY2, IMG_TB_RELAY3, IMG_TB_RELAY4, IMG_TB_RELAY5, IMG_TB_RELAY6, IMG_TB_RELAY7, IMG_TB_RELAY8, IMG_TB_RELAY9, IMG_TB_RELAY10, IMG_TB_RELAY11, IMG_TB_RELAY12, IMG_TB_RELAY13, IMG_TB_RELAY14, IMG_TB_RELAY15, IMG_TB_RELAY16, IMG_TB_RELAY17, IMG_TB_RELAY18, IMG_TB_RELAY19, IMG_TB_RELAY20, IMG_TB_RELAY21, IMG_TB_RELAY22, IMG_TB_RELAY23,
    IMG_TB_RELAY_MAX = 24,
    IMG_TB_COUNT
} img_tb_id;

/* 拿到至少 want 字节的线程私有缓冲（首次自动分配；线程退出自动释放）。
 * 返回 NULL = 内存不足（调用方自己报 IMG_ENOMEM）。*/
char *img_tbuf(img_tb_id id, size_t want);

#ifdef __cplusplus
}
#endif
#endif
