/* img_recovery.c — 恢复分区 / WinRE：查（WIM 内定位）、填（写 NTFS）、设属性
 * 只做机制；建不建、多大、未找到怎么办归 App（见头文件注释）。
 */
#define _DEFAULT_SOURCE
#include "img_recovery.h"
#include "img_ntfs_fs.h"
#include "img_ntfs_fmt.h"
#include "img_partition.h"
#include "img_event.h"
#include "img_wim_fs.h"
/* WIM 里的候选路径（两种约定都试）—— 即使 WIM 关闭也保留：populate 的默认值是 CAND[0] */
static const char *const CAND[] = {
    "/Windows/System32/Recovery/Winre.wim",
    "/Recovery/WindowsRE/Winre.wim",
};
#define NCAND ((int)(sizeof CAND/sizeof CAND[0]))

#if IMG_WITH_WIM
#include <wimlib.h>
#endif
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

/* ★ -DIMG_WITH_WIM=OFF 时这一整块（WIM 查找/抽取）不参与编译，只留 ENOTSUP 占位：
 *   否则 libwim 缺席 = 直接链接失败，"关掉 WIM 就构建不出来"（工程债 #13）。
 *   下面的 img_recovery_populate 仍在 —— 它靠 NTFS 写入，自称"没有 WIM 就抽不出 Winre.wim"，
 *   会如实返回不可用，而不是假成功。*/
#if IMG_WITH_WIM

static bool path_eq_norm(const char *a, const char *b){
    /* 忽略大小写与 / \ 差异 */
    while(*a && *b){
        char ca=*a++, cb=*b++;
        if(ca=='\\') ca='/';
        if(cb=='\\') cb='/';
        if(ca>='A'&&ca<='Z') ca=(char)(ca-'A'+'a');
        if(cb>='A'&&cb<='Z') cb=(char)(cb-'A'+'a');
        if(ca!=cb) return false;
    }
    return *a==0 && *b==0;
}

typedef struct { img_winre_info_t *out; int image; } find_ctx;
static int find_cb(const struct wimlib_dir_entry *d, void *ud){
    find_ctx *c=(find_ctx*)ud;
    if(c->out->found) return 0;
    for(int i=0;i<NCAND;i++){
        if(path_eq_norm(d->full_path, CAND[i])){
            c->out->found=true;
            c->out->index=c->image;
            snprintf(c->out->internal_path,sizeof c->out->internal_path,"%s",CAND[i]);
            c->out->size_bytes=d->streams[0].resource.uncompressed_size;
            return 0;
        }
    }
    return 0;
}

int img_winre_find(const char *wim_path, int index, img_winre_info_t *info){
    if(!wim_path || !info) return IMG_EINVAL;
    memset(info,0,sizeof *info);
    WIMStruct *w=NULL;
    if(wimlib_open_wim(wim_path,0,&w)!=0) return IMG_ENOENT;
    struct wimlib_wim_info wi; memset(&wi,0,sizeof wi);
    if(wimlib_get_wim_info(w,&wi)!=0){ wimlib_free(w); return IMG_EIO; }
    { const char *dot=strrchr(wim_path,'.'); snprintf(info->container,sizeof info->container,"%s",
        (dot && !strcasecmp(dot,".esd"))?"esd":"wim"); }
    int i0=(index>0)?index:1, i1=(index>0)?index:(int)wi.image_count;
    for(int i=i0;i<=i1 && !info->found;i++){
        find_ctx c={info,i};
        wimlib_iterate_dir_tree(w,i,"/",WIMLIB_ITERATE_DIR_TREE_FLAG_RECURSIVE,find_cb,&c);
    }
    wimlib_free(w);
    return IMG_OK;
}

int img_winre_extract_file(const char *wim_path, int index, const char *internal_path,
                           const char *dst_path, char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(!wim_path||!internal_path||!dst_path) return IMG_EINVAL;
    WIMStruct *w=NULL;
    if(wimlib_open_wim(wim_path,0,&w)!=0){ if(err) snprintf(err,errcap,"WIM/ESD 无法打开"); return IMG_ENOENT; }

    /* 先抽到一个临时目录（wimlib 只按目录抽单文件），再搬到 dst */
    const char *tmp=getenv("IMGTOOL_TMPDIR"); if(!tmp||!*tmp) tmp=getenv("TMPDIR");
    if(!tmp||!*tmp){ const char *t2=img_tmpdir(); if(t2) tmp=t2; }
    char tmpl[512]; snprintf(tmpl,sizeof tmpl,"%s/imgtool-winre-XXXXXX",tmp);
    char *dir=mkdtemp(tmpl);
    if(!dir){ wimlib_free(w); if(err) snprintf(err,errcap,"建临时目录失败：%s",tmp); return IMG_EIO; }

    const wimlib_tchar *paths[1]={internal_path};
    int rc=wimlib_extract_paths(w,index,dir,paths,1,0);
    wimlib_free(w);
    if(rc!=0){
        rmdir(dir);
        if(err) snprintf(err,errcap,"从 WIM 抽文件失败（wimlib rc=%d）",rc);
        return IMG_EIO;
    }

    /* wimlib 会保留内部目录结构：dir + internal_path */
    char src[1024];
    snprintf(src,sizeof src,"%s%s",dir,internal_path);
    if(rename(src,dst_path)!=0){
        /* 跨文件系统时退化为复制 */
        FILE *in=fopen(src,"rb"), *out=fopen(dst_path,"wb");
        if(!in||!out){
            if(in) fclose(in);
            if(out) fclose(out);
            rmdir(dir);
            if(err) snprintf(err,errcap,"临时文件无法移动：%s",dst_path);
            return IMG_EIO;
        }
        char buf[1<<20]; size_t n;
        while((n=fread(buf,1,sizeof buf,in))>0) if(fwrite(buf,1,n,out)!=n) break;
        fclose(in); fclose(out); unlink(src);
    }
    /* 清空临时目录（可能有多层）*/
    char cmd[1200];
    snprintf(cmd,sizeof cmd,"rm -rf '%s'",dir);
    if(system(cmd)!=0){ /* 清不掉不致命 */ }
    return IMG_OK;
}

#else  /* !IMG_WITH_WIM —— 占位实现（签名必须与头文件一致） */

int img_winre_find(const char *wim_path, int index, img_winre_info_t *info){
    (void)wim_path; (void)index;
    if(info) memset(info,0,sizeof *info);
    return IMG_ENOTSUP;
}
int img_winre_extract_file(const char *wim_path, int index, const char *internal_path,
                           const char *dst_path, char *err, size_t errcap){
    (void)wim_path; (void)index; (void)internal_path; (void)dst_path;
    if(err && errcap) snprintf(err,errcap,"本构建未启用 WIM/ESD（IMG_WITH_WIM=OFF）");
    return IMG_ENOTSUP;
}
#endif /* IMG_WITH_WIM */

/* ---------- REQ-29：把 wimlib 流出来的字节直接写进 NTFS（0 临时文件） ---------- */
typedef struct { int fd; uint64_t pos; bool eof; } pipe_src;
static int pipe_src_read(void *ud, uint64_t off, void *buf, size_t want, size_t *got){
    pipe_src *p=(pipe_src*)ud;
    if(got) *got=0;
    if(!p||!buf||!got) return IMG_EINVAL;
    if(off!=p->pos) return IMG_ENOTSUP;         /* 顺序读 */
    if(p->eof) return IMG_OK;
    /* ★ 一次把 want 填满再交出去：管道每次只给 64 KB~1 MB，若"读一次就返回"，
     *   下游会退化成几百 KB 一次的小写入 —— 真机实测慢一倍多（4.5 s vs 1.8 s）。*/
    size_t done=0;
    while(done<want){
        ssize_t n=read(p->fd,(char*)buf+done,want-done);
        if(n<0){ if(errno==EINTR) continue; return done?(*got=done,IMG_OK):IMG_EIO; }
        if(n==0){ p->eof=true; break; }
        done+=(size_t)n;
    }
    p->pos+=(uint64_t)done; *got=done;
    return IMG_OK;
}
typedef struct { img_ntfs_fs *fs; const char *dst; img_prog *prog; uint64_t written; } ntfs_cons_t;
static int ntfs_consume_cb(void *ud, int rfd, uint64_t total, char *err, size_t errcap){
    ntfs_cons_t *c=(ntfs_cons_t*)ud;
    pipe_src ps={rfd,0,false};
    img_stream st={&ps,pipe_src_read,NULL};
    img_src_meta meta; memset(&meta,0,sizeof meta);
    meta.size=total;                             /* 目标先按它分配，再逐块校验 */
    img_copy_result cr; memset(&cr,0,sizeof cr);
    int rc=img_ntfs_write_stream(c->fs,c->dst,&st,&meta,NULL,c->prog,&cr);
    c->written=cr.size;
    if(rc!=IMG_OK && err&&errcap&&!err[0]) snprintf(err,errcap,"写入 NTFS 失败（rc=%d）",rc);
    return rc;
}

int img_recovery_populate(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                          const char *winre_src, int winre_index,
                          const char *winre_internal_path,
                          const char *bootsdi_src,
                          bool do_format, const char *label,
                          bool bios_layout,
                          img_prog *prog, uint64_t *written_bytes,
                          char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(written_bytes) *written_bytes=0;
    if(!q || !winre_src) return IMG_EINVAL;

    if(do_format){
        img_ntfs_fmt_opts_t o; img_ntfs_fmt_defaults(&o,part_lba,part_bytes);
        o.label=label?label:"Recovery"; o.force=true; o.quick=true;
        int rc=img_ntfs_format(q,&o,prog);
        if(rc!=IMG_OK){
            if(err) snprintf(err,errcap,
                rc==IMG_ENOTSUP ? "本构建未启用 NTFS 格式化模块" : "格式化恢复分区失败");
            return rc;
        }
    }

    /* ★ REQ-29：不再落临时文件 —— 直接用 wimlib 的 TO_STDOUT 把字节流出来，
     *   灌进我们自己的 NTFS 写入器（img_ntfs_write_stream）。峰值磁盘占用 ≈ 0。
     *   三种来源：
     *     ① winre_internal_path = ""      → winre_src 本身就是裸 winre.wim，直接拷
     *     ② 普通路径                       → 流式解 ③ 标准两处 CAND[] 里找
     *   （②③ 如果 wimlib 太老不支持 TO_STDOUT，会退回"解到临时文件再写"。）*/
    bool is_self = (winre_internal_path && !*winre_internal_path);
    if(is_self){
        struct stat sst;
        if(stat(winre_src,&sst)!=0 || !S_ISREG(sst.st_mode)){
            if(err && errcap) snprintf(err,errcap,"裸 winre.wim 打不开：%s",winre_src);
            return IMG_ENOENT;
        }
    }

    img_ntfs_fs *fs=img_ntfs_mount_rw(q,part_lba,part_bytes);
    if(!fs){ if(err && errcap) snprintf(err,errcap,"恢复分区不是合法 NTFS（先格式化）"); return IMG_ESTATE; }

    const char *dir = bios_layout ? "/Windows/System32/Recovery" : "/Recovery/WindowsRE";
    char dst[320]; snprintf(dst,sizeof dst,"%s/Winre.wim",dir);
    img_ntfs_mkdir_p(fs,dir);

    struct stat st;
    img_span sp; img_span_prep(&sp,"recovery","populate","写入 Winre.wim");
    img_span_kv_str(&sp,"path",dst); img_span_begin(&sp);
    uint64_t wb=0;
    int rc=IMG_OK;

    if(is_self){
        rc=img_ntfs_put_file(fs,winre_src,dst,prog);
        if(rc==IMG_OK && stat(winre_src,&st)==0) wb=(uint64_t)st.st_size;
    } else {
        const char *internal = (winre_internal_path && *winre_internal_path)
                               ? winre_internal_path : CAND[0];
        const char *smode=getenv("IMGTOOL_NO_STREAM");   /* 设 1 = 走老的临时文件路径 */
        bool use_stream=!(smode && *smode && *smode!='0');
        uint64_t dsz=0; char serr[240]="";
        ntfs_cons_t nc; memset(&nc,0,sizeof nc);
        nc.fs=fs; nc.dst=dst; nc.prog=prog;
        int orc=use_stream
                 ? img_wim_stream_consume(winre_src,winre_index,internal,ntfs_consume_cb,&nc,
                                          &dsz,prog,serr,sizeof serr)
                 : IMG_ENOTSUP;
        if(orc==IMG_OK){
            rc=IMG_OK; wb=nc.written;
        } else {
            /* 退路：老办法（解到临时文件再写）。只有老 wimlib/特殊构建才会走到。*/
            const char *tmp=getenv("IMGTOOL_TMPDIR"); if(!tmp||!*tmp) tmp=getenv("TMPDIR");
            if(!tmp||!*tmp){ const char *t2=img_tmpdir(); if(t2) tmp=t2; }
            char tmpf[600];
            snprintf(tmpf,sizeof tmpf,"%s/imgtool-winre-%ld.wim",tmp,(long)getpid());
            rc=img_winre_extract_file(winre_src,winre_index,internal,tmpf,err,errcap);
            if(rc==IMG_OK){
                rc=img_ntfs_put_file(fs,tmpf,dst,prog);
                if(rc==IMG_OK && stat(tmpf,&st)==0) wb=(uint64_t)st.st_size;
                unlink(tmpf);
            } else if(err && errcap && !err[0]){
                snprintf(err,errcap,"%s",serr[0]?serr:"流式取文件失败");
            }
        }
    }
    img_span_kv_u64(&sp,"size_bytes",wb);
    img_span_end(&sp,rc);
    if(rc!=IMG_OK){
        img_ntfs_umount(fs);
        if(err && errcap && !err[0]) snprintf(err,errcap,"写入 Winre.wim 失败（rc=%d）",rc);
        return rc;
    }

    if(bootsdi_src && *bootsdi_src){
        char bdst[320]; snprintf(bdst,sizeof bdst,"%s/boot.sdi",dir);
        int r2=img_ntfs_put_file(fs,bootsdi_src,bdst,prog);
        if(r2!=IMG_OK){
            img_ntfs_umount(fs);
            if(err) snprintf(err,errcap,"写入 boot.sdi 失败（%s）",bootsdi_src);
            return r2;
        }
    }
    img_ntfs_umount(fs);
    if(written_bytes) *written_bytes=wb;
    return IMG_OK;
}

int img_part_set_attributes(img_qcow2 *q, uint64_t part_lba, uint64_t attributes){
    if(!q) return IMG_EINVAL;
    img_parttable_t t; memset(&t,0,sizeof t);
    int rc=img_parttable_read(q,&t,0);
    if(rc!=IMG_OK) return rc;
    if(t.table!=IMG_TABLE_GPT) return IMG_ENOTSUP;      /* MBR 没有 attributes */
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        if(t.parts[i].used && t.parts[i].first_lba==part_lba){
            t.parts[i].attributes=attributes;
            return img_parttable_write(q,&t);
        }
    }
    return IMG_ENOENT;
}

/* 与 winre.auto 完全同一份脚本内容；多加一行 marker（App 能看出是谁写的、给哪块盘写的）*/
#define WINRE_SCRIPT_FMT \
    "@echo off\r\n" \
    "rem marker: imgtool-winre lba=%llu ver=%s\r\n" \
    "rem imgtool: auto-enable Windows RE on first boot (official mechanism: reagentc writes\r\n" \
    "rem          ReAgent.xml + BCD recoverysequence for THIS machine's partition layout)\r\n" \
    "reagentc /enable\r\n" \
    "reagentc /info > \"%%SystemRoot%%\\Temp\\winre-enable.log\" 2>&1\r\n" \
    "rem one-shot: delete this script after it runs (goto aborts the batch first)\r\n" \
    "(goto) 2>nul & del /f /q \"%%~f0\"\r\n" \
    "exit /b 0\r\n"

int img_winre_write_enable_script(img_qcow2 *q, uint64_t os_lba, uint64_t os_bytes,
                                  size_t *out_bytes){
    if(out_bytes) *out_bytes=0;
    if(!q) return IMG_EINVAL;
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,os_lba,os_bytes);
    if(!fs) return IMG_ESTATE;
    char script[1024];
    int n=snprintf(script,sizeof script,WINRE_SCRIPT_FMT,
                   (unsigned long long)os_lba, IMGTOOL_VERSION);
    img_ntfs_mkdir_p(fs,"/Windows/Setup/Scripts");
    int rc=img_ntfs_put_file_mem(fs,script,(size_t)n,"/Windows/Setup/Scripts/SetupComplete.cmd");
    img_ntfs_umount(fs);
    if(rc==IMG_OK && out_bytes) *out_bytes=(size_t)n;
    return rc;
}

/* REQ-29 C：把 WIM/ESD/ISO 里的一个文件直接写进 qcow2 里某个 NTFS 分区（0 临时文件）。
 * 和 recovery.populate 用的是同一条流：wimlib TO_STDOUT → pipe → 我们的 NTFS 写入器。*/
int img_put_wim_file(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes,
                     const char *wim_path, int index, const char *internal_path,
                     const char *dst_abs, img_prog *prog, uint64_t *written,
                     char *err, size_t errcap){
    if(err && errcap) err[0]=0;
    if(written) *written=0;
    if(!q || !wim_path || !internal_path || !dst_abs) return IMG_EINVAL;
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,part_lba,part_bytes);
    if(!fs){ if(err&&errcap) snprintf(err,errcap,"目标分区不是合法 NTFS（先格式化）"); return IMG_ESTATE; }
    ntfs_cons_t nc; memset(&nc,0,sizeof nc);
    nc.fs=fs; nc.dst=dst_abs; nc.prog=prog;
    uint64_t sz=0;
    int rc=img_wim_stream_consume(wim_path,index,internal_path,ntfs_consume_cb,&nc,
                                  &sz,prog,err,errcap);
    img_ntfs_umount(fs);
    if(rc==IMG_OK && written) *written=nc.written;
    return rc;
}
