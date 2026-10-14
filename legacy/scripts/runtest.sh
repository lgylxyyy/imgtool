#!/bin/bash
# $1=镜像  $2=标签  $3=超时秒
cd /workspace/_build
rm -f dbg_$2.log
timeout $3 qemu-system-x86_64 \
  -machine pc -m 256 \
  -drive file="$1",format=qcow2,if=ide \
  -display none -monitor none -serial none \
  -debugcon "file:dbg_$2.log" -global isa-debugcon.iobase=0x402 \
  -no-reboot -no-shutdown 2>&1 | head -3
if [ -s dbg_$2.log ]; then
  echo "  [${2}] debugcon 抓到 $(wc -c < dbg_$2.log) 字节"
else
  echo "  [${2}] debugcon 为空（这个 SeaBIOS 可能没开 DEBUG_LEVEL）"
fi
