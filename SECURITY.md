# 安全与静态扫描

本仓库用 GitHub CodeQL 做静态分析（默认设置，分析 `c-cpp` / `actions` / `python`）。
编译与测试在 `.github/workflows/ci.yml` 的自建镜像里跑，与本文件无关。

## 处置原则

只有两条：

1. **可能有风险 → 修**，并补测试
2. **确认不会有风险 → 屏蔽（dismiss）+ 记录**

不允许的状态是「挂着不管」：开放告警长期不为 0，等于没有告警。
**当前：open = 0**（17 条已屏蔽，4 条已修）。

屏蔽的操作在 GitHub 的 Security 页签里做过（原因写在 `dismissed_comment`），
本文档是同一决定的版本化记录 —— 免得只留在网页上，几年后没人知道当时为什么放过。

## 已修（4 条）

| 告警 | 位置 | 问题 | 提交 |
|---|---|---|---|
| `cpp/overflowing-snprintf` | `src/img_cmds.c`（8 处） | `k += snprintf(...)` 截断时返回「本该写多长」，`k` 超过 `CB_CAP` 后 `cb+k` 出界、`CB_CAP-k` 变成巨大 `size_t` | `8bfc4e2` |
| `cpp/overflowing-snprintf` | `src/img_wim_apply.c` | 同一模式，`cap` 由调用方给，更容易踩到 | `8bfc4e2` |
| `py/insecure-temporary-file` | `tools/ntfs-inventory.py` | `mktemp()` 在返回路径与写文件之间有竞争窗口，换 `mkstemp()` | `8bfc4e2` |
| `cpp/redundant-null-check-simple` | `src/img_cmds.c` | `td && strcmp(...)` 的半截判空是死代码（上一行已 `return`） | `ca7fa2c` |

## 已屏蔽（17 条）

### 1. vendored unrar 的滤波链（6 条 critical，`third_party/unrar/unpack30.cpp`）

5 条 `use-after-free` + 1 条 `double-free`，都报在 RAR3 解包的滤波器叠加逻辑上。

**结论：误读，不是 UAF。** 依据：

- `Prg->FilteredData` 指向 `RarVM::Mem`（`rarvm.cpp` 里 `Init()` 分配的 VM 缓冲），
  整个解包期间有效，**不随 `delete PrgStack[I]` 释放**
- 被 `delete` 的是 `UnpackFilter30` 对象本身，它持有的是 `VM_PreparedProgram` 结构体，
  不是那段数据缓冲
- 循环里 `delete` 的索引与随后读取的 `NextFilter` 也不是同一个对象

保留态度：这是**第三方代码**，我们只做「随上游升级」而不手工打补丁；
如果上游发布相关修复，跟着升版本即可。升级后**重新跑扫描并复核这几条**。

### 2. `cpp/integer-multiplication-cast-to-long`（1 条，`third_party/unrar/rs16.cpp`）

`new uint[NE * ND]` 的乘法。**结论：不可达。**
`RSCoder16` 只在 `recvol5.cpp`（RAR5 恢复卷的修复/生成）里使用，
我们只做解压，从不构造该对象。

### 3. `cpp/toctou-race-condition`（10 条，`src/img_xcopy.c` / `src/img_types.c` / `src/img_io.c`）

先 `stat/lstat` 判类型、再 `unlink/rmdir/open` 的经典模式。

**结论：接受风险（不是误报）。** 工具没看错，是这个用法下没有可利用面：

- 路径来自**本进程自己的临时目录**（名字里带 pid）或**用户自己选的镜像文件**
- 要利用，攻击者得先能在这些目录里写东西 —— 那他已经具备同等权限，
  不需要再借我们的 TOCTOU
- 彻底消掉需要改成 `openat` + `fstat` + `unlinkat` 一套写法，收益为零

## 什么时候重新看

- 升级 `third_party/unrar` 版本之后（第 1、2 类结论都依赖这个版本）
- 改了临时目录的创建/清理策略之后（第 3 类的威胁模型可能变）
- CodeQL 规则库大版本更新之后（同一条代码可能被新规则重新报出来）