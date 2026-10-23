#!/bin/bash
# 端到端建盘流水线：layout → format → WIM灌入 → fixboot → ESP+BCD → bccheck
# 用法: build-image.sh <wim> <index> <out.qcow2> [diskGB] [--bios]
set -e
WIM="$1"; IDX="${2:-1}"; OUT="$3"; DISKGB="${4:-32}"; BIOS="${5:-}"
T=/workspace/imgtool/build/tools
ISO=$(ls /workspace/images/*19044*.iso | head -1)
R=/workspace/imgtool/reference
rm -f "$OUT"
echo "════ ① 分区布局 (GPT) ════"
$T/imglayout mk "$OUT" $((DISKGB*1024*1024*1024)) gpt --esp 512 --msr 16 | tail -1 | sed 's/^/  /'
show(){ $T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
for p in d['parts']: print(f\"     [{p['i']}] LBA {p['first']:>9} {p['sectors']*512//1048576:>5}MB {p['typeGuid'][:8]}\")
print('     diskGuid='+d['diskGuid'])"; }
show

echo "════ ② 格式化系统分区 NTFS ════"
OSLBA=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
OSSIZE=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['sectors']*512//1048576 for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
echo "  LBA=$OSLBA  ${OSSIZE}MB"
/tmp/fmt "$OUT" "$OSLBA" "$OSSIZE" 2>&1 | tail -1 | sed 's/^/  /'

echo "════ ③ 灌入 WIM (镜像 #$IDX) ════"
cat > /tmp/pl.c <<'EOF'
#include "img_wim_apply.h"
#include "img_qcow2.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc,char**argv){
    uint64_t lba=strtoull(argv[2],NULL,0);
    img_io_opts_t io={0};io.writable=true;
    img_io*f=img_io_open_path(argv[1],&io); if(!f)return 1;
    img_qcow2_opts_t qo={0}; img_qcow2*q=img_qcow2_open(f,&qo);
    if(!q||img_qcow2_write_init(q)!=IMG_OK)return 1;
    clock_t t0=clock();
    int rc=img_wim_apply(argv[3],atoi(argv[4]),q,lba,NULL,NULL);
    printf("  %s  耗时 %.1f 秒\n", img_strerror(rc), (double)(clock()-t0)/CLOCKS_PER_SEC);
    return rc==IMG_OK?0:1;
}
EOF
gcc -std=gnu11 -O2 -I/workspace/imgtool/include -DIMG_HAVE_ZLIB=1 -o /tmp/pl /tmp/pl.c /workspace/imgtool/build/libimgtool.a -lwim -lntfs-3g -lz 2>/dev/null
/tmp/pl "$OUT" "$OSLBA" "$WIM" "$IDX"

echo "════ ④ 引导代码 ($Boot 0..9) ════"
if [ -n "$BIOS" ]; then
  $T/imgfixboot "$OUT" "$OSLBA" --tpl "/workspace/images/Windows 10 Xlite.qcow2" 2048 | sed 's/^/  /'
else
  echo "  UEFI 模式跳过（不依赖 NTFS 引导码）"
fi

echo "════ ⑤ ESP：格式化 FAT32 + 注入 bootmgfw.efi + 生成 BCD ════"
W=$(mktemp -d)
# ★ 必须先格式化 ESP —— imglayout 只建分区不建文件系统，
#   不格式化的话后面 mcopy 报 'non DOS media'
ESPLBA=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
$T/imgmkfs "$OUT" --mkfs "$ESPLBA" "EFI\ SYSTEM" 2>&1 | tail -1 | sed 's/^/  /' 
ESPSIZE=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['sectors']*512 for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
python3 -c "
import subprocess
ESPLBA=$ESPLBA; ESPSIZE=$ESPSIZE
with open('$W/esp.raw','wb') as out:
    off=ESPLBA*512; left=ESPSIZE
    while left>0:
        n=min(left, 2*1024*1024)
        out.write(subprocess.run(['$T/imgdump','$OUT',str(off),str(n)],capture_output=True).stdout)
        off+=n; left-=n
print('  抽出 ESP', ESPSIZE//1048576, 'MB')"
# 从参考镜像抽 ESP 文件（bootmgfw.efi / memtest 等）
python3 -c "
import subprocess
with open('$W/src_esp.raw','wb') as out:
    for i in range(204800//2048):
        off=(2048+i*2048)*512
        out.write(subprocess.run(['$T/imgdump','${REFIMG:-/workspace/images/Windows 11 UEFI.qcow2}',str(off),str(2048*512)],capture_output=True).stdout)"
for p in "/EFI/Microsoft/Boot/bootmgfw.efi" "/EFI/Boot/bootx64.efi"; do
  mcopy -n -i "$W/src_esp.raw" "::$p" -o 2>/dev/null && echo "  复制 $p" || true
done
# 用参考 BCD 模板 + 按【新盘】的分区/磁盘 GUID 改写 device 对象
# 否则 bootmgr 解析不到设备 → 0xc0000098（踩过的坑）
mcopy -n -i "$W/src_esp.raw" ::/EFI/Microsoft/Boot/BCD "$W/bcd_ref.bin" 2>/dev/null
PARTGUID=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['partGuid'] for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
DISKGUID=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read());print(d['diskGuid'])")
echo "  新盘 分区GUID=$PARTGUID"
echo "       磁盘GUID=$DISKGUID"
$T/imggenbcd "$W/bcd_ref.bin" "$W/bcd_new.bin" "$PARTGUID" "$DISKGUID" 2>&1 | sed 's/^/  /'
# ★ 新 ESP 是空 FAT32，必须先建目录树（否则 mcopy 报 no match for target）
mmd -i "$W/esp.raw" ::/EFI 2>/dev/null || true
mmd -i "$W/esp.raw" ::/EFI/Boot 2>/dev/null || true
mmd -i "$W/esp.raw" ::/EFI/Microsoft 2>/dev/null || true
mmd -i "$W/esp.raw" ::/EFI/Microsoft/Boot 2>/dev/null || true
mmd -i "$W/esp.raw" ::/EFI/Microsoft/Boot/Fonts 2>/dev/null || true
mcopy -o -i "$W/esp.raw" "$W/bcd_new.bin" ::/EFI/Microsoft/Boot/BCD
echo "  ✅ BCD 已写入 ESP"
mcopy -o -i "$W/src_esp.raw" ::/EFI/Microsoft/Boot/bootmgfw.efi "$W/bootmgfw.efi" 2>/dev/null || true
mcopy -o -i "$W/esp.raw" "$W/bootmgfw.efi" ::/EFI/Microsoft/Boot/bootmgfw.efi 2>/dev/null || true
mcopy -o -i "$W/esp.raw" "$W/bootmgfw.efi" ::/EFI/Boot/bootx64.efi 2>/dev/null || true
$T/imgraw write "$OUT" $((2048*512)) "$W/esp.raw" | sed 's/^/  /' 

echo "════ ⑥ 恢复分区：标准目录 + winre.wim/boot.sdi + GPT 属性 ════"
REC=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
import json as j
try:
    print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])
except: print('')")
if [ -n "$REC" ]; then
  RECIDX=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['i'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
  $T/imgpart attr "$OUT" "$RECIDX" 0x8000000000000001 | sed 's/^/  /'
  /tmp/fmt "$OUT" "$REC" 1024 2>&1 | tail -1 | sed 's/^/  /'
  $T/imgntfsfs "$OUT" "$REC" mkdir "/Recovery/WindowsRE" 2>&1 | grep mkdir | sed 's/^/  /'
  if [ -f /workspace/build/probe/Winre.wim ]; then
    $T/imgntfsfs "$OUT" "$REC" put /workspace/build/probe/Winre.wim "/Recovery/WindowsRE/Winre.wim" 2>&1 | grep -E "put|大小" | sed 's/^/  /'
  fi
  [ -f "$W/boot.sdi" ] || timeout 600 7z e -y "$ISO" -o"$W" "boot/boot.sdi" >/dev/null 2>&1
  if [ -f "$W/boot.sdi" ]; then
    $T/imgntfsfs "$OUT" "$REC" put "$W/boot.sdi" "/Recovery/WindowsRE/boot.sdi" 2>&1 | grep -E "put|大小" | sed 's/^/  /'
  fi
  # 还要在 BCD 里加恢复条目（指向恢复分区）
  PARTG=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read())
print([p['partGuid'] for p in d['parts'] if p['typeGuid'].startswith('de94bba4')][0])")
  DISKG=$($T/imgpart show "$OUT" | python3 -c "
import sys,json;d=json.loads(sys.stdin.read());print(d['diskGuid'])")
  # ★ 先解官方 ISO 的 BCD 当【模板】再判存在 —— 顺序反了的话 if 永远不成立
  mkdir -p "$W/iso"
  [ -f "$W/iso/bcd" ] || timeout 600 7z e -y "$ISO" -o"$W/iso" "efi/microsoft/boot/bcd" >/dev/null 2>&1
  mcopy -i "$W/esp.raw" ::/EFI/Microsoft/Boot/BCD "$W/bcd2.bin" 2>/dev/null || true
  if [ -f "$W/bcd2.bin" ] && [ -f "$W/iso/bcd" ]; then
    /workspace/imgtool/build/tests/recovery_bcd_test "$W/bcd2.bin" "$W/bcd2n.bin" '\Recovery\WindowsRE\Winre.wim' "$W/iso/bcd" "$PARTG" "$DISKG" 2>/dev/null | tail -3 | sed 's/^/  /'
    mcopy -o -i "$W/esp.raw" "$W/bcd2n.bin" ::/EFI/Microsoft/Boot/BCD 2>/dev/null && echo "  ✅ 恢复条目已写入"
    $T/imgraw write "$OUT" $((2048*512)) "$W/esp.raw" >/dev/null && echo "  ✅ ESP 已回写"
  fi
else
  echo "  (无恢复分区，跳过)"
fi

echo "════ ⑦ WinRE 出厂自动启用（SetupComplete.cmd → 首次开机自动 reagentc /enable）════"
# ★ 官方机制：reagentc /enable 按【目标机器实际分区号】写 ReAgent.xml + BCD 恢复序列，
#   所以不离线写死 GLOBALROOT 路径（换机分区号会变）→ 让 Windows 首次开机自己跑。
"$T/imgcmd" winre.auto --img="$OUT" --lba="$OSLBA" --progress=bar

echo "════ ⑧ bccheck ════"
$T/imgbc "$OUT" --uefi 2>/dev/null | tail -1 | sed 's/^/  /'
echo
echo "  ✅ 产出: $OUT  ($(du -h "$OUT"|cut -f1))"
echo "  工作目录(调试): $W"
