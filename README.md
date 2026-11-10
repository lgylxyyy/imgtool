<div align="center">

# imgtool

**离线读写 qcow2 映像内的分区、文件系统与 Windows 引导，不挂载、不开机。**

[English](README_EN.md) | [**中文**](README.md)

[![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-informational)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android%20%7C%20arm64-informational)]()
[![Commands](https://img.shields.io/badge/commands-77-informational)]()

</div>

`imgtool` 是 C11 库 + 命令行工具（`imgcmd`）。把 `.qcow2` / `.img` / `.vhd` / `.vmdk`
当作裸磁盘直接操作：分区表、FAT32 / NTFS、WIM/ESD、ISO、压缩包、Windows 引导
（MBR/VBR 码、BCD、bootmgr、bootmgfw.efi、WinRE）、注册表 hive。

> 相关仓库：[Icdqcow2](https://github.com/lgylxyyy/Icdqcow2)，调用本库的 Android App。

---

## 快速开始

```bash
# 构建（可选依赖缺失时对应命令降级，不会编译失败）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 自检
build/tools/imgcmd --list | head -c 200
# {"version":1,"features":{"wimlib":true,"ntfs3g":true,"zlib":true,"mkntfs":true},...}
```

| 页面 | 内容 |
|---|---|
| [构建](#构建) | 依赖、宿主构建、Android 交叉编译 |
| [能力](#能力) | 支持的格式与操作 |
| [命令行](#命令行) | 常用命令与返回结构 |
| [许可证](#许可证) | 本项目与全部第三方组件 |

---

## 构建

### 本机路径（换机器只需填一次）

源码和脚本里不写死任何本机路径。解析顺序是
**环境变量 > `imgtool.conf` > 自动探测**；三者都不成立时报错并指出缺什么，
不会静默指向一个不存在的目录。

```bash
./configure-local.sh            # 探测并写入 imgtool.conf（不提交）
./configure-local.sh --ask      # 逐项确认，回车沿用探测值
./configure-local.sh --show     # 只看结果，不写文件
./configure-local.sh --presets  # 另外生成 CMakeUserPresets.json
```

`imgtool.conf` 与 `CMakeUserPresets.json` 都在 `.gitignore` 里。
要手工填就照 `imgtool.conf.example` 复制一份，只有这几项：
NDK、Android SDK、构建根、依赖缓存、测试素材目录。

### 宿主

依赖：CMake ≥ 3.16、C11 编译器、`zlib`。以下可选：

| 可选依赖 | 启用的命令 | 关闭方式 |
|---|---|---|
| `libntfs-3g` | 全部 NTFS 读写 | `-DIMG_WITH_NTFS3G=OFF` |
| `ntfsprogs`（mkntfs） | `fs.format --fs=ntfs` | 默认 OFF；`-DIMG_WITH_NTFS=ON` 且先跑 `tools/build-mkntfs-obj.sh` |
| `wimlib` | `wim.*` | `-DIMG_WITH_WIM=OFF` |
| `libarchive` | `archive.*`（zip/7z/tar） | `-DIMG_WITH_ARCHIVE=OFF` |
| `libhivex` | BCD / 注册表 hive | 自动检测 |
| 内置 unrar | `archive.*` 读 RAR | `-DIMG_WITH_RAR=OFF` |

产物：`build/tools/imgcmd`（主命令行）、`build/libimgtool.a`、`build/libimgtool.so`。

> 宿主上构建 `.so` 时，若系统的静态 `libwim.a` 没有用 `-fPIC` 编译
> （Ubuntu amd64 就是这样），`.so` 会自动改用动态 `libwim.so`，
> 代价是 `.so` 里的 ISO 直灌关闭（`imgcmd` 不受影响，仍静态链接）。
> 要强制指定：`-DIMG_WIM_STATIC_PIC=AUTO|ON|OFF`。

### Android（NDK 交叉编译）

路径由 `./configure-local.sh` 一次性填好（见上一节），也可以临时用环境变量：

```bash
./configure-local.sh --ask         # 填 NDK / SDK 路径，写入 imgtool.conf
# 或：export NDK=/path/to/android-ndk-r28c

./android-deps.sh                  # 一次性：交叉编译 ntfs-3g/wimlib/zlib/libarchive/unrar

./android-cross.sh                 # 默认 arm64-v8a armeabi-v7a x86_64
./android-cross.sh arm64-v8a       # 只编一个 ABI
TOOLS=1 TESTS=1 ./android-cross.sh # 连 imgcmd 与单元测试一起编（真机排查用）
NTFS=OFF ./android-cross.sh        # 不含 GPL 的 mkntfs
```

产物：`build/android/<abi>/libimgtool.so`（`API=21` 起；调用方 App `minSdk=33`）。

Android 与宿主的行为差异（同一份代码内 `#ifdef __ANDROID__` 共 4 处）：

- 长任务只能通过**进度回调 + 取消标志**中断（无信号、无多进程）；
- `content://` 路径由 App 先落缓存再传绝对路径；
- `/sdcard` 走 FUSE，无常规 Unix 权限语义；
- 写盘前需由调用方检查剩余空间（`statvfs`）。

详见 [`ANDROID-BUILD.md`](ANDROID-BUILD.md)、[`ARMHF-BUILD.md`](ARMHF-BUILD.md)。

### 分支

| 分支 | 用途 |
|---|---|
| `main` | 主线开发，宿主与 Android 双目标均可构建 |
| `android` | 移动端发布分支：产物 `libimgtool.so` 供 [Icdqcow2](https://github.com/lgylxyyy/Icdqcow2) 使用。定期合并 `main`，确保与主线一致 |

构建 App 所需的库时，检出 `android` 分支后执行 `./android-cross.sh` 即可。

---

## 能力

| 领域 | 能力 |
|---|---|
| **映像格式** | qcow2 建/扩/压/克隆（压缩簇支持 zlib 与 **zstd**）；raw、VHD（动态盘）、VMDK（sparse）读写；按魔数自动识别（不支持项见下表） |
| **分区表** | MBR / GPT 读写、按角色自动布局（ESP + MSR + 系统 + 恢复）、增删改、属性位、导出/导入整表 |
| **FAT32** | 列目录、读写文件、建目录、改名、删除、卷标、时间戳、8.3 短名 |
| **NTFS** | 列目录、读写文件、递归复制/移动、删除、`du`、卷标、格式化（mkntfs） |
| **WIM / ESD** | 列映像、估算展开体积、写入分区、提取单文件、**捕获**（qcow2 生成新 WIM） |
| **ISO** | 浏览 ISO9660 / UDF、提取内部文件（`install.wim` 可直接从 ISO 读取） |
| **压缩包** | zip / 7z / tar / tgz 读写；RAR / CAB 只读；统一 `copy` 命令覆盖解压与打包 |
| **引导** | 写 MBR/VBR 引导码、铺 `bootmgr` / `bootmgfw.efi` / `boot.sdi` / 字体、生成 BCD（BIOS/UEFI 双体系）、32 位 UEFI 位数判定、引导自检 |
| **恢复环境** | `winre.wim` 装入恢复分区或系统分区、写 `ReAgent.xml` 与恢复脚本 |
| **注册表** | 离线读写 5 个标准 hive（SYSTEM / SOFTWARE / SAM / SECURITY / DEFAULT），事务性写入 |

**格式支持矩阵**

| 格式 | 读 | 写 | 建盘 |
|---|---|---|---|
| qcow2 | ✅ | ✅ | ✅ |
| raw（.img/.raw/.dd） | ✅ | ✅ | ✅ |
| VHD（vpc 动态盘） | ✅ | ✅ | ✅ |
| VMDK（sparse） | ✅ | ✅ | ✅ |

> 使用 `img.create --format=vhd` 创建动态 VHD；MBR/GPT 分区表均受支持。

**已知不支持**（遇到时在命令层如实报错，不静默处理、不写出坏数据）

> zstd 压缩簇已支持（2026-11-10），但需要构建时找得到 `libzstd`：缺库的构建会拒绝打开
> 这类映像，并在 `--list` 的 `features.zstd` 里如实报 `false`。

| 项 | 现状 | 碰到会怎样 |
|---|---|---|
| qcow2 **加密** | 头部带 `INCOMPAT_ENCRYPTED` 即拒绝 | 打不开该映像 |
| **VHDX** | 只支持 VHD 动态盘（vpc），没有 VHDX 解析器 | `img.create --format=vhdx` 报参数错 |
| NTFS **硬链接** | 上游 `libntfs-3g` 的 `ntfs_link` 本身仍是 FIXME，建不了 | 没有对应命令；移动/复制不会改变链接数 |

**设计边界**

- 不启动虚拟机、不联网、不做在线挂载；
- 映像必须处于**关机**状态：本工具直接修改文件系统元数据，并发写入必然损坏映像；
- Android 上不做 `fork/exec`，全部为进程内调用；
- 不处理硬链接语义：NTFS 侧建不了硬链接（上游能力缺口），移动目录后链接数不变。

---

## 命令行

```bash
imgcmd <命令> [--参数=值 ...]
imgcmd --list                     # 导出命令表（JSON）
imgcmd --help=<命令>              # 单条命令用法
imgcmd sys.tmpdir                 # 查询当前生效的临时目录
```

`sys.tmpdir` 返回实际使用的临时目录、命中的优先级档位（`env` / `tmpdir` /
`fallback`）、该卷剩余空间，以及本次清扫掉的残留数。
`wim.capture` 等需要临时空间的任务，应先按此路径做容量检查。

- 参数一律字符串形式：`--img=/sdcard/a.qcow2`、`--recursive=true`
- 结果走 stdout（一行 JSON），日志走 stderr
- **成功**：`{"ok":true,"cmd":"img.info","version":1,"duration_ms":12,"data":{...}}`
- **失败**：`{"ok":false,...,"err":{"rc":-4,"error_code":"IMG-OPEN-FAIL","at":"img.info","hint":"<说明>","suggest":"<处置建议>"}}`

`error_code` 是稳定契约；`hint` / `suggest` 为供人阅读的说明文本，**不应解析**。

### 统一路径语法

```
/path/a.qcow2!p3:/Users/a.txt     qcow2 第 3 个已用分区
/path/a.qcow2!lba165888:/a.txt    显式 LBA
/path/a.iso!:/sources             ISO 根目录
/path/a.esd!1:/Windows            WIM/ESD 映像 1
/path/a.zip!:/dir1                压缩包内目录
host:/sdcard/x                    宿主路径（绝对路径亦可）
```

容器类型按**魔数**判定，不依据扩展名。

### 典型流程

```bash
# 1) 建盘（GPT + ESP + MSR + Windows）
imgcmd img.create --out=/sdcard/win11.qcow2 --size-gb=64 --table=gpt

# 2) 写入映像
imgcmd wim.apply --img=/sdcard/win11.qcow2 --lba=<SYS> \
    --wim=/sdcard/win11.iso --index=1 --format=on

# 3) 铺引导（UEFI 一条命令：bootmgfw.efi + bootx64.efi + 字体 + boot.sdi + 内置 BCD）
imgcmd boot.populate --img=/sdcard/win11.qcow2 --mode=uefi --sys-lba=<SYS> --esp-lba=<ESP>

# 4) 自检
imgcmd boot.check --img=/sdcard/win11.qcow2 --mode=uefi
```

完整命令表见 `imgcmd --list`（77 条）。

---

## 在 C 中调用

```c
#include "img_cmd.h"

char err[256];
img_ctx *ctx = img_ctx_open("/sdcard/a.qcow2", true, false, err, sizeof err);
if (!ctx) { /* 无法打开，err 为说明文本 */ }

img_args *a = img_args_new();
img_args_set(a, "lba", "2048");
img_result *r = img_result_new();
img_cmd_exec("part.scan", ctx, a, r);
/* r->rc == IMG_OK 时读 r 的字段 */

img_result_free(r); img_args_free(a); img_ctx_close(ctx);
```

`img_cmd_exec()` 与 CLI 走同一条命令表，参数规格、校验、错误码完全一致。

---

## 返回码

`img_result.rc` 与进程退出码同源（定义于 `include/img_types.h`）：

| rc | 名称 | 含义 |
|---|---|---|
| 0 | `IMG_OK` | 成功 |
| -1 | `IMG_EINVAL` | 参数非法 |
| -2 | `IMG_ENOMEM` | 内存不足 |
| -3 | `IMG_EIO` | 底层读写失败 |
| -4 | `IMG_ENOENT` | 不存在 |
| -5 | `IMG_ENOTSUP` | 暂不支持（如 zstd / 加密） |
| -6 | `IMG_EFORMAT` | 格式不合法（魔数 / 结构错） |
| -7 | `IMG_ECORRUPT` | 数据损坏（校验不过） |
| -8 | `IMG_EAGAIN` | 需要重试 / 资源忙 |
| -9 | `IMG_ECRC` | 校验和 / CRC 不符 |
| -10 | `IMG_EFULL` | 空间不足 |
| -11 | `IMG_EEXISTS` | 已存在 |
| -12 | `IMG_ESTATE` | 状态机不允许该操作 |
| -13 | `IMG_ECANCELLED` | 已取消 |
| -14 | `IMG_ETRUNC` | 数据被截断 |
| -15 | `IMG_ERANGE` | 请求范围越界（偏移 / 长度超出映像） |
| -16 | `IMG_ETIMEDOUT` | I/O 看门狗超时 |
| -17 | `IMG_ECYCLE` | 会造环（目录搬进自己的子树） |
| -18 | `IMG_ESAMEFILE` | 源与目标为同一文件 |
| -19 | `IMG_ENAMETOOLONG` | 文件名超过文件系统上限 |
| -20 | `IMG_EPARTIAL` | 部分完成（复制成功但删源失败） |
| -21 | `IMG_EREFUSED` | 策略拒绝（如未允许删宿主文件） |
| -22 | `IMG_EREADONLY` | 目标是只读容器（ISO / WIM / ESD） |
| -23 | `IMG_ENAMEINVALID` | 名称含 Windows 不接受的字符 / 保留名 / 尾点 |
| -24 | `IMG_ESTALE` | 句柄 / 索引过期（压缩包已变更） |

---

## 目录

```
include/            公开头（img_*.h）
src/                实现：qcow2 / raw / VHD / VMDK / 分区表 / FAT32 / NTFS /
                    WIM / ISO / 归档 / BCD / 引导 / 恢复 / 注册表
tools/              imgcmd、imgbc、imglayout、imgdump …（含 build-mkntfs-obj.sh）
tests/              shell + python + C 回归（缺外部素材时自动跳过，不判失败）
ci/Dockerfile       完整回归用的镜像（ntfs-3g / wimlib / hivex / qemu-img / mtools）
.github/workflows   CI：在自建镜像里跑全量回归
reference/          引导参考件（仅存档，二进制不入库）
legacy/             早期实现，留档、不参与构建
third_party/unrar/  内置 RAR 解码器
LICENSES/           许可证全文
cmake/              Android NDK toolchain（android-arm64host.cmake）
scripts/env.sh      本机路径解析（环境变量 > imgtool.conf > 自动探测）
configure-local.sh  探测本机路径并写入 imgtool.conf
imgtool.conf.example  本机配置模板（imgtool.conf 本身不入库）
android-cross.sh    Android 交叉编译入口
android-deps.sh     Android 第三方依赖编译（一次性）
```

文档：

- [`DESIGN-OUTPUT.md`](DESIGN-OUTPUT.md)：输出、日志、错误与尺寸校验契约（工具 ↔ App）
- [`ANDROID-BUILD.md`](ANDROID-BUILD.md)：Android 交叉构建与产物体检
- [`ARMHF-BUILD.md`](ARMHF-BUILD.md)：armhf 32 位交叉构建与验证
- [`PARTITION-POLICY.md`](PARTITION-POLICY.md)：分区、格式化、克隆的政策决策
- [`SPEC-NTFS-MOVE.md`](SPEC-NTFS-MOVE.md)：NTFS 改名与移动的规范对照
- [`THIRD_PARTY.md`](THIRD_PARTY.md)：第三方组件与版权声明
- [`SECURITY.md`](SECURITY.md)：静态扫描怎么配的、哪些告警修了、哪些屏蔽了以及理由

---

## 工程约束

- **映像必须关机**：并发写入必然损坏元数据。
- **不静默降级**：可选依赖缺失时，对应命令返回 `FEATURE-UNAVAILABLE`，而非静默成功。
- **写前校验**：NTFS/FAT 写入前校验名称合法性；分区操作前校验空间；hive 写入前备份。
- **事务性写入**：注册表、BCD、归档重写均走「临时文件 → 校验 → rename」。
- **无网络**：代码中不含任何网络调用。

---

## 许可证

本项目默认构建（含 `mkntfs`）为 **GPL-2.0-or-later**；
`-DIMG_WITH_NTFS=OFF` 构建仅含本项目自有代码 + 下列 LGPL / BSD / Zlib 组件。

### 本项目

| 范围 | 许可证 |
|---|---|
| 全部自有代码 | **GPL-2.0-or-later**（`-DIMG_WITH_NTFS=OFF` 时为 LGPL-2.1-or-later 兼容集合） |

### 链接进产物的第三方组件

| 组件 | 许可证 | 用途 | 全文 |
|---|---|---|---|
| [zlib](https://zlib.net) | Zlib | qcow2 压缩簇 | [Zlib.txt](LICENSES/Zlib.txt) |
| [libntfs-3g](https://github.com/tuxera/ntfs-3g) | LGPL-2.1-or-later | NTFS 读写 | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [ntfsprogs / mkntfs](https://github.com/tuxera/ntfs-3g/tree/master/ntfsprogs) | **GPL-2.0-or-later** | NTFS 格式化（`-DIMG_WITH_NTFS=OFF` 可去除） | [GPL-2.0.txt](LICENSES/GPL-2.0.txt) |
| [wimlib](https://wimlib.net) | LGPL-2.1-or-later | WIM/ESD | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [libarchive](https://libarchive.org) | BSD-2-Clause / BSD-3-Clause | zip / 7z / tar | [BSD-3-Clause.txt](LICENSES/BSD-3-Clause.txt) |
| [hivex](https://github.com/libguestfs/hivex) | LGPL-2.1-or-later | 注册表 hive（BCD） | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [UnRAR](https://www.rarlab.com/rar_add.htm) | UnRAR 自由软件许可 | 只读解 RAR | [UnRAR.txt](LICENSES/UnRAR.txt) |
| [libxml2](https://gitlab.gnome.org/GNOME/libxml2) | MIT | hivex 依赖 | [MIT.txt](LICENSES/MIT.txt) |
| [libiconv](https://www.gnu.org/software/libiconv/) | LGPL-2.1-or-later | 字符集转换 | [LGPL-2.1.txt](LICENSES/LGPL-2.1.txt) |
| [xz / liblzma](https://tukaani.org/xz/) | 0BSD（及个别 PD 文件） | libarchive 依赖 | [xz 上游声明](https://git.tukaani.org/?p=xz.git;a=blob_plain;f=COPYING;hb=HEAD) |
| Android NDK（sysroot） | Apache-2.0 + 各组件自带 | 交叉编译目标环境 | [Apache-2.0.txt](LICENSES/Apache-2.0.txt) |

### 仅在开发/验证期使用（不进产物）

| 组件 | 许可证 | 用途 |
|---|---|---|
| CMake | BSD-3-Clause | 构建 |
| GCC / clang | GPL-3.0+（含运行时例外） | 编译 |
| QEMU（`qemu-img check`） | GPL-2.0 | 映像结构与格式独立校验 |
| gptfdisk（`sgdisk -v`） | GPL-2.0 | GPT 独立校验 |
| util-linux（`fdisk`） | GPL-2.0+ | MBR 独立校验 |
| mtools（`mdir`/`mcopy`） | GPL-2.0+ | FAT32 独立校验 |
| ntfs-3g 工具（`ntfsls`/`ntfscp`） | GPL-2.0+ / LGPL-2.1+ | NTFS 独立校验 |
| 7-Zip CLI | LGPL-2.1 | 归档交叉验证 |
| Python 3 | PSF-2.0 | 测试脚本 |

### 合规说明

- **LGPL 义务**：`libntfs-3g` / `wimlib` / `hivex` / `libiconv` 静态编入 `libimgtool.so`。
  为满足「可重新链接」要求，本仓库提供完整源码、构建脚本与依赖版本，
  任何人可据此重新生成 `.so`。
- **GPL 义务**：默认构建含 `mkntfs`（GPL-2.0-or-later），整体按 GPL-2.0-or-later 分发。
  需要非 GPL 分发时使用 `-DIMG_WITH_NTFS=OFF`。
- **UnRAR 限制**：仅用于解压，不得用于开发 RAR 兼容压缩器或重建 RAR 压缩算法。
- 逐项修改说明与版权归属见 [`THIRD_PARTY.md`](THIRD_PARTY.md)。
