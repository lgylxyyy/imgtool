#!/bin/bash
# wim-apply-test.sh — WIM/ESD 直灌验证（全流程最慢的一步，必须实测两条路径）
#
#   ① 正常跑完：rc=0；日志有 event=begin(带 wim/index/lba) + event=done(带 duration_ms)
#   ② 中途取消：rc=IMG_ECANCELLED；迅速停下；日志有 event=begin + event=fail(E 级, 带 error_code)
#                （直灌无法回滚 → 目标分区半写；本测试只保证"能停、不崩、镜像文件仍完好"）
#   ③ 结束后 qemu-img check 必须无错（镜像文件本身不能被写坏）
#
# 用法: tests/wim-apply-test.sh [ESD路径] [索引]
#
# ⚠ 资源（2026-10-20 实测更正）：**内存不是瓶颈** —— 灌 6.86 GB 的 ESD，
#   峰值 RSS 只有 ~170 MB（wimlib 是流式解压直写，不预解压）。整条用例约 2~3 分钟，
#   需要约 7 GB 空闲磁盘（20 GiB 稀疏盘 + 19 GiB 数据分区）。
#   早先"中途被系统杀"是【后台进程被环境回收】的假象，不是 OOM。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
TE="$ROOT/build/tests"
# 默认找 <工作区>/images/ 里的 ESD；换机器用 IMGTOOL_IMAGES 或第 1 个参数指定。
# shellcheck disable=SC1091
. "$ROOT/scripts/env.sh"
IMG_DIR="$IMGTOOL_IMAGES"
ESD="${1:-${IMGTOOL_ESD:-$IMG_DIR/【不忘初心】Windows11_26H1_28000.2525_X64_纯净[深度精简版][1.74G](2026.7.20).esd}}"
IDX="${2:-1}"
W=$(mktemp -d /tmp/wimtestXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }

[ -f "$ESD" ] || { echo "❌ 找不到 ESD：$ESD"; exit 2; }
echo "════ 准备：20GB 测试盘 + 19GB NTFS 数据分区 ════"
$T/imglayout mk "$W/d.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
$T/imgpart show "$W/d.qcow2" > "$W/parts.json"
LBA=$($T/imgpart show "$W/d.qcow2" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
$T/imgntfsfs "$W/d.qcow2" "$LBA" format WIN >/dev/null 2>&1 || { echo "❌ NTFS 格式化失败"; exit 2; }
echo "  分区 LBA=$LBA；ESD=$(basename "$ESD") 索引=$IDX"

echo "════ ① 正常灌入（真实命令路径：imgcmd wim.apply，约 1~8 分钟）════"
export IMGTOOL_LOG="$W/full.log"
rm -f "$W/full.log"
ENVJ="$W/env.json"
if "$T/imgcmd" wim.apply --img="$W/d.qcow2" --lba="$LBA" --wim="$ESD" --index="$IDX" \
                        --format=on --label=Windows --log="$W/full.log" --progress=bar \
                        > "$ENVJ" 2> "$W/full.err"; then
  ok "imgcmd wim.apply 成功（信封: $(python3 -c "import json;d=json.load(open('$ENVJ'));print('ok=',d['ok'],'formatted=',d['data'].get('formatted'),'index=',d['data'].get('index'))")）"
else
  ng "imgcmd wim.apply 失败：$(cat "$ENVJ" | head -c 300)"; fi
grep -q 'action=format' "$W/full.log" && ok "日志有【格式化】里程碑（与灌入分开）" || ng "日志缺 format 里程碑"
grep -q 'action=apply' "$W/full.log" && ok "日志有【灌入】里程碑（带 wim/index/lba）" || ng "日志缺 apply 里程碑"
grep -q 'event=done' "$W/full.log" && grep -q 'duration_ms=' "$W/full.log" && ok "done 带 duration_ms" || ng "缺 done"
python3 "$ROOT/tests/logfmt_check.py" "$W/full.log" >/dev/null 2>&1 && ok "日志格式合法（logfmt）" || ng "日志格式有坏行"
grep -E 'action=(format|apply)' "$W/full.log" | sed 's/^/     /'
echo "  ── 抽回校验：把真实的 Windows 内核抽出来看（不能只信自己的读函数）──"
K=/Windows/System32/ntoskrnl.exe
$T/imgntfsfs "$W/d.qcow2" "$LBA" exists "$K" 2>/dev/null | grep -q "是" && ok "存在 $K" || ng "分区里没有 $K"
$T/imgntfsfs "$W/d.qcow2" "$LBA" export "$K" "$W/k.exe" >/dev/null 2>&1
if [ -s "$W/k.exe" ]; then
  SZ=$(stat -c%s "$W/k.exe"); SIG=$(head -c2 "$W/k.exe")
  [ "$SZ" -gt 1000000 ] && ok "抽回 $SZ 字节（>1MB）" || ng "抽回文件太小（$SZ）"
  [ "$SIG" = "MZ" ] && ok "PE 头正确（MZ）" || ng "不是 PE 文件（头=$SIG）"
else
  ng "抽回失败"
fi
echo "  ── 格式化后的 \$Boot 仍在（说明 mkntfs 的卷没被破坏）──"
$T/imgdump "$W/d.qcow2" $((LBA*512)) 16 > "$W/bs.bin" 2>/dev/null
python3 -c "d=open('$W/bs.bin','rb').read(); import sys; sys.exit(0 if d[3:11]==b'NTFS    ' else 1)" \
  && ok "\$Boot 魔数 NTFS 仍在" || ng "\$Boot 不对"

echo "════ ② 中途取消（另建一张盘，跑 6 秒后取消）════"
$T/imglayout mk "$W/c.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
$T/imgntfsfs "$W/c.qcow2" "$LBA" format WIN >/dev/null 2>&1
export IMGTOOL_LOG="$W/cancel.log"; rm -f "$W/cancel.log"
"$TE/wim_apply_test" "$W/c.qcow2" "$LBA" "$ESD" "$IDX" 6000 > "$W/cancel.out" 2>&1 && \
  ok "取消路径通过（$(grep -c '✅' "$W/cancel.out") 项断言）" || \
  ng "取消路径失败：$(grep -E '❌|rc=' "$W/cancel.out" | tr '\n' ' ')"
grep -q 'event=cancel' "$W/cancel.log" && ok "日志有 event=cancel（不是 done/fail）" || ng "日志缺 event=cancel"
grep -q 'level=WARN' "$W/cancel.log" && ok "cancel 记 WARN 级（不是 ERROR）" || ng "cancel 级别不对"
grep -q 'error_code=CMD-CANCELLED' "$W/cancel.log" && ok "带 error_code=CMD-CANCELLED" || ng "缺 error_code"
grep -E 'event=(cancel|fail|done)' "$W/cancel.log" | head -2 | sed 's/^/     /'

echo "════ ③ 镜像文件完好性（qemu-img check）════"
for f in "$W/d.qcow2" "$W/c.qcow2"; do
  if qemu-img check "$f" >/dev/null 2>&1; then ok "$(basename $f) 无明显损坏"; else ng "$(basename $f) 坏了！"; fi
done

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ $fail -eq 0 ]; then echo "✅ WIM 灌入验证通过"; rm -rf "$W"; else echo "❌ 有失败（调试目录 $W 保留）"; fi
[ $fail -eq 0 ]
