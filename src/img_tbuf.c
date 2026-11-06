/* img_tbuf.c — 见 img_tbuf.h 的说明（REQ-21：把 1.5 MB 静态 TLS 搬到堆上）*/
#include "img_tbuf.h"
#include <pthread.h>
#include <stdlib.h>

typedef struct { char *p; size_t cap; } tb_slot;
typedef struct { tb_slot s[IMG_TB_COUNT]; } tb_arena;

static pthread_key_t  g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void tb_free(void *ud){
    tb_arena *a=(tb_arena*)ud;
    if(!a) return;
    for(int i=0;i<IMG_TB_COUNT;i++) free(a->s[i].p);
    free(a);
}
static void tb_init(void){ (void)pthread_key_create(&g_key,tb_free); }

char *img_tbuf(img_tb_id id, size_t want){
    if((int)id<0 || (int)id>=IMG_TB_COUNT || want==0) return NULL;
    pthread_once(&g_once,tb_init);
    tb_arena *a=(tb_arena*)pthread_getspecific(g_key);
    if(!a){
        a=(tb_arena*)calloc(1,sizeof *a);
        if(!a) return NULL;
        if(pthread_setspecific(g_key,a)!=0){ free(a); return NULL; }
    }
    tb_slot *s=&a->s[id];
    if(s->cap < want){
        char *np=(char*)realloc(s->p,want);
        if(!np) return NULL;          /* 旧内容还在，调用方按失败处理 */
        s->p=np; s->cap=want; s->p[0]='\0';
    } else if(!s->p[0]){
        s->p[0]='\0';                 /* 新块第一次用：已 calloc/realloc，这里只是保险 */
    }
    return s->p;
}
