#!/bin/bash
# 二分定位：把 TEST-2 的改动拆成两半
#   TEST-5 = diskA + 恢复分区+文件，【BCD 不动】（只做 ESP 抽出→写回）
#   TEST-6 = diskA + 只改 BCD 加恢复条目，【不加恢复分区】
set -e
T=/workspace/imgtool/build/tools
SRC=/workspace/images/diskA-Windows-UEFI-GPT.qcow2
ISO=$(ls /workspace/images/*19044*.iso | head -1)

mkwinre(){  # $1=目标qcow2  $2=恢复分区LBA
  local D=$1 REC=$2 W=$(mktemp -d)
  "$T/imgntfsfs" "$D" "$REC" format 2>&1 | grep -q "成功"
  "$T/imgntfsfs" "$D" "$REC" mkdir "/Recovery/WindowsRE" >/dev/null 2>&1
  "$T/imgntfsfs" "$D" "$REC" put /workspace/build/probe/Winre.wim "/Recovery/WindowsRE/Winre.wim" >/dev/null 2>&1
  timeout 600 7z e -y "$ISO" -o"$W" "boot/boot.sdi" >/dev/null 2>&1
  "$T/imgntfsfs" "$D" "$REC" put "$W/boot.sdi" "/Recovery/WindowsRE/boot.sdi" >/dev/null 2>&1
  echo "$W"
}
esp_roundtrip(){  # $1=qcow2  $2=可选的新BCD  → 抽ESP，可选替换BCD，写回
  local D=$1 NEWBCD=$2 W=$(mktemp -d)
  python3 -c "
import subprocess
with open('$W/esp.raw','wb') as out:
    for i in range(204800//2048):
        off=(2048+i*2048)*512
        out.write(subprocess.run(['$T/imgdump','$D',str(off),str(2048*512)],capture_output=True).stdout)"
  if [ -n "$NEWBCD" ]; then mcopy -i "$W/esp.raw" -o "$NEWBCD" ::/EFI/Microsoft/Boot/BCD; fi
  "$T/imgraw" write "$D" $((2048*512)) "$W/esp.raw" >/dev/null
  echo "$W"
}

echo "══════ TEST-5：加恢复分区+文件，BCD 完全不动 ══════"
cp "$SRC" "/workspace/images/TEST-5-只加恢复分区-BCD不动.qcow2"
D5="/workspace/images/TEST-5-只加恢复分区-BCD不动.qcow2"
"$T/imgpart" add "$D5" 1300000 de94bba4-06d1-4d40-a16a-bfd50179d6ac Recovery | head -1 | sed 's/^/  /'
REC5=$("$T/imgpart" show "$D5" | python3 -c "import sys,json;d=json.loads(sys.stdin.read());print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
echo "  恢复分区 LBA=$REC5"
W5=$(mkwinre "$D5" "$REC5"); echo "  文件已写入"
esp_roundtrip "$D5" ""; echo "  ESP 抽出→原样写回（BCD 未改）"
IDX=$("$T/imgpart" show "$D5" | python3 -c "import sys,json;d=json.loads(sys.stdin.read());print([p['i'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
"$T/imgpart" attr "$D5" "$IDX" 0x8000000000000001 | sed 's/^/  /'
"$T/imgbc" "$D5" --uefi 2>/dev/null | tail -1 | sed 's/^/  bccheck: /'

echo
echo "══════ TEST-6：只改 BCD 加恢复条目，不加恢复分区 ══════"
D6="/workspace/images/TEST-6-只改BCD-不加恢复分区.qcow2"
cp "$SRC" "$D6"
W6=$(esp_roundtrip "$D6" "" )
mcopy -i "$W6/esp.raw" ::/EFI/Microsoft/Boot/BCD "$W6/bcd_orig.bin"
cp "$W6/bcd_orig.bin" "$W6/bcd_new.bin"
mkdir -p "$W6/iso"; timeout 600 7z e -y "$ISO" -o"$W6/iso" "efi/microsoft/boot/bcd" >/dev/null 2>&1
/workspace/imgtool/build/tests/recovery_bcd_test "$W6/bcd_orig.bin" "$W6/bcd_new.bin" '\Recovery\WindowsRE\Winre.wim' "$W6/iso/bcd" 2>/dev/null | tail -2 | sed 's/^/  /'
# 先写回空ESP（BCD 未动），再单独把新BCD打进去
python3 - <<PY
import subprocess
W="$W6"
# 用 mtools 把新 BCD 放进已抽出的 ESP，再整块写回
subprocess.run(['mcopy','-i',f'{W}/esp.raw','-o',f'{W}/bcd_new.bin','::/EFI/Microsoft/Boot/BCD'],check=True)
print("  新 BCD 已放入 ESP 镜像")
PY
"$T/imgraw" write "$D6" $((2048*512)) "$W6/esp.raw" >/dev/null
echo "  已写回"
"$T/imgbc" "$D6" --uefi 2>/dev/null | tail -1 | sed 's/^/  bccheck: /'
echo
ls -la /workspace/images/TEST-*.qcow2 | awk '{printf "  %-46s %6.2f GB\n",$9,$5/1073741824}'
