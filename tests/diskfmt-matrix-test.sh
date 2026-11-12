#!/bin/bash
# diskfmt-matrix-test.sh — 四格式矩阵：建盘 → 写 → 跨进程读 → qemu 互验，一张表出结果
# （收尾计划 §3；纯整合 img.create / part.apply / fs.format / fat.put / fat.ls / qemu-img，无新逻辑）
#
# 与 diskfmt-test.sh 的分工：那边是「raw 基准盘 → qemu convert 出 vhd/vmdk」；
# 这里是「img.create 直接建四格式」→ 每格式独立走完整流水线 → qemu 双向互验：
#   ① qemu-img check 我们建的盘（raw 不支持 check，显示 —）
#   ② qemu-img convert 回 raw 后再用我们的 fat.ls 读 —— 跨工具互认
set -u
T="${IMGTOOL:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fail=0
have_qemu(){ command -v qemu-img >/dev/null 2>&1; }

# 1 GiB 盘的 MBR 分区表（一个 1022 MiB 分区 @ 2048）
python3 - "$W/pt.json" <<'PY'
import json,sys
d={"kind":"icdq.parttable","v":1,"table":"MBR","sectorSize":512,
   "diskSectors":2097152,"diskGuid":"","diskSignature":"0x12345678","mbrCode":None,
   "parts":[{"first":2048,"last":2095103,"sectors":2093056,"mbri":12,
             "active":True,"logical":False,"typeGuid":"","partGuid":"",
             "name":"","attributes":"0000000000000000"}]}
json.dump(d,open(sys.argv[1],'w'))
PY
echo "hello matrix" > "$W/f.txt"

jq_name(){ python3 -c "import sys,json;d=json.load(sys.stdin);print(d['data']['entries'][0]['name'])" 2>/dev/null; }

echo "════ 四格式矩阵（raw / vhd / vmdk / qcow2）：建盘 → 写 → 跨进程读 → qemu 互验 ════"
printf '%-7s %-4s %-4s %-4s %-4s %-9s %-8s\n' 格式 建盘 信息 写 读 check 回读

for PAIR in raw:raw vhd:vpc vmdk:vmdk qcow2:qcow2; do
  FMT="${PAIR%%:*}"; QFMT="${PAIR##*:}"
  P="$W/m.$FMT"
  cell(){ [ "$1" = 0 ] && printf '✓  ' || { printf '✗  '; fail=$((fail+1)); }; }

  printf '%-7s' "$FMT"
  # ① 建盘
  $T img.create --out="$P" --size-gb=1 --format=$FMT --table=none --limits=off >/dev/null 2>&1; cell $?
  # ② 跨进程读（img.info 是独立进程）
  $T img.info --img="$P" >/dev/null 2>&1; cell $?
  # ③ 写：分区表 + FAT32 格式化 + fat.put
  if $T part.apply --img="$P" --in="$W/pt.json" --force=true >/dev/null 2>&1 \
     && $T fs.format --img="$P" --lba=2048 --fs=fat32 >/dev/null 2>&1 \
     && $T fat.put --img="$P" --lba=2048 --src="$W/f.txt" --dst=/f.txt >/dev/null 2>&1; then
    cell 0
  else
    cell 1
  fi
  # ④ 读回（又一个独立进程）
  v=$($T fat.ls --img="$P" --lba=2048 --path=/ 2>/dev/null | jq_name)
  [ "$v" = "f.txt" ]; cell $?
  # ⑤ qemu-img check 我们建的盘（raw / vhd(vpc) 不支持 check → —，互验靠 ⑥ 回读）
  if have_qemu; then
    if [ "$FMT" = raw ] || [ "$FMT" = vhd ]; then printf '—  '
    elif qemu-img check -f $QFMT "$P" >/dev/null 2>&1; then cell 0; else cell 1; fi
    # ⑥ qemu convert 回 raw → 我们的 fat.ls 再读（跨工具互认）
    v2=$($T fat.ls --img="$W/chk.raw" --lba=2048 --path=/ 2>/dev/null | jq_name)
    if qemu-img convert -f $QFMT -O raw "$P" "$W/chk.raw" >/dev/null 2>&1; then
      v2=$($T fat.ls --img="$W/chk.raw" --lba=2048 --path=/ 2>/dev/null | jq_name)
      [ "$v2" = "f.txt" ]; cell $?
    else
      cell 1
    fi
  else
    printf '⏭  ⏭  '
  fi
  printf '\n'
done

echo
if [ "$fail" = 0 ]; then echo "════ 全部通过 ════"; else echo "════ 失败 $fail 项 ════"; exit 1; fi
