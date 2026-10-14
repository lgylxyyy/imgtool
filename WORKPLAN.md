# Icdqcow2 底层工具 · 工作计划

> 目标：先把**可完整移植到 Android 的底层工具库**做完并自测通过，然后再写 Android 前端。

---

## 一、技术选型（已定）

| 项 | 决定 | 理由 |
|---|---|---|
| 语言 | **纯 C11** | NDK 原生、零运行时、内存省、可跑 iOS/桌面 |
| 构建 | **CMake** 一份配置 | PC + Android NDK 共用 |
| I/O | **fd-based**（不收路径） | 适配 Android SAF |
| 进度 | **回调 + JSON 行**，≥200ms 节流 | 前端画进度条 |
| NTFS | 封装 **libntfs-3g**（纯库，无 FUSE） | 安卓无 FUSE |
| WIM/ESD | 封装 **libwim** | 纯 C 可交叉编译 |
| ISO | 封装 **libarchive** | 纯 C 可交叉编译 |
| qcow2 | **自己实现**（zlib） | 项目核心 |
| FAT32/exFAT | **自己实现** | 结构简单 |
| NTFS 格式化 | **模板法** | 错一字节盘就废 |

---

## 二、模块与阶段

### 阶段 0 · 架构骨架（当前）
```
img_types.h        公共类型 / 错误码 / 进度结构
img_progress.{c,h} 进度上报（节流 200ms + 阶段变化立即发）
img_io.{c,h}       fd 抽象层（PC 用路径，Android 用 fd，只改这一处）
CMakeLists.txt     PC + NDK 双目标
```

### 阶段 1 · qcow2 核心（地基）
```
img_qcow2.h          对外接口
img_qcow2_read.c     读：L1/L2 三级映射、稀疏、zlib 压缩簇
img_qcow2_write.c    写：建文件 / 分配簇 / 覆盖 / 扩盘 / 释放
img_qcow2_create.c   创建全新 qcow2          ← 用户需求 (1)
img_qcow2_info.c     查看信息
验证：sha256 对拍现有 JS 读取器 → qemu-img check → 转回 raw 比对
```

### 阶段 2 · 分区 + 格式化（用户需求 (2)）
```
img_partition.c    GPT/MBR 读+写、建/删/改类型、扩盘搬备份
img_mbr.c          MBR 专用（引导码/CHS/活动标记）—— 含我踩坑的正确实现
img_gpt.c          GPT 专用（★8字节 EFI PART、CRC32、主备）—— 含我的坑
img_fat32.c        FAT32 读+写+格式化
img_exfat.c        exFAT 读+写+格式化
ntfs_template.c    NTFS 模板格式化（内置 $Boot 引导码）
规范来源：Microsoft Learn 官方分区文档（已取 BIOS/MBR，待补 UEFI/GPT）
```

### 阶段 3 · 引导自检 bccheck（替代挂虚拟机）
```
img_bootcheck.c    26 项检查 —— 每项来自我这轮实际踩过的坑
输出：PASS/FAIL + 原因 + 修复建议 + JSON 报告
```

### 阶段 4 · 引导修复 fixboot + BCD 写入
```
img_bootfix.c      重写 MBR / 移植 $Boot 扇区0~9 / 同步 $MFTMirr / 修 GPT 头
img_bcd.c          BCD 读
img_bcd_write.c    BCD 写（改 device、签名、重算 hive 校验）→ 消除最后的借用
```

### 阶段 5 · WIM 流式 + 部署（带进度）
```
img_wim.c          WIM/ESD 流式读（干掉 11GB wimtree）
img_deploy.c       Windows 部署（apply2ntfs 转 C + 加进度 + fd I/O）
```

### 阶段 6 · Android 打包验证
```
NDK 交叉编译 arm64-v8a / armeabi-v7a
无 root、无 FUSE 环境下跑通全流程 → 产出 .so
```

### 阶段 7（前端阶段）· 开源许可记录
```
THIRD_PARTY.md —— 记录所有开源项目与链接，贴进 App 设置页
```

---

## 三、Android 前端要求（记录备查，本阶段不做）

- 风格参考 **miuix**，支持暗色模式
- 交互按 **NP 管理器**：双窗口布局、分栏、跨窗拖拽（NP 管理器开源可参考）
- **图标**
  - 应用图标：miuix 图标库 / Microsoft **Fluent UI System Icons**
  - Windows 文件图标：**HaydenReeve/WindowsIcons**、**MicaUI/WindowsIconCustomization**、Windows 11 图标库
  - **exe 图标提取**：jinghu-moon/win-icon-extractor（或替代方案）
- **功能**：创建/浏览/编辑 qcow2 · 修引导 · 分区增删改 · 调磁盘大小 · 释放 Windows 镜像 · 浏览 iso/wim/esd · 拷文件到系统目录或 qcow2 分区内
- **THIRD_PARTY.md**：记录所用开源项目名称 + 链接，贴进 App 内

---

## 四、验收标准（每阶段通用）

1. **功能对拍** —— sha256 / sgdisk / mtools 真值
2. **交叉校验** —— `qemu-img check`（开发期）
3. **进度事件** —— 每个 phase 都有回调，JSON 协议正确
4. **可移植性** —— 无 FUSE、无 root、fd-based、无 Node 依赖
5. **自检** —— `bccheck` 全绿（阶段3起强制）
6. **实测** —— 阶段性 QEMU 实机确认一次

---

## 五、进度

- [x] 阶段 0 · 架构骨架 ✅ **完成**
- [x] 阶段 1 · qcow2 核心 ✅ **完成**（读+写+创建+扩盘，三重验证全过）
- [x] 阶段 2a · 分区表读写/增删/扩盘 ✅ **完成**（sgdisk+fdisk 全绿）
- [x] 阶段 2c · 约束层 + 可调尺寸 + MBR↔GPT 互转 ✅
- [x] 阶段 2d · 政策限制移至 App 层 + 分区调整 + 填满剩余 + 恢复分区属性 ✅ **完成**
- [ ] 阶段 2b · 格式化器（FAT32 / exFAT / NTFS 模板）⬅ **当前**
- [ ] 阶段 3 · bccheck 自检
- [ ] 阶段 4 · fixboot + BCD 写入
- [ ] 阶段 5 · WIM 流式 + 部署
- [ ] 阶段 6 · Android 打包验证

---

## 阶段进度表（实时）

| # | 阶段 | 状态 | 交付物 / 验证 |
|---|---|---|---|
| 0 | 架构骨架 | ✅ | 类型/进度/IO/CMake · 零警告 |
| 1 | qcow2 核心 | ✅ | 读+写+创建+扩盘 · sha256 对拍 / qemu-img check / QEMU 独立解析 三重验证 |
| 2a | 分区管理 | ✅ | GPT/MBR 读写增删 · MBR↔GPT 互转 · sgdisk `No problems found` + fdisk |
| 2c | 约束层 | ✅ | 政策移至 App，工具层只留技术硬检查 · 尺寸表导出 |
| 2d | 分区调整 | ✅ | resize(自动调后方) / fill_remaining(尾部零空余) · 实测 0 扇区 |
| 2b | **格式化器** | ✅ FAT32+NTFS | |
| 2b.1 | ├ FAT32 | ✅ | mtools 交叉验证全对 |
| 2b.2 | ├ **NTFS** | ✅ | 进程内格式化（ELF 前插，上游零改动）+ ntfscp/ntfsls 写读 · 与 mkntfs 对拍仅 9 字节序列号差 |
| 2b.3 | └ exFAT | ⬜ | （优先级低于引导链）|
| 2e | **恢复映像** | 🟡 **一半** | ✅ 415MB Winre.wim 已写入恢复分区(2.9s) 且读回校验一致 · ⬜ 待 BCD recoverySequence |
| 3 | **bccheck 自检** | ⬜ | 26 项检查，替代"挂虚拟机验证" |
| 4 | **fixboot + BCD 写入** | ⬜ | ⭐ 恢复分区③ BCD 恢复序列靠它 |
| 5 | WIM 流式 + 部署 | ⬜ | 干掉 11GB 临时目录 |
| 6 | Android 打包验证 | ⬜ | NDK 交叉编译 + 无root无FUSE跑通 |

### 未闭环的关键路径
```
NTFS 格式化器 ✅ ──→ 恢复分区能装 winre.wim ✅ ──→ ⬜ BCD 写入 ──→ bccheck/fixboot
        ↑ 已打通（L2 ② 完成）
```
