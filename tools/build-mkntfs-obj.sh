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
#   默认 /workspace/third_party/ntfs-3g → /workspace/build/mkntfs_obj
#
# 之后配置（少一个开关就会退回 OFF）：
#   cd build && cmake .. -DCMAKE_BUILD_TYPE=Release -DIMG_WITH_NTFS=ON && make -j8
#   ./tools/imgcmd --list | grep -o '"features":{[^}]*}'   # mkntfs 必须是 true
set -euo pipefail

SRC=${1:-/workspace/third_party/ntfs-3g}
OD=${2:-/workspace/build/mkntfs_obj}
CC=${CC:-gcc}
OBJCOPY=${OBJCOPY:-objcopy}

[ -d "$SRC/ntfsprogs" ] || { echo "缺 ntfs-3g 源码：$SRC/ntfsprogs" >&2; exit 1; }
[ -f "$SRC/config.h" ]  || { echo "缺 $SRC/config.h（先 ./configure 一次）" >&2; exit 1; }
mkdir -p "$OD"

INC="-DHAVE_CONFIG_H -I$SRC -I$SRC/include -I$SRC/include/ntfs-3g -I$SRC/ntfsprogs"
CF="-O2 -fPIC -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -Wno-unused-result -Wno-address-of-packed-member"

echo "== 源码 $SRC"
echo "== 输出 $OD"
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