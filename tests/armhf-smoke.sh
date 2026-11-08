#!/bin/bash
# armhf-smoke.sh — 32 位（armhf）真编真跑冒烟
#
# 目的：关闭"arm32 只有 64 位验证过"的缺口。重点不是跑分，是抓：
#   · size_t/long 32 位下的截断（镜像与文件偏移全程 uint64_t）
#   · 分区起始 >2GiB / >4GiB 时的偏移计算
#   · NTFS 路径（libntfs-3g + 进程内 mkntfs 对象）在 32 位下能否格式化/写/读回
#
# 环境：Ubuntu 24.04 aarch64 宿主 + gcc-arm-linux-gnueabihf + qemu-user-static
#   构建见 ARMHF-BUILD.md；本脚本默认用 build-armhf/
#
# 用法：bash tests/armhf-smoke.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${BUILD:-$ROOT/build-armhf}
Q=${QEMU:-qemu-arm-static}
QP=${QPREFIX:-/usr/arm-linux-gnueabihf}
HT=${HOST_TOOLS:-$ROOT/../imgtool/build/tools}
T=$B/tools
W=$(mktemp -d /tmp/armhfXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
q(){ $Q -L "$QP" "$@"; }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

echo "════ 0. 前置检查 ════"
for x in "$Q" "$T/imglayout" "$T/imgmkfs" "$T/imgntfsfs" "$T/imgcmd" "$HT/imgfatfs" "$HT/imgntfsfs"; do
  [ -x "$(command -v "$x" 2>/dev/null || echo "$x")" ] || { echo "❌ 缺 $x"; exit 2; }
done
ok "armhf 工具与宿主复核工具齐全"

echo "════ 1. 6GiB GPT 盘（RE 分区起始 > 4GiB）════"
q "$T/imglayout" mk "$W/t.qcow2" $((6*1024*1024*1024)) gpt --esp 64 --msr 16 >/dev/null 2>&1 || { echo "建盘失败"; exit 2; }
q "$T/imgpart" show "$W/t.qcow2" > "$W/pt.json"
ESP=$(jq_ "[p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0]" < "$W/pt.json")
RE=$(jq_  "[p['first'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0]" < "$W/pt.json")
[ -n "$ESP" ] && [ -n "$RE" ] || { echo "取 LBA 失败"; exit 2; }
RE_OFF=$((RE*512))
[ "$RE_OFF" -gt 4294967296 ] && ok "RE LBA=$RE（偏移 $RE_OFF >4GiB）" || ng "RE 偏移 $RE_OFF 没超过 4GiB，测不到 64 位"

head -c 8388608 /dev/urandom > "$W/big.bin"
SRC_MD5=$(md5sum "$W/big.bin" | cut -d' ' -f1)

echo "════ 2. FAT32（ESP）写 8MiB → 宿主抽回对 md5 ════"
q "$T/imgmkfs" "$W/t.qcow2" --mkfs "$ESP" TEST >/dev/null 2>&1
q "$T/imgcmd" fat.put --img="$W/t.qcow2" --lba="$ESP" --src="$W/big.bin" --dst=/EFI/big.bin >/dev/null 2>&1
"$HT/imgfatfs" "$W/t.qcow2" "$ESP" export /EFI/big.bin "$W/fat.bin" >/dev/null 2>&1
[ "$(md5sum "$W/fat.bin" | cut -d' ' -f1)" = "$SRC_MD5" ] && ok "FAT32 md5 逐字节一致" || ng "FAT32 md5 不一致"

echo "════ 3. NTFS（RE，偏移 >4GiB）格式化 + 写 8MiB + 宿主抽回 ════"
q "$T/imgntfsfs" "$W/t.qcow2" "$RE" format RE6 >/dev/null 2>&1
[ $? -eq 0 ] && ok "arm32 进程内 mkntfs 格式化成功" || ng "NTFS 格式化失败"
q "$T/imgntfsfs" "$W/t.qcow2" "$RE" put "$W/big.bin" /big.bin >/dev/null 2>&1
[ $? -eq 0 ] && ok "arm32 NTFS 写入成功" || ng "NTFS 写入失败"
q "$T/imgntfsfs" "$W/t.qcow2" "$RE" ls / > "$W/ls.txt" 2>&1
grep -q "big.bin" "$W/ls.txt" && ok "上游 ntfsls 能看到 big.bin" || ng "ntfsls 看不到文件"
"$HT/imgntfsfs" "$W/t.qcow2" "$RE" export /big.bin "$W/ntfs.bin" >/dev/null 2>&1
[ "$(md5sum "$W/ntfs.bin" | cut -d' ' -f1)" = "$SRC_MD5" ] && ok "NTFS md5 逐字节一致（64 位偏移下）" || ng "NTFS md5 不一致"

echo "════ 4. 镜像完好性（宿主 qemu-img check）════"
if command -v qemu-img >/dev/null 2>&1; then
  qemu-img check "$W/t.qcow2" > "$W/qc.txt" 2>&1
  grep -q "No errors were found" "$W/qc.txt" && ok "qemu-img check 无错误" || { ng "qemu-img check 报错"; tail -5 "$W/qc.txt"; }
else
  echo "  ⏭ 跳过 qemu-img check（没装 qemu-utils）"
fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ $fail -eq 0 ]; then echo "✅ armhf（32 位）冒烟通过"; rm -rf "$W"
else echo "❌ 有失败（调试目录 $W 保留）"; fi
[ $fail -eq 0 ]