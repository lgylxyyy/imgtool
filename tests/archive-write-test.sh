#!/usr/bin/env bash
# REQ-39 archive.create / REQ-40 du / REQ-41 写进压缩包 + iso/wim mtime / REQ-42 多源
set -u
T="${IMGCMD:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
# jget <json> <python-expr on d=data>
jg(){ python3 -c "import sys,json
try:
    d=json.load(sys.stdin)['data']
    print(eval(sys.argv[1]))
except Exception as e:
    print('ERR:'+str(e))" "$2" <<<"$1"; }
je(){ python3 -c "import sys,json
try:
    print(json.load(sys.stdin)['err']['error_code'])
except Exception as e:
    print('NONE')" <<<"$1"; }

mkdir -p "$W/src/sub/deep" "$W/src/empty"
echo hello > "$W/src/a.txt"
head -c 200000 /dev/urandom > "$W/src/big.bin"
echo deep > "$W/src/sub/deep/d.txt"
echo trash > "$W/src/.trashed-123"
echo junk > "$W/src/j.tmp"

echo "════ ① archive.create → zip ════"
o=$($T archive.create --dst="$W/a.zip" --format=zip --src="$W/src" --exclude='*.tmp' 2>/dev/null)
[ "$(jg "$o" "d['files']")" = "3" ] && ok "files=3（a/big/d，.trashed 与 *.tmp 都跳过）" || no "files" "$o"
[ "$(jg "$o" "d['skipped_trashed']")" = "1" ] && ok "skipped_trashed=1" || no "skipped_trashed" "$o"
[ "$(jg "$o" "d['skipped_excluded']")" = "1" ] && ok "skipped_excluded=1" || no "skipped_excluded" "$o"
[ "$(jg "$o" "d['bytes_in']")" = "200011" ] && ok "bytes_in 对得上" || no "bytes_in" "$o"
unzip -t "$W/a.zip" >/dev/null 2>&1 && ok "unzip -t 通过" || no "unzip -t" "$(unzip -t "$W/a.zip" 2>&1 | tail -2)"
n=$($T archive.ls --archive="$W/a.zip" --recursive=true 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)['data']['total'])")
[ "$n" = "7" ] && ok "包内 7 条（4 目录 + 3 文件）" || no "条目数" "total=$n"
# 空目录必须真的在包里
$T archive.ls --archive="$W/a.zip" --recursive=true 2>/dev/null | grep -q '"path":"/src/empty"' \
  && ok "空目录也进包了" || no "空目录" "缺失"

echo "════ ② archive.create → 7z ════"
o=$($T archive.create --dst="$W/a.7z" --src="$W/src" --exclude='*.tmp' 2>/dev/null)
[ "$(jg "$o" "d['format']")" = "7z" ] && ok "格式按扩展名认出 7z" || no "format" "$o"
$T archive.extract --archive="$W/a.7z" --path=/src/big.bin --dst="$W/big.out" >/dev/null 2>&1
a=$(md5sum < "$W/src/big.bin"); b=$(md5sum < "$W/big.out")
[ "$a" = "$b" ] && ok "7z 抽回来的大文件 md5 一致" || no "7z md5" "$a / $b"

echo "════ ③ archive.create → iso ════"
o=$($T archive.create --dst="$W/a.iso" --format=iso --src="$W/src" --iso-volume=TESTVOL 2>/dev/null)
[ "$(jg "$o" "d['udf']")" = "False" ] && ok "明确回 udf=false（不做 UDF）" || no "udf" "$o"
o2=$($T iso.ls --iso="$W/a.iso" --path=/ 2>/dev/null)
echo "$o2" | grep -q '"mtime":[1-9]' && ok "iso.ls 带 mtime（REQ-41 ②）" || no "iso mtime" "$o2"
$T iso.extract --iso="$W/a.iso" --path=/src/big.bin --dst="$W/big2.out" >/dev/null 2>&1
c=$(md5sum < "$W/big2.out")
[ "$a" = "$c" ] && ok "iso 抽回来的 md5 一致" || no "iso md5" "$a / $c"

echo "════ ④ rar 明确不支持 ════"
o=$($T archive.create --dst="$W/x.rar" --format=rar --src="$W/src/a.txt" 2>/dev/null)
[ "$(je "$o")" = "ARCHIVE-FORMAT-READONLY" ] && ok "--format=rar → ARCHIVE-FORMAT-READONLY" || no "rar" "$o"
echo "$o" | grep -q '7z' && ok "hint 提了改用 7z" || no "rar hint" "$o"

echo "════ ⑤ conflict ════"
cp "$W/a.zip" "$W/keep.zip"; before=$(md5sum < "$W/keep.zip")
o=$($T archive.create --dst="$W/keep.zip" --src="$W/src/a.txt" 2>/dev/null)
[ "$(je "$o")" = "ARCHIVE-EXISTS" ] && ok "默认 conflict=fail → ARCHIVE-EXISTS" || no "conflict fail" "$o"
[ "$before" = "$(md5sum < "$W/keep.zip")" ] && ok "失败后原包字节不变" || no "原包变了" ""
o=$($T archive.create --dst="$W/keep.zip" --src="$W/src/a.txt" --conflict=rename 2>/dev/null)
[ "$(jg "$o" "d['dst']")" != "$W/keep.zip" ] && ok "--conflict=rename 落到新名字" || no "rename" "$o"
o=$($T archive.create --dst="$W/keep.zip" --src="$W/src/a.txt" --conflict=overwrite 2>/dev/null)
[ "$(jg "$o" "d['replaced']")" = "True" ] && ok "--conflict=overwrite 标记 replaced" || no "overwrite" "$o"

echo "════ ⑥ 多源（REQ-42）════"
echo b > "$W/b.txt"; echo c > "$W/c.txt"
o=$($T archive.create --dst="$W/m.zip" --src="$W/src/a.txt,$W/b.txt,$W/src/sub" 2>/dev/null)
[ "$(jg "$o" "d['sources']")" = "3" ] && ok "逗号切 3 个源" || no "sources" "$o"
[ "$(jg "$o" "d['files']")" = "3" ] && ok "files=3（含目录里那棵）" || no "files" "$o"
printf 'z' > "$W/名字,带逗号.txt"
o=$($T archive.create --dst="$W/n.zip" --src="$W/名字\\,带逗号.txt" 2>/dev/null)
[ "$(jg "$o" "d['sources']")" = "1" ] && ok "\\, 转义 → 仍是一个源" || no "转义" "$o"
printf '# 注释\n%s\n\n%s\n%s\n' "$W/src/a.txt" "$W/b.txt" "$W/c.txt" > "$W/list.txt"
o=$($T archive.create --dst="$W/l.zip" --src-file="$W/list.txt" 2>/dev/null)
[ "$(jg "$o" "d['sources']")" = "3" ] && ok "--src-file 读 3 行（跳过注释/空行）" || no "src-file" "$o"

echo "════ ⑦ 写进压缩包（REQ-41 ①）════"
echo new > "$W/n.txt"
o=$($T copy --src="host:$W/n.txt" --dst="$W/a.zip!:/sub/n.txt" 2>/dev/null)
[ "$(jg "$o" "d['copied_files']")" = "1" ] && ok "copy → zip" || no "copy zip" "$o"
$T archive.extract --archive="$W/a.zip" --path=/sub/n.txt --dst="$W/n.out" >/dev/null 2>&1
[ "$(cat "$W/n.out")" = "new" ] && ok "抽回来内容一致" || no "内容" "$(cat "$W/n.out" 2>/dev/null)"
o=$($T copy --src="host:$W/n.txt" --dst="$W/a.zip!:/brand/new/x.txt" 2>/dev/null)
$T archive.ls --archive="$W/a.zip" --recursive=true 2>/dev/null | grep -q '"path":"/brand/new"' \
  && ok "父目录自动补进包" || no "父目录" "$o"
o=$($T copy --src="host:$W/n.txt" --dst="$W/a.zip!:/sub/n.txt" 2>/dev/null)
[ "$(je "$o")" = "COPY-DST-EXISTS" ] && ok "重复写 → COPY-DST-EXISTS" || no "dup" "$o"
o=$($T copy --src="host:$W/n.txt" --dst="$W/a.zip!:/sub/n.txt" --conflict=overwrite 2>/dev/null)
cnt=$($T archive.ls --archive="$W/a.zip" --recursive=true 2>/dev/null | python3 -c "import sys,json;print(sum(1 for e in json.load(sys.stdin)['data']['entries'] if e['path']=='/sub/n.txt'))")
[ "$cnt" = "1" ] && ok "overwrite 后只有一条 /sub/n.txt（没有重复条目）" || no "overwrite 重复" "cnt=$cnt"
o=$($T rm --path="$W/a.zip!:/brand" --recursive=true 2>/dev/null)
[ "$(jg "$o" "d['deleted']")" = "True" ] && ok "rm 包内子树" || no "rm" "$o"
$T archive.ls --archive="$W/a.zip" --recursive=true 2>/dev/null | grep -q '"path":"/brand' \
  && no "rm 没删干净" "" || ok "rm 后 /brand 不在包里"
echo mv > "$W/mv.txt"
o=$($T move --src="host:$W/mv.txt" --dst="$W/a.zip!:/moved.txt" --allow_host_delete=true 2>/dev/null)
[ -e "$W/mv.txt" ] && no "move 源没删" "" || ok "move → 包，源已删"
$T archive.ls --archive="$W/a.zip" --recursive=true 2>/dev/null | grep -q '"path":"/moved.txt"' \
  && ok "move 后条目在包里" || no "move 条目" ""

echo "════ ⑧ du（REQ-40）════"
expect_files=5; expect_dirs=3; expect_bytes=200022
o=$($T du --src="host:$W/src" 2>/dev/null)
[ "$(jg "$o" "d['files']")" = "$expect_files" ] && ok "宿主 files=$expect_files" || no "du files" "$o"
[ "$(jg "$o" "d['dirs']")" = "$expect_dirs" ] && ok "宿主 dirs=$expect_dirs（不含 src 自己）" || no "du dirs" "$o"
[ "$(jg "$o" "d['total_bytes']")" = "$expect_bytes" ] && ok "宿主 total_bytes=$expect_bytes" || no "du bytes" "$o"
o=$($T du --src="host:$W/src" --limit=2 2>/dev/null)
[ "$(jg "$o" "d['truncated']")" = "True" ] && ok "--limit=2 → truncated=true，不报错" || no "limit" "$o"
o=$($T du --src="$W/a.zip!:/src" 2>/dev/null)
[ "$(jg "$o" "d['files']")" = "3" ] && ok "包内 files=3（.trashed 不在包）" || no "du zip files" "$o"
# iso: 与 iso.ls 求和一致
o=$($T du --src="$W/a.iso!:/src" 2>/dev/null)
[ "$(jg "$o" "d['files']")" = "4" ] && ok "ISO 内 files=4（.trashed 跳过，j.tmp 在）" || no "du iso" "$o"

# NTFS：与 ntfs.list 求和一致
Q="$W/d.qcow2"
$T img.create --out="$Q" --size-gb=4 --table=gpt --esp-mb=200 --msr-mb=16 --sys-mb=2000 >/dev/null 2>&1
SYS=$($T part.show --img="$Q" 2>/dev/null | python3 -c "import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['data']['pt']['parts'] if p.get('sectors',0)>1000000][0])")
$T fs.format --img="$Q" --lba="$SYS" --fs=ntfs --label=SYS >/dev/null 2>&1
$T ntfs.mkdir --img="$Q" --lba="$SYS" --path=/sub >/dev/null 2>&1
$T ntfs.put --img="$Q" --lba="$SYS" --src="$W/src/a.txt" --dst=/a.txt >/dev/null 2>&1
$T ntfs.put --img="$Q" --lba="$SYS" --src="$W/src/big.bin" --dst=/sub/big.bin >/dev/null 2>&1
o=$($T du --src="$Q!lba$SYS:/" 2>/dev/null)
[ "$(jg "$o" "d['files']")" = "2" ] && ok "NTFS files=2" || no "du ntfs files" "$o"
[ "$(jg "$o" "d['dirs']")" = "1" ] && ok "NTFS dirs=1" || no "du ntfs dirs" "$o"
[ "$(jg "$o" "d['total_bytes']")" = "200006" ] && ok "NTFS total_bytes=200006" || no "du ntfs bytes" "$o"
[ "$(jg "$o" "d['alloc_bytes']")" -gt 0 ] 2>/dev/null && ok "NTFS 有 alloc_bytes（不糊弄）" || no "alloc" "$o"

echo "════ ⑨ 容器内路径明确拒绝（REQ-43）════"
o=$($T archive.create --dst="$W/c.zip" --src="$W/a.zip!:/src" 2>/dev/null)
[ "$(je "$o")" = "ARCHIVE-SRC-CONTAINER" ] && ok "包内路径 → ARCHIVE-SRC-CONTAINER（不是\"源不存在\"）" || no "容器源" "$o"

echo "════ ⑩ 空包语义（REQ-44 ②）════"
python3 -c "import zipfile; zipfile.ZipFile('$W/empty.zip','w').close()"
o=$($T archive.ls --archive="$W/empty.zip" 2>/dev/null)
[ "$(jg "$o" "d['format']")" = "zip" ] && [ "$(jg "$o" "d['total']")" = "0" ] \
  && ok "合法空 zip → format=zip, total=0（不是 empty，别当损坏）" || no "空 zip" "$o"
: > "$W/zero.bin"
o=$($T archive.ls --archive="$W/zero.bin" 2>/dev/null)
[ "$(jg "$o" "d['format']")" = "empty" ] && ok "0 字节文件 → format=empty" || no "0 字节" "$o"
tar cf "$W/empty.tar" --files-from /dev/null
o=$($T archive.ls --archive="$W/empty.tar" 2>/dev/null)
[ "$(jg "$o" "d['format']")" = "tar" ] && [ "$(jg "$o" "d['total']")" = "0" ] \
  && ok "空 tar → format=tar, total=0" || no "空 tar" "$o"

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]