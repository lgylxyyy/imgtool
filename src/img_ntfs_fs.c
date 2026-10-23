#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_ntfs_fs.c — NTFS 文件系统写入（Stage B）
 * 内部用 libntfs-3g 的公开 API（见 include/img_ntfs_fs.h 的决策记录） */
#include "img_ntfs_fs.h"
#include "img_event.h"
#include "ntfs_bridge.h"
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/index.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/unistr.h>
#include "img_ntfs_fmt.h"
#include "img_ntfs_dev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

struct img_ntfs_fs {
    ntfs_volume   *vol;
    struct ntfs_device *dev;
    img_ntfs_ctx  *ctx;
    bool           mounted;
};

img_ntfs_fs *img_ntfs_mount_rw(img_qcow2 *q, uint64_t part_lba, uint64_t part_bytes){
    if(!q || !part_bytes) return NULL;
    if(img_ntfs_probe(q, part_lba, NULL)!=IMG_OK) return NULL;

    img_ntfs_ctx *ctx = img_ntfs_ctx_new(q, part_lba, part_bytes);
    if(!ctx) return NULL;
    img_ntfs_ctx_activate(q, part_lba, part_bytes);
    /* 我们的上下文要能被全局回调找到；activate 已设，但多一份保活避免被置换 */
    /* 造占位名：ntfs_mount 内部对设备名调 realpath() 查 /proc/mounts */
    const char *dn = img_dev_placeholder(); if(!dn) dn = "imgtool-dev";

    struct ntfs_device *dev = ntfs_device_alloc(dn, 0, img_ntfs_device_ops(), NULL);
    if(!dev){ img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx); return NULL; }

    ntfs_volume *vol = ntfs_device_mount(dev, 0);   /* 0 = 读写 */
    if(!vol){
        /* ★ 休眠卷（Fast Startup）自动重试：IGNORE_HIBERFILE 只忽略检查，
           读操作安全；要清休眠请用 rm /hiberfil.sys（见 WORKPLAN 休眠节）*/
        /* 第一次失败可能把全局桥上下文清掉了 → 重试要【整套重建】ctx+dev */
        ntfs_device_free(dev);
        img_ntfs_ctx_deactivate();
        img_ntfs_ctx_free(ctx);
        ctx = img_ntfs_ctx_new(q, part_lba, part_bytes);
        if(ctx){ img_ntfs_ctx_activate(q, part_lba, part_bytes); }
        dev = ntfs_device_alloc("imgtool-dev", 0, img_ntfs_device_ops(), NULL);
        if(dev && ctx){
            vol = ntfs_device_mount(dev, 0x04000000UL);   /* 内部MS_RECOVER(实证) */
        }
    }
    if(!vol){
        ntfs_device_free(dev);
        img_ntfs_ctx_deactivate();
        img_ntfs_ctx_free(ctx);
        return NULL;
    }
    img_ntfs_fs *fs = calloc(1, sizeof *fs);
    if(!fs){ ntfs_umount(vol,false); img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx); return NULL; }
    fs->vol=vol; fs->dev=dev; fs->ctx=ctx; fs->mounted=true;
    return fs;
}

ntfs_volume *img_ntfs_probe_vol(img_ntfs_fs *fs){ return fs?fs->vol:NULL; }   /* 诊断用 */

void img_ntfs_umount(img_ntfs_fs *fs){
    if(!fs) return;
    if(fs->mounted && fs->vol){
        /* ★ ntfs_umount 内部已 ntfs_device_free()，这里绝不能再调一次
         * ★★ 2026-10-18：force 必须 true —— false 时若卷带任何错误标志，
         *   ntfs_umount 会【跳过清理】直接返回 → 卷留脏 → bootmgr 偶发读 BCD
         *   踩到脏结构 triple-fault 重启（双系统盘间歇崩溃的真凶，5连测已证）。*/
        ntfs_umount(fs->vol, true);
        fs->mounted=false;
    }
    img_ntfs_ctx_deactivate();
    img_ntfs_ctx_free(fs->ctx);
    free(fs);
}

/* ---------- 内部辅助 ---------- */

/* ntfs_inode_lookup_by_name 约定（见 dir.c 文档）：
 *   >=0  = 找到（MFT 引用）
 *   (u64)-1 = 未找到或出错，靠 errno 区分：ENOENT=不存在，其余=真错误
 * 我曾把 -1 当"找到了"去 ntfs_inode_open(-1)，导致 mkdir 失败。*/
static bool mftref_ok(u64 ref){ return ref != (u64)-1 && ref != 0; }

/* 拿到 "foo/bar/baz" 里第 idx 级的名字；返回是否取到 */
static bool nth_component(const char *path, int idx, char *out, size_t cap){
    const char *p = path;
    while(*p=='/') p++;
    int n=0;
    while(*p){
        const char *st=p;
        while(*p && *p!='/') p++;
        if(n==idx){
            size_t l=(size_t)(p-st);
            if(l>=cap) l=cap-1;
            memcpy(out,st,l); out[l]=0;
            return true;
        }
        n++;
        while(*p=='/') p++;
    }
    return false;
}

/* 把 UTF-8 路径组件转成 ntfschar */
static bool to_ntfs(const char *s, ntfschar **out, size_t *outlen){
    *out=NULL; *outlen=0;
    if(ntfs_mbstoucs(s, out) < 0 || !*out) return false;
    *outlen = strlen(s);     /* ASCII 组件字符数 = 字节数 */
    return true;
}

/* 逐级查找（不创建）。返回找到的 inode（调用方 ntfs_inode_close），NULL=不存在 */

/* ---------- 子项查找：先精确，再忽略大小写 ----------
 * Windows 路径不区分大小写，而 ntfs_inode_lookup_by_name() 是精确匹配。
 * 实测：Win7 装完是 \Windows\Boot、\Windows\System32；写 /Windows/boot 直接
 *       FAT-LS-FAIL（看起来像"盘坏了/版本无法读取"，其实是大小写）。
 * 第三方精简镜像大小写更乱，所以这里兜一层：精确失败就扫一遍目录，
 * 用 NTFS 自己的 upcase 表比较（对 Unicode 也正确，不是只认 ASCII）。 */
typedef struct {
    const ntfschar *want; size_t want_len;
    const ntfschar *upcase; u32 upcase_len;
    MFT_REF ref; int found;
} ci_ctx;

static int ci_fill(void *ud, const ntfschar *name, const int name_len, const int name_type,
                   const s64 pos, const MFT_REF mref, const unsigned dt_type){
    (void)pos; (void)dt_type;
    ci_ctx *c=(ci_ctx*)ud;
    if(c->found || !name || name_len<=0) return 0;
    if(name_type==FILE_NAME_DOS) return 0;          /* 跳过 8.3 短名 */
    if(ntfs_names_are_equal(c->want,(size_t)c->want_len, name,(size_t)name_len,
                            1, c->upcase, c->upcase_len)){
        c->ref=mref; c->found=1;
    }
    return 0;
}

static bool lookup_child(ntfs_volume *vol, ntfs_inode *dir,
                         const ntfschar *u, size_t ul, MFT_REF *out){
    errno=0;
    MFT_REF ref=ntfs_inode_lookup_by_name(dir, u, ul);
    if(mftref_ok(ref)){ *out=ref; return true; }
    if(!vol || !vol->upcase || !vol->upcase_len) return false;
    ci_ctx c; memset(&c,0,sizeof c);
    c.want=u; c.want_len=ul; c.upcase=vol->upcase; c.upcase_len=vol->upcase_len;
    s64 pos=0;
    if(ntfs_readdir(dir,&pos,&c,ci_fill)<0 && !c.found) return false;
    if(!c.found) return false;
    *out=c.ref; return true;
}

static ntfs_inode *lookup(ntfs_volume *vol, const char *path, ntfs_inode **parent_out){
    ntfs_inode *cur = ntfs_inode_open(vol, FILE_root);
    if(parent_out) *parent_out = NULL;
    if(!cur) return NULL;
    char comp[256];
    int idx=0;
    if(parent_out) *parent_out = ntfs_inode_open(vol, FILE_root);
    while(nth_component(path, idx, comp, sizeof comp)){
        ntfschar *u=NULL; size_t ul=0;
        if(!to_ntfs(comp,&u,&ul)){ ntfs_inode_close(cur); return NULL; }
        errno=0;
        MFT_REF ref = 0;
        bool got = lookup_child(vol, cur, u, ul, &ref);
        int lerr = got ? 0 : (errno?errno:ENOENT);
        free(u);
        if(!got){
            ntfs_inode_close(cur);
            if(parent_out && *parent_out){ ntfs_inode_close(*parent_out); *parent_out=NULL; }
            if(lerr!=0 && lerr!=ENOENT && getenv("IMG_NTFS_FS_DEBUG"))
                fprintf(stderr,"  [dbg] lookup(%s) 真错误 errno=%d (%s)\n", comp, lerr, strerror(lerr));
            return NULL;
        }
        ntfs_inode *nx = ntfs_inode_open(vol, ref);
        ntfs_inode_close(cur);
        if(parent_out){
            /* parent 依次前进（保留引用供创建使用）*/
            ntfs_inode_close(*parent_out);
            ntfs_inode_open(vol, FILE_root);   /* 重新基准，简化处理 */
        }
        cur = nx;
        if(!cur) return NULL;
        idx++;
        if(!nth_component(path, idx, comp, sizeof comp)) break;
    }
    if(parent_out && *parent_out){ ntfs_inode_close(*parent_out); *parent_out=NULL; }
    return cur;
}

bool img_ntfs_path_exists(img_ntfs_fs *fs, const char *abs_path){
    if(!fs||!fs->vol||!abs_path) return false;
    ntfs_inode *ni = lookup(fs->vol, abs_path, NULL);
    if(!ni) return false;
    ntfs_inode_close(ni);
    return true;
}

int img_ntfs_file_size(img_ntfs_fs *fs, const char *abs_path, uint64_t *out_size){
    if(!fs||!fs->vol||!abs_path||!out_size) return IMG_EINVAL;
    ntfs_inode *ni = lookup(fs->vol, abs_path, NULL);
    if(!ni) return IMG_ENOENT;
    /* 目录没有 AT_DATA 或 data_size 无意义 —— 用 mft 记录标志判目录 */
    if((ni->flags & MFT_RECORD_IS_DIRECTORY) != 0) *out_size = 0;
    else *out_size = ni->data_size >= 0 ? (uint64_t)ni->data_size : 0;
    ntfs_inode_close(ni);
    return IMG_OK;
}

/* ---------- 递归建目录 ----------
 * 逐级：已有 → 下钻；没有 → 原地创建（S_IFDIR）。每级只前进一次。 */
int img_ntfs_mkdir_p(img_ntfs_fs *fs, const char *abs_path){
    if(!fs||!fs->vol||!abs_path) return IMG_EINVAL;
    ntfs_inode *cur = ntfs_inode_open(fs->vol, FILE_root);
    if(!cur){ if(getenv("IMG_NTFS_FS_DEBUG")) fprintf(stderr,"  [dbg] root open 失败\n"); return IMG_EIO; }
    if(getenv("IMG_NTFS_FS_DEBUG")) fprintf(stderr,"  [dbg] root ok, path=%s\n", abs_path);
    char comp[256];
    int idx = 0;
    while(nth_component(abs_path, idx, comp, sizeof comp)){
        ntfschar *u=NULL; size_t ul=0;
        if(!to_ntfs(comp,&u,&ul)){
            if(getenv("IMG_NTFS_FS_DEBUG")) fprintf(stderr,"  [dbg] to_ntfs(%s) 失败\n",comp);
            ntfs_inode_close(cur); return IMG_EINVAL;
        }
        errno=0;
        MFT_REF ref = 0;
        bool got = lookup_child(fs->vol, cur, u, ul, &ref);
        int lerr = got ? 0 : (errno?errno:ENOENT);
        ntfs_inode *next = NULL;
        if(getenv("IMG_NTFS_FS_DEBUG"))
            fprintf(stderr,"  [dbg] idx=%d comp='%s' ref=%llu errno=%d\n",
                    idx, comp, (unsigned long long)ref, lerr);
        if(lerr!=0 && lerr!=ENOENT){
            /* 真错误（索引损坏等）—— 不能当成"不存在"硬建 */
            free(u); ntfs_inode_close(cur);
            return IMG_EIO;
        }
        if(mftref_ok(ref)){
            next = ntfs_inode_open(fs->vol, ref);
        } else {
            /* securid=0 → ntfs-3g 用卷默认继承的安全描述符 */
            next = ntfs_create(cur, 0, u, (u8)ul, S_IFDIR);
            if(getenv("IMG_NTFS_FS_DEBUG"))
                fprintf(stderr,"  [dbg]   ntfs_create → %p errno=%d (%s)\n",
                        (void*)next, errno, strerror(errno));
            if(!next){
                if(getenv("IMG_NTFS_FS_DEBUG"))
                    fprintf(stderr,"  [dbg] ntfs_create(%s) failed errno=%d (%s)\n",
                            comp, errno, strerror(errno));
            }
        }
        free(u);
        ntfs_inode_close(cur);
        if(!next) return IMG_EIO;
        cur = next;
        idx++;
    }
    ntfs_inode_close(cur);
    return IMG_OK;
}

/* ---------- 读文件 ---------- */
int img_ntfs_read_file(img_ntfs_fs *fs, const char *abs_path, void **out, size_t *out_len){
    if(!fs||!fs->vol||!abs_path||!out||!out_len) return IMG_EINVAL;
    *out=NULL; *out_len=0;
    ntfs_inode *ni = lookup(fs->vol, abs_path, NULL);
    if(!ni) return IMG_ENOENT;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, NULL, 0);
    if(!na){ ntfs_inode_close(ni); return IMG_EIO; }
    s64 sz = na->data_size;
    if(sz<0 || sz> (s64)4ULL*1024*1024*1024){ ntfs_attr_close(na); ntfs_inode_close(ni); return IMG_ECORRUPT; }
    char *buf = malloc((size_t)sz + 1);
    if(!buf){ ntfs_attr_close(na); ntfs_inode_close(ni); return IMG_ENOMEM; }
    s64 got = ntfs_attr_pread(na, 0, sz, buf);
    ntfs_attr_close(na);
    ntfs_inode_close(ni);
    if(got != sz){ free(buf); return IMG_EIO; }
    buf[sz]=0;
    *out=buf; *out_len=(size_t)sz;
    return IMG_OK;
}

/* 流式导出：1 MiB 一块，边读边写（内存峰值恒定）*/
int img_ntfs_copy_out(img_ntfs_fs *fs, const char *abs_path, const char *dst_path,
                      img_prog *prog, uint64_t *out_bytes){
    if(out_bytes) *out_bytes=0;
    if(!fs||!fs->vol||!abs_path||!dst_path) return IMG_EINVAL;
    ntfs_inode *ni = lookup(fs->vol, abs_path, NULL);
    if(!ni) return IMG_ENOENT;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, NULL, 0);
    if(!na){ ntfs_inode_close(ni); return IMG_EIO; }
    s64 sz = na->data_size;
    if(sz<0){ ntfs_attr_close(na); ntfs_inode_close(ni); return IMG_ECORRUPT; }
    FILE *o=fopen(dst_path,"wb");
    if(!o){ ntfs_attr_close(na); ntfs_inode_close(ni); return IMG_EIO; }
    if(prog){ img_prog_op(prog,IMG_OP_READ);
              img_prog_phase(prog,IMG_PH_EXTRACT,"导出 NTFS 文件");
              img_prog_bytes(prog,0,(uint64_t)sz); }
    enum { CHUNK = 1u<<20 };
    uint8_t *buf=malloc(CHUNK);
    int rc=IMG_OK;
    if(!buf) rc=IMG_ENOMEM;
    s64 done=0;
    while(rc==IMG_OK && done<sz){
        size_t want=(size_t)((sz-done)>(s64)CHUNK?CHUNK:(sz-done));
        s64 got=ntfs_attr_pread(na,done,(s64)want,buf);
        if(got<=0){ rc=IMG_EIO; break; }
        if(fwrite(buf,1,(size_t)got,o)!=(size_t)got){ rc=IMG_EIO; break; }
        done+=got;
        if(prog) img_prog_bytes(prog,(uint64_t)done,(uint64_t)sz);
                img_hb("ntfs","extract","copy_out",done>>20);
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }
    }
    free(buf); fclose(o);
    ntfs_attr_close(na); ntfs_inode_close(ni);
    if(rc!=IMG_OK){ unlink(dst_path); return rc; }
    if(out_bytes) *out_bytes=(uint64_t)done;
    return IMG_OK;
}

/* ---------- 写文件 ---------- */
/* 内存数据写文件（与 put_file 同逻辑，只是数据来源不同）*/
int img_ntfs_put_file_mem(img_ntfs_fs *fs, const void *data, size_t len,
                          const char *dst_abs_path){
    if(!fs||!fs->vol||!data||!dst_abs_path) return IMG_EINVAL;
    /* 父目录 */
    char parent[512]; snprintf(parent,sizeof parent,"%s",dst_abs_path);
    char *slash = strrchr(parent,'/');
    if(slash && slash!=parent){ *slash=0;
        int rc = img_ntfs_mkdir_p(fs, parent);
        if(rc!=IMG_OK) return rc;
    }
    /* 末级名字 */
    const char *fname = strrchr(dst_abs_path,'/');
    fname = fname ? fname+1 : dst_abs_path;
    if(!fname[0]) return IMG_EINVAL;
    char parentdir[512];
    snprintf(parentdir,sizeof parentdir,"%.*s",(int)(fname-dst_abs_path), dst_abs_path);
    if(parentdir[0]=='/' && parentdir[1]==0) parentdir[0]=0;

    ntfs_inode *dir = parentdir[0] ? lookup(fs->vol, parentdir, NULL)
                                   : ntfs_inode_open(fs->vol, FILE_root);
    if(!dir) return IMG_ENOENT;

    ntfschar *uname=NULL; size_t ulen=strlen(fname);
    if(ntfs_mbstoucs((const char*)fname,&uname) < 0){ ntfs_inode_close(dir); return IMG_EINVAL; }
    errno=0;
    MFT_REF ref = ntfs_inode_lookup_by_name(dir, uname, ulen);
    int lerr = (ref==(MFT_REF)-1) ? errno : 0;
    ntfs_inode *file;
    if(lerr!=0 && lerr!=ENOENT){ free(uname); ntfs_inode_close(dir); return IMG_EIO; }
    if(mftref_ok(ref)) file = ntfs_inode_open(fs->vol, ref);
    else               file = ntfs_create(dir, 0, uname, (u8)ulen, S_IFREG);
    free(uname);
    ntfs_inode_close(dir);
    if(!file) return IMG_EIO;

    ntfs_attr *na = ntfs_attr_open(file, AT_DATA, NULL, 0);
    if(!na){ ntfs_inode_close(file); return IMG_EIO; }
    if(ntfs_attr_truncate(na, 0)<0){ ntfs_attr_close(na); ntfs_inode_close(file); return IMG_EIO; }
    int rc = IMG_OK;
    if(len){
        s64 w = ntfs_attr_pwrite(na, 0, (s64)len, data);
        if(w != (s64)len) rc = (errno==ENOSPC)?IMG_EFULL:IMG_EIO;
    }
    ntfs_attr_close(na);
    if(ntfs_inode_close(file)<0 && rc==IMG_OK) rc=IMG_EIO;
    return rc;
}

int img_ntfs_put_file(img_ntfs_fs *fs, const char *local_path,
                      const char *dst_abs_path, img_prog *prog){
    if(!fs||!fs->vol||!local_path||!dst_abs_path) return IMG_EINVAL;
    /* ① 保证父目录存在 */
    char parent[512]; snprintf(parent,sizeof parent,"%s",dst_abs_path);
    char *slash = strrchr(parent,'/');
    if(slash && slash!=parent){ *slash=0;
        int rc = img_ntfs_mkdir_p(fs, parent);
        if(rc!=IMG_OK) return rc;
    } else if(slash==parent){
        /* 根目录下，无需建 */
    }
    /* ② 打开本地文件 */
    int fd = open(local_path, O_RDONLY);
    if(fd<0) return IMG_ENOENT;
    struct stat st; if(fstat(fd,&st)<0){ close(fd); return IMG_EIO; }
    uint64_t fsz = (uint64_t)st.st_size;

    /* ③ 分离路径与文件名 */
    const char *fname = strrchr(dst_abs_path,'/');
    fname = fname ? fname+1 : dst_abs_path;
    if(!fname[0]){ close(fd); return IMG_EINVAL; }
    char parentdir[512];
    snprintf(parentdir,sizeof parentdir,"%.*s",(int)(fname-dst_abs_path), dst_abs_path);
    if(parentdir[0]=='/' && parentdir[1]==0) parentdir[0]=0;   /* 根 */

    /* ④ 打开父目录 */
    ntfs_inode *dir;
    if(parentdir[0]){
        dir = lookup(fs->vol, parentdir, NULL);
    } else {
        dir = ntfs_inode_open(fs->vol, FILE_root);
    }
    if(!dir){ close(fd); return IMG_ENOENT; }

    /* ⑤ 已存在则先截断重写，不存在则创建 */
    ntfschar *u=NULL; size_t ul=0;
    if(!to_ntfs(fname,&u,&ul)){ ntfs_inode_close(dir); close(fd); return IMG_EINVAL; }
    errno=0;
    MFT_REF ref = ntfs_inode_lookup_by_name(dir, u, ul);
    int lerr = (ref==(MFT_REF)-1) ? errno : 0;
    ntfs_inode *file;
    if(lerr!=0 && lerr!=ENOENT){ free(u); ntfs_inode_close(dir); close(fd); return IMG_EIO; }
    if(mftref_ok(ref)){
        file = ntfs_inode_open(fs->vol, ref);
    } else {
        file = ntfs_create(dir, 0, u, (u8)ul, S_IFREG);
    }
    free(u);
    ntfs_inode_close(dir);
    if(!file){ close(fd); return IMG_EIO; }

    ntfs_attr *na = ntfs_attr_open(file, AT_DATA, NULL, 0);
    if(!na){ ntfs_inode_close(file); close(fd); return IMG_EIO; }
    /* 清掉原有数据（覆盖语义） */
    if(ntfs_attr_truncate(na, 0)<0){
        ntfs_attr_close(na); ntfs_inode_close(file); close(fd); return IMG_EIO;
    }

    /* ⑥ 分块写（1MiB，便于进度与降低内存峰值） */
    enum { CHUNK = 1u<<20 };
    char *buf = malloc(CHUNK);
    if(!buf){ ntfs_attr_close(na); ntfs_inode_close(file); close(fd); return IMG_ENOMEM; }
    if(prog){ img_prog_phase(prog,IMG_PH_EXTRACT,"写入 NTFS"); img_prog_op(prog,IMG_OP_WRITE);
              img_prog_bytes(prog,0,fsz); }
    img_span sp;                          /* 里程碑：NTFS 写文件（流式，1MiB 一块）*/
    img_span_prep(&sp,"ntfs","put","写入 NTFS");
    img_span_kv_str(&sp,"path",dst_abs_path);
    img_span_kv_u64(&sp,"size",(uint64_t)fsz);
    img_span_begin(&sp);
    s64 done=0; int rc=IMG_OK;
    for(;;){
        if(img_cancelled()){ rc=IMG_ECANCELLED; break; }   /* ★ 取消检查点（每 1MiB 一次）*/
        ssize_t r = read(fd, buf, CHUNK);
        if(r<0){ rc = img_cancelled() ? IMG_ECANCELLED : IMG_EIO; break; }
        if(r==0) break;
        s64 w = ntfs_attr_pwrite(na, done, r, buf);
        if(w != r){ rc=(errno==ENOSPC)?IMG_EFULL:IMG_EIO; break; }
        done += w;
        if(prog) img_prog_bytes(prog,(uint64_t)done,fsz);
    }
    free(buf); close(fd);
    ntfs_attr_close(na);
    if(ntfs_inode_close(file)<0 && rc==IMG_OK) rc=IMG_EIO;
    if(rc==IMG_OK && prog) img_prog_done(prog,"文件写入完成");
    img_span_end(&sp, rc);
    return rc;
}


/* ---------- ReAgent.xml（WinRE 注册） ----------
 * 官方要求恢复分区里有 Winre.wim + ReAgent.xml；
 * reagentc 靠它把位置注册为 \\?\\GLOBALROOT\\device\\harddiskN\\partitionM\\... */
int img_ntfs_write_reagent(img_ntfs_fs *fs, const char *winre_dir_abs,
                           const char *winre_loc, const char *bcd_guid,
                           uint64_t part_off){
    if(!fs||!winre_dir_abs||!winre_loc||!bcd_guid) return IMG_EINVAL;
    /* ★ 格式真值：官方 Windows 11 28000 镜像里的
     *   \Windows\System32\Recovery\ReAgent.xml（我们灌系统用的 ESD 里就带着），
     *   与公开实测样本一致：
     *     <WindowsRE version="2.0">
     *       <WinreBCD id="{GUID}"/>                       ← reagentc /info 的 BCD identifier
     *       <WinreLocation path="\Recovery\WindowsRE" id="0" offset="<分区字节偏移>" guid="{0}"/>
     *       ...
     *       <InstallState state="1"/>                     ← 1 = 已启用
     *   早期我自造了 <RecoveryEnvironment>/<WinreBCD value=...> 字段名 ——
     *   自己写的检查器能过、Windows 完全不认（"自洽但外部不认"的典型坑）。*/
    char xml[2048];
    snprintf(xml,sizeof xml,
        "<?xml version='1.0' encoding='utf-8' standalone='yes'?>\r\n"
        "<WindowsRE version=\"2.0\">\r\n"
        "\t<WinreBCD id=\"{%s}\"></WinreBCD>\r\n"
        "\t<WinreLocation path=\"%s\" id=\"0\" offset=\"%llu\" "
            "guid=\"{00000000-0000-0000-0000-000000000000}\"></WinreLocation>\r\n"
        "\t<ImageLocation path=\"\" id=\"0\" offset=\"0\" "
            "guid=\"{00000000-0000-0000-0000-000000000000}\"></ImageLocation>\r\n"
        "\t<PBRImageLocation path=\"\" id=\"0\" offset=\"0\" "
            "guid=\"{00000000-0000-0000-0000-000000000000}\" index=\"0\"></PBRImageLocation>\r\n"
        "\t<PBRCustomImageLocation path=\"\" id=\"0\" offset=\"0\" "
            "guid=\"{00000000-0000-0000-0000-000000000000}\" index=\"0\"></PBRCustomImageLocation>\r\n"
        "\t<InstallState state=\"1\"></InstallState>\r\n"
        "\t<OsInstallAvailable state=\"0\"></OsInstallAvailable>\r\n"
        "\t<CustomImageAvailable state=\"0\"></CustomImageAvailable>\r\n"
        "\t<WinREStaged state=\"0\"></WinREStaged>\r\n"
        "\t<ScheduledOperation state=\"4\"></ScheduledOperation>\r\n"
        "\t<OperationParam path=\"\"></OperationParam>\r\n"
        "\t<OperationPermanent state=\"0\"></OperationPermanent>\r\n"
        "\t<OsBuildVersion path=\"\"></OsBuildVersion>\r\n"
        "\t<OemTool state=\"0\"></OemTool>\r\n"
        "</WindowsRE>\r\n",
        bcd_guid, winre_loc, (unsigned long long)part_off);
    char dnorm[256]; snprintf(dnorm,sizeof dnorm,"%s",winre_dir_abs);
    for(char *p=dnorm; *p; p++) if(*p=='\\') *p='/';
    int rc = img_ntfs_mkdir_p(fs, dnorm[0]?dnorm:"/");
    if(rc!=IMG_OK) return rc;
    char full[512];
    snprintf(full,sizeof full, "%s/ReAgent.xml", dnorm[0]?dnorm:"");   /* 空目录 = 根，避免 "//" */
    return img_ntfs_put_file_mem(fs, xml, strlen(xml), full);
}

/* ================================================================
 * 列目录 / 空闲空间 / 卷标 / 搬移原语（part.scan 与 ntfs.rename 的底层）
 * ================================================================ */

/* ---------- 列目录：ntfs_readdir + ntfs_ucstombs → 和 FAT 同一套 cb 契约 ---------- */
typedef struct { img_ntfs_list_cb cb; void *ud; } ntfs_list_ctx;
static int ntfs_list_fill(void *dirent, const ntfschar *name, const int name_len,
                          const int name_type, const s64 pos,
                          const MFT_REF mref, const unsigned dt_type){
    ntfs_list_ctx *c=(ntfs_list_ctx*)dirent;
    (void)name_type; (void)pos;
    /* ★ 系统元数据文件（$MFT/$Bitmap/$Volume… 记录号 < FILE_first_user=16）
     *   会被 ntfs_readdir 列出来 —— 但它们不是"用户数据"，
     *   否则刚格式化的空卷也会被误报成 level2（有数据）。*/
    if(((uint64_t)le64_to_cpu(mref) & 0xFFFFFFFFFFFFULL) < (uint64_t)FILE_first_user) return 0;
    if(name_len==1 && name[0]=='.')  return 0;      /* 防御：NTFS 索引里本不该有 . / .. */
    if(name_len==2 && name[0]=='.' && name[1]=='.') return 0;
    char *out=NULL;
    if(ntfs_ucstombs(name, name_len, &out, -1)<0 || !out) return 0;  /* 转不出的名字跳过，不打断列表 */
    c->cb(c->ud, out, dt_type==NTFS_DT_DIR, 0);
    free(out);
    return 0;
}
int img_ntfs_list_dir(img_ntfs_fs *fs, const char *abs_path,
                      img_ntfs_list_cb cb, void *ud){
    if(!fs||!fs->mounted||!cb) return IMG_EINVAL;
    ntfs_inode *di=lookup(fs->vol, (abs_path&&abs_path[0])?abs_path:"/", NULL);
    if(!di) return IMG_ENOENT;
    ntfs_list_ctx c={cb,ud};
    s64 pos=0;
    int r=ntfs_readdir(di,&pos,&c,ntfs_list_fill);
    ntfs_inode_close(di);
    return r<0?IMG_EIO:IMG_OK;
}


/* 带真实大小的列目录（--sizes 才走这条路）。
 * 为什么默认不做：ntfs_readdir 的回调【不给大小】（见 libntfs-3g/dir.c 的
 * ntfs_filldir：只给 name/mref/dt_type），要拿大小只能逐条 ntfs_inode_open
 * 读 ni->data_size —— System32 那种 4000 项的目录会多出几千次 MFT 记录读取。
 * 所以：默认 size 恒 0（快），显式要才付这个钱。 */
typedef struct { img_ntfs_list_cb cb; void *ud; ntfs_volume *vol; } ntfs_sizectx;
static int ntfs_size_fill(void *dirent, const ntfschar *name, const int name_len,
                          const int name_type, const s64 pos,
                          const MFT_REF mref, const unsigned dt_type){
    ntfs_sizectx *c=(ntfs_sizectx*)dirent;
    (void)name_type; (void)pos;
    uint64_t ino=((uint64_t)le64_to_cpu(mref) & 0xFFFFFFFFFFFFULL);
    if(ino < (uint64_t)FILE_first_user) return 0;
    if(name_len==1 && name[0]=='.')  return 0;
    if(name_len==2 && name[0]=='.' && name[1]=='.') return 0;
    char *out=NULL;
    if(ntfs_ucstombs(name, name_len, &out, -1)<0 || !out) return 0;
    uint64_t sz=0;
    if(dt_type!=NTFS_DT_DIR){
        ntfs_inode *ni=ntfs_inode_open(c->vol, mref);
        if(ni){ if(ni->data_size>0) sz=(uint64_t)ni->data_size; ntfs_inode_close(ni); }
    }
    c->cb(c->ud, out, dt_type==NTFS_DT_DIR, sz);
    free(out);
    return 0;
}
int img_ntfs_list_dir_sizes(img_ntfs_fs *fs, const char *abs_path,
                            img_ntfs_list_cb cb, void *ud){
    if(!fs||!fs->mounted||!cb) return IMG_EINVAL;
    ntfs_inode *di=lookup(fs->vol, (abs_path&&abs_path[0])?abs_path:"/", NULL);
    if(!di) return IMG_ENOENT;
    ntfs_sizectx c={cb,ud,fs->vol};
    s64 pos=0;
    int r=ntfs_readdir(di,&pos,&c,ntfs_size_fill);
    ntfs_inode_close(di);
    return r<0?IMG_EIO:IMG_OK;
}

/* ---------- 空闲/已用：直接数 $Bitmap（NTFS 位图：1=已分配 0=空闲）---------- */
int img_ntfs_free_bytes(img_ntfs_fs *fs, uint64_t *out_free, uint64_t *out_used){
    if(!fs||!fs->vol||!fs->vol->lcnbmp_na) return IMG_EINVAL;
    ntfs_volume *vol=fs->vol;
    s64 nr=vol->nr_clusters;
    int cs=vol->cluster_size;
    if(nr<=0||cs<=0) return IMG_ESTATE;
    size_t nb=(size_t)((nr+7)/8);
    unsigned char *bm=(unsigned char*)calloc(1,nb?nb:1);
    if(!bm) return IMG_ENOMEM;
    s64 got=ntfs_attr_pread(vol->lcnbmp_na,0,(s64)nb,bm);
    if(got!=(s64)nb){ free(bm); return IMG_EIO; }
    s64 free_cl=0;
    for(size_t i=0;i<nb;i++){
        if((i+1)*8<=(size_t)nr) free_cl += 8-__builtin_popcount(bm[i]);
        else for(int b=0;b<8;b++){                 /* 末字节里超出 nr_clusters 的位按"已分配"算 */
            s64 cl=(s64)i*8+b;
            if(cl<nr && !(bm[i]&(1u<<b))) free_cl++;
        }
    }
    free(bm);
    if(out_free) *out_free=(uint64_t)free_cl*(uint64_t)cs;
    if(out_used) *out_used=(uint64_t)(nr-free_cl)*(uint64_t)cs;
    return IMG_OK;
}

/* ---------- 卷标（$Volume 的 AT_VOLUME_NAME；没有就空串）---------- */
int img_ntfs_label(img_ntfs_fs *fs, char *out, size_t cap){
    if(!fs||!out||!cap) return IMG_EINVAL;
    out[0]=0;
    ntfs_inode *vi=ntfs_inode_open(fs->vol, FILE_Volume);
    if(!vi) return IMG_ENOENT;
    ntfs_attr *na=ntfs_attr_open(vi, AT_VOLUME_NAME, NULL, 0);
    if(!na){ ntfs_inode_close(vi); return IMG_OK; }     /* 无卷标 → 空串，不算错 */
    char tmp[512];
    s64 got=ntfs_attr_pread(na, 0, (s64)sizeof tmp, tmp);
    if(got>0){
        char *u=NULL;
        if(ntfs_ucstombs((const ntfschar*)tmp, (int)got/2, &u, -1)>=0 && u){
            snprintf(out,cap,"%s",u); free(u);
        }
    }
    ntfs_attr_close(na);
    ntfs_inode_close(vi);
    return IMG_OK;
}

/* ---------- 搬移原语：/名字 → /目标目录/名字（同名，只换父目录）----------
 * 实测踩坑（tests/ntfs_probe.c 三场景隔离出来的），实现必须遵守三条：
 *   1) ntfs_link 只改【内存】的目录索引（小目录的项住在目标目录 MFT 记录里），
 *      必须 ntfs_inode_sync(目标目录) 落盘 —— 否则 ntfs_delete 收尾时会
 *      重新从盘上打开父目录查索引 → 查不到 → 库里 ENOENT 被改写成 EIO。
 *   2) ntfs_delete 【会消费】传入的 ni 与 dir_ni（无论成败，内部都 close 掉），
 *      调用方绝不能再碰它们；下一轮要重新 open。
 *   3) 删除失败时根目录旧名还在（数据没丢），只需回滚目标目录里多出的链接。*/
int img_ntfs_rm(img_ntfs_fs *fs, const char *abs_path){
    if(!fs||!fs->mounted||!abs_path||!abs_path[0]||!strcmp(abs_path,"/")||!abs_path[1])
        return IMG_EINVAL;
    const char *slash=strrchr(abs_path,'/');
    if(!slash||!slash[1]) return IMG_EINVAL;
    static char parent[512], name[256];
    size_t plen=(size_t)(slash-abs_path);
    if(plen==0){ snprintf(parent,sizeof parent,"/"); }
    else{ if(plen>=sizeof parent) return IMG_EINVAL;
          memcpy(parent,abs_path,plen); parent[plen]=0; }   /* ★起点是 abs_path 不是 slash（抄错过→乱码路径→ENOENT）*/
    if(strlen(slash+1)>=sizeof name) return IMG_EINVAL;
    snprintf(name,sizeof name,"%s",slash+1);

    ntfschar *u=NULL; size_t ul=0;
    if(!to_ntfs(name,&u,&ul)) return IMG_EINVAL;
    ntfs_inode *dir_ni=lookup(fs->vol,parent,NULL);
    if(!dir_ni){ free(u); return IMG_ENOENT; }
    errno=0;
    MFT_REF ref=ntfs_inode_lookup_by_name(dir_ni,u,ul);
    if(!mftref_ok(ref)){ ntfs_inode_close(dir_ni); free(u); return IMG_ENOENT; }
    ntfs_inode *ni=ntfs_inode_open(fs->vol,ref);
    if(!ni){ ntfs_inode_close(dir_ni); free(u); return IMG_EIO; }
    /* ★ 只删文件，不删目录（当前唯一用途：删 SetupComplete.cmd 这类脚本文件）。
     *   目录一律拒绝 —— 递归删的误操作比"删不掉"可怕得多；将来真需要再按索引判空。*/
    if(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY){
        ntfs_inode_close(ni); ntfs_inode_close(dir_ni); free(u);
        return IMG_ENOTSUP;
    }
    errno=0;
    /* ★ 先 sync 父目录（落盘）再删 —— move_into 里"本次事故的根因"就是少这一步 */
    (void)ntfs_inode_sync(dir_ni);
    int dr=ntfs_delete(fs->vol, abs_path, ni, dir_ni, u, (u8)ul); /* 消费 ni + dir_ni */
    free(u);
    if(dr!=0) return (errno==ENOENT)?IMG_ENOENT:IMG_EIO;
    return IMG_OK;
}

int img_ntfs_move_into(img_ntfs_fs *fs, const char *dst_dir,
                       const char **names, int nnames, int *moved_out){
    if(moved_out) *moved_out=0;
    if(!fs||!fs->mounted||!dst_dir||!names||nnames<=0) return IMG_EINVAL;
    if(img_ntfs_mkdir_p(fs, dst_dir)!=IMG_OK) return IMG_EIO;
    /* ★ 目标是根目录时，dst 必须【就是 root 那一份】：
     *   ntfs-3g 对同一个 MFT 号两次 open 会给出各自独立的索引副本
     *   （详见 move_path 里同目录改名的注释），两份混用会把 $I30 写坏。*/
    bool dst_is_root = (strcmp(dst_dir,"/")==0);

    int moved=0, first_err=0;
    for(int i=0;i<nnames;i++){
        const char *nm=names[i];
        if(!nm||!*nm||!strcmp(nm,".")||!strcmp(nm,"..")) continue;
        ntfschar *u=NULL; size_t ul=0;
        if(!to_ntfs(nm,&u,&ul)) continue;

        ntfs_inode *root=ntfs_inode_open(fs->vol, FILE_root);
        ntfs_inode *dst = dst_is_root ? root : lookup(fs->vol, dst_dir, NULL);  /* 每轮新开（上轮可能被消费）*/
        if(!dst||!root){
            if(dst && !dst_is_root) ntfs_inode_close(dst);
            if(root) ntfs_inode_close(root);
            free(u);
            if(!first_err) first_err=IMG_EIO;
            continue;
        }
        errno=0;
        MFT_REF ref=ntfs_inode_lookup_by_name(root,u,ul);
        if(!mftref_ok(ref)){
            if(!dst_is_root) ntfs_inode_close(dst);
            ntfs_inode_close(root); free(u);
            if(!first_err) first_err=IMG_ENOENT;
            continue;
        }
        ntfs_inode *ni=ntfs_inode_open(fs->vol, ref);
        if(!ni){
            if(!dst_is_root) ntfs_inode_close(dst);
            ntfs_inode_close(root); free(u);
            if(!first_err) first_err=IMG_EIO;
            continue;
        }

        int lr=ntfs_link(ni,dst,u,(u8)ul);                 /* ① 目标目录加同名链接（内存）*/
        int err=0;
        if(lr==0 && ntfs_inode_sync(dst)!=0){              /* ② ★ 落盘（本次事故的根因）*/
            err=errno?errno:EIO;
            lr=-1;
        }
        if(lr==0){
            char path[512]; snprintf(path,sizeof path,"/%s",nm);
            errno=0;
            int dr=ntfs_delete(fs->vol, path, ni, root, u, (u8)ul); /* ③ 消费 ni+root */
            ni=NULL; root=NULL;
            if(dr==0){ moved++; if(!dst_is_root) ntfs_inode_close(dst); free(u); continue; }
            err=errno?errno:EIO;
            /* ④ 回滚：把目标目录里多出来的同名链接删掉（fresh opens，它也会被消费）*/
            char dp[600]; snprintf(dp,sizeof dp,"%s/%s", dst_dir, nm);
            ntfs_inode *r2=ntfs_inode_open(fs->vol, FILE_root);
            ntfs_inode *d2=lookup(fs->vol, dst_dir, NULL);
            ntfschar *u2=NULL; size_t ul2=0;
            if(r2&&d2&&to_ntfs(nm,&u2,&ul2)){
                MFT_REF rf2=ntfs_inode_lookup_by_name(d2,u2,ul2);
                if(mftref_ok(rf2)){
                    ntfs_inode *n2=ntfs_inode_open(fs->vol, rf2);
                    if(n2){
                        (void)ntfs_delete(fs->vol, dp, n2, d2, u2, (u8)ul2);
                        n2=NULL;
                    }
                    d2=NULL;                              /* d2 被 ntfs_delete 消费 */
                }
            }
            if(r2) ntfs_inode_close(r2);
            if(d2) ntfs_inode_close(d2);
            free(u2);
        } else {
            /* link 或 sync 失败：链接没进盘 → 直接清理内存对象即可（根目录原样，无残留）*/
            ntfs_inode_close(ni); ntfs_inode_close(root);
        }
        if(!dst_is_root) ntfs_inode_close(dst);
        free(u);
        if(!first_err) first_err = (err==EPERM)?IMG_ENOTSUP:IMG_EIO;
    }
    if(moved>0) *moved_out=moved;
    if(moved>0) return IMG_OK;
    return first_err?first_err:IMG_ESTATE;
}



/* ================= 改名 / 移动（任意单条路径）=================
 * 与 img_ntfs_move_into 同一套机制：【新位置 ntfs_link → sync 父目录落盘 → 旧位置 ntfs_delete】。
 * ★ 顺序是"先加后减"：link count 全程 ≥1，中途失败最坏是"新旧两处都在"（重试即可），
 *   绝不会出现"名字没了、数据也没了"。这是把 ntfs_inode_sync 放在 delete 之前的原因
 *   （move_into 那次事故的根因就是少这一步 sync）。
 * ★ 为什么目录要先实测再放开：NTFS 把"我爹是谁"记在【子目录自己的 $I30 的 ".." 索引项】里，
 *   而 ntfs_link+ntfs_delete 只动父目录的索引 —— 搬完 ".." 仍指向旧父。
 *   实测结论见 tests/ntfs-move-test.sh（同一份代码里判目录）。
 */
static int split_abs(const char *abs,char *dir,size_t dcap,char *name,size_t ncap){
    const char *slash=strrchr(abs,'/');
    if(!slash||!slash[1]) return -1;                       /* 结尾是 '/' 或不带名 */
    size_t plen=(size_t)(slash-abs);
    if(plen==0) snprintf(dir,dcap,"/");                     /* 根下的文件：父="/"（lookup 认这个）*/
    else{ if(plen>=dcap) return -1; memcpy(dir,abs,plen); dir[plen]=0; }
    if(strlen(slash+1)>=ncap) return -1;
    snprintf(name,ncap,"%s",slash+1);
    return 0;
}

/* ---------- 目录移动要用的两个小工具（只用 libntfs-3g 的公开 API） ----------
 * ★ 关键事实（第一次实现就踩了）："." 与 ".." 【不是】inode 的 $FILE_NAME 属性，
 *   它们只是父目录……不，是【该目录自己的 $I30 索引里的两条索引项】。
 *   一个目录 inode 的 $FILE_NAME 只有它自己的名字（实测 ino=64 只有一条 "B"）。
 *   所以读写 ".." 必须走 $I30 索引：ntfs_index_ctx_get(ni,"$I30") + ntfs_index_lookup(键=名字)。
 *   索引项里 parent_directory 出现在两处：entry->key.file_name（排序键）与 ictx->data（值副本），
 *   两处都要改，否则读到的还是旧父。 */

static const ntfschar IMG_DOTDOT[2] = { const_cpu_to_le16('.'), const_cpu_to_le16('.') };

/* 造一个只带名字的 $I30 查找键（collate 只比名字，见 libntfs-3g/collate.c 的
 * ntfs_collate_file_name，其它字段留 0 就行）。调用方 free。 */
static FILE_NAME_ATTR *name_key(const ntfschar *nm, int nlen){
    size_t sz = 0x42 + (size_t)nlen*sizeof(ntfschar);
    FILE_NAME_ATTR *k = (FILE_NAME_ATTR*)calloc(1, sz);
    if(!k){ errno=ENOMEM; return NULL; }
    k->file_name_length = (u8)nlen;
    memcpy(k->file_name, nm, (size_t)nlen*sizeof(ntfschar));
    return k;
}

/* 在目录 sub 自己的 $I30 里定位 ".." 那条索引项。
 * 成功返回 IMG_OK 并把 ictx 留在"已找到"状态（调用方负责 ctx_put）。*/
static int dotdot_lookup(ntfs_inode *sub, ntfs_index_context **out_icx){
    *out_icx=NULL;
    FILE_NAME_ATTR *key = name_key(IMG_DOTDOT, 2);
    if(!key) return IMG_ENOMEM;
    ntfs_index_context *ictx = ntfs_index_ctx_get(sub, NTFS_INDEX_I30, 4);
    if(!ictx){ free(key); return IMG_EIO; }
    int lk = ntfs_index_lookup(key, (int)(0x42 + 2*sizeof(ntfschar)), ictx);
    int le = errno;
    size_t klen = 0x42 + 2*sizeof(ntfschar);
    memset(key,0,klen);            /* 复用同一块内存做第二次尝试的键 */
    free(key);
    if(lk){
        img_ev ev;
        if(img_ev_init(&ev,IMG_LV_DEBUG,"ntfs","dotdot_lookup",IMG_ENOENT,".. 索引项没找到")){
            img_ev_u64(&ev,"ino",sub->mft_no); img_ev_errno(&ev,le); img_ev_emit(&ev);
        }
        ntfs_index_ctx_put(ictx);
        return (le==ENOENT)?IMG_ENOENT:IMG_EIO;
    }
    if(!ictx->data || !ictx->data_len){
        img_ev ev;
        if(img_ev_init(&ev,IMG_LV_DEBUG,"ntfs","dotdot_nodata",IMG_ENOENT,".. 命中但没有数据段")){
            img_ev_u64(&ev,"ino",sub->mft_no);
            img_ev_u64(&ev,"data_len",(uint64_t)ictx->data_len); img_ev_emit(&ev);
        }
        ntfs_index_ctx_put(ictx);
        return IMG_ENOENT;
    }
    *out_icx = ictx;
    return IMG_OK;
}

/* 取一个目录的父 inode 号。两条来源，按可靠性依次尝试：
 *   ① 自己 $I30 里 ".." 索引项的 **mft_ref**（Windows 建的目录都有这一项）
 *   ② 自己 $FILE_NAME 属性里那条"本名"的 parent_directory
 *      （ntfs-3g 建的目录【没有 "."/".." 索引项】—— 上游只在 ntfs_readdir 里
 *       现造这两条，见 libntfs-3g/dir.c 的 dotdot[] 与 filldir 调用）*/
static int dir_parent_ino(ntfs_inode *sub, uint64_t *out){
    ntfs_index_context *ictx=NULL;
    if(dotdot_lookup(sub,&ictx)==IMG_OK){
        uint64_t p = ictx->entry ? MREF_LE(ictx->entry->indexed_file) : 0;
        if(!p){
            FILE_NAME_ATTR *fnx=(FILE_NAME_ATTR*)ictx->data;
            if(fnx) p = MREF_LE(fnx->parent_directory);
        }
        ntfs_index_ctx_put(ictx);
        if(p){ if(out) *out=p; return IMG_OK; }
    }
    /* 退到 ② */
    ntfs_attr_search_ctx *ctx=ntfs_attr_get_search_ctx(sub,NULL);
    if(!ctx) return IMG_EIO;
    int rc=IMG_ENOENT;
    while(!ntfs_attr_lookup(AT_FILE_NAME,NULL,0,0,0,NULL,0,ctx)){
        FILE_NAME_ATTR *fn=(FILE_NAME_ATTR*)((u8*)ctx->attr+le16_to_cpu(ctx->attr->value_offset));
        if(!fn->file_name_length) continue;                       /* 跳过 "."（ntfs-3g 偶见）*/
        if(fn->file_name_length==2
           && fn->file_name[0]==const_cpu_to_le16('.')
           && fn->file_name[1]==const_cpu_to_le16('.')) continue;
        uint64_t p=MREF_LE(fn->parent_directory);
        if(p){ if(out) *out=p; rc=IMG_OK; break; }
    }
    ntfs_attr_put_search_ctx(ctx);
    return rc;
}

/* 【实测修正 2026-10-23】三张真实 Windows 盘（diskA-Windows-UEFI-GPT、Win10 Xlite、
 * 我们自己灌的 w10b）上抽查 /Windows、/Users、/Windows/System32 等目录：
 *   dotdot_present 全是 false —— 也就是说这些卷的 $I30 里【没有 ".." 索引项】，
 *   "." 与 ".." 是枚举时现造的（ntfs-3g 的 ntfs_readdir 正是这么干的，
 *   见 libntfs-3g/dir.c 的 dotdot[] 与 filldir 调用）。
 * ⇒ 目录的父引用【权威来源是它自己 $FILE_NAME 里的 parent_directory】，
 *   而 ntfs_link + ntfs_delete 本来就会把它更新到新父 —— 这才是"搬目录安全"的真正理由。
 * ⇒ 下面这个函数是【保险】：万一某个卷（或某些第三方工具）真的写了 ".." 索引项，
 *   我们把它一起改掉；没有就直接返回"无需同步"。
 *
 * 把子目录 ".." 的父引用改成 newpar。若存在，改三处：
 *     ① 索引项头部的 indexed_file（该条目指向的 inode）        —— ".." 到底指向哪个 inode（最关键的一处）
 *     ② 索引项的排序键 file_name 里的 parent_directory（= 新父的父）
 *     ③ 索引项数据副本里的同一字段
 * 没有 ".." 索引项的目录（实测所有 Windows 卷都没有）→ *out_had_entry=false，返回 IMG_OK：
 * 父引用本来就在自己的 $FILE_NAME 里，link+delete 已经把它更新了。
 *
 * ★ 为什么必须我们自己补：上游【没有】"改父引用"的能力 ——
 *   libntfs-3g/dir.c 里 ntfs_link 的注释原文写着
 *   "FIXME: Create internal __ntfs_link ... Write ntfs_rename that uses __ntfs_link"，
 *   FUSE 层（src/ntfs-3g.c 的 ntfs_fuse_rename）对目录也只是 link+unlink。
 *   也就是说 ntfs-3g 自己挂载时 mv 一个目录，".." 同样会留在旧父上。
 *   这是上游能力缺口，不是我们写得笨 —— 找不到能直接调的现成函数。*/
static int dir_set_dotdot_parent(ntfs_inode *sub, ntfs_inode *newpar, bool *out_had_entry){
    if(out_had_entry) *out_had_entry=false;
    ntfs_index_context *ictx=NULL;
    int rc = dotdot_lookup(sub,&ictx);
    if(rc==IMG_ENOENT) return IMG_OK;          /* 这个目录本来就没有 ".." 索引项 */
    if(rc!=IMG_OK) return rc;

    MFT_REF nref = MK_LE_MREF(newpar->mft_no, le16_to_cpu(newpar->mrec->sequence_number));
    /* 新父的父（".." 项里 FILE_NAME 的 parent_directory 应该是爷爷）*/
    uint64_t gp=0;
    (void)dir_parent_ino(newpar,&gp);
    MFT_REF gref = gp ? MK_LE_MREF(gp,0) : nref;

    if(ictx->entry){
        ictx->entry->indexed_file = nref;                                   /* ① */
        FILE_NAME_ATTR *k=&ictx->entry->key.file_name;
        if(k) k->parent_directory = (le64)gref;                        /* ② */
    }
    FILE_NAME_ATTR *fnx=(FILE_NAME_ATTR*)ictx->data;
    if(fnx) fnx->parent_directory = (le64)gref;                        /* ③ */
    ntfs_index_entry_mark_dirty(ictx);
    ntfs_index_ctx_put(ictx);
    ntfs_inode_mark_dirty(sub);
    if(out_had_entry) *out_had_entry=true;
    if(ntfs_inode_sync(sub)!=0) return IMG_EIO;
    return IMG_OK;
}

/* 目标父目录是不是在"被搬目录"的子树里（含搬进自己）。
 * 从 dpar 沿 ".." 一级级往上走，撞见 anc_ino 就是环。走 64 层封顶防病态索引。*/
static int dir_is_self_or_ancestor(ntfs_volume *vol, uint64_t anc_ino, ntfs_inode *dpar){
    ntfs_inode *cur=dpar;
    bool owned=false;
    int hit=0;
    for(int i=0;i<64 && cur;i++){
        if(cur->mft_no==anc_ino){ hit=1; break; }
        uint64_t p=0;
        if(dir_parent_ino(cur,&p)!=IMG_OK) break;
        if(p==cur->mft_no || p==0) break;              /* 到根了 */
        if(owned) ntfs_inode_close(cur);
        cur=ntfs_inode_open(vol,p);
        owned=true;
    }
    if(owned && cur) ntfs_inode_close(cur);
    return hit;
}

/* ---------- 目录父引用自洽检查（只读，给 ntfs.parent 与测试用） ----------
 * 三个问题一次问清：
 *   ① 这个目录的 ".." 指向哪个 inode（parent_ino）
 *   ② 按路径去找它的父，是不是同一个（same_parent）—— 不是就说明 ".." 是陈的
 *   ③ 父目录的索引里，这个名字是否确实指回本目录（backlink_ok）
 * 目录移动（ntfs.move 搬目录、ntfs.rename 的 move_into）之后必须三项全真。*/
int img_ntfs_dir_parent_check(img_ntfs_fs *fs, const char *abs_path,
                              uint64_t *out_ino, uint64_t *out_parent_ino,
                              uint64_t *out_parent_by_path, bool *out_backlink_ok,
                              bool *out_dotdot_present){
    if(!fs||!fs->mounted||!abs_path||abs_path[0]!='/') return IMG_EINVAL;
    char pdir[512], pname[256];
    bool is_root = (!strcmp(abs_path,"/") || !strcmp(abs_path,"\\"));
    if(!is_root && split_abs(abs_path,pdir,sizeof pdir,pname,sizeof pname)!=0) return IMG_EINVAL;
    if(is_root){ snprintf(pdir,sizeof pdir,"/"); pname[0]=0; }
    ntfs_inode *ni=lookup(fs->vol,abs_path,NULL);
    if(!ni) return IMG_ENOENT;
    if(!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)){ ntfs_inode_close(ni); return IMG_ENOTSUP; }
    if(ni->mft_no==FILE_root){          /* 根目录没有父：自指，直接给结论 */
        if(out_ino) *out_ino=ni->mft_no;
        if(out_parent_ino) *out_parent_ino=ni->mft_no;
        if(out_parent_by_path) *out_parent_by_path=ni->mft_no;
        if(out_backlink_ok) *out_backlink_ok=true;
        if(out_dotdot_present) *out_dotdot_present=false;
        ntfs_inode_close(ni);
        return IMG_OK;
    }
    uint64_t pino=0;
    { ntfs_index_context *dd=NULL;
      bool present = (dotdot_lookup(ni,&dd)==IMG_OK);
      if(present && dd) ntfs_index_ctx_put(dd);
      if(out_dotdot_present) *out_dotdot_present=present; }
    int rc=dir_parent_ino(ni,&pino);
    if(out_ino) *out_ino=ni->mft_no;
    if(out_parent_ino) *out_parent_ino=pino;
    if(out_backlink_ok) *out_backlink_ok=false;
    if(out_parent_by_path) *out_parent_by_path=0;
    ntfs_inode *par=lookup(fs->vol,pdir[0]?pdir:"/",NULL);
    if(!par){ ntfs_inode_close(ni); return IMG_ENOENT; }
    if(out_parent_by_path) *out_parent_by_path=par->mft_no;
    ntfschar *u=NULL; size_t ul=0;
    if(to_ntfs(pname,&u,&ul)){
        errno=0;
        MFT_REF ref=ntfs_inode_lookup_by_name(par,u,ul);
        if(mftref_ok(ref) && out_backlink_ok) *out_backlink_ok = (MREF_LE(ref)==ni->mft_no);
        free(u);
    }
    ntfs_inode_close(par);
    ntfs_inode_close(ni);
    return rc;
}



int img_ntfs_move_path(img_ntfs_fs *fs,const char *src_abs,const char *dst_abs,bool overwrite){
    if(!fs||!fs->mounted||!src_abs||!dst_abs) return IMG_EINVAL;
    if(src_abs[0]!='/'||dst_abs[0]!='/'||!src_abs[1]||!dst_abs[1]) return IMG_EINVAL;
    static char sdir[512],sname[256],ddir[512],dname[256];
    if(split_abs(src_abs,sdir,sizeof sdir,sname,sizeof sname)!=0) return IMG_EINVAL;
    if(split_abs(dst_abs,ddir,sizeof ddir,dname,sizeof dname)!=0) return IMG_EINVAL;

    /* ★★ 同目录改名时 spar 与 dpar 必须是【同一个 ntfs_inode 对象】★★
     *   ntfs-3g 的 ntfs_inode_open() 对同一个 MFT 号会各开一份独立副本
     *   （NIDATA 缓存是"取走即移出"，第二次 open 就重新读盘）。
     *   两份的 $I30 索引根各是各的：ntfs_link 写 A 份并落盘，ntfs_delete 却拿
     *   B 份的【旧索引】去删 → ntfs_index_remove 报 EIO；最坏把这个子目录的
     *   索引写空（2026-10-23 真机实测：/dir/a.txt → /dir/b.txt 失败且 /dir 变空，
     *   而根目录下的同样改名是好的 —— 根目录的索引项在磁盘块里，看不出问题）。
     *   所以：同目录只开一份，dpar 就是 spar；也绝不 close 两次。*/
    bool same_dir = (strcmp(sdir,ddir)==0);
    ntfs_inode *spar=lookup(fs->vol,sdir,NULL);
    if(!spar) return IMG_ENOENT;                           /* 源父目录不存在 */
    ntfs_inode *dpar=spar;
    bool dpar_extra=false;                                 /* dpar 是另开的一份 → 要单独 close */
    if(!same_dir){
        dpar=lookup(fs->vol,ddir,NULL);
        if(!dpar){
            /* 目标父目录：不存在就建（与 ntfs.put 的 --mkdir 行为一致）*/
            if(img_ntfs_mkdir_p(fs,ddir)!=IMG_OK){ ntfs_inode_close(spar); return IMG_EIO; }
            dpar=lookup(fs->vol,ddir,NULL);
            if(!dpar){ ntfs_inode_close(spar); return IMG_EIO; }
        }
        dpar_extra=true;
    }
    ntfschar *su=NULL; size_t sul=0;
    ntfschar *du=NULL; size_t dul=0;
    if(!to_ntfs(sname,&su,&sul)){ if(dpar_extra) ntfs_inode_close(dpar); ntfs_inode_close(spar); return IMG_EINVAL; }
    errno=0;
    MFT_REF sref=ntfs_inode_lookup_by_name(spar,su,sul);
    if(!mftref_ok(sref)){                                   /* 源不存在 */
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su);
        return IMG_ENOENT;
    }
    ntfs_inode *ni=ntfs_inode_open(fs->vol,sref);
    if(!ni){
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su);
        return IMG_EIO;
    }
    bool isdir=(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)!=0;
    /* 目录现在放开：搬完会同步子目录 ".." 的父引用（见 dir_set_dotdot_parent）。
     * 文件不受影响（文件没有父引用这回事）。
     * 两条硬规矩：① 不许把目录搬进它自己的子树（会造出环，Windows 直接懵）；
     *             ② 目标已存在且是目录 → 依旧拒绝（IMG_ESTATE）。*/
    if(isdir && !same_dir && dir_is_self_or_ancestor(fs->vol, ni->mft_no, dpar)){
        ntfs_inode_close(ni);
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su);
        return IMG_EINVAL;
    }
    if(!to_ntfs(dname,&du,&dul)){
        ntfs_inode_close(ni);
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su);
        return IMG_EINVAL;
    }
    errno=0;
    MFT_REF dref=ntfs_inode_lookup_by_name(dpar,du,dul);
    if(mftref_ok(dref) && dref==sref){                      /* 已指向同一 inode（大小写写法不同）→ 成功 */
        ntfs_inode_close(ni);
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su); free(du);
        return IMG_OK;
    }
    if(mftref_ok(dref)){
        if(!overwrite){
            ntfs_inode_close(ni);
            if(dpar_extra) ntfs_inode_close(dpar);
            ntfs_inode_close(spar); free(su); free(du);
            return IMG_EEXISTS;                             /* 目标已存在：不悄悄覆盖 */
        }
        /* 覆盖：只能覆盖【文件】—— 目标若是目录，删它会连累整棵子树，一律拒绝 */
        ntfs_inode *dst_ni=ntfs_inode_open(fs->vol,dref);
        bool dst_isdir = dst_ni && (dst_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY);
        if(dst_ni) ntfs_inode_close(dst_ni);
        if(dst_isdir){
            /* 与"源是目录"分开返回码（IMG_ESTATE）：一个是政策不放行，一个是
             * 绝不允许拿文件顶掉目录 —— App 的文案与处理完全不同 */
            ntfs_inode_close(ni);
            if(dpar_extra) ntfs_inode_close(dpar);
            ntfs_inode_close(spar); free(su); free(du);
            return IMG_ESTATE;
        }
        int r=img_ntfs_rm(fs,dst_abs);
        if(r!=IMG_OK){
            ntfs_inode_close(ni);
            if(dpar_extra) ntfs_inode_close(dpar);
            ntfs_inode_close(spar); free(su); free(du);
            return r;
        }
        /* rm 动过索引：把目录 inode 全部丢掉重开一份（两种情形合起来只有一份）*/
        if(dpar_extra){ ntfs_inode_close(dpar); dpar=NULL; }
        ntfs_inode_close(spar); spar=NULL; dpar=NULL;
        spar=lookup(fs->vol,sdir,NULL);
        if(!spar){ ntfs_inode_close(ni); free(su); free(du); return IMG_EIO; }
        if(same_dir){ dpar=spar; dpar_extra=false; }
        else {
            dpar=lookup(fs->vol,ddir,NULL);
            if(!dpar){ ntfs_inode_close(ni); ntfs_inode_close(spar); free(su); free(du); return IMG_EIO; }
            dpar_extra=true;
        }
    }
    /* 逐步骤 DEBUG 留痕（只在 --log-level=debug 时产生，零成本否则）：
     * 真机上排查 move 只能靠这个 —— 一个 EIO 后面可能站着四种完全不同的原因。*/
    { img_ev ev;
      if(img_ev_init(&ev,IMG_LV_DEBUG,"ntfs","move_plan",IMG_OK,"move 计划")){
          img_ev_str(&ev,"src",src_abs); img_ev_str(&ev,"dst",dst_abs);
          img_ev_str(&ev,"sdir",sdir); img_ev_str(&ev,"ddir",ddir);
          img_ev_str(&ev,"sname",sname); img_ev_str(&ev,"dname",dname);
          img_ev_u64(&ev,"same_dir",same_dir?1u:0u);
          img_ev_emit(&ev);
      } }
    /* ① 新位置加链接 */
    if(ntfs_link(ni,dpar,du,(u8)dul)!=0){
        int e=errno;
        img_ev ev;
        if(img_ev_init(&ev,IMG_LV_ERROR,"ntfs","move_link_fail",IMG_EIO,"加链接失败")){
            img_ev_str(&ev,"src",src_abs); img_ev_str(&ev,"dst",dst_abs);
            img_ev_errno(&ev,e); img_ev_emit(&ev);
        }
        ntfs_inode_close(ni);
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su); free(du);
        return (e==EDQUOT||e==ENOSPC)?IMG_EFULL:IMG_EIO;
    }
    { img_ev ev;
      if(img_ev_init(&ev,IMG_LV_DEBUG,"ntfs","move_linked",IMG_OK,"新位置已加链接")){
          img_ev_str(&ev,"dst",dst_abs); img_ev_emit(&ev);
      } }
    /* ② ★ 先把新父目录同步落盘，再动旧目录项（顺序不能反）*/
    if(ntfs_inode_sync(dpar)!=0){
        int e=errno;
        img_ev ev;
        if(img_ev_init(&ev,IMG_LV_ERROR,"ntfs","move_sync_fail",IMG_EIO,"新父目录落盘失败")){
            img_ev_str(&ev,"src",src_abs); img_ev_str(&ev,"dst",dst_abs);
            img_ev_errno(&ev,e); img_ev_emit(&ev);
        }
        ntfs_inode_close(ni);
        if(dpar_extra) ntfs_inode_close(dpar);
        ntfs_inode_close(spar); free(su); free(du);
        return IMG_EIO;
    }
    /* ②′ 目录换了父 → 同步子目录 ".." 的父引用；失败就把刚加的链接撤掉，回到原样 */
    if(isdir && !same_dir){
        bool had_dd=false;
        int prc=dir_set_dotdot_parent(ni, dpar, &had_dd);
        if(prc!=IMG_OK){
            img_ev ev;
            if(img_ev_init(&ev,IMG_LV_ERROR,"ntfs","move_dotdot_fail",prc,"同步 .. 失败，撤销新链接")){
                img_ev_str(&ev,"src",src_abs); img_ev_str(&ev,"dst",dst_abs); img_ev_emit(&ev);
            }
            /* ★ 顺序很重要：先把 ni 关掉，再另开一份去撤销新链接。
             *   ntfs_inode_open 对同一 inode 会给【独立副本】，两份同时在世
             *   就可能把旧的 link_count 写回去（本轮刚在"同目录改名"上栽过一次）。*/
            ntfs_inode_close(ni); ni=NULL;
            if(dpar_extra) ntfs_inode_close(dpar);
            { ntfs_inode *bk=ntfs_inode_open(fs->vol, sref);
              ntfs_inode *nd=lookup(fs->vol,ddir,NULL);
              if(bk && nd) (void)ntfs_delete(fs->vol,dst_abs,bk,nd,du,(u8)dul);
              else { if(bk) ntfs_inode_close(bk); if(nd) ntfs_inode_close(nd); } }
            free(su); free(du);
            ntfs_inode_close(spar);
            return prc;
        }
    }
    if(dpar_extra) ntfs_inode_close(dpar);
    { img_ev ev;
      if(img_ev_init(&ev,IMG_LV_DEBUG,"ntfs","move_synced",IMG_OK,"新父已落盘，准备删旧")){
          img_ev_str(&ev,"src",src_abs); img_ev_emit(&ev);
      } }
    /* ③ 删旧目录项：ntfs_delete 会【消费】ni 与 spar，之后绝不能再 close/free */
    errno=0;
    int dr=ntfs_delete(fs->vol,src_abs,ni,spar,su,(u8)sul);
    ni=NULL; spar=NULL;
    free(du);
    if(dr!=0){
        int e=errno;
        img_ev ev;
        if(img_ev_init(&ev,IMG_LV_ERROR,"ntfs","move_delete_fail",IMG_EIO,"删旧目录项失败")){
            img_ev_str(&ev,"src",src_abs); img_ev_str(&ev,"dst",dst_abs);
            img_ev_errno(&ev,e); img_ev_emit(&ev);
        }
        /* 旧项没删掉 → 新旧两处都在（数据完好），如实报失败让调用方重试 */
        return (e==ENOENT)?IMG_ENOENT:IMG_EIO;
    }
    free(su);
    return IMG_OK;
}

/* 卷是否被标脏。用【只读】挂载 —— 脏卷本身能只读挂上，我们要读的就是它的 flags。
 * 脏到连只读都挂不上 → 当作脏（宁可提示重格式化，也别让人对着 WIM-APPLY-FAIL 猜）。*/
int img_ntfs_dirty_check(img_qcow2 *q,uint64_t part_lba,uint64_t part_bytes,bool *out_dirty){
    if(out_dirty) *out_dirty=false;
    if(!q||!part_bytes) return IMG_EINVAL;
    if(img_ntfs_probe(q,part_lba,NULL)!=IMG_OK) return IMG_EFORMAT;
    img_ntfs_ctx *ctx=img_ntfs_ctx_new(q,part_lba,part_bytes);
    if(!ctx) return IMG_EIO;
    img_ntfs_ctx_activate(q,part_lba,part_bytes);
    const char *dn=img_dev_placeholder(); if(!dn) dn="imgtool-dev";
    struct ntfs_device *dev=ntfs_device_alloc(dn,0,img_ntfs_device_ops(),NULL);
    if(!dev){ img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx); return IMG_EIO; }
    ntfs_volume *vol=ntfs_device_mount(dev, NTFS_MNT_RDONLY|NTFS_MNT_IGNORE_HIBERFILE);
    if(!vol){
        ntfs_device_free(dev); img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx);
        if(out_dirty) *out_dirty=true;
        return IMG_OK;
    }
    if(out_dirty) *out_dirty=(le16_to_cpu(vol->flags) & VOLUME_IS_DIRTY)!=0;
    ntfs_umount(vol,false);
    img_ntfs_ctx_deactivate();
    img_ntfs_ctx_free(ctx);
    return IMG_OK;
}

/* 灌入前只读预检：卷脏 + 根目录项数。一次挂载问两件事（挂载本身有成本）。
 * 列目录复用 img_ntfs_list_dir —— 只读卷上它同样能工作（只是不能写）。*/
static void precheck_count_cb(void *ud,const char *name,bool is_dir,uint64_t size){
    (void)name;(void)is_dir;(void)size; (*(int*)ud)++;
}
int img_ntfs_precheck(img_qcow2 *q,uint64_t part_lba,uint64_t part_bytes,
                      bool *out_dirty,int *out_root_entries){
    if(out_dirty) *out_dirty=false;
    if(out_root_entries) *out_root_entries=0;
    if(!q||!part_bytes) return IMG_EINVAL;
    if(img_ntfs_probe(q,part_lba,NULL)!=IMG_OK) return IMG_EFORMAT;
    img_ntfs_ctx *ctx=img_ntfs_ctx_new(q,part_lba,part_bytes);
    if(!ctx) return IMG_EIO;
    img_ntfs_ctx_activate(q,part_lba,part_bytes);
    const char *dn=img_dev_placeholder(); if(!dn) dn="imgtool-dev";
    struct ntfs_device *dev=ntfs_device_alloc(dn,0,img_ntfs_device_ops(),NULL);
    if(!dev){ img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx); return IMG_EIO; }
    ntfs_volume *vol=ntfs_device_mount(dev, NTFS_MNT_RDONLY|NTFS_MNT_IGNORE_HIBERFILE);
    if(!vol){
        ntfs_device_free(dev); img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx);
        if(out_dirty) *out_dirty=true;
        return IMG_OK;
    }
    if(out_dirty) *out_dirty=(le16_to_cpu(vol->flags) & VOLUME_IS_DIRTY)!=0;
    if(out_root_entries){
        img_ntfs_fs tmp; memset(&tmp,0,sizeof tmp);
        tmp.vol=vol; tmp.mounted=true;
        int n=0;
        if(img_ntfs_list_dir(&tmp,"/",precheck_count_cb,&n)==IMG_OK) *out_root_entries=n;
    }
    ntfs_umount(vol,false);
    img_ntfs_ctx_deactivate();
    img_ntfs_ctx_free(ctx);
    return IMG_OK;
}
