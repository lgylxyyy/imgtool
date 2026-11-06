#!/bin/bash
# stack-test.sh — REQ-20261026-21：1 MB 栈线程里跑 fs.format / wim.apply /
#                 recovery.populate / boot.populate，不许崩
#
# 为什么：App 的工作线程只有 ~1 MB 栈，mkntfs 与 wimlib 各吃过一次 SIGSEGV（整进程闪退）。
#   imgcmd 主线程 8 MB，所以在 CLI 上永远测不出来 —— 这条测试专门用 1 MB 线程跑。
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/build/tools"
ST="$ROOT/build/tests/stack_test"
W=$(mktemp -d /tmp/stacktestXXXXXX)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
cleanup(){ rm -rf "$W"; }
trap cleanup EXIT

if [ ! -x "$ST" ]; then echo "（跳过：stack_test 未编译）"; exit 0; fi
if ! command -v wimlib-imagex >/dev/null 2>&1 || ! command -v xorriso >/dev/null 2>&1; then
    echo "（跳过：缺 wimlib-imagex / xorriso）"; exit 0
fi

echo "════ 0. 造夹具（install.wim 里带 WinRE + 引导文件，封成 ISO）════"
mkdir -p "$W/pay/Windows/System32/Recovery" "$W/pay/Windows/Boot/EFI" "$W/pay/Windows/Boot/PCAT"
printf '<ReAgent/>\n' > "$W/pay/Windows/System32/Recovery/ReAgent.xml"
head -c 400000 /dev/urandom > "$W/pay/Windows/System32/Recovery/Winre.wim"
head -c 300000 /dev/urandom > "$W/pay/Windows/Boot/EFI/bootmgfw.efi"
head -c 300000 /dev/urandom > "$W/pay/Windows/Boot/PCAT/bootmgr"
head -c 2000 /dev/urandom > "$W/pay/Windows/System32/winload.exe"
head -c 2000 /dev/urandom > "$W/pay/Windows/System32/winload.efi"
mkdir -p "$W/iso/sources"
wimlib-imagex capture "$W/pay" "$W/iso/sources/install.wim" "Test" --compress=LZX >/dev/null 2>&1 ||
    { echo "（跳过：wimlib-imagex capture 失败）"; exit 0; }
xorriso -as mkisofs -J -joliet-long -iso-level 3 -o "$W/a.iso" "$W/iso" >/dev/null 2>&1 ||
    { echo "（跳过：xorriso 失败）"; exit 0; }
ok "夹具就绪（install.wim + WinRE + boot 文件）"

echo "════ 1. 造盘（20 GiB GPT：ESP/MSR/系统 8 GiB/恢复 1 GiB）════"
IMG="$W/d.qcow2"
"$T/imgcmd" img.create --out="$IMG" --size-gb=20 --table=gpt \
    --esp-mb=200 --msr-mb=16 --sys-mb=8192 --rec-mb=1024 >/dev/null 2>&1 &&
    ok "img.create" || ng "img.create"
SYS=$("$T/imgcmd" part.show --img="$IMG" 2>/dev/null | python3 -c \
    "import sys,json;p=json.load(sys.stdin)['data']['pt']['parts'];print(p[-2]['first'])")
REC=$("$T/imgcmd" part.show --img="$IMG" 2>/dev/null | python3 -c \
    "import sys,json;p=json.load(sys.stdin)['data']['pt']['parts'];print(p[-1]['first'])")
echo "  sys_lba=$SYS rec_lba=$REC"

echo "════ 2. 1 MB 栈线程（App 同款）════"
OUT=$("$ST" "$IMG" "$SYS" "$REC" "$W/a.iso" 2>&1); RC=$?
echo "$OUT" | sed 's/^/    /'
if [ $RC -eq 0 ]; then ok "1 MB 线程里全部命令成功（fs.format/wim.apply/recovery.populate/boot.populate）"
else ng "1 MB 线程里有命令失败（rc=$RC）"; fi

echo "════ 3. 对照：关掉大栈保护，看是否真的需要（不判失败，只报事实）════"
IMGRC=0
IMGTOOL_NO_BIGSTACK=1 "$ST" "$IMG" "$SYS" "$REC" "$W/a.iso" >"$W/nb.log" 2>&1 || IMGRC=$?
if [ $IMGRC -eq 0 ]; then echo "  ℹ  关掉保护也过了（本机 1 MB 栈够用；Android ART worker 上会崩）"
else echo "  ℹ  关掉保护就失败/崩了（rc=$IMGRC）→ 这就是 REQ-21 的现场，保护确实在起作用"; tail -3 "$W/nb.log" | sed 's/^/      /'; fi

echo
echo "════ 结果：通过 $pass · 失败 $fail ════"
[ $fail -eq 0 ] || exit 1
