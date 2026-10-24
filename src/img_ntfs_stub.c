/* img_ntfs_stub.c — NTFS 功能关闭（-DIMG_WITH_NTFS3G=OFF）时的占位实现
 *
 * 为什么需要它（工程债 #13）：img_recovery / img_identify / img_cmds 里的
 *   part.scan、win.version 等**命令本身与 NTFS 无关**，但会顺手做 NTFS 那一支。
 *   以前这些引用直接把链接打断 —— 于是"关掉 NTFS 就构建不出来"，
 *   降级开关形同虚设。
 *
 * 设计取舍：
 *   · 不把这些调用点全部 #if 掉 —— 命令层会因此长出几十处条件编译，反而更容易漏；
 *   · 用**同一份头文件**的占位实现，签名由编译器强制对齐；少了/多了函数立刻报错；
 *   · 一律返回 IMG_ENOTSUP、mount 返回 NULL —— 命令层本来就有
 *     `if(!IMG_WITH_NTFS3G) return feat_unavail(...)` 的入口闸，
 *     真走到这里也只会在结果里得到"功能不可用"，不会静默成功。
 * 本文件只在 NTFS 关闭时参与编译（见 CMakeLists.txt）。
 */
#include "img_ntfs_fs.h"

#define NA ((int)IMG_ENOTSUP)

img_ntfs_fs *img_ntfs_mount_rw(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes){
    (void)q; (void)part_lba; (void)part_bytes; return NULL;
}
void img_ntfs_umount(img_ntfs_fs *fs){ (void)fs; }

int img_ntfs_mkdir_p(img_ntfs_fs *fs, const char *abs_path){ (void)fs; (void)abs_path; return NA; }
bool img_ntfs_path_exists(img_ntfs_fs *fs, const char *abs_path){ (void)fs; (void)abs_path; return false; }

int img_ntfs_read_file(img_ntfs_fs *fs, const char *abs_path, void **out, size_t *out_len){
    (void)fs; (void)abs_path; (void)out; (void)out_len; return NA;
}
int img_ntfs_copy_out(img_ntfs_fs *fs, const char *abs_path, const char *dst_path,
                      img_prog *prog, uint64_t *out_bytes){
    (void)fs; (void)abs_path; (void)dst_path; (void)prog; (void)out_bytes; return NA;
}
int img_ntfs_put_file_mem(img_ntfs_fs *fs, const void *data, size_t len, const char *dst_abs_path){
    (void)fs; (void)data; (void)len; (void)dst_abs_path; return NA;
}
int img_ntfs_write_reagent(img_ntfs_fs *fs, const char *winre_dir_abs, const char *winre_loc,
                           const char *bcd_guid, uint64_t part_off){
    (void)fs; (void)winre_dir_abs; (void)winre_loc; (void)bcd_guid; (void)part_off; return NA;
}
int img_ntfs_put_file(img_ntfs_fs *fs, const char *local_path, const char *dst_abs_path,
                      img_prog *prog){
    (void)fs; (void)local_path; (void)dst_abs_path; (void)prog; return NA;
}
int img_ntfs_file_size(img_ntfs_fs *fs, const char *abs_path, uint64_t *out_size){
    (void)fs; (void)abs_path; (void)out_size; return NA;
}
int img_ntfs_list_dir(img_ntfs_fs *fs, const char *abs_path, img_ntfs_list_cb cb, void *ud){
    (void)fs; (void)abs_path; (void)cb; (void)ud; return NA;
}
int img_ntfs_list_dir_sizes(img_ntfs_fs *fs, const char *abs_path,
                            img_ntfs_list_cb cb, void *ud){
    (void)fs; (void)abs_path; (void)cb; (void)ud; return NA;
}
int img_ntfs_free_bytes(img_ntfs_fs *fs, uint64_t *out_free, uint64_t *out_used){
    (void)fs; (void)out_free; (void)out_used; return NA;
}
int img_ntfs_label(img_ntfs_fs *fs, char *out, size_t cap){
    (void)fs; if(out && cap) out[0]=0; return NA;
}
int img_ntfs_rm(img_ntfs_fs *fs, const char *abs_path){ (void)fs; (void)abs_path; return NA; }
int img_ntfs_move_into(img_ntfs_fs *fs, const char *dst_dir, const char **names,
                       int nnames, int *moved_out){
    (void)fs; (void)dst_dir; (void)names; (void)nnames; (void)moved_out; return NA;
}
int img_ntfs_move_path(img_ntfs_fs *fs, const char *src_abs, const char *dst_abs,
                          bool overwrite, bool allow_dir_move){
    (void)fs; (void)src_abs; (void)dst_abs; (void)overwrite; return NA;
}
/* 写后自证（NTFS 关闭时同样如实报"功能不可用"，绝不能返回 IMG_OK 骗人） */
int img_ntfs_verify_dir(img_ntfs_fs *fs, const char *abs_path){
    (void)fs;(void)abs_path; return IMG_ENOTSUP;
}
int img_ntfs_verify_path(img_ntfs_fs *fs, const char *abs_path,
                         uint64_t *ino_out, bool *isdir_out){
    (void)fs;(void)abs_path;
    if(ino_out) *ino_out=0;
    if(isdir_out) *isdir_out=false;
    return IMG_ENOTSUP;
}

int img_ntfs_dir_parent_check(img_ntfs_fs *fs, const char *abs_path,
                              uint64_t *out_ino, uint64_t *out_parent_ino,
                              uint64_t *out_parent_by_path, bool *out_backlink_ok,
                              bool *out_dotdot_present){
    (void)fs; (void)abs_path; (void)out_ino; (void)out_parent_ino;
    (void)out_parent_by_path; (void)out_backlink_ok; (void)out_dotdot_present; return NA;
}
int img_ntfs_dirty_check(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes, bool *out_dirty){
    (void)q; (void)part_lba; (void)part_bytes; (void)out_dirty; return NA;
}
int img_ntfs_precheck(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                      bool *out_dirty, int *out_root_entries){
    (void)q; (void)part_lba; (void)part_bytes; (void)out_dirty; (void)out_root_entries; return NA;
}
/* 流式读写（跨分区/跨镜像复制）在裁剪构建里一律"不支持" */
int img_ntfs_rd_open(img_ntfs_fs *fs, const char *abs_path, img_ntfs_rd **out,
                     img_src_meta *meta){
    (void)fs;(void)abs_path;(void)meta;
    if(out) *out=NULL;
    return NA;
}
int  img_ntfs_rd_read(img_ntfs_rd *rd, uint64_t off, void *buf, size_t want, size_t *got){
    (void)rd;(void)off;(void)buf;(void)want;
    if(got) *got=0;
    return NA;
}
void img_ntfs_rd_close(img_ntfs_rd *rd){ (void)rd; }
int img_ntfs_write_stream(img_ntfs_fs *fs, const char *dst_abs,
                          const img_stream *src, const img_src_meta *meta,
                          const img_copy_opts *opt, img_prog *prog,
                          img_copy_result *out){
    (void)fs;(void)dst_abs;(void)src;(void)meta;(void)opt;(void)prog;(void)out;
    return NA;
}

/* 时间 / 唯一名（跨分区复制引擎会调；裁剪构建一律"不支持"） */
int img_ntfs_file_times(img_ntfs_fs *fs, const char *abs_path, uint64_t out[4]){
    (void)fs;(void)abs_path;(void)out; return NA;
}
int img_ntfs_set_times(img_ntfs_fs *fs, const char *abs_path, const uint64_t in[4]){
    (void)fs;(void)abs_path;(void)in; return NA;
}
int img_ntfs_unique_name(img_ntfs_fs *fs, const char *dir_abs, const char *base,
                         char *out, size_t cap){
    (void)fs;(void)dir_abs;(void)base;(void)out;(void)cap; return NA;
}
int img_ntfs_dir_rev(img_ntfs_fs *fs, const char *dir_abs, uint64_t *out_rev){
    (void)fs;(void)dir_abs;(void)out_rev; return NA;
}
int img_ntfs_dir_lookup_ino(img_ntfs_fs *fs, const char *dir_abs, const char *name,
                            uint64_t *out_ino){
    (void)fs;(void)dir_abs;(void)name;(void)out_ino; return NA;
}

/* 命名数据流（裁剪构建没有 NTFS，全部不支持） */
int img_ntfs_list_xattrs(img_ntfs_fs *fs, const char *abs_path,
                         img_ntfs_xattr_cb cb, void *ud){
    (void)fs;(void)abs_path;(void)cb;(void)ud; return NA;
}
int img_ntfs_get_xattr(img_ntfs_fs *fs, const char *abs_path, const char *name,
                       void **out, size_t *out_len){
    (void)fs;(void)abs_path;(void)name;(void)out;(void)out_len; return NA;
}
int img_ntfs_set_xattr(img_ntfs_fs *fs, const char *abs_path, const char *name,
                       const void *data, size_t len){
    (void)fs;(void)abs_path;(void)name;(void)data;(void)len; return NA;
}
int img_ntfs_copy_xattrs(img_ntfs_fs *src_fs, const char *src_abs,
                         img_ntfs_fs *dst_fs, const char *dst_abs, int *count){
    (void)src_fs;(void)src_abs;(void)dst_fs;(void)dst_abs;(void)count; return NA;
}

int img_ntfs_rm_tree(img_ntfs_fs *fs, const char *abs_path, int *removed){
    (void)fs;(void)abs_path;
    if(removed) *removed=0;
    return NA;
}
