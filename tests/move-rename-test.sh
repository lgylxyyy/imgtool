#!/bin/bash
# move-rename-test.sh — 移动的两种实现必须能区分（T3）
#
# 背景：raw 映像的「移动」在 App 里有两个结果完全不同的路径：
#   · rename(2)  —— 只改目录项，8 GiB 稀疏文件 2 毫秒完成，占用不变；
#   · copy+删源  —— 逐字节重写，8 GiB 稀疏文件变成 8 GiB 实体占用（可能爆盘）。
# 用户看不出区别，只能靠工具如实报告。所以 move 必须回 moved_by=rename|copy。
#
# 断言：
#   1 同卷不同目录 → moved_by=rename、st_blocks 不变、耗时 < 5 s（8 GiB 稀疏）
#   2 同目录改名   → mode=rename、moved_by=rename
#   3 mv a a      → noop=true、moved_by=rename
#   4 镜像内改名   → moved_by=rename（NTFS 索引操作）
#   5 host → 镜像内 → mode=cross_volume、moved_by=copy（真的搬了数据）
#   6 清单文件（list_file）里的每一项也带 moved_by
set -u
T="${IMGTOOL:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fail=0
ok(){ echo "  ✅ $1"; }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

mkdir -p "$W/a" "$W/b"

echo "════ 1. 同卷 move：8 GiB 稀疏 raw 必须走 rename ════"
truncate -s 8G "$W/a/sp.img"
dd if=/dev/urandom of="$W/a/sp.img" bs=1M count=1 seek=1000 conv=notrunc 2>/dev/null
B0=$(stat -c %b "$W/a/sp.img")
T0=$(date +%s%N)
OUT=$($T move --src="host:$W/a/sp.img" --dst="host:$W/b/sp.img" 2>/dev/null | tail -1)
T1=$(date +%s%N)
MS=$(( (T1-T0)/1000000 ))
BY=$(echo "$OUT" | jq_ "d['data']['moved_by']")
MD=$(echo "$OUT" | jq_ "d['data']['mode']")
B1=$(stat -c %b "$W/b/sp.img" 2>/dev/null || echo -1)
[ "$BY" = "rename" ] && ok "moved_by=rename（mode=$MD）" || ng "moved_by=$BY（应 rename）"
[ "$MD" = "move" ] && ok "mode=move（父目录变了）" || ng "mode=$MD（应 move）"
[ "$B0" = "$B1" ] && ok "宿主块数不变（$B0 → $B1，稀疏原样保留）" || ng "块数 $B0 → $B1"
[ "$MS" -lt 5000 ] && ok "耗时 ${MS} ms（没有复制 8 GiB）" || ng "耗时 ${MS} ms，像在复制"

echo "════ 2. 同目录改名 ════"
OUT=$($T move --src="host:$W/b/sp.img" --dst="host:$W/b/sp2.img" 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['mode']")" = "rename" ] && ok "mode=rename" || ng "mode 不是 rename"
[ "$(echo "$OUT" | jq_ "d['data']['moved_by']")" = "rename" ] && ok "moved_by=rename" || ng "moved_by 不是 rename"

echo "════ 3. mv a a（no-op）════"
OUT=$($T move --src="host:$W/b/sp2.img" --dst="host:$W/b/sp2.img" 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['noop']")" = "True" ] && ok "noop=true" || ng "noop 不是 true"
[ "$(echo "$OUT" | jq_ "d['data']['moved_by']")" = "rename" ] && ok "moved_by=rename" || ng "moved_by 不是 rename"

echo "════ 4. 镜像内改名（NTFS 索引操作）════"
$T img.create --out="$W/d.qcow2" --size-gb=1 --format=qcow2 --table=mbr --limits=off >/dev/null 2>&1
$T fs.format --img="$W/d.qcow2" --lba=2048 --fs=ntfs >/dev/null 2>&1
echo hello > "$W/h.txt"
$T ntfs.put --img="$W/d.qcow2" --lba=2048 --src="$W/h.txt" --dst=/a.txt >/dev/null 2>&1
OUT=$($T move --src="$W/d.qcow2!lba2048:/a.txt" --dst="$W/d.qcow2!lba2048:/b.txt" 2>/dev/null | tail -1)
[ "$(echo "$OUT" | jq_ "d['data']['moved_by']")" = "rename" ] && ok "镜像内改名：moved_by=rename" || ng "镜像内改名：moved_by 不是 rename"

echo "════ 5. host → 镜像内（真的搬数据）════"
OUT=$($T move --src="host:$W/h.txt" --dst="$W/d.qcow2!lba2048:/c.txt" --allow_host_delete=true 2>/dev/null | tail -1)
MD=$(echo "$OUT" | jq_ "d['data']['mode']")
BY=$(echo "$OUT" | jq_ "d['data']['moved_by']")
DL=$(echo "$OUT" | jq_ "d['data']['deleted']")
[ "$MD" = "cross_volume" ] && ok "mode=cross_volume" || ng "mode=$MD"
[ "$BY" = "copy" ] && ok "moved_by=copy（App 据此提示 raw 会实体化）" || ng "moved_by=$BY（应 copy）"
[ "$DL" = "True" ] && ok "deleted=true" || ng "deleted=$DL"

echo "════ 6. 清单文件每项也带 moved_by ════"
echo hi > "$W/x1"; echo hi > "$W/x2"
cat > "$W/list.txt" <<EOF
host:$W/x1 > host:$W/b/x1
host:$W/x2 > host:$W/b/x2
EOF
OUT=$($T move --list_file="$W/list.txt" 2>/dev/null | tail -1)
N=$(echo "$OUT" | jq_ "len([i for i in d['data']['items'] if i.get('moved_by')=='rename'])" )
[ "$N" = "2" ] && ok "两项都是 moved_by=rename" || ng "带 moved_by=rename 的项数=$N（应 2）"

echo
echo "════ 失败 $fail ════"
[ $fail -eq 0 ] || exit 1