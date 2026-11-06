/* img_winre.c — WinRE 状态检测（只读）。口径见 include/img_winre.h 头注释。 */
#include "img_winre.h"
#include "img_ntfs_fs.h"
#include "img_identify.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *WIM_VARIANTS[] = { "Winre.wim", "winre.wim", "WINRE.WIM" };

static bool ntfs_is_rec(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_RECOVERY, 8)==0;
    /* MBR 0x27：活动分区是 SR，不是恢复分区（bootcheck 的老结论，照用）*/
    if(p->status & 0x80) return false;
    return p->type == IMG_MBR_RECOVERY;
}
static bool ntfs_is_basic(const img_partition_t *p){
    if(p->type_guid[0]) return strncmp(p->type_guid, IMG_GUID_BASIC, 8)==0;
    return p->type == IMG_MBR_NTFS_EXFAT || p->type == IMG_MBR_RECOVERY;
}

/* 系统分区：带 \Windows\System32\winload.* 的那个；退化到"最大的 NTFS" */
static int find_os(const img_parttable_t *t, uint64_t *lba, uint64_t *bytes){
    int best=-1; uint64_t bestsec=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used || !ntfs_is_basic(p)) continue;
        uint64_t sec=p->last_lba-p->first_lba+1;
        if(sec>bestsec){ bestsec=sec; best=i; }
    }
    if(best<0) return -1;
    *lba=t->parts[best].first_lba;
    *bytes=bestsec*512u;
    return best;
}

/* WIM XML 读（Winre.wim 通常 200~500MB，只读头+XML）*/
typedef struct { img_ntfs_fs *fs; const char *path; } rdw_t;
static int rd_ntfs(void *ud, uint64_t off, void *buf, size_t len){
    rdw_t *r=(rdw_t*)ud;
    size_t got=0;
    int rc=img_ntfs_read_range(r->fs,r->path,off,buf,len,&got);
    if(rc!=IMG_OK) return rc;
    return got==len?IMG_OK:IMG_EIO;
}
static void read_version(img_ntfs_fs *fs, const char *path, img_winre_status_t *st){
    rdw_t r={fs,path};
    img_wim_image_t imgs[4]; int n=0; char err[128]="";
    if(img_wim_identify(rd_ntfs,&r,imgs,4,&n,err,sizeof err)==IMG_OK && n>0){
        st->winre_version_known=true;
        st->winre_major=imgs[0].major; st->winre_minor=imgs[0].minor; st->winre_build=imgs[0].build;
        snprintf(st->winre_arch,sizeof st->winre_arch,"%s",imgs[0].arch);
    }
}

static bool ntfs_first_of(img_ntfs_fs *fs, const char *const *paths, int n, char *hit, size_t hitcap){
    for(int i=0;i<n;i++) if(img_ntfs_path_exists(fs,paths[i])){
        if(hit && hitcap) snprintf(hit,hitcap,"%s",paths[i]);
        return true;
    }
    return false;
}

int img_winre_status(img_qcow2 *q, const img_parttable_t *t,
                     uint64_t os_lba, img_winre_status_t *st,
                     char *err, size_t errcap){
    if(err&&errcap) err[0]=0;
    if(!q||!t||!st) return IMG_EINVAL;
    memset(st,0,sizeof *st);

    /* ---- ① 恢复分区 ---- */
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used || !ntfs_is_rec(p)) continue;
        st->recovery_partition_present=true;
        st->rec_lba=p->first_lba;
        st->rec_bytes=(p->last_lba-p->first_lba+1)*512u;
        st->rec_is_gpt=(p->type_guid[0]!=0);
        st->rec_attr=p->attributes;
        st->rec_attr_ok = st->rec_is_gpt &&
            (p->attributes & IMG_GPT_ATTR_PLATFORM_REQUIRED) &&
            (p->attributes & IMG_GPT_ATTR_NO_DRIVE_LETTER);
        break;
    }

    /* ---- ② 系统分区 ---- */
    if(!os_lba){
        if(find_os(t,&st->os_lba,&st->os_bytes)<0){ st->os_found=false; }
        else st->os_found=true;
    } else {
        st->os_lba=os_lba;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].first_lba==os_lba){
                st->os_bytes=(t->parts[i].last_lba-t->parts[i].first_lba+1)*512u; break;
            }
        st->os_found=(st->os_bytes!=0);
    }

    /* ---- ③ 三处找 Winre.wim ---- */
    static const char *rec_paths[3]={
        "/Recovery/WindowsRE/Winre.wim","/Recovery/WindowsRE/winre.wim","/Recovery/WindowsRE/WINRE.WIM"};
    static const char *sys_paths[3]={
        "/Windows/System32/Recovery/Winre.wim","/Windows/System32/Recovery/winre.wim",
        "/Windows/System32/Recovery/WINRE.WIM"};
    static const char *os_rec_paths[3]={
        "/Recovery/WindowsRE/Winre.wim","/Recovery/WindowsRE/winre.wim","/Recovery/WindowsRE/WINRE.WIM"};

    img_ntfs_fs *rfs=NULL;              /* 恢复分区挂载（存在才挂）*/
    if(st->recovery_partition_present){
        rfs=img_ntfs_mount_rw(q,st->rec_lba,st->rec_bytes);
        if(!rfs){
            /* 分区在但挂不上（没格式化/不是 NTFS）——对 WinRE 来说就是空的 */
            st->recovery_empty=true;
            snprintf(st->note,sizeof st->note,"恢复分区挂不上（没格式化或不是 NTFS）");
        } else {
            char hit[300];
            if(ntfs_first_of(rfs,rec_paths,3,hit,sizeof hit)){
                st->winre_present=true;
                snprintf(st->winre_location,sizeof st->winre_location,"recovery");
                snprintf(st->wim_path,sizeof st->wim_path,"%s",hit);
                st->wim_part_lba=st->rec_lba;
                img_ntfs_file_size(rfs,hit,&st->wim_bytes);
                read_version(rfs,hit,st);
            } else {
                st->recovery_empty=true;   /* ★ 分区在但空的：只报字段，不自动建（用户拍板）*/
            }
            /* boot.sdi / ReAgent.xml（都按 Winre 同目录找）*/
            st->bootsdi_present = img_ntfs_path_exists(rfs,"/Recovery/WindowsRE/boot.sdi")
                               || img_ntfs_path_exists(rfs,"/Recovery/WindowsRE/Boot.sdi")
                               || img_ntfs_path_exists(rfs,"/boot.sdi");
            st->reagent_present  = img_ntfs_path_exists(rfs,"/Recovery/WindowsRE/ReAgent.xml")
                               || img_ntfs_path_exists(rfs,"/ReAgent.xml");
            img_ntfs_umount(rfs);
        }
    }

    /* ---- ④ 系统分区两处（③没命中才看）---- */
    if(!st->winre_present && st->os_found){
        img_ntfs_fs *sfs=img_ntfs_mount_rw(q,st->os_lba,st->os_bytes);
        if(!sfs){
            if(err&&errcap&&!err[0]) snprintf(err,errcap,"系统分区挂不上（可能不是 NTFS 或还没有系统）");
        } else {
            char hit[300];
            if(ntfs_first_of(sfs,sys_paths,3,hit,sizeof hit)){
                st->winre_present=true;
                snprintf(st->winre_location,sizeof st->winre_location,"system32");
                snprintf(st->wim_path,sizeof st->wim_path,"%s",hit);
                st->wim_part_lba=st->os_lba;
                img_ntfs_file_size(sfs,hit,&st->wim_bytes);
                read_version(sfs,hit,st);
            } else if(ntfs_first_of(sfs,os_rec_paths,3,hit,sizeof hit)){
                st->winre_present=true;
                snprintf(st->winre_location,sizeof st->winre_location,"os_recovery");
                snprintf(st->wim_path,sizeof st->wim_path,"%s",hit);
                st->wim_part_lba=st->os_lba;
                img_ntfs_file_size(sfs,hit,&st->wim_bytes);
                read_version(sfs,hit,st);
            }
            /* 系统分区侧的 boot.sdi / ReAgent.xml（winre 在 system32 时它们也在那）*/
            if(st->winre_present && !strcmp(st->winre_location,"system32")){
                if(!st->bootsdi_present)
                    st->bootsdi_present = img_ntfs_path_exists(sfs,"/Windows/System32/Recovery/boot.sdi");
                if(!st->reagent_present)
                    st->reagent_present = img_ntfs_path_exists(sfs,"/Windows/System32/Recovery/ReAgent.xml");
            }
            /* OS 版本（顺手，给 App 的卡片副标题）*/
            { img_winver_t v; char e2[128]="";
              if(img_win_version(q,st->os_lba,st->os_bytes,&v,e2,sizeof e2)==IMG_OK){
                  int b=v.consensus_build?v.consensus_build:v.current_build;
                  if(v.kernel_major>0)
                      snprintf(st->os_version,sizeof st->os_version,"%d.%d.%d",
                               v.kernel_major,v.kernel_minor,b);
                  else
                      snprintf(st->os_version,sizeof st->os_version,"%s.%d",
                               v.current_version[0]?v.current_version:"10",b);
              }
            }
            img_ntfs_umount(sfs);
        }
    }

    return IMG_OK;
}
