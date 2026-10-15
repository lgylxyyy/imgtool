#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* img_ntfs_fs.c — NTFS 文件系统写入（Stage B）
 * 内部用 libntfs-3g 的公开 API（见 include/img_ntfs_fs.h 的决策记录） */
#include "img_ntfs_fs.h"
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
    s64 done=0; int rc=IMG_OK;
    for(;;){
        ssize_t r = read(fd, buf, CHUNK);
        if(r<0){ rc=IMG_EIO; break; }
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
