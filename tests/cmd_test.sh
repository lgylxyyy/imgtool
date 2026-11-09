#!/bin/bash
# cmd_test.sh — 命令表骨架验收（成功 / 失败 / 参数校验 / 日志）
# 用法: tests/cmd_test.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
# 外部素材与构建根统一由 scripts/env.sh 解析（环境变量 > imgtool.conf > 自动探测）
# shellcheck disable=SC1091
. "$ROOT/scripts/env.sh"
ASSETS="$IMGTOOL_IMAGES"
ISO_WIN10="$IMGTOOL_ISO_WIN10"
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
rm -f "$W/blob.out"
$T/imgcmd fat.export --img="$W/t.qcow2" --lba=$LBA --path=/EFI/blob.bin --dst="$W/blob.out" --progress=none >/dev/null 2>&1
[ "$(md5sum < "$W/blob.bin")" = "$(md5sum < "$W/blob.out" 2>/dev/null)" ] && ok "fat.export 抽回来 md5 一致（UEFI 的 BCD 靠它）" || ng "fat.export 异常"

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
# 2026-10-24 合并后：旧命令必须报 CMD-NOTFOUND（即使 --img 不存在，也不能报 IMG-OPEN-FAIL）
check_err "旧命令 ntfs.copy 已移除" CMD-NOTFOUND $T/imgcmd ntfs.copy --img=/no/such.qcow2 --lba=1 --src=/a --dst=/b
check_err "旧命令 file.copy 已移除" CMD-NOTFOUND $T/imgcmd file.copy --src=ntfs:/a --dst=ntfs:/b
# 位置参数边界：copy 最多 2 个；img.info 不支持位置参数；rm 只收 1 个
$T/imgcmd copy /a /b extra >/dev/null 2>&1; [ $? -eq 2 ] && ok "copy 位置参数过多 → 退出 2" || ng "copy 位置参数过多没拦住"
$T/imgcmd img.info /x >/dev/null 2>&1; [ $? -eq 2 ] && ok "非文件操作命令不接受位置参数 → 退出 2" || ng "img.info 位置参数没拦住"
$T/imgcmd rm /a extra >/dev/null 2>&1; [ $? -eq 2 ] && ok "rm 位置参数过多 → 退出 2" || ng "rm 位置参数过多没拦住"
$T/imgcmd --help=copy 2>&1 | grep -q "用法: imgcmd copy" && ok "--help=copy 显示位置参数用法" || ng "--help=copy 没有用法行"
check_err "缺必填参数 --img"   CMD-ARG-BAD       $T/imgcmd img.info
check_err "数字参数传成文字"    CMD-ARG-BAD       $T/imgcmd img.info --img="$W/t.qcow2" --lba=abc
check_err "LBA=0（MBR 区）"   CMD-ARG-BAD       $T/imgcmd fat.ls --img="$W/t.qcow2" --lba=0
check_err "源文件不存在"       SRC-NOTFOUND      $T/imgcmd fat.put --img="$W/t.qcow2" --lba=$LBA --src=/no/such --dst=/x
check_err "LBA 上不是 FAT32"   FAT-MOUNT-FAIL    $T/imgcmd fat.ls --img="$W/t.qcow2" --lba=999999
check_err "镜像不存在"         IMG-OPEN-FAIL     $T/imgcmd img.info --img=/no/such.qcow2

# ★ 真回归（手册 §4.5）：目录存在时，命令表入口【绝不创建 0 字节镜像】。
#   上面那条走的是 /no/…（目录都不存在），旧实现也会失败，抓不到这个坑 ——
#   旧代码在"目录存在、文件不存在"时会静默建出一个空文件并报 QC-OPEN-FAIL。
NEWH="$W/typo.qcow2"
code=$($T/imgcmd img.info --img="$NEWH" 2>/dev/null | jq_ "d['err']['error_code']")
[ "$code" = "IMG-OPEN-FAIL" ] && [ ! -e "$NEWH" ] \
  && ok "可写入口不创建空镜像（选错路径不留垃圾文件）" \
  || ng "可写入口：code=$code 文件=$([ -e "$NEWH" ] && echo "被创建了(错)" || echo 无)"
# boot.repair 自己也开一次镜像（不经 ctx）；CLI 会先开 ctx，所以这里先命中 IMG-OPEN-FAIL
# —— 两条路径都改成"绝不创建"，故只断言：非零 + 没有留下文件。
code=$($T/imgcmd boot.repair --img="$W/typo2.qcow2" --sys-lba=2048 2>/dev/null | jq_ "d['err']['error_code']")
{ [ "$code" = "IMG-OPEN-FAIL" ] || [ "$code" = "HOST-OPEN-FAIL" ]; } && [ ! -e "$W/typo2.qcow2" ] \
  && ok "boot.repair 也不创建镜像（$code）" \
  || ng "boot.repair：code=$code 文件=$([ -e "$W/typo2.qcow2" ] && echo "被创建了(错)" || echo 无)"
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
ESD=$(ls "$IMGTOOL_WS_ROOT"/*.esd 2>/dev/null | head -1)
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

echo "════ 4b. part.scan / ntfs.list（三级口径）════"
# 未格式化 → level 0
$T/imglayout mk "$W/scan.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
SL=$($T/imgpart show "$W/scan.qcow2" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
j=$("$T/imgcmd" part.scan --img="$W/scan.qcow2" --lba=$SL)
lv=$(echo "$j" | jq_ "d['data']['level']")
[ "$lv" = "0" ] && ok "未格式化 → level=0（fs=none）" || ng "未格式化 → 期望 level=0 实得 $lv"
# 空 NTFS → level 1（不能误报有数据）
$T/imgntfsfs "$W/scan.qcow2" $SL format SCANVOL >/dev/null 2>&1
j=$("$T/imgcmd" part.scan --img="$W/scan.qcow2" --lba=$SL)
lv=$(echo "$j" | jq_ "d['data']['level']")
hd=$(echo "$j" | jq_ "d['data']['has_data']")
[ "$lv" = "1" ] && [ "$hd" = "False" ] && ok "NTFS 空卷 → level=1 has_data=False（不误报）" || ng "空卷误报: level=$lv has_data=$hd"
# ntfs.list 反用例：坏路径
check_err "ntfs.list 路径不存在" "FAT-LS-FAIL" \
  "$T/imgcmd" ntfs.list --img="$W/scan.qcow2" --lba=$SL --path=/no/such --progress=bar
# part.scan 反用例：LBA 不在分区表
check_err "part.scan LBA 不存在" "CMD-ARG-BAD" \
  "$T/imgcmd" part.scan --img="$W/scan.qcow2" --lba=123456 --progress=bar

echo "════ 4c. img.clone（克隆 + 护栏）════"
$T/imglayout mk "$W/clone.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
CE=$($T/imgpart show "$W/clone.qcow2" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
$T/imgmkfs "$W/clone.qcow2" --mkfs $CE ESPVOL >/dev/null 2>&1
printf 'clone-payload' > /tmp/clone_p.bin
$T/imgfatfs "$W/clone.qcow2" $CE put /tmp/clone_p.bin /p.bin >/dev/null 2>&1
G0=$($T/imgcmd img.info --img="$W/clone.qcow2" 2>/dev/null | jq_ "d['data']['disk_guid']")
j=$("$T/imgcmd" img.clone --img="$W/clone.qcow2" --keep=$CE)
bw=$(echo "$j" | jq_ "d['data']['bytes_written']")
[ -n "$bw" ] && [ "$bw" -gt 0 ] && ok "img.clone 成功（写入 $bw 字节，稀疏生效）" || ng "bytes_written=$bw"
[ ! -e "$W/clone.old.qcow2" ] && ok "默认不保留旧副本（校验通过后删 .old）" || ng ".old 还在"
G1=$($T/imgcmd img.info --img="$W/clone.qcow2" 2>/dev/null | jq_ "d['data']['disk_guid']")
[ "$G1" != "$G0" ] && ok "磁盘 GUID 已重新生成" || ng "GUID 没变（应随机）"
rm -f /tmp/clone_out.bin
$T/imgfatfs "$W/clone.qcow2" $CE export /p.bin /tmp/clone_out.bin >/dev/null 2>&1
cmp -s /tmp/clone_p.bin /tmp/clone_out.bin && ok "克隆后内容逐字节一致" || ng "内容不一致"
before=$(md5sum "$W/clone.qcow2" | cut -c1-32)
check_err "img.clone 保留名单含坏 LBA" "CMD-ARG-BAD" \
  "$T/imgcmd" img.clone --img="$W/clone.qcow2" --keep=999999 --progress=bar
after=$(md5sum "$W/clone.qcow2" | cut -c1-32)
[ "$before" = "$after" ] && ok "预检失败 → 源文件一字节未动" || ng "源文件被改了！"

echo "════ 4d. winre.auto（WinRE 出厂自动启用脚本）════"
$T/imglayout mk "$W/winre.qcow2" $((20*1024*1024*1024)) gpt --esp 200 --msr 16 >/dev/null 2>&1
WE=$($T/imgpart show "$W/winre.qcow2" | python3 -c "
import sys,json; d=json.load(sys.stdin)
print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0])")
$T/imgntfsfs "$W/winre.qcow2" $WE format Windows >/dev/null 2>&1
j=$("$T/imgcmd" winre.auto --img="$W/winre.qcow2" --lba=$WE)
wb=$(echo "$j" | jq_ "d['data']['bytes']")
[ -n "$wb" ] && [ "$wb" -gt 0 ] && ok "winre.auto 写入脚本（$wb 字节）" || ng "写入失败"
$T/imgntfsfs "$W/winre.qcow2" $WE export /Windows/Setup/Scripts/SetupComplete.cmd /tmp/wr.cmd >/dev/null 2>&1
grep -q "reagentc /enable" /tmp/wr.cmd && ok "脚本内容含 reagentc /enable" || ng "内容不对"
LC_ALL=C grep -qP '[^\x09\x0A\x0D\x20-\x7E]' /tmp/wr.cmd && ng "脚本含非 ASCII 字节" || ok "脚本纯 ASCII（cmd.exe 按 OEM 读，带 BOM/UTF-8 会乱）"
grep -q $'\r$' /tmp/wr.cmd && ok "行尾 CRLF（Windows 要求）" || ng "行尾不是 CRLF"
$T/imgntfsfs "$W/winre.qcow2" $WE exists /Windows/Setup/Scripts/SetupComplete.cmd 2>&1 | grep -q "是" && \
  ok "上游 ntfsls 复核：路径存在" || ng "路径不存在"
"$T/imgcmd" winre.auto --img="$W/winre.qcow2" --lba=$WE --enable=false >/dev/null 2>&1
$T/imgntfsfs "$W/winre.qcow2" $WE exists /Windows/Setup/Scripts/SetupComplete.cmd 2>&1 | grep -q "否" && \
  ok "enable=false 删除成功" || ng "删除失败"
"$T/imgcmd" winre.auto --img="$W/winre.qcow2" --lba=$WE >/dev/null 2>&1
$T/imgntfsfs "$W/winre.qcow2" $WE exists /Windows/Setup/Scripts/SetupComplete.cmd 2>&1 | grep -q "是" && \
  ok "再写回幂等" || ng "重写失败"

echo "════ 4e0. 扩展/逻辑分区：part.show 不重复列、导出 logical、check 不误报 ════"
{   # ★ 2026-10-20 修：旧实现把逻辑分区也塞进 MBR 主槽 → part.show 重复列同一条，
    #   且 imglayout check 报「MBR 最多 4 个主分区，当前 5 个」的假阳性（手册 §13.9）。
    X="$W/ext.qcow2"
    $T/imglayout mk "$X" $((4*1024*1024*1024)) mbr >/dev/null 2>&1
    $T/imgcmd part.layout --img="$X" --sr-mb=64 --sys-gb=1 --rec-gb=0 --extended=true --progress=none >/dev/null 2>&1
    $T/imgcmd part.show --img="$X" --ro --progress=none > "$W/ext.json" 2>/dev/null
    python3 - "$W/ext.json" <<'PYX'
import json,sys
d=json.load(open(sys.argv[1]))
ps=d['data']['pt']['parts']
keys=[(p['first'],p['last']) for p in ps]
dup = len(keys)!=len(set(keys))
log = [p for p in ps if p.get('logical')]
ok = (not dup) and len(log)==1
print(("OK" if ok else "BAD"), "parts=%d dup=%s logical=%d" % (len(ps), dup, len(log)))
sys.exit(0 if ok else 1)
PYX
    [ $? -eq 0 ] && ok "part.show 无重复条目、逻辑分区带 logical=true（$W/ext.json）" \
                  || ng "part.show 对扩展/逻辑分区仍重复或没导出 logical：$(head -c 300 "$W/ext.json")"
    if $T/imglayout check "$X" 2>&1 | grep -q "最多 4 个主分区"; then
        ng "imglayout check 仍把逻辑分区算进 4 个主槽（假阳性）"
    else
        ok "imglayout check 不再误报「MBR 最多 4 个主分区」"
    fi
}

echo "════ 4e. ntfs.exists / fat.exists（断点续跑探测原语）════"
j=$("$T/imgcmd" ntfs.exists --img="$W/winre.qcow2" --lba=$WE --path=/Windows/Setup/Scripts/SetupComplete.cmd)
ex=$(echo "$j" | jq_ "d['data']['exists']")
sz=$(echo "$j" | jq_ "d['data']['size_bytes']")
[ "$ex" = "True" ] && [ "${sz:-0}" -gt 0 ] && ok "ntfs.exists 存在且大小=$sz（断点判定可用）" || ng "exists=$ex size=$sz"
grep -q 'del /f /q' /tmp/wr.cmd && ok "脚本含自删指令（一次性）" || ng "脚本缺自删指令"
j=$("$T/imgcmd" ntfs.exists --img="$W/winre.qcow2" --lba=$WE --path=/Windows/System32/ntoskrnl.exe)
ex=$(echo "$j" | jq_ "d['data']['exists']")
[ "$ex" = "False" ] && ok "ntfs.exists 不存在（没灌过的分区）" || ng "误报存在"
check_err "ntfs.exists 路径不带 /" "CMD-ARG-BAD" \
  "$T/imgcmd" ntfs.exists --img="$W/winre.qcow2" --lba=$WE --path=Windows --progress=bar

echo "════ 4f. 建盘流水线（img.create / fs.format / ntfs.put / ntfs.export / boot.check / bcd.gen-uefi）════"
{   # ★ 这 6 条是 2026-10-20 补的：以前 App 只能调表外 CLI 才能建盘（手册 §11.0）
    P="$W/pipe.qcow2"
    $T/imgcmd img.create --out="$P" --size-gb=6 --progress=none > "$W/cr.json" 2>/dev/null
    n=$(jq_ "d['data']['parts_total']" < "$W/cr.json")
    [ "$n" = "3" ] && ok "img.create：GPT 默认 3 个分区（ESP200+MSR16+Windows 其余）" \
                   || ng "img.create parts_total=$n（应为 3）"

    # 已存在且没 force → 必须拒绝（不许悄悄覆盖）
    code=$($T/imgcmd img.create --out="$P" --size-gb=6 --progress=none 2>/dev/null | jq_ "d['err']['error_code']")
    [ "$code" = "CMD-ARG-BAD" ] && ok "img.create 已存在 → 拒绝（$code）" || ng "img.create 覆盖护栏失效：$code"

    ESP=$($T/imgcmd part.show --img="$P" --ro --progress=none 2>/dev/null | jq_ "[p['first'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('c12a7328')][0]")
    SYS=$($T/imgcmd part.show --img="$P" --ro --progress=none 2>/dev/null | jq_ "[p['first'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0]")
    SG=$($T/imgcmd part.show --img="$P" --ro --progress=none 2>/dev/null | jq_ "[p['partGuid'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('ebd0a0a2')][0]")
    EG=$($T/imgcmd part.show --img="$P" --ro --progress=none 2>/dev/null | jq_ "[p['partGuid'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('c12a7328')][0]")
    DG=$($T/imgcmd part.show --img="$P" --ro --progress=none 2>/dev/null | jq_ "d['data']['pt']['diskGuid']")

    $T/imgcmd fs.format --img="$P" --lba="$ESP" --fs=fat32 --label=EFI --progress=none > "$W/f1.json" 2>/dev/null
    [ "$(jq_ "d['data']['fs']" < "$W/f1.json")" = "FAT32" ] && ok "fs.format FAT32（ESP）" || ng "fs.format FAT32 失败"
    $T/imgcmd fs.format --img="$P" --lba="$SYS" --fs=ntfs --label=Windows --progress=none > "$W/f2.json" 2>/dev/null
    [ "$(jq_ "d['data']['fs']" < "$W/f2.json")" = "NTFS" ] && ok "fs.format NTFS（快速，$(jq_ "d['duration_ms']" < "$W/f2.json") ms）" || ng "fs.format NTFS 失败"

    head -c 5000 /etc/services > "$W/put.bin"
    $T/imgcmd ntfs.put --img="$P" --lba="$SYS" --src="$W/put.bin" --dst=/Windows/System32/Recovery/x.bin --progress=none > "$W/np.json" 2>/dev/null
    [ "$(jq_ "d['data']['written_bytes']" < "$W/np.json")" = "5000" ] && ok "ntfs.put 写入 5000 字节（父目录自动建）" || ng "ntfs.put 失败：$(head -c 200 "$W/np.json")"
    $T/imgcmd ntfs.exists --img="$P" --lba="$SYS" --path=/Windows/System32/Recovery/x.bin --progress=none > "$W/ne.json" 2>/dev/null
    [ "$(jq_ "d['data']['exists']" < "$W/ne.json")" = "True" ] && ok "ntfs.exists 看到刚写的文件" || ng "ntfs.exists 没看到"
    $T/imgcmd ntfs.export --img="$P" --lba="$SYS" --path=/Windows/System32/Recovery/x.bin --dst="$W/back.bin" --progress=none >/dev/null 2>&1
    [ "$(md5sum < "$W/put.bin" | cut -d' ' -f1)" = "$(md5sum < "$W/back.bin" | cut -d' ' -f1)" ] && ok "ntfs.export 抽回来 md5 一致" || ng "ntfs.export 内容不一致"

    $T/imgcmd boot.check --img="$P" --mode=uefi --progress=none > "$W/bc.json" 2>/dev/null
    cnt=$($T/imgcmd boot.check --img="$P" --mode=uefi --progress=none 2>/dev/null | jq_ "d['data']['count']")
    [ "${cnt:-0}" -ge 11 ] && ok "boot.check 返回 $cnt 项（items 是合法 JSON 数组）" || ng "boot.check 异常：$(head -c 200 "$W/bc.json")"

    $T/imgcmd bcd.gen-uefi --ref="$ROOT/reference/bcd-reference-uefi.bin" --out="$W/bcd_new.bin" \
        --sys-guid="$SG" --disk-guid="$DG" --esp-guid="$EG" --progress=none > "$W/bg.json" 2>/dev/null
    h=$(jq_ "d['data']['bindings_patched']" < "$W/bg.json")
    [ "${h:-0}" -ge 3 ] && [ -s "$W/bcd_new.bin" ] && ok "bcd.gen-uefi 改到 $h 处设备并写出 BCD" || ng "bcd.gen-uefi 失败：$(head -c 200 "$W/bg.json")"
}

echo "════ 4g. ISO 抽取（iso.ls / iso.extract；UDF 桥）════"
{   # ★ Windows 安装盘的 ISO9660 目录树是空的，install.wim 只在 UDF 里 —— 这几条锁住"能进去"。
    ISO=$(ls "$ASSETS"/*LTSC*.iso 2>/dev/null | head -1)
    if [ -z "$ISO" ]; then
        echo "  ⏭ 没找到 LTSC ISO，跳过 ISO 用例"
    else
        $T/imgcmd iso.ls --iso="$ISO" --path=/ --progress=none > "$W/il.json" 2>/dev/null
        n=$(python3 -c "import json;d=json.load(open('$W/il.json'));print(d['data']['count'] if d['ok'] else -1)" 2>/dev/null)
        [ "${n:-0}" -gt 5 ] && ok "iso.ls / → $n 项（UDF 根目录）" || ng "iso.ls / 失败：$(head -c 200 "$W/il.json")"
        names=$(python3 -c "
import json;d=json.load(open('$W/il.json'))
print(','.join(e['name'].lower() for e in d['data']['entries']))" 2>/dev/null)
        echo "$names" | grep -q "sources" && ok "根目录里能看到 sources/（ISO9660 树里是没有的）" || ng "根目录缺 sources/"
        $T/imgcmd iso.ls --iso="$ISO" --path=/sources --progress=none > "$W/il2.json" 2>/dev/null
        [ "$(python3 -c "
import json;d=json.load(open('$W/il2.json'))
print([e['size'] for e in d['data']['entries'] if e['name'].lower()=='install.wim'][0])" 2>/dev/null)" = "3966348023" ] \
            && ok "iso.ls /sources → install.wim 大小 3,966,348,023（与抽出来的文件一致）" \
            || ng "install.wim 大小不对：$(head -c 200 "$W/il2.json")"
        # 回退路径：强制走 ISO9660（这张盘的 ISO9660 树里只有 README.TXT）
        ic=$(IMG_ISO_FORCE_ISO9660=1 $T/imgcmd iso.ls --iso="$ISO" --path=/ --progress=none 2>/dev/null | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin); print(d['data']['count'])
except Exception: print(-1)")
        [ "$ic" = "1" ] && ok "iso.ls 回退到 ISO9660 → 1 项（UDF 优先，回退可用）" || ng "ISO9660 回退异常：count=$ic"
        rm -f "$W/r.txt"
        IMG_ISO_FORCE_ISO9660=1 $T/imgcmd iso.extract --iso="$ISO" --path=/README.TXT --dst="$W/r.txt" --progress=none >/dev/null 2>&1
        [ "$(stat -c%s "$W/r.txt" 2>/dev/null)" = "135" ] && ok "iso.extract 走 ISO9660 → 135 字节" || ng "ISO9660 抽取异常"
        # —— 递归 / find（2026-10-23：以前只能一层层 ls，找 install.wim 要点很多次）——
        $T/imgcmd iso.ls --iso="$ISO" --find=install.wim --progress=none > "$W/if.json" 2>/dev/null
        [ "$(python3 -c "
import json;d=json.load(open('$W/if.json'))
print(1 if d['ok'] and any(e['path'].lower().endswith('/install.wim') for e in d['data']['entries']) else 0)" 2>/dev/null)" = "1" ] \
            && ok "iso.ls --find=install.wim 一次定位（自动递归，条目带完整 path）" || ng "iso.ls --find 失败：$(head -c 200 "$W/if.json")"
        $T/imgcmd iso.ls --iso="$ISO" --recursive --max=50 --progress=none > "$W/ir.json" 2>/dev/null
        [ "$(python3 -c "
import json;d=json.load(open('$W/ir.json'))
print('%d/%s' % (d['data']['count'], d['data']['truncated']))" 2>/dev/null)" = "50/True" ] \
            && ok "iso.ls --recursive --max=50 → 正好 50 项并标记 truncated（裸布尔参数也认）" || ng "iso.ls --recursive 失败：$(head -c 200 "$W/ir.json")"
        rm -f "$W/b.sdi"
        $T/imgcmd iso.extract --iso="$ISO" --path=/boot/boot.sdi --dst="$W/b.sdi" --progress=none > "$W/ix.json" 2>/dev/null
        if [ -s "$W/b.sdi" ] && [ "$(stat -c%s "$W/b.sdi")" = "3170304" ]; then
            if [ -f $ISO_WIN10/boot.sdi ]; then
                [ "$(md5sum < "$W/b.sdi")" = "$(md5sum < $ISO_WIN10/boot.sdi)" ] \
                    && ok "iso.extract /boot/boot.sdi → 3,170,304 字节，md5 与已知真值一致" \
                    || ng "boot.sdi md5 不一致"
            else
                ok "iso.extract /boot/boot.sdi → 3,170,304 字节"
            fi
        else
            ng "iso.extract 失败：$(head -c 200 "$W/ix.json")"
        fi
    fi
}

echo "════ 4h. 恢复分区四件套（part.attr / winre.find / recovery.populate*）════"
{   # ★ 政策是"默认不建"；这里验的是"勾了之后能不能纯命令表做完"。
    WIM=$ISO_WIN10/install.wim
    SDI=$ISO_WIN10/boot.sdi
    if [ ! -f "$WIM" ]; then
        echo "  ⏭ 没有 install.wim，跳过恢复分区用例"
    else
        R="$W/rec.qcow2"
        $T/imgcmd img.create --out="$R" --size-gb=12 --rec-mb=1024 --limits=off --progress=none >/dev/null 2>&1
        REC=$($T/imgcmd part.show --img="$R" --ro --progress=none 2>/dev/null | jq_ "[p['first'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('de94bba4')][0]")
        [ -n "$REC" ] && ok "img.create --rec-mb=1024 建出恢复分区（LBA $REC）" || ng "没有恢复分区"

        $T/imgcmd winre.find --src="$WIM" --index=1 --progress=none > "$W/wf.json" 2>/dev/null
        [ "$(jq_ "d['data']['size_bytes']" < "$W/wf.json")" = "435363851" ] \
            && ok "winre.find → 找到 Winre.wim（435,363,851 字节）" || ng "winre.find 异常：$(head -c 200 "$W/wf.json")"
        # 反例：不是 WIM 的文件 → 稳定错误码
        code=$($T/imgcmd winre.find --src=/etc/hostname --progress=none 2>/dev/null | jq_ "d['err']['error_code']")
        [ "$code" = "WIM-NOTFOUND" ] && ok "winre.find 垃圾输入 → WIM-NOTFOUND" || ng "winre.find 反例：$code"

        $T/imgcmd recovery.populate --img="$R" --rec-lba="$REC" --winre-src="$WIM" \
            --winre-index=1 --bootsdi-src="$SDI" --format=true --progress=none > "$W/rp.json" 2>/dev/null
        [ "$(jq_ "d['data']['winre_bytes']" < "$W/rp.json")" = "435363851" ] \
            && ok "recovery.populate → 写入 Winre.wim 435 MB" || ng "recovery.populate 失败：$(head -c 200 "$W/rp.json")"
        [ "$(jq_ "d['data']['gpt_attr_set']" < "$W/rp.json")" = "True" ] \
            && ok "recovery.populate → GPT 属性已设" || ng "GPT 属性没设上"
        ex=$($T/imgcmd ntfs.exists --img="$R" --lba="$REC" --path=/Recovery/WindowsRE/Winre.wim --progress=none 2>/dev/null | jq_ "d['data']['exists']")
        [ "$ex" = "True" ] && ok "恢复分区里确实有 Winre.wim" || ng "文件不在"
        at=$($T/imgcmd part.show --img="$R" --ro --progress=none 2>/dev/null | jq_ "[p['attributes'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('de94bba4')][0]")
        [ "$at" = "0x8000000000000001" ] && ok "part.show 读回属性 = 0x8000000000000001" || ng "属性读回=$at"
        $T/imgcmd part.attr --img="$R" --lba="$REC" --attr=0x1 --progress=none >/dev/null 2>&1
        at2=$($T/imgcmd part.show --img="$R" --ro --progress=none 2>/dev/null | jq_ "[p['attributes'] for p in d['data']['pt']['parts'] if p['typeGuid'].startswith('de94bba4')][0]")
        [ "$at2" = "0x0000000000000001" ] && ok "part.attr 能改（0x8000000000000001 → 0x1）" || ng "part.attr 没生效：$at2"
    fi
}

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
