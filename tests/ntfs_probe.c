/* ntfs_probe — 搬移原语三场景矩阵实测（隔离 ntfs_delete EIO 的病因）
 * 用法: ntfs_probe <qcow2> <part_lba> <file|emptydir|fulldir>
 *   每个场景一张全新卷，互不影响；退出码 0=通过
 * 打印 link_count（MFT_RECORD.link_count）观察 ntfs_link 是否正确计数 ——
 * 若 link 后 count 没变，ntfs_delete 就会以为"最后一个链接"去释放目录 → 与症状吻合。*/
#include "img_ntfs_fs.h"
#include "img_qcow2.h"
#include "img_io.h"
#include "img_partition.h"
#include "ntfs_bridge.h"   /* 仅诊断用：直接调 libntfs-3g（已 target_include_directories src/）*/
#include <ntfs-3g/logging.h>
#include <ntfs-3g/index.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static bool mftref_ok(u64 ref){ return ref!=(u64)-1 && ref!=0; }
static int fails=0;
/* 与库内 to_ntfs 等价：转 UTF-16（不改大小写），返回字节数级长度 */
static bool to_ntfs_check(const char *s, ntfschar **out, size_t *outlen){
    *out=NULL; *outlen=0;
    if(ntfs_mbstoucs(s,out)<0 || !*out) return false;
    *outlen=strlen(s); return true;
}
#define CHECK(cond, msg) do{ if(cond) printf("  ✅ %s\n", msg); else { printf("  ❌ %s\n", msg); fails++; } }while(0)

typedef struct { int n; char names[32][64]; } cnt_t;
static void lc(void *ud, const char *name, bool is_dir, uint64_t size){
    (void)is_dir; (void)size; cnt_t *c=(cnt_t*)ud;
    if(c->n<32){ snprintf(c->names[c->n],sizeof c->names[0],"%s",name); c->n++; }
}
static int has(cnt_t*c,const char*n){ for(int i=0;i<c->n;i++) if(!strcmp(c->names[i],n)) return 1; return 0; }


int main(int argc, char **argv){
    if(argc<4){ fprintf(stderr,"用法: %s <qcow2> <part_lba> <file|emptydir|fulldir>\n",argv[0]); return 2; }
    setvbuf(stdout,NULL,_IONBF,0);
    uint64_t lba=strtoull(argv[2],NULL,0);
    const char *mode=argv[3];

    img_io_opts_t io={0}; io.writable=true;
    img_io *fh=img_io_open_path(argv[1],&io);
    if(!fh){ printf("❌ 打不开镜像\n"); return 1; }
    img_qcow2_opts_t qo={0};
    img_qcow2 *q=img_qcow2_open(fh,&qo);
    if(!q || img_qcow2_write_init(q)!=IMG_OK){ printf("❌ qcow2\n"); return 1; }
    img_parttable_t t; if(img_parttable_read(q,&t,0)){ printf("❌ 分区表\n"); return 1; }
    uint64_t pb=0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==lba){ pb=(t.parts[i].last_lba-t.parts[i].first_lba+1)*512; break; }
    if(!pb){ printf("❌ 分区不存在\n"); return 1; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ printf("❌ 挂载失败\n"); return 1; }

    const char *victim;
    if(!strcmp(mode,"file")){
        victim="T3";
        img_ntfs_put_file_mem(fs,"file-payload",12,"/T3");
    } else if(!strcmp(mode,"emptydir")){
        victim="T2";
        img_ntfs_mkdir_p(fs,"/T2");
    } else if(!strcmp(mode,"rawdel")){
        victim="D1";
        img_ntfs_put_file_mem(fs,"rawdel-payload",14,"/D1");
    } else if(!strcmp(mode,"linkreopen")){
        victim="D2";
        img_ntfs_put_file_mem(fs,"linkreopen-pay",14,"/D2");
    } else {
        victim="T1";
        img_ntfs_mkdir_p(fs,"/T1/child");
        img_ntfs_put_file_mem(fs,"inside",6,"/T1/child/f.bin");
    }
    img_ntfs_mkdir_p(fs,"/Dst");

    cnt_t r0={0}; img_ntfs_list_dir(fs,"/",lc,&r0);
    printf("  场景=%s 初始根: ", mode); for(int i=0;i<r0.n;i++) printf("%s ",r0.names[i]); printf("\n");

    uint64_t f1=0,u1=0; img_ntfs_free_bytes(fs,&f1,&u1);

    /* ================= 诊断模式（原生调用，隔离病因） ================= */
    if(!strcmp(mode,"rawdel") || !strcmp(mode,"linkthen") || !strcmp(mode,"linkreopen") || !strcmp(mode,"stepwise")){
        ntfs_volume *vol=img_ntfs_probe_vol(fs);
        /* 打开 libntfs-3g 自己的日志（ERROR/PERROR/CRITICAL），直接看它报哪句 */
        ntfs_log_set_levels(NTFS_LOG_LEVEL_ERROR|NTFS_LOG_LEVEL_PERROR|
                            NTFS_LOG_LEVEL_CRITICAL|NTFS_LOG_LEVEL_WARNING|
                            NTFS_LOG_LEVEL_DEBUG|NTFS_LOG_LEVEL_INFO);
        ntfs_log_set_flags(NTFS_LOG_FLAG_PREFIX|NTFS_LOG_FLAG_LINE|NTFS_LOG_FLAG_FUNCTION);
        const char *victim_name = !strcmp(mode,"rawdel") ? "D1" : (!strcmp(mode,"linkthen") ? "D2" : "D3");
        char vp[64]; snprintf(vp,sizeof vp,"/%s",victim_name);
        img_ntfs_put_file_mem(fs,"payload-0123456789",19,vp);
        ntfs_inode *root=ntfs_inode_open(vol, FILE_root);
        ntfschar *u=NULL; size_t ul=0;
        to_ntfs_check(victim_name,&u,&ul);
        MFT_REF ref=ntfs_inode_lookup_by_name(root,u,ul);
        if(!mftref_ok(ref)){ printf("  ❌ 找不到 %s\n",victim_name); return 1; }
        ntfs_inode *ni=ntfs_inode_open(vol,ref);
        printf("  [diag] %s link_count=%u\n", victim_name, (unsigned)ni->mrec->link_count);

        if(!strcmp(mode,"stepwise")){
            /* 复刻 ntfs_delete() 的关键三步，逐点打印 rc/errno —— 定位 EIO 出在哪一步 */
            img_ntfs_put_file_mem(fs,"step-payload",12,"/SW");
            ntfs_inode *root2=ntfs_inode_open(vol,FILE_root);
            ntfschar *us=NULL; size_t uls=0; to_ntfs_check("SW",&us,&uls);
            MFT_REF rf=ntfs_inode_lookup_by_name(root2,us,uls);
            ntfs_inode *ni2=ntfs_inode_open(vol,rf);
            printf("  [step] 0) link_count=%u\n",(unsigned)ni2->mrec->link_count);

            ntfs_inode *dst2=NULL; {
                ntfschar *ud=NULL; size_t uld=0; to_ntfs_check("Dst",&ud,&uld);
                MFT_REF drf=ntfs_inode_lookup_by_name(root2,ud,uld);
                if(mftref_ok(drf)) dst2=ntfs_inode_open(vol,drf);
                free(ud);
            }
            errno=0; int lr=ntfs_link(ni2,dst2,us,(u8)uls);
            printf("  [step] 1) ntfs_link rc=%d errno=%d link_count=%u\n",lr,errno,(unsigned)ni2->mrec->link_count);

            ntfs_attr_search_ctx *actx=ntfs_attr_get_search_ctx(ni2,NULL);
            if(!actx){ printf("  [step] 2) get_search_ctx FAILED errno=%d\n",errno); return 1; }
            errno=0;
            int er=ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0, NULL, 0, actx);
            printf("  [step] 2) lookup FILE_NAME rc=%d errno=%d\n",er,errno);
            if(er){ ntfs_attr_put_search_ctx(actx); ntfs_inode_close(dst2); ntfs_inode_close(ni2); ntfs_inode_close(root2); free(us); return 1; }
            FILE_NAME_ATTR *fn=(FILE_NAME_ATTR*)((u8*)actx->attr + le16_to_cpu(actx->attr->value_offset));
            printf("  [step]    fn.parent=%llu dir.mft=%llu name_len=%u name0=%u\n",
                   (unsigned long long)MREF_LE(fn->parent_directory),
                   (unsigned long long)root2->mft_no,
                   (unsigned)fn->file_name_length,(unsigned)fn->file_name[0]);

            /* ★ 复刻 ntfs_inode_sync_file_name 的动作：对每个剩下的 FILE_NAME，
             *   去父目录索引里查（close 时就是这么干的；查不到 → 被改成 EIO）。*/
            printf("  [walk] ── link 后立即遍历 FILE_NAME 并查父目录索引 ──\n");
            {   ntfs_attr_reinit_search_ctx(actx);
                while(!ntfs_attr_lookup(AT_FILE_NAME, NULL, 0, 0, 0, NULL, 0, actx)){
                    FILE_NAME_ATTR *f2=(FILE_NAME_ATTR*)((u8*)actx->attr + le16_to_cpu(actx->attr->value_offset));
                    char nm[64]; int ml=(int)f2->file_name_length; if(ml>31) ml=31;
                    for(int i=0;i<ml;i++) nm[i]=(char)(f2->file_name[i]&0x7f);
                    nm[ml]=0;
                    printf("  [walk]  name=%s parent=%llu type=%u\n", nm,
                           (unsigned long long)MREF_LE(f2->parent_directory),
                           (unsigned)f2->file_name_type);
                    ntfs_inode *pi=ntfs_inode_open(vol, MREF_LE(f2->parent_directory));
                    if(!pi){ printf("  [walk]    open parent FAILED errno=%d\n",errno); continue; }
                    ntfs_index_context *ic=ntfs_index_ctx_get(pi, NTFS_INDEX_I30, 4);
                    if(!ic){ printf("  [walk]    index_ctx_get FAILED errno=%d\n",errno); ntfs_inode_close(pi); continue; }
                    errno=0;
                    int lr2=ntfs_index_lookup(f2, sizeof(FILE_NAME_ATTR), ic);
                    printf("  [walk]    index_lookup rc=%d errno=%d(%s)\n",lr2,errno,strerror(errno));
                    ntfs_index_ctx_put(ic);
                    if(pi!=root2) ntfs_inode_close(pi);
                }
            }
            errno=0;
            int r1=ntfs_index_remove(root2, ni2, fn, le32_to_cpu(actx->attr->value_length));
            printf("  [step] 3) ntfs_index_remove rc=%d errno=%d(%s)\n",r1,errno,strerror(errno));

            errno=0;
            int r2=ntfs_attr_record_rm(actx);
            printf("  [step] 4) ntfs_attr_record_rm rc=%d errno=%d(%s)\n",r2,errno,strerror(errno));
            ntfs_attr_put_search_ctx(actx);
            printf("  [step]    rm 后 link_count=%u\n",(unsigned)ni2->mrec->link_count);

            cnt_t rr={0}; img_ntfs_list_dir(fs,"/",lc,&rr);
            printf("  [step] 根: "); for(int i=0;i<rr.n;i++) printf("%s ",rr.names[i]); printf("\n");
            ntfs_inode_close(dst2); ntfs_inode_close(ni2); ntfs_inode_close(root2); free(us);
            img_ntfs_umount(fs); img_qcow2_close(q); img_io_close(fh);
            return (r1==0&&r2==0)?0:1;
        }
        if(!strcmp(mode,"rawdel")){
            errno=0;
            int dr=ntfs_delete(vol, vp, ni, root, u, (u8)ul);
            printf("  [diag] 原生 ntfs_delete(未 link) rc=%d errno=%d(%s)\n", dr, errno, strerror(errno));
            printf("  [diag] delete 后 link_count=%u\n", (unsigned)ni->mrec->link_count);
        } else {
            ntfs_inode *dst=NULL; {
                ntfschar *ud=NULL; size_t uld=0; to_ntfs_check("Dst",&ud,&uld);
                MFT_REF dr=ntfs_inode_lookup_by_name(root,ud,uld);
                if(mftref_ok(dr)) dst=ntfs_inode_open(vol,dr);
                free(ud);
            }
            if(!dst){ printf("  ❌ 没有 /Dst\n"); return 1; }
            errno=0;
            int lr=ntfs_link(ni,dst,u,(u8)ul);
            printf("  [diag] ntfs_link rc=%d errno=%d(%s)  link_count=%u\n",
                   lr, errno, strerror(errno), (unsigned)ni->mrec->link_count);
            if(!strcmp(mode,"linkreopen")){
                ntfs_inode_close(ni);
                ni=ntfs_inode_open(vol,ref);
                printf("  [diag] 重开后 link_count=%u\n", (unsigned)ni->mrec->link_count);
            }
            errno=0;
            int dr=ntfs_delete(vol, vp, ni, root, u, (u8)ul);
            printf("  [diag] ntfs_delete(link%s) rc=%d errno=%d(%s)\n",
                   strcmp(mode,"linkreopen")?" 后":"+重开后", dr, errno, strerror(errno));
            ntfs_inode_close(dst);
        }
        ntfs_inode_close(ni); ntfs_inode_close(root); free(u);

        cnt_t r={0}; img_ntfs_list_dir(fs,"/",lc,&r);
        printf("  [diag] 根目录: "); for(int i=0;i<r.n;i++) printf("%s ",r.names[i]); printf("\n");
        printf(has(&r,victim_name) ? "  → 旧名还在（delete 没删掉）\n" : "  → 旧名已删\n");
        img_ntfs_umount(fs); img_qcow2_close(q); img_io_close(fh);
        return 0;
    }

    printf("════ 搬移 /%s → /Dst/%s ════\n", victim, victim);
    const char *names[1]={victim};
    int moved=0;
    int rc=img_ntfs_move_into(fs,"/Dst",names,1,&moved);
    printf("  rc=%d moved=%d errno=%d\n", rc, moved, errno);

    /* 校验 */
    cnt_t r1={0}; img_ntfs_list_dir(fs,"/",lc,&r1);
    printf("  搬后根: "); for(int i=0;i<r1.n;i++) printf("%s ",r1.names[i]); printf("\n");
    cnt_t d1={0}; img_ntfs_list_dir(fs,"/Dst",lc,&d1);
    printf("  Dst 内: "); for(int i=0;i<d1.n;i++) printf("%s ",d1.names[i]); printf("\n");

    char deep[128]; snprintf(deep,sizeof deep,"/Dst/%s",victim);
    CHECK(rc==IMG_OK && moved==1, "搬移返回成功");
    CHECK(!has(&r1,victim), "根目录已无原项");
    CHECK(has(&d1,victim), "目标目录有该项");

    if(!strcmp(mode,"file")){
        void *b=NULL; size_t l=0;
        CHECK(img_ntfs_read_file(fs,"/Dst/T3",&b,&l)==IMG_OK && l==12 && !memcmp(b,"file-payload",12),
              "文件内容完好");
        free(b);
    } else {
        CHECK(img_ntfs_path_exists(fs,"/Dst/T2") || img_ntfs_path_exists(fs,"/Dst/T1"), "目录存在");
    }
    if(!strcmp(mode,"fulldir")){
        void *b=NULL; size_t l=0;
        int rr=img_ntfs_read_file(fs,"/Dst/T1/child/f.bin",&b,&l);
        CHECK(rr==IMG_OK && l==6 && !memcmp(b,"inside",6), "子目录里的文件完好");
        free(b);
    }
    uint64_t f2=0,u2=0; img_ntfs_free_bytes(fs,&f2,&u2);
    printf("  空闲 %llu → %llu (Δ%lld)\n",(unsigned long long)f1,(unsigned long long)f2,
           (long long)f2-(long long)f1);

    img_ntfs_umount(fs);
    img_qcow2_close(q); img_io_close(fh);
    printf(fails? "\n❌ %s 场景失败\n" : "\n✅ %s 场景通过\n", mode);
    return fails?1:0;
}
