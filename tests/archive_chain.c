/* archive_chain.c — 同进程内验证 index_token 的生命周期（REQ-34 §3）
 *  ① 建索引 → 拿 token ②用 token 续页 ③改包（mtime/size）→ 期望 IMG_ESTALE
 *  ④ 用 --path 前缀过滤（不重扫）
 * 单独一个进程跑，因为 token 只在本进程内有效（App 也是在同一个进程里来回调）。
 */
#include "img_archive.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>

static int pass=0, fail=0;
static void chk(const char *what, bool ok, const char *extra){
    if(ok){ pass++; printf("  \u2705 %s\n", what); }
    else  { fail++; printf("  \u274c %s  %s\n", what, extra?extra:""); }
}
static int cnt;
static void cb(void *ud, const img_archive_ent *e){ (void)ud; (void)e; cnt++; }

int main(int argc, char **argv){
    if(argc<2){ fprintf(stderr,"用法: archive_chain <包>\n"); return 2; }
    const char *p=argv[1];
    img_archive_index *ix=NULL;
    char err[256]="";
    chk("建索引", img_archive_index_build(p,&ix,err,sizeof err)==0, err);
    if(!ix) return 1;
    char tok[32]; snprintf(tok,sizeof tok,"%s",img_archive_index_token(ix));
    printf("     token=%s total=%llu fmt=%s\n", tok,
           (unsigned long long)img_archive_index_total(ix), img_archive_index_format(ix));

    img_archive_index *ix2=NULL;
    chk("用 token 取回（不重扫）", img_archive_index_by_token(tok,&ix2,err,sizeof err)==0, err);
    chk("  … 是同一份索引", ix2==ix, "不同指针");

    cnt=0; img_archive_index_walk(ix,"/",true,cb,NULL);
    long total_all=cnt;
    cnt=0; img_archive_index_walk(ix,"/sub",true,cb,NULL);
    printf("     全部=%ld  /sub 下=%d\n", total_all, cnt);
    chk("前缀过滤（递归）", cnt>0 && cnt<total_all, "计数不对");
    cnt=0; img_archive_index_walk(ix,"/",false,cb,NULL);
    chk("非递归只回直接子项", cnt>0 && cnt<total_all, "计数不对");
    cnt=0; img_archive_index_walk(ix,"/不存在",true,cb,NULL);
    chk("不存在的目录 → 0 项（不报错）", cnt==0, "应为 0");

    /* 改包：mtime +1 小时 → 必须失效 */
    struct stat st; stat(p,&st);
    struct timespec ts[2];
    ts[0].tv_sec=st.st_atime; ts[0].tv_nsec=0;
    ts[1].tv_sec=st.st_mtime+3600; ts[1].tv_nsec=0;
    utimensat(AT_FDCWD,p,ts,0);
    img_archive_index *ix3=NULL;
    int rc=img_archive_index_by_token(tok,&ix3,err,sizeof err);
    chk("包改过 → IMG_ESTALE（App 重新索引）", rc==IMG_ESTALE, err);
    printf("     %s\n", err);

    printf("\u2550\u2550\u2550\u2550 结果：通过 %d · 失败 %d \u2550\u2550\u2550\u2550\n", pass, fail);
    return fail?1:0;
}
