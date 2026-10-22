/* img_win_finish.c — 离线修 SYSTEM\Setup\Status\ChildCompletion\setup.exe
 * 见头文件里的官方依据。这里只做一件事：把那一个 DWORD 改成 3，其余字节不动。
 */
#include "img_win_finish.h"
#include "img_ntfs_fs.h"
#include <errno.h>
#include <hivex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HIVE_PATH "/Windows/System32/config/SYSTEM"

/* 读一个 REG_DWORD；没有/类型不对都返回 -1（诊断用，不报错）*/
static int get_dword(hive_h *h, hive_node_h node, const char *key){
    if(!h || node == 0) return -1;
    hive_value_h v = hivex_node_get_value(h, node, key);
    if(v == 0) return -1;
    hive_type t;
    size_t len = 0;
    if(hivex_value_type(h, v, &t, &len) < 0) return -1;
    if(t != hive_t_REG_DWORD || len != 4) return -1;
    char *data = hivex_value_value(h, v, &t, &len);
    if(!data) return -1;
    uint32_t d;
    memcpy(&d, data, 4);
    free(data);
    return (int)d;
}

int img_win_finish(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes, bool dry_run,
                   img_win_finish_t *out, char *err, size_t errcap){
    if(!out) return IMG_EINVAL;
    memset(out, 0, sizeof *out);
    out->old_setup_exe = -1;
    out->setup_type = -1;
    out->system_setup_in_progress = -1;
    if(!q || !part_bytes) return IMG_EINVAL;

    img_ntfs_fs *fs = img_ntfs_mount_rw(q, part_lba, part_bytes);
    if(!fs){
        if(err) snprintf(err, errcap, "目标分区不是可写 NTFS（LBA 不对或未格式化）");
        return IMG_ESTATE;
    }

    /* SYSTEM hive 约 9 MB：先落到临时文件，hivex 只接受文件路径 */
    const char *tdir = img_tmpdir();
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s/imgtool-system-XXXXXX", (tdir && tdir[0]) ? tdir : "/tmp");
    int fd = mkstemp(tmp);
    if(fd < 0){
        if(err) snprintf(err, errcap, "建临时文件失败: %s", strerror(errno));
        img_ntfs_umount(fs);
        return IMG_EIO;
    }
    close(fd);

    int rc = img_ntfs_copy_out(fs, HIVE_PATH, tmp, NULL, NULL);
    if(rc != IMG_OK){
        if(err) snprintf(err, errcap,
            "读不到 %s（这个分区可能不是 Windows 系统分区，或 LBA 给错）", HIVE_PATH);
        unlink(tmp);
        img_ntfs_umount(fs);
        return rc;
    }

    hive_h *h = hivex_open(tmp, dry_run ? 0 : HIVEX_OPEN_WRITE);
    if(!h){
        if(err) snprintf(err, errcap, "hivex 打不开 SYSTEM hive（文件损坏？）");
        unlink(tmp);
        img_ntfs_umount(fs);
        return IMG_EFORMAT;
    }

    hive_node_h root  = hivex_root(h);
    hive_node_h setup = root ? hivex_node_get_child(h, root, "Setup") : 0;
    out->setup_type              = get_dword(h, setup, "SetupType");
    out->system_setup_in_progress = get_dword(h, setup, "SystemSetupInProgress");
    out->cmdline_present         = (setup && hivex_node_get_value(h, setup, "CmdLine") != 0);

    hive_node_h st = setup ? hivex_node_get_child(h, setup, "Status") : 0;
    hive_node_h cc = st    ? hivex_node_get_child(h, st,   "ChildCompletion") : 0;
    out->found_node = (cc != 0);

    if(cc){
        out->old_setup_exe = get_dword(h, cc, "setup.exe");
        out->found_value   = (out->old_setup_exe >= 0);
        out->new_setup_exe = out->found_value ? out->old_setup_exe : 3;

        if(!dry_run && out->old_setup_exe != 3){
            uint32_t three = 3;
            hive_set_value sv;
            memset(&sv, 0, sizeof sv);
            sv.key   = (char *)"setup.exe";
            sv.t     = hive_t_REG_DWORD;
            sv.len   = sizeof three;
            sv.value = (char *)&three;
            if(hivex_node_set_value(h, cc, &sv, 0) < 0){
                if(err) snprintf(err, errcap, "写 ChildCompletion\\setup.exe 失败");
                hivex_close(h);
                unlink(tmp);
                img_ntfs_umount(fs);
                return IMG_EIO;
            }
            out->new_setup_exe = 3;
            out->changed = true;
        }
        if(out->changed && hivex_commit(h, NULL, 0) < 0){
            if(err) snprintf(err, errcap, "hivex_commit 失败（hive 写不回去）");
            hivex_close(h);
            unlink(tmp);
            img_ntfs_umount(fs);
            return IMG_EIO;
        }
    }
    hivex_close(h);

    if(out->changed){
        rc = img_ntfs_put_file(fs, tmp, HIVE_PATH, NULL);
        if(rc != IMG_OK){
            if(err) snprintf(err, errcap, "把改好的 SYSTEM hive 写回分区失败");
            unlink(tmp);
            img_ntfs_umount(fs);
            return rc;
        }
    }

    unlink(tmp);
    img_ntfs_umount(fs);
    return IMG_OK;
}