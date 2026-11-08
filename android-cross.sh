#!/usr/bin/env bash
# android-cross.sh —— aarch64 主机上交叉编译 imgtool（NDK sysroot + 宿主 clang）
# 用法：./android-cross.sh [abi...]      默认 arm64-v8a armeabi-v7a x86_64
#       TOOLS=1 TESTS=1 时一并交叉编译命令行工具与单元测试（推到真机上跑）
#       NTFS=OFF 可以不把 GPL 的 mkntfs 编进来（默认 ON：用户 2026-10-22 决定开源，接受 GPL）
set -euo pipefail
NDK="${NDK:-/workspace/sdk/android-ndk/android-ndk-r28c}"
API="${API:-21}"
ABIS=("$@"); [ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a armeabi-v7a x86_64)
SRC="$(cd "$(dirname "$0")" && pwd)"
OUT="${OUT:-/workspace/build/android}"

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
