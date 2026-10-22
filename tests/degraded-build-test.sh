#!/bin/bash
# degraded-build-test.sh — 验证"关掉可选依赖仍然构建得出来，并且命令如实报不可用"
#
# 为什么要有它（工程债 #13）：以前 -DIMG_WITH_WIM=OFF / -DIMG_WITH_NTFS3G=OFF
#   会直接链接失败（img_recovery.c 里 wimlib_*、命令层 img_ntfs_* 没门控），
#   于是"缺库降级"只是说说。2026-10-23 用 img_ntfs_stub.c + img_recovery 条件编译修好，
#   这个脚本就是防回退的。
#
# 用法：tests/degraded-build-test.sh            （默认两种组合都测）
#       tests/degraded-build-test.sh both-off   （只测最狠的那组）
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
pass=0; fail=0
ok(){ echo "  ✅ $1"; pass=$((pass+1)); }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
chk(){ if eval "$2" >/dev/null 2>&1; then ok "$1"; else ng "$1"; fi }

J() { python3 -c "import json,sys;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

build(){ # $1=目录 $2..=cmake 参数
  local dir="$1"; shift
  rm -rf "$dir"
  cmake -S "$ROOT" -B "$dir" -DIMG_BUILD_TESTS=OFF "$@" > "$dir.cm.log" 2>&1 || { echo "  cmake 失败：$dir.cm.log"; return 1; }
  cmake --build "$dir" -j6 > "$dir.build.log" 2>&1 || { echo "  编译失败：$dir.build.log"; return 1; }
  return 0
}

echo "════ 1. 最狠的一组：WIM + NTFS 全关 ════"
B=/tmp/imgtool-degraded-both
if build "$B" -DIMG_WITH_WIM=OFF -DIMG_WITH_NTFS3G=OFF -DIMG_WITH_NTFS=OFF; then
  ok "配置+编译通过（这正是修之前失败的那一步）"
  T="$B/tools"
  [ -x "$T/imgcmd" ] && ok "imgcmd 仍然产出（命令表不随库裁剪消失）" || ng "imgcmd 缺失"
  feats=$("$T/imgcmd" --list 2>/dev/null | J "'%s/%s/%s' % (d['features']['wimlib'], d['features']['ntfs3g'], d['features']['zlib'])")
  [ "$feats" = "False/False/True" ] && ok "features 如实上报：wimlib=False ntfs3g=False zlib=True" || ng "features 不对：$feats"
  n=$("$T/imgcmd" --list 2>/dev/null | J "len(d['cmds'])")
  [ "${n:-0}" -ge 40 ] && ok "命令表仍有 $n 条（能力靠 FEATURE-UNAVAILABLE 表达，不是删条目）" || ng "命令数异常：$n"
  # 先建一张小盘：--img 会在命令入口之前打开，用 /dev/null 只会得到 IMG-OPEN-FAIL，
  # 测不到"功能闸"（第一版就栽在这）。
  W=$(mktemp -d /tmp/imgdegXXXXXX)
  "$T/imglayout" mk "$W/d.qcow2" $((64*1024*1024)) gpt --esp 32 >/dev/null 2>&1 \
    && "$T/imgmkfs" "$W/d.qcow2" --mkfs 2048 T >/dev/null 2>&1
  c=$("$T/imgcmd" ntfs.list --img="$W/d.qcow2" --lba=2048 2>/dev/null | J "d['err']['error_code']")
  [ "$c" = "FEATURE-UNAVAILABLE" ] && ok "ntfs.list → FEATURE-UNAVAILABLE" || ng "ntfs.list 返回 $c"
  c=$("$T/imgcmd" wim.size --wim=/dev/null 2>/dev/null | J "d['err']['error_code']")
  [ "$c" = "FEATURE-UNAVAILABLE" ] && ok "wim.size → FEATURE-UNAVAILABLE" || ng "wim.size 返回 $c"
  printf 'degraded-ok' > "$W/a.txt"
  "$T/imgcmd" fat.put --img="$W/d.qcow2" --lba=2048 --src="$W/a.txt" --dst=/EFI/a.txt >/dev/null 2>&1
  cnt=$("$T/imgcmd" fat.ls --img="$W/d.qcow2" --lba=2048 --path=/EFI 2>/dev/null | J "d['data']['count']")
  [ "$cnt" = "1" ] && ok "FAT 链路照常工作（裁剪不影响无关功能）" || ng "fat.ls 异常：count=$cnt"
  rm -rf "$W"
else
  ng "配置/编译失败（看上一条提示的日志）"
fi

echo
echo "════ 2. 只关 WIM（NTFS 仍在）════"
B=/tmp/imgtool-degraded-nowim
if build "$B" -DIMG_WITH_WIM=OFF; then
  ok "配置+编译通过"
  feats=$("$B/tools/imgcmd" --list 2>/dev/null | J "'%s/%s' % (d['features']['wimlib'], d['features']['ntfs3g'])")
  [ "$feats" = "False/True" ] && ok "features：wimlib=False ntfs3g=True（没被连坐）" || ng "features 不对：$feats"
  c=$("$B/tools/imgcmd" winre.find --src=/dev/null 2>/dev/null | J "d['err']['error_code']")
  [ "$c" = "FEATURE-UNAVAILABLE" ] && ok "winre.find → FEATURE-UNAVAILABLE" || ng "winre.find 返回 $c"
else
  ng "配置/编译失败"
fi

echo
echo "════════ degraded-build-test：通过 $pass · 失败 $fail ════════"
[ $fail -eq 0 ] || { echo "（日志：/tmp/imgtool-degraded-*.cm.log / .build.log）"; exit 1; }