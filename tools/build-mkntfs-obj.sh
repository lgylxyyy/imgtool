#!/usr/bin/env bash
# build-mkntfs-obj.sh — 从 ntfs-3g 源码生成【宿主】用的 mkntfs 目标文件
#
# 为什么需要这一步：IMG_WITH_NTFS=ON 时我们不给用户装 ntfsprogs，而是把 mkntfs
# 的源文件编成 .o，用 objcopy 把 main 改名成 mkntfs_main 再链进 libimgtool
# （进程内格式化 qcow2 分区，无临时文件、无 fork、无 loop）。这些 .o 是【构建输入】，
# 不在仓库里，也不是普通构建垃圾 —— 删了就会出现
# features.mkntfs=false / fs.format --fs=ntfs 报 FEATURE-UNAVAILABLE。
#
# 用法：
#   tools/build-mkntfs-obj.sh [源码目录] [输出目录]
#   默认 <工作区>/third_party/ntfs-3g → <构建根>/mkntfs_obj
#   工作区与构建根由脚本位置推导，仓库内 build 是软链时取其所指目录的父目录。
#
# 之后配置（少一个开关就会退回 OFF）：
#   cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DIMG_WITH_NTFS=ON && make -j8
#   ./tools/imgcmd --list | grep -o '"features":{[^}]*}'   # mkntfs 必须是 true
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# 路径统一由 scripts/env.sh 解析（环境变量 > imgtool.conf > 自动探测）。
# shellcheck disable=SC1091
. "$REPO/scripts/env.sh"
SRC=${1:-$IMGTOOL_WS_ROOT/third_party/ntfs-3g}
OD=${2:-$IMGTOOL_BUILD_ROOT/mkntfs_obj}
CC=${CC:-gcc}
OBJCOPY=${OBJCOPY:-objcopy}

[ -d "$SRC/ntfsprogs" ] || { echo "缺 ntfs-3g 源码：$SRC/ntfsprogs" >&2; exit 1; }
[ -f "$SRC/config.h" ]  || { echo "缺 $SRC/config.h（先 ./configure 一次）" >&2; exit 1; }
mkdir -p "$OD"

INC="-DHAVE_CONFIG_H -I$SRC -I$SRC/include -I$SRC/include/ntfs-3g -I$SRC/ntfsprogs"
CF="-O2 -fPIC -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Wno-unused-result -Wno-address-of-packed-member"

echo "== 源码 $SRC"
echo "== 输出 $OD"

# ★ 必需补丁（2026-10-26）：mkntfs 是一次性 CLI，模块级状态靠"进程退出"归零；
#   我们是【进程内复用】（objcopy 把 main 改名成 mkntfs_main），所以
#   mkntfs_cleanup() 里 free(g_allocation) 之后必须置 NULL，否则同一进程里
#   第二次格式化会从野指针走链表 → SIGSEGV（真机 bitmap_allocate+84 崩过）。
#   幂等：已经有这行就跳过。源码树不进仓库也行 —— 每次编 .o 之前都会补。
patch_mkntfs() {
  local f="$SRC/ntfsprogs/mkntfs.c"
  [ -f "$f" ] || { echo "!! 缺 $f" >&2; return 1; }
  if grep -q 'g_allocation = NULL;' "$f"; then
    echo "  [patch] mkntfs g_allocation 已置空，跳过"
    return 0
  fi
  python3 - "$f" <<'PY' || return 1
import sys
p = sys.argv[1]
s = open(p, encoding='utf-8', errors='surrogateescape').read()
old = "\tp = g_allocation;\n\twhile (p) {\n\t\tq = p->next;\n\t\tfree(p);\n\t\tp = q;\n\t}\n}"
assert old in s, "cleanup 段没找到（ntfs-3g 版本变了？）"
new = old[:-1] + "\t/* 2026-10-26: 进程内可重复格式化（img_ntfs_fmt 把 main 前插成 mkntfs_main）*/\n\tg_allocation = NULL;\n}"
s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8', errors='surrogateescape').write(s)
print("  [patch] 已给 mkntfs.c 补上 g_allocation = NULL")
PY
}
patch_mkntfs || exit 1

for f in attrdef boot sd utils; do
  $CC $CF $INC -c "$SRC/ntfsprogs/$f.c" -o "$OD/mkntfs-$f.o"
done
$CC $CF $INC -c "$SRC/ntfsprogs/mkntfs.c" -o "$OD/mkntfs-main.o"
# imgntfscp / imgntfsfs 两个小工具用到的三份（各自也带 main）
for f in ntfscp ntfsls ntfscat; do
  $CC $CF $INC -c "$SRC/ntfsprogs/$f.c" -o "$OD/$f.o"
done

# ★ 上游这些文件都有 main，会和我们的入口抢；统一改名（与主机历史约定一致）
$OBJCOPY --redefine-sym main=mkntfs_main "$OD/mkntfs-main.o"
for f in ntfscp ntfsls ntfscat; do
  $OBJCOPY --redefine-sym main=${f}_main "$OD/$f.o"
done

nm --defined-only "$OD"/*.o | grep -qE ' [TDB] main$' && {
  echo "!! 还有裸 main，会抢入口" >&2; exit 1; }
nm --defined-only "$OD/mkntfs-main.o" | grep -q ' mkntfs_main$' || {
  echo "!! mkntfs_main 没生成" >&2; exit 1; }

echo "== 完成：$(ls "$OD" | wc -l) 个 .o"
ls -l "$OD"
echo
echo "别忘了：cmake .. -DIMG_WITH_NTFS=ON"