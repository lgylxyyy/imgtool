#!/usr/bin/env bash
# REQ-31 压缩包只读：列目录 / 抽文件 / 抽目录 / 加密拒绝 / 7z solid
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="${IMGCMD:-$ROOT/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }

mkdir -p "$W/src/sub/deep" "$W/src/empty"
echo hello > "$W/src/a.txt"
head -c 200000 /dev/urandom > "$W/src/big.bin"
echo deep > "$W/src/sub/deep/d.txt"

command -v zip >/dev/null && ( cd "$W/src" && zip -qr "$W/t.zip" . )
command -v 7z  >/dev/null && ( cd "$W/src" && 7z a -bso0 -bsp0 "$W/t.7z" . >/dev/null )
tar czf "$W/t.tar.gz" -C "$W/src" .
tar cJf "$W/t.tar.xz" -C "$W/src" .
command -v gcab >/dev/null && gcab -c "$W/t.cab" "$W/src"/* 2>/dev/null

echo "════ ① 格式识别 ════"
for f in zip:zip 7z:7z tar.gz:tar tar.xz:tar; do
  ext=${f%%:*}; want=${f##*:}
  [ -f "$W/t.$ext" ] || continue
  got=$($T archive.ls --archive="$W/t.$ext" --limit=1 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)['data']['format'])" 2>/dev/null)
  [ "$got" = "$want" ] && ok "$ext → $want" || no "$ext 格式" "got=$got"
done

echo "════ ② 列目录 ════"
out=$($T archive.ls --archive="$W/t.zip" --recursive=true 2>/dev/null)
cnt=$(python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print(d['total'])" <<<"$out")
[ "$cnt" -ge 5 ] && ok "递归列全（total=$cnt）" || no "递归列" "$out"
paths=$(python3 -c "import sys,json;d=json.load(sys.stdin)['data'];[print(e['path']) for e in d['entries']]" <<<"$out")
echo "$paths" | grep -q '^/sub/deep/d.txt$' && ok "path 是 / 开头的包内绝对路径" || no "路径格式" "$paths"
out=$($T archive.ls --archive="$W/t.zip" --path=/ --limit=100 2>/dev/null)
nr=$(python3 -c "import sys,json;print(json.load(sys.stdin)['data']['total'])" <<<"$out")
[ "$nr" -lt "$cnt" ] && ok "非递归只列直接子项（$nr < $cnt）" || no "非递归" "$nr/$cnt"
kid=$(python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print([e['children_count'] for e in d['entries'] if e['name']=='sub'][0])" <<<"$out")
[ "$kid" -ge 1 ] && ok "目录 children_count=$kid" || no "children_count" "$kid"

echo "════ ③ 抽文件 ════"
$T archive.extract --archive="$W/t.zip" --path=/big.bin --dst="$W/o1.bin" >/dev/null 2>&1
cmp -s "$W/o1.bin" "$W/src/big.bin" && ok "zip 抽单文件字节一致" || no "zip 抽单文件" "$(ls -la "$W/o1.bin" 2>/dev/null)"
[ -f "$W/t.7z" ] && { $T archive.extract --archive="$W/t.7z" --path=/sub/deep/d.txt --dst="$W/o2.txt" >/dev/null 2>&1
  cmp -s "$W/o2.txt" "$W/src/sub/deep/d.txt" && ok "7z 抽单文件字节一致（solid）" || no "7z 单文件" ""; }
$T archive.extract --archive="$W/t.tar.xz" --path=/a.txt --dst="$W/o3.txt" >/dev/null 2>&1
cmp -s "$W/o3.txt" "$W/src/a.txt" && ok "tar.xz 抽单文件字节一致" || no "tar.xz 单文件" ""

echo "════ ④ 抽目录 ════"
$T archive.extract --archive="$W/t.zip" --path=/sub --dst="$W/od" >/dev/null 2>&1
[ -f "$W/od/deep/d.txt" ] && ok "抽整棵子树（保留结构）" || no "抽目录" "$(find "$W/od" 2>/dev/null)"
$T archive.extract --archive="$W/t.zip" --path=/sub --dst="$W/od" >/dev/null 2>&1
echo "$?" | grep -q 0 && no "非空目标应拒绝" "居然成功了" || ok "非空目标拒绝（不做合并）"
out=$($T archive.extract --archive="$W/t.zip" --path=/nope --dst="$W/x" 2>/dev/null)
echo "$out" | grep -q 'ARCHIVE-ENTRY-NOTFOUND' && ok "不存在 → ARCHIVE-ENTRY-NOTFOUND" || no "不存在" "$out"

echo "════ ⑤ 加密包 ════"
if command -v zip >/dev/null; then
  echo secret > "$W/enc.txt"
  ( cd "$W" && zip -q -P pass123 "$W/t-enc.zip" enc.txt )
  out=$($T archive.extract --archive="$W/t-enc.zip" --path=/enc.txt --dst="$W/enc.out" 2>/dev/null)
  echo "$out" | grep -q 'ARCHIVE-ENCRYPTED' && ok "加密条目 → ARCHIVE-ENCRYPTED（不做密码）" || no "加密" "$out"
fi

echo "════ ⑤b copy 直接从包里取（img_path 的 !:/ 语法）════"
rm -f "$W/via.txt"
out=$($T copy --src="$W/t.zip!:/a.txt" --dst="host:$W/via.txt" 2>/dev/null)
echo "$out" | grep -q '"ok":true' && cmp -s "$W/via.txt" "$W/src/a.txt" && ok "copy --src='包!:/内部路径'（0 临时）" || no "copy from archive" "$out"
out=$($T copy --src="host:$W/t.zip!:/sub/deep/d.txt" --dst="host:$W/via2.txt" 2>/dev/null)
echo "$out" | grep -q '"ok":true' && ok "host: 前缀 + ! 也认" || no "host: + !" "$out"
# REQ-41 之后：zip 可写（整包重写）。只测"写进去能读回来"，细节在 archive-write-test.sh
out=$($T copy --src="$W/src/a.txt" --dst="$W/t.zip!:/inject.txt" 2>/dev/null)
echo "$out" | grep -q '"ok":true' || no "写进压缩包" "$out"
$T archive.extract --archive="$W/t.zip" --path=/inject.txt --dst="$W/inj.out" >/dev/null 2>&1
cmp -s "$W/inj.out" "$W/src/a.txt" && ok "写进 zip 再抽回来字节一致（REQ-41）" || no "写回不一致" ""

echo "════ ⑥ 坏文件 ════"
head -c 5000 /dev/urandom > "$W/bad.bin"
out=$($T archive.ls --archive="$W/bad.bin" 2>/dev/null)
echo "$out" | grep -qE 'ARCHIVE-OPEN-FAIL|ARCHIVE-NOTFOUND' && ok "乱码文件 → ARCHIVE-OPEN-FAIL" || no "坏文件" "$out"

echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]
