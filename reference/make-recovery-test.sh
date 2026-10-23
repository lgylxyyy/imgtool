#!/bin/bash
# 在 UEFI 真值素材盘基础上（原 diskA 已丢失，见 docs/清理记录…§1.5）加【恢复分区】并打【BCD 恢复条目】→ 供 QEMU 实测
# 用法: make-recovery-test.sh <源qcow2> <输出qcow2>
set -e
# diskA 已丢失（2026-10-23 误删，见 docs/清理记录-2026-10-23.md §1.5）⇒ 默认换成幸存的
# 真值 UEFI 盘；第一参数或环境变量 SRC 都可覆盖。
SRC="${1:-${SRC:-/workspace/images/Windows 11 UEFI.qcow2}}"
DST="${2:-/workspace/images/TEST-2-带恢复分区-预期0xc0000098.qcow2}"
T=/workspace/imgtool/build/tools
ISO=$(ls /workspace/images/*19044*.iso | head -1)
WORK=$(mktemp -d)
echo "  工作目录: $WORK"

echo "════ ① 复制源盘 ════"
cp "$SRC" "$DST"; echo "  ✓ $(du -h "$DST" | cut -f1)"

echo "════ ② 尾部加恢复分区 (de94bba4) ════"
"$T/imgpart" add "$DST" 1300000 de94bba4-06d1-4d40-a16a-bfd50179d6ac Recovery | head -1 | sed 's/^/  /'
REC=$("$T/imgpart" show "$DST" | python3 -c "
import sys,json; d=json.loads(sys.stdin.read())
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
echo "  恢复分区 LBA=$REC"

echo "════ ③ 格式化 NTFS + 建标准目录 ════"
"$T/imgntfsfs" "$DST" "$REC" format 2>&1 | grep "格式化" | sed 's/^/  /'
"$T/imgntfsfs" "$DST" "$REC" mkdir "/Recovery/WindowsRE" 2>&1 | grep "mkdir" | sed 's/^/  /'

echo "════ ④ 取 boot.sdi + 写入 winre.wim / boot.sdi ════"
if [ ! -f "$WORK/boot.sdi" ]; then
  mkdir -p "$WORK"; timeout 600 7z e -y "$ISO" -o"$WORK" "boot/boot.sdi" >/dev/null 2>&1
fi
"$T/imgntfsfs" "$DST" "$REC" put /workspace/build/probe/Winre.wim "/Recovery/WindowsRE/Winre.wim" 2>&1 | grep -E "put|大小" | sed 's/^/  /'
"$T/imgntfsfs" "$DST" "$REC" put "$WORK/boot.sdi" "/Recovery/WindowsRE/boot.sdi" 2>&1 | grep -E "put|大小" | sed 's/^/  /'

echo "════ ⑤ GPT 属性位 0x8000000000000001 ════"
IDX=$("$T/imgpart" show "$DST" | python3 -c "
import sys,json; d=json.loads(sys.stdin.read())
print([p['i'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
"$T/imgpart" attr "$DST" "$IDX" 0x8000000000000001 | sed 's/^/  /'

echo "════ ⑥ 抽 ESP → 打 BCD 恢复条目 → 写回 ════"
python3 -c "
import subprocess
with open('$WORK/esp.raw','wb') as out:
    for i in range(204800//2048):
        off=(2048+i*2048)*512
        out.write(subprocess.run(['$T/imgdump','$DST',str(off),str(2048*512)],capture_output=True).stdout)
print('  抽出 ESP 100MB')"
mcopy -i "$WORK/esp.raw" ::/EFI/Microsoft/Boot/BCD "$WORK/bcd_orig.bin"
# 需要官方 ISO 的 BCD 作模板（复制 ramdiskoptions + WinPE loader）
mkdir -p "$WORK/iso" && timeout 600 7z e -y "$ISO" -o"$WORK/iso" "efi/microsoft/boot/bcd" >/dev/null 2>&1
cp "$WORK/bcd_orig.bin" "$WORK/bcd_new.bin"
# ★ BCD 设备路径必须用反斜杠（Windows 惯例）；正斜杠会让 bootmgr 解析失败
  # ★ BCD 设备路径必须用反斜杠；且 ramdisksdidevice 必须显式指向【恢复分区】
  #   （否则 bootmgr 会去 BCD 所在的 ESP 找 wim → 0xc0000098）
  PARTGUID=$("$T/imgpart" show "$DST" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['partGuid'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
  DISKGUID=$("$T/imgpart" show "$DST" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read());print(d['diskGuid'])")
  echo "  恢复分区 GUID = $PARTGUID"
  echo "  磁盘 GUID     = $DISKGUID"
  /workspace/imgtool/build/tests/recovery_bcd_test "$WORK/bcd_orig.bin" "$WORK/bcd_new.bin" '\Recovery\WindowsRE\Winre.wim' "$WORK/iso/bcd" "$PARTGUID" "$DISKGUID" 2>/dev/null | tail -8 | sed 's/^/  /'
mcopy -i "$WORK/esp.raw" -o "$WORK/bcd_new.bin" ::/EFI/Microsoft/Boot/BCD
"$T/imgraw" write "$DST" $((2048*512)) "$WORK/esp.raw"

echo "════ ⑦ bccheck 验收 ════"
"$T/imgbc" "$DST" --uefi 2>/dev/null > "$WORK/bc.json" || true
python3 - <<'PY'
import json,os,glob
p=glob.glob('/tmp/tmp*/bc.json')+glob.glob('/var/tmp/tmp*/bc.json')
f=None
for c in p:
    try:
        if os.path.getmtime(c)>0: f=c
    except: pass
PY
tail -1 "$WORK/bc.json" | sed 's/^/  /'
echo "  ── 恢复项 ──"
grep '"id":"rec' "$WORK/bc.json" | python3 -c "
import sys,json
for l in sys.stdin:
    o=json.loads(l); print(f\"    {'✅' if o['pass'] else '❌'} {o['name']:<26} {o['detail']}\")"
echo
echo "  ✅ 产出: $DST"
