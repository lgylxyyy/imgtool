/* img_recovery.c — 恢复分区 / WinRE：查（WIM 内定位）、填（写 NTFS）、设属性
 * 只做机制；建不建、多大、未找到怎么办归 App（见头文件注释）。
 */
#define _DEFAULT_SOURCE
#include "img_recovery.h"
#include "img_ntfs_fs.h"
#include "img_ntfs_fmt.h"
#include "img_partition.h"
#include "img_event.h"
#include <wimlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

/* WIM 里的候选路径（两种约定都试）*/
static const char *const CAND[] = {
    "/Windows/System32/Recovery/Winre.wim",
    "/Recovery/WindowsRE/Winre.wim",
};
#define NCAND ((int)(sizeof CAND/sizeof CAND[0]))

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

    /* 先把 Winre.wim 抽到临时文件（App 侧 TMPDIR 要可写）*/
    char tmpf[600];
    const char *tmp=getenv("IMGTOOL_TMPDIR"); if(!tmp||!*tmp) tmp=getenv("TMPDIR");
    if(!tmp||!*tmp){ const char *t2=img_tmpdir(); if(t2) tmp=t2; }
    snprintf(tmpf,sizeof tmpf,"%s/imgtool-winre-%ld.wim",tmp,(long)getpid());
    int rc=img_winre_extract_file(winre_src, winre_index,
                                  winre_internal_path?winre_internal_path:CAND[0],
                                  tmpf, err, errcap);
    if(rc!=IMG_OK) return rc;

    img_ntfs_fs *fs=img_ntfs_mount_rw(q,part_lba,part_bytes);
    if(!fs){ unlink(tmpf); if(err) snprintf(err,errcap,"恢复分区不是合法 NTFS（先格式化）"); return IMG_ESTATE; }

    const char *dir = bios_layout ? "/Windows/System32/Recovery" : "/Recovery/WindowsRE";
    char dst[320]; snprintf(dst,sizeof dst,"%s/Winre.wim",dir);
    img_ntfs_mkdir_p(fs,dir);

    struct stat st;
    img_span sp; img_span_prep(&sp,"recovery","populate","写入 Winre.wim");
    img_span_kv_str(&sp,"path",dst); img_span_begin(&sp);
    uint64_t wb=0;
    rc=img_ntfs_put_file(fs,tmpf,dst,prog);
    if(rc==IMG_OK && stat(tmpf,&st)==0) wb=(uint64_t)st.st_size;
    img_span_kv_u64(&sp,"size_bytes",wb);
    img_span_end(&sp,rc);
    unlink(tmpf);
    if(rc!=IMG_OK){ img_ntfs_umount(fs); if(err) snprintf(err,errcap,"写入 Winre.wim 失败"); return rc; }

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
