add_executable(imgtest_progress progress_test.c)
target_link_libraries(imgtest_progress PRIVATE imgtool)
add_executable(imgdump imgdump.c)
add_executable(imgq2test imgq2test.c)
add_executable(imgparts imgparts.c)
add_executable(imgpart imgpart.c)
add_executable(imglayout imglayout.c)
add_executable(imgresize imgresize.c)
add_executable(imgmkfs imgmkfs.c)
target_link_libraries(imgq2test PRIVATE imgtool)
target_link_libraries(imgdump PRIVATE imgtool)
target_link_libraries(imgparts PRIVATE imgtool)
target_link_libraries(imgpart PRIVATE imgtool)
target_link_libraries(imglayout PRIVATE imgtool)
target_link_libraries(imgresize PRIVATE imgtool)
target_link_libraries(imgmkfs PRIVATE imgtool)

add_executable(imgbc imgbc.c)
target_link_libraries(imgbc PRIVATE imgtool)

add_executable(imgfixboot imgfixboot.c)
target_link_libraries(imgfixboot PRIVATE imgtool)

add_executable(imgntfscp imgntfscp.c)
target_link_libraries(imgntfscp PRIVATE imgtool)
target_sources(imgntfscp PRIVATE /workspace/build/mkntfs_obj/ntfscp.o)

add_executable(imgraw imgraw.c)
target_link_libraries(imgraw PRIVATE imgtool)

add_executable(imgntfsfs imgntfsfs.c)
target_link_libraries(imgntfsfs PRIVATE imgtool)
target_sources(imgntfsfs PRIVATE /workspace/build/mkntfs_obj/ntfsls.o /workspace/build/mkntfs_obj/ntfscat.o)

add_executable(imggenbcd imggenbcd.c ../src/img_bcd.c)
target_include_directories(imggenbcd PRIVATE ${HIVEX_INC})
target_link_libraries(imggenbcd PRIVATE imgtool ${HIVEX_LIB})

add_executable(imgfatfs imgfatfs.c)
target_link_libraries(imgfatfs PRIVATE imgtool)
# img_fat32_fs.c 已在主库中，勿重复编译

add_executable(imgwimls imgwimls.c)
target_link_libraries(imgwimls PRIVATE wim)
add_executable(imgcmd imgcmd.c)
target_link_libraries(imgcmd PRIVATE imgtool)
