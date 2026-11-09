#!/usr/bin/env bash
# android-cross.sh —— aarch64 主机上交叉编译 imgtool（NDK sysroot + 宿主 clang）
# 用法：./android-cross.sh [abi...]      默认 arm64-v8a armeabi-v7a x86_64
#       TOOLS=1 TESTS=1 时一并交叉编译命令行工具与单元测试（推到真机上跑）
#       NTFS=OFF 可以不把 GPL 的 mkntfs 编进来（默认 ON：用户 2026-10-22 决定开源，接受 GPL）
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 路径统一由 scripts/env.sh 解析（环境变量 > imgtool.conf > 自动探测）。
# shellcheck disable=SC1091
. "$SRC/scripts/env.sh"
imgtool_need "Android NDK" "${IMGTOOL_NDK:-}" "设 NDK / ANDROID_NDK_HOME，或运行 ./configure-local.sh" || exit 1
NDK="$IMGTOOL_NDK"
BUILD_ROOT="$IMGTOOL_BUILD_ROOT"
API="${API:-21}"
ABIS=("$@"); [ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a armeabi-v7a x86_64)
OUT="${OUT:-$BUILD_ROOT/android}"

# 0) 一次性把 NDK builtins 挂进宿主 clang 的 resource-dir（缺了会 ld -lgcc 失败）
RD="$(clang -print-resource-dir)/lib/linux"
# ★ 用 ls -L 解引用：NDK 搬家后旧软链会变成悬空链接，只看名字会误判"已存在"
if ! ls -L "$RD"/libclang_rt.builtins-*-android.a >/dev/null 2>&1; then
  mkdir -p "$RD"
  rm -f "$RD"/libclang_rt.builtins-*-android.a
  ln -sf "$NDK"/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/19/lib/linux/libclang_rt.builtins-*-android.a "$RD"/
  echo "[toolchain] 已链接 compiler-rt builtins → $RD"
fi

SYS="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"

# 1.5) 为每个 ABI 准备 stage 前缀：把 NDK 自带的 zlib（头 + 静态库）放成标准布局，
#      交叉编译回来的第三方库也装进这里，find_package 就能正常命中。
stage_prepare() {
  local abi=$1 triple=$2 st="$OUT/stage/$abi"
  mkdir -p "$st/include" "$st/lib" "$st/lib/pkgconfig"
  if [ ! -e "$st/lib/libz.a" ] && [ -e "$SYS/usr/lib/$triple/libz.a" ]; then
    cp -f "$SYS"/usr/include/zlib.h "$SYS"/usr/include/zconf.h "$st/include/"
    cp -f "$SYS/usr/lib/$triple/libz.a" "$st/lib/"
    echo "[stage] $abi: NDK zlib 1.x 静态库就位"
  fi
  echo "$st"
}

for abi in "${ABIS[@]}"; do
  case "$abi" in
    arm64-v8a)   triple=aarch64-linux-android ;;
    armeabi-v7a) triple=arm-linux-androideabi ;;
    x86_64)      triple=x86_64-linux-android ;;
    x86)         triple=i686-linux-android ;;
  esac
  STAGE="$(stage_prepare "$abi" "$triple" | tail -1)"
  b="$OUT/$abi"
  echo "=== $abi (API $API) → $b ==="
  # 缓存自愈：CMakeCache.txt 记着构建时的源码绝对路径。源码换位置（换分支、
  # 换 clone 目录、合并仓库）后复用同一 build 目录，CMake 会以
  # "source does not match ... used to generate cache" 拒绝构建。
  # 检测到不一致就删掉缓存重配（不影响 stage/ 里的依赖产物）。
  cmake_src() { [ -f "$1/CMakeCache.txt" ] && sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$1/CMakeCache.txt"; }
  if [ -f "$b/CMakeCache.txt" ] && [ "$(cmake_src "$b")" != "$SRC" ]; then
    echo "  [cache] 源码路径已变更（$(cmake_src "$b") → $SRC），清除构建缓存"
    rm -rf "$b"
  fi
  cmake -S "$SRC" -B "$b" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/android-arm64host.cmake" \
    -DNDK_ROOT="$NDK" -DIMG_ANDROID_ABI="$abi" -DIMG_ANDROID_API="$API" \
    -DIMG_ANDROID_STAGE="$STAGE" -DIMG_ARCHIVE_ROOT="$STAGE" \
    -DIMG_BUILD_TESTS="${TESTS:-OFF}" -DIMG_BUILD_TOOLS="${TOOLS:-OFF}" \
    -DIMG_WITH_NTFS="${NTFS:-ON}" -DIMG_MKNTFS_OBJ_DIR="$STAGE/mkntfs_obj"
  cmake --build "$b" -j"$(nproc)"
  so=$(find "$b" -maxdepth 1 -name 'libimgtool.so' | head -1)
  [ -n "$so" ] || { echo "!! $abi 没产出 libimgtool.so"; exit 1; }
  printf '    ELF      %s\n' "$(readelf -h "$so" | awk -F: '/Class/{gsub(/ /,"",$2);printf "%s ",$2} /Machine/{gsub(/^ +/,"",$2);printf "%s",$2}')"
  printf '    LOAD对齐 %s\n' "$(readelf -lW "$so" | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')"
  printf '    NEEDED   %s\n' "$(readelf -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
  printf '    导出     %s 个符号\n' "$(nm -D --defined-only "$so" | grep -cE ' [TWtw] ')"
done
echo "=== 完成：${ABIS[*]} ==="
