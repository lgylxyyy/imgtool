#!/bin/bash
cd /workspace/_build
( sleep $2; echo "xp /4000xb 0xb8000"; sleep 3; echo "quit" ) | \
timeout $(( $2 + 25 )) qemu-system-x86_64 -machine pc -m 1024 \
  -drive file="$1",format=qcow2,if=ide -vga std \
  -display none -monitor stdio -serial none -no-reboot -no-shutdown 2>/dev/null > mon_$3.txt
node vgatext.mjs mon_$3.txt
