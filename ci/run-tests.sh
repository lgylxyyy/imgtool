#!/usr/bin/env bash
# ci/run-tests.sh —— 在 ci/Dockerfile 的镜像里跑完整构建与回归
#
# 镜像内执行；源码通过 -v "$PWD:/src" 挂进来，所以这个脚本永远跑的是
# 当前检出的代码，不是镜像里的旧副本。
#
# 与本地 tests/run_all.sh 的区别只有两点：
#   ① 先把 mkntfs 对象编出来并显式传给 CMake（镜像里带了 ntfs-3g 源码）
#   ② 断言 features 全为 true —— 缺库时测试会静默跳过，CI 里不能静默
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "════ 1. 生成 mkntfs 对象 ════"
: "${NTFS3G_SRC:=/opt/ntfs3g-src}"
OBJ_DIR="${OBJ_DIR:-$ROOT/build/mkntfs_obj}"
tools/build-mkntfs-obj.sh "$NTFS3G_SRC" "$OBJ_DIR"

echo
echo "════ 2. 配置与编译 ════"
cmake -S "$ROOT" -B "$ROOT/build" \
      -DCMAKE_BUILD_TYPE=Release \
      -DIMG_WITH_NTFS=ON \
      -DIMG_MKNTFS_OBJ_DIR="$OBJ_DIR" \
      -DIMG_BUILD_TESTS=ON
cmake --build "$ROOT/build" -j"$(nproc)"

echo
echo "════ 3. 依赖能力断言（缺库会让用例静默跳过，这里必须拦下）════"
FEAT="$("$ROOT/build/tools/imgcmd" --list | python3 -c 'import json,sys;d=json.load(sys.stdin)["features"];print("%s/%s/%s/%s" % (d["wimlib"],d["ntfs3g"],d["zlib"],d["mkntfs"]))')"
echo "features(wimlib/ntfs3g/zlib/mkntfs) = $FEAT"
if [ "$FEAT" != "True/True/True/True" ]; then
  echo "错误：镜像缺依赖，拒绝跑一个会静默跳过用例的回归" >&2
  exit 1
fi

echo
echo "════ 4. 全量回归 ════"
exec bash "$ROOT/tests/run_all.sh"