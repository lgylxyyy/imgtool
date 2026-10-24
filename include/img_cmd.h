/* img_cmd.h — 命令表（"加新功能 = 表里加一行 + 写一个 handler"）
 *
 * 为什么这么设计（见 DESIGN-OUTPUT.md 第七节 / WORKPLAN-HARDEN.md）：
 *   现在每加一个能力要在 5 处各写一遍（库/CLI/JNI/App UI/文档），而且校验规则
 *   写两遍（CLI 一遍、App 一遍）→ 必然分叉。做成表之后：
 *     · 参数规格（类型/必填/默认/约束/帮助）只写一处
 *     · CLI 解析、App 校验、帮助文本、App 自动生成 UI 全部读这张表
 *     · 结果统一成 {"ok":true,"data":{...}} 信封，错误统一成 {"ok":false,"err":{...}}
 *   加新功能的标准动作：写一个 handler + 表里加一行 + 加两个用例。
 *
 * 入口稳定性：CLI / App(JNI) / 测试 / 以后 Windows 版，都调同一份 img_cmd_exec()，
 * 只是"谁来喂参数"不同 → 功能是数据、入口是稳定的，这就是"加功能代价最小"。
 */
#ifndef IMG_CMD_H
#define IMG_CMD_H
#include "img_types.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_progress.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 参数集合（名称 → 字符串值；按规格做类型转换与校验） ---------------- */
typedef struct img_args img_args;
img_args   *img_args_new(void);
void        img_args_free(img_args *a);
int         img_args_set(img_args *a, const char *k, const char *v);   /* CLI/App 都传字符串 */
const char *img_args_get(const img_args *a, const char *k);            /* 无则 NULL */
uint64_t    img_args_u64(const img_args *a, const char *k, uint64_t def);
long long   img_args_i64(const img_args *a, const char *k, long long def);
bool        img_args_bool(const img_args *a, const char *k, bool def);
int         img_args_count(const img_args *a);

/* ---------------- 参数规格 ---------------- */
typedef enum { IMG_A_STR = 1, IMG_A_U64, IMG_A_I64, IMG_A_BOOL, IMG_A_ENUM } img_arg_ty;
typedef struct img_arg {
    const char *name;                 /* "lba" / "src" / "check_size" */
    img_arg_ty  ty;
    bool        required;
    const char *def;                  /* 默认值（字符串形式，可 NULL）*/
    const char *help;                 /* ★ 帮助文本：CLI --help 与 App 提示共用 */
    int       (*check)(const char *val, char *why, size_t whycap);   /* 可选：值级校验 */
} img_arg;

/* 按规格校验：缺必填 / 类型不符 / check 不通过 → 返回错误码并填 why（给人话用） */
int img_args_validate(const img_args *a, const img_arg *spec, int nargs,
                      char *why, size_t whycap);

/* ---------------- 结果信封（栈上结构，无 malloc） ---------------- */
#define IMG_RES_FIELDS 24   /* win.version 一条就出 18 个字段，16 会静默丢 */
typedef struct img_result {
    bool        ok;
    const char *cmd;
    long long   t0_ms;
    /* data: 键值对（字符串/整数/布尔） */
    /* s/at 放大到 1024：255 个中文字符 = 765 字节，256 字节会把名字
     * 【静默截断】（UI 看到的名字与盘上不一致，比报错更坏）。 */
    struct { char k[40]; int ty; char s[1024]; long long n; bool b; const char *raw; } f[IMG_RES_FIELDS];
    int nf, nf_drop;    /* nf_drop>0 → 结果里会出现 "_overflow"，绝不静默丢字段 */
    /* err */
    int  rc;
    char code[48], why[256], at[1024], suggest[256];
} img_result;

void img_result_begin(img_result *r, const char *cmd);
void img_result_str (img_result *r, const char *k, const char *v);
void img_result_u64 (img_result *r, const char *k, uint64_t v);
void img_result_i64 (img_result *r, const char *k, long long v);
void img_result_bool(img_result *r, const char *k, bool v);
/* 直接把一段【已渲染好的 JSON】塞进 data（用于列表/子对象；指针需活到渲染完成）*/
void img_result_raw (img_result *r, const char *k, const char *json);
/* 失败：填 err 段（含建议），并写事件流日志（E 级，任何级别都会记） */
void img_result_fail(img_result *r, int rc, const char *code, const char *at,
                     const char *why, const char *suggest);
int  img_result_json(const img_result *r, char *buf, int cap);   /* 渲染一行 JSON */

/* ---------------- 命令上下文（镜像句柄 + 惰性打开） ---------------- */
typedef struct img_ctx img_ctx;
img_ctx   *img_ctx_open(const char *image_path, bool writable, bool check_size,
                        char *err, size_t errcap);
void       img_ctx_close(img_ctx *c);
const char *img_ctx_path(const img_ctx *c);
bool        img_ctx_check_size(const img_ctx *c);
img_qcow2  *img_ctx_qcow2(img_ctx *c);                 /* 惰性打开；失败返回 NULL */
img_io     *img_ctx_io(img_ctx *c);                    /* 底层 I/O（拿 fd 查宿主空间）*/
int         img_ctx_parttable(img_ctx *c, img_parttable_t **out);   /* 惰性读 */

/* ---------------- 命令表 ---------------- */
typedef struct img_cmd {
    const char *name;        /* "part.show" */
    const char *since;       /* "1.0" —— 版本兼容用 */
    const char *help;
    const img_arg *args; int nargs;
    int (*run)(img_ctx *ctx, const img_args *in, img_result *out);
    /* 位置参数（2026-10-24）：只有 copy/move/rename/rm 用。
     * usage 给 CLI --help 与 App 显示；npos 是允许的位置参数个数，
     * 其余命令保持 NULL/0（传位置参数直接报错，不会瞎填）。*/
    const char *usage;
    int npos;
} img_cmd;

const img_cmd *img_cmd_find(const char *name);
const img_cmd *img_cmd_table(int *out_count);
/* 查表 → 校验参数 → 计时 → 调 handler；返回 IMG_OK 或错误码（结果都写进 out） */
int img_cmd_exec(const char *name, img_ctx *ctx, const img_args *in, img_result *out);
/* 导出命令表（含参数规格/帮助/校验提示）→ App 可用它自动生成界面、也能生成 --help */
int img_cmd_list_json(char *buf, int cap);

/* ---------------- 进度通道（P2） ----------------
 * 之前每个 handler 都往库函数里传 NULL → App 只能看到 begin/done 里程碑，
 * 灌 6.8 GB 的 ESD 与写 6 KB 小文件在 UI 上完全一样（手册 §7）。
 * 用法：
 *     img_cmd_set_progress(my_cb, ud);          // 进程级；传 NULL 关闭
 *     ... img_cmd_exec(...) ...                 // handler 内部自动带上
 * 语义：cb 只在【注册了】才被触发（没注册 = 零开销，与旧行为完全一致）；
 *       节流/相位变化由 img_prog 内部处理（默认 200 ms，phase/op/item 变化立即发）。
 * ⚠ 进程级单例：两个线程并行跑不同镜像时进度会混在一起 —— 与日志同一个取舍，
 *    App 侧请串行执行（或自己按 cmd 名过滤）。*/
void img_cmd_set_progress(img_progress_cb cb, void *ud);
/* handler 里拿进度器；没注册返回 NULL（可以直接传给库函数）。*/
img_prog *img_cmd_prog(void);

#ifdef __cplusplus
}
#endif
#endif
