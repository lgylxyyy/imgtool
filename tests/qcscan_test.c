/* qcscan-test — qcow2 结构自检（qc.verify 的底层）
 *
 * 为什么要有：手机上进程被杀是必然场景。qcow2 的 L1/L2 可能只写了一半，
 * 再往里写就会出现"两个 guest 簇指向同一个 host 簇"（别名），上层看到的是
 * NTFS「索引块签名错误」——实测 Win8 x86 灌 WIM 撞过一次，但不可稳定复现。
 * 所以不猜：写前扫一遍 L1/L2，别名/越界/坏表直接查出来。
 *
 * 本测试钉三件事：
 *   ① 健康镜像：clean=true，referenced 合理
 *   ② 手工把两条 L2 指向同一簇 → duplicate_refs=1（模拟别名）
 *   ③ 手工写一条指向文件外的 L2 → out_of_range=1
 */
#include "img_qcow2_int.h"
#include "img_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static int fails = 0;
#define OK(c, m) do{ if(c) printf("  ✅ %s\n", m); else { printf("  ❌ %s\n", m); fails++; } }while(0)

static uint64_t be64r(const unsigned char *b){
    return ((uint64_t)b[0]<<56)|((uint64_t)b[1]<<48)|((uint64_t)b[2]<<40)|((uint64_t)b[3]<<32)|
           ((uint64_t)b[4]<<24)|((uint64_t)b[5]<<16)|((uint64_t)b[6]<<8)|(uint64_t)b[7];
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    const char *td = img_tmpdir(); char path[528];
    snprintf(path, sizeof path, "%s/qcscanXXXXXX", td ? td : ".");
    int fd = mkstemp(path); if(fd >= 0) close(fd);
    printf("qcscan-test — qcow2 结构自检\n");

    /* ---- 建 64 MiB 镜像 ---- */
    img_io_opts_t io = {0}; io.writable = true;
    img_io *fh = img_io_open_path(path, &io);
    img_qcow2_create_opts_t co = {0}; co.size = 64ull<<20; co.cluster_bits = 16;
    OK(fh && img_qcow2_create(fh, &co) == IMG_OK, "建 64 MiB qcow2");
    img_io_close(fh);

    /* ---- 写两段数据 ---- */
    io.writable = true; fh = img_io_open_path(path, &io);
    img_qcow2_opts_t qo = {0}; img_qcow2 *q = img_qcow2_open(fh, &qo);
    OK(q && img_qcow2_write_init(q) == IMG_OK, "打开可写");
    char buf[4096]; memset(buf, 'A', sizeof buf);
    OK(img_qcow2_write(q, 0, buf, sizeof buf) == IMG_OK, "写 guest 0");
    OK(img_qcow2_write(q, 1u<<20, buf, sizeof buf) == IMG_OK, "写 guest 1 MiB");
    img_io_sync(q->io);

    img_qcow2_scan_t sc;
    OK(img_qcow2_scan(q, &sc) == IMG_OK, "扫描");
    OK(sc.duplicate_refs == 0, "健康镜像：无别名");
    OK(sc.out_of_range == 0 && sc.bad_l2 == 0, "健康镜像：无越界/坏表");
    OK(sc.l2_tables >= 1 && sc.referenced >= 3, "健康镜像：L2 表与引用簇计数合理");

    /* ---- 手工造别名：把 L2[1] 改成 L2[0] ---- */
    const img_qcow2_header_t *h = img_qcow2_header(q);
    uint64_t l1e = 0;
    { unsigned char b[8]; img_io_read_full(q->io, h->l1_table_offset, b, 8); l1e = be64r(b); }
    uint64_t l2off = l1e & 0x00fffffffffffe00ULL;
    OK(l2off != 0, "读到 L1[0] → L2 表偏移");
    { unsigned char tbl[65536];
      img_io_read_full(q->io, l2off, tbl, 65536);
      if(be64r(tbl) == 0){ printf("  ⚠ L2[0] 是 0，跳过别名造例\n"); }
      else { memcpy(tbl + 8, tbl, 8); img_io_write_full(q->io, l2off, tbl, 65536); }
      img_io_sync(q->io); }
    img_qcow2_close(q);       /* 关盘（内部会 sync）*/
    img_io_close(fh);

    io.writable = false; fh = img_io_open_path(path, &io);
    img_qcow2 *q2 = img_qcow2_open(fh, &qo);
    img_qcow2_scan_t sc2; memset(&sc2, 0, sizeof sc2);
    OK(q2 && img_qcow2_scan(q2, &sc2) == IMG_OK, "重开后再扫描");
    OK(sc2.duplicate_refs >= 1, "★ 造出来的别名被查出来（duplicate_refs≥1）");

    /* ---- 手工造越界：L2[2] 指向文件外 ----
     * 注意：q2 是只读打开的，poke 要用另一个可写句柄（第一次写成只读 → 静默失败，
     * 我还以为是扫描漏了；这也是个教训：写失败必须判返回值）。*/
    { unsigned char tbl[65536]; unsigned char b[8];
      img_io_read_full(q2->io, h->l1_table_offset, b, 8);
      uint64_t lo = be64r(b) & 0x00fffffffffffe00ULL;
      img_io_read_full(q2->io, lo, tbl, 65536);
      uint64_t bogus = (h->size * 8) & 0x00fffffffffffe00ULL;   /* 远超文件 */
      tbl[16] = (unsigned char)(bogus >> 56); tbl[17] = (unsigned char)(bogus >> 48);
      tbl[18] = (unsigned char)(bogus >> 40); tbl[19] = (unsigned char)(bogus >> 32);
      tbl[20] = (unsigned char)(bogus >> 24); tbl[21] = (unsigned char)(bogus >> 16);
      tbl[22] = (unsigned char)(bogus >> 8);  tbl[23] = (unsigned char)bogus;
      img_io_opts_t wio = {0}; wio.writable = true;
      img_io *w = img_io_open_path(path, &wio);
      int wrc = w ? img_io_write_full(w, lo, tbl, 65536) : IMG_EIO;
      if(w){ img_io_sync(w); img_io_close(w); }
      OK(wrc == IMG_OK, "写入伪造的 L2 条目（可写句柄）"); }
    img_qcow2_scan_t sc3; memset(&sc3, 0, sizeof sc3);
    OK(img_qcow2_scan(q2, &sc3) == IMG_OK, "越界后再扫描");
    OK(sc3.out_of_range >= 1, "★ 越界引用被查出来（out_of_range≥1）");

    img_qcow2_close(q2); img_io_close(fh);

    /* ================= 崩溃场景：写一半被 kill -9 =================
     * 手机上"进程被系统杀掉"是必然事件，所以这条必须过：
     *   被杀之后重开 → 扫描必须干净（无别名/越界）→ 继续写不许串到旧数据里。*/
    printf("\n── kill -9 之后续写（崩溃一致性）──\n");
    char path2[528];
    snprintf(path2, sizeof path2, "%s/qckillXXXXXX", td ? td : ".");
    int fd2 = mkstemp(path2); if(fd2 >= 0) close(fd2);
    img_io_opts_t io2 = {0}; io2.writable = true;
    img_io *fh2 = img_io_open_path(path2, &io2);
    img_qcow2_create_opts_t co2 = {0}; co2.size = 256ull<<20; co2.cluster_bits = 16;
    OK(fh2 && img_qcow2_create(fh2, &co2) == IMG_OK, "建 256 MiB 测试盘");
    img_io_close(fh2);

    pid_t pid = fork();
    if(pid == 0){
        /* 子进程：连续写 64K 块，每块按偏移填不同字节；被 SIGKILL 打断 */
        img_io_opts_t ci = {0}; ci.writable = true;
        img_io *cf = img_io_open_path(path2, &ci);
        img_qcow2_opts_t cq0 = {0}; img_qcow2 *cq = cf ? img_qcow2_open(cf, &cq0) : NULL;
        if(!cq || img_qcow2_write_init(cq) != IMG_OK) _exit(3);
        unsigned char *blk = malloc(65536);
        for(uint64_t i = 0; i < 3000; i++){
            memset(blk, (int)(i & 0xff), 65536);
            if(img_qcow2_write(cq, i * 65536, blk, 65536) != IMG_OK) _exit(4);
        }
        _exit(0);
    }
    struct timespec ts = {0, 60*1000*1000};   /* 60 ms 后杀掉 */
    nanosleep(&ts, NULL);
    kill(pid, SIGKILL);
    int st = 0; waitpid(pid, &st, 0);
    OK(WIFSIGNALED(st), "子进程确实是被 SIGKILL 打死的（不是自己跑完）");

    io2.writable = true; fh2 = img_io_open_path(path2, &io2);
    img_qcow2_opts_t qo2 = {0}; img_qcow2 *q3 = img_qcow2_open(fh2, &qo2);
    OK(q3 && img_qcow2_write_init(q3) == IMG_OK, "被杀后重新打开可写");
    img_qcow2_scan_t sc4; memset(&sc4, 0, sizeof sc4);
    OK(img_qcow2_scan(q3, &sc4) == IMG_OK, "被杀后扫描");
    OK(sc4.duplicate_refs == 0 && sc4.out_of_range == 0 && sc4.bad_l2 == 0,
       "★ 被杀后镜像仍然干净（无别名/越界）");

    /* 继续往高位写一段"毒"数据，再看旧区域有没有被串改 */
    unsigned char *poison = malloc(1u<<20); memset(poison, 0xEE, 1u<<20);
    OK(img_qcow2_write(q3, 128ull<<20, poison, 1u<<20) == IMG_OK, "续写 1 MiB 到 128 MiB 处");
    int bad = 0;
    unsigned char *rb = malloc(65536);
    for(uint64_t i = 0; i < 64; i++){        /* 抽查前 4 MiB */
        memset(rb, 0, 65536);
        if(img_qcow2_read_full(q3, i*65536, rb, 65536) != IMG_OK){ bad++; continue; }
        for(int k = 0; k < 65536; k++){
            unsigned char want = (unsigned char)(i & 0xff);
            if(rb[k] != want && rb[k] != 0xEE && rb[k] != 0){ bad++; break; }  /* 0xEE 出现＝串了 */
            if(rb[k] == 0xEE){ bad++; break; }
        }
    }
    OK(bad == 0, "★ 续写没有串到旧区域（前 4 MiB 抽查 64 块）");
    free(poison); free(rb);
    img_qcow2_close(q3); img_io_close(fh2);
    unlink(path2);

    unlink(path);
    printf(fails ? "\n❌ qcscan-test 失败 %d 项\n" : "\n✅ qcscan-test 全过\n", fails);
    return fails ? 1 : 0;
}