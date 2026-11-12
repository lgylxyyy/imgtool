#!/bin/bash
# copy-sparse-test.sh — 宿主 ↔ 宿主复制必须能保住稀疏（T4）
#
# 为什么重要：raw 映像 st_size 是容量、st_blocks 才是占用。用户在文件页
# 「复制」一个 64 GiB 稀疏 raw，如果逐字节复制就会写满 64 GiB —— 手机直接爆盘，
# 而且用户完全看不出为什么。这里把三种结果都钉住：
#   auto/always + 源有洞  → 目标占用 ≈ 源占用，sparse_kept=true
#   never                 → 目标按完整容量写，sparse_kept=false（用户明确要求）
#   源没洞                → 普通复制，sparse_source=false
# 目标在 FAT32（无洞概念）→ sparse_kept=false，App 必须提示。
set -u
T="${IMGTOOL:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="${IMG_TEST_TMP:-/tmp}/copysparse-$$"; mkdir -p "$W"; trap 'rm -rf "$W"' EXIT
fail=0
ok(){ echo "  ✅ $1"; }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

SZ=$((256*1024*1024))       # 256 MiB 逻辑大小，实际只写 1 MiB

echo "════ 0. 造 256 MiB 稀疏文件（1 MiB 数据在 100 MiB 处）════"
truncate -s $SZ "$W/s.img"
dd if=/dev/urandom of="$W/s.img" bs=1M count=1 seek=100 conv=notrunc 2>/dev/null
SBLK=$(stat -c %b "$W/s.img")
[ "$SBLK" -lt $((SZ/512/8)) ] && ok "源确实稀疏（$(($SBLK*512/1024)) KiB 占用 / $((SZ/1024/1024)) MiB）" \
    || ng "源不稀疏（blocks=$SBLK）"

echo "════ 1. copy --sparse=auto：目标占用 ≈ 源占用 ════"
OUT=$($T copy --src="host:$W/s.img" --dst="host:$W/a.img" --sparse=auto 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['sparse_source']")" = "True" ] && ok "sparse_source=true" || ng "sparse_source 不是 true"
[ "$(echo "$OUT" | jq_ "d['data']['sparse_kept']")" = "True" ] && ok "sparse_kept=true" || ng "sparse_kept 不是 true"
HB=$(echo "$OUT" | jq_ "d['data']['hole_bytes']")
[ "${HB:-0}" -gt $((SZ/2)) ] && ok "hole_bytes=$HB（超过一半）" || ng "hole_bytes=$HB 太小"
ABLK=$(stat -c %b "$W/a.img")
[ "$ABLK" -le $((SBLK*2+2048)) ] && ok "目标块数 ${ABLK} ≈ 源 ${SBLK}（真的没写洞）" || ng "目标块数 $ABLK 远大于源 $SBLK"
[ "$(stat -c %s "$W/a.img")" = "$SZ" ] && ok "目标长度 = 容量（$SZ）" || ng "目标长度不对"
cmp -s "$W/s.img" "$W/a.img" && ok "内容逐字节一致" || ng "内容不一致"

echo "════ 2. copy --sparse=never：按完整容量写，如回报 false ════"
OUT=$($T copy --src="host:$W/s.img" --dst="host:$W/n.img" --sparse=never 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['sparse_source']")" = "True" ] && ok "sparse_source 仍报 true（源的事实）" || ng "sparse_source 丢了"
[ "$(echo "$OUT" | jq_ "d['data']['sparse_kept']")" = "False" ] && ok "sparse_kept=false（如实报告）" || ng "sparse_kept 应为 false"
NBLK=$(stat -c %b "$W/n.img")
[ "$NBLK" -gt $((SZ/512/2)) ] && ok "目标块数 $NBLK（确实实体化了）" || ng "目标块数 $NBLK，没有实体化"
cmp -s "$W/s.img" "$W/n.img" && ok "never 的内容也一致" || ng "never 的内容不一致"

echo "════ 2b. 洞在【中间】（REQ-76 ①的真机样本）：也必须保住 ════"
# 真机复现：数据在 10 MB 与 200 MB，洞在中间 → 旧实现只数尾洞，会把 256 MB 全部写实
truncate -s $SZ "$W/mid.img"
dd if=/dev/urandom of="$W/mid.img" bs=1M count=1 seek=10 conv=notrunc 2>/dev/null
dd if=/dev/urandom of="$W/mid.img" bs=1M count=1 seek=200 conv=notrunc 2>/dev/null
MBLK=$(stat -c %b "$W/mid.img")
OUT=$($T copy --src="host:$W/mid.img" --dst="host:$W/mid2.img" --sparse=auto 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['sparse_source']")" = "True" ] && ok "中间有洞：sparse_source=true" || ng "中间有洞被判成无洞（sparse_source=$(echo "$OUT" | jq_ "d['data']['sparse_source']")）"
MH=$(echo "$OUT" | jq_ "d['data']['hole_bytes']")
[ "${MH:-0}" -gt $((SZ/2)) ] && ok "hole_bytes=$MH（超过一半，中间的洞被算进去了）" || ng "hole_bytes=$MH 太小（中间洞漏算）"
M2BLK=$(stat -c %b "$W/mid2.img")
[ "$M2BLK" -le $((MBLK*2+2048)) ] && ok "目标块数 ${M2BLK} ≈ 源 ${MBLK}（洞没被写实）" || ng "目标块数 $M2BLK 远大于源 $MBLK（洞被写实了）"
cmp -s "$W/mid.img" "$W/mid2.img" && ok "内容逐字节一致" || ng "内容不一致"

echo "════ 3. 源没洞：普通复制，sparse_source=false ════"
dd if=/dev/urandom of="$W/dense.bin" bs=1M count=8 2>/dev/null
OUT=$($T copy --src="host:$W/dense.bin" --dst="host:$W/dense2.bin" 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['sparse_source']")" = "False" ] && ok "sparse_source=false" || ng "普通文件被误报成有洞"
[ "$(echo "$OUT" | jq_ "d['data']['sparse_kept']")" = "True" ] && ok "sparse_kept=true（没放大）" || ng "普通文件 sparse_kept 应为 true"
cmp -s "$W/dense.bin" "$W/dense2.bin" && ok "内容一致" || ng "内容不一致"

echo "════ 4. 目录递归里含稀疏文件 ════"
mkdir -p "$W/dir/sub"
cp --sparse=always "$W/s.img" "$W/dir/sparse.img" 2>/dev/null || truncate -s $SZ "$W/dir/sparse.img"
echo hi > "$W/dir/sub/a.txt"
OUT=$($T copy --src="host:$W/dir" --dst="host:$W/dir2" 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['ok']")" = "True" ] && ok "目录复制成功" || ng "目录复制失败"
DBLK=$(stat -c %b "$W/dir2/sparse.img" 2>/dev/null || echo 99999999)
[ "$DBLK" -le $((SBLK*2+2048)) ] && ok "目录里的稀疏文件也保住了（${DBLK} 块）" || ng "目录复制把稀疏弄丢了（$DBLK 块）"

echo "════ 5. 目标在 FAT32（镜像内）：没有洞概念，必须报 false ════"
$T img.create --out="$W/fat.qcow2" --size-gb=1 --format=qcow2 --table=mbr --limits=off >/dev/null 2>&1
$T fs.format --img="$W/fat.qcow2" --lba=2048 --fs=fat32 >/dev/null 2>&1
OUT=$($T copy --src="host:$W/s.img" --dst="$W/fat.qcow2!lba2048:/s.img" 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['ok']")" = "True" ] && ok "写进 FAT32 成功" || ng "写进 FAT32 失败"
[ "$(echo "$OUT" | jq_ "d['data']['sparse_kept']")" = "False" ] && ok "sparse_kept=false（FAT 无洞）" || ng "FAT 目标应报 sparse_kept=false"
[ "$(echo "$OUT" | jq_ "d['data']['sparse_source']")" = "True" ] && ok "sparse_source=true（源有洞）" || ng "sparse_source 应 true"

echo "════ 6. 参数校验 ════"
$T copy --src="host:$W/dense.bin" --dst="host:$W/x.bin" --sparse=bogus >/dev/null 2>&1
[ $? -ne 0 ] && ok "sparse=bogus 被拒绝" || ng "sparse=bogus 没被拒绝"

echo
echo "════ 失败 $fail ════"
[ $fail -eq 0 ] || exit 1