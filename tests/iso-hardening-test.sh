#!/usr/bin/env bash
# REQ-45：iso.ls 在真机 SIGSEGV 的回归
#   真凶：UDF 列目录时给 FE 取 mtime，rd_lb 的第 4 参是【块数】，传成了 sizeof(buf) → 往 2KB 栈里读 4MB。
#   同时锁死：多 extent 目录要紧凑拼接、FID 记录长度要夹住、截断文件不许崩。
set -u
T="${IMGCMD:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
jget(){ python3 -c "import sys,json
try:
    d=json.load(sys.stdin)['data']
    print(eval(sys.argv[1]))
except Exception as e:
    print('ERR:'+str(e))" "$2" <<<"$1"; }

FIX="$(cd "$(dirname "$0")" && pwd)/udf-fixture.py"
python3 "$FIX" "$W/u.iso" >/dev/null 2>&1
python3 "$FIX" "$W/bogus.iso" --bogus >/dev/null 2>&1

echo "════ ① UDF 列目录（旧代码这里必崩）════"
$T iso.ls --iso="$W/u.iso" --path=/ >"$W/out.json" 2>"$W/err.log"
rc=$?
[ "$rc" -eq 0 ] && ok "iso.ls 正常退出（rc=0，不是 139）" || no "退出码" "rc=$rc $(tail -2 "$W/err.log")"
o=$(cat "$W/out.json")
[ "$(jget "$o" "d['count']")" = "2" ] && ok "两个 extent 都读到（count=2）" || no "count" "$o"
[ "$(jget "$o" "d['kind']")" = "udf" ] && ok "kind=udf（走了 UDF 分支）" || no "kind" "$o"
[ "$(jget "$o" "[e['size'] for e in d['entries'] if e['name']=='a.txt'][0]")" = "12345" ] \
  && ok "文件大小取自 FE（12345）" || no "size" "$o"
[ "$(jget "$o" "[e['mtime'] for e in d['entries'] if e['name']=='a.txt'][0]")" -gt 0 ] 2>/dev/null \
  && ok "mtime 取自 FE（UDF）" || no "mtime" "$o"

echo "════ ② 坏 FID（Implementation-Use 长度离谱）════"
$T iso.ls --iso="$W/bogus.iso" --path=/ >"$W/b.json" 2>/dev/null
rc=$?
[ "$rc" -eq 0 ] && ok "不崩（rc=0），坏记录被夹住" || no "坏 FID 退出码" "rc=$rc"
[ "$(jget "$(cat "$W/b.json")" "d['count']")" = "1" ] \
  && ok "只列到能读的那条（count=1）" || no "坏 FID count" "$(cat "$W/b.json")"

echo "════ ③ iso.probe：只读卷描述符（两段式预检）════"
o=$($T iso.probe --iso="$W/u.iso" 2>/dev/null)
[ "$(jget "$o" "d['udf']")" = "True" ] && ok "probe 认 UDF" || no "probe udf" "$o"
[ "$(jget "$o" "d['volume']")" = "UDFTEST" ] && ok "probe 带卷标" || no "probe vol" "$o"
[ "$(jget "$o" "d['label']")" = "UDFTEST" ] && ok "probe 带 label（REQ-46 §3.2）" || no "probe label" "$o"
o=$($T iso.probe --iso="$W/nothere.iso" 2>/dev/null)
echo "$o" | grep -q '"ok":false' && ok "不存在 → 错误信封（不崩）" || no "probe 不存在" "$o"

echo "════ ④ 截断模糊：任何长度都不许 SIGSEGV ════"
sz=$(stat -c %s "$W/u.iso")
bad=0
for cut in 0 512 4096 20000 2080768 3072000 "$((sz/2))" "$((sz-1))"; do
  [ "$cut" -le "$sz" ] || continue
  head -c "$cut" "$W/u.iso" > "$W/t.iso"
  $T iso.ls --iso="$W/t.iso" --path=/ >/dev/null 2>&1
  r=$?
  if [ "$r" -ge 128 ]; then bad=1; echo "      cut=$cut → rc=$r"; fi
  $T iso.probe --iso="$W/t.iso" >/dev/null 2>&1
  r=$?
  if [ "$r" -ge 128 ]; then bad=1; echo "      probe cut=$cut → rc=$r"; fi
done
[ "$bad" -eq 0 ] && ok "8 种截断长度全部无崩溃" || no "截断模糊" "有 rc>=128（信号）"

echo "════ ⑤ ISO9660 盘仍然正常（kind 字段）════"
mkdir -p "$W/src" && echo hello > "$W/src/a.txt"
if command -v xorriso >/dev/null 2>&1; then
  xorriso -as mkisofs -quiet -o "$W/plain.iso" "$W/src" 2>/dev/null
  o=$($T iso.ls --iso="$W/plain.iso" --path=/ 2>/dev/null)
  [ "$(jget "$o" "d['kind']")" = "iso9660" ] && ok "普通 ISO → kind=iso9660" || no "iso9660 kind" "$o"
  o=$($T iso.ls --iso="$W/plain.iso" --path=/ --recursive=true 2>/dev/null)
  [ "$(jget "$o" "d['count']")" -ge 1 ] 2>/dev/null && ok "递归列目录正常" || no "递归" "$o"
else
  echo "  ⏭ 没有 xorriso，跳过"
fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]