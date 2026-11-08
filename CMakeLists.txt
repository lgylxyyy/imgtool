cmake_minimum_required(VERSION 3.16)
project(imgtool C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(IMG_BUILD_TOOLS "Build CLI tools" ON)
option(IMG_BUILD_TESTS "Build unit tests" ON)
option(IMG_WITH_ZLIB   "Enable qcow2 compression (zlib)" ON)
# ═══ 功能开关 + "库缺失降级"（硬缺口 #3）═══
#   之前：缺库只发 warning，照样编译链接 → 直接构建失败（那不是降级）。
#   现在：缺库 = 自动关掉对应功能；命令仍在表里，但明确报 FEATURE-UNAVAILABLE。
option(IMG_WITH_WIM    "WIM/ESD 支持（需要系统 libwim）" ON)
option(IMG_WITH_NTFS3G "NTFS 读写（需要系统 libntfs-3g；mkntfs 格式化另见 IMG_WITH_NTFS）" ON)
# REQ-50：RAR 读取（solid/RAR5）。解码器来自 third_party/unrar（UnRAR license，
#   可自由分发，但不能用来做 RAR 压缩器 —— 我们只读）。关掉后 rar 退回 libarchive，
#   solid 包会报 ARCHIVE-OPEN-FAIL。
option(IMG_WITH_RAR    "RAR 读取（内置 unrar 解码器；REQ-50）" ON)


# ---- RAR（REQ-50）：内置 unrar 解码器 ----
if(IMG_WITH_RAR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unrar/archive.cpp)
  enable_language(CXX)
  set(UNRAR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unrar)
  set(UNRAR_SRCS
    ${UNRAR_DIR}/rar.cpp ${UNRAR_DIR}/strlist.cpp ${UNRAR_DIR}/strfn.cpp ${UNRAR_DIR}/pathfn.cpp
    ${UNRAR_DIR}/smallfn.cpp ${UNRAR_DIR}/global.cpp ${UNRAR_DIR}/file.cpp ${UNRAR_DIR}/filefn.cpp
    ${UNRAR_DIR}/filcreat.cpp ${UNRAR_DIR}/archive.cpp ${UNRAR_DIR}/arcread.cpp
    ${UNRAR_DIR}/unicode.cpp ${UNRAR_DIR}/system.cpp ${UNRAR_DIR}/crypt.cpp ${UNRAR_DIR}/crc.cpp
    ${UNRAR_DIR}/rawread.cpp ${UNRAR_DIR}/encname.cpp ${UNRAR_DIR}/resource.cpp
    ${UNRAR_DIR}/match.cpp ${UNRAR_DIR}/timefn.cpp ${UNRAR_DIR}/rdwrfn.cpp ${UNRAR_DIR}/consio.cpp
    ${UNRAR_DIR}/options.cpp ${UNRAR_DIR}/errhnd.cpp ${UNRAR_DIR}/rarvm.cpp
    ${UNRAR_DIR}/secpassword.cpp ${UNRAR_DIR}/rijndael.cpp ${UNRAR_DIR}/getbits.cpp
    ${UNRAR_DIR}/sha1.cpp ${UNRAR_DIR}/sha256.cpp ${UNRAR_DIR}/blake2s.cpp ${UNRAR_DIR}/hash.cpp
    ${UNRAR_DIR}/extinfo.cpp ${UNRAR_DIR}/extract.cpp ${UNRAR_DIR}/volume.cpp ${UNRAR_DIR}/list.cpp
    ${UNRAR_DIR}/find.cpp ${UNRAR_DIR}/unpack.cpp ${UNRAR_DIR}/headers.cpp
    ${UNRAR_DIR}/threadpool.cpp ${UNRAR_DIR}/rs16.cpp ${UNRAR_DIR}/cmddata.cpp ${UNRAR_DIR}/ui.cpp
    ${UNRAR_DIR}/largepage.cpp ${UNRAR_DIR}/filestr.cpp ${UNRAR_DIR}/scantree.cpp
    ${UNRAR_DIR}/dll.cpp ${UNRAR_DIR}/qopen.cpp)
  add_library(img_unrar STATIC ${UNRAR_SRCS})
  target_include_directories(img_unrar PUBLIC ${UNRAR_DIR})
  target_compile_definitions(img_unrar PRIVATE RARDLL RAR_SMP _UNIX _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE)
  # 第三方代码：关警告、隐藏符号（别让 .so 多出几千个 C++ 符号）
  target_compile_options(img_unrar PRIVATE -w -fvisibility=hidden -fno-strict-aliasing)
  set_target_properties(img_unrar PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON
                        POSITION_INDEPENDENT_CODE ON)
  add_compile_definitions(IMG_WITH_RAR=1)
  # ★ Android：C 可执行文件链 C++ 静态库时不会自动带 libc++/libunwind
  #   （实测 ld.lld: undefined symbol: _Unwind_Resume）。这里给后续所有目标补上。
  if(ANDROID)
    add_link_options(-stdlib=libc++ -static-libstdc++)
    # libc++abi 还要 libunwind，而它在 clang 的 resource dir 里（不在 sysroot）：
    #   $NDK/toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux/<arch>/libunwind.a
    set(_unwind_arch "aarch64")
    if(ANDROID_ABI STREQUAL "armeabi-v7a")
      set(_unwind_arch "arm")
    elseif(ANDROID_ABI STREQUAL "x86")
      set(_unwind_arch "i686")
    elseif(ANDROID_ABI STREQUAL "x86_64")
      set(_unwind_arch "x86_64")
    endif()
    # 从编译器路径推 prebuilt 根（CMAKE_ANDROID_NDK 在部分工具链文件里没定义）
    get_filename_component(_clang_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_llvm_pre "${_clang_bin}" DIRECTORY)
    file(GLOB _unwind_libs
         "${_llvm_pre}/lib/clang/*/lib/linux/${_unwind_arch}/libunwind.a"
         "${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux/${_unwind_arch}/libunwind.a")
    if(NOT _unwind_libs AND NDK_ROOT)
      file(GLOB _unwind_libs "${NDK_ROOT}/toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux/${_unwind_arch}/libunwind.a")
    endif()
    if(NOT _unwind_libs AND CMAKE_TOOLCHAIN_FILE)
      # NDK 根 = <ndk>/build/cmake/android.toolchain.cmake 往上三层
      get_filename_component(_ndk "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
      get_filename_component(_ndk "${_ndk}" DIRECTORY)
      get_filename_component(_ndk "${_ndk}" DIRECTORY)
      file(GLOB _unwind_libs "${_ndk}/toolchains/llvm/prebuilt/*/lib/clang/*/lib/linux/${_unwind_arch}/libunwind.a")
    endif()
    if(_unwind_libs)
      list(GET _unwind_libs 0 _unwind_lib)
      get_filename_component(_unwind_dir "${_unwind_lib}" DIRECTORY)
      link_directories("${_unwind_dir}")
      set(IMG_UNWIND_LIB "${_unwind_lib}")
      message(STATUS "imgtool: unrar 链接 ${_unwind_lib}")
    else()
      message(WARNING "imgtool: 没找到 libunwind.a（RAR 在 Android 上会链接失败）")
    endif()
  endif()
  message(STATUS "imgtool: RAR 读取已启用（third_party/unrar）")
else()
  add_compile_definitions(IMG_WITH_RAR=0)
  message(STATUS "imgtool: RAR 读取关闭（没有 third_party/unrar）→ solid RAR 读不了")
endif()

# 警告即错误（保证移植质量）
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(IMG_SOURCES
  src/img_types.c
  src/img_text.c      # 库内说明的中英双份（img_t / img_lang_set）
  src/img_event.c
  src/img_cmd.c
  src/img_clone.c       # 扇区级克隆（机制；政策在 App/命令层）
  src/img_cmds.c
  src/img_lock.c
  src/img_host.c
  src/img_progress.c
  src/img_tbuf.c       # 线程级缓冲（堆上，替代 1.5MB 静态 TLS，REQ-21）
  src/img_io.c
  src/img_qcow2.c
  src/img_qcow2_write.c
  src/img_partition.c
  src/img_ptfile.c      # 分区表文件(.json) 导出/读入（REQ-14）
  src/img_constraints.c
  src/img_fat32.c
  src/img_ntfs_dev.c
  src/img_ntfs_ops.c     # NTFS 通用管道（ctx/回调/probe；与 mkntfs 无关）
  src/ntfs_bridge.c      # 通用桥（fs/bootcheck 共用；NTFS3G 关时随 REMOVE 摘掉）
  src/img_fixboot.c
  src/img_ntfs_fs.c
  src/img_fat32_fs.c
  src/img_xcopy.c      # 跨分区/跨镜像/宿主↔镜像/ISO/WIM → 镜像 的统一复制引擎
  src/img_path.c       # 统一容器路径（! 语法：qcow2!p3:/x、iso!:/a!1:/b）
  src/img_name.c       # Windows 文件名合法性（NTFS/FAT 写前校验：非法字符/保留名/尾点）
  src/img_wim_apply.c
  src/img_wim_fs.c        # WIM/ESD 内部只读浏览 + 抽单文件
  src/img_ntfs_fmt.c     # 常驻：模块缺席时返回 IMG_ENOTSUP，不让链接炸
  src/img_iso.c          # ISO/UDF 只读（自研解析；抽 install.wim）
  src/img_vfd.c          # 虚拟 fd：让 wimlib 从 ISO 里【直接读】，不落临时文件（REQ-16）
  src/img_recovery.c     # 恢复分区/WinRE（查/填/设属性）
  src/img_winre.c        # WinRE 状态检测（只读；与引导修复解耦，recovery.status）
  src/img_identify.c     # 从内容读 Windows 版本（WIM XML / SOFTWARE hive）
  src/img_bootplan.c     # 分区表/固件与 Windows 版本的兼容判定（boot.plan）
  src/img_archive.c      # 压缩包只读（libarchive；REQ-31）
  src/img_archive_w.c    # 压缩包写端（zip/7z/tar/iso；REQ-39/41）
  src/img_du.c           # 递归大小/项数（REQ-40）
  src/img_rar.c          # RAR 只读（内置 unrar 解码器；REQ-50）
  src/img_7z_solid.c     # 7z 固实标志（自解 7z 头；REQ-51 A）
)

# ---- zlib（qcow2 压缩簇必需；安卓 NDK 自带或可交叉编译）----
set(IMG_ZLIB_LIBS "")
if(IMG_WITH_ZLIB)
  add_compile_definitions(IMG_WITH_ZLIB=1)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    set(IMG_ZLIB_LIBS ZLIB::ZLIB)
    add_compile_definitions(IMG_HAVE_ZLIB=1)
  else()
    message(WARNING "zlib 未找到 → qcow2 压缩簇功能关闭（只读仍可用，写会报 ENOTSUP）")
  endif()
endif()

# ---- libarchive（只读压缩包：zip/7z/rar/tar.*/cab；REQ-31）----
option(IMG_WITH_ARCHIVE "只读压缩包（libarchive）" ON)
set(IMG_ARCHIVE_LIBS "")
if(IMG_WITH_ARCHIVE)
  find_path(ARCHIVE_INC archive.h HINTS ${IMG_ARCHIVE_ROOT} $ENV{IMG_ARCHIVE_ROOT} PATH_SUFFIXES include)
  find_library(ARCHIVE_LIB archive HINTS ${IMG_ARCHIVE_ROOT} $ENV{IMG_ARCHIVE_ROOT} PATH_SUFFIXES lib)
  if(ARCHIVE_INC AND ARCHIVE_LIB)
    add_compile_definitions(IMG_WITH_ARCHIVE=1)
    set(IMG_ARCHIVE_LIBS ${ARCHIVE_LIB})
    # 静态 libarchive 要带上它的解码器（7z 靠 liblzma，zip/tar.gz 靠 zlib）
    find_library(LZMA_LIB lzma HINTS ${IMG_ARCHIVE_ROOT} $ENV{IMG_ARCHIVE_ROOT} PATH_SUFFIXES lib)
    if(LZMA_LIB)
      list(APPEND IMG_ARCHIVE_LIBS ${LZMA_LIB})
    endif()
    # ★ 静态库的链接顺序：libarchive 用了 zlib/lzma 的符号，它们必须排在 libarchive 之后
    # zlib 必须排在 libarchive **之后**（静态库按序解析符号）；这里显式补一次
    list(APPEND IMG_ARCHIVE_LIBS z)
    message(STATUS "imgtool: libarchive 启用（只读压缩包）")
  else()
    message(STATUS "imgtool: libarchive 未找到 → archive.* 命令报 FEATURE-UNAVAILABLE")
    set(IMG_WITH_ARCHIVE OFF)
  endif()
endif()
if(NOT IMG_WITH_ARCHIVE)
  add_compile_definitions(IMG_WITH_ARCHIVE=0)
endif()

# ---- 核心静态库 ----
# ---- 库缺失降级判定（wim / ntfs-3g）----
if(IMG_WITH_NTFS3G)
  find_path(NTFS3G_INC ntfs-3g/volume.h)   # Linux 侧主头（ntfsvolume.h 是 Windows 侧的）
  find_library(NTFS3G_LIB ntfs-3g)
  if(NTFS3G_INC AND NTFS3G_LIB)
    add_compile_definitions(IMG_WITH_NTFS3G=1)
  else()
    message(WARNING "imgtool: libntfs-3g 未找到 → 关闭 NTFS 功能"
                    "（ntfs.* / winre.auto / wim.* 命令将报 FEATURE-UNAVAILABLE）")
    set(IMG_WITH_NTFS3G OFF)
  endif()
endif()
if(NOT IMG_WITH_NTFS3G)
  add_compile_definitions(IMG_WITH_NTFS3G=0)
  list(REMOVE_ITEM IMG_SOURCES
       src/img_ntfs_fs.c src/img_ntfs_dev.c src/img_ntfs_ops.c src/ntfs_bridge.c
       src/img_bootcheck.c)
  # ★ 占位实现：命令层与 img_recovery/img_identify 里"顺手做 NTFS"的那几支仍在，
  #   没有符号就谁也别想构建（工程债 #13）。占位函数一律 ENOTSUP，命令入口另有闸。
  list(APPEND IMG_SOURCES src/img_ntfs_stub.c)
  if(IMG_WITH_WIM)
    message(WARNING "imgtool: NTFS 关闭 → WIM 直灌一并关闭（img_wim_apply 依赖 NTFS 设备层）")
    set(IMG_WITH_WIM OFF)
  endif()
endif()
option(IMG_WIM_STATIC "静态链接 wimlib（ISO 直灌的前提；共享库版会退回抽取）" ON)
if(IMG_WITH_WIM)
  find_library(WIM_LIB wim)
  if(WIM_LIB)
    add_compile_definitions(IMG_WITH_WIM=1)
    # ★ 2026-10-26（REQ-16）：ISO 直灌靠 img_vfd.c 把 libc 的 fd 操作在本 .so 内改道。
    #   共享库版 wimlib 调的是系统 libc 的 read/pread，拦不住 → 只能静态。
    get_filename_component(_wimdir "${WIM_LIB}" DIRECTORY)
    find_file(WIM_STATIC NAMES libwim.a PATHS "${_wimdir}" NO_DEFAULT_PATH)
    if(IMG_WIM_STATIC AND WIM_STATIC)
      set(WIM_LIB "${WIM_STATIC}")
      add_compile_definitions(IMG_WIM_VFD=1)
      message(STATUS "imgtool: wimlib 静态链接 → ISO 直灌可用（不落临时文件）")
    else()
      add_compile_definitions(IMG_WIM_VFD=0)
      message(STATUS "imgtool: wimlib 是共享库 → ISO 直灌关闭（wim.apply 会先抽 install.wim）")
    endif()
  else()
    message(WARNING "imgtool: libwim 未找到 → 关闭 WIM/ESD 功能（wim.* 命令报 FEATURE-UNAVAILABLE）")
    set(IMG_WITH_WIM OFF)
  endif()
endif()
if(NOT IMG_WITH_WIM)
  add_compile_definitions(IMG_WITH_WIM=0 IMG_WIM_VFD=0)
  list(REMOVE_ITEM IMG_SOURCES src/img_wim_apply.c src/img_wim_fs.c)
endif()

# ---- libhivex（BCD hive 读写；imggenbcd 需要；缺了就只跳过该工具）----
find_path(HIVEX_INC hivex.h)
find_library(HIVEX_LIB hivex)

find_package(Threads REQUIRED)
add_library(imgtool STATIC ${IMG_SOURCES})
if(HIVEX_LIB)
  target_sources(imgtool PRIVATE src/img_bcd.c src/img_win_finish.c src/img_reg.c)
  target_include_directories(imgtool PRIVATE ${HIVEX_INC})
  target_link_libraries(imgtool PUBLIC ${HIVEX_LIB})
endif()
if(IMG_WITH_NTFS3G)
  target_include_directories(imgtool PRIVATE ${NTFS3G_INC})
  target_link_libraries(imgtool PUBLIC ${NTFS3G_LIB})
  # 17 项启动自检只需 libntfs-3g（此前被错绑在 mkntfs 开关下 → 默认构建里 imgbc 链接失败）
  target_sources(imgtool       PRIVATE src/img_bootcheck.c)
endif()
target_link_libraries(imgtool PUBLIC ${IMG_ZLIB_LIBS} Threads::Threads)
if(TARGET img_unrar)
  target_link_libraries(imgtool PUBLIC img_unrar)
  if(IMG_UNWIND_LIB)
    target_link_libraries(imgtool PUBLIC ${IMG_UNWIND_LIB})   # libc++abi 要它
  endif()
endif()
if(IMG_WITH_ARCHIVE AND ARCHIVE_LIB)
  target_include_directories(imgtool PRIVATE ${ARCHIVE_INC})
  target_link_libraries(imgtool PUBLIC ${IMG_ARCHIVE_LIBS})
endif()
if(IMG_WITH_WIM AND WIM_LIB)
  target_link_libraries(imgtool PUBLIC ${WIM_LIB})
endif()

# ---- 共享库（Android .so）----
add_library(imgtool_shared SHARED ${IMG_SOURCES})
set_target_properties(imgtool_shared PROPERTIES OUTPUT_NAME "imgtool")
if(HIVEX_LIB)
  # ★ 静态库有 img_bcd.c，共享库曾漏掉它 → 任何链接 .so 的消费者
  #   调 boot.repair/imggenbcd 都会 "undefined symbol: img_bcd_*"。
  target_sources(imgtool_shared PRIVATE src/img_bcd.c src/img_win_finish.c src/img_reg.c)
  target_include_directories(imgtool_shared PRIVATE ${HIVEX_INC})
  target_link_libraries(imgtool_shared PUBLIC ${HIVEX_LIB})
endif()
if(IMG_WITH_NTFS3G)
  target_include_directories(imgtool_shared PRIVATE ${NTFS3G_INC})
  target_link_libraries(imgtool_shared PUBLIC ${NTFS3G_LIB})
  target_sources(imgtool_shared PRIVATE src/img_bootcheck.c)
endif()
target_link_libraries(imgtool_shared PUBLIC ${IMG_ZLIB_LIBS} Threads::Threads)
if(TARGET img_unrar)
  target_link_libraries(imgtool_shared PRIVATE img_unrar)
  if(IMG_UNWIND_LIB)
    target_link_libraries(imgtool_shared PRIVATE ${IMG_UNWIND_LIB})
  endif()
  # 第三方 C++ 的符号不外泄（否则 .so 从 1286 个导出涨到 1566）
  if(NOT APPLE)
    target_link_options(imgtool_shared PRIVATE
      "-Wl,--exclude-libs,libimg_unrar.a"
      "-Wl,--exclude-libs,libc++_static.a"
      "-Wl,--exclude-libs,libc++abi.a"
      "-Wl,--exclude-libs,libunwind.a")
  endif()
endif()
if(IMG_WITH_WIM AND WIM_LIB)
  target_link_libraries(imgtool_shared PUBLIC ${WIM_LIB})
endif()
if(IMG_WITH_ARCHIVE AND ARCHIVE_LIB)
  target_include_directories(imgtool_shared PRIVATE ${ARCHIVE_INC})
  target_link_libraries(imgtool_shared PUBLIC ${IMG_ARCHIVE_LIBS})
endif()

# ---- 虚拟 fd 的符号改道（REQ-16：img_vfd.c 的 __wrap_* 需要配对的 --wrap=）----
#   ★ 无论 VFD 开没开都要给：img_vfd.c 引用了 __real_*，不给就是未定义符号。
#   ★ 不包装的 fd 一律原样转 __real_*，行为零变化（只是每此 IO 多一次函数调用）。
set(_VFD_SYMS open open64 read pread pread64 lseek lseek64 fstat fstat64 close realpath)
if(NOT ANDROID)
  # glibc 的 fortify 版本（bionic 没有这两个符号，硬给会链接失败）
  list(APPEND _VFD_SYMS __open_2 __realpath_chk)
endif()
set(_VFD_FLAGS "")
foreach(_s ${_VFD_SYMS})
  list(APPEND _VFD_FLAGS "-Wl,--wrap=${_s}")
endforeach()
target_link_options(imgtool        INTERFACE ${_VFD_FLAGS})
# ★ REQ-27（P0）：.so 这一行必须 PRIVATE。
#   INTERFACE 只把选项【传给使用方】，编 libimgtool.so 自己时根本不生效 ——
#   于是 img_vfd.c 的 __wrap_* 编进去了却没人调用，VFD 假路径 /imgtool-vfd/N 在 .so 里
#   没人认：wimlib 一 open 就失败（App 里 1~16 ms 就报"没有 WinRE/打不开 WIM"）。
#   imgcmd 是可执行文件（INTERFACE 从静态库 imgtool 传过去），所以 CLI 一直正常 —— 和
#   REQ-18（__real_* 未定义）是同一个设计在两类产物上的差异。
target_link_options(imgtool_shared PRIVATE ${_VFD_FLAGS})

# ---- 工具 / 测试 ----
if(IMG_BUILD_TOOLS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt)
  add_subdirectory(tools)
endif()
if(IMG_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "imgtool: C=${CMAKE_C_COMPILER}  build=${CMAKE_BUILD_TYPE}  zlib=${IMG_WITH_ZLIB}")

# ═══════════════════════════════════════════════════════════════
# NTFS 格式化模块（可选：-DIMG_WITH_NTFS=ON/OFF，默认 OFF）
#   依赖系统 libntfs-3g-dev；启用后 imgtool 内含 GPL-2.0+ 代码
#   （mkntfs 对象，经 ELF 符号前插接入，上游源码零改动）
# ═══════════════════════════════════════════════════════════════
option(IMG_WITH_NTFS "Enable in-process NTFS formatting (pulls in GPL mkntfs objects)" OFF)
if(IMG_WITH_NTFS AND IMG_WITH_NTFS3G)
  find_path(NTFSDEV_INC ntfs-3g/device.h)
  find_library(NTFSDEV_LIB ntfs-3g)
  # mkntfs 对象：由 tools/build-mkntfs-obj.sh 产出（构建输入，不是产物）
  # 默认放在工作区的 build/mkntfs_obj；换目录用 -DIMG_MKNTFS_OBJ_DIR=...
  set(IMG_MKNTFS_OBJ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build/mkntfs_obj"
      CACHE PATH "mkntfs 目标文件目录（构建输入，见 third_party_ntfs3g.md）")
  file(GLOB MKNTFS_OBJS "${IMG_MKNTFS_OBJ_DIR}/mkntfs-*.o")
  if(NTFSDEV_INC AND NTFSDEV_LIB AND MKNTFS_OBJS)
    # img_ntfs_fmt.c 已在 IMG_SOURCES 里常驻，这里只补 GPL 对象
    target_sources(imgtool       PRIVATE ${MKNTFS_OBJS})
    target_include_directories(imgtool       PRIVATE ${NTFSDEV_INC})
    target_link_libraries(imgtool       PUBLIC ${NTFSDEV_LIB})
    target_sources(imgtool_shared PRIVATE ${MKNTFS_OBJS})
    target_include_directories(imgtool_shared PRIVATE ${NTFSDEV_INC})
    target_link_libraries(imgtool_shared PUBLIC ${NTFSDEV_LIB})
    add_compile_definitions(IMG_WITH_NTFS=1)
    message(STATUS "imgtool: NTFS 格式化模块已启用 (GPL-2.0+)")
  else()
    message(WARNING "imgtool: IMG_WITH_NTFS=ON 但缺依赖，已跳过"
                    " (需要 libntfs-3g-dev 与 ${IMG_MKNTFS_OBJ_DIR}/*.o)")
    add_compile_definitions(IMG_WITH_NTFS=0)
  endif()
endif()
