#!/bin/bash
# run_all.sh — 一键跑全部健壮性/回归测试（改完代码先跑它）
#   用法: tests/run_all.sh
# 覆盖：
#   cmd_test.sh          命令表 27 条（成功/失败/参数校验/check_size/宿主空间/尺寸估算/日志）
#   corrupt-chain-test.sh 反向：环状 FAT 必须快速报错，不许死循环
#   fd_test              fd 生命周期（坏 fd/所有权/提前关闭）
#   thread_test          并发（日志线程安全/两镜像并行/同 ctx 串行）
#   cancel_test          取消（不留半成品/不漏簇/不污染下一次/覆盖写时旧文件完好）
#   bcd_test             BCD 读写（需 libhivex）
#   recovery_bcd_test    恢复条目（需 libhivex + 官方 ISO BCD 参考）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
pass=0; fail=0; skipped=0
run(){ # $1=名字 $2...=命令
  local name="$1"; shift
  printf "\n════ %s ════\n" "$name"
  if "$@" > /tmp/runall.out 2>&1; then
    echo "  ✅ $name"; pass=$((pass+1))
    tail -3 /tmp/runall.out | sed 's/^/     /'
  else
    echo "  ❌ $name（失败，末尾输出↓）"; fail=$((fail+1))
    tail -12 /tmp/runall.out | sed 's/^/     /'
  fi
}

# 公共：一张小盘（供 fd/thread/cancel/corrupt 用）
W=$(mktemp -d /tmp/runallXXXXXX)
$T/imglayout mk "$W/a.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
$T/imgmkfs "$W/a.qcow2" --mkfs 2048 T >/dev/null 2>&1
$T/imglayout mk "$W/b.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
$T/imgmkfs "$W/b.qcow2" --mkfs 2048 T >/dev/null 2>&1
head -c 16777216 /dev/urandom > "$W/big.bin"      # 取消测试要够大的源（16MB）

run "cmd_test（命令表/校验/日志 27 条）"  bash "$ROOT/tests/cmd_test.sh"
run "corrupt-chain-test（防死循环）"      bash "$ROOT/tests/corrupt-chain-test.sh"
run "fd_test（fd 生命周期 15 条）"        "$ROOT/build/tests/fd_test" "$W/a.qcow2"
run "thread_test（并发 4 组）"            "$ROOT/build/tests/thread_test" "$W/a.qcow2" "$W/b.qcow2" 2048 2048 "$W/t.log"
run "cancel_test（取消 7 条）"            "$ROOT/build/tests/cancel_test" "$W/a.qcow2" 2048 "$W/big.bin"
if [ -x "$ROOT/build/tests/bcd_test" ]; then
  run "bcd_test（BCD 读写）" "$ROOT/build/tests/bcd_test" "$ROOT/reference/bcd-reference-uefi.bin" "$W/bcd_out.bin"
else skipped=$((skipped+1)); echo "  ⏭ 跳过 bcd_test（没编进构建）"; fi
if [ -x "$ROOT/build/tests/recovery_bcd_test" ]; then
  ISOBCD="$ROOT/../build/w128/iso/bcd"
  if [ -f "$ISOBCD" ]; then
    run "recovery_bcd_test（恢复条目）" "$ROOT/build/tests/recovery_bcd_test" \
        "$ROOT/reference/bcd-reference-uefi.bin" "$W/rec.bin" '\Recovery\WindowsRE\Winre.wim' "$ISOBCD" \
        5616a432-a581-0f42-8302-cff19ec2690f 08cb2fb9-d284-f948-9892-70ef8e7d70cd
  else skipped=$((skipped+1)); echo "  ⏭ 跳过 recovery_bcd_test（缺官方 ISO BCD 参考）"; fi
else skipped=$((skipped+1)); echo "  ⏭ 跳过 recovery_bcd_test（没编进构建）"; fi

echo
echo "════ 总计：通过 $pass · 失败 $fail · 跳过 $skipped ════"
if [ $fail -eq 0 ]; then echo "✅ 全量回归通过"; rm -rf "$W"; else echo "❌ 有失败（调试目录 $W 保留）"; fi
[ $fail -eq 0 ]
