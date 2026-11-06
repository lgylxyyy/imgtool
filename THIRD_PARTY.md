# 第三方组件与版权声明 / Third-Party Notices

本文件列出 `imgtool` 使用或链接的第三方代码、对应许可证，以及本项目对其所做的修改。
许可证全文在 `LICENSES/`。**本项目自身**的许可口径见 `README.md` 末尾「许可」一节。

最后核对：2026-11-06（`imgcmd --list` 报告 features = wimlib / ntfs3g / zlib / mkntfs 全开）

---

## 一、会被链接进产物的组件

### 1. zlib

- 许可证：**Zlib**（`LICENSES/Zlib.txt`）
- 用途：qcow2 压缩簇的 deflate 解压/压缩
- 修改：无。使用系统 `libz`，`find_package(ZLIB)` 探测，缺失则 `IMG_WITH_ZLIB=0`。
- 版权：Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler

### 2. libntfs-3g

- 许可证：**LGPL-2.1-or-later**（`LICENSES/LGPL-2.1.txt`）
- 用途：NTFS 卷的读写（目录项、MFT、属性、压缩文件、稀疏文件）
- 链接方式：动态链接（Android 上静态编入 `libimgtool.so`，源码可自行重编）
- 修改：无。只通过公开头 `ntfs-3g` API 与 `ntfs_device` 抽象接入；
  本项目的 `img_ntfs_dev.c` 提供自定义 `ntfs_device`（在 qcow2 扇区上模拟块设备），
  该文件是本项目自有代码，不属于 ntfs-3g。
- 版权：Copyright (C) 2001-2024 Anton Altaparmakov, Szabolcs Szakacsits, Tuxera Inc.

### 3. ntfsprogs / mkntfs（可选，默认关闭）

- 许可证：**GPL-2.0-or-later**（`LICENSES/GPL-2.0.txt`）
- 用途：NTFS 格式化（`fs.format --fs=ntfs`）
- 开关：`-DIMG_WITH_NTFS=OFF`（默认 OFF）。**打开后整个产物须按 GPL-2.0-or-later 分发。**
- 构建方式：`tools/build-mkntfs-obj.sh` 把 `mkntfs` 的目标文件重建成一个可重定位对象，
  再由本项目链接（见该脚本头部注释）。
- 修改：为可重定位链接做了符号/入口调整，改动记录在该脚本与提交信息中。
- 版权：Copyright (C) 2002-2024 Anton Altaparmakov, Richard Russon, Szabolcs Szakacsits 等

### 4. wimlib

- 许可证：**LGPL-2.1-or-later**
- 用途：WIM/ESD 解析与解压（`wim.ls` / `wim.apply` / `wim.extract`）
- 链接方式：动态链接；缺失则 `wim.*` 全部降级为 `FEATURE-UNAVAILABLE`
- 修改：无
- 版权：Copyright (C) 2012-2024 Eric Biggers

### 5. libarchive（可选）

- 许可证：**BSD-2-Clause / BSD-3-Clause**（`LICENSES/BSD-3-Clause.txt`）
- 用途：zip / 7z / tar 的只读浏览与解压
- 修改：无
- 版权：Copyright (C) 2003-2024 Tim Kientzle 等

### 6. hivex（可选）

- 许可证：**LGPL-2.1-or-later**
- 用途：读写 Windows 注册表 hive —— BCD 就是一个 hive
- 修改：无
- 版权：Copyright (C) 2009-2024 Richard W.M. Jones（Red Hat Inc.）

### 7. UnRAR（源码内置于 `third_party/unrar/`）

- 许可证：**UnRAR 自由软件许可**（`LICENSES/UnRAR.txt`，原文随源码在 `third_party/unrar/license.txt`）
- 用途：只读打开 RAR（含 RAR5 / 固实包 / 中文名）
- 限制：**不得用其源码开发 RAR 兼容压缩器，也不得重建 RAR 压缩算法**。
  本项目只做解压，不做压缩。
- 修改：仅裁剪（去掉命令行/UI 部分），解码逻辑未改。
- 版权：Copyright (C) 1993-2024 Alexander Roshal

---

## 二、只在开发/验证期使用（不进产物）

| 组件 | 许可证 | 用途 |
|---|---|---|
| CMake | BSD-3-Clause | 构建 |
| GCC / clang | GPL-3.0+（含运行时例外） | 编译 |
| QEMU（`qemu-img check`） | GPL-2.0 | qcow2 结构独立校验 |
| gptfdisk（`sgdisk -v`） | GPL-2.0 | GPT 独立校验 |
| util-linux（`fdisk`） | GPL-2.0+ | MBR 独立校验 |
| mtools（`mdir`/`mcopy`） | GPL-2.0+ | FAT32 独立校验 |
| ntfs-3g 工具（`ntfsls`/`ntfscp`） | GPL-2.0+ / LGPL-2.1+ | NTFS 独立校验 |
| 7-Zip CLI | LGPL-2.1 | 归档交叉验证 |
| Python 3 | PSF-2.0 | 测试脚本 |

## 三、规范与参考（未链接代码）

| 来源 | 用途 |
|---|---|
| QEMU `docs/interop/qcow2.txt` | qcow2 格式权威定义 |
| Microsoft FAT 规范（fatgen103） | FAT32 BPB / FSInfo / FAT 大小推算 |
| UEFI Specification | GPT 结构、分区属性位 |
| Microsoft Learn（分区/BCD 文档） | BIOS/MBR 与 UEFI/GPT 的分区要求、恢复分区 GUID |

## 四、修改与再分发提示

1. 本仓库对外分发时，`LICENSES/` 必须随附。
2. 若你打开了 `-DIMG_WITH_NTFS=ON`，产物为 GPL-2.0-or-later，须同时提供源码。
3. 若你修改了 `third_party/unrar/` 下的任何文件，必须保留原许可声明并注明改动。
4. `reference/` 里的 `bcd-reference-*.bin`、`mbr-code-*.bin` 是**数据**（微软引导扇区/
   BCD 二进制片段），不适用上述开源许可证；再分发前请自行确认合规。
