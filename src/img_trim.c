/* img_trim.c — 按客户机空闲位图原地释放 qcow2 簇（REQ-66）
 *
 * 为什么不能只靠"整盘重写"（img.compact）：
 *   12 GB 的系统盘，compact 要再写 5.7 GB，宿主上得同时放得下两份。
 *   这里改的是元数据 + 打洞，理论上不写任何数据字节。
 *
 * 取的是哪门子"空闲"：卷自己的位图（NTFS 的 $Bitmap / FAT 的 FAT 表）。
 *   —— 那就是客户机认为"这些簇没被任何文件用"的唯一权威来源。
 *   我们不用"内容全是零"来猜（Windows 上大量合法数据是零）。
 *
 * 许可证：算法语义参考 QEMU 的 qcow2 discard 路径（block/qcow2*.c，MIT）
 *   与 ntfs-3g 的 FITRIM（GPL-2.0）；实现是照语义重写的，没有复制代码。
 */
#include "img_trim.h"
#include "img_qcow2_int.h"
#if defined(IMG_WITH_NTFS3G) && IMG_WITH_NTFS3G
#include "img_ntfs_fs.h"
#include "img_ntfs_fmt.h"   /* img_ntfs_probe */
#else
#define IMG_WITH_NTFS3G 0
#endif
#include "img_fat32_fs.h"
#include "img_io.h"
#include "img_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t host_alloc_bytes(img_io *io){
    if(!io) return 0;
    int fd = img_io_fd(io);
    if(fd < 0) return 0;
    struct stat st;
    if(fstat(fd, &st) != 0) return 0;
    return (uint64_t)st.st_blocks * 512ULL;
}

static inline int bit_get(const uint8_t *b, uint64_t i){
    return (b[i >> 3] >> (i & 7)) & 1;
}

/* 动态数组：要释放的 host 簇偏移 */
typedef struct { uint64_t *v; size_t n, cap; } u64v;
static int u64v_push(u64v *a, uint64_t x){
    if(a->n == a->cap){
        size_t nc = a->cap ? a->cap * 2 : 1024;
        uint64_t *nv = (uint64_t*)realloc(a->v, nc * sizeof *nv);
        if(!nv) return IMG_ENOMEM;
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = x;
    return IMG_OK;
}

int img_trim(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
             const img_trim_opts_t *opts, img_trim_result_t *out,
             img_prog *prog, char *err, size_t errcap){
    if(err && errcap) err[0] = 0;
    if(!q || !out || !part_bytes) return IMG_EINVAL;
    memset(out, 0, sizeof *out);
    const bool dry   = opts ? opts->dry_run : false;
    const bool punch = opts ? opts->punch   : true;

    /* ---------------- 门禁 ---------------- */
    if(q->fmt != DISK_FMT_QCOW2){
        if(err&&errcap) snprintf(err,errcap,"只有 qcow2 能原地清理（当前是 %s）",
                                 img_diskfmt_name(q->fmt));
        return IMG_ENOTSUP;
    }
    if(q->h.nb_snapshots){
        if(err&&errcap) snprintf(err,errcap,"映像里有 %u 个快照：释放簇会改掉快照的历史数据，先合并或删除快照",
                                 q->h.nb_snapshots);
        return IMG_ENOTSUP;
    }
    if(q->h.backing_file_offset){
        if(err&&errcap) snprintf(err,errcap,"映像是 backing 链的一层：我们读不到底层数据，空闲位图会算错");
        return IMG_ENOTSUP;
    }
    if(q->h.incompatible & IMG_QCOW2_INCOMPAT_DIRTY){
        if(err&&errcap) snprintf(err,errcap,"映像被标记为脏（上次没有正常关闭），先用 qemu-img check 修");
        return IMG_ESTATE;
    }
    if(q->h.incompatible & IMG_QCOW2_INCOMPAT_CORRUPT){
        if(err&&errcap) snprintf(err,errcap,"映像被标记为损坏，先用 qemu-img check 修");
        return IMG_ESTATE;
    }
    if(q->refcount_bits != 16 && q->refcount_bits != 32){
        if(err&&errcap) snprintf(err,errcap,"refcount 位宽是 %u 位（只支持 16/32），不敢改",
                                 q->refcount_bits);
        return IMG_ENOTSUP;
    }

    const uint64_t cs = q->h.cluster_size;
    if(!cs) return IMG_ESTATE;
    const uint64_t part_start = part_lba * 512ULL;
    const uint64_t part_end   = part_start + part_bytes;

    out->host_cluster_bytes = cs;
    out->alloc_before = host_alloc_bytes(q->io);

    if(prog){
        img_prog_phase(prog, IMG_PH_FIX, "读取空闲位图");
        img_prog_op(prog, IMG_OP_SCAN);
    }

    /* ---------------- 读卷的空闲位图 ---------------- */
    uint64_t gcs = 0;               /* 客户机分配单元 */
    uint8_t *bits = NULL;           /* 1=空闲；域 = 分区内第 i 个簇 */
    uint64_t nbits = 0;
    const char *fsname = "?";

    /* NTFS 先试（probe 只看 BPB，不挂载）*/
#if IMG_WITH_NTFS3G
    if(img_ntfs_probe(q, part_lba, NULL) == IMG_OK){
        img_ntfs_fs *fs = img_ntfs_mount_rw(q, part_lba, part_bytes);
        if(!fs){
            if(err&&errcap) snprintf(err,errcap,"NTFS 挂载失败（卷可能被标记为脏）");
            return IMG_ESTATE;
        }
        if(img_ntfs_is_dirty(fs)){
            img_ntfs_umount(fs);
            if(err&&errcap) snprintf(err,errcap,"NTFS 卷是脏的（上次没正常关机）：位图不可信，先让系统正常启动一次");
            return IMG_ESTATE;
        }
        gcs = img_ntfs_cluster_size(fs);
        uint64_t n = img_ntfs_cluster_count(fs);
        nbits = part_bytes / (gcs ? gcs : 1);
        if(gcs && (part_bytes % gcs)) nbits++;          /* 尾部不满一簇也算一格 */
        bits = (uint8_t*)calloc(1, (size_t)((nbits + 7) / 8) + 1);
        if(!bits){ img_ntfs_umount(fs); return IMG_ENOMEM; }
        /* 默认全"已用"，只有 $Bitmap 说空闲的才置 1 —— 引导扇区/MFT 保留区
         * 不在 $Bitmap 覆盖范围，保持"已用"是保守正确的。*/
        int rc = img_ntfs_free_map(fs, bits, nbits);
        (void)n;
        img_ntfs_umount(fs);
        if(rc != IMG_OK){
            free(bits);
            if(err&&errcap) snprintf(err,errcap,"读 $Bitmap 失败（rc=%d）", rc);
            return rc;
        }
        fsname = "ntfs";
    } else
#endif
    if(img_fat32_probe(q, part_lba, NULL) == IMG_OK){
        img_fat32_fs *fs = img_fat32_mount(q, part_lba, false);
        if(!fs){
            if(err&&errcap) snprintf(err,errcap,"FAT32 挂载失败");
            return IMG_ESTATE;
        }
        if(img_fat32_is_dirty(fs)){
            img_fat32_unmount(fs);
            if(err&&errcap) snprintf(err,errcap,"FAT32 卷是脏的（上次没正常卸载）：位图不可信");
            return IMG_ESTATE;
        }
        gcs = img_fat32_cluster_bytes(fs);
        nbits = part_bytes / (gcs ? gcs : 1) + ((gcs && (part_bytes % gcs)) ? 1 : 0);
        bits = (uint8_t*)calloc(1, (size_t)((nbits + 7) / 8) + 1);
        if(!bits){ img_fat32_unmount(fs); return IMG_ENOMEM; }
        int rc = img_fat32_free_map(fs, bits, nbits);
        img_fat32_unmount(fs);
        if(rc != IMG_OK){
            free(bits);
            if(err&&errcap) snprintf(err,errcap,"读 FAT 表失败（rc=%d）", rc);
            return rc;
        }
        fsname = "fat32";
    } else {
        if(err&&errcap) snprintf(err,errcap,"这个分区既不是 NTFS 也不是 FAT32");
        return IMG_ENOTSUP;
    }

    snprintf(out->fs, sizeof out->fs, "%s", fsname);
    out->fs_cluster_bytes = gcs;
    if(!gcs || gcs == 0){ free(bits); return IMG_ESTATE; }
    if(gcs > cs){
        free(bits);
        if(err&&errcap) snprintf(err,errcap,"卷的簇（%llu 字节）比 qcow2 簇（%llu 字节）还大，判断不了整簇空闲",
                                 (unsigned long long)gcs, (unsigned long long)cs);
        return IMG_ENOTSUP;
    }
    const uint64_t per_host = cs / gcs;          /* 一个 qcow2 簇覆盖几个卷簇 */

    /* ---------------- 扫 L1/L2，挑出可释放的簇 ---------------- */
    if(prog){
        img_prog_phase(prog, IMG_PH_FIX, "扫描已分配簇");
        img_prog_op(prog, IMG_OP_SCAN);
        img_prog_count(prog, 0, q->h.l1_size);
    }
    u64v hosts; memset(&hosts, 0, sizeof hosts);
    int rc = IMG_OK;
    uint64_t l2_dirty_tables = 0;

    for(uint64_t l1i = 0; l1i < q->h.l1_size; l1i++){
        if(img_cancelled()){ rc = IMG_ECANCELLED; break; }
        if(prog) img_prog_count(prog, l1i, q->h.l1_size);

        uint64_t l2_addr = 0;
        uint8_t *l2 = NULL;
        if(qc2_l2_load(q, l1i, &l2_addr, &l2) != IMG_OK) continue;   /* 这一层没分配 */

        bool changed = false;
        for(uint64_t i = 0; i < q->h.l2_entries; i++){
            uint64_t e = rd_be64_i(l2 + i*8);
            uint64_t host = e & QCOW2_OFFSET_MASK;
            if(!host) continue;
            out->host_clusters_checked++;

            if(e & QCOW2_COMPRESSED_BIT){ out->compressed_skipped++; continue; }

            uint64_t guest = l1i * q->h.l1_span_bytes + i * cs;
            /* 只碰【完全落在分区内】的簇 */
            if(guest < part_start || guest + cs > part_end) continue;
            /* 所覆盖的卷簇必须全部空闲 */
            uint64_t base = (guest - part_start) / gcs;
            bool all_free = true;
            for(uint64_t k = 0; k < per_host; k++){
                uint64_t idx = base + k;
                if(idx >= nbits || !bit_get(bits, idx)){ all_free = false; break; }
            }
            if(!all_free) continue;
            /* 共享簇（快照/别名）一律不动 */
            uint32_t rc_n = 0;
            if(qc2_refcount_get(q, host / cs, &rc_n) != IMG_OK || rc_n != 1){
                out->shared_skipped++;
                continue;
            }
            out->clusters_freed++;
            if(dry) continue;
            wr_be64_i(l2 + i*8, 0);
            changed = true;
            if(u64v_push(&hosts, host) != IMG_OK){ rc = IMG_ENOMEM; break; }
        }
        if(changed){
            if((rc = qc2_l2_store(q, l2_addr, l2)) != IMG_OK){ free(l2); break; }
            l2_dirty_tables++;
        }
        free(l2);
        if(rc != IMG_OK) break;
    }
    free(bits);

    out->guest_bytes_freed = out->clusters_freed * cs;

    if(dry){                                  /* 一个字节都没改 */
        free(hosts.v);
        out->alloc_after = out->alloc_before;
        return IMG_OK;
    }
    if(rc != IMG_OK){ free(hosts.v); return rc; }

    /* ① L2 已落盘 → fsync（此后中断只会留下"泄漏簇"，不会丢数据）*/
    if(l2_dirty_tables && img_io_sync(q->io) != IMG_OK){ free(hosts.v); return IMG_EIO; }

    if(prog){
        img_prog_phase(prog, IMG_PH_FIX, "更新 refcount");
        img_prog_op(prog, IMG_OP_SYNC_INDEX);
        img_prog_count(prog, 0, hosts.n);
    }
    /* ② refcount 归零 */
    for(size_t i = 0; i < hosts.n; i++){
        if(img_cancelled()){ rc = IMG_ECANCELLED; break; }
        if(prog && (i & 0x3ff) == 0) img_prog_count(prog, i, hosts.n);
        if(qc2_refcount_set(q, hosts.v[i] / cs, 0) != IMG_OK){ rc = IMG_EIO; break; }
    }
    img_io_sync(q->io);
    if(rc != IMG_OK){ free(hosts.v); out->alloc_after = host_alloc_bytes(q->io); return rc; }

    /* ③ 打洞：相邻簇合并成一段，失败只计数（不写零）*/
    if(punch && hosts.n){
        if(prog){
            img_prog_phase(prog, IMG_PH_FIX, "回收宿主空间");
            img_prog_op(prog, IMG_OP_WRITE);
            img_prog_count(prog, 0, hosts.n);
        }
        size_t i = 0;
        while(i < hosts.n){
            uint64_t start = hosts.v[i];
            size_t j = i + 1;
            while(j < hosts.n && hosts.v[j] == hosts.v[j-1] + cs) j++;
            uint64_t len = (uint64_t)(j - i) * cs;
            int prc = img_io_punch_strict(q->io, start, len);
            if(prc == IMG_OK) out->host_bytes_punched += len;
            else              out->punch_failed += (uint64_t)(j - i);
            i = j;
        }
        img_io_sync(q->io);
    }
    free(hosts.v);
    out->alloc_after = host_alloc_bytes(q->io);
    (void)fsname;
    if(prog) img_prog_done(prog, "清理完成");
    return IMG_OK;
}