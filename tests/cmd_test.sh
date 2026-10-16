#!/bin/bash
# cmd_test.sh — 命令表骨架验收（成功 / 失败 / 参数校验 / 日志）
# 用法: tests/cmd_test.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
W=$(mktemp -d /tmp/cmdtestXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

echo "════ 0. 造一张小盘（20GiB / ESP 200MB / FAT32）════"
$T/imglayout mk "$W/t.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1 || { echo "建盘失败"; exit 2; }
LBA=$($T/imgpart show "$W/t.qcow2" | python3 -c "import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
$T/imgmkfs "$W/t.qcow2" --mkfs "$LBA" TEST >/dev/null 2>&1
echo "  盘=$W/t.qcow2  ESP LBA=$LBA"

echo "════ 1. 自描述：--list / --help（App 靠它生成界面）════"
$T/imgcmd --list > "$W/list.json" 2>/dev/null
n=$(jq_ "len(d['cmds'])" < "$W/list.json")
[ "${n:-0}" -ge 5 ] && ok "--list 输出 $n 个命令（JSON 可解析）" || ng "--list 异常"
jq_ "[a['name'] for c in d['cmds'] if c['name']=='fat.put' for a in c['args']]" < "$W/list.json" | grep -q "check_size" \
  && ok "--list 带参数规格（App 可据此自动生成控件）" || ng "--list 缺参数规格"
$T/imgcmd --help=fat.put 2>/dev/null | grep -q "check_size" && ok "--help 从表自动生成" || ng "--help 异常"

echo "════ 2. 成功路径 ════"
$T/imgcmd img.info --img="$W/t.qcow2" > "$W/i.json" 2>/dev/null
[ "$(jq_ "d['ok']" < "$W/i.json")" = "True" ] && [ "$(jq_ "d['data']['table']" < "$W/i.json")" = "GPT" ] \
  && ok "img.info → table=GPT size=$(jq_ "d['data']['size_bytes']" < "$W/i.json")" || ng "img.info 异常：$(head -c 200 "$W/i.json")"
$T/imgcmd part.show --img="$W/t.qcow2" > "$W/p.json" 2>/dev/null
[ "$(jq_ "d['data']['pt']['parts'][0]['typeGuid'][:8]" < "$W/p.json")" = "c12a7328" ] && ok "part.show → 首分区是 ESP" || ng "part.show 异常"
head -c 6000 /etc/services > "$W/blob.bin"
$T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src="$W/blob.bin" --dst=/EFI/blob.bin > "$W/put.json" 2>/dev/null
[ "$(jq_ "d['data']['written_bytes']" < "$W/put.json")" = "$(stat -c%s "$W/blob.bin")" ] && ok "fat.put 写入成功（默认不校验空间）" || ng "fat.put 异常：$(head -c 200 "$W/put.json")"
$T/imgcmd fat.ls --img="$W/t.qcow2" --lba=$LBA --path=/EFI > "$W/ls.json" 2>/dev/null
[ "$(jq_ "[e['name'] for e in d['data']['entries']]" < "$W/ls.json" | grep -c blob.bin)" = "1" ] && ok "fat.ls 看到刚写的文件" || ng "fat.ls 异常"

echo "════ 3. check_size 开关（默认关 / 显式开）════"
head -c 400000000 /dev/zero > "$W/huge.bin"
$T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src="$W/huge.bin" --dst=/EFI/huge.bin --check_size=1 > "$W/e1.json" 2>/dev/null
code=$(jq_ "d['err']['error_code']" < "$W/e1.json")
[ "$code" = "FAT-ENOSPC" ] && ok "check_size=1 → 提前拦下（$(jq_ "d['err']['why']" < "$W/e1.json")）" || ng "check_size 未拦截（code=$code）"
rm -f "$W/huge.bin"      # 400MB 临时文件立刻删（/tmp 是内存盘，别占着）

echo "════ 4. 失败与参数校验（每条都要 信封+错误码+建议+非零退出）════"
check_err(){
  local d="$1" want="$2"; shift 2
  set -- "$@" --progress=bar     # 失败卡片只在 human 模式渲染（stderr 不是 tty 时要显式开）
  local f="$W/err.json"
  local card="$W/err.txt"
  "$@" > "$f" 2> "$card"; local rc=$?      # stdout=JSON 信封，stderr=失败卡片（CLI 渲染）
  local got; got=$(jq_ "d['err']['error_code']" < "$f")
  local sug=0
  grep -qE "建议    |Suggest " "$card" && sug=1
  if [ "$got" = "$want" ] && [ "$sug" = "1" ] && [ $rc -ne 0 ]; then ok "$d → $got（卡片含建议）"
  else ng "$d → 期望 $want 实得 $got（rc=$rc 卡片建议=$sug）"; fi
}
check_err "未知命令"          CMD-NOTFOUND      $T/imgcmd nosuch.cmd
check_err "缺必填参数 --img"   CMD-ARG-BAD       $T/imgcmd img.info
check_err "数字参数传成文字"    CMD-ARG-BAD       $T/imgcmd img.info --img="$W/t.qcow2" --lba=abc
check_err "LBA=0（MBR 区）"   CMD-ARG-BAD       $T/imgcmd fat.ls --img="$W/t.qcow2" --lba=0
check_err "源文件不存在"       SRC-NOTFOUND      $T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src=/no/such --dst=/x
check_err "LBA 上不是 FAT32"   FAT-MOUNT-FAIL    $T/imgcmd fat.ls --img="$W/t.qcow2" --lba=999999
check_err "镜像不存在"         IMG-OPEN-FAIL     $T/imgcmd img.info --img=/no/such.qcow2
check_err "目录不存在"         FAT-DIR-NOTFOUND  $T/imgcmd fat.ls --img="$W/t.qcow2" --lba=$LBA --path=/nope

check_err "wim.apply 非法 format 值" "CMD-ARG-BAD" \
  "$T/imgcmd" wim.apply --img="$W/t.qcow2" --lba=$LBA --wim=/nope.esd --format=maybe --progress=bar
check_err "wim.apply 缺必填 wim" "CMD-ARG-BAD" \
  "$T/imgcmd" wim.apply --img="$W/t.qcow2" --lba=$LBA --progress=bar
check_err "wim.apply LBA 不在分区表" "CMD-ARG-BAD" \
  "$T/imgcmd" wim.apply --img="$W/t.qcow2" --lba=999999 --wim=/nope.esd --progress=bar

echo "════ 4a. 宿主空间（默认开、不可关）════"
$T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src="$W/blob.bin" --dst=/EFI/blk2.bin \
    --host_free=1000000 > "$W/host.json" 2>/dev/null
if [ "$(jq_ "d['err']['error_code']" < "$W/host.json")" = "HOST-ENOSPC" ]; then
  ok "写前就拒绝（$(jq_ "d['err']['why']" < "$W/host.json")）"
else ng "宿主空间没拦住（code=$(jq_ "d['err']['error_code']" < "$W/host.json")）"; fi
got=$($T/imgcmd fat.ls --img="$W/t.qcow2" --lba=$LBA --path=/EFI 2>/dev/null | jq_ "[e['name'] for e in d['data']['entries']]" | grep -c "blk2.bin")
[ "${got:-0}" = "0" ] && ok "拒绝后【没写入】任何字节（不留半成品）" || ng "拒绝后居然写进去了 $got 个"
$T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src="$W/blob.bin" --dst=/EFI/blk3.bin \
    --reserve=1 > "$W/res.json" 2>/dev/null
[ "$(jq_ "d['ok']" < "$W/res.json")" = "True" ] && ok "reserve=1 正常执行（fallocate 失败也只警告不拦）" || ng "reserve=1 失败：$(head -c 160 "$W/res.json")"

echo "════ 4b. wim.size 尺寸估算（工具给数字，App 定政策）════"
ESD=$(ls /workspace/*.esd 2>/dev/null | head -1)
if [ -n "$ESD" ]; then
  $T/imgcmd wim.size --wim="$ESD" --index=1 > "$W/w.json" 2>/dev/null
  d=$(jq_ "d['data']['data_bytes']" < "$W/w.json"); e=$(jq_ "d['data']['est_ntfs_bytes']" < "$W/w.json")
  f=$(jq_ "d['data']['file_count']" < "$W/w.json")
  if [ "${e:-0}" -gt "${d:-0}" ] && [ "${d:-0}" -gt 0 ]; then
    ok "wim.size → 数据 ${d} 字节 / 文件 $f / 估算 ${e} 字节（含元数据+余量）"
  else ng "wim.size 异常：$(head -c 200 "$W/w.json")"; fi
  $T/imgcmd wim.size --wim="$ESD" --index=999 > "$W/w2.json" 2>/dev/null
  [ "$(jq_ "d['err']['error_code']" < "$W/w2.json")" = "WIM-IDX-INVALID" ] && ok "非法索引 → WIM-IDX-INVALID（含建议）" || ng "非法索引没拦住"
else
  echo "  ⏭ 跳过（没找到 .esd 样本）"
fi

echo "════ 5. 日志（--log）：里程碑 + 错误都落盘，且逐行可解析 ════"
$T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src="$W/blob.bin" --dst=/EFI/blob2.bin \
    --log="$W/run.log" --log-level=info >/dev/null 2>&1
[ -s "$W/run.log" ] && ok "日志已生成 $(wc -l < "$W/run.log") 行" || ng "日志没生成"
grep -q "event=session" "$W/run.log" && ok "有会话头（版本/命令/镜像）" || ng "缺会话头"
grep -q "event=begin"  "$W/run.log" && ok "有里程碑 begin" || ng "缺 begin"
grep -q "event=done"   "$W/run.log" && ok "有里程碑 done（带 ms/rc）" || ng "缺 done"
$T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src="$W/huge.bin" --dst=/EFI/h2 --check_size=1 \
    --log="$W/err.log" >/dev/null 2>&1
grep -q "level=ERROR" "$W/err.log" && ok "错误进日志（E 级任何级别都不漏）" || ng "错误没进日志"
python3 "$ROOT/tests/logfmt_check.py" "$W/run.log" && ok "日志每行都是合法 logfmt（time=/level=/event= 齐全、引号成对）" || ng "日志格式有坏行"
echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
if [ $fail -eq 0 ]; then rm -rf "$W"; else echo "  （有失败，调试目录保留：$W）"; fi
[ $fail -eq 0 ]
