# 输出 / 日志 / 错误 / 尺寸校验 —— 工具 ↔ App 契约

> 本文是**接口约定**（不是实现笔记）。工具只提供"事实 + 开关"，**决定权在 App**。
> 决策日期 2026-10-15，参与方：用户（产品）+ 工具层。
> 实现状态：见文末"落地进度"。

---

## 一、一件事，三张脸（唯一打点源）

```
库层打点（只打一次）
   │  事件：级别 / 模块 / 事件名 / 错误码 / 人话 / 上下文(键值)
   ├─① 日志文件      里程碑 + 错误，落盘、能带走            ← 给排障
   ├─② App 屏幕      过程实时输出，不落盘                  ← 给用户看"在干什么"
   └─③ 失败卡片      出错时：原因 + 位置 + 建议 + 尾部       ← 给用户复制/发我
```
铁律：**不许三处各自打点**（必然分叉；本项目已吃过"自测绿、外部不认"的亏）。
App 屏幕上那块 = 调同一个渲染函数塞进自己的控件，不重写一套文案。

---

## 二、日志文件：只写"里程碑 + 错误"（默认档）

**用户决策：日志只记【开始做】和【完成做】，过程只给 App 显示，不落盘。**

```
time="2026-10-15 18:39:01.448" level=INFO tool=imgmkfs event=session args=/workspace/images/x.qcow2
time="2026-10-15 18:39:01.450" level=INFO logger=fat32 event=begin message="格式化 FAT32 分区" action=format lba=2048 size_bytes=134217728
time="2026-10-15 18:39:01.526" level=INFO logger=fat32 event=done rc=0 action=format duration_ms=76
time="2026-10-15 18:39:30.900" level=ERROR logger=cmd event=fail rc=-10 error_code=FAT-ENOSPC at=/EFI/x message="宿主剩余 196 MB，本次预计需要 286 MB"
```

### 字段名对照表（★ 键名照抄业界惯例，不是我发明的）

| 键 | 含义 | 名字来源 |
|---|---|---|
| `time` | 事件时刻（本地时间，毫秒）—— **值里有空格，logfmt 规范强制加引号** | slog `time=` / zap `ts=` |
| `level` | `ERROR`/`WARN`/`INFO`/`DEBUG`/`TRACE` | slog / zap / logfmt 通吃 |
| `logger` | 模块（`fat32`/`ntfs`/`bcd`/`part`/`app`…） | logfmt 常用 `logger=` 或 `component=` |
| `tool` | 哪个程序（`imgmkfs`…，只在会话行出现一次） | — |
| `event` | 生命周期：`session`/`begin`/`done`/**`cancel`**/`fail`/`alive` | 惯例："event = 发生了什么"。**取消是独立事件**（`WARN` 级、必写、带 `error_code=CMD-CANCELLED`），不与"完成/出错"混用 |
| `action` | 具体操作：`format`/`put`/`save`/`check`… | 审计日志惯用 `action` |
| `message` | 人话一句话 | Python/R logfmt 用 `message`（Go 用 `msg`）|
| `rc` | 返回码（0=成功，负值=`img_err_t`） | C/Unix 通用缩写（用户选定保留）|
| `error_code` | 稳定的错误桶名（`FAT-ENOSPC`…） | 惯例 `error`/`error_code` |
| `duration_ms` | 该操作耗时（毫秒） | 单位写进键名（惯例），值不换算 |
| `size_bytes`/`written_bytes`/`need_bytes` | 字节数 | 同上；**值一律原始字节，永不换算成 MB/KB**（用户要求：换算会丢精度、害排查）|
| `path`/`at`/`lba`/`part_lba`/`cluster`… | 上下文字段，按需出现 | `lba` 是存储领域通用术语（起始扇区号）|

> **格式：logfmt（`key=value`，空格分隔）**
> *键 / 数字 → 永不加引号；字符串 → 只在含空格、`=`、引号或为空时才加引号并转义。*
> 同一行 JSON 111 字节、logfmt 83 字节（省 25%）。
> 为什么不用 JSON：JSON 规定键和字符串**都必须**带引号，减不掉（此前试过键不加引号
> → `{ok:true}` 被 Python 直接判非法）。
> ⚠ 时间带空格所以必须加引号，这是**唯一必须**带引号的字段（`message` 含空格时也会加）。
> **stdout 上的结果信封仍是 JSON**（`{"ok":true,"version":1,"data":{...}}`）——
> 那是给 App 程序解析的接口，标准格式不能动。
> ⚠ **stdout 上的"结果信封"仍是 JSON**（`{"ok":true,"data":{...}}`）——那是给 App 的
> 程序接口，标准格式不能动；日志文件（人查/我查）用 logfmt。
```

### 硬性要求
| 项 | 约定 |
|---|---|
| 字段名 | 短键：`t`(相对起始 ms) `l`(E/W/I/D/T) `m`(模块) `ev`(事件) `rc` `code` `ms` `msg` + 按需上下文 |
| **长操作必须拆段** | 否则"开始/完成"之间隔十分钟没法定位（例：灌系统拆成 ①建目录树 ②解压数据 ③写引导） |
| **begin 必须带参数** | 对象/大小/分区/LBA — 否则"开始写入"等于没说 |
| **done 必须带耗时 + rc** | 耗时能发现性能退化；rc 形成完整链路 |
| 错误 | `l="E"`，**任何级别都写**（`--log-level=error` 也一条不漏）；**立即 write**（不 fsync） |
| 心跳 | 仅"有操作进行中"时打；正常 2s/条，**原地不动则指数退避 2→4→8…→60s**（卡住时反而更省，且"最后一条 alive 的时间"就是卡死证据） |
| 过程事件 | **不落盘**；内存环形缓冲保留最近 **1000** 条（≈200 KB），出错时随失败卡片交出 |
| 轮转 | 单文件上限 4 MB → 改名 `.1`（旧的删）→ 恒定 ≤8 MB；轮转后新文件头补写一行 `session`（版本/命令/镜像指纹），保证半截日志也能单独看懂 |
| 日志写不进去 | 不报错、不阻塞 → 降级内存环形缓冲，卡片注明"日志未落盘" |
| 进度事件 | 默认**不进**日志（进度走 stderr/UI）；`debug` 档才按节流记里程碑 |

### 级别
| 级别 | 内容 | 一次建盘量级 |
|---|---|---|
| `error` | 只记失败 | 几 KB |
| **`info`（默认）** | 里程碑 + 所有失败 + 心跳 | **≈30~80 KB** |
| `debug` | 加字节级明细（写目录项/LFN/短名/BCD 改 GUID 的期望 vs 实际） | 3~8 MB（受轮转压住） |
| `trace` | 加每次 I/O | 冲上限，仅短时使用 |

> `debug` 档必须保留：**"每步都成功、最后外部不认"**这类问题（如 LFN 字节 bug → bootmgr 报 0xc000000f）
> 在里程碑档里全是绿灯，只能靠 debug 复现一次看明细。

---

## 三、App 屏幕输出（像 Linux 终端）

```
✔ 打开镜像 x.qcow2（128 GB · GPT）
✔ 扫描分区 5 个：ESP/MSR/系统/数据/恢复
▶ 写入 winre.wim  415 MB  ▓▓▓▓▓▓░░░ 62%  257/415 MB  剩余 1s
✔ 写入 winre.wim（415 MB · 1.2 秒）
✖ 失败：写入 winre.wim（原因：目标分区剩余 380 MB < 需要 415 MB）
   建议：把恢复分区扩到 ≥1.5 GB，或换更小的 winre.wim
```
- 完成的事 → 打 ✔ **留在屏幕上**（历史不可变）
- 进行中的事 → 只有这一行**原地刷新**百分比
- 失败 → 一行 ✖ + 原因 + **建议**
- 结束 → 摘要（几步/耗时/产出路径）
- 技术细节（GUID/簇号/错误码）不刷屏

---

## 四、失败卡片（用户可整段复制）

出错时屏幕给一张纯文本卡片（无颜色、固定宽度、`════…` 包住，方便整段选中），内容：
```
工具/版本/平台(ABI)  ·  命令(argv)  ·  镜像指纹(大小/qcow2头/GPT diskGuid)
失败位置(阶段→操作→对象：分区/LBA/路径)  ·  错误码  ·  原因(期望 vs 实际)
当前状态(哪几步已完成 ✔ / 是否半成品 + 建议)  ·  建议(来自错误码表)
耗时(总/该步)  ·  事件尾部(内存最近 1000 条)
```
**双保险**：同时在 `<镜像名>.error.txt` 落一份并把路径打在卡片上（终端里拖选长文本很痛苦，让用户直接发文件）。

### 错误码表（App 可用来做本地化提示 / 一键跳到修复功能）
| 错误码 | 触发 | 建议 |
|---|---|---|
| `FAT-CHAIN-LOOP` | 簇链遍历超过簇总数（判定成环） | 镜像损坏 → `imgq2test` 校验/换镜像 |
| `NTFS-ENOSPC` | 目标分区放不下 | 扩分区 / 换小文件 |
| `WIM-IDX-INVALID` | WIM/ESD 索引不存在 | 先 `imgwimls` 列索引 |
| `BCD-DEV-NOTFOUND` | device 指的分区 GUID 不在本盘 | `imggenbcd` 重映射 |
| `PART-OVERLAP` / `GPT-CRC-BAD` | 分区表不合法 | `imglayout check` |
| `IO-EACCES` / `IO-ENOSPC` | 底层 fd 失败 | 权限/空间（带 errno） |

---

## 五、尺寸估算 与 校验开关

**用户决策：默认【不校验】，App 显式传参才校验**（这个值本来就该由 App 给，工具不替它决定）。

```
缺省（App 不传）      → 不估算、不拦，直接写
App 传 check_size=on  → 事前"需要 vs 剩余"，不够就提前拒（无半成品）
估算查询（随时可查）   → 不受开关影响；App 想在自己界面显示"需要 7.9 GB"就自己查、自己决定
写中真出 ENOSPC       → 【不可关】照旧报错 + 错误卡片（这是"出错必须说清"，不是"校验"）
未校验时             → 日志留痕 {"ev":"sizecheck","skipped":true,"reason":"app-off"}
```
- 要估的是**两个空间**：① qcow2 内目标分区 ② **宿主磁盘上 qcow2 文件会长多大**（用户真正会踩的是②）
- **不需要临时空间**：WIM 直灌、零临时目录（现有设计已做到）
- 估算公式：`Σ文件按簇取整 + 文件数×元数据开销(≈1~2KB) + MFT/索引 + 余量 5~10%`，**宁可保守高估**
- 写入**结束后返回实际占用字节数** → App 可校正估算；卡片里可写"已写入 62%"
- 接口：`img_write_opts_t.check_size`（0=不校验缺省 / 1=校验目标分区 / 2=含宿主）；CLI `--check-size=on|off|host`（不传=off）

---

## 六、参数清单（CLI 与 App opts 一一对应）

| `part.scan` | 扫描分区（App 弹不弹提示的依据）| `img` `lba` 必填 → 返回 level(0/1/2)/fs/label/cluster_bytes/entries_root/used_bytes/free_bytes/looks_like_windows |
| `ntfs.list` | 列 NTFS 目录（给 App 名单）| `img` `lba` 必填；`path=/` 可选 → count/names(JSON 数组) |
| `ntfs.rename` | 搬移原语：把【给定名单】移进目标目录 | `img` `lba` `dst` `names` 必填 → moved/dir_used（冲突自动 .001）|
| `winre.auto` | 写/删 WinRE 出厂自动启用脚本（SetupComplete.cmd）| `img` `lba`(系统分区) 必填；`enable=true` 可选 → written/bytes 或 removed |
| `img.clone` | 克隆保留分区到新盘（换系统主路径）| `img` `keep` 必填；`guid=random` `keep_old=false` `old_name` 可选 → bytes_written/parts_copied/old_kept |
| `wim.apply` | 灌入 WIM/ESD 到分区（**默认先格式化**防残留；`--format=off` 保留原分区） | `img` `lba` `wim` 必填；`index=1` `format=on` `label=Windows` `no_acls` `no_attrs` `wimboot` 可选 |
| `wim.size` | 空间估算 | `wim` 必填；`index=1` 可选（命令见 §五） |
```
--log=<路径>| - | none      日志落盘位置（缺省：镜像旁 x.imgtool.log；App 侧建议私有目录+fd）
--log-level=error|info|debug|trace     缺省 info
--log-max=4M                单文件上限（缺省 4M）
--log-keep=1                保留几份历史（缺省 1）
--log-new                   每次运行新文件（留最近 3 个）
--check-size=on|off|host    缺省 off（App 显式请求才校验）
--progress=bar|json|none    屏幕输出形态（缺省：tty=bar，非 tty=json）
```

---

## 七、并发 与 fd 生命周期 契约

### 并发（App 多线程调用时）
| 场景 | 保证 |
|---|---|
| **同一个句柄**（同一 `img_ctx` / `img_qcow2`）并发调用 | ✅ 自动串行（`img_cmd_exec` 内建句柄锁；绕过命令层直接调库函数的，请自己用 `img_lock_*`）|
| **不同镜像**（不同句柄）并行 | ✅ 允许 —— 各模块句柄状态私有（`img_qcow2` / `img_fat32_fs` / `img_ntfs_fs` 都是按句柄分配）|
| **日志 / 环形缓冲** | ✅ 内部自带进程级锁，随便并发写（验证：4 线程 × 250 条 → 1000 行全部是合法 JSON，无交错坏行）|
| 同一进程里同时跑两个**写**操作到不同镜像 | ✅ 允许，但不建议（IO 争用）|
> 为什么不是"全局大锁"：那样 App 里两个后台任务会互相堵；句柄锁粒度才够用。
> 为什么不是"完全无锁"：库里有进程级全局状态（日志器、NTFS 前插上下文），无锁并发会
> 呈现"莫名其妙的数据错"，是最难查的一类 bug。

### fd 生命周期（Android SAF 传 fd 进库）
```
img_io_open_fd(fd, take_ownership, opts)
  take_ownership=true   → img_io_close() 负责关掉它
  take_ownership=false  → 调用方自己关，且必须保证在 img_io_close() 之前一直有效
```
入口即校验（就近报错，而不是后面莫名失败）：
| 传进来的 fd | 行为 |
|---|---|
| 负数 / 已关闭 | `NULL`（立刻失败）|
| 目录 fd | `NULL` |
| 管道（不可 seek）| `NULL` |
| 只读 fd 但要求可写 | `NULL` |
| 合法 | 正常；`img_io_is_fd()` / `img_io_fd()` 可查询 |
| 调用方**提前关掉**（违反契约）| 后续读写返回错误码（带 `errno`），**不崩** |

验证脚本：`tests/fd_test.c`（15 条）、`tests/thread_test.c`（4 组并发场景）。


| 步 | 内容 | 状态 |
|---|---|---|
| 0a | 事件流 API + 关键路径打点（含"需要/剩余"类上下文） | ⬜ |
| 0b | 里程碑日志 + 内存环形缓冲 + 轮转 + 心跳退避 | ⬜ |
| 0c | 屏幕人话行 + 失败卡片 + `.error.txt` + 错误码表 | ⬜ |
| 0d | 反向验证：造 5 类失败，确认卡片能指对位置/原因/建议 | ⬜ |
| 0e | 尺寸估算（工具出数字）+ 校验开关（缺省 off）+ 写中错误卡片 | ⬜ |

---


## 八、多语言（i18n）契约：库发码，视图层出文案

**用户结论**：分开写 —— **库（imgtool 核心）不带任何文案**，"中文还是英文"由调用端决定：
CLI 是我们自己用的临时渲染（中文默认 / `--lang=en`）；App 用同一份码 + 自己的语言表。

### 库给出什么（语义码 + 数值，语言中立）
| 用途 | 字段 |
|---|---|
| "正在做什么" | `event=begin/done`、`action=format\|put\|save\|check`、`path`/`lba`/`size_bytes` |
| "出了什么错" | `error_code=FAT-ENOSPC`、`rc`、`at`、`duration_ms`、`size_bytes` |
| 失败快照 | `img_error_info()` → `{code, rc, at, what, status, img, ctx, log, events(机器格式尾部)}` |
| 库的文本 | **没有**。`message` 字段只是【开发期备注】（可能中文）写进日志，**不是 App 契约** |
| stdout 结果信封 | `{"ok":true,"version":1,"data":{...}}` / `{"ok":false,"err":{"rc","error_code","at"}}` —— **不再含中文文案** |

### 视图层出什么
```c
void img_out_set_text(img_text_cb cb, void *ud);   /* 库发事件 → 回调由消费端渲染文案 */
```
- **CLI**（`tools/imgcmd.c`）：内置一张 zh/en 文案表（`A_TEXT` 动作名 / `E_TEXT` 错误码→原因+建议），
  渲染人话行与失败卡片，并落 `<镜像>.error.txt`。**这张表是 CLI 专用**，不参与库。
- **App**：拿 `event/action/error_code + 数值` 用自己的表渲染；失败后调 `img_error_info()`
  取结构化快照（含事件尾部），渲染本地化卡片 + "复制到剪贴板"。

### 加语言的代价
- CLI：文案表加两列（`zh/en` → 再加 `ja` 等）
- App：App 自己的语言资源文件（与库无关）
- **库一行都不用改**


## 九、落地进度
