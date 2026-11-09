#!/usr/bin/env bash
# REQ-30（sort/mkdir/stat/批量时间）+ REQ-32（copy 跳过回收站）回归
# 只依赖宿主 imgcmd；不碰网络、不碰真机。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="${IMGCMD:-$ROOT/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
jget(){ python3 -c "import sys,json;d=json.load(sys.stdin);
p='$2'.split('.')
for k in p:
    d=d.get(k) if isinstance(d,dict) else None
print(json.dumps(d,ensure_ascii=False) if not isinstance(d,str) else d)" <<<"$1"; }

Q="$W/a.qcow2"
$T img.create --out="$Q" --size-gb=4 --table=gpt --esp-mb=200 --msr-mb=16 --sys-mb=2000 >/dev/null 2>&1
parts=$($T part.show --img="$Q" 2>/dev/null)
SYS=$(python3 -c "import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['data']['pt']['parts'] if p.get('sectors',0)>1000000][0])" <<<"$parts")
ESP=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d['data']['pt']['parts'][0]['first'])" <<<"$parts")
$T fs.format --img="$Q" --lba="$SYS" --fs=ntfs --label=SYS >/dev/null 2>&1
$T fs.format --img="$Q" --lba="$ESP" --fs=fat32 --label=ESP >/dev/null 2>&1
mkdir -p "$W/src/sub"
echo aaa > "$W/src/a.txt"; head -c 4096 /dev/urandom > "$W/src/Big.bin"
echo ddd > "$W/src/sub/d.txt"
echo trash > "$W/src/.trashed-1795289429-u.xml"
echo trash > "$W/src/sub/.trashed-1795339251-x.qcow2"
echo tmpfile > "$W/src/junk.tmp"

echo "════ ① ntfs.list --sort（REQ-30.1）════"
for f in a.txt Big.bin; do $T ntfs.put --img="$Q" --lba="$SYS" --src="$W/src/$f" --dst="/$f" >/dev/null 2>&1; done
$T ntfs.mkdir --img="$Q" --lba="$SYS" --path=/zzz >/dev/null 2>&1
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=name 2>/dev/null)
names=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(','.join(e['name'] for e in d['data']['names']))" <<<"$out")
[ "$names" = "a.txt,Big.bin,zzz" ] && ok "sort=name 整目录排序" || no "sort=name" "$names"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=size --desc=true 2>/dev/null)
first=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d['data']['names'][0]['name'])" <<<"$out")
[ "$first" = "Big.bin" ] && ok "sort=size --desc" || no "sort=size --desc" "$first"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=dir 2>/dev/null)
first=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d['data']['names'][0]['name'])" <<<"$out")
[ "$first" = "zzz" ] && ok "sort=dir（目录在前）" || no "sort=dir" "$first"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=name --after=5 2>/dev/null)
echo "$out" | grep -q 'CMD-ARG-BAD' && ok "--sort 与 --after 互斥（明确报错）" || no "互斥" "$out"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=size --sort-max=1 2>/dev/null)
echo "$out" | grep -q 'LIST-TOO-BIG' && ok "超 --sort-max 报 LIST-TOO-BIG（不降级）" || no "TOO-BIG" "$out"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=name --limit=1 --offset=1 2>/dev/null)
k=$(python3 -c "import sys,json;d=json.load(sys.stdin);print(d['data']['names'][0]['sort_key'])" <<<"$out")
[ "$k" = "Big.bin" ] && ok "分页后条目带 sort_key" || no "sort_key" "$k"

echo "════ ② mkdir / stat（REQ-30.2/30.3）════"
out=$($T ntfs.mkdir --img="$Q" --lba="$SYS" --path=/x/y/z 2>/dev/null)
echo "$out" | grep -q '"created":true' && ok "ntfs.mkdir --parents 建多层" || no "ntfs.mkdir" "$out"
out=$($T ntfs.mkdir --img="$Q" --lba="$SYS" --path=/x/y/z 2>/dev/null)
echo "$out" | grep -q '"created":false' && ok "重复 mkdir 幂等（created:false）" || no "mkdir 幂等" "$out"
out=$($T ntfs.stat --img="$Q" --lba="$SYS" --path=/Big.bin 2>/dev/null)
sz=$(python3 -c "import sys,json;print(json.load(sys.stdin)['data']['size'])" <<<"$out")
[ "$sz" = "4096" ] && ok "ntfs.stat size" || no "ntfs.stat size" "$sz"
abl=$(python3 -c "import sys,json;print(json.load(sys.stdin)['data']['alloc_bytes'])" <<<"$out")
[ "$abl" -ge 4096 ] && ok "ntfs.stat alloc_bytes（簇对齐）" || no "alloc" "$abl"
out=$($T ntfs.stat --img="$Q" --lba="$SYS" --path=/nope 2>/dev/null)
echo "$out" | grep -q 'PATH-NOT-FOUND' && ok "ntfs.stat 不存在 → PATH-NOT-FOUND" || no "stat missing" "$out"
out=$($T fat.mkdir --img="$Q" --lba="$ESP" --path=/EFI/BOOT 2>/dev/null)
echo "$out" | grep -q '"created":true' && ok "fat.mkdir" || no "fat.mkdir" "$out"
$T fat.put --img="$Q" --lba="$ESP" --src="$W/src/a.txt" --dst=/EFI/BOOT/a.txt >/dev/null 2>&1
out=$($T fat.stat --img="$Q" --lba="$ESP" --path=/EFI/BOOT/a.txt 2>/dev/null)
sz=$(python3 -c "import sys,json;print(json.load(sys.stdin)['data']['size'])" <<<"$out")
[ "$sz" = "4" ] && ok "fat.stat size" || no "fat.stat" "$out"
out=$($T fat.ls --img="$Q" --lba="$ESP" --path=/EFI --times=true 2>/dev/null)
echo "$out" | grep -q '"ctime":' && ok "fat.ls --times 一次遍历带时间" || no "fat times" "$out"

echo "════ ③ copy 跳过回收站（REQ-32）════"
out=$($T copy --src="host:$W/src" --dst="host:$W/out" --recursive=true 2>/dev/null)
st=$(python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print(d['skipped_trashed'])" <<<"$out")
[ "$st" = "2" ] && ok "目录复制跳过 2 个 .trashed-*" || no "skipped_trashed" "$out"
[ -e "$W/out/.trashed-1795289429-u.xml" ] && no "子项仍被复制" "$(ls -a "$W/out")" || ok "回收站文件没进目标"
[ -e "$W/out/sub/.trashed-1795339251-x.qcow2" ] && no "子目录里的回收站仍被复制" "" || ok "子目录里的也跳过"
[ -e "$W/out/a.txt" ] && [ -e "$W/out/sub/d.txt" ] && ok "正常文件照常复制" || no "正常文件丢了" "$(find "$W/out" -type f)"
out=$($T copy --src="host:$W/src/.trashed-1795289429-u.xml" --dst="host:$W/manual.xml" 2>/dev/null)
echo "$out" | grep -q '"ok":true' && ok "显式复制回收站文件 = 照常成功（顶层不排除）" || no "顶层误排除" "$out"
out=$($T copy --src="host:$W/src" --dst="host:$W/out2" --recursive=true --exclude='*.tmp' 2>/dev/null)
se=$(python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print(d['skipped_excluded'])" <<<"$out")
[ "$se" = "1" ] && ok "--exclude=*.tmp 生效" || no "exclude" "$out"
printf '.trashed-*\n*.tmp\n' > "$W/ex.txt"
out=$($T copy --src="host:$W/src" --dst="host:$W/out3" --recursive=true --exclude-from="$W/ex.txt" 2>/dev/null)
st=$(python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print(d['skipped_trashed'],d['skipped_excluded'])" <<<"$out")
[ "$st" = "2 1" ] && ok "--exclude-from 生效" || no "exclude-from" "$st"
out=$($T copy --src="host:$W/src" --dst="host:$W/out4" --recursive=true --skip-trashed=false 2>/dev/null)
[ -e "$W/out4/.trashed-1795289429-u.xml" ] && ok "--skip-trashed=false 关掉过滤" || no "关过滤" "$out"
cp -r "$W/src" "$W/mv"
out=$($T move --src="host:$W/mv" --dst="host:$W/mvd" --recursive=true --allow_host_delete=true 2>/dev/null)
[ -e "$W/mvd/.trashed-1795289429-u.xml" ] && ok "move 不跳过（整棵搬）" || no "move 误跳过" "$out"

echo "════ ④ 参数名别名 / 命令专属参数（REQ-36）════"
out=$($T copy --src="host:$W/src" --dst="host:$W/o5" --recursive=true --skip_trashed=true 2>/dev/null)
echo "$out" | grep -q '"skipped_trashed":2' && ok "--skip_trashed（下划线）与中划线等价" || no "下划线别名" "$out"
out=$($T move --src="host:$W/src" --dst="host:$W/m2" --recursive=true --skip-trashed=true 2>/dev/null)
echo "$out" | grep -q 'move 不过滤回收站文件' && ok "move 传 --skip-trashed → 说明提示（不是\"未知参数\"）" || no "move 提示" "$out"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=name --after=abc 2>/dev/null)
echo "$out" | grep -q -- '--sort 与 --after 互斥' && ok "互斥判断先于类型校验" || no "互斥顺序" "$out"
out=$($T ntfs.list --img="$Q" --lba="$SYS" --path=/ --sort=name --sort_max=1 2>/dev/null)
echo "$out" | grep -q 'LIST-TOO-BIG' && ok "--sort_max 别名也认" || no "sort_max 别名" "$out"

echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]
