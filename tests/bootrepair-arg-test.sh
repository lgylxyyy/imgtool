#!/usr/bin/env bash
# bootrepair-arg-test.sh — REQ-48 §3：`--vbr-tpl=builtin` 不许再当参数错误
# 真机上 App 按 help 里的字面传 builtin → CMD-ARG-BAD@vbr-tpl → BIOS 修复一直失败。
set -u
T="${IMGCMD:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✅ $1"; }
no(){ fail=$((fail+1)); echo "  ❌ $1"; echo "      $2"; }
code(){ python3 -c "import sys,json;print(json.load(sys.stdin).get('err',{}).get('error_code',''))" <<<"$1"; }
hint(){ python3 -c "import sys,json;print(json.load(sys.stdin).get('err',{}).get('hint',''))" <<<"$1"; }

echo "════ ① builtin 被接受（只会在打开镜像那步失败）════"
o=$($T boot.repair --img=/nonexistent.qcow2 --sys-lba=2048 --vbr-tpl=builtin 2>/dev/null)
c=$(code "$o")
[ "$c" != "CMD-ARG-BAD" ] && ok "--vbr-tpl=builtin 不再报 CMD-ARG-BAD（实际：$c）" || no "builtin" "$o"

echo "════ ② default / 内置 同样接受 ════"
for v in default 内置; do
  o=$($T boot.repair --img=/nonexistent.qcow2 --sys-lba=2048 --vbr-tpl="$v" 2>/dev/null)
  c=$(code "$o"); [ "$c" != "CMD-ARG-BAD" ] && ok "--vbr-tpl=$v 接受" || no "--vbr-tpl=$v" "$o"
done

echo "════ ③ 真写错格式时，hint 要说清\"不传就是内置\" ════"
# 需要一个能打开的镜像（命令框架会先开 --img），现造一个 96 MB 的
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
"$(dirname "$T")/imglayout" mk "$W/t.qcow2" 96MiB gpt --esp 48 >/dev/null 2>&1
o=$($T boot.repair --img="$W/t.qcow2" --sys-lba=2048 --vbr-tpl=xx 2>/dev/null)
c=$(code "$o"); h=$(hint "$o")
[ "$c" = "CMD-ARG-BAD" ] && ok "xx → CMD-ARG-BAD" || no "xx 错误码" "$o"
case "$h" in *内置*) ok "hint 里有\"内置模板\"的说明" ;; *) no "hint" "$h" ;; esac

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ "$fail" -eq 0 ]