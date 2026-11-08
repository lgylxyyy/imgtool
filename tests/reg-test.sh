#!/bin/bash
# reg-test.sh — 注册表通用编辑（P2）：reg.get / reg.set / reg.list / reg.del
#
# 素材：reference/bcd-reference-bios-win11.bin（BCD hive，格式与 SYSTEM 相同，
#       用作替代品避免引入真 Windows 注册表 —— 版权与体积都不可接受）
#
# 覆盖：
#   ① reg.list 根键 / 子键
#   ② reg.set/get DWORD 往返
#   ③ reg.set/get 字符串往返（含中文）
#   ④ reg.del 删值
#   ⑤ reg.del 非空键拒绝（REG-KEY-NOT-EMPTY）
#   ⑥ reg.del 递归删键
#   ⑦ 事务性：改完的 hive 能从分区读回且能重新打开
set -u
T="${IMGTOOL:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fail=0
ok(){ echo "  ✅ $1"; }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

IMG="$W/t.qcow2"; LBA=2048
echo "════ 0. 造盘 + 放 hive ════"
$T img.create --out="$IMG" --size-gb=1 --table=mbr --sys-mb=256 --win-mb=0 >/dev/null 2>&1 || { echo "造盘失败"; exit 2; }
$T fs.format --img="$IMG" --lba=$LBA --fs=ntfs >/dev/null 2>&1 || { echo "格式化失败"; exit 2; }
$T ntfs.mkdir --img="$IMG" --lba=$LBA --path=/Windows/System32/config >/dev/null 2>&1
if ! $T ntfs.put --img="$IMG" --lba=$LBA --src="$ROOT/reference/bcd-reference-bios-win11.bin" \
      --dst=/Windows/System32/config/SYSTEM >/dev/null 2>&1; then
  echo "放 hive 失败（缺 reference/*.bin？）"; exit 2
fi

echo "════ 1. reg.list 根键 ════"
n=$($T reg.list --img="$IMG" --lba=$LBA --path=SYSTEM 2>/dev/null | jq_ "d['data']['count']")
[ "${n:-0}" -ge 1 ] && ok "根键有 ${n} 项" || ng "根键列表为空"

echo "════ 2. reg.set / reg.get DWORD ════"
$T reg.set --img="$IMG" --lba=$LBA --path='SYSTEM\Setup\TestD' --type=dword --value=42 >/dev/null 2>&1
v=$($T reg.get --img="$IMG" --lba=$LBA --path='SYSTEM\Setup\TestD' 2>/dev/null | jq_ "d['data']['value']")
[ "$v" = "42" ] && ok "DWORD 往返 42" || ng "DWORD 往返失败（读到 $v）"

echo "════ 3. reg.set / reg.get 字符串（含中文）════"
$T reg.set --img="$IMG" --lba=$LBA --path='SYSTEM\Setup\TestS' --type=sz --value='中文 hello' >/dev/null 2>&1
v=$($T reg.get --img="$IMG" --lba=$LBA --path='SYSTEM\Setup\TestS' 2>/dev/null | jq_ "d['data']['value']")
[ "$v" = "中文 hello" ] && ok "字符串往返（中文）" || ng "字符串往返失败（读到 $v）"

echo "════ 4. reg.del 删值 ════"
$T reg.del --img="$IMG" --lba=$LBA --path='SYSTEM\Setup' --value=TestD >/dev/null 2>&1
r=$($T reg.get --img="$IMG" --lba=$LBA --path='SYSTEM\Setup\TestD' 2>/dev/null | jq_ "d['err']['error_code']")
[ "$r" = "REG-NOTFOUND" ] && ok "删值后读不到" || ng "删值失败（$r）"

echo "════ 5. reg.del 非空键拒绝 ════"
r=$($T reg.del --img="$IMG" --lba=$LBA --path='SYSTEM\Objects' 2>/dev/null | jq_ "d['err']['error_code']")
[ "$r" = "REG-KEY-NOT-EMPTY" ] && ok "非空键被拒" || ng "非空键应拒绝（$r）"

echo "════ 6. reg.del 递归删键 ════"
$T reg.set --img="$IMG" --lba=$LBA --path='SYSTEM\Tree\Sub\Val' --type=dword --value=1 >/dev/null 2>&1
$T reg.del --img="$IMG" --lba=$LBA --path='SYSTEM\Tree' --recursive=true >/dev/null 2>&1
r=$($T reg.get --img="$IMG" --lba=$LBA --path='SYSTEM\Tree\Sub\Val' 2>/dev/null | jq_ "d['err']['error_code']")
[ "$r" = "REG-NOTFOUND" ] && ok "递归删干净" || ng "递归删失败（$r）"

echo "════ 7. 事务性：改完的 hive 能读回 ════"
$T ntfs.export --img="$IMG" --lba=$LBA --path=/Windows/System32/config/SYSTEM --dst="$W/back.hive" >/dev/null 2>&1
if [ -s "$W/back.hive" ] && hivexsh "$W/back.hive" </dev/null >/dev/null 2>&1; then
  ok "hive 从分区读回且结构完好"
else
  ng "hive 读不回或结构损坏"
fi

echo
echo "════ 结果：通过 $((7-fail)) · 失败 $fail ════"
[ $fail -eq 0 ] || exit 1
