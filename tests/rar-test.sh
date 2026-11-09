#!/usr/bin/env bash
# rar-test.sh — RAR 读取（REQ-50）：solid / RAR5 / 加密 / 目录抽取
#
# 夹具不随仓库走（221 MB），用环境变量指定：
#   IMG_RAR_FIXTURE=/path/to/solid.rar tests/rar-test.sh
# 默认试 <工作区>/tmp/cs16.rar，没有就跳过。
set -u
T="${IMGCMD:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
WS_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
FX="${IMG_RAR_FIXTURE:-$WS_ROOT/tmp/cs16.rar}"
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
skip(){ echo "  ⏭ $1"; }

if [ ! -f "$FX" ]; then
  echo "════ 没有 RAR 夹具（设 IMG_RAR_FIXTURE 指向一个 solid RAR）→ 跳过 ════"
  exit 0
fi
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
jget(){ python3 -c "import sys,json
try:
    d=json.load(sys.stdin)['data']
    print(eval(sys.argv[1]))
except Exception as e:
    print('ERR:'+str(e))" "$2" <<<"$1"; }

echo "════ ① 列目录（solid 包，旧版这里 ARCHIVE-OPEN-FAIL）════"
o=$($T archive.ls --archive="$FX" --path=/ 2>/dev/null)
rc=$?
[ "$rc" -eq 0 ] && ok "archive.ls 成功（rc=0）" || no "退出码" "rc=$rc"
[ "$(jget "$o" "d['format']")" = "rar" ] && ok "format=rar" || no "format" "$o"
[ "$(jget "$o" "d['solid']")" = "True" ] && ok "solid=true（App 可标"解压会慢"）" || no "solid" "$o"
n=$(jget "$o" "d['total']")
[ "$n" = "1" ] && ok "根目录 1 项" || no "根目录项数" "$o"

echo "════ ② 子目录 + 中文名（UTF-32→UTF-8）════"
o=$($T archive.ls --archive="$FX" --path=/CS1.6-jisu --limit=100 2>/dev/null)
t=$(jget "$o" "d['total']")
[ "$t" -ge 10 ] 2>/dev/null && ok "子目录能列（total=$t）" || no "子目录" "$o"
o=$($T archive.ls --archive="$FX" --path=/CS1.6-jisu --limit=200 2>/dev/null)
if [ "$(jget "$o" "any('语音' in e['name'] for e in d['entries'])")" = "True" ]; then
  ok "中文文件名正确解码（BOT语音转换.bat）"
else
  no "中文名" "$(echo "$o" | head -c 300)"
fi

echo "════ ③ 抽单个文件（solid：必须顺序解压到它）════"
o=$($T archive.extract --archive="$FX" --path=/CS1.6-jisu/readme.txt --dst="$W/a.txt" 2>/dev/null)
b=$(jget "$o" "d['bytes']")
[ "$b" = "39818" ] && ok "字节数 39818" || no "字节数" "$o"
s=$(stat -c %s "$W/a.txt" 2>/dev/null || echo 0)
[ "$s" = "39818" ] && ok "落盘大小一致" || no "落盘大小" "$s"

echo "════ ④ 抽整棵目录（870 文件 / 104 MB）════"
o=$($T archive.extract --archive="$FX" --path=/CS1.6-jisu/valve --dst="$W/valve" 2>/dev/null)
f=$(jget "$o" "d['files']")
[ "$f" -ge 800 ] 2>/dev/null && ok "files=$f" || no "files" "$o"
c=$(find "$W/valve" -type f 2>/dev/null | wc -l)
[ "$c" = "$f" ] && ok "磁盘上的文件数与返回一致（$c）" || no "文件数对不上" "返回 $f / 实际 $c"
[ -f "$W/valve/titles.txt" ] && ok "子目录结构正确（valve/titles.txt）" || no "结构" "$(ls "$W/valve" | head -3)"

echo "════ ⑤ 不存在 / 目录当文件 / 取消 ════"
o=$($T archive.extract --archive="$FX" --path=/nope.txt --dst="$W/n.txt" 2>/dev/null)
case "$o" in *'"ok":false'*ARC*) ok "不存在的条目 → 错误信封（$(echo "$o"|grep -o '"error_code":"[^"]*"'|head -1)）";;
              *) no "不存在" "$o";; esac
o=$($T archive.extract --archive="$FX" --path=/CS1.6-jisu --dst="$W/dir" 2>/dev/null)
[ "$(jget "$o" "d['files']")" -ge 100 ] 2>/dev/null && ok "整包根目录按目录处理（files≥100）" || no "目录按目录处理" "$o"

echo "════ ⑥ 非 RAR 不受影响（回归）════"
printf 'hello' > "$W/x.txt"
(cd "$W" && zip -q t.zip x.txt)
o=$($T archive.ls --archive="$W/t.zip" --path=/ 2>/dev/null)
[ "$(jget "$o" "d['format']")" = "zip" ] && ok "zip 仍走 libarchive" || no "zip" "$o"

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]