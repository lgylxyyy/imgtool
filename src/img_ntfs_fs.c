#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_ntfs_fs.c — NTFS 文件系统写入（Stage B）
 * 内部用 libntfs-3g 的公开 API（见 include/img_ntfs_fs.h 的决策记录） */
#include "img_ntfs_fs.h"
#include "img_event.h"
#include "ntfs_bridge.h"
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
    int dfd = open("imgtool-dev", O_RDWR|O_CREAT|O_TRUNC, 0666);
    if(dfd>=0) close(dfd);

    struct ntfs_device *dev = ntfs_device_alloc("imgtool-dev", 0, img_ntfs_device_ops(), NULL);
    if(!dev){ img_ntfs_ctx_deactivate(); img_ntfs_ctx_free(ctx); return NULL; }

    ntfs_volume *vol = ntfs_device_mount(dev, 0);   /* 0 = 读写 */
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
        /* ★ ntfs_umount 内部已 ntfs_device_free()，这里绝不能再调一次 */
        ntfs_umount(fs->vol, false);
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
        MFT_REF ref = ntfs_inode_lookup_by_name(cur, u, ul);
        int lerr = (ref==(MFT_REF)-1) ? errno : 0;
        free(u);
        if(!mftref_ok(ref)){
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
        MFT_REF ref = ntfs_inode_lookup_by_name(cur, u, ul);
        int lerr = (ref==(MFT_REF)-1) ? errno : 0;
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

    int moved=0, first_err=0;
    for(int i=0;i<nnames;i++){
        const char *nm=names[i];
        if(!nm||!*nm||!strcmp(nm,".")||!strcmp(nm,"..")) continue;
        ntfschar *u=NULL; size_t ul=0;
        if(!to_ntfs(nm,&u,&ul)) continue;

        ntfs_inode *dst=lookup(fs->vol, dst_dir, NULL);     /* 每轮新开（上轮可能被消费）*/
        ntfs_inode *root=ntfs_inode_open(fs->vol, FILE_root);
        if(!dst||!root){
            if(dst) ntfs_inode_close(dst);
            if(root) ntfs_inode_close(root);
            free(u);
            if(!first_err) first_err=IMG_EIO;
            continue;
        }
        errno=0;
        MFT_REF ref=ntfs_inode_lookup_by_name(root,u,ul);
        if(!mftref_ok(ref)){
            ntfs_inode_close(dst); ntfs_inode_close(root); free(u);
            if(!first_err) first_err=IMG_ENOENT;
            continue;
        }
        ntfs_inode *ni=ntfs_inode_open(fs->vol, ref);
        if(!ni){
            ntfs_inode_close(dst); ntfs_inode_close(root); free(u);
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
            if(dr==0){ moved++; ntfs_inode_close(dst); free(u); continue; }
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
        ntfs_inode_close(dst);
        free(u);
        if(!first_err) first_err = (err==EPERM)?IMG_ENOTSUP:IMG_EIO;
    }
    if(moved>0) *moved_out=moved;
    if(moved>0) return IMG_OK;
    return first_err?first_err:IMG_ESTATE;
}
