# ============================================================================
# cmake/android-arm64host.cmake —— 在 aarch64(ARM64) Linux 主机上交叉编译 Android
# ============================================================================
# 为什么不用 NDK 自带的 android.toolchain.cmake：
#   Google 只为 NDK 提供 linux-x86_64 宿主二进制，官方没有 linux-arm64 宿主。
#   本文件把「宿主 clang/lld（原生，快）」与「NDK 的 Bionic sysroot（官方，权威）」
#   拼起来用：编译前端是宿主的，目标 ABI、头文件、链接库全部来自 NDK sysroot。
#
# 用法（arm64-v8a 为例）：
#   cmake -S . -B build-android/arm64-v8a \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/android-arm64host.cmake \
#     -DNDK_ROOT=<ndk 目录> \
#     -DIMG_ANDROID_ABI=arm64-v8a -DIMG_ANDROID_API=21
#
# 必要前提（一次性）：把 NDK 的 compiler-rt builtins 暴露给宿主 clang
#     clang -print-resource-dir  →  .../lib/clang/18/lib/linux
#     ln -sf $NDK/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/19/lib/linux/ \
#            libclang_rt.builtins-*-android.a  <那个目录>
#   并在编译选项里带 -rtlib=compiler-rt（本文件已默认加上）。
# ============================================================================

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_HOST_SYSTEM_PROCESSOR}")

# ---- 1. 定位 NDK 与 sysroot -----------------------------------------------
# ★ 参数一律 env 回填：CMake 的 try_compile 子项目不会继承主项目的 -D 缓存变量，
#   工具链文件会被重新 include；只看环境变量才不会在 ABI 探测时炸掉。
macro(_tc_get _var _env)
  if(NOT ${_var} OR "${${_var}}" STREQUAL "")
    set(${_var} "$ENV{${_env}}")
  endif()
endmacro()

_tc_get(NDK_ROOT NDK_ROOT)
_tc_get(IMG_ANDROID_ABI IMG_ANDROID_ABI)
_tc_get(IMG_ANDROID_API IMG_ANDROID_API)

if(NOT NDK_ROOT)
  # 只认环境变量与 SDK 目录下的 ndk/，不写死任何本机路径
  foreach(_c "$ENV{ANDROID_NDK_HOME}" "$ENV{ANDROID_NDK_ROOT}"
             "$ENV{ANDROID_HOME}/ndk" "$ENV{ANDROID_SDK_ROOT}/ndk")
    if(_c AND IS_DIRECTORY "${_c}")
      if(EXISTS "${_c}/source.properties")
        set(NDK_ROOT "${_c}")
        break()
      else()
        file(GLOB _subs LIST_DIRECTORIES true ${_c}/*)
        list(SORT _subs)
        list(GET _subs -1 _last)
        if(EXISTS "${_last}/source.properties")
          set(NDK_ROOT "${_last}")
          break()
        endif()
      endif()
    endif()
  endforeach()
endif()
if(NOT NDK_ROOT OR NOT IS_DIRECTORY "${NDK_ROOT}")
  message(FATAL_ERROR "NDK_ROOT 未设置或不存在（-DNDK_ROOT=<ndk 目录>，或设环境变量 NDK_ROOT）")
endif()

if(NOT IMG_ANDROID_ABI)
  set(IMG_ANDROID_ABI "arm64-v8a")
endif()
if(NOT IMG_ANDROID_API)
  # 26 而不是 NDK 默认的 21：stdin/stdout(__INTRODUCED_IN(23))、hasmntopt(26)
  # 是 wimlib/ntfs-3g 引用的 bionic 符号，21 的 libc 桩里没有（见 android-cross.sh 注释）
  set(IMG_ANDROID_API "26")
endif()

set(ENV{NDK_ROOT}        "${NDK_ROOT}")
set(ENV{IMG_ANDROID_ABI} "${IMG_ANDROID_ABI}")
set(ENV{IMG_ANDROID_API} "${IMG_ANDROID_API}")

set(_host_tag "linux-x86_64")          # NDK 内固定目录名，与宿主架构无关
set(NDK_TOOLCHAIN "${NDK_ROOT}/toolchains/llvm/prebuilt/${_host_tag}")
set(NDK_SYSROOT   "${NDK_TOOLCHAIN}/sysroot")
# builtins 目录随 NDK 里的 clang 大版本变化（r28 = 19），自动探测
file(GLOB _rt ${NDK_TOOLCHAIN}/lib/clang/*/lib/linux)
list(SORT _rt)
list(GET _rt -1 NDK_RT_LIB)
if(NOT IS_DIRECTORY "${NDK_SYSROOT}")
  message(FATAL_ERROR "NDK sysroot 不存在：${NDK_SYSROOT}")
endif()

# ---- 2. ABI → target triple / flags --------------------------------------
set(_a "${IMG_ANDROID_ABI}")
if(_a STREQUAL "arm64-v8a")
  set(_triple "aarch64-linux-android")
  set(_cpu    "aarch64")
  set(_abi_flags "")
elseif(_a STREQUAL "armeabi-v7a")
  set(_triple "armv7a-linux-androideabi")
  set(_cpu    "armv7-a")
  set(_abi_flags "-march=armv7-a -mfpu=neon -mfloat-abi=softfp")
elseif(_a STREQUAL "x86_64")
  set(_triple "x86_64-linux-android")
  set(_cpu    "x86_64")
  set(_abi_flags "")
elseif(_a STREQUAL "x86")
  set(_triple "i686-linux-android")
  set(_cpu    "i686")
  set(_abi_flags "")
else()
  message(FATAL_ERROR "不支持的 IMG_ANDROID_ABI=${_a}（arm64-v8a|armeabi-v7a|x86_64|x86）")
endif()

set(CMAKE_SYSTEM_PROCESSOR "${_cpu}")
set(CMAKE_ANDROID_ABI      "${_a}")
set(CMAKE_ANDROID_API      "${IMG_ANDROID_API}")
set(CMAKE_ANDROID_SYSROOT  "${NDK_SYSROOT}")

# ---- 3. 编译器：宿主原生 clang，target 里带 API level ----------------------
find_program(_clang NAMES clang REQUIRED)
find_program(_clangxx NAMES clang++ REQUIRED)
find_program(_lld NAMES ld.lld lld REQUIRED)
set(CMAKE_C_COMPILER   "${_clang}")
set(CMAKE_CXX_COMPILER "${_clangxx}")
set(CMAKE_C_COMPILER_TARGET   "${_triple}${IMG_ANDROID_API}")
set(CMAKE_CXX_COMPILER_TARGET "${_triple}${IMG_ANDROID_API}")

# 编译期：sysroot + ABI 标志。安卓从 API 16 起强制 PIC，这里统一加上，
# 否则用本工具链交叉出来的静态库（libxml2 等）塞不进 libimgtool.so。
# -DHAVE_FFS=1：见 android-deps.sh 同处说明（ntfs-3g compat.h 与 Bionic 冲突）
set(_common " --sysroot=${NDK_SYSROOT} -fPIC -DHAVE_FFS=1 ${_abi_flags}")
set(CMAKE_C_FLAGS_INIT   "${_common}")
set(CMAKE_CXX_FLAGS_INIT "${_common} -nostdinc++ -isystem ${NDK_SYSROOT}/usr/include/c++/v1")

# 链接期：-rtlib=compiler-rt 用 NDK 的 builtins（Ubuntu clang 默认去找不存在的 libgcc）；
#         16 KB 页（Android 15+ 要求）+ 可复现 build-id
# -L<stage>/lib：有些目标直接链裸名字（imgwimls 用 `wim`），只给 find_library
# 全路径不够，链接期也得能找到 stage 里的交叉库
set(_ld " -fuse-ld=${_lld} -rtlib=compiler-rt -L${IMG_ANDROID_STAGE}/lib -Wl,-z,max-page-size=16384 -Wl,--build-id=sha1")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_ld}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ld}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_ld}")

# ---- 3.5 让依赖 ANDROID 变量的工程（app/jni）也能识别 ---------------------
set(ANDROID ON CACHE BOOL "cross-building for Android with a host-native clang")
set(ANDROID_ABI "${_a}" CACHE STRING "Android ABI")
set(ANDROID_PLATFORM "android-${IMG_ANDROID_API}" CACHE STRING "minSdk/API")
set(ANDROID_NDK "${NDK_ROOT}" CACHE PATH "NDK root")

# ---- 4. 只在 sysroot 里找依赖（勿混进宿主 glibc 的头/库）------------------
# NDK 把系统库的链接 stub 放在 usr/lib/<triple>/<api>/ 下，不是标准的 lib/ 布局，
# find_library 光靠 FIND_ROOT_PATH 找不到（zlib 就是这么静默掉过功能的）。
set(_apidir "${NDK_SYSROOT}/usr/${_triple}/${IMG_ANDROID_API}")
set(CMAKE_LIBRARY_PATH "${_apidir};${NDK_SYSROOT}/usr/${_triple}")

# 交叉编译来的依赖统一装进 stage/<abi>（标准 include/ + lib/ 布局），
# 这样 find_package / find_library 不用为 NDK 的非标准目录结构特判。
if(NOT IMG_ANDROID_STAGE)
  get_filename_component(_stage_root "${CMAKE_CURRENT_LIST_DIR}/../.." DIRECTORY)
  set(IMG_ANDROID_STAGE "${_stage_root}/stage/android/${_a}")
endif()
set(ENV{IMG_ANDROID_STAGE} "${IMG_ANDROID_STAGE}")
if(EXISTS "${IMG_ANDROID_STAGE}/include")
  set(_common "${_common} -I${IMG_ANDROID_STAGE}/include")
endif()
set(CMAKE_PREFIX_PATH      "${IMG_ANDROID_STAGE}")
set(CMAKE_FIND_ROOT_PATH   "${IMG_ANDROID_STAGE};${NDK_SYSROOT}/usr/${_triple};${NDK_SYSROOT}/usr")

# stage 里的库优先于 sysroot stub（静态 zlib 优先于 libz.so 桩）
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- 5. 别把主机构建目录写进 RPATH ---------------------------------------
# 交叉产物要进 APK，DT_RUNPATH 里出现宿主绝对路径毫无意义。
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH "")
set(CMAKE_BUILD_RPATH "")

message(STATUS "android-arm64host: ABI=${_a} target=${_triple}${IMG_ANDROID_API} clang=${_clang}")
message(STATUS "android-arm64host: sysroot=${NDK_SYSROOT}")
