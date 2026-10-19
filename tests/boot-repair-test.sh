#!/bin/bash
# boot-repair-test.sh — boot.repair 一键修复（BIOS）回归
#
# 覆盖：
#   ① 只有诊断：ok=false / fixed_count=0（不许假装修好了）
#   ② 完整修复：MBR 码 + 签名 + VBR + BCD → ok=true / 四项 verify 全 true
#   ③ MBR 头签名真的落盘（独立用 imgdump + od 验证）
#   ④ BCD 里 5 处设备对象全改：2 处指 SR、3 处指系统分区（不是只改 3 处）
#   ⑤ 幂等：再跑一次仍 ok=true
#   ⑥ 反用例：拿非 BCD 文件当 --bcd-ref → BCD-REF-BAD，不许崩
#
# 布局是确定的：SR=2048（64MiB），系统=133120（=2048+64MiB/512）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
PT="$ROOT/build/tests/mbr_bcd_test"
W=$(mktemp -d /tmp/bootrepairXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }
IMG="$W/bios.qcow2"; SR=2048; SYS=133120

echo "════ 0. 造一张 3GiB MBR 盘（SR 64MiB + 系统 1GiB）════"
$T/imglayout mk "$IMG" $((3*1024*1024*1024)) mbr >/dev/null 2>&1 &&
$T/imgcmd part.layout --img="$IMG" --sr-mb=64 --sys-gb=1 --progress=none >/dev/null 2>&1 &&
$T/imgntfsfs "$IMG" $SR format SYSTEM >/dev/null 2>&1 &&
$T/imgntfsfs "$IMG" $SYS format WIN >/dev/null 2>&1 &&
$T/imgntfsfs "$IMG" $SR put "$ROOT/reference/bcd-reference-bios-win11.bin" /Boot/BCD >/dev/null 2>&1 \
  || { echo "造盘失败"; exit 2; }
echo "  盘=$IMG"

echo "════ 1. 只诊断：ok=false / fixed_count=0 ════"
$T/imgcmd boot.repair --img="$IMG" --sys-lba=$SYS --sr-lba=$SR > "$W/diag.json" 2>/dev/null
okflag=$(jq_ "d['data']['ok']" < "$W/diag.json")
fixed=$(jq_ "d['data']['fixed_count']" < "$W/diag.json")
mcode=$(jq_ "d['data']['diag.mbr_code_ok']" < "$W/diag.json")
bcdok=$(jq_ "d['data']['diag.bcd_ok']" < "$W/diag.json")
[ "$okflag" = "False" ] && ok "诊断：ok=false（没修就不许说好）" || ng "诊断 ok=$okflag（应为 False）"
[ "$fixed" = "0" ] && ok "诊断：fixed_count=0" || ng "诊断 fixed_count=$fixed"
[ "$mcode" = "False" ] && ok "诊断：mbr_code_ok=false（还没写引导码）" || ng "诊断 mbr_code_ok=$mcode"
[ "$bcdok" = "True" ] && ok "诊断：bcd_ok=true（参考 BCD 已在 SR）" || ng "诊断 bcd_ok=$bcdok"

echo "════ 2. 完整修复 ════"
$T/imgcmd boot.repair --img="$IMG" --sys-lba=$SYS --sr-lba=$SR --sig=11223344 \
  --mbr-code="$ROOT/reference/mbr-code-bios.bin" \
  --vbr-tpl="$IMG:$SYS" \
  --bcd-ref="$ROOT/reference/bcd-reference-bios-win11.bin" > "$W/fix.json" 2>/dev/null
rc=$?
[ $rc -eq 0 ] && ok "命令退出码 0" || ng "退出码 $rc"
[ "$(jq_ "d['ok']" < "$W/fix.json")" = "True" ] && ok "ok=true" || ng "ok 不是 true"
[ "$(jq_ "d['data']['fixed_count']" < "$W/fix.json")" = "3" ] && ok "fixed_count=3（MBR/VBR/BCD）" || ng "fixed_count=$(jq_ "d['data']['fixed_count']" < "$W/fix.json")"
for k in mbr_sig_ok mbr_code_ok vbr_ntfs_ok bcd_ok; do
  v=$(jq_ "d['data']['verify.$k']" < "$W/fix.json")
  [ "$v" = "True" ] && ok "verify.$k=true" || ng "verify.$k=$v"
done
[ "$(jq_ "d['data']['bcd.bindings_patched']" < "$W/fix.json")" = "5" ] \
  && ok "BCD 批量改写 5 处（含 memtest/resume，不只 3 处）" \
  || ng "bcd.bindings_patched=$(jq_ "d['data']['bcd.bindings_patched']" < "$W/fix.json")"

echo "════ 3. 独立验证：MBR 签名真的在盘上 ════"
sig=$($T/imgdump "$IMG" 440 4 | od -An -tx1 | tr -d ' \n')
[ "$sig" = "44332211" ] && ok "MBR @440 = 44 33 22 11（LE 0x11223344）" || ng "MBR 签名=$sig"

echo "════ 4. 独立验证：BCD 设备对象逐个检查 ════"
$T/imgntfsfs "$IMG" $SR export /Boot/BCD "$W/bcd.bin" >/dev/null 2>&1
[ -s "$W/bcd.bin" ] || { ng "BCD 导出失败"; }
"$PT" --list "$W/bcd.bin" > "$W/bind.txt" 2>/dev/null
n=$(( $(grep -c '^' "$W/bind.txt") - 1 ))
[ "$n" = "5" ] && ok "BCD 里 5 个 type=6 设备对象" || ng "设备对象数=$n（应为 5）"
nbad=$(grep -c 'sig=11223344' "$W/bind.txt")
[ "$nbad" = "5" ] && ok "5 处签名全部 =11223344" || ng "签名命中 $nbad/5"
nsr=$(grep -c "off=$((SR*512))" "$W/bind.txt")
nsys=$(grep -c "off=$((SYS*512))" "$W/bind.txt")
[ "$nsr" = "2" ] && ok "2 处指 SR（bootmgr + memtest，off=$((SR*512))）" || ng "指 SR 的=$nsr（应为 2）"
[ "$nsys" = "3" ] && ok "3 处指系统分区（resume + OSLoader 两个设备，off=$((SYS*512))）" || ng "指系统的=$nsys（应为 3）"

echo "════ 5. 幂等：再跑一次 ════"
$T/imgcmd boot.repair --img="$IMG" --sys-lba=$SYS --sr-lba=$SR --sig=11223344 \
  --mbr-code="$ROOT/reference/mbr-code-bios.bin" \
  --vbr-tpl="$IMG:$SYS" \
  --bcd-ref="$ROOT/reference/bcd-reference-bios-win11.bin" > "$W/fix2.json" 2>/dev/null
[ "$(jq_ "d['ok']" < "$W/fix2.json")" = "True" ] && ok "第二次仍 ok=true" || ng "第二次 ok=$(jq_ "d['ok']" < "$W/fix2.json")"

echo "════ 6. 反用例：--bcd-ref 传垃圾文件 ════"
$T/imgcmd boot.repair --img="$IMG" --sys-lba=$SYS --sr-lba=$SR \
  --bcd-ref=/etc/services --progress=bar > "$W/bad.json" 2>/dev/null
rc=$?
code=$(jq_ "d['err']['error_code']" < "$W/bad.json")
[ $rc -ne 0 ] && [ "$code" = "BCD-REF-BAD" ] && ok "垃圾 BCD → BCD-REF-BAD（rc=$rc，不崩）" \
  || ng "垃圾 BCD → rc=$rc code=$code"

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ $fail -eq 0 ]; then echo "✅ boot.repair 回归通过"; rm -rf "$W"
else echo "❌ 有失败（调试目录 $W 保留）"; fi
[ $fail -eq 0 ]