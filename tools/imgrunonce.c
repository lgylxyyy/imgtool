/* imgrunonce — 离线给"已装好的 Windows"写一次性启动脚本（HKLM RunOnce）
 *
 *   用法: imgrunonce <qcow2> <系统分区lba> <脚本(宿主路径)> <NTFS内目标路径> [值名]
 *         例: imgrunonce disk.qcow2 206848 /tmp/winre-once.cmd \
 *                 /Windows/System32/imgtool/once.cmd WinRE-Enable
 *
 * ★ 为什么用 RunOnce（微软文档）：
 *   - SetupComplete.cmd 只在"部署阶段"跑一次（装完、登录界面出现之前）——
 *     克隆一块【已经进过桌面】的盘时，那次机会已经用掉了，永远不会再执行。
 *   - HKLM\...\RunOnce：【管理员下次登录时】执行，且值在执行前就被删除
 *     （值名加 '!' 前缀 = 成功后才删、失败下次重试）→ 天然"只跑一次、自动清理"。
 *   - （Startup 目录是"每次登录都跑"且不自删，需要脚本 del "%~f0" 自杀，且要求登录。）
 *
 * ★ 做法（全程离线，改完即用）：
 *   ① 脚本文件写进系统分区（NTFS）
 *   ② 读 \Windows\System32\config\SOFTWARE hive → 临时文件 → libhivex 打开
 *   ③ 在 Software\Microsoft\Windows\CurrentVersion\RunOnce 下加 REG_SZ 值
 *      （数据 = 脚本的 Windows 路径，如 "C:\Windows\System32\imgtool\once.cmd"）
 *   ④ 提交 → 原样写回 NTFS
 * ★ 脚本本身建议自删（del "%~f0"）→ 连文件都不残留。
 */
#define _POSIX_C_SOURCE 200809L
#include "img_event.h"
#include "img_io.h"
#include "img_ntfs_fs.h"
#include "img_partition.h"
#include "img_qcow2.h"
#include <hivex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void die(const char *msg, int rc){
    fprintf(stderr, "❌ %s: %s\n", msg, img_strerror(rc));
    exit(1);
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    img_log_cli(argc, argv, "imgrunonce");
    if(argc<5){
        fprintf(stderr,"用法: %s <qcow2> <系统分区lba> <脚本(宿主路径)> <NTFS内目标路径> [值名]\n", argv[0]);
        return 2;
    }
    const char *img=argv[1];
    uint64_t lba = strtoull(argv[2],NULL,0);
    /* ★ --winre-auto 模式（2026-10-19 用户定的新 WinRE 方案）：
       winre.wim+boot.sdi 放进系统分区 \Windows\System32\Recovery\（Windows 原生
       机制：reagentc /enable 找不到 RE 分区时默认从这里取），RunOnce 跑
       reagentc /enable —— 不依赖恢复分区检测（旧方案 RE 分区系统认不到）。
       用法: imgrunonce <qcow2> <系统分区lba> --winre-auto [--winre-from=<qcow2>:<RE分区lba>] */
    int winre_auto = (argc>3 && !strcmp(argv[3],"--winre-auto"));
    const char *src=NULL, *dst="/Windows/System32/imgtool/winre-once.cmd";
    const char *valname="WinRE-Enable";
    const char *wr_from=NULL;
    if(winre_auto){
        for(int i=4;i<argc;i++)
            if(!strncmp(argv[i],"--winre-from=",13)) wr_from=argv[i]+13;
    } else {
        if(argc<5){ fprintf(stderr,"用法见文件头\n"); return 2; }
        src=argv[3]; dst=argv[4];
        valname = (argc>5 && argv[5][0]) ? argv[5] : "imgtool-one-shot";
    }

    /* 打开镜像 + 挂载 NTFS */
    img_io_opts_t io={0}; io.writable=true;
    img_io *fh = img_io_open_path(img,&io);
    if(!fh) die("镜像打不开", IMG_ENOENT);
    img_qcow2_opts_t qo={0};
    img_qcow2 *q = img_qcow2_open(fh,&qo);
    if(!q || img_qcow2_write_init(q)!=IMG_OK) die("qcow2", IMG_EFORMAT);
    img_parttable_t t;
    if(img_parttable_read(q,&t,0)!=IMG_OK) die("分区表", IMG_ESTATE);
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==lba){ pb=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512u; break; }
    if(!pb) die("该 LBA 不在分区表里", IMG_EINVAL);
    img_ntfs_fs *fs = img_ntfs_mount_rw(q,lba,pb);
    if(!fs) die("系统分区不是合法 NTFS", IMG_ESTATE);

    if(winre_auto){
        if(wr_from){
            /* 源 RE 分区 → 系统分区 Recovery 文件夹（内存直搬，无宿主临时文件）*/
            char wf[512]; snprintf(wf,sizeof wf,"%s",wr_from);
            char *colon=strrchr(wf,':');
            if(!colon){ img_ntfs_umount(fs); die("--winre-from 要写成 镜像:lba", IMG_EINVAL); }
            *colon=0; uint64_t slba=strtoull(colon+1,NULL,0);
            img_io_opts_t io2={0};
            img_io *f2=img_io_open_path(wf,&io2);
            img_qcow2_opts_t qo2={0};
            img_qcow2 *q2=f2?img_qcow2_open(f2,&qo2):NULL;
            img_parttable_t t2; uint64_t pb2=0;
            if(!q2||img_parttable_read(q2,&t2,0)!=IMG_OK){ img_ntfs_umount(fs); die("源盘/表", IMG_ESTATE); }
            for(int i=0;i<IMG_MAX_PARTITIONS;i++)
                if(t2.parts[i].used&&t2.parts[i].first_lba==slba){ pb2=(t2.parts[i].last_lba-t2.parts[i].first_lba+1)*512u; break; }
            img_ntfs_fs *fs2=pb2?img_ntfs_mount_rw(q2,slba,pb2):NULL;
            if(!fs2){ img_ntfs_umount(fs); die("源 RE 分区不是 NTFS", IMG_ESTATE); }
            /* ★ 阶段1：源卷【只读不写】——桥的当前卷是全局的，两卷交替挂会串台
               （写目标时 ctx 还指着源卷 → EIO，2026-10-19 实测抓到）。先全读进内存。*/
            static const char *CAND[][2]={
                {"/Recovery/WindowsRE/Winre.wim","/Windows/System32/Recovery/Winre.wim"},
                {"/Recovery/WindowsRE/winre.wim","/Windows/System32/Recovery/Winre.wim"},
                {"/Recovery/WindowsRE/boot.sdi","/Windows/System32/Recovery/boot.sdi"},
                {"/boot.sdi","/Windows/System32/Recovery/boot.sdi"}};
            void *bufs[4]={0}; size_t lens[4]={0};
            for(int k=0;k<4;k++){
                void *b=NULL; size_t n=0;
                if(img_ntfs_read_file(fs2,CAND[k][0],&b,&n)==IMG_OK&&n){
                    int slot=(k<2)?0:2;   /* wim 槽 / sdi 槽（同槽后者覆盖前者）*/
                    if(k>=2) slot=2;
                    if(k==1) slot=1;
                    if(bufs[slot]) free(bufs[slot]);
                    bufs[slot]=b; lens[slot]=n;
                    fprintf(stderr,"  读入 %s (%zu 字节)\n", CAND[k][0], n);
                }
            }
            /* 简化槽位：0=wim(取第一个命中的) 2=sdi(取第一个命中的) */
            img_ntfs_umount(fs2);
            /* ★ 全局 ctx 被源卷顶掉后不会自动归还 → 目标卷【重挂】换新句柄+激活 ctx
               （2026-10-19 实测：仅卸源卷不够，写目标仍 EIO）*/
            fs = img_ntfs_mount_rw(q, lba, pb);
            if(!fs) die("目标卷重挂失败", IMG_ESTATE);
            img_ntfs_mkdir_p(fs,"/Windows/System32/Recovery");
            int got=0;
            static const int USE[2]={0,2};
            for(int u=0;u<2;u++){
                int sl=USE[u];
                if(!bufs[sl]) continue;
                char tmpn[]="/tmp/winreXXXXXX"; int tfd=mkstemp(tmpn);
                int rc=IMG_EIO;
                if(tfd>=0){
                    if(write(tfd,bufs[sl],lens[sl])==(ssize_t)lens[sl]){ close(tfd);
                        rc=img_ntfs_put_file(fs,tmpn,CAND[sl==0?0:2][1],NULL);
                    } else close(tfd);
                    unlink(tmpn);
                }
                fprintf(stderr,"  %s → %s (%zu 字节)%s\n",
                        rc==IMG_OK?"✅":"❌", CAND[sl==0?0:2][1], lens[sl],
                        rc==IMG_OK?"":img_strerror(rc));
                if(rc==IMG_OK) got++;
                free(bufs[sl]); bufs[sl]=NULL;
            }
            if(!got){ img_ntfs_umount(fs); die("源分区里找不到 winre.wim", IMG_ENOENT); }
            fprintf(stderr,"  ⚠ 未给 --winre-from：只写启用脚本（假定 Recovery 文件夹已有 Winre.wim）\n");
        }
        static const char SCRIPT[]="@echo off\r\nreagentc /enable\r\ndel \"%~f0\"\r\n";
        img_ntfs_mkdir_p(fs,"/Windows/System32/imgtool");
        int rc=img_ntfs_put_file_mem(fs,SCRIPT,sizeof SCRIPT-1,dst);
        if(rc!=IMG_OK){ img_ntfs_umount(fs); die("启用脚本写入", rc); }
        fprintf(stderr,"  ✅ 启用脚本 → %s（RunOnce: %s）\n", dst, valname);
    }
    /* ① 脚本写进 NTFS（自动建目录） */
    if(src){
        char dir[512]; snprintf(dir,sizeof dir,"%s",dst);
        char *slash=strrchr(dir,'/');
        if(slash && slash!=dir){ *slash=0; img_ntfs_mkdir_p(fs,dir); }
        int rc=img_ntfs_put_file(fs, src, dst, NULL);
        if(rc!=IMG_OK){ img_ntfs_umount(fs); die("脚本写入失败", rc); }
        fprintf(stderr,"  ✅ 脚本已写入 %s\n", dst);
    }

    /* ② 读 SOFTWARE hive → 临时文件 */
    void *hive=NULL; size_t hn=0;
    int rc=img_ntfs_read_file(fs, "/Windows/System32/config/SOFTWARE", &hive, &hn);
    if(rc!=IMG_OK){ img_ntfs_umount(fs); die("SOFTWARE hive 读不出", rc); }
    char tmpl[]="/tmp/imgrunonce_XXXXXX";
    int tfd=mkstemp(tmpl);
    if(tfd<0){ free(hive); img_ntfs_umount(fs); die("临时文件", IMG_EIO); }
    if(write(tfd,hive,hn)!=(ssize_t)hn){ close(tfd); free(hive); img_ntfs_umount(fs); die("临时文件写", IMG_EIO); }
    close(tfd); free(hive);
    fprintf(stderr,"  → SOFTWARE hive %zu 字节（临时文件 %s）\n", hn, tmpl);

    /* ③ hivex 打开 → RunOnce 加值 */
    int rc2=IMG_OK;
    hive_h *h = hivex_open(tmpl, HIVEX_OPEN_WRITE);
    if(!h){ rc2=IMG_EIO; goto fail; }
    hive_node_h n = hivex_root(h);
    const char *want[]={"Software","Microsoft","Windows","CurrentVersion","RunOnce",NULL};
    for(int i=0; want[i]; i++){
        hive_node_h c = hivex_node_get_child(h, n, want[i]);
        if(!c){
            c = hivex_node_add_child(h, n, want[i]);
            if(!c){ rc2=IMG_EIO; break; }
            fprintf(stderr,"  → 建节点 %s\n", want[i]);
        }
        n=c;
    }
    if(rc2==IMG_OK){
        /* NTFS 路径 → Windows 路径（假定系统盘盘符 C:，克隆盘首次进系统通常就是 C:）*/
        char cmd[600];
        snprintf(cmd,sizeof cmd,"C:%s", dst);
        for(char *p=cmd; *p; p++) if(*p=='/') *p='\\';
        hive_set_value v;
        v.key   = (char*)valname;
        v.t     = hive_t_REG_SZ;      /* REG_SZ */
        v.value = cmd;
        v.len   = strlen(cmd)+1;      /* 含结尾 NUL（Windows REG_SZ 惯例）*/
        if(hivex_node_set_value(h, n, &v, 0)==0){
            fprintf(stderr,"  ✅ RunOnce[%s] = %s\n", valname, cmd);
        } else rc2=IMG_EIO;
    }
    if(rc2==IMG_OK && hivex_commit(h, tmpl, 0)!=0) rc2=IMG_EIO;
    hivex_close(h);

    if(rc2!=IMG_OK){ fail: unlink(tmpl); img_ntfs_umount(fs); die("hivex 改 RunOnce", rc2); }

    /* ④ 写回 NTFS（同一路径覆盖；大小可变，NTFS 侧按新长度写） */
    {
        FILE *f=fopen(tmpl,"rb");
        if(!f){ unlink(tmpl); img_ntfs_umount(fs); die("临时文件回读", IMG_EIO); }
        fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
        void *nb=malloc((size_t)sz);
        if(!nb || fread(nb,1,(size_t)sz,f)!=(size_t)sz){ if(nb)free(nb); fclose(f); unlink(tmpl); img_ntfs_umount(fs); die("回读", IMG_EIO); }
        fclose(f);
        rc=img_ntfs_put_file_mem(fs, nb, (size_t)sz, "/Windows/System32/config/SOFTWARE");
        free(nb); unlink(tmpl);
        if(rc!=IMG_OK){ img_ntfs_umount(fs); die("hive 写回失败", rc); }
        fprintf(stderr,"  ✅ SOFTWARE hive 已写回（%ld 字节）\n", sz);
    }
    img_ntfs_umount(fs);
    img_qcow2_close(q); img_io_close(fh);
    printf("{\"ok\":true,\"cmd\":\"reg.runonce\",\"val\":\"%s\"}\n", valname);
    return 0;
}
