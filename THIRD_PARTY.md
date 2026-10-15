# 第三方开源组件清单 / Third-Party Notices

> 本文件列出 Icdqcow2（工具库 + Android 应用）用到的所有开源项目。
> 按「运行时依赖 / 构建与测试 / 参考实现 / 前端素材」分类。
> 最后更新：阶段 2b（NTFS 集成规划）

---

## 一、运行时依赖（会打进产物）

### 工具库 `imgtool`

| 组件 | 许可证 | 用途 | 链接 |
|---|---|---|---|
| **zlib** | zlib License | qcow2 压缩簇解码（inflate raw） | https://zlib.net / https://github.com/madler/zlib |
| **libntfs-3g** | **LGPL-2.1+** | NTFS 读写核心（`ntfs_device` 抽象可插拔） | https://github.com/tuxera/ntfs-3g |
| **ntfsprogs (mkntfs)** | **GPL-2.0-or-later** | NTFS 格式化器（作为**可选模块** `-DIMG_WITH_NTFS=ON/OFF`） | https://github.com/tuxera/ntfs-3g/tree/master/ntfsprogs |
| **libregf**（规划中） | LGPL-3.0 | Windows BCD（registry hive）读写 | https://github.com/libyal/libregf |
| **exfatprogs**（规划中） | GPL-2.0 | exFAT 格式化（若自研失败的备选） | https://github.com/exfatprogs/exfatprogs |
| **libhivex** | **LGPL-2.1+** | BCD（注册表 hive）读写；已处理校验/cell分配/次序号自增 | https://github.com/libguestfs/hivex |

> ⚠️ **许可证提示**：`mkntfs` 为 GPL-2.0+，启用该模块时 `imgtool` 整体须以 GPL-2.0-or-later 发布。
> 已设计为**可选编译开关**，关闭后其余部分不带 GPL 代码。

### Android 应用（规划）

| 组件 | 许可证 | 用途 | 链接 |
|---|---|---|---|
| **miuix** | Apache-2.0 | UI 设计风格参考 / 组件库 | https://github.com/miuix-tech/miuix |
| **NP 管理器**（参考） | 开源 | 双窗口 / 分栏 / 跨窗拖拽交互参考 | 搜索 "NP Manager" |
| **Microsoft Fluent UI System Icons** | MIT | 应用与文件图标 | https://github.com/microsoft/fluentui-system-icons |
| **HaydenReeve/WindowsIcons** | 参见其仓库 | Windows 风格文件图标 | https://github.com/HaydenReeve/WindowsIcons |
| **MicaUI/WindowsIconCustomization** | 参见其仓库 | Win11 图标风格 | https://github.com/MicaUI/WindowsIconCustomization |
| **jinghu-moon/win-icon-extractor** | 参见其仓库 | 提取 `.exe` 内嵌图标 | https://github.com/jinghu-moon/win-icon-extractor |

---

## 二、构建与测试工具（开发期，不进产物）

| 组件 | 许可证 | 用途 |
|---|---|---|
| **CMake** | BSD-3-Clause | 跨平台构建（PC + Android NDK 共用一份配置） |
| **GCC / clang** | GPL-3.0+（运行时例外） | C11 编译，`-Wall -Wextra -Wpedantic` 零警告 |
| **QEMU** | GPL-2.0 | `qemu-img check/convert` 校验 + 实机启动验证 |
| **gptfdisk (sgdisk)** | GPL-2.0 | GPT 独立第三方校验（`--verify` `--cross-check`） |
| **util-linux (fdisk)** | GPL-2.0+ | MBR 独立校验 |
| **mtools** | GPL-2.0+ | FAT32 独立读写验证（`mdir`/`mcopy`/`minfo`） |
| **ntfs-3g (ntfsinfo/ntfsls/ntfscp)** | GPL-2.0+ / LGPL | NTFS 独立校验与读写验证 |
| **wimlib** | GPL-3.0 | WIM/ESD 解析验证、镜像信息读取 |
| **p7zip / 7z** | LGPL-2.1 | ISO/Udf 解包 |
| **libarchive (bsdtar)** | BSD-3-Clause | ISO 读取 |
| **PyYAML** | MIT | SKILL.md 与配置校验 |
| **git** | GPL-2.0 | 版本管理 |
| **GitHub CLI (`gh`)** | MIT | 只读检索 GitHub（配合 Skill 使用） |
| **jq** | MIT | JSON 处理 |

---

## 三、参考实现 / 规范来源（不链接，仅参考思想）

| 项目 | 许可证 | 我们从中学到什么 |
|---|---|---|
| **`qcow2-viewer`**（本工作区） | 自有 | 经 sha256 交叉验证的 qcow2/NTFS/GPT 读取实现，`imgtool` 的 C 代码由其翻译 |
| **DiscUtils** | MIT | C# 的完整 NTFS 格式化逻辑（`NtfsFormatter.cs` 18KB），仅作**结构参考** |
| **QEMU `docs/interop/qcow2.txt`** | Apache-2.0 | qcow2 格式权威规范 |
| **Microsoft Learn** | MIT（文档） | 分区要求（BIOS/MBR、UEFI/GPT、恢复分区 GUID） |
| **Microsoft FAT 规范 (fatgen103)** | 公开文档 | FAT32 BPB / FSInfo / 迭代算 FAT 大小 |
| **UEFI Specification** | Fragment 许可 | GPT 结构、属性位定义 |

---

## 四、Android 前端设计参考（仅参考，未链接代码）

- **miuix** —— 界面风格与暗色模式
- **NP 管理器** —— 双窗口布局、分栏、跨窗拖拽
- **Fluent UI System Icons** —— 图标体系
- **WindowsIcons / WindowsIconCustomization / win-icon-extractor** —— Windows 风格文件图标与 `.exe` 图标提取

---

## 五、技能与工具链（Agent 工作流）

| 组件 | 用途 | 链接 |
|---|---|---|
| **SkillHub** | Skill 商店 CLI（本环境安装） | https://skillhub.cn |
| `@indiv-yaoyao/gh-cli-readonly-agent` | GitHub 只读检索技能 | 经 SkillHub 安装 |
| `@indiv-seafish/code-review` | 代码审查 | 经 SkillHub 安装 |
| `@clawhub_sf0799/code-refactor` | 重构 | 经 SkillHub 安装 |
| `@user_d4c0944b/git-commit` | Conventional Commit | 经 SkillHub 安装 |

---

## 许可证汇总

| 许可证 | 涉及组件 | 对本项目的影响 |
|---|---|---|
| **GPL-2.0+** | mkntfs / ntfsprogs | ⚠️ 启用 NTFS 格式化模块时，imgtool 须以 GPL-2.0+ 发布（**可选开关关闭则不受影响**） |
| **LGPL-3.0** | libregf | 动态链接即可 |
| **MIT** | zlib / PyYAML / jq / gh / Fluent UI 等 | 仅需保留版权声明 |
| **LGPL-2.1+** | libntfs-3g / **libhivex** | 动态链接即可，需保留版权声明 |
| **BSD-3-Clause** | CMake / libarchive | 同上 |

> 详细许可证文本与版权声明见各仓库 `LICENSE` / `COPYING` 文件。
> 本项目对其它开源项目的**修改**会逐项记录在对应文件头与 Git 提交中。
