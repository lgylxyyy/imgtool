#!/bin/bash
# build-bios-re.sh — 组装 MBR/BIOS + 恢复分区的启动盘（GPT→MBR 跨格式迁移）
# 源: /workspace/images/Windows 11 UEFI.qcow2 (GPT: 系统分区 50GB 已进过桌面)
# 目标: 128GB MBR: 系统保留100M(活动) + 系统64G(克隆) + 恢复1G(0x27) + 扩展分区(逻辑 Data)
set -eu
ROOT=/workspace/imgtool
T=$ROOT/build/tools
SRC="/workspace/images/Windows 11 UEFI.qcow2"
OUT=${1:-/workspace/images/BUILD-BIOS-RE-128G.qcow2}
W=/tmp/biosre; rm -rf $W; mkdir -p $W
REF=$ROOT/reference
DISK=$((128*1024*1024*1024))

echo "════ ① 建 128GB MBR 布局（SR100M + 系统64G + 恢复1G + 扩展分区）════"
rm -f "$OUT"
$T/imglayout mk "$OUT" $DISK mbr >/dev/null
$T/imgcmd part.layout --img="$OUT" --sr-mb=100 --sys-gb=64 --rec-gb=1 --logical-name=Data --progress=bar
SR=$(  $T/imgpart show "$OUT" | python3 -c "import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['parts'] if p.get('active')][0])")
SYS=$( $T/imgpart show "$OUT" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['parts'][1]['first'])")
REC=$( $T/imgpart show "$OUT" | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['parts'][2]['first'])")
LOG=$( $T/imgpart show "$OUT" | python3 -c "import sys,json;d=json.load(sys.stdin);ls=[p for p in d['parts'] if p.get('logical')];print(ls[0]['first'] if ls else 0)")
echo "  SR=$SR SYS=$SYS REC=$REC LOG=$LOG"

echo "════ ② 写 MBR 引导码（没有它 BIOS 报找不到操作系统）════"
$T/imgcmd part.mbrboot --img="$OUT" --code=$REF/mbr-code-bios.bin --progress=bar

echo "════ ③ 格式化 SR / 恢复 / 逻辑分区（系统分区不格式化，直接搬内容）════"
$T/imgntfsfs "$OUT" $SR   format SYSTEM  >/dev/null
$T/imgntfsfs "$OUT" $REC  format Recovery >/dev/null
[ "$LOG" != "0" ] && $T/imgntfsfs "$OUT" $LOG format Data >/dev/null || true
echo "  格式化完成"

echo "════ ④ 搬系统盘内容（源系统分区 → 新盘 64G 系统分区；不扩容 NTFS）════"
$T/imgcmd part.copy --img="$OUT" --src="$SRC" --src-lba=296960 --dst-lba=$SYS --progress=bar

echo "════ ⑤ BIOS 引导文件 → 系统保留分区 ════"
# ⑤a 从新盘系统分区取它自己的 BIOS 版 bootmgr（Windows 自带）
$T/imgntfsfs "$OUT" $SYS export /Windows/Boot/PCAT/bootmgr $W/bootmgr >/dev/null
$T/imgntfsfs "$OUT" $SR  put $W/bootmgr /bootmgr >/dev/null
echo "  bootmgr → /bootmgr"
# ⑤b BCD：用 diskB（能启动的 BIOS 盘）的 \Boot\BCD 当模板（BIOS BCD 设备路径是 partition(N)，
#     我们的布局 SR=1/系统=2 与它一致；恢复条目交给首启的 reagentc 自建）
$T/imgntfsfs "/workspace/images/diskB-Windows-BIOS.qcow2" 2048 export /Boot/BCD $W/BCD >/dev/null
$T/imgntfsfs "$OUT" $SR put $W/BCD /Boot/BCD >/dev/null
echo "  BCD → /Boot/BCD（diskB 真值模板）"
# ⑤c BCD 字体（bootmgr 菜单显示需要）
$T/imgntfsfs "/workspace/images/diskB-Windows-BIOS.qcow2" 2048 export /Boot/fonts/fonts.ttf $W/fonts.ttf >/dev/null 2>&1 || true
if [ -s $W/fonts.ttf ]; then $T/imgntfsfs "$OUT" $SR put $W/fonts.ttf /Boot/fonts/fonts.ttf >/dev/null; echo "  fonts.ttf → /Boot/fonts/"; fi
# ⑤d ★ VBR 的"分区起始扇区"字段：mkntfs 拿不到分区偏移（我们的设备不报几何），
#     写的是 0 → Windows 嫌"不能从这个设备启动"。
#     ★ 修正：保留【本卷自己的 VBR】（MFT 位置等都是对的），只把隐藏扇区字段（0x1C）改成真实 LBA。
#     （之前抄 diskB 的 VBR 是错的：那是另一个卷的 VBR，MFT LCN 对不上 → "A disk read error"）
$T/imgdump "$OUT" $((SR*512)) 512 > $W/vbr.bin
python3 - "$W/vbr.bin" "$SR" <<'PYEOF'
import sys,struct
p, sr = sys.argv[1], int(sys.argv[2])
d=bytearray(open(p,'rb').read())
struct.pack_into('<I', d, 0x1C, sr)          # hidden sectors = 分区起始 LBA
open(p,'wb').write(d)
print(f"  VBR 隐藏扇区字段 → {sr}")
PYEOF
$T/imgraw write "$OUT" $((SR*512)) $W/vbr.bin >/dev/null && echo "  VBR 已就地补丁（不抄别人的卷）"

echo "════ ⑥ 恢复分区：官方 winre.wim + boot.sdi（从源盘的恢复分区取官方件）════"
# winre.wim：用官方镜像里的 RE（从官方 ESD/ISO 提取的真值，之前流水线同款）
cp /workspace/build/probe/Winre.wim $W/winre.wim
# boot.sdi：源盘恢复分区里的官方件
$T/imgntfsfs "$SRC" 266336256 export /Recovery/WindowsRE/boot.sdi $W/boot.sdi >/dev/null
ls -la $W/winre.wim $W/boot.sdi | awk '{print "  ",$9,$5,"字节"}'
$T/imgntfsfs "$OUT" $REC put $W/winre.wim  /Recovery/WindowsRE/winre.wim  >/dev/null
$T/imgntfsfs "$OUT" $REC put $W/boot.sdi /Recovery/WindowsRE/boot.sdi >/dev/null
echo "  恢复分区已放官方件"

echo "════ ⑦ 一次性脚本（RunOnce：下次登录自动 reagentc /enable，跑完自动消失）════"
cat > $W/winre-once.cmd <<'EOF'
@echo off
reagentc /enable
reagentc /info > "%SystemRoot%\Temp\winre-enable.log" 2>&1
del "%~f0"
exit /b 0
EOF
$ROOT/build/tools/imgrunonce "$OUT" $SYS $W/winre-once.cmd /Windows/System32/imgtool/winre-once.cmd '!imgtool-winre'

echo "════ ⑧ 自检（BCD/引导链）════"
$T/imgbc "$OUT" --bios 2>/dev/null | tail -3 || true

echo
echo "✅ 组装完成: $OUT"
echo "   系统保留=$SR  系统=$SYS  恢复=$REC  逻辑=$LOG"
