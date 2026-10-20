/* imglayout — 布局/约束/转换 工具
 *   imglayout limits                          列出所有限制（前端用来画滑块）
 *   imglayout mk <out> <bytes> gpt|mbr [--esp N MiB] [--msr N] [--sys N] [--rec N]
 *   imglayout check <image>                   校验分区表
 *   imglayout convert <image> gpt|mbr         表类型互转
 */
#include "img_constraints.h"
#include "img_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char*role_name(img_part_role_t r){
    switch(r){
    case IMG_ROLE_ESP: return "ESP (EFI系统分区)";
    case IMG_ROLE_SYSPART: return "系统分区 (BIOS活动)";
    case IMG_ROLE_MSR: return "MSR (保留分区)";
    case IMG_ROLE_WINDOWS: return "Windows 分区";
    case IMG_ROLE_RECOVERY: return "恢复分区";
    default: return "数据分区";
    }
}
static void human(char *b,size_t c,uint64_t v){
    if(v==0){snprintf(b,c,"—");return;}
    if(v%(1ULL<<30)==0) snprintf(b,c,"%.0f GiB",(double)v/(1ULL<<30));
    else if(v%(1ULL<<20)==0) snprintf(b,c,"%.0f MiB",(double)v/(1ULL<<20));
    else snprintf(b,c,"%.0f KiB",(double)v/1024);
}

int main(int argc,char**argv){
    /* --log 支持：镜像旁边会多出 <镜像>.imgtool.log（里程碑 + 错误）；
       也支持 --log=路径 / --log-level=... / --log-max=N（见 DESIGN-OUTPUT.md）*/
    img_log_cli(argc, argv, "imglayout");
    if(argc<2){ fprintf(stderr,
        "用法:\n  %s limits\n  %s mk <out.qcow2> <bytes> gpt|mbr [--esp MiB] [--msr MiB] [--sys MiB] [--rec MiB]\n"
        "  %s check <image.qcow2>\n  %s convert <image.qcow2> gpt|mbr\n",
        argv[0],argv[0],argv[0],argv[0]); return 2; }

    if(!strcmp(argv[1],"limits")){
        printf("%-24s %10s %10s %10s %10s  %s\n","角色","最小","推荐","默认","上限","约束");
        for(int r=IMG_ROLE_ESP;r<IMG_ROLE_COUNT;r++){
            const img_role_limits_t *L=img_role_limits((img_part_role_t)r);
            char mn[32],re[32],de[32],mx[32];
            human(mn,sizeof mn,L->min_bytes); human(re,sizeof re,L->rec_bytes);
            /* ★ 0 写成"0(不建)"：恢复分区的默认值就是 0（政策：默认不建）*/
            if(L->def_bytes) human(de,sizeof de,L->def_bytes);
            else snprintf(de,sizeof de,"0(不建)");
            human(mx,sizeof mx,L->max_bytes);
            char tags[64]={0}; int k=0;
            if(L->must_fat32) k+=snprintf(tags+k,sizeof tags-k,"%s必须FAT32",k?",":"");
            if(L->must_ntfs)  k+=snprintf(tags+k,sizeof tags-k,"%s必须NTFS",k?",":"");
            if(L->must_active)k+=snprintf(tags+k,sizeof tags-k,"%s必须活动",k?",":"");
            printf("%-24s %10s %10s %10s %10s  %s\n",role_name((img_part_role_t)r),mn,re,de,mx,tags);
        }
        printf("\n其它硬限制:\n");
        printf("  MBR: 最多 4 个主分区；单分区 ≤2 TiB；只能 1 个活动分区\n");
        printf("  GPT: 最多 128 个分区；单分区 ≤16 EiB；需要 ESP(FAT32)+MSR(16MiB)\n");
        printf("  FAT32: Windows 下最大 32 GiB   |   NTFS: ≤256 TiB   |   exFAT: ≤16 EiB\n");
        printf("  ESP: 官方 512B 扇区 ≥100~200 MiB、4K 扇区 ≥300 MiB；本工具默认 200 MiB\n");
        printf("  BIOS系统分区: ≥100 MiB（官方），推荐 500 MiB（升级/BitLocker 需要）\n");
        return 0;
    }

    if(!strcmp(argv[1],"mk")){
        if(argc<5){ fprintf(stderr,"参数不足\n"); return 2; }
        uint64_t sz=strtoull(argv[3],NULL,0);
        bool gpt = !strcmp(argv[4],"gpt");
        img_layout_opts_t o; img_layout_defaults(&o, gpt?IMG_TABLE_GPT:IMG_TABLE_MBR);
        for(int i=5;i+2<argc+1;i++){
            if(i+1>=argc) break;
            uint64_t mb=strtoull(argv[i+1],NULL,0);
            if(!strcmp(argv[i],"--esp")) o.esp_bytes=mb*1024*1024;
            else if(!strcmp(argv[i],"--msr")) o.msr_bytes=mb*1024*1024;
            else if(!strcmp(argv[i],"--sys")) o.sys_bytes=mb*1024*1024;
            else if(!strcmp(argv[i],"--rec")) o.recovery_bytes=mb*1024*1024;
        }
        /* 先建 qcow2 */
        img_io_opts_t io={0}; io.writable=true;
        img_io *f=img_io_open_path(argv[2],&io);
        if(!f){ fprintf(stderr,"❌ 打不开输出\n"); return 1; }
        img_qcow2_create_opts_t co={0}; co.size=sz; co.cluster_bits=16;
        int rc=img_qcow2_create(f,&co);
        img_io_close(f);
        if(rc){ fprintf(stderr,"❌ create: %s\n",img_strerror(rc)); return 1; }

        img_qcow2_opts_t qo={0}; img_io_opts_t io2={0}; io2.writable=true;
        img_io *f2=img_io_open_path(argv[2],&io2);
        img_qcow2 *q=img_qcow2_open(f2,&qo);
        if(!q){ fprintf(stderr,"❌ 重开失败\n"); return 1; }
        if(img_qcow2_write_init(q)!=IMG_OK){ fprintf(stderr,"❌ init\n"); return 1; }

        img_parttable_t t; img_check_result_t err;
        rc=img_layout_build(&t, sz/512, gpt?IMG_TABLE_GPT:IMG_TABLE_MBR, &o, &err);
        if(rc!=IMG_OK){ fprintf(stderr,"❌ 布局校验失败：%s\n  建议：%s\n",err.reason,err.suggestion); return 1; }
        rc=img_parttable_write(q,&t);
        if(rc){ fprintf(stderr,"❌ write: %s\n",img_strerror(rc)); return 1; }
        printf("✅ 已创建 %s 盘（%.1f GiB）\n", gpt?"GPT/UEFI":"MBR/BIOS",(double)sz/(1ULL<<30));
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            const img_partition_t *p=&t.parts[i];
            if(!p->used) continue;
            double mb=(double)(p->last_lba-p->first_lba+1)*512/(1024*1024);
            printf("   LBA %8llu  %8.1f MiB  %s %s\n",
                (unsigned long long)p->first_lba, mb,
                p->status==0x80?"★活动":"      ", p->name);
        }
        img_qcow2_close(q); img_io_close(f2);
        return 0;
    }

    if(!strcmp(argv[1],"check")){
        img_io_opts_t io={0}; io.writable=true;
        img_io *f=img_io_open_path(argv[2],&io); if(!f){fprintf(stderr,"❌\n");return 1;}
        img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
        if(!q){fprintf(stderr,"❌\n");return 1;}
        if(img_qcow2_write_init(q)!=IMG_OK) return 1;
        img_parttable_t t;
        int rc=img_parttable_read(q,&t,0);
        if(rc){ fprintf(stderr,"❌ 读分区表: %s\n",img_strerror(rc)); return 1; }
        img_check_result_t err;
        rc=img_constraints_check_table(&t,&err);
        if(rc==IMG_OK){ printf("✅ 分区表合法\n"); }
        else { printf("❌ %s\n   建议：%s\n",err.reason,err.suggestion); }
        img_qcow2_close(q); img_io_close(f);
        return rc==IMG_OK?0:1;
    }

    if(!strcmp(argv[1],"convert")){
        if(argc<4){fprintf(stderr,"需要指定目标格式\n");return 2;}
        img_io_opts_t io={0}; io.writable=true;
        img_io *f=img_io_open_path(argv[2],&io); if(!f){fprintf(stderr,"❌\n");return 1;}
        img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
        if(!q){fprintf(stderr,"❌\n");return 1;}
        if(img_qcow2_write_init(q)!=IMG_OK) return 1;
        img_parttable_t t;
        int rc=img_parttable_read(q,&t,0);
        if(rc){fprintf(stderr,"❌ %s\n",img_strerror(rc));return 1;}
        img_check_result_t err;
        if(!strcmp(argv[3],"gpt")) rc=img_convert_mbr_to_gpt(&t,&err);
        else rc=img_convert_gpt_to_mbr(&t, 0x4ccd932b, &err);
        if(rc!=IMG_OK){ fprintf(stderr,"❌ 转换失败：%s\n   建议：%s\n",err.reason,err.suggestion); return 1; }
        rc=img_parttable_write(q,&t);
        if(rc){fprintf(stderr,"❌ 写回: %s\n",img_strerror(rc));return 1;}
        printf("✅ 已转换为 %s 并写回\n", t.table==IMG_TABLE_GPT?"GPT":"MBR");
        for(int i=0;i<IMG_MAX_PARTITIONS;i++){
            const img_partition_t *p=&t.parts[i];
            if(!p->used) continue;
            double mb=(double)(p->last_lba-p->first_lba+1)*512/(1024*1024);
            printf("   LBA %8llu  %8.1f MiB  %-12s %s\n",(unsigned long long)p->first_lba,mb,
                (t.table==IMG_TABLE_GPT ? (p->type_guid[0]?p->type_guid:"(—)") : img_mbr_type_name(p->type)),
                p->status==0x80?"★活动":p->name);
        }
        img_qcow2_close(q); img_io_close(f);
        return 0;
    }
    fprintf(stderr,"未知命令\n"); return 2;
}
