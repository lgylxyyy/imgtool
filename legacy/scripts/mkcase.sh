#!/bin/bash
# $1=名字 $2=卷大小(字节) $3=bootmgr源 $4=卷内NTFS扇区数
set -e
cd /workspace/_build
rm -f $1.img $1.raw $1.qcow2
truncate -s $2 $1.img
mkntfs -F -Q -s 512 -c 4096 $1.img >/dev/null 2>&1
node -e "
const fs=require('fs'); const ref=fs.readFileSync('t-vbr');
const fd=fs.openSync('$1.img','r+'); const SEC=Math.floor(parseInt('$2')/512);
for(const off of [0,(SEC-1)*512]){ const v=Buffer.alloc(512); fs.readSync(fd,v,0,512,off);
  ref.copy(v,0x4a,0x4a,0x1fe); v.writeUInt32LE(2048,28); v.writeUInt16LE(63,24); v.writeUInt16LE(255,26);
  fs.writeSync(fd,v,0,512,off); }
fs.closeSync(fd);
"
./ntfsput $1.img $3 /bootmgr >/dev/null
./ntfsput $1.img bcd-base.bin /Boot/BCD >/dev/null
# ★ 磁盘大小 = 分区#1 起点 + 卷扇区数（字节 = 扇区数 × 512）
VOLSEC=$(($2/512))
DISKSEC=$((2048+VOLSEC))
DISKB=$((DISKSEC*512))
echo "  卷 $VOLSEC 扇区 → 磁盘 $DISKSEC 扇区 = $DISKB 字节"
truncate -s $DISKB $1.raw
dd if=$1.img of=$1.raw bs=512 seek=2048 conv=notrunc status=none
node -e "
const fs=require('fs');
const m=Buffer.alloc(512,0);
fs.readFileSync('mbr-bootcode.bin').copy(m,0);
const SPT=63,H=255; function chs(l){const C=Math.min(Math.floor(l/(SPT*H)),1023),r=l%(SPT*H);return [Math.floor(r/SPT),((r%SPT)+1)|((C>>8)<<6),C&0xff];}
const e=Buffer.alloc(16,0); e[0]=0x80; const a=chs(2048);e[1]=a[0];e[2]=a[1];e[3]=a[2];e[4]=0x07;
const b=chs($DISKSEC-1);e[5]=b[0];e[6]=b[1];e[7]=b[2];
e.writeUInt32LE(2048,8); e.writeUInt32LE($VOLSEC,12);
e.copy(m,446); Buffer.from([0x2b,0x93,0xcd,0x4c]).copy(m,440); m[510]=0x55;m[511]=0xaa;
const fd=fs.openSync('$1.raw','r+'); fs.writeSync(fd,m,0,512,0); fs.closeSync(fd);
console.log('  MBR #1: LBA='+m.readUInt32LE(446+8)+' size='+m.readUInt32LE(446+12)+' → 结束于 '+(m.readUInt32LE(446+8)+m.readUInt32LE(446+12))+'  盘=$DISKSEC');
"
qemu-img convert -f raw -O qcow2 $1.raw $1.qcow2
rm -f $1.img $1.raw
