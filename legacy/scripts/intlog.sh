#!/bin/bash
cd /workspace/_build
rm -f int_$2.log
timeout ${3:-20} qemu-system-x86_64 -machine pc -m 256 \
  -drive file="$1",format=qcow2,if=ide \
  -display none -monitor none -serial none \
  -d int -D int_$2.log -no-reboot -no-shutdown >/dev/null 2>&1
echo "  [$2] $(wc -l < int_$2.log) 行"
