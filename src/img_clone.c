/* img_clone.c —— 扇区级克隆（机制不是政策；政策在 App/命令层）
 *
 *   见 include/img_clone.h 的契约。核心顺序：
 *     源只读打开 → 目标建新盘（同虚拟大小）→ 写新 GPT（主+备+CRC，可换 GUID）
 *     → 逐个保留分区 1MiB 分块拷贝（全零块跳过 = 保持稀疏）
 */
#define _POSIX_C_SOURCE 200809L
#include "img_clone.h"
#include "img_io.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_event.h"
#include <stdlib.h>
#include <string.h>

#define CLONE_CHUNK (1024u*1024u)   /* 1MiB：与其它写入路径一致（大文件不整读进内存）*/

int img_clone(const char *src_path, const char *dst_path,
              const uint64_t *keep_lbas, int nkeep,
              bool guid_random,
              bool compress,
              img_clone_ctype_t ctype,
              img_clone_stats_t *out, img_prog *prog){
    if(!src_path||!dst_path||!keep_lbas||nkeep<=0) return IMG_EINVAL;
    img_clone_stats_t st; memset(&st,0,sizeof st);
    int rc=IMG_OK;
    img_io *dfi=NULL; img_qcow2 *dq=NULL;

    /* ① 源：只读打开（★ 源全程不写是这个方案最大的优点） */
    img_io_opts_t sio={0};
    img_io *sfi=img_io_open_path(src_path,&sio);
    if(!sfi) return IMG_ENOENT;
    img_qcow2_opts_t qo={0};
    img_qcow2 *sq=img_qcow2_open(sfi,&qo);
    if(!sq){ img_io_close(sfi); return IMG_EFORMAT; }
    uint64_t vsize=img_qcow2_size(sq);
    img_parttable_t stab;
    rc=img_parttable_read(sq,&stab,0);
    if(rc!=IMG_OK) goto out_end;

    /* ② 目标：建新盘（虚拟大小 = 源盘；分区表原样保留） */
    img_io_opts_t dio={0}; dio.writable=true;
    dfi=img_io_open_path(dst_path,&dio);
    if(!dfi){ rc=IMG_EIO; goto out_end; }
    img_qcow2_create_opts_t co={0};
    co.size=vsize; co.cluster_bits=16;          /* 64KiB 簇：与本工具所有镜像一致 */
    /* 目标盘压缩算法：显式指定优先，AUTO 跟源盘（zstd 源 → zstd 盘，不降级）。
     * 本构建没有 libzstd 时 create 对 ZSTD 直接返回 ENOTSUP —— 造不出自己读不了的盘。 */
    {
        uint8_t src_ct=(uint8_t)img_qcow2_header(sq)->compression_type;
        switch(ctype){
            case IMG_CLONE_CTYPE_ZSTD: co.compression_type=IMG_QCOW2_COMP_ZSTD; break;
            case IMG_CLONE_CTYPE_ZLIB: co.compression_type=IMG_QCOW2_COMP_ZLIB; break;
            default:                   co.compression_type=src_ct;              break;
        }
    }
    rc=img_qcow2_create(dfi,&co);
    img_io_close(dfi); dfi=NULL;
    if(rc!=IMG_OK) goto out_end;
    img_io_opts_t dio2={0}; dio2.writable=true;
    dfi=img_io_open_path(dst_path,&dio2);
    if(!dfi){ rc=IMG_EIO; goto out_end; }
    dq=img_qcow2_open(dfi,&qo);
    if(!dq||img_qcow2_write_init(dq)!=IMG_OK){ rc=IMG_EIO; goto out_end; }

    /* ③ 新 GPT：结构照抄源表；可选重新生成磁盘/分区 GUID。
     *    img_parttable_write 负责主表+备份表+CRC（结构类铁律第一条）。 */
    img_parttable_t dt=stab;
    if(guid_random){
        img_guid_new(dt.disk_guid);
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(dt.parts[i].used) img_guid_new(dt.parts[i].part_guid);
    }
    rc=img_parttable_write(dq,&dt);
    if(rc!=IMG_OK) goto out_end;

    /* ④ 拷贝保留分区（1MiB 分块；全零块跳过 → 保持稀疏） */
    /* ★ REQ-79：整体进度按【所有保留分区】的累计字节算。
     *   以前每个分区各自报 0..bytes、且中途调 img_prog_done 直接报 100%，
     *   App 的圆盘在第二个分区开始就被 do_emit 的「阶段内不倒退」钳死在 100%
     *   （5.3 GB 压缩跑 20 分钟、界面一直 100% 就是这个）。
     *   现在 total 固定 = keep 分区字节和，进度用 base+done 单调推进。 */
    uint64_t total_all = 0;
    for(int k=0;k<nkeep;k++){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(stab.parts[i].used && stab.parts[i].first_lba==keep_lbas[k]){
                total_all += (stab.parts[i].last_lba - stab.parts[i].first_lba + 1)*512u;
                break;
            }
    }
    uint64_t base = 0;                            /* 前面分区已完成的字节累计 */
    if(prog){
        img_prog_phase(prog,IMG_PH_COPY,"克隆保留分区");
        img_prog_dst(prog, dst_path);   /* ★ REQ-79：dst_item 报【输出】文件，不是源 */
        img_prog_bytes(prog, 0, total_all);
    }
    unsigned char *buf = (unsigned char*)malloc(CLONE_CHUNK);
    unsigned char *zero= (unsigned char*)calloc(1,CLONE_CHUNK);
    if(!buf||!zero){ free(buf); free(zero); rc=IMG_ENOMEM; goto out_end; }
    for(int k=0; k<nkeep && rc==IMG_OK; k++){
        uint64_t lba=keep_lbas[k];
        img_partition_t *pp=NULL;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(stab.parts[i].used && stab.parts[i].first_lba==lba){ pp=&stab.parts[i]; break; }
        if(!pp){ rc=IMG_EINVAL; break; }         /* 名单里的 LBA 不在源分区表 */
        uint64_t off=pp->first_lba*512u;
        uint64_t bytes=(pp->last_lba - pp->first_lba + 1)*512u;
        img_span sp;
        img_span_prep(&sp,"clone","copy","克隆保留分区");
        img_span_kv_u64(&sp,"lba",lba);
        img_span_kv_u64(&sp,"size_bytes",bytes);
        img_span_begin(&sp);
        uint64_t done=0;
        /* ★ 2026-10-25（img.compact）：按【簇】粒度跳读 ——
         *   先问源盘这段有没有已分配的簇（没有 → 读出来必然是零，连读都省了），
         *   再读、再判全零。只按 1 MiB 判零会把一个元数据簇带成 1 MiB 的分配，
         *   另存出来的文件反而比源盘还大（实测 77 MB → 80 MB）。*/
        uint64_t unit=img_qcow2_header(sq)->cluster_size;
        if(unit<4096) unit=4096;
        if(unit>CLONE_CHUNK) unit=CLONE_CHUNK;
        /* ★ T6：压缩簇必须是【目标】的整簇。源是 raw 时 unit 只有 4 KiB，
         *   按 4 KiB 写根本压不了；所以这里改成按目标簇大小读写。*/
        uint64_t dcs = img_qcow2_header(dq)->cluster_size;
        if(compress && dcs >= 4096 && dcs <= CLONE_CHUNK) unit = dcs;
        while(done<bytes && rc==IMG_OK){
            size_t n=(size_t)((bytes-done)<unit?(bytes-done):unit);
            bool any=false;
            rc=img_qcow2_any_allocated(sq, off+done, n, &any);
            if(rc==IMG_OK && any){
                rc=img_qcow2_read_full(sq, off+done, buf, n);
                if(rc==IMG_OK && memcmp(buf,zero,n)!=0){   /* ★ 全零块跳过（保持稀疏） */
                    if(compress && n==dcs){
                        int wrc=img_qcow2_write_compressed(dq, off+done, buf, n);
                        if(wrc==IMG_OK){ st.clusters_compressed++; }
                        else if(wrc==IMG_ENOTSUP) rc=img_qcow2_write(dq, off+done, buf, n);
                        else rc=wrc;
                    } else {
                        rc=img_qcow2_write(dq, off+done, buf, n);
                    }
                    if(rc==IMG_OK){ st.bytes_written += n; }
                }
            }
            done+=n;
            if(prog) img_prog_bytes(prog,base+done,total_all);
        }
        img_span_end(&sp,rc);
        if(rc==IMG_OK) st.parts_copied++;
        base += bytes;                            /* 下一个分区的累计起点 */
    }
    free(buf); free(zero);
    /* 整个克隆结束才报 100%（各分区中间不再报，否则圆盘提前冲到 100） */
    if(prog && rc==IMG_OK) img_prog_done(prog,"克隆完成");
    if(out && rc==IMG_OK) *out=st;

out_end:
    if(dq) img_qcow2_close(dq);
    if(dfi) img_io_close(dfi);
    img_qcow2_close(sq);
    img_io_close(sfi);
    return rc;
}
