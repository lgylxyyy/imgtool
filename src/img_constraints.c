/* img_constraints.c — 分区约束：唯一真相源 */
#include "img_constraints.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define KiB (1024ULL)
#define MiB (1024ULL*KiB)
#define GiB (1024ULL*MiB)
#define TiB (1024ULL*GiB)
#define EiB (1024ULL*TiB)

/* FAT32 在【Windows 格式化工具】下的实际上限是 32GB（规范可到 2TB，但 Windows 不认 >32GB）。
 * 我们是给 Windows 用的镜像 → 按 32GB 限制。 */
#define FAT32_WIN_MAX (32ULL*GiB)
#define NTFS_MAX      (256ULL*TiB)
#define EXFAT_MAX     (16ULL*EiB)
#define MBR_PART_MAX  (2ULL*TiB)      /* MBR：LBA 是 32 位 → 最大 2TB */
#define MBR_DISK_MAX  (2ULL*TiB)
#define GPT_PART_MAX  (16ULL*EiB)

/* ------------------------------------------------------------------ */
/* 各角色约束（★ 来源标注，便于以后核对）                             */
/* ------------------------------------------------------------------ */
static const img_role_limits_t LIMITS[IMG_ROLE_COUNT] = {
    /* ★ 这张表是【参考数据】，工具层【不强制】。
     *   政策性判断（过小/过大弹警告）由 App 层决定 —— 因为本库会被其它调用方使用，
     *   硬编政策不合适。工具层只拦【技术正确性】问题（见 img_constraints_check_partition）。 */
    [IMG_ROLE_ESP] = {
        /* ★ 默认 200 MiB（用户 2026-10-20 定：128 MiB ~ 512 MiB 都行，默认取 200）。
         *   依据：ESP 只放启动文件（实测 Win11 一套约 34 MB），512B 簇的 FAT32
         *   写满要逐簇写 —— ESP 越大越浪费（见手册 §13.10 的 6.3 MB/s）。
         *   官方口径：512B 扇区 ≥100~200 MiB；4K 扇区 ≥300 MiB。上限留 1 GiB 不拦。*/
        .min_bytes = 128*MiB, .rec_bytes = 512*MiB, .def_bytes = 200*MiB,
        .max_bytes = 1*GiB, .must_fat32 = true,
        .note = "UEFI 只认 FAT 系；默认 200 MiB（128~512 都合规），4K 扇区请给 300 MiB",
    },
    [IMG_ROLE_SYSPART] = {
        .min_bytes = 128*MiB, .rec_bytes = 512*MiB, .def_bytes = 512*MiB,
        .max_bytes = 1*GiB, .must_active = true,
        .note = "BIOS/MBR 活动分区；太小会让系统升级/BitLocker 因空间不足失败",
    },
    [IMG_ROLE_MSR] = {
        .min_bytes = 16*MiB, .rec_bytes = 16*MiB, .def_bytes = 16*MiB,
        .max_bytes = 16*MiB,
        .note = "GPT 磁盘专用，官方固定 16 MiB，不存用户数据",
    },
    [IMG_ROLE_WINDOWS] = {
        .min_bytes = 16*GiB, .rec_bytes = 128*GiB, .def_bytes = 128*GiB,
        .max_bytes = 262144ULL*GiB, .must_ntfs = true,
        .note = "必须 NTFS；262144 GiB = 256 TiB (NTFS 上限)",
    },
    [IMG_ROLE_RECOVERY] = {
        /* ★ 默认【不建】（用户 2026-10-19 定的政策：默认不创建恢复分区、不写 WinRE，
         *   勾选才做 —— DESIGN-OUTPUT.md §十一 / app/docs 手册 §15）。
         *   要建就必须显式给：imglayout mk ... --rec 1024，或 part.layout --rec-gb=1。
         *   尺寸依据：实测 Winre.wim = 415 MB → 分区至少 ~500 MB 才放得下
         *   （微软文档说的 250MB 是旧基准，LTSC/Win11 这份更大）。*/
        .min_bytes = 0,      /* 0 = 可以不建恢复分区 */
        .rec_bytes = 1*GiB, .def_bytes = 0, .max_bytes = 2*GiB,
        .must_ntfs = true,
        .note = "默认不建；勾选后建议 ≥500MB（实测 Winre.wim 415MB）",
    },
    [IMG_ROLE_DATA] = {
        .min_bytes = 0, .rec_bytes = 0, .def_bytes = 0, .max_bytes = 0,
        .note = "普通数据分区，无尺寸限制",
    },
};

const img_role_limits_t *img_role_limits(img_part_role_t role){
    if(role<=IMG_ROLE_NONE || role>=IMG_ROLE_COUNT) return &LIMITS[IMG_ROLE_DATA];
    return &LIMITS[role];
}

/* ------------------------------------------------------------------ */
/* 内部：结果构造                                                      */
/* ------------------------------------------------------------------ */
static void res_ok(img_check_result_t *r){
    if(!r) return;
    r->ok=true; r->code=IMG_OK; r->reason[0]=0; r->suggestion[0]=0;
}
static void res_fail(img_check_result_t *r, int code, const char *fmt, ...){
    if(!r) return;
    r->ok=false; r->code=code;
    va_list ap; va_start(ap,fmt);
    vsnprintf(r->reason,sizeof r->reason,fmt,ap);
    va_end(ap);
    r->suggestion[0]=0;
}
static void sug(img_check_result_t *r, const char *fmt, ...){
    if(!r) return;
    va_list ap; va_start(ap,fmt);
    vsnprintf(r->suggestion,sizeof r->suggestion,fmt,ap);
    va_end(ap);
}
static void human(char *buf,size_t cap,uint64_t b){
    if(b==0){ snprintf(buf,cap,"0"); return; }
    if(b%(1ULL<<30)==0) snprintf(buf,cap,"%.0f GiB",(double)b/(1ULL<<30));
    else if(b%(1ULL<<20)==0) snprintf(buf,cap,"%.0f MiB",(double)b/(1ULL<<20));
    else snprintf(buf,cap,"%.0f KiB",(double)b/1024);
}

/* ------------------------------------------------------------------ */
/* 单个分区校验                                                        */
/* ------------------------------------------------------------------ */
int img_constraints_check_partition(const img_part_request_t *q, img_check_result_t *out){
    if(!q) return IMG_EINVAL;
    res_ok(out);
    uint64_t bytes = q->sectors * (uint64_t)(q->sector_size? q->sector_size : 512);
    const img_role_limits_t *L = img_role_limits(q->role);
    char hs[32], hr[32], hd[32];
    human(hs,sizeof hs, L->min_bytes);
    human(hr,sizeof hr, L->rec_bytes);
    human(hd,sizeof hd, bytes);

    /* ---- ① 表类型相关硬限制 ---- */
    if(q->table==IMG_TABLE_MBR){
        if(q->first_lba + q->sectors - 1 > 0xFFFFFFFFULL){
            res_fail(out,IMG_EINVAL,
                "MBR 分区的 LBA 是 32 位，结束位置 %llu 超出 2TB 上限",
                (unsigned long long)(q->first_lba+q->sectors-1));
            sug(out,"改用 GPT（UEFI）磁盘，单分区可达 16EB");
            return IMG_EINVAL;
        }
        if(bytes > MBR_PART_MAX){
            res_fail(out,IMG_EINVAL,"MBR 单个分区不能超过 2TB（当前 %s）",hd);
            sug(out,"改用 GPT 磁盘");
            return IMG_EINVAL;
        }
        if(q->existing_count >= 4){
            res_fail(out,IMG_EFULL,
                "MBR 最多 4 个主分区，已用 %d 个", q->existing_count);
            sug(out,"① 改用 GPT；② 建一个扩展分区，再在其中加逻辑分区");
            return IMG_EFULL;
        }
    } else if(q->table==IMG_TABLE_GPT){
        if(bytes > GPT_PART_MAX){
            res_fail(out,IMG_EINVAL,"GPT 单个分区不能超过 16EB（当前 %s）",hd);
            return IMG_EINVAL;
        }
    } else {
        res_fail(out,IMG_ESTATE,"尚未选择分区表类型（MBR 或 GPT）");
        return IMG_ESTATE;
    }

    /* ★ 政策性检查（最小/最大尺寸、必须什么格式、必须活动）已【移除】，
     *   交由 App 层决定（超出范围 → 弹警告而非拒绝，用户可自定义）。
     *   这里只保留【不拦就会产出坏镜像】的技术检查。 */

    if(q->role == IMG_ROLE_MSR && q->table != IMG_TABLE_GPT){
        res_fail(out, IMG_EINVAL,
            "MSR 是 GPT 磁盘专用的，MBR 磁盘不能有 MSR 分区");
        sug(out, "MBR 磁盘不需要保留分区");
        return IMG_EINVAL;
    }
    return IMG_OK;
}

int img_constraints_check_table(const img_parttable_t *t, img_check_result_t *out){
    if(!t) return IMG_EINVAL;
    res_ok(out);
    int used=0;                        /* ★ 只数【主表项】：逻辑分区不占 MBR 的 4 个槽 */
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(t->parts[i].used && !t->parts[i].logical) used++;

    if(t->table==IMG_TABLE_MBR){
        if(used>4){
            res_fail(out,IMG_EFULL,"MBR 最多 4 个主分区，当前 %d 个",used);
            sug(out,"改用 GPT，或用扩展分区+逻辑分区");
            return IMG_EFULL;
        }
        int active=0;
        for(int i=0;i<IMG_MAX_PARTITIONS;i++)
            if(t->parts[i].used && t->parts[i].status==0x80) active++;
        if(active>1){
            res_fail(out,IMG_EINVAL,"MBR 只能有 1 个活动分区，当前有 %d 个",active);
            sug(out,"多余的会报 \"Invalid partition table\"");
            return IMG_EINVAL;
        }
    }
    /* 重叠检查 */
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *a=&t->parts[i];
        if(!a->used) continue;
        if(a->last_lba < a->first_lba){
            res_fail(out,IMG_EINVAL,"分区#%d 结束 LBA 小于起始 LBA",i);
            return IMG_EINVAL;
        }
        if(a->last_lba >= t->disk_sectors){
            res_fail(out,IMG_EINVAL,
                "分区#%d 结束于 LBA %llu，但磁盘只有 %llu 扇区（超出磁盘！）",
                i,(unsigned long long)a->last_lba,(unsigned long long)t->disk_sectors);
            sug(out,"这类错误会导致 BIOS 报 \"A disk read error occurred\"");
            return IMG_EINVAL;
        }
        for(int j=i+1;j<IMG_MAX_PARTITIONS;j++){
            const img_partition_t *b=&t->parts[j];
            if(!b->used) continue;
            /* ★ 扩展容器【本来就要】包住它里面的逻辑分区 —— 这不是重叠 */
            bool a_ext=(a->type==IMG_MBR_EXTENDED||a->type==IMG_MBR_EXTENDED_L);
            bool b_ext=(b->type==IMG_MBR_EXTENDED||b->type==IMG_MBR_EXTENDED_L);
            if((a->logical&&b_ext)||(b->logical&&a_ext)) continue;
            if(a->first_lba<=b->last_lba && b->first_lba<=a->last_lba){
                res_fail(out,IMG_EINVAL,"分区#%d 与 #%d 重叠",i,j);
                sug(out,"重叠的分区会导致数据损坏");
                return IMG_EINVAL;
            }
        }
    }
    return IMG_OK;
}

/* ------------------------------------------------------------------ */
/* 布局生成                                                            */
/* ------------------------------------------------------------------ */
void img_layout_defaults(img_layout_opts_t *o, img_table_t table){
    if(!o) return;
    memset(o,0,sizeof *o);
    o->alignment = 2048;
    o->msr_bytes  = 16*MiB;
    if(table==IMG_TABLE_GPT){
        o->esp_bytes  = LIMITS[IMG_ROLE_ESP].def_bytes;    /* 200 MiB */
        o->recovery_bytes = LIMITS[IMG_ROLE_RECOVERY].def_bytes;
    } else {
        o->sys_bytes  = LIMITS[IMG_ROLE_SYSPART].def_bytes;/* 512 MiB */
    }
}

int img_layout_build(img_parttable_t *t, uint64_t disk_sectors,
                     img_table_t table, const img_layout_opts_t *o,
                     img_check_result_t *err){
    if(!t||disk_sectors<4096) return IMG_EINVAL;
    img_layout_opts_t LO;
    if(!o){ img_layout_defaults(&LO,table); o=&LO; }
    uint64_t align = o->alignment ? o->alignment : 2048;
    uint64_t sec   = 512;

    memset(t,0,sizeof *t);
    t->table = table;
    t->disk_sectors = disk_sectors;
    t->disk_signature = 0x4ccd932b;
    img_guid_new(t->disk_guid);
    t->part_entry_count=128; t->part_entry_size=128;

    int rc;
    uint64_t off = align;
    int idx;

    if(table==IMG_TABLE_GPT){
        /* UEFI/GPT 官方顺序：ESP → MSR → Windows → Recovery */
        img_part_request_t rq={0};
        rq.table=table; rq.disk_sectors=disk_sectors; rq.sector_size=sec;
        rq.role=IMG_ROLE_ESP; rq.gpt_type_guid=IMG_GUID_ESP;
        rq.sectors = o->esp_bytes/sec; rq.first_lba=off; rq.existing_count=0;
        if((rc=img_constraints_check_partition(&rq,err))!=IMG_OK) return rc;
        if((rc=img_partition_add(t,off,rq.sectors,IMG_MBR_ESP,IMG_GUID_ESP,"EFI system partition",false,&idx))!=IMG_OK) return rc;
        off += rq.sectors;

        rq = (img_part_request_t){0};
        rq.table=table; rq.disk_sectors=disk_sectors; rq.sector_size=sec;
        rq.role=IMG_ROLE_MSR; rq.gpt_type_guid=IMG_GUID_MSR;
        rq.sectors = o->msr_bytes/sec; rq.first_lba=off; rq.existing_count=1;
        if((rc=img_constraints_check_partition(&rq,err))!=IMG_OK) return rc;
        if((rc=img_partition_add(t,off,rq.sectors,IMG_MBR_EMPTY,IMG_GUID_MSR,"Microsoft reserved partition",false,&idx))!=IMG_OK) return rc;
        off += rq.sectors;
    } else {
        /* BIOS/MBR：活动系统分区 */
        img_part_request_t rq={0};
        rq.table=table; rq.disk_sectors=disk_sectors; rq.sector_size=sec;
        /* ★ 系统保留分区必须用【0x27】—— 微软官方明文（Learn《BIOS/MBR-based hard drive
         *   partitions》"System and utility partitions"）：
         *     "To keep system and utility partitions from being reset, use type 0x27.
         *      Do not use any of the following types: 0x7, 0x0c, 0x0b, 0x0e, 0x06, and 0x42."
         *   本工具 2026-10-16~18 的克隆路径用过 0x07 且能到徽标（因为克隆的系统已完成
         *   specialize），但裸灌路径实测：0x07 时 Win7 报"驱动器号不匹配"（Windows 把
         *   这个普通 NTFS 卷认成 C:），0x27 后该错误消失。官方文档 + 实测同向。
         *   实测（QEMU + Win7 32 位）：0x07 时 Windows 首次启动会把这个 512MiB 的
         *   普通 NTFS 卷当成 C:，真正的系统分区变成 D: → specialize 阶段报
         *   "没有确保计算机之间分配的驱动器号相匹配"，卡死。
         *   改 0x27 后同样条件推进到"为首次使用计算机做准备"，该错误消失。*/
        rq.role=IMG_ROLE_SYSPART; rq.mbr_type=IMG_MBR_RECOVERY; rq.active=true;
        rq.sectors = o->sys_bytes/sec; rq.first_lba=off; rq.existing_count=0;
        if((rc=img_constraints_check_partition(&rq,err))!=IMG_OK) return rc;
        if((rc=img_partition_add(t,off,rq.sectors,IMG_MBR_RECOVERY,NULL,"System",true,&idx))!=IMG_OK) return rc;
        off += rq.sectors;
    }

    /* 恢复分区（可选，放在 Windows 分区之后）*/
    uint64_t recovery_sec = (o->recovery_bytes && o->recovery_bytes>=52*MiB) ? o->recovery_bytes/sec : 0;

    /* Windows 分区：占满剩余（扣掉恢复分区+对齐余量） */
    uint64_t win_start = (off + align-1)/align*align;
    uint64_t reserve_tail = recovery_sec ? ((recovery_sec + align-1)/align*align + align) : align;
    if(disk_sectors <= win_start + reserve_tail + 2048) return IMG_EFULL;
    uint64_t win_sec = disk_sectors - win_start - reserve_tail;

    /* ★ 修复：原先 GPT 路径【完全忽略 o->sys_bytes】，Windows 分区永远吃满剩余，
     *   导致 imglayout --sys 无效、也没法留出独立数据盘。
     *   现在：给了 sys_bytes 就按它建系统盘，剩下的留给数据盘。 */
    uint64_t want_win = 0;
    if(table==IMG_TABLE_GPT && o->sys_bytes > 0){
        want_win = (o->sys_bytes + sec-1)/sec;
        if(want_win < win_sec && want_win > 0) win_sec = want_win;
    }

    img_part_request_t rq={0};
    rq.table=table; rq.disk_sectors=disk_sectors; rq.sector_size=sec;
    rq.role=IMG_ROLE_WINDOWS; rq.sectors=win_sec; rq.first_lba=win_start;
    rq.existing_count=2; rq.gpt_type_guid=IMG_GUID_BASIC; rq.mbr_type=IMG_MBR_NTFS_EXFAT;
    if((rc=img_constraints_check_partition(&rq,err))!=IMG_OK) return rc;
    if(table==IMG_TABLE_GPT)
        rc=img_partition_add(t,win_start,win_sec,0x07,IMG_GUID_BASIC,"Basic data partition",false,&idx);
    else
        rc=img_partition_add(t,win_start,win_sec,IMG_MBR_NTFS_EXFAT,NULL,"Windows",false,&idx);
    if(rc) return rc;
    off = win_start + win_sec;

    /* ★ 数据盘：紧接系统盘，吃满到恢复分区之前（只在给了 --sys 时启用）*/
    if(table==IMG_TABLE_GPT && want_win > 0){
        uint64_t d_start = (off + align-1)/align*align;
        uint64_t d_end   = disk_sectors - reserve_tail && recovery_sec
                           ? (disk_sectors - ((recovery_sec + align-1)/align*align + align))
                           : disk_sectors - align;
        if(d_end > d_start + align){
            uint64_t d_sec = d_end - d_start;
            img_part_request_t dq = (img_part_request_t){0};
            dq.table=table; dq.disk_sectors=disk_sectors; dq.sector_size=sec;
            dq.role=IMG_ROLE_WINDOWS; dq.sectors=d_sec; dq.first_lba=d_start;
            dq.existing_count=3; dq.gpt_type_guid=IMG_GUID_BASIC; dq.mbr_type=IMG_MBR_NTFS_EXFAT;
            if((rc=img_constraints_check_partition(&dq,err))!=IMG_OK) return rc;
            if((rc=img_partition_add(t,d_start,d_sec,0x07,IMG_GUID_BASIC,
                                     "Basic data partition",false,&idx))!=IMG_OK) return rc;
            off = d_start + d_sec;
        }
    }

    if(recovery_sec){
        uint64_t r_start=(off+align-1)/align*align;
        rq = (img_part_request_t){0};
        rq.table=table; rq.disk_sectors=disk_sectors; rq.sector_size=sec;
        rq.role=IMG_ROLE_RECOVERY; rq.sectors=recovery_sec; rq.first_lba=r_start;
        rq.gpt_type_guid=IMG_GUID_RECOVERY; rq.mbr_type=IMG_MBR_RECOVERY;
        if((rc=img_constraints_check_partition(&rq,err))!=IMG_OK) return rc;
        if(table==IMG_TABLE_GPT)
            rc=img_partition_add(t,r_start,recovery_sec,0x07,IMG_GUID_RECOVERY,"Windows RE tools",false,&idx);
        else
            rc=img_partition_add(t,r_start,recovery_sec,IMG_MBR_RECOVERY,NULL,"Recovery",false,&idx);
        if(rc) return rc;
    }
    return img_constraints_check_table(t,err);
}

/* ------------------------------------------------------------------ */
/* 表类型转换                                                          */
/* ------------------------------------------------------------------ */
int img_convert_mbr_to_gpt(img_parttable_t *t, img_check_result_t *err){
    if(!t) return IMG_EINVAL;
    if(t->table==IMG_TABLE_GPT){ res_ok(err); return IMG_OK; }
    int used=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) if(t->parts[i].used) used++;
    if(used>4){
        res_fail(err,IMG_EINVAL,"MBR 有 %d 个主分区，无法转成 GPT（最多4个）",used);
        return IMG_EINVAL;
    }
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        p->status = 0;                       /* GPT 没有活动标记 */
        p->attributes = 0;
        /* 按 MBR 类型映射到 GPT 类型 GUID（★ 恢复分区用专用 GUID） */
        if(!p->type_guid[0]){
            const char *g;
            switch(p->type){
            case IMG_MBR_ESP:      g=IMG_GUID_ESP;      break;
            case IMG_MBR_RECOVERY: g=IMG_GUID_RECOVERY; break;
                default:               g=IMG_GUID_BASIC;    break;
            }
            snprintf(p->type_guid,sizeof p->type_guid,"%s",g);
        }
        if(!p->part_guid[0]) img_guid_new(p->part_guid);
    }
    t->table = IMG_TABLE_GPT;
    img_guid_new(t->disk_guid);
    t->part_entry_count=128; t->part_entry_size=128;
    res_ok(err);
    return IMG_OK;
}

int img_convert_gpt_to_mbr(img_parttable_t *t, uint32_t disk_signature,
                           img_check_result_t *err){
    if(!t) return IMG_EINVAL;
    if(t->table==IMG_TABLE_MBR){ res_ok(err); return IMG_OK; }

    /* 收集真实分区（跳过 MSR —— MBR 没有对应概念） */
    img_partition_t keep[IMG_MAX_PARTITIONS]; int n=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++){
        const img_partition_t *p=&t->parts[i];
        if(!p->used) continue;
        if(strcmp(p->type_guid, IMG_GUID_MSR)==0) continue;   /* 丢弃 MSR */
        if(p->last_lba > 0xFFFFFFFFULL){
            res_fail(err,IMG_EINVAL,
                "分区#%d 超出 2TB（MBR 的 LBA 是 32 位），无法转换",i);
            sug(err,"先缩小/删除该分区，或保留 GPT");
            return IMG_EINVAL;
        }
        if(n>=4){
            res_fail(err,IMG_EFULL,"MBR 最多 4 个主分区，GPT 里有更多");
            sug(err,"删除多余分区，或改用扩展分区+逻辑分区");
            return IMG_EFULL;
        }
        keep[n++]=*p;
    }
    if(n==0){ res_fail(err,IMG_ESTATE,"GPT 里没有可转换的分区"); return IMG_ESTATE; }

    /* 第一个分区设为活动（BIOS 需要） */
    for(int i=0;i<IMG_MAX_PARTITIONS;i++) memset(&t->parts[i],0,sizeof t->parts[i]);
    for(int i=0;i<n;i++){
        t->parts[i]=keep[i];
        t->parts[i].status=0;
        t->parts[i].attributes=0;
        /* GUID → MBR 类型 */
        if(!strcmp(t->parts[i].type_guid, IMG_GUID_ESP))       t->parts[i].type=IMG_MBR_ESP;
        else if(!strcmp(t->parts[i].type_guid, IMG_GUID_RECOVERY)) t->parts[i].type=IMG_MBR_RECOVERY;
        else                                                    t->parts[i].type=IMG_MBR_NTFS_EXFAT;
    }
    t->parts[0].status = 0x80;   /* 第一个分区设为活动 */
    t->count = n;
    t->table = IMG_TABLE_MBR;
    t->disk_signature = disk_signature;
    memset(t->disk_guid,0,sizeof t->disk_guid);
    res_ok(err);
    return IMG_OK;
}
