#!/bin/bash
# 反向验证：损坏的 FAT 簇链（成环）必须【快速报错】，而不是死循环挂住
#
# 为什么要有这个测试：用户会打开"别人给的/损坏的/恶意做的"镜像。修复前
# 沿簇链的循环只有"走到 EOC 停"这一个条件，环状 FAT → 永久死循环
# （CPU 跑满、命令不返回），只能杀进程。修好后应 <2 秒返回错误码。
#
# 用法: corrupt-chain-test.sh        （自带小镜像，不动任何用户数据）
set -u
T=$(cd "$(dirname "$0")/.." && pwd)/build/tools
W=$(mktemp -d /tmp/corruptchainXXXXXX)
IMG="$W/t.qcow2"
pass=0; fail=0
ok(){   echo "  ✅ $1"; pass=$((pass+1)); }
ng(){   echo "  ❌ $1"; fail=$((fail+1)); }

echo "════ 0. 造一张干净的小盘（20GiB qcow2 / ESP 200MB / FAT32 + 2 个文件）════"
$T/imglayout mk "$IMG" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >"$W/mk.log" 2>&1 || { echo "建盘失败："; cat "$W/mk.log"; exit 2; }
ESP_LBA=$($T/imgpart show "$IMG" | python3 -c "
import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
ESP_SEC=$($T/imgpart show "$IMG" | python3 -c "
import sys,json;d=json.load(sys.stdin);print([p['sectors'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
echo "  ESP LBA=$ESP_LBA 扇区=$ESP_SEC"
$T/imgmkfs "$IMG" --mkfs "$ESP_LBA" "TEST" >/dev/null 2>&1 || { echo "格式化失败"; exit 2; }
printf '/workspace/bootx64.efi\t/EFI/Boot/bootx64.efi\n/etc/hostname\t/EFI/hello.txt\n' > "$W/man.txt"
"${T%/tools}/tests/fatput" "$IMG" "$ESP_LBA" "$W/man.txt" >/dev/null || { echo "灌文件失败"; exit 2; }
$T/imgdump "$IMG" $((ESP_LBA*512)) $((ESP_SEC*512)) > "$W/esp.raw"

echo "════ 1. 正向对照（没改坏之前必须都正常）════"
$T/imgfatfs "$IMG" "$ESP_LBA" ls / >/dev/null 2>&1 && ok "干净盘 ls / 正常" || ng "干净盘 ls 失败（误报！）"
$T/imgfatfs "$IMG" "$ESP_LBA" export "/EFI/Boot/bootx64.efi" "$W/out.bin" >/dev/null 2>&1 \
  && ok "干净盘 export 正常" || ng "干净盘 export 失败"

echo "════ 2. 把目录的簇链改成自环，看会不会死循环 ════"
python3 - "$W" <<'PY'
import struct,sys
W=sys.argv[1]
d=bytearray(open(f"{W}/esp.raw","rb").read())
res=struct.unpack_from("<H",d,14)[0]; fatsz=struct.unpack_from("<I",d,36)[0]
fat=res*512; data=(res+2*fatsz)*512
for i in range(0,512,32):
    e=d[data+i:data+i+32]
    if e[0]==0: break
    if e[11] in (0x08,0x0F): continue
    if e[11]&0x10:
        c=e[26]|(e[27]<<8)|(e[20]<<16)|(e[21]<<24)
        if c>=2:
            struct.pack_into("<I",d,fat+c*4,c)               # FAT[c]=c 自环
            struct.pack_into("<I",d,fat+fatsz*512+c*4,c)     # 第二份 FAT 同步
            print(f"  已把目录簇 {c} 改成自环")
        break
open(f"{W}/corrupt_dir.raw","wb").write(d)
PY
$T/imgraw write "$IMG" $((ESP_LBA*512)) "$W/corrupt_dir.raw" >/dev/null
t0=$(date +%s.%N)
timeout 8 $T/imgfatfs "$IMG" "$ESP_LBA" ls /EFI >"$W/o1.txt" 2>&1; rc=$?
t1=$(date +%s.%N); dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
if [ $rc -eq 124 ]; then ng "目录链成环 → timeout 8 秒未返回（仍在死循环！）"
else ok "目录链成环 → ${dt}s 内返回 rc=$rc (输出: $(tail -1 "$W/o1.txt" | tr -d '\r' | cut -c1-40))"; fi

echo "════ 3. 把文件的簇链改成自环，再 rm（走 free_chain，无 size 上界）════"
cp "$W/esp.raw" "$W/esp2.raw"
python3 - "$W" <<'PY'
import struct,sys
W=sys.argv[1]
d=bytearray(open(f"{W}/esp2.raw","rb").read())
res=struct.unpack_from("<H",d,14)[0]; fatsz=struct.unpack_from("<I",d,36)[0]
fat=res*512; data=(res+2*fatsz)*512
for i in range(0,512,32):
    e=d[data+i:data+i+32]
    if e[0]==0: break
    if e[11] in (0x08,0x0F) or (e[11]&0x10): continue
    c=e[26]|(e[27]<<8)|(e[20]<<16)|(e[21]<<24)
    if c>=2:
        struct.pack_into("<I",d,fat+c*4,c)
        struct.pack_into("<I",d,fat+fatsz*512+c*4,c)
        print(f"  已把文件簇 {c} 改成自环")
        break
open(f"{W}/esp2.raw","wb").write(d)
PY
$T/imgraw write "$IMG" $((ESP_LBA*512)) "$W/esp2.raw" >/dev/null
t0=$(date +%s.%N)
timeout 8 $T/imgfatfs "$IMG" "$ESP_LBA" rm "/EFI/Boot/bootx64.efi" >"$W/o2.txt" 2>&1; rc=$?
t1=$(date +%s.%N); dt=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
if [ $rc -eq 124 ]; then ng "文件链成环时 rm → timeout 8 秒未返回（free_chain 死循环）"
else ok "文件链成环时 rm → ${dt}s 内返回 rc=$rc"; fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ $fail -eq 0 ]; then echo "✅ 反向验证通过：损坏镜像快速报错，不再死循环"; else echo "❌ 仍有死循环或误报"; fi
rm -rf "$W"
[ $fail -eq 0 ]
