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
# ★ 2026-11-10：API 从 21 提到 26。Bionic 的 libc 桩在 21 里没有 `stdin`/`stdout`
#   （__INTRODUCED_IN(23)）和 `hasmntopt`（__INTRODUCED_IN(26)），而 wimlib 的
#   textfile.c/header.c 与 ntfs-3g 的 volume.c 正好引用了它们 —— 编 .so 时因为
#   shared 允许未定义符号没暴露，一编命令行工具(可执行文件)就链接失败：
#     ld.lld: error: undefined symbol: stdin / stdout
#     ld.lld: error: undefined symbol: hasmntopt
#   这两种符号没法用 stub 补（stdin/stdout 是数据符号，值是 libc 的 __sF），
#   所以按 bionic 的真实门槛统一到 26。代价：产物要求 Android 8.0+（App minSdk 33，无影响）。
API="${API:-26}"
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
  # ★ 缓存自愈 2：有人为了「先编过去」往 CMakeCache 里塞过
  #   -Wl,--unresolved-symbols=ignore-all（REQ-20261110-71 §3 就是这种产物）。
  #   留着它会把真正的链接错误藏起来，这里直接摘掉。
  if [ -f "$b/CMakeCache.txt" ] && grep -q "unresolved-symbols=ignore-all" "$b/CMakeCache.txt"; then
    sed -i 's/ *-Wl,--unresolved-symbols=ignore-all//g' "$b/CMakeCache.txt"
    echo "  [cache] 已摘掉遗留的 --unresolved-symbols=ignore-all（它会掩盖链接错误）"
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
  ALIGN="$(readelf -lW "$so" | awk '/LOAD/{print $NF}' | sort -u | tr '\n' ' ')"
  printf '    LOAD对齐 %s\n' "$ALIGN"
  # ★ 2026-11-10：arm64 的 .so 必须每个 PT_LOAD 都 16 KB 对齐，否则 Android 16
  #   装机会弹「此应用不满足 16 KB 页大小要求」（libimgapp 就是缺这个参数）。
  #   这里当场失败 —— 别等装到手机上才发现。
  if [ "$abi" = "arm64-v8a" ] && [ "$ALIGN" != "0x4000 " ]; then
    echo "  ❌ arm64 的 LOAD 对齐是 [$ALIGN]，要求 0x4000（16 KB）" >&2
    exit 1
  fi
  printf '    NEEDED   %s\n' "$(readelf -d "$so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')"
  # ★ 2026-11-10：.so 允许未定义符号（运行时从 libc 解析），所以「编得过」不代表能跑：
  #   stdin/stdout(__INTRODUCED_IN(23))、hasmntopt(26) 在低 API 的 libc 里根本不存在，
  #   设备上走到那条路径才会炸。这里对着【本 API 的 libc 桩】逐个核，缺了就当场失败。
  UNDEF="$(nm -D --undefined-only "$so" | awk '{print $NF}' | sed 's/@.*//' | sort -u || true)"
  case "$abi" in
    arm64-v8a)   _t=aarch64-linux-android  ;;
    armeabi-v7a) _t=arm-linux-androideabi  ;;
    x86_64)      _t=x86_64-linux-android   ;;
    x86)         _t=i686-linux-android     ;;
  esac
  LIBCSTUB="$SYS/usr/lib/$_t/$API/libc.so"
  MISSING=""
  for sym in stdin stdout stderr hasmntopt; do
    case " $UNDEF " in *" $sym "*)
      if ! readelf -sW --dyn-syms "$LIBCSTUB" 2>/dev/null | grep -q " $sym@"; then
        MISSING="$MISSING $sym"
      fi ;;
    esac
  done
  if [ -n "$MISSING" ]; then
    echo "  ❌ libimgtool.so 需要 bionic API $API 里没有的符号：[$MISSING ]" >&2
    echo "     stdin/stdout 要 API 23，hasmntopt 要 26；把 API 提到 26 再编" >&2
    exit 1
  fi
  printf '    导出     %s 个符号\n' "$(nm -D --defined-only "$so" | grep -cE ' [TWtw] ')"
done
echo "=== 完成：${ABIS[*]} ==="
