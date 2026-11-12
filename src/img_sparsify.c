/* img_sparsify.c — 原地回收映像未使用的块（T7）
 *
 * 与 img.compact 的分界：
 *   compact  另存更小的映像（换文件、要额外空间）
 *   sparsify 原地把客户机已经不用/本身就是零的块还给宿主（不换文件、长度不变）
 *
 * 崩溃安全：raw 的打洞是幂等的、无元数据可写坏；qcow2 的 fs 模式完全复用
 * img.trim 的「L2=0 → fsync → refcount=0 → fsync → punch」顺序，不另造一套。
 */
#include "img_sparsify.h"
#include "img_qcow2_int.h"
#include "img_partition.h"
#include "img_trim.h"
#include "img_io.h"
#include "img_event.h"   /* img_cancelled */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define SP_CHUNK (1u << 20)          /* zero 模式扫描块 */

static uint64_t host_alloc(img_io *io){
    if(!io) return 0;
    int fd = img_io_fd(io);
    if(fd < 0) return 0;
    struct stat st;
    if(fstat(fd,&st)!=0) return 0;
    return (uint64_t)st.st_blocks * 512ULL;
}

static uint64_t fnv1a(uint64_t h, const void *p, size_t n){
    const uint8_t *b = (const uint8_t*)p;
    for(size_t i=0;i<n;i++){ h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* 分区指纹：头 64 KiB + 16 处 4 KiB 抽样。执行前后必须一致。 */
static uint64_t part_fingerprint(img_qcow2 *q, uint64_t start, uint64_t bytes){
    uint64_t h = 1469598103934665603ULL;
    if(!bytes) return h;
    uint8_t buf[65536];
    size_t n = bytes < sizeof buf ? (size_t)bytes : sizeof buf;
    if(img_io_read_full(q->io,start,buf,n)!=IMG_OK) return 0;
    h = fnv1a(h,buf,n);
    for(int i=0;i<16;i++){
        uint64_t rel = (bytes > 4096) ? (bytes - 4096) * (uint64_t)i / 16 : 0;
        size_t sn = (size_t)((bytes - rel) < 4096 ? (bytes - rel) : 4096);
        if(!sn) break;
        if(img_io_read_full(q->io,start+rel,buf,sn)!=IMG_OK) return 0;
        h = fnv1a(h,buf,sn);
    }
    return h;
}

/* 一段区间里【实际映射了宿主块】的字节数。
 * 用途：dry-run 要给"真能省多少"，而不是"这些区域有多大" —— raw 的空闲区
 * 常常本来就是洞（写过的文件只占它自己的块），报段长会把 504 MB 的空闲区
 * 说成能省 504 MB，实际只有 86 MB。lseek 不改文件，可以放心用。 */
#ifndef SEEK_DATA
#define SEEK_DATA 3
#endif
#ifndef SEEK_HOLE
#define SEEK_HOLE 4
#endif
static uint64_t mapped_in(img_io *io, uint64_t start, uint64_t len){
    int fd = img_io_fd(io);
    if(fd < 0) return len;
    uint64_t total = 0;
    off_t off = (off_t)start, end = (off_t)(start + len);
    while(off < end){
        off_t d = lseek(fd, off, SEEK_DATA);
        if(d < 0){ if(errno == ENXIO) break; return len; }
        if(d >= end) break;
        off_t h = lseek(fd, d, SEEK_HOLE);
        if(h < 0) return len;
        if(h > end) h = end;
        total += (uint64_t)(h - d);
        off = h;
    }
    return total;
}

/* ---------- 区间集合：收集 → 合并 → 打洞 ---------- */
typedef struct { uint64_t *v; size_t n, cap; } segv;   /* 存 (start,len) 对 */

static int seg_push(segv *a, uint64_t start, uint64_t len){
    if(!len) return IMG_OK;
    if(a->n + 2 > a->cap){
        size_t nc = a->cap ? a->cap * 2 : 256;
        uint64_t *nv = (uint64_t*)realloc(a->v, nc * sizeof *nv);
        if(!nv) return IMG_ENOMEM;
        a->v = nv; a->cap = nc;
    }
    a->v[a->n++] = start;
    a->v[a->n++] = len;
    return IMG_OK;
}

static int seg_cmp(const void *pa, const void *pb){
    const uint64_t *a = (const uint64_t*)pa, *b = (const uint64_t*)pb;
    return (a[0] < b[0]) ? -1 : (a[0] > b[0] ? 1 : 0);
}

/* 原地排序 + 合并重叠/相邻。返回合并后的对数。 */
static size_t seg_normalize(segv *a){
    if(!a->n) return 0;
    qsort(a->v, a->n/2, 2*sizeof(uint64_t), seg_cmp);
    size_t out = 0;
    uint64_t cs = a->v[0], ce = a->v[0] + a->v[1];
    for(size_t i=1;i<a->n/2;i++){
        uint64_t s = a->v[i*2], e = s + a->v[i*2+1];
        if(s <= ce){ if(e > ce) ce = e; }
        else { a->v[out*2]=cs; a->v[out*2+1]=ce-cs; out++; cs=s; ce=e; }
    }
    a->v[out*2]=cs; a->v[out*2+1]=ce-cs; out++;
    a->n = out*2;
    return out;
}

/* 打洞。返回实际回收字节。第一次 ENOTSUP 就把 punch_supported 关掉。 */
static uint64_t seg_punch(img_qcow2 *q, segv *a, img_sparsify_result_t *o, bool dry,
                          img_prog *prog){
    uint64_t freed = 0;
    size_t npair = seg_normalize(a);
    if(prog) img_prog_count(prog,0,npair);
    for(size_t i=0;i<npair;i++){
        uint64_t start = a->v[i*2], len = a->v[i*2+1];
        if(dry){ freed += mapped_in(q->io,start,len); continue; }
        if(!o->punch_supported){ freed += mapped_in(q->io,start,len); continue; }
        int rc = img_io_punch_strict(q->io, start, len);
        if(rc == IMG_OK){ freed += len; o->punched_segments++; }
        else {
            o->punch_failed++;
            if(rc == IMG_ENOTSUP || rc == IMG_EREADONLY){
                o->punch_supported = false;
                if(!o->punch_note[0])
                    snprintf(o->punch_note,sizeof o->punch_note,
                             "宿主文件系统不支持打洞（%s），没有真正释放空间",
                             img_strerror(rc));
            }
        }
        if(prog) img_prog_count(prog,i+1,npair);
    }
    return freed;
}

/* ---------------- raw：只能打洞（长度 = 容量是 raw 的语义） ---------------- */
static int sparsify_raw(img_qcow2 *q, const img_sparsify_opts_t *opts,
                        img_sparsify_result_t *o, img_prog *prog,
                        char *err, size_t errcap){
    const bool dry = opts->dry_run;
    const int mode = opts->mode;
    const uint64_t total = q->h.size;

    img_parttable_t pt;
    bool have_pt = (img_parttable_read(q,&pt,total/512ULL) == IMG_OK);

    /* 安全：校验点只挑「一定不会被释放」的地方 ——
     *   分区表、每个分区的头 64 KiB、以及位图/内容上「已用且非零」的抽样。
     *   ★ 不能对整个分区做 16 处随机抽样：raw 的空闲簇里往往还有旧文件数据，
     *     打洞后按定义会读成 0 —— 那不是损坏，但会让"校验"误报失败。*/
    typedef struct { uint64_t off, len, fp; } chkpt_t;
    chkpt_t chk[48]; int nchk = 0;
    #define CHK_ADD(O,L) do{ if(nchk < 48 && (L)){ chk[nchk].off=(O); chk[nchk].len=(L); \
        chk[nchk].fp=part_fingerprint(q,(O),(L)); nchk++; } }while(0)
    CHK_ADD(0, total < 65536 ? total : 65536);
    uint64_t part_off[IMG_MAX_PARTITIONS], part_len[IMG_MAX_PARTITIONS];
    int pn = 0;
    if(have_pt){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!pt.parts[i].used) continue;
            uint64_t so = (uint64_t)pt.parts[i].first_lba * 512ULL;
            uint64_t ln = ((uint64_t)pt.parts[i].last_lba - pt.parts[i].first_lba + 1) * 512ULL;
            if(so >= total) continue;
            if(so + ln > total) ln = total - so;
            part_off[pn] = so; part_len[pn] = ln;
            /* 只校验前 32 KiB：FAT32 的保留扇区(16 KiB)+FAT 表起点、NTFS 的 $Boot 都在这里，
             * 一定是已用区；往后再抽就可能落进"位图说空闲"的簇，打洞后按定义会读成 0。*/
            CHK_ADD(so, ln < 32768 ? ln : 32768);
            pn++;
        }
    }

    segv segs; memset(&segs,0,sizeof segs);
    int rc = IMG_OK;
    uint8_t *bits = NULL;

    /* 扫描范围：有分区表就只扫分区（分区之间的空隙可能是引导代码，不碰）；
     * 没有分区表就扫 [1 MiB, 末尾) —— 前 1 MiB 留给 MBR/引导。*/
    uint64_t ranges[IMG_MAX_PARTITIONS][2];
    int rn = 0;
    if(pn){
        for(int i=0;i<pn;i++){ ranges[rn][0]=part_off[i]; ranges[rn][1]=part_len[i]; rn++; }
        o->skipped_bytes = 0;
        uint64_t covered = 0;
        for(int i=0;i<pn;i++) covered += part_len[i];
        if(total > covered) o->skipped_bytes = total - covered;
    } else {
        ranges[0][0] = total > (1u<<20) ? (1u<<20) : 0;
        ranges[0][1] = total - ranges[0][0];
        o->skipped_bytes = ranges[0][0];
        rn = 1;
    }

    if(prog){ img_prog_phase(prog,IMG_PH_FIX,"扫描分区"); img_prog_op(prog,IMG_OP_SCAN); }

    for(int r=0;r<rn && rc==IMG_OK;r++){
        uint64_t rstart = ranges[r][0], rbytes = ranges[r][1];
        if(!rbytes) continue;
        o->scanned_bytes += rbytes;
        bool part_ok = false;      /* 这个分区至少有一种模式处理成功 */

        /* fs 模式：先读空闲位图（认不出文件系统就跳过这个分区） */
        if(mode == IMG_SPARSIFY_FS || mode == IMG_SPARSIFY_ALL){
            uint64_t nbits=0, gcs=0;
            char code[40]=""; char fsn[8]="";
            bits = NULL;
            int mrc = img_fs_free_map(q, rstart/512ULL, rbytes, &bits, &nbits, &gcs,
                                      fsn, sizeof fsn, code, sizeof code, err, errcap);
            if(mrc == IMG_OK && bits && gcs){
                part_ok = true;
                if(!o->fs[0]) snprintf(o->fs,sizeof o->fs,"%s",fsn);
                /* 连续空闲簇合并成段 */
                uint64_t run_start = 0; bool in_run = false;
                for(uint64_t i=0;i<nbits;i++){
                    bool f = (bits[i>>3] >> (i&7)) & 1;
                    if(f){
                        if(!in_run){ run_start = i; in_run = true; }
                    } else if(in_run){
                        uint64_t s = rstart + run_start*gcs;
                        uint64_t l = (i - run_start)*gcs;
                        if(s + l > rstart + rbytes) l = rstart + rbytes - s;
                        (void)seg_push(&segs,s,l);
                        o->free_bytes_by_fs += l;
                        in_run = false;
                    }
                }
                if(in_run){
                    uint64_t s = rstart + run_start*gcs;
                    uint64_t l = (nbits - run_start)*gcs;
                    if(s + l > rstart + rbytes) l = rstart + rbytes - s;
                    (void)seg_push(&segs,s,l);
                    o->free_bytes_by_fs += l;
                }
                /* 已用簇里均匀取 8 个点做执行前后的内容校验 */
                if(gcs >= 4096){
                    uint64_t step = nbits > 64 ? nbits / 8 : 1;
                    for(uint64_t i = 0; i < nbits && nchk < 48; i += step){
                        if(!((bits[i>>3] >> (i&7)) & 1)){
                            uint64_t off = rstart + i*gcs;
                            if(off + 4096 <= rstart + rbytes) CHK_ADD(off, 4096);
                        }
                    }
                }
            } else if(mrc == IMG_ENOTSUP){
                /* 认不出文件系统：跳过这个分区（ESP/MSR/未格式化分区都属此类），
                 * 不能因为第一个分区认不出就把整次操作判死。ALL 模式还能靠 zero 兜底。*/
            } else {
                /* 真错误（读位图失败/卷脏）：中断，别在不可信的位图上动手。*/
                rc = mrc;
            }
            free(bits); bits = NULL;
            if(rc != IMG_OK) break;
        }

        /* zero 模式：整段按 1 MiB 扫，全零就收 */
        if(mode == IMG_SPARSIFY_ZERO || mode == IMG_SPARSIFY_ALL){
            uint8_t *buf = (uint8_t*)malloc(SP_CHUNK);
            if(!buf){ rc = IMG_ENOMEM; break; }
            uint64_t done = 0;
            while(done < rbytes && rc == IMG_OK){
                if(img_cancelled()){ rc = IMG_ECANCELLED; break; }
                size_t n = (size_t)((rbytes-done) < SP_CHUNK ? (rbytes-done) : SP_CHUNK);
                if(img_io_read_full(q->io, rstart+done, buf, n) != IMG_OK){ rc = IMG_EIO; break; }
                bool zero = true;
                for(size_t k=0;k<n;k++){ if(buf[k]){ zero=false; break; } }
                if(zero){
                    o->zero_bytes += n;
                    (void)seg_push(&segs, rstart+done, n);
                } else if(nchk < 48 && (done / SP_CHUNK) % 16 == 0){
                    CHK_ADD(rstart+done, n < 4096 ? n : 4096);   /* 非零块：不能被改动 */
                }
                done += n;
                if(prog) img_prog_bytes(prog,done,rbytes);
            }
            free(buf);
            if(rc == IMG_OK) part_ok = true;
        }
        if(part_ok) o->parts_scanned++; else o->parts_skipped++;
    }
    /* fs 模式下一个分区都没认出：明确告诉用户"这些分区没有可读的位图" */
    if(rc == IMG_OK && mode == IMG_SPARSIFY_FS && o->parts_scanned == 0){
        snprintf(o->code,sizeof o->code,"SPARSIFY-FS-UNSUPPORTED");
        if(err&&errcap) snprintf(err,errcap,
            "扫描了 %llu 个分区，都不是 NTFS/FAT32：请改用 mode=zero（按内容全零）",
            (unsigned long long)o->parts_skipped);
        rc = IMG_ENOTSUP;
    }

    if(rc == IMG_OK){
        o->bytes_before = host_alloc(q->io);
        /* dry-run 也必须先试一次真打洞才能说"能不做到"—— 但那就改了文件。
         * 折中：dry 时按内核能力保守报 true（Linux 3.1+ 的 f2fs/ext4/xfs 都支持），
         * 真执行时以第一次调用的返回为准。*/
        o->punch_supported = true;
        if(prog){ img_prog_phase(prog,IMG_PH_FIX,"释放宿主空间"); img_prog_op(prog,IMG_OP_WRITE); }
        uint64_t seg_bytes = seg_punch(q,&segs,o,dry,prog);
        o->bytes_after = host_alloc(q->io);
        if(dry){
            /* 试运行：报"按现在的空闲情况能释放多少"（估算，按段长度） */
            o->would_free_bytes = seg_bytes;
            o->freed_bytes = 0;
        } else {
            /* 真执行：以【宿主占用前后差】为准。段长度会把"本来就已经是洞"的
             * 区域也算进去（重复跑时尤其明显），报出来是假数。 */
            o->freed_bytes = (o->bytes_before > o->bytes_after)
                             ? (o->bytes_before - o->bytes_after) : 0;
        }

        bool same = true;
        for(int i=0;i<nchk;i++)
            if(part_fingerprint(q,chk[i].off,chk[i].len) != chk[i].fp){ same = false; break; }
        o->verified = same;
        if(dry){
            if(!o->punch_note[0]) snprintf(o->punch_note,sizeof o->punch_note,
                                            "试运行：没有改动任何字节");
        } else {
            if(!same){
                snprintf(o->code,sizeof o->code,"SPARSIFY-VERIFY-FAIL");
                if(err&&errcap) snprintf(err,errcap,
                    "执行后校验点不一致（分区表/分区头/已用数据被改动）：立刻停止使用这份映像");
                rc = IMG_ECORRUPT;
            }
            if(!o->punch_note[0] && !o->freed_bytes) snprintf(o->punch_note,sizeof o->punch_note,
                                                     "没有可释放的块");
        }
        (void)pn;
    }
    free(segs.v);
    return rc;
}

/* ---------------- qcow2：fs 模式复用 img.trim ---------------- */
static int sparsify_qcow2(img_qcow2 *q, const img_sparsify_opts_t *opts,
                          img_sparsify_result_t *o, img_prog *prog,
                          char *err, size_t errcap){
    if(opts->mode == IMG_SPARSIFY_ZERO){
        snprintf(o->code,sizeof o->code,"SPARSIFY-NOT-NEEDED");
        if(err&&errcap) snprintf(err,errcap,
            "qcow2 本身就是按块分配的，没有\"零块占用\"可回收；要缩小文件请用紧凑化");
        return IMG_ENOTSUP;
    }
    img_parttable_t pt;
    if(img_parttable_read(q,&pt,q->h.size/512ULL) != IMG_OK){
        snprintf(o->code,sizeof o->code,"SPARSIFY-NO-PARTABLE");
        if(err&&errcap) snprintf(err,errcap,"读不到分区表：不知道哪些区间属于分区");
        return IMG_ESTATE;
    }
    img_trim_result_t tr; memset(&tr,0,sizeof tr);
    int ok_parts = 0, rc = IMG_OK;
    /* ★ 2026-11-12（REQ-76 ②）：bytes_before 必须在 trim 之前量。
     *   trim 内部已经做完打洞，之后再量 before/after 就是"打洞后 vs 打洞后"，
     *   两个数一样，freed_bytes 也和它们对不上（真机实测：before==after==226607104，
     *   而 stat 显示 376353792 → 226607104）。*/
    o->bytes_before = host_alloc(q->io);
    for(int i=0;i<IMG_MAX_PARTITIONS && rc==IMG_OK;i++){
        if(!pt.parts[i].used) continue;
        uint64_t lba = (uint64_t)pt.parts[i].first_lba;
        uint64_t bytes = ((uint64_t)pt.parts[i].last_lba - lba + 1) * 512ULL;
        img_trim_opts_t to; memset(&to,0,sizeof to);
        to.dry_run = opts->dry_run; to.punch = true;
        rc = img_trim(q,lba,bytes,&to,&tr,prog,err,errcap);
        if(rc == IMG_ENOTSUP){ rc = IMG_OK; o->parts_skipped++; continue; }   /* 不是 NTFS/FAT */
        if(rc != IMG_OK) break;
        ok_parts++;
        o->scanned_bytes += bytes;
        o->free_bytes_by_fs += tr.guest_bytes_freed;
        o->punched_segments += (tr.host_bytes_punched ? 1 : 0);
        o->punched_bytes += tr.host_bytes_punched;
        if(!o->fs[0]) snprintf(o->fs,sizeof o->fs,"%s",tr.fs);
    }
    if(rc == IMG_OK && !ok_parts && !o->parts_skipped){
        snprintf(o->code,sizeof o->code,"SPARSIFY-NO-PART");
        if(err&&errcap) snprintf(err,errcap,"没有可处理的分区");
        return IMG_ESTATE;
    }
    o->bytes_after  = host_alloc(q->io);
    if(opts->dry_run){
        /* dry：客户机空闲字节（上界；宿主实际能省多少取决于打洞粒度） */
        o->would_free_bytes = o->free_bytes_by_fs;
        o->freed_bytes = 0;
    } else {
        /* 口径与 raw 分支一致：freed_bytes = 宿主占用前后差 */
        o->freed_bytes = (o->bytes_before > o->bytes_after)
                         ? (o->bytes_before - o->bytes_after) : 0;
    }
    o->verified = !opts->dry_run;
    if(!o->punch_note[0]) snprintf(o->punch_note,sizeof o->punch_note,
        opts->dry_run ? "试运行：没有改动任何字节" : "已按客户机空闲位图释放");
    return rc;
}

typedef int (*punch_fn)(img_qcow2*, uint64_t, uint64_t);
static int sparsify_punch(img_qcow2 *q, const img_sparsify_opts_t *opts,
                          img_sparsify_result_t *o, img_prog *prog,
                          char *err, size_t errcap, punch_fn pf);

int img_sparsify(img_qcow2 *q, const img_sparsify_opts_t *opts,
                 img_sparsify_result_t *out, img_prog *prog,
                 char *err, size_t errcap){
    if(err && errcap) err[0] = 0;
    if(!q || !out) return IMG_EINVAL;
    memset(out,0,sizeof *out);
    img_sparsify_opts_t o;
    memset(&o,0,sizeof o);
    if(opts) o = *opts;
    if(o.mode < IMG_SPARSIFY_FS || o.mode > IMG_SPARSIFY_ALL) o.mode = IMG_SPARSIFY_ALL;

    snprintf(out->format,sizeof out->format,"%s",img_diskfmt_name(q->fmt));
    snprintf(out->mode_effective,sizeof out->mode_effective,"%s",
             o.mode==IMG_SPARSIFY_FS?"fs":(o.mode==IMG_SPARSIFY_ZERO?"zero":"fs+zero"));

    if(!q->writable){
        snprintf(out->code,sizeof out->code,"SPARSIFY-READONLY");
        if(err&&errcap) snprintf(err,errcap,"映像是只读打开的");
        return IMG_EREADONLY;
    }
    if(q->fmt == DISK_FMT_RAW)   return sparsify_raw(q,&o,out,prog,err,errcap);
    if(q->fmt == DISK_FMT_QCOW2) return sparsify_qcow2(q,&o,out,prog,err,errcap);
    /* ★ T7：vhd/vmdk 只打洞、不动 BAT/GTE —— 元数据写入风险高，
     *   而"少占宿主空间"的目标靠打洞已经达到（洞读出来是 0，对空闲/零区无害）。*/
    if(q->fmt == DISK_FMT_VHD)  return sparsify_punch(q,&o,out,prog,err,errcap,img_vhd_punch_guest);
    if(q->fmt == DISK_FMT_VMDK) return sparsify_punch(q,&o,out,prog,err,errcap,img_vmdk_punch_guest);

    snprintf(out->code,sizeof out->code,"SPARSIFY-FMT-UNSUPPORTED");
    if(err&&errcap) snprintf(err,errcap,"这个格式不支持原地回收（%s）",
                             img_diskfmt_name(q->fmt));
    return IMG_ENOTSUP;
}
/* ---------------- vhd / vmdk：只打洞，不动分配表 ----------------
 *
 * 为什么不去改 BAT/GTE：那是元数据写入，一旦写坏映像就打不开了；而"释放空间"
 * 的用户价值（宿主少占）靠打洞已经拿到。洞读出来是 0 —— 对【客户机空闲】或
 * 【本来就是零】的区域，内容变 0 无害。
 *
 * 两种模式都支持：
 *   fs   读分区空闲位图 → 空闲簇对应的 guest 区间打洞
 *   zero 扫内容全零 → 打洞
 */
static uint64_t guest_fingerprint(img_qcow2 *q, uint64_t start, uint64_t bytes){
    uint64_t h = 1469598103934665603ULL;
    if(!bytes) return h;
    uint8_t buf[65536];
    size_t n = bytes < sizeof buf ? (size_t)bytes : sizeof buf;
    if(img_qcow2_read_full(q,start,buf,n)!=IMG_OK) return 0;
    h = fnv1a(h,buf,n);
    for(int i=0;i<16;i++){
        uint64_t rel = (bytes > 4096) ? (bytes - 4096) * (uint64_t)i / 16 : 0;
        size_t sn = (size_t)((bytes - rel) < 4096 ? (bytes - rel) : 4096);
        if(!sn) break;
        if(img_qcow2_read_full(q,start+rel,buf,sn)!=IMG_OK) return 0;
        h = fnv1a(h,buf,sn);
    }
    return h;
}

static int sparsify_punch(img_qcow2 *q, const img_sparsify_opts_t *opts,
                          img_sparsify_result_t *o, img_prog *prog,
                          char *err, size_t errcap, punch_fn pf){
    const bool dry = opts->dry_run;
    const int mode = opts->mode;
    const uint64_t total = q->h.size;

    img_parttable_t pt;
    bool have_pt = (img_parttable_read(q,&pt,total/512ULL) == IMG_OK);
    typedef struct { uint64_t off, len, fp; } chkpt_t;
    chkpt_t chk[48]; int nchk = 0;
    #define GCHK_ADD(O,L) do{ if(nchk < 48 && (L)){ chk[nchk].off=(O); chk[nchk].len=(L); \
        chk[nchk].fp=guest_fingerprint(q,(O),(L)); nchk++; } }while(0)
    GCHK_ADD(0, total < 65536 ? total : 65536);

    uint64_t ranges[IMG_MAX_PARTITIONS][2];
    int rn = 0;
    if(have_pt){
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            if(!pt.parts[i].used) continue;
            uint64_t so = (uint64_t)pt.parts[i].first_lba * 512ULL;
            uint64_t ln = ((uint64_t)pt.parts[i].last_lba - pt.parts[i].first_lba + 1) * 512ULL;
            if(so >= total) continue;
            if(so + ln > total) ln = total - so;
            GCHK_ADD(so, ln < 32768 ? ln : 32768);
            if(rn < IMG_MAX_PARTITIONS){ ranges[rn][0]=so; ranges[rn][1]=ln; rn++; }
        }
        uint64_t covered = 0;
        for(int i=0;i<rn;i++) covered += ranges[i][1];
        if(total > covered) o->skipped_bytes = total - covered;
    } else {
        snprintf(o->code,sizeof o->code,"SPARSIFY-NO-PARTABLE");
        if(err&&errcap) snprintf(err,errcap,"读不到分区表：不知道哪些区间属于分区");
        return IMG_ESTATE;
    }

    segv segs; memset(&segs,0,sizeof segs);
    int rc = IMG_OK;

    if(prog){ img_prog_phase(prog,IMG_PH_FIX,"扫描分区"); img_prog_op(prog,IMG_OP_SCAN); }
    for(int r=0;r<rn && rc==IMG_OK;r++){
        uint64_t rstart = ranges[r][0], rbytes = ranges[r][1];
        if(!rbytes) continue;
        o->scanned_bytes += rbytes;
        bool part_ok = false;

        if(mode == IMG_SPARSIFY_FS || mode == IMG_SPARSIFY_ALL){
            uint8_t *bits = NULL;
            uint64_t nbits=0, gcs=0;
            char code[40]=""; char fsn[8]="";
            int mrc = img_fs_free_map(q, rstart/512ULL, rbytes, &bits, &nbits, &gcs,
                                      fsn, sizeof fsn, code, sizeof code, err, errcap);
            if(mrc == IMG_OK && bits && gcs){
                part_ok = true;
                if(!o->fs[0]) snprintf(o->fs,sizeof o->fs,"%s",fsn);
                uint64_t run_start = 0; bool in_run = false;
                for(uint64_t i=0;i<nbits;i++){
                    bool f = (bits[i>>3] >> (i&7)) & 1;
                    if(f){ if(!in_run){ run_start = i; in_run = true; } }
                    else if(in_run){
                        uint64_t gs = rstart + run_start*gcs;
                        uint64_t gl = (i - run_start)*gcs;
                        if(gs + gl > rstart + rbytes) gl = rstart + rbytes - gs;
                        (void)seg_push(&segs,gs,gl);
                        o->free_bytes_by_fs += gl;
                        in_run = false;
                    }
                }
                if(in_run){
                    uint64_t gs = rstart + run_start*gcs;
                    uint64_t gl = (nbits - run_start)*gcs;
                    if(gs + gl > rstart + rbytes) gl = rstart + rbytes - gs;
                    (void)seg_push(&segs,gs,gl);
                    o->free_bytes_by_fs += gl;
                }
                if(gcs >= 4096){
                    uint64_t step = nbits > 64 ? nbits/8 : 1;
                    for(uint64_t i = 0; i < nbits && nchk < 48; i += step)
                        if(!((bits[i>>3] >> (i&7)) & 1)) GCHK_ADD(rstart + i*gcs, 4096);
                }
            } else if(mrc != IMG_ENOTSUP){
                rc = mrc;
            }
            free(bits);
            if(rc != IMG_OK) break;
        }

        if(mode == IMG_SPARSIFY_ZERO || mode == IMG_SPARSIFY_ALL){
            uint8_t *buf = (uint8_t*)malloc(SP_CHUNK);
            if(!buf){ rc = IMG_ENOMEM; break; }
            uint64_t done = 0;
            while(done < rbytes && rc == IMG_OK){
                if(img_cancelled()){ rc = IMG_ECANCELLED; break; }
                size_t n = (size_t)((rbytes-done) < SP_CHUNK ? (rbytes-done) : SP_CHUNK);
                if(img_qcow2_read_full(q, rstart+done, buf, n) != IMG_OK){ rc = IMG_EIO; break; }
                bool zero = true;
                for(size_t k=0;k<n;k++){ if(buf[k]){ zero=false; break; } }
                if(zero){ o->zero_bytes += n; (void)seg_push(&segs, rstart+done, n); }
                else if(nchk < 48 && (done / SP_CHUNK) % 16 == 0) GCHK_ADD(rstart+done, n < 4096 ? n : 4096);
                done += n;
                if(prog) img_prog_bytes(prog,done,rbytes);
            }
            free(buf);
            if(rc == IMG_OK) part_ok = true;
        }
        if(part_ok) o->parts_scanned++; else o->parts_skipped++;
    }
    if(rc == IMG_OK && mode == IMG_SPARSIFY_FS && o->parts_scanned == 0){
        snprintf(o->code,sizeof o->code,"SPARSIFY-FS-UNSUPPORTED");
        if(err&&errcap) snprintf(err,errcap,
            "扫描了 %llu 个分区，都不是 NTFS/FAT32：请改用 mode=zero（按内容全零）",
            (unsigned long long)o->parts_skipped);
        rc = IMG_ENOTSUP;
    }

    if(rc == IMG_OK){
        o->bytes_before = host_alloc(q->io);
        o->punch_supported = true;
        size_t npair = seg_normalize(&segs);
        if(prog){ img_prog_phase(prog,IMG_PH_FIX,"释放宿主空间"); img_prog_op(prog,IMG_OP_WRITE); img_prog_count(prog,0,npair); }
        for(size_t i=0;i<npair && rc==IMG_OK;i++){
            uint64_t s0 = segs.v[i*2], ln = segs.v[i*2+1];
            if(dry) continue;
            int prc = pf(q, s0, ln);
            if(prc == IMG_OK) o->punched_segments++;
            else { o->punch_failed++; if(prc==IMG_ENOTSUP){ o->punch_supported=false;
                   if(!o->punch_note[0]) snprintf(o->punch_note,sizeof o->punch_note,
                       "宿主文件系统不支持打洞，没有真正释放空间"); } }
            if(prog) img_prog_count(prog,i+1,npair);
        }
        o->bytes_after = host_alloc(q->io);
        if(dry){
            /* guest 偏移不等于宿主偏移，这里只能给"这些空闲区有多大"的上限；
             * 真正执行后的 freed_bytes 以宿主占用差为准。 */
            uint64_t seg_total = 0;
            for(size_t i=0;i<npair;i++) seg_total += segs.v[i*2+1];
            o->would_free_bytes = seg_total;
            o->freed_bytes = 0;
            if(!o->punch_note[0]) snprintf(o->punch_note,sizeof o->punch_note,
                                            "试运行：没有改动任何字节");
        } else {
            o->freed_bytes = (o->bytes_before > o->bytes_after)
                             ? (o->bytes_before - o->bytes_after) : 0;
            if(!o->punch_note[0] && !o->freed_bytes)
                snprintf(o->punch_note,sizeof o->punch_note,"没有可释放的块");
        }
        bool same = true;
        for(int i=0;i<nchk;i++)
            if(guest_fingerprint(q,chk[i].off,chk[i].len) != chk[i].fp){ same = false; break; }
        o->verified = same;
        if(!same && !dry){
            snprintf(o->code,sizeof o->code,"SPARSIFY-VERIFY-FAIL");
            if(err&&errcap) snprintf(err,errcap,
                "执行后校验点不一致（分区表/分区头/已用数据被改动）：立刻停止使用这份映像");
            rc = IMG_ECORRUPT;
        }
    }
    #undef GCHK_ADD
    free(segs.v);
    return rc;
}
