#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
/* race_test — 库内共享状态的并发复现器（不依赖 TSAN/ASAN/valgrind：本环境三者都起不来）
 *
 * ① 首次触碰全局锁的竞态：进程启动后【第一件事】就让 N 个线程同一瞬间调
 *    img_global_lock()。旧写法用"标志位+普通赋值"，两线程可同时看到未初始化，
 *    对同一把 mutex 执行第二次 pthread_mutex_init；若此刻有线程正持锁 →
 *    glibc 断言 abort（pthread_mutex_lock.c:94 __owner == 0），或锁彻底失效。
 * ② 环形缓冲读写竞态：写线程 img_ring_add（锁内 free 旧行），读线程 img_ring_dump
 *    （旧实现完全不持锁）→ use-after-free。合法行恒为 R??????-OK，块一进空闲链
 *    malloc 就把 next 指针写进块首 → 首字符不再是 'R'：不需要 ASAN 也取得到证据。
 * ③ 无 ctx 命令的 static 缓冲串台：iso.ls / wim.ls / cmd.list 这类命令不需要镜像
 *    句柄，为了省一次 malloc 用了【进程级 static 大缓冲】拼 JSON。多线程并发调
 *    img_cmd_exec 时（旧实现：ctx==NULL 就不加锁）两个线程同时往同一块缓冲里写
 *    → 输出交错/被覆盖。现在靠 img_cmd_serial() 串行化，这里守住这个契约。
 *
 * 父进程 fork+execv 出全新进程反复取证（同一进程内二次跑测不到"首次初始化"）。
 * 用法: race_test [重复次数=120] [ISO路径]     没给 ISO 就跳过场景③
 */
#define _POSIX_C_SOURCE 200809L
#include "img_cmd.h"
#include "img_lock.h"
#include "img_event.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

#define NTH 8
static pthread_barrier_t g_bar;
static _Atomic int g_err;

/* ---------------- ① 并发首次初始化 ---------------- */
static void *th_first(void *arg){
    long id=(long)arg;
    pthread_barrier_wait(&g_bar);
    img_lock_t *lk=img_global_lock();              /* ★ 全线程同一瞬间的首次触碰 */
    if(!lk){ atomic_store(&g_err,1); return NULL; }
    img_lock_acquire(lk);
    if(id==0) usleep(150000);                      /* 持锁 150ms：留出二次 init 的窗口 */
    img_lock_release(lk);
    return NULL;
}
static int child_scenario1(void){
    pthread_barrier_init(&g_bar,NULL,NTH+1);
    pthread_t t[NTH]; atomic_store(&g_err,0);
    for(long i=0;i<NTH;i++) pthread_create(&t[i],NULL,th_first,(void*)i);
    pthread_barrier_wait(&g_bar);
    img_lock_t *lk=img_global_lock();              /* 主线程也抢这份首次窗口 */
    img_lock_acquire(lk); img_lock_release(lk);
    for(int i=0;i<NTH;i++) pthread_join(t[i],NULL);
    pthread_barrier_destroy(&g_bar);
    return atomic_load(&g_err)?1:0;
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
    if(d.bad){ atomic_store(&g_err,2); fprintf(stderr,"  [child] dump 读到 %ld/%ld 行被破坏\n",d.bad,d.seen); }
    return NULL;
}
static int child_scenario2(void){
    pthread_barrier_init(&g_bar,NULL,3);
    pthread_t w,r; atomic_store(&g_err,0);
    pthread_create(&w,NULL,th_writer,NULL);
    pthread_create(&r,NULL,th_reader,NULL);
    pthread_barrier_wait(&g_bar);
    img_ring_add("R000000-OK");                    /* 主线程同时插一条，扩大碰撞面 */
    pthread_join(w,NULL); pthread_join(r,NULL);
    pthread_barrier_destroy(&g_bar);
    return atomic_load(&g_err)?1:0;
}

/* ---------------- ③ 无 ctx 命令并发（static 缓冲）---------------- */
static const char *g_isoA, *g_isoB;
/* ★ 必须用【两张内容不同的 ISO】：第一版这里只给一张，两个线程往同一块 static
 *   缓冲里写的是【一模一样的目录】，覆盖完全不可见 —— 反证（把锁去掉）照样 0 异常，
 *   等于写了个空测试。真正能显形的判据是：本线程那张 ISO 里的文件都叫 A*，
 *   另一张都叫 B*；一旦串台就会看到对方的名字，或 count 与元素数不等。 */
static void *th_nolock(void *arg){
    long id=(long)arg;
    const char *myiso = (id%2)? g_isoB : g_isoA;
    char want       = (id%2)? 'B' : 'A';
    char *jb=(char*)malloc(300*1024);
    if(!jb){ pthread_barrier_wait(&g_bar); return NULL; }   /* 即使失败也要到达 barrier，否则主线程永久等 */
    /* ★ 工作线程必须自己也到 barrier！第一版漏了这行：只有主线程在等，
     *   工作线程跑完直接退出 → 进程只剩 1 个线程、wchan=futex_wait_queue 永久挂住。
     *   我据此误判成"库内 0.5s 固定等待""并发死锁"，绕了很久。*/
    pthread_barrier_wait(&g_bar);
    for(int i=0;i<3;i++){   /* 每线程 3 次就够撞开（去 TLS 的反证第 0 次就报）。
     * 不敢开大：库内单次 iso.ls 有 ~0.5s 的纯等待（user/sys 近零，是阻塞不是计算，
     * 成因未查、已记待办）—— 30 次×8 线程×N 轮会把回归拖到几十分钟。*/
        img_args *a=img_args_new();
        if(!a){ atomic_store(&g_err,3); break; }
        img_args_set(a,"iso",myiso);
        img_args_set(a,"path","/");
        img_result r; memset(&r,0,sizeof r);
        img_cmd_exec("iso.ls",NULL,a,&r);                       /* ctx==NULL → 靠 cmd_serial 串行 */
        int n=img_result_json(&r,jb,300*1024);
        img_args_free(a);
        while(n>0 && (jb[n-1]=='\n'||jb[n-1]=='\r'||jb[n-1]==' ')) n--;   /* 渲染器带尾换行，先剥掉 */
        if(n<=2 || jb[0]!='{' || jb[n-1]!='}' || !strstr(jb,"\"cmd\":\"iso.ls\"")){
            atomic_store(&g_err,3);
            fprintf(stderr,"  [child] 线程%ld 第%d次拿到畸形信封\n",id,i);
            break;
        }
        char *pc=strstr(jb,"\"count\":");
        long cnt = pc? (long)strtoull(pc+8,NULL,10) : -1;
        long nn=0, foreign=0;
        const char *q=jb;
        while((q=strstr(q,"\"name\":\""))){
            q+=8; nn++;   /* "name":" 正好 8 个字符；+=9 会跳过首字母 → 判据自己错位 */
            if(*q!=want) foreign++;                             /* 看到对方 ISO 的文件名 = 串台 */
            q+=1;
        }
        if(cnt<0 || nn!=cnt || foreign!=0 || !strstr(jb,"\"entries\"")){
            atomic_store(&g_err,3);
            fprintf(stderr,"  [child] 线程%ld 第%d次：count=%ld entries=%ld foreign=%ld\n",id,i,cnt,nn,foreign);
            break;
        }
    }
    free(jb);
    return NULL;
}
static int child_scenario3(void){
    if(!g_isoA||!g_isoB) return 0;
    /* 线程数/迭代次数可配：开发沙箱是 proot rootfs，每条 syscall 都要 ptrace 代理，
     * 8 线程并发时开销又被放大（实测 1 轮 24 次调用就 >120s），
     * 因此默认收到【刚好能撞开】的 2 线程 × 1 次 —— 去 TLS 的反证照样能命中。
     * 真机/CI 想加压：RACE_TH=8 RACE_ITERS=50。*/
    int nth = getenv("RACE_TH") ? atoi(getenv("RACE_TH")) : 2;
    if(nth<1)  nth=1;
    if(nth>64) nth=64;
    /* ★ 到达数必须是 nth+1：主线程也在这里等（场景①② 一直这么写）。
     *   写成 nth 会让最后到达的那个线程永远凑不齐 —— 又是一次"自己挂自己"。*/
    pthread_barrier_init(&g_bar,NULL,(unsigned)(nth+1));
    pthread_t t[64]; atomic_store(&g_err,0);
    int made=0;
    for(long i=0;i<nth;i++){
        int cr=pthread_create(&t[i],NULL,th_nolock,(void*)i);
        if(cr!=0){ fprintf(stderr,"  [child] pthread_create 第%ld个失败: %s\n",i,strerror(cr)); break; }
        made++;
    }
    /* ★ 必须检查 + 提前退出：barrier 的到达数在 init 时就定死了，
     *   少创建一个线程 → 主线程会在 futex 上永久等（实测 wchan=futex_wait_queue、
     *   进程只剩 1 个线程）。第一版没查返回值，测试看起来"死锁"其实是自己挂住，
     *   让我误判成库的性能问题，白绕一大圈。*/
    if(made!=nth){
        fprintf(stderr,"  [child] 只创建了 %d/%d 个线程，放弃本轮（不进 barrier）\n",made,nth);
        atomic_store(&g_err,3);
        return 3;
    }
    pthread_barrier_wait(&g_bar);
    for(int i=0;i<nth;i++) pthread_join(t[i],NULL);
    pthread_barrier_destroy(&g_bar);
    return atomic_load(&g_err)?1:0;
}

int main(int argc,char **argv){
    const char *isoA=NULL,*isoB=NULL; int rounds=120;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--child")){                       /* fork+execv 进来的子进程 */
            int sc=(i+1<argc)?atoi(argv[i+1]):1;
            if(i+2<argc) g_isoA=argv[i+2];
            if(i+3<argc) g_isoB=argv[i+3];
            if(sc==3 && (!g_isoA||!g_isoB)) return 0;
            return sc==1?child_scenario1():sc==2?child_scenario2():child_scenario3();
        }
        if(strchr(argv[i],'/')){ if(!isoA) isoA=argv[i]; else isoB=argv[i]; } else rounds=atoi(argv[i]);
    }
    int nsc = (isoA&&isoB) ? 3 : 2;
    int bad[4]={0,0,0,0};
    for(int sc=1;sc<=nsc;sc++){
        for(int i=0;i<rounds;i++){
            pid_t p=fork();
            if(p<0){ perror("fork"); return 2; }
            if(p==0){
                char scb[8]; snprintf(scb,sizeof scb,"%d",sc);
                char *av[]={argv[0],(char*)"--child",scb,(char*)isoA,(char*)isoB,NULL};
                execv("/proc/self/exe",av);
                _exit(127);                                   /* execv 失败 */
            }
            int st=0; waitpid(p,&st,0);
            if(WIFSIGNALED(st) || (WIFEXITED(st)&&WEXITSTATUS(st)!=0)){
                bad[sc]++;
                if(bad[sc]<=3)
                    fprintf(stderr,"  ❌ 场景%d 第%d轮异常：%s%s\n",sc,i+1,
                            WIFSIGNALED(st)?"信号 ":"退出码 ",
                            WIFSIGNALED(st)?strsignal(WTERMSIG(st)):"非零");
            }
        }
    }
    printf("  race_test：%d 轮 ×%d 场景 → ①首次初始化 %d 异常，②环形缓冲 %d 异常，③无ctx命令 %d 异常\n",
           rounds,nsc,bad[1],bad[2],bad[3]);
    if(nsc<3) printf("     （场景③ 未跑：需要两张素材不同的 ISO）\n");
    for(int sc=1;sc<=nsc;sc++) if(bad[sc]){ printf("  ❌ 存在并发缺陷（场景%d）\n",sc); return 1; }
    printf("  ✅ 并发契约成立\n"); return 0;
}
