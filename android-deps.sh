#!/usr/bin/env bash
# android-deps.sh —— 把 imgtool 的第三方依赖交叉编译到 stage/<abi>
#
# 依赖顺序：zlib(NDK 自带，静态) → libxml2 → ntfs-3g → wimlib(要带 ntfs-3g) → mkntfs → libiconv → hivex
# 用法：  ./android-deps.sh [abi...]            默认 arm64-v8a、全部库
#         DEPS="wimlib ntfs3g" ./android-deps.sh arm64-v8a
# 环境变量：NDK API OUT WORK SRC_DL NTFS3G_SRC
# 默认值由脚本位置推导；要复用别处的依赖缓存，显式设 SRC_DL / NTFS3G_SRC
set -euo pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 路径统一由 scripts/env.sh 解析（环境变量 > imgtool.conf > 自动探测）。
# shellcheck disable=SC1091
. "$SRC/scripts/env.sh"
imgtool_need "Android NDK" "${IMGTOOL_NDK:-}" "设 NDK / ANDROID_NDK_HOME，或运行 ./configure-local.sh" || exit 1
NDK="$IMGTOOL_NDK"
API="${API:-21}"
OUT="${OUT:-$IMGTOOL_BUILD_ROOT/android}"
SRC_DL="${SRC_DL:-$IMGTOOL_DEPS_ROOT/src}"
NTFS3G_SRC="${NTFS3G_SRC:-$IMGTOOL_DEPS_ROOT/third_party/ntfs-3g}"
WORK="$OUT/deps"
# 注意：绝不能用 LIBS/CFLAGS 之类的名字做脚本自己的变量——它们会被 autoconf 继承
SYS="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
mkdir -p "$WORK"

abi_env() {
  case "$1" in
    arm64-v8a)   TRIPLE=aarch64-linux-android ;;
    armeabi-v7a) TRIPLE=arm-linux-androideabi ;;
    x86_64)      TRIPLE=x86_64-linux-android ;;
    x86)         TRIPLE=i686-linux-android ;;
    *) echo "未知 ABI: $1" >&2; return 1 ;;
  esac
  STAGE="$OUT/stage/$1"
  TGT="$TRIPLE$API"
  mkdir -p "$STAGE"/{include,lib,lib/pkgconfig,bin}
  R="-rtlib=compiler-rt"
  export CC="clang --target=$TGT --sysroot=$SYS $R"
  export CXX="clang++ --target=$TGT --sysroot=$SYS $R"
  # ARM ABI 默认 char 是 unsigned，x86/glibc 是 signed；hivex 的 UTF-16/名字比对
  # 依赖符号性，统一按主机语义走 -fsigned-char（工具链侧同样设置）
# HAVE_FFS：ntfs-3g 的 compat.h 未定义它时会 `extern int ffs(int)`，而 Bionic 的
  # strings.h 把 ffs 写成 static inline → 32 位安卓 "static declaration follows
  # non-static" 编不过。Bionic 确实有 ffs，所以如实声明。
    export CFLAGS="-O2 -fPIC -fsigned-char -DHAVE_FFS=1"
  export CXXFLAGS="$CFLAGS -nostdinc++ -isystem $SYS/usr/include/c++/v1"
  export LDFLAGS="--target=$TGT --sysroot=$SYS $R -L$STAGE/lib -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1"
  export CPPFLAGS="-I$STAGE/include -I$SYS/usr/include"
  export PKG_CONFIG_PATH="$STAGE/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="$STAGE/lib/pkgconfig"
  unset CONFIG_SITE HOST_CC LIBS 2>/dev/null || true
}
have() { [ -e "$STAGE/lib/$1" ]; }

# 源码目录必须在位；tar 包在但目录没解（上次中断留下的常见状态）就自动补解。
need_src() {   # $1=目录名  $2=tar 包名
  [ -d "$SRC_DL/$1" ] && return 0
  if [ -f "$SRC_DL/$2" ]; then
    echo "  [src] 解压 $2"
    tar --no-same-permissions -xf "$SRC_DL/$2" -C "$SRC_DL"
  fi
  [ -d "$SRC_DL/$1" ] || { echo "!! 缺源码：$SRC_DL/$1（请先下载 $2）" >&2; return 1; }
}

build_libxml2() {
  have libxml2.a && { echo "  [libxml2] 已存在，跳过"; return 0; }
  need_src libxml2-2.14.6 libxml2-2.14.6.tar.xz || return 1
  local s="$SRC_DL/libxml2-2.14.6" b="$WORK/libxml2-$1"
  cmake -S "$s" -B "$b" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$PROJ/cmake/android-arm64host.cmake" \
    -DNDK_ROOT="$NDK" -DIMG_ANDROID_ABI="$1" -DIMG_ANDROID_API="$API" \
    -DIMG_ANDROID_STAGE="$STAGE" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$STAGE" -DBUILD_SHARED_LIBS=OFF \
    -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_LZMA=OFF \
    -DLIBXML2_WITH_ZLIB=ON -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF \
    >"$b.cfg.log" 2>&1
  cmake --build "$b" -j"$(nproc)" >>"$b.log" 2>&1
  cmake --install "$b" >>"$b.log" 2>&1
  echo "  [libxml2] → $(ls $STAGE/lib/libxml2.a)"
}

build_wimlib() {
  have libwim.a && { echo "  [wimlib] 已存在，跳过"; return 0; }
  need_src wimlib-1.14.4 wimlib-1.14.4.tar.gz || return 1
  local s="$SRC_DL/wimlib-1.14.4" b="$WORK/wimlib-$1"
  rm -rf "$b"; cp -a "$s" "$b"
  ( cd "$b"
    # ★ 必须开 ntfs-3g：img_wim_apply 用的是 WIMLIB_EXTRACT_FLAG_NTFS（直接把
    #   系统灌进未挂载的 NTFS 卷）。之前图省事写了 --without-ntfs-3g，交叉出来的
    #   libwim 不支持该模式，真机上 wim.apply 9 毫秒就报"底层读写失败"。
    #   ntfs-3g 的 .pc 我们没装，所以直接覆盖 pkg-config 的两个变量。
    ./configure --host="$TRIPLE" --prefix="$STAGE" \
      --enable-shared=no --enable-static=yes --with-pic \
      --with-ntfs-3g --without-fuse --without-openssl \
      LIBNTFS_3G_CFLAGS="-I$STAGE/include" \
      LIBNTFS_3G_LIBS="-L$STAGE/lib -lntfs-3g -lz -lm" \
      LIBS="-lxml2 -lz -lm -ldl" \
      ac_cv_have_decl___builtin_bswap16=yes \
      >cfg.log 2>&1 && make -j"$(nproc)" >>make.log 2>&1 && make install >>make.log 2>&1 )
  echo "  [wimlib] → $(ls $STAGE/lib/libwim.a)"
}

build_ntfs3g() {
  have libntfs-3g.a && { echo "  [ntfs-3g] 已存在，跳过"; return 0; }
  [ -d "$NTFS3G_SRC" ] || { echo "!! 缺 ntfs-3g 源码：$NTFS3G_SRC" >&2; return 1; }
  local b="$WORK/ntfs3g-$1"
  rm -rf "$b"; cp -a "$NTFS3G_SRC" "$b"; ( cd "$b"
    make distclean >/dev/null 2>&1 || true
    # 只要库：ntfs-3g/lowntfs-3g 这两个挂载程序需要 libfuse（安卓上没有），
    # mkntfs 等 ntfsprogs 也不需要——格式化用的是库内的 ntfs_format()。
    ./configure --host="$TRIPLE" --prefix="$STAGE" \
      --enable-static=yes --enable-shared=no \
      --disable-nfconv --disable-ntfsprogs --without-fuse \
      >cfg.log 2>&1
    make -C libntfs-3g -j"$(nproc)" >>make.log 2>&1
    mkdir -p "$STAGE/lib"
    cp -f libntfs-3g/.libs/libntfs-3g.a "$STAGE/lib/" )
  # 公开头在 include/ntfs-3g/（imgtool 里是 #include <ntfs-3g/volume.h> 这种形式）
  mkdir -p "$STAGE/include/ntfs-3g"
  cp -f "$b"/include/ntfs-3g/*.h "$STAGE/include/ntfs-3g/"
  # ★ 上游的 types.h 里有 #ifdef HAVE_CONFIG_H → #include "config.h"，而交叉编译
  #   时我们要单独编 ntfsprogs 的 mkntfs 等目标文件，必须能解析到这份 config.h，
  #   否则 "config.h file not found"（v7a/x86_64 就栽过）。
  cp -f "$b"/config.h "$STAGE/include/ntfs-3g/config.h"
  echo "  [ntfs-3g] → $(ls $STAGE/lib/libntfs-3g.a)"
}

build_libiconv() {
  have libiconv.a && { echo "  [libiconv] 已存在，跳过"; return 0; }
  need_src libiconv-1.18 libiconv-1.18.tar.gz || return 1
  local b="$WORK/libiconv-$1"
  rm -rf "$b"; cp -a "$SRC_DL/libiconv-1.18" "$b"; ( cd "$b"
    # ★ 为什么需要它：hivex 解节点名走 iconv(3)。BCD 里的名字是 Latin-1 压缩存储，
    #   而 Bionic 的 iconv 只支持 UTF-16LE/UTF-8/ASCII，iconv_open("UTF-8","LATIN1")
    #   直接 EINVAL → 所有节点名读不出、BCD 查不到对象。
    #   glibc 全支持，所以主机上永远测不出来。
    ./configure --host="$TRIPLE" --prefix="$STAGE" \
      --enable-static --disable-shared --disable-nls >cfg.log 2>&1
    make -j"$(nproc)" >>make.log 2>&1
    make install >>make.log 2>&1 || true )
  echo "  [libiconv] → $(ls $STAGE/lib/libiconv.a 2>/dev/null || echo 缺失)"
}

build_mkntfs() {
  # mkntfs 是 GPL-2.0+ 的 ntfsprogs 代码。安卓端要不要它 = 要不要在 APK 里带 GPL 代码。
  # 用户 2026-10-22 拍板：要（反正项目也开源）。没有它就只能在电脑上格 NTFS。
  local od="$STAGE/mkntfs_obj" b="$WORK/mkntfs-$1"
  [ -e "$od/mkntfs-main.o" ] && [ -e "$od/ntfscp.o" ] && { echo "  [mkntfs] 已存在，跳过"; return 0; }
  [ -d "$NTFS3G_SRC" ] || { echo "!! 缺 ntfs-3g 源码：$NTFS3G_SRC" >&2; return 1; }
  rm -rf "$b"; cp -a "$NTFS3G_SRC" "$b"; mkdir -p "$od"
  # ★ 必需补丁（2026-10-26）：mkntfs 是一次性 CLI，模块级状态靠"进程退出"归零；
  #   我们是进程内复用，所以 free(g_allocation) 之后必须置 NULL —— 否则同一进程里
  #   第二次 fs.format --fs=ntfs 会从野指针走链表 → SIGSEGV（真机 bitmap_allocate+84）。
  #   幂等：已经有这行就跳过。与主机 tools/build-mkntfs-obj.sh 里的补丁同源。
  if grep -q 'g_allocation = NULL;' "$b/ntfsprogs/mkntfs.c"; then
    echo "  [patch] mkntfs g_allocation 已置空，跳过"
  else
    python3 - "$b/ntfsprogs/mkntfs.c" <<'PY' || { echo "  !! mkntfs 补丁失败" >&2; return 1; }
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
  fi
  # ★ config.h 用 stage 里那份（为这个 ABI 生成的）；绝不把源码树根加进 -I，
  #   那里躺着别人留下的宿主 config.h，混进来就是"能编译但全错"。
  local INC="-DHAVE_CONFIG_H -I$STAGE/include/ntfs-3g -I$STAGE/include -I$b/ntfsprogs"
  local f
  for f in attrdef boot sd utils; do
    $CC $CFLAGS $INC -c "$b/ntfsprogs/$f.c" -o "$od/mkntfs-$f.o" || { echo "  !! mkntfs-$f.c 编译失败" >&2; return 1; }
  done
  $CC $CFLAGS $INC -c "$b/ntfsprogs/mkntfs.c" -o "$od/mkntfs-main.o" \
    || { echo "  !! mkntfs.c 编译失败" >&2; return 1; }
  # 工具（imgntfscp / imgntfsfs）要的三份，各自也带 main
  for f in ntfscp ntfsls ntfscat; do
    $CC $CFLAGS $INC -c "$b/ntfsprogs/$f.c" -o "$od/$f.o" \
      || { echo "  !! $f.c 编译失败" >&2; return 1; }
  done
  # ★ 上游这些文件都以 main 为入口，而我们的可执行文件自己写 main：
  #   统一改名成 <名字>_main（与主机 build/mkntfs_obj 的约定一致），否则抢入口。
  llvm-objcopy --redefine-sym main=mkntfs_main "$od/mkntfs-main.o"
  for f in ntfscp ntfsls ntfscat; do
    llvm-objcopy --redefine-sym main=${f}_main "$od/$f.o"
  done
  nm --defined-only "$od/mkntfs-main.o" | grep -q ' mkntfs_main$' || { echo "!! mkntfs_main 没生成" >&2; return 1; }
  nm --defined-only "$od"/*.o | grep -qE ' [TDB] main$' && { echo "!! 还留着裸 main，会抢入口" >&2; return 1; }
  echo "  [mkntfs] → $od（$(ls "$od" | wc -l) 个 .o）"
}

build_hivex() {
  have libhivex.a && { echo "  [hivex] 已存在，跳过"; return 0; }
  need_src hivex-1.3.24 hivex-1.3.24.tar.gz || return 1
  local s="$SRC_DL/hivex-1.3.24" b="$WORK/hivex-$1"
  rm -rf "$b"; cp -a "$s" "$b"; ( cd "$b"
    # gnulib 的 full_read/full_write 与 wimlib 里的同名符号会冲突（主机上各自进
    # .so 看不出来，安卓端要静态合进一个 libimgtool.so 就会 duplicate symbol）。
    # 在这一侧统一改名，冲突从根上不可能发生。
    ./configure --host="$TRIPLE" --prefix="$STAGE" \
      CPPFLAGS="$CPPFLAGS -Dfull_read=hivex_gnulib_full_read -Dfull_write=hivex_gnulib_full_write" \
      --enable-static=yes --disable-shared --disable-ocaml --disable-perl \
      --disable-python --disable-ruby --without-readline \
      --disable-optimize --disable-gcc-warnings \
      ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
      >cfg.log 2>&1
    # 顶层 make 会走进 images/ 去*运行*交叉编译出来的 mklarge（交叉编译必炸），
    # 我们只要库：单独编 gnulib 与 lib，再把两者合成一个 libhivex.a。
    make -C gnulib/lib -j"$(nproc)" >>make.log 2>&1
    make -C lib -j"$(nproc)" >>make.log 2>&1
    mkdir -p "$STAGE/lib" "$STAGE/include"
    [ -f "$STAGE/lib/libiconv.a" ] || echo "  !! 缺 libiconv.a：安卓上 hivex 读不出 Latin-1 节点名" >&2
    ar -M >>/dev/null <<MRI
CREATE $STAGE/lib/libhivex.a
ADDLIB lib/.libs/libhivex.a
ADDLIB gnulib/lib/.libs/libgnu.a
ADDLIB $STAGE/lib/libiconv.a
ADDLIB $STAGE/lib/libcharset.a
SAVE
END
MRI
    cp -f include/hivex.h "$STAGE/include/" )
  echo "  [hivex] → $(ls $STAGE/lib/libhivex.a)"
}


# liblzma（7z 必需：7z 的内容是 LZMA/LZMA2；没有它 libarchive 只能列 7z 的目录、解不出数据）
build_xz() {
  have liblzma.a && { echo "  [xz] 已存在，跳过"; return 0; }
  need_src xz-5.6.3 xz-5.6.3.tar.xz || return 1
  local s="$SRC_DL/xz-5.6.3" b="$WORK/xz-$1"
  rm -rf "$b"; mkdir -p "$b"
  ( cd "$b"
    "$s/configure" --host="$TRIPLE" --prefix="$STAGE" \
      --disable-shared --enable-static --disable-xz --disable-xzdec \
      --disable-lzmadec --disable-lzmainfo --disable-scripts --disable-doc \
      --disable-nls --disable-microlzma >"$b.cfg.log" 2>&1
    make -j"$(nproc)" >>"$b.log" 2>&1
    make install >>"$b.log" 2>&1 )
  [ -e "$STAGE/lib/liblzma.a" ] || { echo "!! [xz] 失败，看 $b.log" >&2; return 1; }
  echo "  [xz] → $STAGE/lib/liblzma.a"
}

# libarchive（只读）：zip/7z/rar/tar/cab 一个库全覆盖。不带 openssl/xml2/expat/lz4/zstd/bz2。
build_libarchive() {
  have libarchive.a && { echo "  [libarchive] 已存在，跳过"; return 0; }
  need_src libarchive-3.7.7 libarchive-3.7.7.tar.xz || return 1
  local s="$SRC_DL/libarchive-3.7.7" b="$WORK/libarchive-$1"
  rm -rf "$b"
  cmake -S "$s" -B "$b" -G "Unix Makefiles" \
    -DCMAKE_TOOLCHAIN_FILE="$PROJ/cmake/android-arm64host.cmake" \
    -DNDK_ROOT="$NDK" -DIMG_ANDROID_ABI="$1" -DIMG_ANDROID_API="$API" \
    -DIMG_ANDROID_STAGE="$STAGE" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$STAGE" -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_PREFIX_PATH="$STAGE" \
    -DENABLE_OPENSSL=OFF -DENABLE_LIBXML2=OFF -DENABLE_EXPAT=OFF \
    -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF -DENABLE_BZip2=OFF \
    -DENABLE_LZMA=ON -DENABLE_ZLIB=ON -DENABLE_TEST=OFF \
    -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF \
    >"$b.cfg.log" 2>&1 || { echo "!! [libarchive] configure 失败，看 $b.cfg.log" >&2; return 1; }
  cmake --build "$b" -j"$(nproc)" >>"$b.log" 2>&1 || { echo "!! [libarchive] 编不过，看 $b.log" >&2; return 1; }
  cmake --install "$b" >>"$b.log" 2>&1
  [ -e "$STAGE/lib/libarchive.a" ] || { echo "!! [libarchive] 没产物" >&2; return 1; }
  echo "  [libarchive] → $STAGE/lib/libarchive.a"
}

PROJ="$(cd "$(dirname "$0")" && pwd)"
ABIS=("$@"); [ ${#ABIS[@]} -eq 0 ] && ABIS=(arm64-v8a)
read -r -a DEPS_ARR <<< "${DEPS:-libxml2 ntfs3g wimlib mkntfs libiconv hivex xz libarchive}"
for abi in "${ABIS[@]}"; do
  echo "=== $abi (API $API) stage=$OUT/stage/$abi ==="
  abi_env "$abi"
  for L in "${DEPS_ARR[@]}"; do
    case "$L" in
      libxml2) build_libxml2 "$abi" ;;
      wimlib)  build_wimlib  "$abi" ;;
      ntfs3g)   build_ntfs3g   "$abi" ;;
      libiconv) build_libiconv "$abi" ;;
      mkntfs)   build_mkntfs   "$abi" ;;
      hivex)   build_hivex   "$abi" ;;
      xz)        build_xz        "$abi" ;;
      libarchive) build_libarchive "$abi" ;;
      *) echo "  未知库: $L" >&2 ;;
    esac
  done
done
