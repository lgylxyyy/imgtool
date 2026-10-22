/* race_test — 库内全局锁并发缺陷复现器（不依赖 TSAN/ASAN/valgrind：本环境三者都起不来）
 *   ① 首次触碰全局锁的竞态：进程启动后【第一件事】就让 N 个线程同一瞬间调用
 *      img_global_lock()。若懒初始化不是原子的，同一把 mutex 会被 pthread_mutex_init
 *      两次；第二次落在别的线程【已持锁期间】→ mutex 的 owner 被清零 →
 *      glibc 断言 abort（pthread_mutex_lock.c:94: __owner == 0）或锁彻底失效。
 *   ② 环形缓冲读/写竞态：一线程狂写 img_ring_add，一线程狂读 img_ring_dump。
 *      读侧若不持锁，会在写侧 free 同一块后继续用它 → 拿到的是空闲链指针。
 *      （合法行恒为 R??????-OK；块一进 free 链，malloc 就把 next 指针写进块首，
 *        首字符立刻不是 'R' —— 这就是不需要 ASAN 也能取到的证据。）
 * 父进程重复 fork 子进程取证；任一次异常（abort/signal/内容不符）即整测失败。
 * 用法: race_test [重复次数=120]
 */
#define _POSIX_C_SOURCE 200809L
#include "img_lock.h"
#include "img_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

#define NTH 8
static pthread_barrier_t g_bar;
static int g_scenario;
static volatile sig_atomic_t g_err;

/* ---------------- ① 并发首次初始化 ---------------- */
static void *th_first(void *arg){
    long id=(long)arg;
    pthread_barrier_wait(&g_bar);
    img_lock_t *lk=img_global_lock();              /* ★ 全线程同一瞬间的首次触碰 */
    if(!lk){ g_err=1; return NULL; }
    img_lock_acquire(lk);
    if(id==0) usleep(150000);                      /* 持锁 150ms：留出二次 init 的窗口 */
    img_lock_release(lk);
    return NULL;
}
static int child_scenario1(void){
    pthread_barrier_init(&g_bar,NULL,NTH+1);
    pthread_t t[NTH]; g_err=0;
    for(long i=0;i<NTH;i++) pthread_create(&t[i],NULL,th_first,(void*)i);
    pthread_barrier_wait(&g_bar);
    img_lock_t *lk=img_global_lock();              /* 主线程也抢这份首次窗口 */
    img_lock_acquire(lk); img_lock_release(lk);
    for(int i=0;i<NTH;i++) pthread_join(t[i],NULL);
    pthread_barrier_destroy(&g_bar);
    return g_err?1:0;
}

/* ---------------- ② add / dump 竞争 ---------------- */
#define ITEMS 4000
static void *th_writer(void *a){
    (void)a; char line[32];
    pthread_barrier_wait(&g_bar);
    for(int i=0;i<ITEMS;i++){ snprintf(line,sizeof line,"R%06d-OK",i); img_ring_add(line); }
    return NULL;
}
typedef struct { long seen,bad; } dctx;
static int dcb(void *ud,const char *line){
    dctx *d=(dctx*)ud; d->seen++;
    if(!(line && line[0]=='R' && line[7]=='-' && line[8]=='O' && line[9]=='K' && line[10]==0)) d->bad++;
    return 0;
}
static void *th_reader(void *a){
    (void)a; dctx d={0,0};
    pthread_barrier_wait(&g_bar);
    for(int k=0;k<400;k++){ img_ring_dump(&d,dcb,0); img_ring_count(); usleep(500); }
    if(d.bad){ g_err=2; fprintf(stderr,"  [child] dump 读到 %ld/%ld 行被破坏\n",d.bad,d.seen); }
    return NULL;
}
static int child_scenario2(void){
    pthread_barrier_init(&g_bar,NULL,3);
    pthread_t w,r; g_err=0;
    pthread_create(&w,NULL,th_writer,NULL);
    pthread_create(&r,NULL,th_reader,NULL);
    pthread_barrier_wait(&g_bar);
    img_ring_add("R000000-OK");                    /* 主线程同时插一条，扩大碰撞面 */
    pthread_join(w,NULL); pthread_join(r,NULL);
    pthread_barrier_destroy(&g_bar);
    return g_err?1:0;
}

int main(int argc,char **argv){
    if(argc>2 && !strcmp(argv[1],"--child")){       /* 子进程模式：由 fork+execv 进来 */
        int sc=atoi(argv[2]); return sc==1?child_scenario1():child_scenario2();
    }
    int rounds=(argc>1)?atoi(argv[1]):120;
    int bad1=0,bad2=0;
    for(int sc=1;sc<=2;sc++){
        for(int i=0;i<rounds;i++){
            pid_t p=fork();
            if(p<0){ perror("fork"); return 2; }
            if(p==0){
                char scb[8]; snprintf(scb,sizeof scb,"%d",sc);
                char *av[]={argv[0],(char*)"--child",scb,NULL};
                execv("/proc/self/exe",av);
                _exit(127);                          /* execv 失败 */
            }
            int st=0; waitpid(p,&st,0);
            int abnormal = WIFSIGNALED(st) || (WIFEXITED(st)&&WEXITSTATUS(st)!=0);
            if(abnormal){
                if(sc==1) bad1++; else bad2++;
                if(bad1+bad2<=3)
                    fprintf(stderr,"  ❌ 场景%d 第%d轮异常：%s%s\n",sc,i+1,
                            WIFSIGNALED(st)?"信号 ":"退出码 ",
                            WIFSIGNALED(st)?strsignal(WTERMSIG(st)):"非零");
            }
        }
    }
    printf("  race_test：%d 轮×2 场景 → ①首次初始化异常 %d 次，②环形缓冲损坏 %d 次\n",rounds,bad1,bad2);
    if(bad1||bad2){ printf("  ❌ 存在并发缺陷\n"); return 1; }
    printf("  ✅ 并发契约成立\n"); return 0;
}
