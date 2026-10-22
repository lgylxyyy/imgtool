/* timeout_test — I/O 看门狗（无进展超时）验证
 *
 *   ① 起 150ms 看门狗 → 期间不做任何 I/O 睡 500ms → 应已判超时 + 已触发取消
 *   ② touch（刚做过 I/O）→ 睡 100ms(<300ms) → 不应超时
 *   ③ 看门狗停止后 → 不判超时
 *   ④ 超时后【随后的 I/O 立刻失败】（IMG_ETIMEDOUT）—— 卡死后的 fail-fast
 *   ⑤ 超时判定【不许跨命令残留】—— 真机事故：一次超时让整个进程的 I/O 永久短路
 * 退出码 0=通过。设计依据（见 WORKPLAN）：1MiB 块在 5MB/s 慢卡上只需 ~170ms，
 * 30s 无 I/O = 不是慢是死；这里用 150ms 便于秒级验证。
 */
#define _DEFAULT_SOURCE
#include "img_event.h"
#include "img_io.h"
#include "img_cmd.h"      /* ⑤ 要跑一条真命令 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static int fails=0;
#define OK(c,m) do{ if(c) printf("  ✅ %s\n", m); else { printf("  ❌ %s\n", m); fails++; } }while(0)

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);

    /* ① 无进展 → 判超时 + 取消 */
    img_cancel_reset();
    img_io_watchdog_start(150);
    usleep(500*1000);
    OK(img_io_timed_out(), "500ms 无 I/O → 看门狗判超时");
    OK(img_cancelled(),    "超时后自动触发取消（协作式停）");

    /* ② touch 重新计时 → 不应再超时 */
    img_io_watchdog_stop();
    img_cancel_reset();
    img_io_watchdog_start(300);
    img_io_touch();                 /* 刚完成一次 I/O */
    usleep(100*1000);               /* 100ms < 300ms */
    OK(!img_io_timed_out(), "刚 touch 过（100ms<300ms）→ 不判超时");

    /* ③ 关闭状态 */
    img_io_watchdog_stop();
    img_cancel_reset();
    usleep(400*1000);
    OK(!img_io_timed_out(), "看门狗已停 → 不判超时");

    /* ④ 超时后 I/O fail-fast */
    img_io_watchdog_start(100);
    usleep(300*1000);
    if(!img_io_timed_out()){ printf("  ❌ 前置失败（本应已超时）\n"); fails++; }
    else {
        const char *td=img_tmpdir(); char path[528];
        snprintf(path,sizeof path,"%s/timeout_test_XXXXXX",td?td:".");
        int fd=mkstemp(path); if(fd>=0) close(fd);
        img_io_opts_t o={0};
        img_io *io=img_io_open_path(path,&o);
        if(!io){ printf("  ❌ 临时文件打不开\n"); fails++; }
        else {
            char b[512]; size_t done=0;
            int rc=img_io_read(io,0,b,sizeof b,&done);
            OK(rc==IMG_ETIMEDOUT, "超时后第一次读 → IMG_ETIMEDOUT（fail-fast，不再挂死）");
            img_io_close(io);
        }
        unlink(path);
    }
    img_io_watchdog_stop();

    /* ⑤ ★ 超时判定是进程级的，绝不能带进下一条命令。
     *   真机上出过事故：一次 wim.apply 触发看门狗之后，同一进程里所有镜像
     *   都打不开（被报成"不是合法 qcow2"），只有重启进程才好。
     *   CLI 一条命令一个进程，永远测不出来。 */
    img_io_watchdog_start(100);
    usleep(300*1000);
    if(!img_io_timed_out()){ printf("  ❌ 前置失败（本应已超时）\n"); fails++; }
    else {
        OK(img_io_timed_out(), "前置：已判超时");
        img_result r; memset(&r, 0, sizeof r);
        img_cmd_exec("cmd.list", NULL, NULL, &r);        /* 任何命令都要先清上一条的超时 */
        OK(!img_io_timed_out(), "跑过一条命令后 → 超时判定已复位");
        const char *td = img_tmpdir(); char p2[528];
        snprintf(p2, sizeof p2, "%s/timeout_after_XXXXXX", td ? td : ".");
        int fd2 = mkstemp(p2); if(fd2 >= 0) close(fd2);
        img_io_opts_t o2 = {0}; img_io *io2 = img_io_open_path(p2, &o2);
        char b2[64]; size_t d2 = 0;
        int rc2 = io2 ? img_io_read(io2, 0, b2, sizeof b2, &d2) : -999;
        OK(rc2 != IMG_ETIMEDOUT, "之后的 I/O 不再被短路（能正常发起读）");
        if(io2) img_io_close(io2);
        unlink(p2);
    }
    img_io_watchdog_stop();

    if(fails){ printf("\n════ 结果：❌ 有失败（%d）════\n", fails); return 1; }
    printf("\n════ 结果：✅ 超时契约通过（失败 0）════\n");
    return 0;
}
