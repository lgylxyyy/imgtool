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

# 警告即错误（保证移植质量）
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(IMG_SOURCES
  src/img_types.c
  src/img_text.c      # 库内人话的中英双份（img_t / img_lang_set）
  src/img_event.c
  src/img_cmd.c
  src/img_clone.c       # 扇区级克隆（机制；政策在 App/命令层）
  src/img_cmds.c
  src/img_lock.c
  src/img_host.c
  src/img_progress.c
  src/img_io.c
  src/img_qcow2.c
  src/img_qcow2_write.c
  src/img_partition.c
  src/img_constraints.c
  src/img_fat32.c
  src/img_ntfs_dev.c
  src/img_ntfs_ops.c     # NTFS 通用管道（ctx/回调/probe；与 mkntfs 无关）
  src/ntfs_bridge.c      # 通用桥（fs/bootcheck 共用；NTFS3G 关时随 REMOVE 摘掉）
  src/img_fixboot.c
  src/img_ntfs_fs.c
  src/img_fat32_fs.c
  src/img_wim_apply.c
  src/img_iso.c          # ISO/UDF 只读（自研解析；抽 install.wim）
  src/img_recovery.c     # 恢复分区/WinRE（查/填/设属性）
  src/img_identify.c     # 从内容读 Windows 版本（WIM XML / SOFTWARE hive）
  src/img_bootplan.c     # 分区表/固件与 Windows 版本的兼容判定（boot.plan）
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
       src/img_ntfs_fmt.c src/img_bootcheck.c)
  if(IMG_WITH_WIM)
    message(WARNING "imgtool: NTFS 关闭 → WIM 直灌一并关闭（img_wim_apply 依赖 NTFS 设备层）")
    set(IMG_WITH_WIM OFF)
  endif()
endif()
if(IMG_WITH_WIM)
  find_library(WIM_LIB wim)
  if(WIM_LIB)
    add_compile_definitions(IMG_WITH_WIM=1)
  else()
    message(WARNING "imgtool: libwim 未找到 → 关闭 WIM/ESD 功能（wim.* 命令报 FEATURE-UNAVAILABLE）")
    set(IMG_WITH_WIM OFF)
  endif()
endif()
if(NOT IMG_WITH_WIM)
  add_compile_definitions(IMG_WITH_WIM=0)
  list(REMOVE_ITEM IMG_SOURCES src/img_wim_apply.c)
endif()

# ---- libhivex（BCD hive 读写；imggenbcd 需要；缺了就只跳过该工具）----
find_path(HIVEX_INC hivex.h)
find_library(HIVEX_LIB hivex)

find_package(Threads REQUIRED)
add_library(imgtool STATIC ${IMG_SOURCES})
if(HIVEX_LIB)
  target_sources(imgtool PRIVATE src/img_bcd.c)
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
if(IMG_WITH_WIM AND WIM_LIB)
  target_link_libraries(imgtool PUBLIC ${WIM_LIB})
endif()

# ---- 共享库（Android .so）----
add_library(imgtool_shared SHARED ${IMG_SOURCES})
set_target_properties(imgtool_shared PROPERTIES OUTPUT_NAME "imgtool")
if(HIVEX_LIB)
  # ★ 静态库有 img_bcd.c，共享库曾漏掉它 → 任何链接 .so 的消费者
  #   调 boot.repair/imggenbcd 都会 "undefined symbol: img_bcd_*"。
  target_sources(imgtool_shared PRIVATE src/img_bcd.c)
  target_include_directories(imgtool_shared PRIVATE ${HIVEX_INC})
  target_link_libraries(imgtool_shared PUBLIC ${HIVEX_LIB})
endif()
if(IMG_WITH_NTFS3G)
  target_include_directories(imgtool_shared PRIVATE ${NTFS3G_INC})
  target_link_libraries(imgtool_shared PUBLIC ${NTFS3G_LIB})
  target_sources(imgtool_shared PRIVATE src/img_bootcheck.c)
endif()
target_link_libraries(imgtool_shared PUBLIC ${IMG_ZLIB_LIBS} Threads::Threads)
if(IMG_WITH_WIM AND WIM_LIB)
  target_link_libraries(imgtool_shared PUBLIC ${WIM_LIB})
endif()

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
  # mkntfs 对象：由 third_party_ntfs3g.md 描述的构建流程产出
  file(GLOB MKNTFS_OBJS "${CMAKE_CURRENT_SOURCE_DIR}/../build/mkntfs_obj/mkntfs-*.o")
  if(NTFSDEV_INC AND NTFSDEV_LIB AND MKNTFS_OBJS)
    target_sources(imgtool       PRIVATE src/img_ntfs_fmt.c ${MKNTFS_OBJS})
    target_include_directories(imgtool       PRIVATE ${NTFSDEV_INC})
    target_link_libraries(imgtool       PUBLIC ${NTFSDEV_LIB})
    target_sources(imgtool       PRIVATE src/img_ntfs_fmt.c ${MKNTFS_OBJS})
    target_sources(imgtool_shared PRIVATE src/img_ntfs_fmt.c ${MKNTFS_OBJS})
    target_include_directories(imgtool_shared PRIVATE ${NTFSDEV_INC})
    target_link_libraries(imgtool_shared PUBLIC ${NTFSDEV_LIB})
    add_compile_definitions(IMG_WITH_NTFS=1)
    message(STATUS "imgtool: NTFS 格式化模块已启用 (GPL-2.0+)")
  else()
    message(WARNING "imgtool: IMG_WITH_NTFS=ON 但缺依赖，已跳过"
                    " (需要 libntfs-3g-dev 与 ../build/mkntfs_obj/*.o)")
  endif()
endif()
