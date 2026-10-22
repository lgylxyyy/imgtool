/* img_types.h — 公共类型 / 错误码 / 进度结构（全库共用） */
#ifndef IMG_TYPES_H
#define IMG_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 版本 ---------- */
#define IMG_VERSION "0.1.0"

/* ---------- 线程局部存储 ----------
 * 命令表里几个"拼 JSON 的大缓冲"必须用它。为什么不是别的方案：
 *   · 进程级 static → 无 ctx 的命令（iso.ls / wim.ls / wim.identify / cmd.list）是允许
 *     并行的，两个线程同时往同一块缓冲里写 → 另一边的输出被咬（race_test 场景③ 取证）。
 *   · per-call malloc → img_result_raw() 只存指针不拷贝（大列表复制代价高），
 *     缓冲必须活到调用方渲染完，命令函数里 free 等于留一个悬垂指针。
 *   · 给无 ctx 命令上全局锁 → 把库的并发能力压成排队，实测直接把并行压测拖到超时。
 * MSVC 用 __declspec(thread)，其余走 C11 _Thread_local（GCC/clang/Bionic 都支持）。*/
#if defined(_MSC_VER)
#  define IMG_THREAD_LOCAL __declspec(thread)
#else
#  define IMG_THREAD_LOCAL _Thread_local
#endif

/* ---------- 错误码（负值，<=0 表示失败） ---------- */
typedef enum {
    IMG_OK            =  0,
    IMG_EINVAL        = -1,   /* 参数非法 */
    IMG_ENOMEM        = -2,   /* 内存不足 */
    IMG_EIO           = -3,   /* 底层读写失败 */
    IMG_ENOENT        = -4,   /* 不存在 */
    IMG_ENOTSUP       = -5,   /* 暂不支持（如 zstd/加密） */
    IMG_EFORMAT       = -6,   /* 格式不合法（魔数/结构错） */
    IMG_ECORRUPT      = -7,   /* 数据损坏（校验不过） */
    IMG_EAGAIN        = -8,   /* 需要重试/资源忙 */
    IMG_ECRC          = -9,   /* 校验和/CRC 不符 */
    IMG_EFULL         = -10,  /* 空间不足 */
    IMG_EEXISTS       = -11,  /* 已存在 */
    IMG_ESTATE        = -12,  /* 状态机不允许该操作 */
    IMG_ECANCELLED    = -13,  /* 用户取消 */
    IMG_ETRUNC        = -14,  /* 数据被截断 */
    IMG_ERANGE        = -15,  /* 请求范围越界（偏移/长度超出镜像，含加法溢出） */
    IMG_ETIMEDOUT     = -16,  /* I/O 超时：N 秒内没有任何读写完成（看门狗） */
} img_err_t;

const char *img_strerror(int err);

typedef struct {
    bool     ok;
    int      code;              /* img_err_t */
    char     reason[256];       /* 【中文备注】开发期/CLI 用；App 请用 code 做 i18n 查表，不要直接显示本字段 */
    char     suggestion[256];   /* 【中文备注】同上：App 按 code 查自己的语言表 */
} img_check_result_t;


/* ---------- 分区表类型 ---------- */
typedef enum {
    IMG_TABLE_NONE = 0,
    IMG_TABLE_MBR  = 1,       /* Legacy BIOS */
    IMG_TABLE_GPT  = 2,       /* UEFI */
} img_table_t;

/* ---------- 文件系统类型 ---------- */
typedef enum {
    IMG_FS_UNKNOWN = 0,
    IMG_FS_NTFS    = 0x07,    /* MBR type 0x07 */
    IMG_FS_FAT32   = 0x0b,
    IMG_FS_EXFAT   = 0x07,    /* Windows 把 exFAT 也标 0x07（需内容判定） */
    IMG_FS_FAT32LBA= 0x0c,
    IMG_FS_EXT4    = 0x83,
    IMG_FS_ESP     = 0xef,    /* EFI System Partition */
    IMG_FS_MSR     = 0x02,    /* Microsoft Reserved */
    IMG_FS_RECOVERY= 0x27,    /* ★ 官方要求恢复分区用 0x27 */
} img_fs_t;

/* ---------- 进度事件结构（前端直接用它画进度条） ---------- */
typedef enum {
    IMG_PH_NONE       = 0,
    IMG_PH_OPEN       = 1,    /* 打开镜像 */
    IMG_PH_CREATE     = 2,    /* 创建镜像 */
    IMG_PH_EXPAND     = 3,    /* 扩盘 */
    IMG_PH_PARTITION  = 4,    /* 分区表操作 */
    IMG_PH_FORMAT     = 5,    /* 格式化 */
    IMG_PH_DEPLOY     = 6,    /* 部署 Windows */
    IMG_PH_BROWSE     = 7,    /* 浏览/读取 */
    IMG_PH_CHECK      = 8,    /* 引导自检 */
    IMG_PH_FIX        = 9,    /* 引导修复 */
    IMG_PH_BCD        = 10,   /* BCD 操作 */
    IMG_PH_EXTRACT    = 11,   /* 解压 WIM/ISO */
    IMG_PH_COPY       = 12,   /* 拷贝文件 */
} img_phase_t;

typedef enum {
    IMG_OP_NONE       = 0,
    IMG_OP_READ       = 1,
    IMG_OP_ALLOC      = 2,    /* 分配簇 */
    IMG_OP_WRITE      = 3,
    IMG_OP_SYNC_INDEX = 4,    /* 同步 L1/L2/refcount 索引 */
    IMG_OP_WRITE_FILE = 5,    /* 写某个文件 */
    IMG_OP_WRITE_INDEX= 6,    /* 写目录索引 */
    IMG_OP_SCAN       = 7,    /* 扫描/列举 */
    IMG_OP_VERIFY     = 8,    /* 校验 */
    IMG_OP_COMPRESS   = 9,    /* 压缩 */
    IMG_OP_DECOMPRESS = 10,   /* 解压 */
} img_op_t;

typedef struct {
    img_phase_t phase;          /* 大阶段 */
    img_op_t    op;             /* 具体操作 */
    uint64_t    cur;            /* 当前第几个条目 */
    uint64_t    total;          /* 条目总数（0=未知） */
    uint64_t    cur_bytes;      /* 已处理字节 */
    uint64_t    total_bytes;    /* 总字节（0=未知） */
    const char *item;           /* ★ 当前处理对象（如文件路径） */
    const char *msg;            /* 【中文备注】开发期日志/CLI 用；App 渲染进度请用 phase/op 枚举 + cur/total/pct/item（语言中立）*/
    int         pct;            /* 0~100，可为 -1（不明确） */
    int         eta_s;          /* 预计剩余秒，-1=未知 */
    int         elapsed_s;      /* 已用秒 */
} img_progress_t;

/* 进度回调：>=200ms 触发一次；phase/op 变化立即触发；结束必发一次 100% */
typedef void (*img_progress_cb)(void *ud, const img_progress_t *p);

/* 进度阶段名（给 JSON 用） */
const char *img_phase_name(img_phase_t ph);
const char *img_op_name(img_op_t op);

#ifdef __cplusplus
}
#endif
/* 可写临时目录：IMGTOOL_TMPDIR → TMPDIR → /tmp → "."，取第一个可写的。
 * ★ 安卓没有 /tmp，库内任何地方都不许再写死 "/tmp"。返回 NULL 表示没有可写目录，
 *   调用方必须报错，不能继续往 /tmp 上撞。 */
const char *img_tmpdir(void);

/* ntfs-3g / wimlib 需要一个"已存在的普通文件"当设备名占位（它们会 realpath 它，
   wimlib 还要 stat 判类型）。★ 绝不能写死相对路径：App 进程的工作目录是 "/"，
   根本创建不了文件（真机上 wim.apply 就是这么废掉的）。返回 NULL = 没有可写目录。 */
const char *img_dev_placeholder(void);

#endif
