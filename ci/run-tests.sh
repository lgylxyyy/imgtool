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
if [ "${MODE:-full}" = "debug-fat" ]; then
  echo "════ 调试模式：跑真实测试，崩溃就抓 argv + 栈 ════"
  T="$ROOT/build/tools"
  # 套一层壳：正常快跑；只有子进程被信号杀掉时，才用 gdb 重跑同一条命令打栈。
  cp -f "$T/imgcmd" "$T/imgcmd.real"
  cat > "$T/imgcmd" <<'WRAP'
#!/bin/bash
here="$(cd "$(dirname "$0")" && pwd)"
real="$here/imgcmd.real"
"$real" "$@"; rc=$?
if [ "$rc" -ge 128 ]; then
  echo "=== 崩溃 rc=$rc ===" >&2
  echo "=== argv: $* ===" >&2
  gdb -batch -nx -ex run -ex 'bt 20' --args "$real" "$@" 2>&1 | tail -40 >&2
fi
exit "$rc"
WRAP
  chmod +x "$T/imgcmd"
  for t in copy-test.py fileops-test.py xcopy-test.py ads-test.sh fat-fs-test.py; do
    echo
    echo "──── $t ────"
    case "$t" in
      *.py) python3 "$ROOT/tests/$t" 2>&1 | grep -nE '崩溃|argv:|^#[0-9]+ |✅|❌|结果：' | tail -80 || true ;;
      *)    bash    "$ROOT/tests/$t" 2>&1 | grep -nE '崩溃|argv:|^#[0-9]+ |✅|❌|结果：' | tail -80 || true ;;
    esac
  done
  exit 0
fi

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