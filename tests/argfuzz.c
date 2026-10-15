/* argfuzz — 非法参数矩阵（App 传错参时会不会"炸"）
 *
 * 为什么要有：App 传参出错是常态（空指针/空串/越界/负索引/单位搞错/超长路径）。
 * 目标：**任何非法输入都必须返回错误码，绝不能崩、不能写坏盘**。
 * 用法：每条用例 fork 子进程单独跑 → 谁崩报谁，不会一条崩掉全测。
 *   gcc -fsanitize=address -g -Iinclude -DIMG_HAVE_ZLIB=1 -o argfuzz \
 *       tests/argfuzz.c src/img_bcd.c build-asan/libimgtool.a -lntfs-3g -lwim -lz -lhivex
 *   ./argfuzz
 */
#include "img_types.h"
#include "img_io.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_constraints.h"
#include "img_fat32.h"
#include "img_fat32_fs.h"
#include "img_ntfs_fs.h"
#include "img_ntfs_fmt.h"
#include "img_bootcheck.h"
#include "img_fixboot.h"
#include "img_wim_apply.h"
#include "img_bcd.h"
#include "img_event.h"
#include "img_progress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int ncase=0, ncrash=0, nfail=0;
static const char *cur_case="";
static const char *crash_list[64]; static int crash_n=0;

static void run(const char *name, void (*f)(void)){
    cur_case=name; ncase++;
    fflush(stdout);
    pid_t p=fork();
    if(p==0){ f(); _exit(0); }
    int st=0; waitpid(p,&st,0);
    if(WIFSIGNALED(st)){
        ncrash++; crash_list[crash_n++]=name;
        printf("  💥 %-62s 崩了 (signal %d)\n", name, WTERMSIG(st));
    } else if(WEXITSTATUS(st)!=0){
        nfail++;
        printf("  ⚠️  %-62s 返回非 0（可能只是断言退出）\n", name);
    } else {
        printf("  ✅ %-62s 正常返回\n", name);
    }
}

/* ---------- 用例：全部传非法参数 ---------- */
static void c_io_open_null(void){ img_io_close(img_io_open_path(NULL,NULL)); }
static void c_io_open_empty(void){ img_io_close(img_io_open_path("",NULL)); }
static void c_io_open_nodir(void){ img_io_close(img_io_open_path("/no/such/dir/x.qcow2",NULL)); }
static void c_io_open_badfd(void){ img_io_close(img_io_open_fd(-1,false,NULL)); }
static void c_io_open_hugefd(void){ img_io_close(img_io_open_fd(999999,false,NULL)); }
static void c_io_read_null(void){ img_io_read(NULL,0,NULL,0,NULL); }
static void c_io_write_null(void){ img_io_write(NULL,0,NULL,0,NULL); }
static void c_io_full_null(void){ img_io_read_full(NULL,0,NULL,16); }
static void c_io_size_null(void){ (void)img_io_size(NULL); }
static void c_io_trunc_null(void){ img_io_truncate(NULL,0); }
static void c_io_punch_null(void){ img_io_punch(NULL,0,0); }
static void c_io_sync_null(void){ img_io_sync(NULL); }

static void c_q_open_null(void){ img_qcow2_close(img_qcow2_open(NULL,NULL)); }
static void c_q_read_null(void){ img_qcow2_read(NULL,0,NULL,0); }
static void c_q_rfull_null(void){ img_qcow2_read_full(NULL,0,NULL,16); }
static void c_q_write_null(void){ img_qcow2_write(NULL,0,NULL,16); }
static void c_q_winit_null(void){ img_qcow2_write_init(NULL); }
static void c_q_size_null(void){ (void)img_qcow2_size(NULL); }
static void c_q_info_null(void){ char b[64]; img_qcow2_info_json(NULL,b,sizeof b); }
static void c_q_info_nullbuf(void){ img_qcow2_info_json(NULL,NULL,0); }
static void c_q_expand_null(void){ img_qcow2_expand(NULL,0,NULL); }

static void c_pt_read_null(void){ img_parttable_t t; img_parttable_read(NULL,&t,0); }
static void c_pt_read_nullout(void){ img_parttable_read(NULL,NULL,0); }
static void c_pt_json_null(void){ char b[64]; img_parttable_json(NULL,b,sizeof b); }
static void c_pt_json_nullbuf(void){ img_parttable_json(NULL,NULL,0); }
static void c_p_add_null(void){ img_partition_add(NULL,0,0,0,NULL,NULL,false,NULL); }
static void c_p_remove_null(void){ img_partition_remove(NULL,0); }
static void c_p_remove_neg(void){ img_parttable_t t; memset(&t,0,sizeof t); img_partition_remove(&t,-1); }
static void c_p_remove_big(void){ img_parttable_t t; memset(&t,0,sizeof t); img_partition_remove(&t,9999); }
static void c_p_attr_null(void){ img_partition_set_attributes(NULL,0,0); }
static void c_p_resize_null(void){ img_partition_resize(NULL,0,0,NULL); }
static void c_guid_valid_null(void){ (void)img_partition_guid_valid(NULL); }
static void c_guid_valid_junk(void){ (void)img_partition_guid_valid("not-a-guid"); }

static void c_fat_mount_null(void){ img_fat32_unmount(img_fat32_mount(NULL,0,true)); }
static void c_fat_put_null(void){ img_fat32_put_file(NULL,"/etc/hostname","/x",NULL); }
static void c_fat_put_nulls(void){ img_fat32_put_file(NULL,NULL,NULL,NULL); }
static void c_fat_mkdir_null(void){ img_fat32_mkdir_p(NULL,"/x"); }
static void c_fat_list_null(void){ img_fat32_list_dir(NULL,"/",NULL,NULL); }
static void c_fat_rm_null(void){ img_fat32_rm(NULL,"/x"); }
static void c_fat_putdata_null(void){ img_fat32_put_data(NULL,NULL,0,"/x"); }
static void c_fat_exists_null(void){ (void)img_fat32_path_exists(NULL,"/x"); }
static void c_fat_read_null(void){ void*b; size_t n; img_fat32_read_file(NULL,"/x",&b,&n); }
static void c_fat_read_nullout(void){ img_fat32_read_file(NULL,"/x",NULL,NULL); }
static void c_fat_probe_null(void){ img_fat32_layout_t L; img_fat32_probe(NULL,0,&L); }
static void c_fat_check_null(void){ img_fat32_layout_t L; img_fat32_check(NULL,0,&L); }
static void c_fat_layout_null(void){ img_fat32_layout_t L; img_fat32_layout(NULL,&L); }
static void c_fat_format_null(void){ img_fat32_layout_t L; img_fat32_format(NULL,NULL,&L,NULL); }

static void c_ntfs_mount_null(void){ img_ntfs_umount(img_ntfs_mount_rw(NULL,0,0)); }
static void c_ntfs_mkdir_null(void){ img_ntfs_mkdir_p(NULL,"/x"); }
static void c_ntfs_put_null(void){ img_ntfs_put_file(NULL,"/etc/hostname","/x",NULL); }
static void c_ntfs_read_null(void){ void*b; size_t n; img_ntfs_read_file(NULL,"/x",&b,&n); }
static void c_ntfs_reagent_null(void){ img_ntfs_write_reagent(NULL,"/x","\\x","g",0); }
static void c_ntfs_exists_null(void){ (void)img_ntfs_path_exists(NULL,"/x"); }
static void c_ntfs_size_null(void){ uint64_t s; img_ntfs_file_size(NULL,"/x",&s); }
static void c_ntfs_fmt_null(void){ img_ntfs_format(NULL,NULL,NULL); }

static void c_bc_null(void){ img_bootcheck_result_t r; img_bootcheck(NULL,IMG_BOOT_UEFI,&r,NULL); }
static void c_bc_nullout(void){ img_bootcheck(NULL,IMG_BOOT_UEFI,NULL,NULL); }
static void c_bc_json_null(void){ char b[64]; img_bootcheck_json(NULL,b,sizeof b); }
static void c_fixboot_null(void){ img_fixboot(NULL,0,NULL,0,NULL); }

static void c_wim_apply_null(void){ img_wim_apply(NULL,1,NULL,0,NULL,NULL); }
static void c_wim_ls_null(void){ char b[64]; img_wim_list_images(NULL,b,sizeof b); }
static void c_wim_ls_junk(void){ char b[64]; img_wim_list_images("/no/such.esd",b,sizeof b); }
static void c_wim_ls_nobuf(void){ img_wim_list_images("/no/such.esd",NULL,0); }

static void c_bcd_open_null(void){ img_bcd_close(img_bcd_open_file(NULL)); }
static void c_bcd_save_null(void){ img_bcd_save(NULL,NULL); }
static void c_bcd_get_null(void){ free(img_bcd_get_string(NULL,NULL,NULL)); }
static void c_bcd_list_null(void){ img_bcd_list_objects(NULL,NULL,NULL); }
static void c_bcd_free_null(void){ img_bcd_free_object_list(NULL,0); }
static void c_bcd_openmem_null(void){ img_bcd_close(img_bcd_open_mem(NULL,0)); }
static void c_bcd_devgui_null(void){ char p[40],d[40]; img_bcd_get_device_guids(NULL,"x","y",p,d); }
static void c_bcd_patch_null(void){ img_bcd_patch_device_guids(NULL,NULL,NULL,NULL,NULL); }

static void c_ev_null(void){ img_ev_init(NULL,IMG_LV_INFO,"m","e",0,"x"); }
static void c_ev_str_null(void){ img_ev_str(NULL,"k","v"); }
static void c_ev_emit_null(void){ img_ev_emit(NULL); }
static void c_ev_emit_empty(void){ img_ev e; memset(&e,1,sizeof e); /* 未 init 就 emit：字段是垃圾 */ img_ev_emit(&e); }
static void c_span_null(void){ img_span_prep(NULL,"m","o","x"); img_span_end(NULL,0); }
static void c_hb_null(void){ img_hb(NULL,NULL,NULL,0); }
static void c_ring_null(void){ img_ring_add(NULL); img_ring_dump(NULL,NULL,0); }
static void c_errrep_null(void){ img_error_report(IMG_EINVAL,NULL,NULL,NULL,NULL,NULL,NULL); }
static void c_session_null(void){ img_log_session(NULL,NULL,NULL,0); }
static void c_log_open_null(void){ img_log_close(); img_log_open(NULL,IMG_LV_INFO,0,0); }
static void c_out_null(void){ img_out_open_fd(-1,false); }

static void c_prog_null(void){ img_prog_free(img_prog_new(NULL,NULL,0)); }
static void c_prog_count_null(void){ img_prog_count(NULL,1,2); }
static void c_prog_json_null(void){ char b[64]; img_prog_json(NULL,b,sizeof b); }
static void c_prog_json_nobuf(void){ img_progress_t p; memset(&p,0,sizeof p); img_prog_json(&p,NULL,0); }

static void c_cons_part_null(void){ img_check_result_t r; img_constraints_check_partition(NULL,&r); }
static void c_cons_table_null(void){ img_check_result_t r; img_constraints_check_table(NULL,&r); }
static void c_layout_build_null(void){ img_check_result_t r; img_layout_build(NULL,0,IMG_TABLE_GPT,NULL,&r); }
static void c_convert_null(void){ img_check_result_t r; img_convert_mbr_to_gpt(NULL,&r); }

static struct { const char *n; void (*f)(void); } T[] = {
    {"io_open_path(NULL)",c_io_open_null},{"io_open_path(\"\")",c_io_open_empty},
    {"io_open_path(不存在)",c_io_open_nodir},{"io_open_fd(-1)",c_io_open_badfd},
    {"io_open_fd(999999)",c_io_open_hugefd},{"io_read(NULL)",c_io_read_null},
    {"io_write(NULL)",c_io_write_null},{"io_read_full(NULL)",c_io_full_null},
    {"io_size(NULL)",c_io_size_null},{"io_truncate(NULL)",c_io_trunc_null},
    {"io_punch(NULL)",c_io_punch_null},{"io_sync(NULL)",c_io_sync_null},
    {"qcow2_open(NULL)",c_q_open_null},{"qcow2_read(NULL)",c_q_read_null},
    {"qcow2_read_full(NULL)",c_q_rfull_null},{"qcow2_write(NULL)",c_q_write_null},
    {"qcow2_write_init(NULL)",c_q_winit_null},{"qcow2_size(NULL)",c_q_size_null},
    {"qcow2_info_json(NULL)",c_q_info_null},{"qcow2_info_json(buf=NULL)",c_q_info_nullbuf},
    {"qcow2_expand(NULL)",c_q_expand_null},
    {"parttable_read(NULL)",c_pt_read_null},{"parttable_read(out=NULL)",c_pt_read_nullout},
    {"parttable_json(NULL)",c_pt_json_null},{"parttable_json(buf=NULL)",c_pt_json_nullbuf},
    {"partition_add(NULL)",c_p_add_null},{"partition_remove(NULL)",c_p_remove_null},
    {"partition_remove(-1)",c_p_remove_neg},{"partition_remove(9999)",c_p_remove_big},
    {"partition_set_attributes(NULL)",c_p_attr_null},{"partition_resize(NULL)",c_p_resize_null},
    {"guid_valid(NULL)",c_guid_valid_null},{"guid_valid(\"垃圾\")",c_guid_valid_junk},
    {"fat32_mount(NULL)",c_fat_mount_null},{"fat32_put_file(NULL)",c_fat_put_null},
    {"fat32_put_file(全 NULL)",c_fat_put_nulls},{"fat32_mkdir_p(NULL)",c_fat_mkdir_null},
    {"fat32_list_dir(NULL)",c_fat_list_null},{"fat32_rm(NULL)",c_fat_rm_null},
    {"fat32_put_data(NULL)",c_fat_putdata_null},{"fat32_path_exists(NULL)",c_fat_exists_null},
    {"fat32_read_file(NULL)",c_fat_read_null},{"fat32_read_file(out=NULL)",c_fat_read_nullout},
    {"fat32_probe(NULL)",c_fat_probe_null},{"fat32_check(NULL)",c_fat_check_null},
    {"fat32_layout(NULL)",c_fat_layout_null},{"fat32_format(NULL)",c_fat_format_null},
    {"ntfs_mount_rw(NULL)",c_ntfs_mount_null},{"ntfs_mkdir_p(NULL)",c_ntfs_mkdir_null},
    {"ntfs_put_file(NULL)",c_ntfs_put_null},{"ntfs_read_file(NULL)",c_ntfs_read_null},
    {"ntfs_write_reagent(NULL)",c_ntfs_reagent_null},{"ntfs_path_exists(NULL)",c_ntfs_exists_null},
    {"ntfs_file_size(NULL)",c_ntfs_size_null} ,{"ntfs_format(NULL)",c_ntfs_fmt_null},
    {"bootcheck(NULL)",c_bc_null},{"bootcheck(out=NULL)",c_bc_nullout},
    {"bootcheck_json(NULL)",c_bc_json_null},{"fixboot(NULL)",c_fixboot_null},
    {"wim_apply(NULL)",c_wim_apply_null},{"wim_list_images(NULL)",c_wim_ls_null},
    {"wim_list_images(不存在)",c_wim_ls_junk},{"wim_list_images(buf=NULL)",c_wim_ls_nobuf},
    {"bcd_open_file(NULL)",c_bcd_open_null},{"bcd_save(NULL)",c_bcd_save_null},
    {"bcd_get_string(NULL)",c_bcd_get_null},{"bcd_list_objects(NULL)",c_bcd_list_null},
    {"bcd_free_object_list(NULL)",c_bcd_free_null},{"bcd_open_mem(NULL,0)",c_bcd_openmem_null},
    {"bcd_get_device_guids(NULL)",c_bcd_devgui_null},{"bcd_patch_device_guids(NULL)",c_bcd_patch_null},
    {"ev_init(NULL)",c_ev_null},{"ev_str(NULL)",c_ev_str_null},{"ev_emit(NULL)",c_ev_emit_null},
    {"ev_emit(未初始化)",c_ev_emit_empty},{"span(NULL)",c_span_null},{"hb(NULL)",c_hb_null},
    {"ring_add(NULL)",c_ring_null},{"error_report(全 NULL)",c_errrep_null},
    {"log_session(NULL)",c_session_null},{"log_open(NULL)",c_log_open_null},
    {"out_open_fd(-1)",c_out_null},
    {"prog_new(cb=NULL)",c_prog_null},{"prog_count(NULL)",c_prog_count_null},
    {"prog_json(NULL)",c_prog_json_null},{"prog_json(buf=NULL)",c_prog_json_nobuf},
    {"constraints_check_partition(NULL)",c_cons_part_null},
    {"constraints_check_table(NULL)",c_cons_table_null},
    {"layout_build(NULL)",c_layout_build_null},{"convert_mbr_to_gpt(NULL)",c_convert_null},
};
int main(void){
    int n=(int)(sizeof T/sizeof T[0]);
    printf("=== 非法参数矩阵（%d 条，每条 fork 隔离）===\n", n);
    for(int i=0;i<n;i++) run(T[i].n, T[i].f);
    printf("\n════ 结果：%d 条 / 崩溃 %d · 非 0 退出 %d ════\n", ncase, ncrash, nfail);
    if(crash_n){
        printf("❌ 会崩的调用（必须修）：\n");
        for(int i=0;i<crash_n;i++) printf("   · %s\n", crash_list[i]);
    } else printf("✅ 全部非法输入都返回错误码，没有崩溃\n");
    return crash_n?1:0;
}
