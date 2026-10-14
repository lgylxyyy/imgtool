#!/bin/bash
set -e
cd /workspace/_build
echo "════════ 完全自建 BIOS 启动盘 ════════"
BP=52428800; VOLSEC=102400
echo "── ① 50MB 引导分区 ──"
rm -f bp.img; truncate -s $BP bp.img
mkntfs -F -Q -s 512 -c 4096 bp.img >/dev/null 2>&1
echo "   ✓ mkntfs"
echo "── ② 移植真实引导代码（\$Boot 扇区0~9）──"
node buildboot.mjs bp.img 2048 $VOLSEC
echo "── ③ bootmgr(ESD) + \\Boot\\BCD ──"
./ntfsput bp.img bm-esd.bin /bootmgr >/dev/null
./ntfsput bp.img bcd-base.bin /Boot/BCD >/dev/null
echo "   ✓"
echo "── ④ 组装 ──"
WINSEC=65533952; DISKSEC=$((104448+WINSEC))
rm -f final.raw; truncate -s $((DISKSEC*512)) final.raw
dd if=bp.img of=final.raw bs=512 seek=2048 conv=notrunc status=none
dd if=sysvolA.img of=final.raw bs=512 seek=104448 conv=notrunc,sparse status=none
echo "   ✓ 盘 $DISKSEC 扇区"
echo "── ⑤ 自建 MBR ──"
node mkmbr.mjs final.raw $DISKSEC 104448 $WINSEC
rm -f final.qcow2
qemu-img convert -f raw -O qcow2 final.raw diskB-Windows-BIOS-FINAL.qcow2
rm -f final.raw bp.img
echo "   ✓ 完成"
