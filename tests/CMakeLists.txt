if(NOT DEFINED IMG_WITH_WIM)
  set(IMG_WITH_WIM ON)
endif()
if(NOT DEFINED IMG_WITH_NTFS3G)
  set(IMG_WITH_NTFS3G ON)
endif()

# NTFS 设备后端验证（需要系统 libntfs-3g-dev；缺失时自动跳过）
find_path(NTFS_INC ntfs-3g/volume.h)
find_library(NTFS_LIB ntfs-3g)
if(NTFS_INC AND NTFS_LIB)
  add_executable(ntfs_dev_test ntfs_dev_test.c)
  target_include_directories(ntfs_dev_test PRIVATE ${NTFS_INC})
  target_link_libraries(ntfs_dev_test PRIVATE imgtool ${NTFS_LIB})
  message(STATUS "imgtool: NTFS 设备后端测试已启用 (${NTFS_LIB})")
else()
  message(STATUS "imgtool: 未找到 libntfs-3g dev，跳过 NTFS 设备后端测试")
endif()

# BCD 读写测试（需 libhivex）
find_path(HIVEX_INC hivex.h)
find_library(HIVEX_LIB hivex)
if(HIVEX_INC AND HIVEX_LIB)
  add_executable(bcd_test bcd_test.c ../src/img_bcd.c)
  target_include_directories(bcd_test PRIVATE ${HIVEX_INC})
  target_link_libraries(bcd_test PRIVATE imgtool ${HIVEX_LIB})
  message(STATUS "imgtool: BCD 测试已启用")
else()
  message(STATUS "imgtool: 未找到 hivex，跳过 BCD 测试")
endif()

# 恢复条目构建测试（依赖 libhivex + 官方 ISO BCD 参考文件）
if(HIVEX_INC AND HIVEX_LIB)
  add_executable(recovery_bcd_test recovery_bcd_test.c ../src/img_bcd.c)
  target_include_directories(recovery_bcd_test PRIVATE ${HIVEX_INC})
  target_link_libraries(recovery_bcd_test PRIVATE imgtool ${HIVEX_LIB})

  # MBR/BIOS BCD 设备命门批量改写（boot.repair 依赖；也是 --list 探针）
  add_executable(mbr_bcd_test mbr_bcd_test.c)
  target_include_directories(mbr_bcd_test PRIVATE ${HIVEX_INC})
  target_link_libraries(mbr_bcd_test PRIVATE imgtool ${HIVEX_LIB})
endif()

# ---- 健壮性验证 ----
add_executable(fd_test fd_test.c)
target_link_libraries(fd_test PRIVATE imgtool)

add_executable(thread_test thread_test.c)
target_link_libraries(thread_test PRIVATE imgtool)

add_executable(cancel_test cancel_test.c)
target_link_libraries(cancel_test PRIVATE imgtool)

if(IMG_WITH_WIM)
  add_executable(wim_apply_test wim_apply_test.c)
endif()

target_link_libraries(wim_apply_test PRIVATE imgtool)

add_executable(fatput fatput.c)
target_link_libraries(fatput PRIVATE imgtool)

if(IMG_WITH_NTFS3G)
  add_executable(ntfs_probe ntfs_probe.c)
endif()

target_link_libraries(ntfs_probe PRIVATE imgtool)
target_include_directories(ntfs_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)


add_executable(timeout_test timeout_test.c)
target_link_libraries(timeout_test PRIVATE imgtool)

add_executable(ebr_test ebr_test.c)
target_link_libraries(ebr_test PRIVATE imgtool)

# 版本识别"多源投票"（纯逻辑，不需要真盘）
if(HIVEX_INC AND HIVEX_LIB)
  add_executable(bcdcreate_test bcdcreate_test.c)
  target_include_directories(bcdcreate_test PRIVATE ${HIVEX_INC})
  target_link_libraries(bcdcreate_test PRIVATE imgtool ${HIVEX_LIB})
endif()

add_executable(identify_test identify_test.c)
target_link_libraries(identify_test PRIVATE imgtool)
