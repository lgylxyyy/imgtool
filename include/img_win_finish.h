/* img_win_finish.h — 让「裸灌(apply) 后」的 Windows 能继续首次启动
 *
 * 依据（微软官方）：Learn《OS start-up – Computer restarted unexpectedly or
 * encountered an unexpected error》。现象是裸灌 Win7/Win10 首次启动时弹
 *   "计算机意外地重新启动或遇到错误。Windows 安装无法继续。"
 * 官方修复：把
 *   HKLM\SYSTEM\Setup\Status\ChildCompletion\setup.exe  从 1 改成 3
 * 然后重启，安装会继续（进入 OOBE）。
 *
 * 为什么必须离线做：我们在沙箱里改的是 qcow2 里的 NTFS，没有 Windows 运行环境，
 * 跑不了 regedit / reagentc。所以直接改 SYSTEM hive 文件（libhivex）。
 *
 * 只改这一个值，其余字节原样保留；dry_run 时只读不写（便于先诊断）。
 */
#ifndef IMG_WIN_FINISH_H
#define IMG_WIN_FINISH_H
#include "img_types.h"
#include "img_qcow2.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool found_node;               /* \Setup\Status\ChildCompletion 存在 */
    bool found_value;              /* 该键下 setup.exe 存在 */
    int  old_setup_exe;            /* 原值；-1 = 没有这个值 */
    int  new_setup_exe;            /* 写完后的值（dry_run 时=计划值） */
    bool changed;                  /* 是否真的写了盘 */
    /* 诊断：帮助判断它是不是卡在 setup 阶段（-1 = 没有该值）*/
    int  setup_type;               /* \Setup\SetupType */
    int  system_setup_in_progress;  /* \Setup\SystemSetupInProgress */
    bool cmdline_present;          /* \Setup\CmdLine 是否存在 */
} img_win_finish_t;

/* 返回 IMG_OK / IMG_ESTATE（不是可写 NTFS）/ IMG_EFORMAT（hive 打不开）/ IMG_EIO */
int img_win_finish(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes, bool dry_run,
                   img_win_finish_t *out, char *err, size_t errcap);

#endif