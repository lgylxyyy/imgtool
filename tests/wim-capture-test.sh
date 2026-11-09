#!/bin/bash
# wim-capture-test.sh — wim.capture 端到端：捕获、临时目录清理、产物可读
#
# 关注点：
#   ① 捕获成功，产物 WIM 能被 wim.ls 读出来
#   ② 捕获用的临时目录被删干净（现在用 img_tmpdir_remove()，不再 system("rm -rf")）
#   ③ estimate=true 只估算，不建临时目录、不产出文件
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
W=$(mktemp -d /tmp/wimcapXXXXXX)
pass=0; fail=0
ok(){ if [ "$1" = "1" ]; then echo "  ✅ $2"; pass=$((pass+1)); else echo "  ❌ $2"; fail=$((fail+1)); fi; }
q(){ "$T/imgcmd" "$@" --progress=none 2>/dev/null; }
jget(){ python3 -c "import json,sys; d=json.load(sys.stdin); v=d.get('data',{}); print($1)" 2>/dev/null; }

echo "════ wim.capture ════"
# 1 GiB qcow2，MBR：一个 700 MiB NTFS 分区
q img.create --out="$W/s.qcow2" --size-gb=1 --table=mbr --sys-mb=128 --win-mb=700 --limits=off >/dev/null
LBA=$(q part.show --img="$W/s.qcow2" | jget "sorted([p['first'] for p in d['data']['pt']['parts'] if p['sectors']>100000])[-1]")
if [ -z "$LBA" ]; then echo "  ❌ 造盘失败"; exit 1; fi
q fs.format --img="$W/s.qcow2" --lba="$LBA" --fs=ntfs --label=CAP >/dev/null
mkdir -p "$W/src/Windows/System32"
head -c 65536 /dev/urandom > "$W/src/Windows/System32/ntoskrnl.exe"
echo "capture me" > "$W/src/Windows/System32/a.txt"
q ntfs.put --img="$W/s.qcow2" --lba="$LBA" --src="$W/src/Windows/System32/ntoskrnl.exe" --dst=/Windows/System32/ntoskrnl.exe >/dev/null
q ntfs.put --img="$W/s.qcow2" --lba="$LBA" --src="$W/src/Windows/System32/a.txt" --dst=/Windows/System32/a.txt >/dev/null

# 临时目录单独放，便于检查是否清干净
export IMGTOOL_TMPDIR="$W/tmp"
mkdir -p "$IMGTOOL_TMPDIR"

# ① estimate=true：只估算
d=$(q wim.capture --img="$W/s.qcow2" --lba="$LBA" --estimate=true)
est_ok=$(echo "$d" | jget "d.get('ok')")
need=$(echo "$d" | jget "d.get('data',{}).get('bytes_needed',0)")
[ "$est_ok" = "True" ] && ok 1 "estimate=true 返回结果" || ok 0 "estimate=true 失败：$(echo "$d" | head -c 200)"
[ "${need:-0}" -gt 0 ] && ok 1 "bytes_needed=${need}（临时树体积）" || ok 0 "bytes_needed 异常：${need:-无}"
[ -z "$(ls -A "$IMGTOOL_TMPDIR" 2>/dev/null)" ] && ok 1 "estimate 不留临时目录" || ok 0 "estimate 留下了临时目录"

# ② 真捕获
d=$(q wim.capture --img="$W/s.qcow2" --lba="$LBA" --out="$W/out.wim" --comp=none)
cap_ok=$(echo "$d" | jget "d.get('ok')")
[ "$cap_ok" = "True" ] && ok 1 "捕获成功" || ok 0 "捕获失败：$(echo "$d" | head -c 300)"
[ -s "$W/out.wim" ] && ok 1 "产物 WIM 非空（$(stat -c%s "$W/out.wim" 2>/dev/null) 字节）" || ok 0 "产物 WIM 为空"
# ★ 临时目录必须被清干净
left=$(ls -A "$IMGTOOL_TMPDIR" 2>/dev/null | wc -l)
[ "$left" = "0" ] && ok 1 "临时目录已清干净" || ok 0 "临时目录残留 $left 项"

# ③ 产物能读回来
n=$(q wim.ls --wim="$W/out.wim" --index=1 --path=/Windows/System32 | jget "len(d.get('data',{}).get('entries',[]))")
[ "${n:-0}" -ge 2 ] && ok 1 "产物能列出 $n 项" || ok 0 "产物读不出内容（${n:-无}）"

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ "$fail" = "0" ]; then rm -rf "$W"; else echo "调试目录：$W"; fi
[ "$fail" = "0" ]
