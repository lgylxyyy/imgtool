#!/bin/bash
# build-bios-min.sh — 单变量最小盘：GPT→MBR 纯克隆，只有【系统保留+系统】两个分区
# （无恢复分区、无扩展分区、其余未分配）—— 2026-10-18 SeaBIOS 实测：能启动到 Windows 徽标 ✓
set -eu
ROOT=/workspace/imgtool; T=$ROOT/build/tools
SRC="/workspace/images/Windows 11 UEFI.qcow2"
OUT=${1:-/workspace/images/MIN-2分区-GPT转MBR.qcow2}
W=/tmp/min; rm -rf $W; mkdir -p $W
echo "① 128GB MBR 盘 + 两分区布局（SR100M 活动 + 系统50G=源卷大小）"
rm -f "$OUT"; $T/imglayout mk "$OUT" $((128*1024*1024*1024)) mbr >/dev/null
$T/imgcmd part.layout --img="$OUT" --sr-mb=100 --sys-gb=50 --rec-gb=0
echo "② MBR 引导码 + 签名（与 Win11 BCD 内嵌值对齐）"
$T/imgcmd part.mbrboot --img="$OUT" --code=$ROOT/reference/mbr-code-bios.bin --sig=3d935ebe
echo "③ 格式化 SR"
$T/imgntfsfs "$OUT" 2048 format SYSTEM >/dev/null
echo "④ 克隆系统分区（GPT 源 296960 → MBR 盘 206848）"
$T/imgcmd part.copy --img="$OUT" --src="$SRC" --src-lba=296960 --dst-lba=206848
echo "⑤ 引导文件（bootmgr=源盘 Win11 PCAT；BCD=Win11 bcdboot 真值参考件）"
$T/imgntfsfs "$OUT" 206848 export /Windows/Boot/PCAT/bootmgr $W/bootmgr >/dev/null
$T/imgntfsfs "$OUT" 2048 put $W/bootmgr /bootmgr >/dev/null
cp $ROOT/reference/bcd-reference-bios-win11.bin $W/BCD
$T/imgntfsfs "$OUT" 2048 put $W/BCD /Boot/BCD >/dev/null
echo "⑥ NTFS 引导码（imgfixboot 方案B：只移植引导码，保留本卷 BPB）"
$T/imgfixboot "$OUT" 2048 --tpl "/workspace/images/Windows 10 Xlite.qcow2" 2048
echo "⑦ 两卷几何字段（隐藏扇区/SPT=63/头=255 —— mkntfs 写 0 会'不能启动'）"
python3 - <<'PYX'
import subprocess, struct
T="/workspace/imgtool/build/tools"; M="/workspace/images/MIN-2分区-GPT转MBR.qcow2"
for lba in (2048, 206848):
    with open('/tmp/v.bin','wb') as f:
        subprocess.run([T+"/imgdump",M,str(lba*512),"512"],stdout=f,check=True)
    d=bytearray(open('/tmp/v.bin','rb').read())
    struct.pack_into('<I',d,0x1C,lba); struct.pack_into('<H',d,0x18,63); struct.pack_into('<H',d,0x1A,255)
    open('/tmp/v.bin','wb').write(d)
    subprocess.run([T+"/imgraw","write",M,str(lba*512),"/tmp/v.bin"],check=True,stdout=subprocess.DEVNULL)
    print("  LBA",lba,"几何 ✓")
PYX
echo "⑧ 自检"; $T/imgbc "$OUT" --bios | tail -1
echo "✅ 完成: $OUT"
