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

# 警告即错误（保证移植质量）
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

set(IMG_SOURCES
  src/img_types.c
  src/img_event.c
  src/img_cmd.c
  src/img_cmds.c
  src/img_lock.c
  src/img_progress.c
  src/img_io.c
  src/img_qcow2.c
  src/img_qcow2_write.c
  src/img_partition.c
  src/img_constraints.c
  src/img_fat32.c
  src/img_ntfs_dev.c
  src/img_fixboot.c
  src/ntfs_bridge.c
  src/img_ntfs_fs.c
  src/img_fat32_fs.c
  src/img_wim_apply.c
)

# ---- zlib（qcow2 压缩簇必需；安卓 NDK 自带或可交叉编译）----
set(IMG_ZLIB_LIBS "")
if(IMG_WITH_ZLIB)
  find_package(ZLIB QUIET)
  if(ZLIB_FOUND)
    set(IMG_ZLIB_LIBS ZLIB::ZLIB)
    add_compile_definitions(IMG_HAVE_ZLIB=1)
  else()
    message(WARNING "zlib 未找到 → qcow2 压缩簇功能关闭（只读仍可用，写会报 ENOTSUP）")
  endif()
endif()

# ---- 核心静态库 ----
# ---- wimlib（WIM/ESD 解压；img_wim_apply.c 依赖它，所以要作为库的真实依赖声明）----
find_library(WIM_LIB wim)
if(NOT WIM_LIB)
  message(WARNING "libwim 未找到 → WIM/ESD 相关功能（img_wim_apply / wim.size 命令）不可用")
endif()

find_package(Threads REQUIRED)
add_library(imgtool STATIC ${IMG_SOURCES})
target_link_libraries(imgtool PUBLIC ${IMG_ZLIB_LIBS} Threads::Threads ${WIM_LIB})

# ---- 共享库（Android .so）----
add_library(imgtool_shared SHARED ${IMG_SOURCES})
set_target_properties(imgtool_shared PROPERTIES OUTPUT_NAME "imgtool")
target_link_libraries(imgtool_shared PUBLIC ${IMG_ZLIB_LIBS} Threads::Threads ${WIM_LIB})

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
if(IMG_WITH_NTFS)
  find_path(NTFSDEV_INC ntfs-3g/device.h)
  find_library(NTFSDEV_LIB ntfs-3g)
  # mkntfs 对象：由 third_party_ntfs3g.md 描述的构建流程产出
  file(GLOB MKNTFS_OBJS "${CMAKE_CURRENT_SOURCE_DIR}/../build/mkntfs_obj/mkntfs-*.o")
  if(NTFSDEV_INC AND NTFSDEV_LIB AND MKNTFS_OBJS)
    target_sources(imgtool       PRIVATE src/img_ntfs_fmt.c ${MKNTFS_OBJS})
    target_include_directories(imgtool       PRIVATE ${NTFSDEV_INC})
    target_link_libraries(imgtool       PUBLIC ${NTFSDEV_LIB})
    target_sources(imgtool       PRIVATE src/img_ntfs_fmt.c src/img_bootcheck.c ${MKNTFS_OBJS})
    target_sources(imgtool_shared PRIVATE src/img_ntfs_fmt.c src/img_bootcheck.c ${MKNTFS_OBJS})
    target_include_directories(imgtool_shared PRIVATE ${NTFSDEV_INC})
    target_link_libraries(imgtool_shared PUBLIC ${NTFSDEV_LIB})
    add_compile_definitions(IMG_WITH_NTFS=1)
    message(STATUS "imgtool: NTFS 格式化模块已启用 (GPL-2.0+)")
  else()
    message(WARNING "imgtool: IMG_WITH_NTFS=ON 但缺依赖，已跳过"
                    " (需要 libntfs-3g-dev 与 ../build/mkntfs_obj/*.o)")
  endif()
endif()
