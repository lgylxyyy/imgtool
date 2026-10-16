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
        if(prog){ img_prog_phase(prog,IMG_PH_COPY,"克隆保留分区"); img_prog_bytes(prog,0,bytes); }
        uint64_t done=0;
        while(done<bytes && rc==IMG_OK){
            size_t n=(size_t)((bytes-done)<CLONE_CHUNK?(bytes-done):CLONE_CHUNK);
            rc=img_qcow2_read_full(sq, off+done, buf, n);
            if(rc==IMG_OK && memcmp(buf,zero,n)!=0){   /* ★ 全零块跳过（保持稀疏） */
                rc=img_qcow2_write(dq, off+done, buf, n);
                if(rc==IMG_OK) st.bytes_written += n;
            }
            done+=n;
            if(prog) img_prog_bytes(prog,done,bytes);
        }
        img_span_end(&sp,rc);
        if(rc==IMG_OK){ st.parts_copied++; if(prog) img_prog_done(prog,"分区克隆完成"); }
    }
    free(buf); free(zero);
    if(out && rc==IMG_OK) *out=st;

out_end:
    if(dq) img_qcow2_close(dq);
    if(dfi) img_io_close(dfi);
    img_qcow2_close(sq);
    img_io_close(sfi);
    return rc;
}
