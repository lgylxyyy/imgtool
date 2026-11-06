#!/bin/bash
# ads-test.sh — 命名数据流（ADS）复制：统一 copy 必须保留附加数据
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
P="$ROOT/build/tests/ads_probe"
W=$(mktemp -d /tmp/adsXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1  ${2:-}"; fail=$((fail+1)); }
chk(){ if eval "$2" >/dev/null 2>&1; then ok "$1"; else ng "$1" "${3:-}"; fi; }
J(){ python3 -c "import json,sys;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

IMG="$W/a.qcow2"
IMG2="$W/b.qcow2"
"$T/imgcmd" img.create --out="$IMG" --size-gb=1 --table=gpt --esp-mb=64 --msr-mb=16 --limits=off >/dev/null 2>&1 || { echo "建盘失败"; exit 1; }
"$T/imgcmd" img.create --out="$IMG2" --size-gb=1 --table=gpt --esp-mb=32 --msr-mb=16 --limits=off >/dev/null 2>&1
LBA2=$("$T/imgcmd" part.show --img="$IMG2" | J "d['data']['pt']['parts'][-1]['first']")
"$T/imgcmd" fs.format --img="$IMG2" --lba="$LBA2" --fs=ntfs --label=ADS2 >/dev/null 2>&1
LBA=$("$T/imgcmd" part.show --img="$IMG" | J "d['data']['pt']['parts'][-1]['first']")
"$T/imgcmd" fs.format --img="$IMG" --lba="$LBA"   --fs=ntfs  --label=ADS >/dev/null 2>&1
"$T/imgcmd" fs.format --img="$IMG" --lba=2048      --fs=fat32 --label=ESP >/dev/null 2>&1

echo "════ ADS（命名数据流）════"
chk "ads_probe write：写入无名数据 + 2 条命名流" "$P write '$IMG' $LBA"
chk "源文件两条流可读回" "$P verify '$IMG' $LBA /ADS.bin"

d=$("$T/imgcmd" copy "$IMG!lba$LBA:/ADS.bin" "$IMG!lba$LBA:/N.bin")
chk "copy 同卷 成功" "[ \"$(echo "$d" | J "d['ok']")\" = True ]" "$d"
chk "  同卷保留两条命名流" "$P verify '$IMG' $LBA /N.bin"

d=$("$T/imgcmd" copy "$IMG!lba$LBA:/N.bin" "$IMG!lba$LBA:/F.bin")
chk "copy NTFS→NTFS 成功" "[ \"$(echo "$d" | J "d['ok']")\" = True ]" "$d"
chk "  保留两条命名流" "$P verify '$IMG' $LBA /F.bin"
ADS_N=$(echo "$d" | J "d['data'].get('ads')")
chk "  返回 ads=2" "[ '$ADS_N' = 2 ]" "$d"

# NTFS → FAT：FAT 没有 ADS，主数据必须完整
d=$("$T/imgcmd" copy "$IMG!lba$LBA:/F.bin" "$IMG!lba2048:/F.bin")
chk "NTFS→FAT 主数据复制成功" "[ \"$(echo "$d" | J "d['ok']")\" = True ]" "$d"
"$T/imgcmd" fat.export --img="$IMG" --lba=2048 --path=/F.bin --dst="$W/out.bin" >/dev/null 2>&1
chk "  FAT 上内容 = MAIN" "printf 'MAIN\\n' | cmp -s - '$W/out.bin'"

# FAT → NTFS：仍然可复制，只是没有 ADS（记录为 0）
d=$("$T/imgcmd" copy "$IMG!lba2048:/F.bin" "$IMG!lba$LBA:/B.bin")
chk "FAT→NTFS 成功" "[ \"$(echo "$d" | J "d['ok']")\" = True ]" "$d"
ADS_N=$(echo "$d" | J "d['data'].get('ads')")
chk "  返回 ads=0（FAT 没有命名流）" "[ '$ADS_N' = 0 ]" "$d"

# 跨镜像 ADS
d=$("$T/imgcmd" copy "$IMG!lba$LBA:/N.bin" "$IMG2!lba$LBA2:/X.bin")
chk "跨镜像 NTFS→NTFS 成功" "[ \"$(echo "$d" | J "d['ok']")\" = True ]" "$d"
chk "  跨镜像保留两条命名流" "$P verify '$IMG2' $LBA2 /X.bin"

chk "qc.verify 通过" "[ \"$("$T/imgcmd" qc.verify --img="$IMG" | J "d['ok']")\" = True ]"
echo "════ 结果：通过 $pass · 失败 $fail ════"
rm -rf "$W"
exit $((fail>0))
