#!/usr/bin/env bash
# ci/repro-fat.sh —— 在 CI 镜像里复现「FAT 操作 buffer overflow」并打出栈
#
# 背景：CI（Ubuntu 24.04 amd64）上 copy-test / xcopy-test / fileops-test 里
# 与 FAT 有关的操作报 "*** buffer overflow detected ***: terminated"，
# 而本机 aarch64 用同样的 Release 配置跑不出来。这个脚本把出错的命令
# 单独拿出来，用 gdb 跑，直接把栈打出来。
#
# 用法（CI 里由 workflow_dispatch 的 mode=debug-fat 触发）：
#   MODE=debug-fat bash ci/run-tests.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$ROOT/build/tools"
IMG=/tmp/repro-fat.qcow2
W=/tmp/repro-fat
rm -rf "$W" "$IMG"; mkdir -p "$W"

run() { echo "+ $*"; "$@"; }

echo "════ 0. 造一张 1GiB GPT 盘（ESP 100MiB FAT32）════"
"$T/imgcmd" img.create --out="$IMG" --size-gb=1 --table=gpt --esp-mb=100 --sys-mb=600 --rec-mb=0 --limits=off >/dev/null
ESP=$("$T/imgcmd" part.show --img="$IMG" 2>/dev/null | python3 -c 'import json,sys
d=json.load(sys.stdin)["data"]["pt"]["parts"]
print([p["first"] for p in d if p["typeGuid"].startswith("c12a7328")][0])')
echo "  ESP=$ESP"
"$T/imgcmd" fs.format --img="$IMG" --lba="$ESP" --fs=fat32 >/dev/null

echo "════ 1. 放两个文件（ASCII 名 + 中文名）════"
head -c 4096 /dev/urandom > "$W/a.bin"
head -c 4096 /dev/urandom > "$W/长名 文件.bin"
"$T/imgcmd" fat.put --img="$IMG" --lba="$ESP" --src="$W/a.bin" --dst=/fa.bin >/dev/null 2>&1
echo "  fat.put ASCII rc=$?"
"$T/imgcmd" fat.put --img="$IMG" --lba="$ESP" --src="$W/长名 文件.bin" --dst='/长名 文件.bin' >/dev/null 2>&1
echo "  fat.put 中文 rc=$?"

echo "════ 2. 逐条跑可疑操作，崩溃就打栈 ════"
try() { # $1=标签 $2..=参数
  local tag="$1"; shift
  echo
  echo "---- $tag ----"
  gdb -batch -nx -ex run -ex 'bt 25' --args "$T/imgcmd" "$@" 2>&1 | grep -vE '^\[|Reading symbols|^$' | tail -30
  echo "---- $tag 结束 ----"
}

try "fat.copy ASCII → ASCII" copy --img="$IMG" --lba="$ESP" --src=/fa.bin --dst=/fb.bin
try "fat.copy 中文 → ASCII" copy --img="$IMG" --lba="$ESP" --src='/长名 文件.bin' --dst=/fc.bin
try "fat.copy ASCII → 中文" copy --img="$IMG" --lba="$ESP" --src=/fa.bin --dst='/中文 副本.bin'
try "fat.copy 同名冲突(fail)" copy --img="$IMG" --lba="$ESP" --src=/fa.bin --dst=/fa.bin
try "fat.copy 到多级目录" copy --img="$IMG" --lba="$ESP" --src=/fa.bin --dst=/A/B/C/x.bin

echo
echo "════ 3. 环境信息 ════"
grep -m1 'DISTRIB_DESCRIPTION' /etc/lsb-release 2>/dev/null
"$T/imgcmd" --list 2>/dev/null | python3 -c 'import json,sys;print("features:", json.load(sys.stdin)["features"])'
echo "glibc: $(ldd --version | head -1)"