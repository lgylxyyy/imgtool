/* img_path.h — 统一"容器路径"语法（2026-10-24 定稿）
 *
 * 目标：App 只传一个字符串，工具自己判断"这是手机文件 / qcow2 某分区里的文件 /
 * ISO 里的文件 / WIM 里的文件 / ISO 里的 WIM 里的文件"。
 *
 * 语法（! = 进入容器；宿主路径里真要 ! 就写 \!）：
 *   /storage/.../a.txt                         宿主文件
 *   host:/storage/.../a.txt                    宿主文件（显式）
 *   /storage/.../Win.qcow2!p3:/Users/a.txt     qcow2 第 3 个已用分区
 *   /storage/.../Win.qcow2!lba165888:/a.txt    显式 LBA（兼容老写法）
 *   /storage/.../Win11.iso!:/bootmgr           ISO 根目录
 *   /storage/.../Win11.iso!:/sources/install.esd!1:/Windows/System32/cmd.exe
 *   /storage/.../boot.wim!2:/Windows/...       WIM/ESD 映像 2
 *
 * 规则：
 *   · 容器类型按【魔数】判断，不靠扩展名；
 *   · 容器内分隔符统一 /，输入里的 \ 自动归一；
 *   · 禁止 .. 穿越；
 *   · ISO/WIM/ESD 是只读链（is_readonly=1），写操作由命令层拒绝；
 *   · ISO 里的 WIM 会先抽取到临时文件（temp_path），用完必须 img_path_cleanup()。
 */
#ifndef IMG_PATH_H
#define IMG_PATH_H
#include "img_types.h"
#include "img_xcopy.h"     /* img_xkind */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    img_xkind kind;          /* HOST / NTFS / FAT / ISO / WIM */
    char      host[2048];    /* 容器文件（WIM 在 ISO 里时是临时文件） */
    uint64_t  lba;           /* NTFS/FAT 分区起始扇区 */
    int       index;         /* WIM/ESD 映像索引（1 起） */
    char      inner[2048];   /* 容器内路径（/ 开头；HOST 时为空） */
    char      temp_path[2048]; /* 需要清理的临时文件（可为空） */
    bool      is_readonly;   /* 链上有 ISO/WIM/ESD */
    bool      is_new_container; /* ★ dst 模式：容器不存在，按扩展名新建（zip/7z/tar/tgz） */
} img_path;

/* 解析。失败返回 IMG_E*，why 里填写说明。
 *
 * ★ 2026-11-08（P1 统一 copy）：新增 dst 模式。
 *   dst=true 时，容器文件【不存在】不报错 —— 按扩展名判格式。
 *   这样 `copy --dst=/x/a.zip!:/f.txt` 能"新建包并写第一个文件"。
 *   （源侧仍然必须存在：不存在就是真的打不开。） */
int  img_path_parse(const char *spec, img_path *out, char *why, size_t whycap);
int  img_path_parse_ex(const char *spec, img_path *out, bool is_dst, char *why, size_t whycap);
static inline int img_path_parse_dst(const char *spec, img_path *out, char *why, size_t whycap){
    return img_path_parse_ex(spec, out, true, why, whycap);
}

/* 渲染成规范串（错误信息/日志用）。 */
int  img_path_format(const img_path *p, char *out, size_t cap);

/* 删除解析过程中产生的临时文件（可重复调用）。 */
void img_path_cleanup(img_path *p);

/* 只读链判断的便捷函数 */
static inline bool img_path_readonly(const img_path *p){ return p && p->is_readonly; }

#ifdef __cplusplus
}
#endif
#endif
