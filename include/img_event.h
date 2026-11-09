/* img_event.h — 事件流（一处打点，多处消费）
 *
 * 契约见 DESIGN-OUTPUT.md。三条铁律：
 *   ① 不允许三处各自打点（日志/屏幕/错误卡片必须同源，否则必然分叉）
 *   ② 日志只写【里程碑 + 错误】；过程事件只进内存环形缓冲（出错时随卡片交出）
 *   ③ 错误行任何级别都写、且立即 write（不 fsync）→ 崩溃/被杀也已在盘上
 *
 * 用法（热路径里可以靠返回值提前退出，级别不够时零开销）：
 *   img_ev e;
 *   if(!img_ev_init(&e, IMG_LV_DEBUG, "fat32", "entry", IMG_OK, "写入目录项")) return;
 *   img_ev_str(&e, "path", dst); img_ev_u64(&e, "size", len);
 *   img_ev_emit(&e);
 *
 * 里程碑（自动计时）：
 *   img_span sp; img_span_begin(&sp,"ntfs","put","写入 Winre.wim");
 *   ... 干活 ...
 *   img_span_end(&sp, rc);      → 写 done 行（带 ms 与 rc）
 */
#ifndef IMG_EVENT_H
#define IMG_EVENT_H

#define IMGTOOL_VERSION "imgtool 2026.10.23"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMG_LV_ERROR = 0,
    IMG_LV_WARN,
    IMG_LV_INFO,     /* 默认：里程碑 + 错误 + 心跳 */
    IMG_LV_DEBUG,    /* 加字节级明细（自测绿但外部不认那类问题靠它） */
    IMG_LV_TRACE     /* 加每次 I/O，仅短时使用 */
} img_lv_t;

/* ---------- 消费者：日志文件 ---------- */
/* path 为 NULL 时用 fd（Android SAF 没路径）；max_bytes=0 用默认 4MiB；keep=保留份数 */
int  img_log_open   (const char *path, img_lv_t lv, size_t max_bytes, int keep);
int  img_log_open_fd(int fd,          img_lv_t lv, size_t max_bytes, int keep);
void img_log_close  (void);
/* 真正打开着的日志路径；没开日志 / 用 fd 模式 → NULL。
 * ★ 失败卡片的"日志："这一行必须用它，不能自己拼 <img>.imgtool.log —— 那样
 *   --log=/别的路径 时卡片会印错，而"卡片里印的路径无法打开"会让用户以为附件漏发。*/
const char *img_log_path(void);
void img_log_set_level(img_lv_t lv);

/* 结果/日志的输出 fd（-1 = 没开）。流式导出时要把 fd 1 让给数据，
 * 这时把日志/进度改到一个 dup 出来的真 stdout 上，免得把字节流冲烂。*/
int  img_log_out_fd(void);
void img_log_set_out_fd(int fd);
img_lv_t img_log_level(void);
bool img_log_enabled(img_lv_t lv);
/* 会话头（版本/命令/镜像指纹）：立刻写一行；发生轮转时自动在新文件头补写 */
/* 老 CLI 一行接入：扫描 argv 里的 --log=/--log-level=/--log-max=/--log
 *   （不给值的 --log 用 <第一个参数>.imgtool.log 当默认路径）
 *   返回是否打开了日志；没传 --log 就是什么都不做（返回 0，不算错）。 */
int  img_log_cli(int argc, char **argv, const char *tool);
/* 把 argv 里我们认识的开关抽掉（不想动原参数解析的工具可调用它得到干净的 argv）*/
int  img_log_strip_args(int argc, char **argv, char **out, int outcap);

void img_log_session(const char *tool, const char *argv, const char *img, uint64_t img_size);
/* 屏幕消费者（0c 用；现在挂上去也只是原样打里程碑行）*/
void img_out_open_fd(int fd, bool human);

/* ---------- 打点 ---------- */
#define IMG_EV_MAXF 12
#define IMG_EV_MAGIC 0x45564E54u   /* 'EVNT'：哨兵，防止"未 init 就 emit"*/
typedef struct img_ev {
    unsigned magic;
    img_lv_t lv;
    const char *mod, *ev, *msg;
    int rc;
    long long err;                     /* errno */
    struct { const char *k; int kind; const char *s; long long n; } f[IMG_EV_MAXF];
    int nf;
    int enabled;
} img_ev;

/* ★ 文案渲染回调：库只发【语义码 + 数值】，文案（中文/英文/图标）由消费端决定。
 *   CLI 传一个渲染中文/英文的回调；App 传自己的（或不传 = 不要说明行）。
 *   （这就是"库不带语言，语言在视图层"的落点）*/
typedef void (*img_text_cb)(const img_ev *e, void *ud);
void img_out_set_text(img_text_cb cb, void *ud);

/* 返回 1=已启用（继续填空并 emit） 0=级别不够（可立即 return，零开销）*/
int  img_ev_init(img_ev *e, img_lv_t lv, const char *mod, const char *ev, int rc, const char *msg);
void img_ev_str (img_ev *e, const char *k, const char *v);
void img_ev_u64 (img_ev *e, const char *k, uint64_t v);
void img_ev_i64 (img_ev *e, const char *k, long long v);
void img_ev_hex (img_ev *e, const char *k, uint64_t v);
void img_ev_errno(img_ev *e, int err);
void img_ev_emit(img_ev *e);
void img_ev_free (img_ev *e);      /* 用完后清哨兵（可选）*/

/* ---------- 里程碑（begin/done 配对） ---------- */
/* 用法：
 *   img_span sp;
 *   img_span_prep(&sp,"fat32","put","写入文件");     // 计时 + 攒 begin 行的字段
 *   img_span_kv_str(&sp,"path",dst); img_span_kv_u64(&sp,"size",len);
 *   img_span_begin(&sp);                             // 发 begin 行（带上面这些字段）
 *   ...干活...
 *   img_span_kv_u64(&sp,"written",n);                 // done 前还能追加
 *   img_span_end(&sp, rc);                            // 发 done 行（ms + rc）
 */
typedef struct img_span {
    const char *mod, *op;
    long long t0;
    int depth;
    int started;
    img_ev ev;                 /* begin 行的字段（复用事件构造器）*/
} img_span;
void img_span_prep (img_span *s, const char *mod, const char *op, const char *msg);
void img_span_kv_str(img_span *s, const char *k, const char *v);
void img_span_kv_u64(img_span *s, const char *k, uint64_t v);
void img_span_begin(img_span *s);
void img_span_end  (img_span *s, int rc);

/* ---------- 心跳（长循环里调；内部退避 2→4→8…→60s，只在有操作进行中才打） ---------- */
void img_hb(const char *mod, const char *op, const char *where, uint64_t iter);

/* ---------- 过程事件环形缓冲（不落盘；出错时随失败卡片交出） ---------- */
/* ★ 环形缓冲级别独立于日志级别：日志可以只记 info，但过程仍按 debug 抓进内存 */
void img_ring_set_level(img_lv_t lv);
void img_ring_add(const char *line);
/* 倒序/正序导出最近 n 条，cb 返回非 0 中止；返回实际条数 */
int  img_ring_dump(void *ud, int (*cb)(void *ud, const char *line), int max);
int  img_ring_count(void);


/* ---------- 失败上下文（App 拿字段自己渲染本地化卡片）----------
 * 事件里已有语义码（event/action/error_code + 各数值），这里补一份"可直接给 UI"的
 * 结构化失败快照：App 用自己的语言渲染卡片，不必解析日志/JSON。*/
typedef struct {
    char cmd[64];        /* 哪个命令失败 */
    char code[48];       /* 稳定错误码，如 FAT-ENOSPC（★ App 本地化查表的键）*/
    int  rc;             /* img_err_t */
    char at[256];        /* 失败位置（路径/分区/LBA）*/
    char what[256];      /* 【中文备注】干到哪一步；App 请用 code/at（语言中立）*/
    char status[256];    /* 【中文备注】当前状态；App 按 code 自己渲染（如 CMD-CANCELLED→"该分区需重做"）*/
    char img[512];       /* 镜像路径 */
    char ctx[256];       /* 命令与参数 */
    char log[560];       /* 日志文件路径（App 可提示用户一并发）*/
    char events[2048];   /* 事件尾部（内存环形缓冲里的机器格式行）*/
    int  events_len;     /* 尾部字节数（注意：不是条数）*/
} img_error_info_t;
void img_error_info(img_error_info_t *out);      /* 取"上一次失败"的上下文（无失败则 code 为空）*/
/* 内部：失败时记录快照（img_result_fail / img_error_report 都会调）*/
void img_error_record(const char *cmd, const char *code, int rc, const char *at,
                      const char *what, const char *why);

/* 文案查表：App 可以直接用，也可以用自己的 i18n 覆盖（key = error_code / action）
 * lang: "zh"（默认）| "en"；查不到返回通用文案（不会返回 NULL）*/
void img_set_image(const char *path);             /* 记录当前镜像（CLI 卡片/.error.txt 会用）*/
const char *img_platform(void);                   /* "Linux"/"Android"/"windows"（CLI 卡片用）*/
const char *img_abi(void);                        /* "64-bit" */

/* ---------- 失败卡片（0c 完整版；这里先落地最小可用） ---------- */
void img_error_report(int rc, const char *code, const char *mod, const char *op,
                      const char *where, const char *why, const char *suggest);
/* ★ 失败卡片：说明 + 可整段复制。写屏幕（human 模式）并落 <镜像>.error.txt，
 *   内容含：工具/版本/命令/镜像指纹 / 失败位置 / 错误码 / 原因 / 建议 / 事件尾部。
 *   App 侧可以直接把它塞进"复制到剪贴板"按钮。*/
/* ---------- I/O 看门狗（无进展超时）----------
 * 语义：【连续 N 毫秒没有任何一次磁盘读写完成】→ 视为无响应：
 *   1) 记一条 E 级日志（code=IO-TIMEOUT，任何日志级别都写）
 *   2) 触发取消（协作式检查点退出）
 *   ★ 它不能打断"卡在内核/JNI 里的那一次调用"，但能让【随后的每一次 I/O】立刻失败、
 *     让操作尽快停下来并给出明确错误码 —— 比"静默挂死"强得多。
 * 默认 30 秒（见 DESIGN-OUTPUT.md 第十二节：1MiB 块在 5MB/s 慢卡上只需 ~170ms，
 * 30s 无 I/O = 不是慢是死）；0 = 关闭。
 * 消费方：img_io_read/write 每次完成时 touch；超时后它们在入口 fail-fast。*/
void img_io_watchdog_start(int timeout_ms);   /* 0=关闭 */
void img_io_watchdog_stop(void);
bool img_io_timed_out(void);                  /* 是否已判超时（I/O 入口据此 fail-fast）*/
void img_io_touch(void);                      /* I/O 层每完成一次读写调一次 */
/* 清掉"已判超时"。★ 每条命令开始时必须调：这个标志是进程级的，
   残留会让之后所有 I/O 立刻失败（真机上表现为"所有映像都不是合法 qcow2"，
   只有重启进程才好）。 */
void img_io_timeout_reset(void);

/* ---------- 取消（P0-2）----------
 * 全局标志：任何线程可置位，正在跑的操作在"安全检查点"退出。
 * 检查点 = 数据写入的每个 1MiB 块；取消后【不写目录项 + 释放已分配簇链】，
 * 所以不会留下"半成品文件"。*/
void img_cancel(void);        /* 请求取消 */
void img_cancel_reset(void);  /* 下一个操作前清掉 */
bool img_cancelled(void);     /* 当前是否已请求取消 */

/* 卡片里"命令与参数"那一行（CLI/App 启动时给一次即可）*/
void img_card_set_context(const char *kv_line);

#ifdef __cplusplus
}
#endif
#endif
