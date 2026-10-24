/* ads_probe.c — 命名数据流（ADS）测试探针
 *
 * 用法：
 *   ads_probe write  <img> <lba>      在 /ADS.bin 上写无名数据 + 两条命名流
 *   ads_probe verify <img> <lba> <path>
 *       校验无名数据是 "MAIN\n"，且两条命名流内容/大小正确
 *
 * 为什么单独写 C 探针：CLI 没有"写命名流"的命令，
 * 而 ADS 是 libntfs-3g 的公开能力；用它才能证明 copy 真的保留了附加数据。
 */
#include "img_ntfs_fs.h"
#include "img_qcow2.h"
#include "img_io.h"
#include "img_partition.h"
#include "img_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t part_bytes(img_qcow2 *q, uint64_t lba){
    img_parttable_t t;
    if(img_parttable_read(q,&t,0)!=IMG_OK) return 0;
    for(int i=0;i<IMG_MAX_PARTITIONS;i++)
        if(t.parts[i].used && t.parts[i].first_lba==lba)
            return (t.parts[i].last_lba - t.parts[i].first_lba + 1)*512ULL;
    return 0;
}

static const char *ZONE = "[ZoneTransfer]\r\nZoneId=3\r\n";
static const char *META = "hello-meta-stream";

static int do_write(img_ntfs_fs *fs){
    const char *main_data = "MAIN\n";
    int rc=img_ntfs_put_file_mem(fs, main_data, strlen(main_data), "/ADS.bin");
    if(rc!=IMG_OK){ printf("put ADS.bin rc=%d\n",rc); return 1; }
    rc=img_ntfs_set_xattr(fs,"/ADS.bin","Zone.Identifier",ZONE,strlen(ZONE));
    if(rc!=IMG_OK){ printf("set Zone.Identifier rc=%d\n",rc); return 1; }
    rc=img_ntfs_set_xattr(fs,"/ADS.bin","meta",META,strlen(META));
    if(rc!=IMG_OK){ printf("set meta rc=%d\n",rc); return 1; }
    printf("WROTE\n");
    return 0;
}

static int cmp_stream(img_ntfs_fs *fs, const char *path, const char *name,
                      const char *want, size_t want_len){
    void *buf=NULL; size_t len=0;
    int rc=img_ntfs_get_xattr(fs,path,name,&buf,&len);
    if(rc!=IMG_OK){ printf("MISSING %s (rc=%d)\n",name,rc); return 1; }
    int bad = (len!=want_len || memcmp(buf,want,len)!=0);
    free(buf);
    if(bad){ printf("BAD %s len=%zu want=%zu\n",name,len,want_len); return 1; }
    printf("OK %s len=%zu\n",name,len);
    return 0;
}

static int do_verify(img_ntfs_fs *fs, const char *path){
    void *buf=NULL; size_t len=0;
    int rc=img_ntfs_get_xattr(fs,path,"__no_such_stream__",&buf,&len);
    if(rc!=IMG_ENOENT){ printf("unexpected rc for missing stream: %d\n",rc); return 1; }
    printf("OK missing-stream\n");

    void *main=NULL; size_t mlen=0;
    rc=img_ntfs_read_file(fs,path,&main,&mlen);
    if(rc!=IMG_OK){ printf("read main rc=%d\n",rc); return 1; }
    if(mlen!=5 || memcmp(main,"MAIN\n",5)!=0){ free(main); printf("BAD main len=%zu\n",mlen); return 1; }
    free(main);
    printf("OK main len=%zu\n",(size_t)5);

    int bad=0;
    bad |= cmp_stream(fs,path,"Zone.Identifier",ZONE,strlen(ZONE));
    bad |= cmp_stream(fs,path,"meta",META,strlen(META));

    int cnt=img_ntfs_list_xattrs(fs,path,NULL,NULL);
    if(cnt!=IMG_EINVAL){ printf("list NULL cb should be EINVAL, got %d\n",cnt); return 1; }
    printf("OK list-guard\n");
    return bad?1:0;
}

int main(int argc,char **argv){
    if(argc<4){ fprintf(stderr,"usage: %s write|verify <img> <lba> [path]\n",argv[0]); return 2; }
    const char *mode=argv[1], *img=argv[2];
    uint64_t lba=strtoull(argv[3],NULL,0);
    const char *path=(argc>4)?argv[4]:"/ADS.bin";

    img_lang_set("en");
    img_io *io=img_io_open_existing(img,true);
    if(!io){ fprintf(stderr,"open image failed: %s\n",img); return 2; }
    img_qcow2_opts_t o={0};
    img_qcow2 *q=img_qcow2_open(io,&o);
    if(!q){ fprintf(stderr,"not qcow2\n"); return 2; }
    if(img_qcow2_write_init(q)!=IMG_OK){ fprintf(stderr,"write_init failed\n"); return 2; }
    uint64_t pb=part_bytes(q,lba);
    if(!pb){ fprintf(stderr,"partition not found at lba %llu\n",(unsigned long long)lba); return 2; }
    img_ntfs_fs *fs=img_ntfs_mount_rw(q,lba,pb);
    if(!fs){ fprintf(stderr,"mount ntfs failed\n"); return 2; }

    int rc = (!strcmp(mode,"write")) ? do_write(fs) : do_verify(fs,path);
    img_ntfs_umount(fs);
    img_qcow2_close(q);
    return rc;
}