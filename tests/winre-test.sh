#!/bin/bash
# winre-test.sh — REQ-24/25：WinRE 检测必须可信 + 裸 winre.wim 能用
#
# 覆盖：
#   ① ISO 里 boot.wim（WinPE，无 WinRE）排在 install.wim 前面 → 不给 --inner 也要找到 install.wim
#   ② 就算 App 传了 --inner=/sources/boot.wim（拿错候选）→ 工具继续试别的，仍然找到
#   ③ 裸 winre.wim → found + self:true
#   ④ recovery.populate --winre-dry-run → 只回答，不写盘
#   ⑤ recovery.populate --winre-src=<裸 winre.wim> → 写进去，winre_bytes 与文件大小一致
#   ⑥ 真没有 WinRE 的 ISO → WINRE-NOTFOUND（不是"工具出错"）
#   ⑦ --winre-self=true 配普通 WIM → CMD-ARG-BAD（互相矛盾要明说）
#   ⑧ 同进程连跑（winre_chain）：identify --scan → size → find×6 都 found
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
CH="$ROOT/build/tests/winre_chain"
W=$(mktemp -d /tmp/winretestXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);
p=sys.argv[1].split('.')
for k in p:
    d=(d.get(k) if isinstance(d,dict) else None) if k else d
print(d)" "$1" 2>/dev/null; }
cleanup(){ rm -rf "$W"; }
trap cleanup EXIT

command -v wimlib-imagex >/dev/null 2>&1 || { echo "（跳过：缺 wimlib-imagex）"; exit 0; }
command -v xorriso >/dev/null 2>&1 || { echo "（跳过：缺 xorriso）"; exit 0; }

echo "════ 0. 夹具：一个 ISO（boot.wim 在前、install.wim 里有 WinRE）+ 一份裸 winre.wim ════"
mkdir -p "$W/boot/Windows/System32" "$W/win/Windows/System32/Recovery" "$W/iso/sources"
head -c 1000 /dev/urandom > "$W/boot/Windows/System32/x.txt"
head -c 400000 /dev/urandom > "$W/win/Windows/System32/Recovery/Winre.wim"
head -c 1000 /dev/urandom > "$W/win/Windows/System32/winload.exe"
wimlib-imagex capture "$W/boot" "$W/iso/sources/boot.wim" "WinPE" --compress=LZX >/dev/null 2>&1
wimlib-imagex capture "$W/win"  "$W/iso/sources/install.wim" "Win10" --compress=LZX >/dev/null 2>&1
xorriso -as mkisofs -J -joliet-long -iso-level 3 -o "$W/a.iso" "$W/iso" >/dev/null 2>&1
# 裸 winre.wim：根下没有 \Windows
mkdir -p "$W/wr/WindowsRE"
head -c 300000 /dev/urandom > "$W/wr/WindowsRE/winre.wim"
wimlib-imagex capture "$W/wr" "$W/winre.wim" "WinRE" --compress=LZX >/dev/null 2>&1
WR_SIZE=$(stat -c %s "$W/winre.wim")
ok "夹具就绪（ISO 双候选 + 裸 winre.wim ${WR_SIZE}B）"

echo "════ ① ISO 不给 --inner（旧行为会撞上 boot.wim）════"
OUT=$("$T/imgcmd" winre.find --src="$W/a.iso" 2>/dev/null)
GOT=$(printf '%s' "$OUT" | jq_ 'data.iso_inner_path')
if [ "$GOT" = "/sources/install.wim" ]; then ok "自动选中 install.wim（跳过 boot.wim）"; else ng "选中了 $GOT"; fi

echo "════ ② 硬塞 --inner=/sources/boot.wim（模拟 App 拿错候选）════"
OUT=$("$T/imgcmd" winre.find --src="$W/a.iso" --inner=/sources/boot.wim 2>/dev/null)
GOT=$(printf '%s' "$OUT" | jq_ 'data.iso_inner_path')
SZ=$(printf '%s' "$OUT" | jq_ 'data.size_bytes')
if [ "$GOT" = "/sources/install.wim" ] && [ "$SZ" = "400000" ]; then ok "继续试别的候选 → 仍然找到"; else ng "got=$GOT size=$SZ"; fi

echo "════ ③ 裸 winre.wim ════"
OUT=$("$T/imgcmd" winre.find --src="$W/winre.wim" 2>/dev/null)
SELF=$(printf '%s' "$OUT" | jq_ 'data.self'); SZ=$(printf '%s' "$OUT" | jq_ 'data.size_bytes')
if [ "$SELF" = "True" ] && [ "$SZ" = "$WR_SIZE" ]; then ok "self=true 且大小一致"; else ng "self=$SELF size=$SZ 期望 $WR_SIZE"; fi

echo "════ ④ dry-run 不写盘 ════"
IMG="$W/d.qcow2"
"$T/imgcmd" img.create --out="$IMG" --size-gb=12 --table=gpt --esp-mb=200 --msr-mb=16 \
    --sys-mb=8192 --rec-mb=1024 >/dev/null 2>&1
REC=$("$T/imgcmd" part.show --img="$IMG" 2>/dev/null | python3 -c \
     "import sys,json;print(json.load(sys.stdin)['data']['pt']['parts'][-1]['first'])")
OUT=$("$T/imgcmd" recovery.populate --img="$IMG" --rec-lba="$REC" --winre-src="$W/a.iso" \
      --winre-dry-run=true 2>/dev/null)
CAN=$(printf '%s' "$OUT" | jq_ 'data.can_populate'); BY=$(printf '%s' "$OUT" | jq_ 'data.winre_bytes')
if [ "$CAN" = "True" ] && [ "$BY" = "400000" ]; then ok "can_populate + winre_bytes 都对"; else ng "can=$CAN bytes=$BY"; fi
# 分区还没格式化 → ntfs.list 应当失败；万一成功也必须是 0 项
LS=$("$T/imgcmd" ntfs.list --img="$IMG" --lba="$REC" --path=/ 2>/dev/null)
LSOK=$(printf '%s' "$LS" | jq_ 'ok'); LSN=$(printf '%s' "$LS" | jq_ 'data.count')
if [ "$LSOK" != "True" ] || [ "$LSN" = "0" ]; then ok "dry-run 确实没写盘（分区仍未格式化/为空）"
else ng "dry-run 之后分区里有 $LSN 项"; fi

echo "════ ⑤ 裸 winre.wim 写进恢复分区 ════"
OUT=$("$T/imgcmd" recovery.populate --img="$IMG" --rec-lba="$REC" --winre-src="$W/winre.wim" \
      --format=true --label=Recovery 2>/dev/null)
BY=$(printf '%s' "$OUT" | jq_ 'data.winre_bytes'); SELF=$(printf '%s' "$OUT" | jq_ 'data.self')
if [ "$BY" = "$WR_SIZE" ] && [ "$SELF" = "True" ]; then ok "self 写入成功（$BY 字节）"; else ng "bytes=$BY self=$SELF"; fi
N=$("$T/imgcmd" ntfs.list --img="$IMG" --lba="$REC" --path=/Recovery/WindowsRE 2>/dev/null | \
    python3 -c "import sys,json;d=json.load(sys.stdin)['data'];print([e['name'] for e in (d.get('names') or [])])" 2>/dev/null)
case "$N" in *Winre.wim*) ok "分区里确实有 Winre.wim";; *) ng "分区里没有 Winre.wim：$N";; esac

echo "════ ⑥ 真没有 WinRE → WINRE-NOTFOUND ════"
mkdir -p "$W/plain/sources"
head -c 500 /dev/urandom > "$W/plain/sources/install.wim"
wimlib-imagex capture "$W/win" "$W/plain/sources/install.wim" "NoWinRE" --compress=LZX >/dev/null 2>&1
# 从 install.wim 里去掉 WinRE：造一个只有 Windows\System32\winload 的映像
mkdir -p "$W/nowin/Windows/System32"
head -c 100 /dev/urandom > "$W/nowin/Windows/System32/winload.exe"
wimlib-imagex capture "$W/nowin" "$W/plain/sources/install.wim" "NoWinRE" --compress=LZX >/dev/null 2>&1
xorriso -as mkisofs -J -joliet-long -iso-level 3 -o "$W/none.iso" "$W/plain" >/dev/null 2>&1
OUT=$("$T/imgcmd" winre.find --src="$W/none.iso" 2>/dev/null)
CODE=$(printf '%s' "$OUT" | jq_ 'err.error_code')
[ "$CODE" = "WINRE-NOTFOUND" ] && ok "WINRE-NOTFOUND（"真没有"才是这个码）" || ng "got $CODE"

echo "════ ⑦ 参数矛盾要明说 ════"
OUT=$("$T/imgcmd" recovery.populate --img="$IMG" --rec-lba="$REC" --winre-src="$W/iso/sources/install.wim" \
      --winre-self=true 2>/dev/null)
CODE=$(printf '%s' "$OUT" | jq_ 'err.error_code')
[ "$CODE" = "CMD-ARG-BAD" ] && ok "普通 WIM + --winre-self=true → CMD-ARG-BAD" || ng "got $CODE"

echo "════ ⑧ 同进程连跑（App 的顺序）════"
if [ -x "$CH" ]; then
    OUT=$("$CH" "$W/a.iso" 1 2>&1); RC=$?
    if [ $RC -eq 0 ]; then ok "identify --scan → size → find×6 全过"; else ng "chain 失败"; echo "$OUT" | tail -5 | sed 's/^/      /'; fi
else
    echo "  ⏭  winre_chain 没编（跳过）"
fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ $fail -eq 0 ] || exit 1
