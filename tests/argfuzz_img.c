/* argfuzz_img — 第二轮：真实镜像上的极端参数（App 最容易踩的越界/单位错/超长路径）
 * 用法: argfuzz_img <image.qcow2> <esp_lba>
 * 每条 fork 隔离；重点看：会不会崩、会不会写坏盘（MBR/分区表）
 */
#include "img_types.h"
#include "img_event.h"
#include "img_io.h"
#include "img_qcow2.h"
#include "img_partition.h"
#include "img_fat32.h"
#include "img_fat32_fs.h"
#include "img_ntfs_fs.h"
#include "img_bootcheck.h"
#include "img_fixboot.h"
#include "img_wim_apply.h"
#include "img_bcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *IMG; static uint64_t ESP;
static const char *GW="."; static int g_case=0;
static int g_rc=0;   /* 最近一次调用的返回码（子进程打印用）*/
static int ncase=0,ncrash=0; static const char *crash_list[64]; static int crash_n=0;

static char g_cur[512];
static img_qcow2 *openq(void){
    /* ★ 每个用例一张独立复制盘：既避免用例互相污染，也便于事后逐张 qemu-img check
     *   判断"这个坏参数有没有把镜像写坏" */
    snprintf(g_cur,sizeof g_cur,"%s/case%03d.qcow2",GW,++g_case);
    char cmd[1200]; snprintf(cmd,sizeof cmd,"cp -p '%s' '%s' 2>/dev/null",IMG,g_cur);
    int st=system(cmd); if(st!=0) return NULL;
    img_io_opts_t io={0}; io.writable=true;
    img_io *f=img_io_open_path(g_cur,&io); if(!f) return NULL;
    img_qcow2_opts_t qo={0}; img_qcow2 *q=img_qcow2_open(f,&qo);
    if(!q) return NULL;
    img_qcow2_write_init(q);
    return q;
}
static void run(const char *name, void (*f)(void)){
    ncase++; fflush(stdout);
    pid_t p=fork();
    if(p==0){ f(); printf("  %-6s %-58s rc=%-4d %s\n", g_rc?"✅":"🔎", name, g_rc, g_rc?img_strerror(g_rc):"(成功)"); fflush(stdout); _exit(0); }
    int st=0; waitpid(p,&st,0);
    if(WIFSIGNALED(st)){
        ncrash++; crash_list[crash_n++]=name;
        printf("  💥 %-58s 崩了 (signal %d)\n",name,WTERMSIG(st));
    } /* 正常：子进程已打印 */
}
/* 便捷：值 + 期望说明 */
static void c_r_umax(void){ img_qcow2*q=openq(); char b[4096]; g_rc=img_qcow2_read(q,~(uint64_t)0,b,sizeof b); img_qcow2_close(q); }
static void c_rf_umax(void){ img_qcow2*q=openq(); char b[4096]; g_rc=img_qcow2_read_full(q,~(uint64_t)0-15,b,sizeof b); img_qcow2_close(q); }
static void c_r_end(void){ img_qcow2*q=openq(); char b[4096]; g_rc=img_qcow2_read(q,img_qcow2_size(q),b,sizeof b); img_qcow2_close(q); }
static void c_r_endminus1(void){ img_qcow2*q=openq(); char b[4096]; g_rc=img_qcow2_read_full(q,img_qcow2_size(q)-1,b,sizeof b); img_qcow2_close(q); }
static void c_rf_huge_len(void){ img_qcow2*q=openq(); char b[16]; g_rc=img_qcow2_read_full(q,0,b,~(size_t)0); img_qcow2_close(q); }
static void c_w_umax(void){ img_qcow2*q=openq(); char b[4096]={1}; g_rc=img_qcow2_write(q,~(uint64_t)0-4096,b,sizeof b); img_qcow2_close(q); }
static void c_w_end(void){ img_qcow2*q=openq(); char b[4096]={1}; g_rc=img_qcow2_write(q,img_qcow2_size(q),b,sizeof b); img_qcow2_close(q); }
static void c_w_lenhuge(void){ img_qcow2*q=openq(); char b[16]={1}; g_rc=img_qcow2_write(q,0,b,~(size_t)0); img_qcow2_close(q); }
static void c_expand_0(void){ img_qcow2*q=openq(); g_rc=img_qcow2_expand(q,0,NULL); img_qcow2_close(q); }
static void c_expand_1(void){ img_qcow2*q=openq(); g_rc=img_qcow2_expand(q,1,NULL); img_qcow2_close(q); }
static void c_expand_umax(void){ img_qcow2*q=openq(); g_rc=img_qcow2_expand(q,~(uint64_t)0,NULL); img_qcow2_close(q); }
static void c_expand_small(void){ img_qcow2*q=openq(); g_rc=img_qcow2_expand(q,1024*1024,NULL); img_qcow2_close(q); }

static void c_fat_lba0(void){ img_qcow2*q=openq(); { img_fat32_fs*f=img_fat32_mount(q,0,true); g_rc=(int)(f?0:-1); img_fat32_unmount(f); } img_qcow2_close(q); }
static void c_fat_lba1(void){ img_qcow2*q=openq(); { img_fat32_fs*f=img_fat32_mount(q,1,true); g_rc=(int)(f?0:-1); img_fat32_unmount(f); } img_qcow2_close(q); }
static void c_fat_lba_end(void){ img_qcow2*q=openq(); { img_fat32_fs*f=img_fat32_mount(q,img_qcow2_size(q)/512,true); g_rc=(int)(f?0:-1); img_fat32_unmount(f); } img_qcow2_close(q); }
static void c_fat_lba_1t(void){ img_qcow2*q=openq(); { img_fat32_fs*f=img_fat32_mount(q,1ull<<40,true); g_rc=(int)(f?0:-1); img_fat32_unmount(f); } img_qcow2_close(q); }
static void c_fat_fmt_lba0(void){ img_qcow2*q=openq(); img_fat32_layout_t L; g_rc=img_fat32_format(q,NULL,&L,NULL); img_qcow2_close(q); }

static void c_fat_put_longpath(void){
    img_qcow2*q=openq(); img_fat32_fs*fs=img_fat32_mount(q,ESP,true);
    if(fs){ char p[5000]; memset(p,'x',sizeof p); p[0]='/'; p[4095]=0;
            g_rc=img_fat32_put_file(fs,"/etc/hostname",p,NULL); }
    img_qcow2_close(q);
}
static void c_fat_mkdir_deep(void){
    img_qcow2*q=openq(); img_fat32_fs*fs=img_fat32_mount(q,ESP,true);
    if(fs){ char p[3000]; int o=0;
            for(int i=0;i<100&&o<2900;i++) o+=snprintf(p+o,sizeof p-(size_t)o,"/d%d",i);
            g_rc=img_fat32_mkdir_p(fs,p); }
    img_qcow2_close(q);
}
static void c_fat_read_longpath(void){
    img_qcow2*q=openq(); img_fat32_fs*fs=img_fat32_mount(q,ESP,true);
    if(fs){ char p[5000]; memset(p,'y',sizeof p); p[0]='/'; p[4999]=0;
            void*b=NULL; size_t n=0; g_rc=img_fat32_read_file(fs,p,&b,&n); free(b); }
    img_qcow2_close(q);
}
static void c_fat_putdata_umax(void){
    img_qcow2*q=openq(); img_fat32_fs*fs=img_fat32_mount(q,ESP,true);
    if(fs){ char b[16]={0}; g_rc=img_fat32_put_data(fs,b,~(size_t)0,"/EFI/huge.bin"); }
    img_qcow2_close(q);
}
static void c_fat_putdata_zero(void){
    img_qcow2*q=openq(); img_fat32_fs*fs=img_fat32_mount(q,ESP,true);
    if(fs){ g_rc=img_fat32_put_data(fs,"",0,"/EFI/zero.bin"); }
    img_qcow2_close(q);
}
static void c_fat_probe_lba0(void){ img_qcow2*q=openq(); img_fat32_layout_t L; g_rc=img_fat32_probe(q,0,&L); img_qcow2_close(q); }

static void c_ntfs_bytes0(void){ img_qcow2*q=openq(); { img_ntfs_fs*f=img_ntfs_mount_rw(q,ESP+1000,0); g_rc=(int)(f?0:-1); img_ntfs_umount(f); } img_qcow2_close(q); }
static void c_ntfs_bytes1(void){ img_qcow2*q=openq(); { img_ntfs_fs*f=img_ntfs_mount_rw(q,ESP+1000,1); g_rc=(int)(f?0:-1); img_ntfs_umount(f); } img_qcow2_close(q); }
static void c_ntfs_bytes_umax(void){ img_qcow2*q=openq(); { img_ntfs_fs*f=img_ntfs_mount_rw(q,ESP+1000,~(uint64_t)0); g_rc=(int)(f?0:-1); img_ntfs_umount(f); } img_qcow2_close(q); }
static void c_ntfs_lba_umax(void){ img_qcow2*q=openq(); { img_ntfs_fs*f=img_ntfs_mount_rw(q,~(uint64_t)0,4096); g_rc=(int)(f?0:-1); img_ntfs_umount(f); } img_qcow2_close(q); }

static void c_pt_disksec0(void){ img_qcow2*q=openq(); img_parttable_t t; g_rc=img_parttable_read(q,&t,0); img_qcow2_close(q); }
static void c_pt_disksec_huge(void){ img_qcow2*q=openq(); img_parttable_t t; g_rc=img_parttable_read(q,&t,1ull<<50); img_qcow2_close(q); }
static void c_pt_add_umax(void){ img_parttable_t t; memset(&t,0,sizeof t); t.table=IMG_TABLE_GPT; t.disk_sectors=1000;
    uint32_t i=0; g_rc=img_partition_add(&t,~(uint64_t)0,~(uint64_t)0,IMG_MBR_EMPTY,IMG_GUID_BASIC,NULL,false,&i); }
static void c_pt_resize_umax(void){ img_parttable_t t; memset(&t,0,sizeof t); t.table=IMG_TABLE_GPT; t.disk_sectors=1000;
    g_rc=img_partition_resize(&t,0,~(uint64_t)0,NULL); }
static void c_pt_fill_big(void){ img_parttable_t t; memset(&t,0,sizeof t); t.table=IMG_TABLE_GPT; t.disk_sectors=1000;
    g_rc=img_partition_fill_remaining(&t,0,NULL); }

static void c_bc_badmode(void){ img_qcow2*q=openq(); img_bootcheck_result_t r; g_rc=img_bootcheck(q,(img_boot_mode_t)999,&r,NULL); img_qcow2_close(q); }
static void c_fixboot_lba0(void){ img_qcow2*q=openq(); g_rc=img_fixboot(q,0,NULL,0,NULL); img_qcow2_close(q); }   /* 别写坏 MBR */
static void c_fixboot_umax(void){ img_qcow2*q=openq(); g_rc=img_fixboot(q,~(uint64_t)0,NULL,0,NULL); img_qcow2_close(q); }

static void c_wim_notwim(void){ img_qcow2*q=openq(); g_rc=img_wim_apply("/etc/hostname",1,q,ESP,NULL,NULL); img_qcow2_close(q); }
static void c_wim_badidx(void){ img_qcow2*q=openq(); g_rc=img_wim_apply("/etc/hostname",99999,q,ESP,NULL,NULL); img_qcow2_close(q); }

static void c_bcd_nothive(void){ img_bcd_close(g_rc=img_bcd_open_file("/etc/hostname")); }
static void c_bcd_mem_zero(void){ img_bcd_close(g_rc=img_bcd_open_mem("",0)); }
static void c_bcd_mem_huge(void){ char b[16]={0}; img_bcd_close(img_bcd_open_mem(b,~(size_t)0)); }

static struct { const char *n; void(*f)(void); } T[]={
    {"qcow2_read(偏移=UINT64_MAX)",c_r_umax},
    {"qcow2_read_full(偏移=UINT64_MAX-15)",c_rf_umax},
    {"qcow2_read(偏移=文件末尾)",c_r_end},
    {"qcow2_read_full(偏移=末尾-1,跨边界)",c_r_endminus1},
    {"qcow2_read_full(len=SIZE_MAX)",c_rf_huge_len},
    {"qcow2_write(偏移=UINT64_MAX-4096)",c_w_umax},
    {"qcow2_write(偏移=文件末尾)",c_w_end},
    {"qcow2_write(len=SIZE_MAX)",c_w_lenhuge},
    {"qcow2_expand(0)",c_expand_0},{"qcow2_expand(1字节)",c_expand_1},
    {"qcow2_expand(UINT64_MAX)",c_expand_umax},{"qcow2_expand(缩到1MB)",c_expand_small},
    {"fat32_mount(LBA 0 = MBR 区！)",c_fat_lba0},
    {"fat32_mount(LBA 1 = GPT 头！)",c_fat_lba1},
    {"fat32_mount(LBA=盘尾)",c_fat_lba_end},
    {"fat32_mount(LBA=1TB)",c_fat_lba_1t},
    {"fat32_format(默认=盘首？)",c_fat_fmt_lba0},
    {"fat32_put_file(4000字符路径)",c_fat_put_longpath},
    {"fat32_mkdir_p(100层嵌套)",c_fat_mkdir_deep},
    {"fat32_read_file(5000字符路径)",c_fat_read_longpath},
    {"fat32_put_data(len=SIZE_MAX)",c_fat_putdata_umax},
    {"fat32_put_data(len=0)",c_fat_putdata_zero},
    {"fat32_probe(LBA 0)",c_fat_probe_lba0},
    {"ntfs_mount_rw(part_bytes=0)",c_ntfs_bytes0},
    {"ntfs_mount_rw(part_bytes=1)",c_ntfs_bytes1},
    {"ntfs_mount_rw(part_bytes=UINT64_MAX)",c_ntfs_bytes_umax},
    {"ntfs_mount_rw(LBA=UINT64_MAX)",c_ntfs_lba_umax},
    {"parttable_read(disk_sectors=0)",c_pt_disksec0},
    {"parttable_read(disk_sectors=1PB)",c_pt_disksec_huge},
    {"partition_add(first/sectors=UINT64_MAX)",c_pt_add_umax},
    {"partition_resize(0, UINT64_MAX)",c_pt_resize_umax},
    {"partition_fill_remaining(全盘0段)",c_pt_fill_big},
    {"bootcheck(mode=999 非法枚举)",c_bc_badmode},
    {"fixboot(LBA 0 = MBR 区！)",c_fixboot_lba0},
    {"fixboot(LBA=UINT64_MAX)",c_fixboot_umax},
    {"wim_apply(非 WIM 文件)",c_wim_notwim},
    {"wim_apply(索引=99999)",c_wim_badidx},
    {"bcd_open_file(非 hive 文件)",c_bcd_nothive},
    {"bcd_open_mem(len=0)",c_bcd_mem_zero},
    {"bcd_open_mem(len=SIZE_MAX)",c_bcd_mem_huge},
};
int main(int argc,char**argv){
    if(argc<4){ fprintf(stderr,"用法: %s <模板.qcow2> <esp_lba> <工作目录>\n",argv[0]); return 2; }
    IMG=argv[1]; ESP=strtoull(argv[2],NULL,0); GW=argv[3];
    int n=(int)(sizeof T/sizeof T[0]);
    printf("=== 极端参数矩阵（%d 条 · 真实镜像 %s）===\n",n,IMG);
    for(int i=0;i<n;i++) run(T[i].n,T[i].f);
    printf("\n════ 结果：%d 条 / 崩溃 %d ════\n",ncase,ncrash);
    if(crash_n){ printf("❌ 会崩：\n"); for(int i=0;i<crash_n;i++) printf("   · %s\n",crash_list[i]); }
    else printf("✅ 没有崩溃\n");

    /* ★ 数据安全扫描：坏参数跑完后，镜像还能被【独立实现 qemu-img】正常打开吗？
     *   （"传错参把用户盘写坏"比崩溃更不可接受）*/
    printf("\n── 数据安全扫描（qemu-img check 每张用例盘）──\n");
    char cmd[1200]; int bad=0;
    snprintf(cmd,sizeof cmd,"ls %s/case*.qcow2 2>/dev/null",GW);
    FILE *p=popen(cmd,"r"); char line[512];
    while(p && fgets(line,sizeof line,p)){
        size_t L=strlen(line); while(L&&(line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0;
        if(!L) continue;
        char c2[1400]; snprintf(c2,sizeof c2,"qemu-img check '%s' >/dev/null 2>&1",line);
        if(system(c2)!=0){ bad++; printf("  💾 %s 被写坏（qemu-img 打不开）\n",line); }
    }
    if(p) pclose(p);
    printf(bad? "❌ 有 %d 张镜像被坏参数写坏\n" : "✅ 全部用例盘仍可被 qemu-img 正常打开（坏参数没毁数据）\n", bad);
    return (crash_n||bad)?1:0;
}
