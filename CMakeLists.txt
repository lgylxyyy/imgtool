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
  src/img_progress.c
  src/img_io.c
  src/img_qcow2.c
  src/img_qcow2_write.c
  src/img_partition.c
  src/img_constraints.c
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
add_library(imgtool STATIC ${IMG_SOURCES})
target_link_libraries(imgtool PUBLIC ${IMG_ZLIB_LIBS})

# ---- 共享库（Android .so）----
add_library(imgtool_shared SHARED ${IMG_SOURCES})
set_target_properties(imgtool_shared PROPERTIES OUTPUT_NAME "imgtool")
target_link_libraries(imgtool_shared PUBLIC ${IMG_ZLIB_LIBS})

# ---- 工具 / 测试 ----
if(IMG_BUILD_TOOLS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt)
  add_subdirectory(tools)
endif()
if(IMG_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "imgtool: C=${CMAKE_C_COMPILER}  build=${CMAKE_BUILD_TYPE}  zlib=${IMG_WITH_ZLIB}")
