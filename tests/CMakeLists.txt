if(NOT DEFINED IMG_WITH_WIM)
  set(IMG_WITH_WIM ON)
endif()
if(NOT DEFINED IMG_WITH_NTFS3G)
  set(IMG_WITH_NTFS3G ON)
endif()

# NTFS 设备后端验证（需要系统 libntfs-3g-dev；缺失时自动跳过）
# ★ 2026-11-09：还要看 IMG_WITH_NTFS3G 开关 —— 以前只查库在不在，
#   于是 -DIMG_WITH_NTFS3G=OFF 时仍去编这个测试 → 链接失败（整棵构建都挂）。
find_path(NTFS_INC ntfs-3g/volume.h)
find_library(NTFS_LIB ntfs-3g)
if(IMG_WITH_NTFS3G AND NTFS_INC AND NTFS_LIB)
  add_executable(ntfs_dev_test ntfs_dev_test.c)
  target_include_directories(ntfs_dev_test PRIVATE ${NTFS_INC})
  target_link_libraries(ntfs_dev_test PRIVATE imgtool ${NTFS_LIB})
  message(STATUS "imgtool: NTFS 设备后端测试已启用 (${NTFS_LIB})")
else()
  message(STATUS "imgtool: 未找到 libntfs-3g dev，跳过 NTFS 设备后端测试")
endif()

# wimlib 错误码映射（REQ-48 §1）
if(IMG_WITH_WIM)
  add_executable(wimcodes_test wimcodes_test.c)
  target_link_libraries(wimcodes_test PRIVATE imgtool)
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

# 进度/ETA 口径（REQ-67：阶段清零、字节优先、ETA 同单位）
add_executable(progress_test progress_test.c)
target_link_libraries(progress_test PRIVATE imgtool)

if(IMG_WITH_WIM)
  add_executable(wim_apply_test wim_apply_test.c)
  target_link_libraries(wim_apply_test PRIVATE imgtool)   # 原来写在外层，WIM=OFF 时链接不存在的目标
endif()

add_executable(fatput fatput.c)
target_link_libraries(fatput PRIVATE imgtool)

if(IMG_WITH_NTFS3G)
  add_executable(ntfs_probe ntfs_probe.c)
  target_link_libraries(ntfs_probe PRIVATE imgtool)          # 同 wim_apply_test：原来写在外层
  target_include_directories(ntfs_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()


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

add_executable(bootplan_test bootplan_test.c)
target_link_libraries(bootplan_test PRIVATE imgtool)

add_executable(qcscan_test qcscan_test.c)
target_include_directories(qcscan_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(qcscan_test PRIVATE imgtool)

add_executable(identify_test identify_test.c)
target_link_libraries(identify_test PRIVATE imgtool)

add_executable(race_test race_test.c)
target_link_libraries(race_test PRIVATE imgtool)

# ADS（命名数据流）探针：file.copy / ntfs.copy 必须保留附加数据
if(NTFS_INC AND NTFS_LIB)
  add_executable(ads_probe ads_probe.c)
  target_include_directories(ads_probe PRIVATE ${NTFS_INC})
  target_link_libraries(ads_probe PRIVATE imgtool ${NTFS_LIB})
endif()

# 进程内复用：同一进程里连续两次 fs.format（NTFS）不许崩（mkntfs 静态状态）
add_executable(format_twice_test format_twice_test.c)
target_link_libraries(format_twice_test PRIVATE imgtool)

# REQ-21：在 1 MB 栈的线程里跑会吃栈的命令（模拟 App 的 ART worker）
add_executable(stack_test stack_test.c)
target_link_libraries(stack_test PRIVATE imgtool)

# REQ-25：同一进程里按 App 顺序连跑（ISO 句柄/VFD 槽位泄漏会在这里暴露）
add_executable(winre_chain winre_chain.c)
target_link_libraries(winre_chain PRIVATE imgtool)

# REQ-27：dlopen 预编译 .so 再跑命令（专门抓"静态 CLI 好、App 里的 .so 坏"这一类）
add_executable(dl_cmd dl_cmd.c)
target_link_libraries(dl_cmd PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(dl_cmd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

# REQ-31：index_token 生命周期（同进程）
add_executable(archive_chain archive_chain.c)
target_link_libraries(archive_chain PRIVATE imgtool)
