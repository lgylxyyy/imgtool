#!/usr/bin/env bash
# 7z-solid-test.sh — REQ-51 A：archive.ls 的 solid 对 7z 也要准
#   固实（-ms=on）→ true；非固实（-ms=off）→ false；单文件 → false；zip → false。
# 需要宿主 7z（只用来造夹具）；没有就跳过。
set -u
T="${IMGCMD:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
jget(){ python3 -c "import sys,json
try:
    d=json.load(sys.stdin)['data']
    print(eval(sys.argv[1]))
except Exception as e:
    print('ERR:'+str(e))" "$2" <<<"$1"; }

if ! command -v 7z >/dev/null 2>&1; then
  echo "════ 没有 7z 命令 → 跳过 ════"; exit 0
fi
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cd "$W" || exit 1
for i in 1 2 3; do head -c 120000 /dev/urandom > f$i.bin; done
7z a -t7z -ms=on  solid.7z   f1.bin f2.bin f3.bin >/dev/null
7z a -t7z -ms=off nosolid.7z f1.bin f2.bin f3.bin >/dev/null
7z a -t7z one.7z f1.bin >/dev/null
7z a -t7z -mhc=off -ms=on nohdr.7z f1.bin f2.bin >/dev/null 2>&1 || true
printf 'hello' > x.txt && zip -q t.zip x.txt

echo "════ ① 固实 7z（默认压缩头）════"
o=$($T archive.ls --archive="$W/solid.7z" --path=/ 2>/dev/null)
[ "$(jget "$o" "d['format']")" = "7z" ] && ok "format=7z" || no "format" "$o"
[ "$(jget "$o" "d['solid']")" = "True" ] && ok "solid=true" || no "solid" "$o"

echo "════ ② 非固实 7z ════"
o=$($T archive.ls --archive="$W/nosolid.7z" --path=/ 2>/dev/null)
[ "$(jget "$o" "d['solid']")" = "False" ] && ok "solid=false" || no "solid" "$o"

echo "════ ③ 单文件 7z ════"
o=$($T archive.ls --archive="$W/one.7z" --path=/ 2>/dev/null)
[ "$(jget "$o" "d['solid']")" = "False" ] && ok "单文件 → false" || no "single" "$o"

echo "════ ④ 未压缩头（-mhc=off）也要认 ════"
if [ -f "$W/nohdr.7z" ]; then
  o=$($T archive.ls --archive="$W/nohdr.7z" --path=/ 2>/dev/null)
  [ "$(jget "$o" "d['solid']")" = "True" ] && ok "未压缩头 + 固实 → true" || no "nohdr" "$o"
else
  echo "  ⏭ 这张 7z 不支持 -mhc=off，跳过"
fi

echo "════ ⑤ zip 永远 false（没有 solid 概念）════"
o=$($T archive.ls --archive="$W/t.zip" --path=/ 2>/dev/null)
[ "$(jget "$o" "d['solid']")" = "False" ] && ok "zip solid=false" || no "zip" "$o"

echo "════ ⑥ 坏 7z 不许崩 ════"
head -c 300 "$W/solid.7z" > "$W/bad.7z"
$T archive.ls --archive="$W/bad.7z" --path=/ >/dev/null 2>&1
r=$?
[ "$r" -lt 128 ] && ok "截断的 7z 不崩（rc=$r）" || no "坏包" "rc=$r"

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]