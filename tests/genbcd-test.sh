#!/bin/bash
# genbcd-test.sh — imggenbcd（UEFI/GPT BCD 定制）回归
#
# 背景：imggenbcd 是 UEFI 盘的"最后一块"——把参考 BCD 里写死的
#       源盘分区/磁盘 GUID 换成目标盘的；漏改一处 bootmgr 就 0xc0000098/0xc000000e。
# 本测试用一张新 GPT 盘的真 GUID 生成 BCD，再用独立的 hive 读取路径逐个核对：
#   · 系统分区 GUID 应出现 3 处（resume + OSLoader 11000001/21000001）
#   · ESP  GUID 应出现 2 处（bootmgr + memtest）
#   · 磁盘 GUID 应出现 5 处（每个分区设备对象一个）
# 另外覆盖：非法 GUID → 非零退出；生成的 BCD 能被 hivex 重新打开。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
PT="$ROOT/build/tests/mbr_bcd_test"
REF="$ROOT/reference/bcd-reference-uefi.bin"
W=$(mktemp -d /tmp/genbcdXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }

echo "════ 0. 造一张 3GiB GPT 盘（ESP 200MiB + MSR + 系统 + RE）════"
$T/imglayout mk "$W/t.qcow2" $((3*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1 \
  || { echo "建盘失败"; exit 2; }
$T/imgpart show "$W/t.qcow2" > "$W/pt.json" 2>/dev/null
SYSG=$(python3 -c "import json;d=json.load(open('$W/pt.json'));print([p['partGuid'] for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
ESPG=$(python3 -c "import json;d=json.load(open('$W/pt.json'));print([p['partGuid'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
DISKG=$(python3 -c "import json;d=json.load(open('$W/pt.json'));print(d['diskGuid'])")
echo "  系统=$SYSG"; echo "  ESP =$ESPG"; echo "  磁盘=$DISKG"
[ -n "$SYSG" ] && [ -n "$ESPG" ] && [ -n "$DISKG" ] || { echo "取 GUID 失败"; exit 2; }

echo "════ 1. 生成 BCD（5 参：系统/磁盘/ESP）════"
$T/imggenbcd "$REF" "$W/out.bin" "$SYSG" "$DISKG" "$ESPG" > "$W/gen.log" 2>&1
rc=$?
[ $rc -eq 0 ] && ok "退出码 0" || { ng "退出码 $rc"; cat "$W/gen.log"; }
[ -s "$W/out.bin" ] && ok "输出 BCD $(stat -c%s "$W/out.bin") 字节" || ng "没生成输出"

echo "════ 2. 独立核对：逐个分区设备对象 ════"
"$PT" --list-guid "$W/out.bin" > "$W/list.txt" 2>/dev/null
[ $? -eq 0 ] && ok "生成的 BCD 能被 hivex 重新打开" || ng "生成的 BCD 打不开"
n=$(grep -c '^' "$W/list.txt"); n=$((n-1))
[ "$n" = "5" ] && ok "共 5 个分区设备对象" || ng "设备对象数=$n（应为 5）"
nsys=$(grep -c "part=$SYSG" "$W/list.txt")
nesp=$(grep -c "part=$ESPG" "$W/list.txt")
ndisk=$(grep -c "disk=$DISKG" "$W/list.txt")
[ "$nsys" = "3" ] && ok "系统分区 GUID 命中 3 处" || ng "系统 GUID 命中 $nsys/3"
[ "$nesp" = "2" ] && ok "ESP GUID 命中 2 处" || ng "ESP GUID 命中 $nesp/2"
[ "$ndisk" = "5" ] && ok "磁盘 GUID 命中 5 处" || ng "磁盘 GUID 命中 $ndisk/5"

echo "════ 3. 4 参模式（只改 OS 设备，不动 ESP）════"
$T/imggenbcd "$REF" "$W/out2.bin" "$SYSG" "$DISKG" > "$W/gen2.log" 2>&1
rc=$?
[ $rc -eq 0 ] && [ -s "$W/out2.bin" ] && ok "4 参模式可用（rc=0）" || ng "4 参模式 rc=$rc"
"$PT" --list-guid "$W/out2.bin" > "$W/list2.txt" 2>/dev/null
[ "$(grep -c "part=$SYSG" "$W/list2.txt")" = "3" ] && ok "4 参模式：系统 GUID 已换（3 处）" || ng "4 参模式系统 GUID 不对"

echo "════ 4. 反用例：非法 GUID ════"
$T/imggenbcd "$REF" "$W/bad.bin" "not-a-guid" "$DISKG" "$ESPG" > "$W/bad.log" 2>&1
rc=$?
[ $rc -ne 0 ] && ok "非法系统 GUID → 非零退出（rc=$rc，不崩）" || ng "非法 GUID 竟然返回 0"

echo "════ 5. 反用例：参考件不是 BCD ════"
$T/imggenbcd /etc/services "$W/nope.bin" "$SYSG" "$DISKG" "$ESPG" > "$W/nope.log" 2>&1
rc=$?
[ $rc -ne 0 ] && ok "垃圾参考件 → 非零退出（rc=$rc）" || ng "垃圾参考件竟然返回 0"

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ $fail -eq 0 ]; then echo "✅ imggenbcd 回归通过"; rm -rf "$W"
else echo "❌ 有失败（调试目录 $W 保留）"; fi
[ $fail -eq 0 ]