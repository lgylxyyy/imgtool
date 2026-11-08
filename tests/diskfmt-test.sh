#!/bin/bash
# diskfmt-test.sh — 磁盘格式三件套（P3）：raw / VHD / VMDK
#
# 需要 qemu-img（有则做双向验证；没有则跳过那两项）
set -u
T="${IMGTOOL:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fail=0
ok(){ echo "  ✅ $1"; }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
skip(){ echo "  ⏭️  $1"; }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }
have_qemu(){ command -v qemu-img >/dev/null 2>&1; }

echo "════ 0. 造 64MB raw 基准盘 + 分区表 + FAT32 ════"
dd if=/dev/zero of="$W/base.img" bs=1M count=64 2>/dev/null
# 64MB 分区表（一个 60MB 分区 @ 2048）
python3 - "$W/pt.json" <<'PY'
import json,sys
d={"kind":"icdq.parttable","v":1,"table":"MBR","sectorSize":512,
   "diskSectors":131072,"diskGuid":"","diskSignature":"0x12345678","mbrCode":None,
   "parts":[{"first":2048,"last":124927,"sectors":122880,"mbri":12,
             "active":True,"logical":False,"typeGuid":"","partGuid":"",
             "name":"","attributes":"0000000000000000"}]}
json.dump(d,open(sys.argv[1],'w'))
PY
$T part.apply --img="$W/base.img" --in="$W/pt.json" --force=true >/dev/null 2>&1 || { echo "分区表失败"; exit 2; }
$T fs.format --img="$W/base.img" --lba=2048 --fs=fat32 >/dev/null 2>&1 || { echo "格式化失败"; exit 2; }
echo "hello diskfmt" > "$W/f.txt"
$T fat.put --img="$W/base.img" --lba=2048 --src="$W/f.txt" --dst=/f.txt >/dev/null 2>&1
echo "  基准盘就绪"

echo
echo "════ 1. raw 后端 ════"
n=$($T img.info --img="$W/base.img" 2>/dev/null | jq_ "d['data']['partitions']")
[ "$n" = "1" ] && ok "raw：img.info 认出 1 个分区" || ng "raw：分区数 $n（应 1）"
v=$($T fat.ls --img="$W/base.img" --lba=2048 --path=/ 2>/dev/null | jq_ "d['data']['entries'][0]['name']")
[ "$v" = "f.txt" ] && ok "raw：FAT32 可读" || ng "raw：FAT32 读失败"

for PAIR in "vhd:vpc" "vmdk:vmdk"; do
  FMT="${PAIR%%:*}"; QFMT="${PAIR##*:}"
  echo
  echo "════ 2. $FMT 后端（qemu 格式名 $QFMT）════"
  if have_qemu; then
    qemu-img convert -f raw -O $QFMT "$W/base.img" "$W/t.$FMT" >/dev/null 2>&1
    if [ ! -s "$W/t.$FMT" ]; then ng "$FMT：qemu-img 转换失败"; continue; fi
    ok "$FMT：qemu-img 转出测试盘"
  else
    skip "$FMT：无 qemu-img，跳过"
    continue
  fi

  n=$($T img.info --img="$W/t.$FMT" 2>/dev/null | jq_ "d['data']['partitions']")
  [ "$n" = "1" ] && ok "$FMT：读出 1 个分区" || ng "$FMT：分区数 $n（应 1）"

  v=$($T fat.ls --img="$W/t.$FMT" --lba=2048 --path=/ 2>/dev/null | jq_ "d['data']['entries'][0]['name']")
  [ "$v" = "f.txt" ] && ok "$FMT：FAT32 可读" || ng "$FMT：FAT32 读失败（$v）"

  # 写入
  echo "written to $FMT" > "$W/w.txt"
  r=$($T fat.put --img="$W/t.$FMT" --lba=2048 --src="$W/w.txt" --dst=/w.txt 2>/dev/null | jq_ "d['ok']")
  [ "$r" = "True" ] && ok "$FMT：写入成功" || ng "$FMT：写入失败"

  # qemu 反向验证
  qemu-img convert -f $QFMT -O raw "$W/t.$FMT" "$W/back.raw" >/dev/null 2>&1
  if [ -s "$W/back.raw" ]; then
    a=$(od -An -tx1 -j $((2048*512)) -N 3 "$W/base.img" 2>/dev/null | tr -d ' ')
    b=$(od -An -tx1 -j $((2048*512)) -N 3 "$W/back.raw" 2>/dev/null | tr -d ' ')
    [ "$a" = "$b" ] && ok "$FMT：qemu 反向还原一致（VBR $a）" || ng "$FMT：VBR 不一致（$a vs $b）"
  else
    ng "$FMT：qemu 无法还原"
  fi
done

echo
echo "════ 结果：失败 $fail ════"
[ $fail -eq 0 ] || exit 1
