#!/bin/bash
# build-bios-full.sh — UEFI(GPT)源 → MBR BIOS 启动盘【完整形态】可复现流水线
# 构成：系统保留100M(活动) + 系统50G(=源卷,扇区级克隆) + 恢复1G(0x27,官方件) + 扩展77.9G/逻辑
# 配套：RunOnce 一次性脚本(首启 reagentc /enable 自删)  |  2026-10-18 四步单变量验证全过
set -eu
ROOT=/workspace/imgtool; T=$ROOT/build/tools
SRC="${2:-/workspace/images/Windows 11 UEFI.qcow2}"
OUT="${1:-/workspace/images/BUILD-FULL-BIOS-RE-128G.qcow2}"
W=/tmp/full; rm -rf $W; mkdir -p $W

echo "① 128GB MBR 五分区布局（表项与卷对齐：系统=源卷50G）"
rm -f "$OUT"; $T/imglayout mk "$OUT" $((128*1024*1024*1024)) mbr >/dev/null
$T/imgcmd part.layout --img="$OUT" --sr-mb=100 --sys-gb=50 --rec-gb=1 --extended=true --progress=bar
echo "② MBR 引导码 + 签名（与 Win11 BCD 内嵌值 3d935ebe 对齐；★重写表后必打）"
$T/imgcmd part.mbrboot --img="$OUT" --code=$ROOT/reference/mbr-code-bios.bin --sig=3d935ebe
echo "③ 格式化系统保留分区"
$T/imgntfsfs "$OUT" 2048 format SYSTEM >/dev/null
echo "④ 扇区级克隆系统分区（GPT 源 296960 → MBR 盘 206848；不扩容卷）"
$T/imgcmd part.copy --img="$OUT" --src="$SRC" --src-lba=296960 --dst-lba=206848 --progress=bar
echo "⑤ 引导文件 → SR（bootmgr=目标 Win11 PCAT；BCD=Win11 bcdboot 真值参考件）"
$T/imgntfsfs "$OUT" 206848 export /Windows/Boot/PCAT/bootmgr $W/bootmgr >/dev/null
$T/imgntfsfs "$OUT" 2048 put $W/bootmgr /bootmgr >/dev/null
cp $ROOT/reference/bcd-reference-bios-win11.bin $W/BCD
$T/imgntfsfs "$OUT" 2048 put $W/BCD /Boot/BCD >/dev/null
echo "⑥ NTFS 引导码（imgfixboot 方案B：只移植引导码区，保留本卷 BPB；★mkntfs 只写扇区0）"
$T/imgfixboot "$OUT" 2048 --tpl "/workspace/images/Windows 10 Xlite.qcow2" 2048
echo "⑦ 恢复分区：格式化 + 官方 winre.wim/boot.sdi"
$T/imgntfsfs "$OUT" 105064448 format Recovery >/dev/null
$T/imgntfsfs "$OUT" 105064448 put /workspace/build/probe/Winre.wim /Recovery/WindowsRE/winre.wim >/dev/null
$T/imgntfsfs "$SRC" 266336256 export /Recovery/WindowsRE/boot.sdi $W/boot.sdi >/dev/null 2>&1 || true
[ -s $W/boot.sdi ] && $T/imgntfsfs "$OUT" 105064448 put $W/boot.sdi /Recovery/WindowsRE/boot.sdi >/dev/null && echo "  boot.sdi ✓"
echo "⑧ 三个 NTFS 卷几何字段（格式化后跑，含恢复卷） NTFS 卷几何字段（隐藏扇区/SPT=63/头=255 —— 全0会'不能启动'）"
python3 - <<'PYX'
import subprocess, struct
T="/workspace/imgtool/build/tools"; M="/workspace/images/BUILD-FULL-BIOS-RE-128G.qcow2"
for lba in (2048, 206848, 105064448):
    with open('/tmp/v.bin','wb') as f: subprocess.run([T+"/imgdump",M,str(lba*512),"512"],stdout=f,check=True)
    d=bytearray(open('/tmp/v.bin','rb').read())
    if len(d)<512 or d[3:11]!=b'NTFS    ': print("  LBA",lba,": 非NTFS跳过"); continue
    struct.pack_into('<I',d,0x1C,lba); struct.pack_into('<H',d,0x18,63); struct.pack_into('<H',d,0x1A,255)
    open('/tmp/v.bin','wb').write(d)
    subprocess.run([T+"/imgraw","write",M,str(lba*512),"/tmp/v.bin"],check=True,stdout=subprocess.DEVNULL)
    print("  LBA",lba,"几何 ✓")
PYX
echo "⑨ RunOnce 一次性脚本（SYSTEM；部署首启跑一次 reagentc /enable 后自删；值名加'!'=成功才删）"
printf '@echo off\r\nreagentc /enable\r\nreagentc /info > "%%SystemRoot%%\\Temp\\winre-enable.log" 2>&1\r\ndel "%%~f0"\r\nexit /b 0\r\n' > $W/once.cmd
$T/imgrunonce "$OUT" 206848 $W/once.cmd /Windows/System32/imgtool/winre-once.cmd '!imgtool-winre' >/dev/null
echo "⑩ 自检（必需项含 mbr.chs/引导链/恢复件）"
$T/imgbc "$OUT" --bios | tail -1
echo "✅ 完成: $OUT"
