# 健壮性加固工作计划（P0→P2）

> 起因：用户问"有没有冗余设计/防卡死/进度可见"。查证后按**最坏影响**排优先级。
> 每阶段都要**自验**（正向对拍 + 反向造坏数据），独立实现交叉验证是硬要求。

---

## 现状勘察结论（先澄清两处）
```
qcow2  用 L1→L2 索引表（不是链），L2 只缓存一张表 → 无绕圈风险、无内存膨胀
NTFS   走 libntfs-3g，其内有自己的校验；我们自己的循环（1MiB 分块写）有界
FAT    真正的问题所在：多处 `while(c>=2 && c<FAT32_EOC)` 沿链走，
       环状 FAT 会永久死循环；read_file 有 size 界，其余没有
```
| 现有 | 位置 |
|---|---|
✅ EINTR 重试 | `img_io.c:90,106`
✅ `chain_nth` 步数上限 / `alloc_cluster` 扫描上限 | `img_fat32_fs.c:103,75`

## 优先级与验收标准

### P0-1 · 簇链遍历加界（防打开坏镜像死循环）
**最坏影响**：用户打开别人给的损坏/恶意 qcow2 → 永久死循环、只能杀进程。
```
改动  img_fat32_fs.c: dir_find / list_dir / dir_make_room / free_chain / read_file
      → 步数上限 = cluster_count+1（天然上界），超界返回 IMG_ECORRUPT
验收① 正向：完整 ESP 142 文件重灌 + mtools 抽回 md5 全一致（回归）
验收② 反向：脚本故意把目录簇 FAT 项改成自环 → `timeout 10` 跑 ls/读文件，
      必须 <1s 返回错误码（死循环时代会 timeout）
```

### P0-2 · 取消真的能停（避免"以为卡死→杀进程→坏镜像"）
**最坏影响**：点取消无反应；强杀时正在写盘 → 半成品/损坏镜像。
```
改动  在长循环插检查点：FAT/NTFS 每 1MiB、WIM 提取回调、格式化阶段、qcow2 大块写
      取消语义：返回 IMG_ECANCELLED，且【不写目录项】、释放已分配簇链
验收① 扩展 progress_test：回调里调 cancel → 断言后续循环返回 IMG_ECANCELLED
验收② FAT/NTFS 两条写入路径各测一次；取消后镜像仍能被 mtools/ntfsls 读（无残留）
```

### P1-3 · 命令行能看见进度与"正在干什么"
**最坏影响**：静默数分钟 → 误判卡死 → 杀进程 → 坏镜像（本次 0xc000000f 排查中就反复出现"静默像卡死"）。
```
改动  新增 img_prog_render_bar()（把 img_progress_t 渲染成一行人读文本）
      CLI 加 --progress=bar|json|none（默认 bar 若是 tty，否则 json），
      imgmkfs/imgntfsfs/imgfatfs/imgfixboot/imgwimls 接上回调
验收① tty 与非 tty 两种输出各抓一次；bar 模式不影响既有 JSON 消费者
验收② 跑 415MB winre.wim 写入，肉眼可见百分比+当前文件+ETA
```

### P1-4 · FAT 写文件改分块（去掉 OOM 风险 + 进度变流式）
**最坏影响**：往 ESP 拷 400MB+ 文件内存爆 → App 被系统杀（闪退）。
```
改动  img_fat32_put_file：不再 malloc 整个文件；预分配簇链 → 1MiB 分块读→按簇映射写
      → 末尾写目录项；进度按字节递增（与 NTFS 侧对齐）
验收① /usr/bin/time -v 看 MaxRSS：715MB 源文件应从 ~700MB 降到 <16MiB
验收② mtools 抽回逐字节 md5 一致；ESP 全树重灌回归
```

### P2-5 · 记录待办（本轮不做，避免范围失控）
```
⬜ I/O 超时（SAF 读远端/慢介质）—— 需要接口设计（异步/超时参数）
⬜ 库缺失时的降级路径（例：无 NTFS 库时禁用该功能并明确报错）
```

---
## 执行记录（边做边填）

| 阶段 | 状态 | 自验结果 |
|---|---|---|
| P0-1 簇链加界 | ✅ 已提交 `5d91c9e` | 反向用例 `tests/corrupt-chain-test.sh`：环状 FAT 从"永久死循环"变为 <2s 返回 `IMG_ECORRUPT`；顺带修出 2 个真缺陷（rm 丢 free_chain 错误码、空目录判断传 NULL 回调） |
| P0-附加 非法参数 | ✅ 已提交 `a71e0c3` `7c18145` | 见下表：130 条非法输入，修前 4 崩（含 1 处**毁镜像**），修后 **0 崩 0 毁盘** |
| P0-2 取消接线 | ✅ | `cancel_test` 7/7：中途取消→`IMG_ECANCELLED`、**不留半成品文件**、**剩余空间回基线（不漏簇）**、不污染下一次、**覆盖写时取消旧文件完好**。检查点在 FAT/NTFS 每 1MiB 一处，并通过 `ntfs_device` 回调**穿透 wimlib** 直灌 |
| P1-3 CLI 进度可见 | ✅ | 事件流 + 人话行 + 失败卡片 + `.error.txt` + **16 个老 CLI 全部接上 `--log`**（共享 `img_log_cli`，各自零改动）；`--progress=bar|json|none` |
| P1-4 FAT 写分块 | ✅ | 1MiB 流式写：写 64MB 源文件 **MaxRSS=9.5MB**（原 64MB+，Android OOM 风险归零）；mtools 抽回 md5 逐字节一致；顺带改成"先写新、后释放旧"→ 覆盖写失败不再产生悬空目录项 |
| 0e 尺寸估算 + check_size | ✅ 估算 + 开关都通 | `wim.size` 命令：真实 ESD → `data_bytes=6861906464`（与上一轮实测 6.86GB 一致）、`file_count=32707`（一致）、`est_ntfs_bytes=7483201632`（含元数据+8% 余量）；`fat.put --check_size=1` 提前拦下并给"剩余 196MB / 需要 286MB"；非法索引 → `WIM-IDX-INVALID` |
| C 命令表骨架 | ✅ `a2a35cc` | `--list`（App 生成界面用）/`--help` 自动生成/统一信封/22 条验收全绿 |
| B 并发 + fd 契约 | ✅ `5914928` | `fd_test` 15 条 · `thread_test` 4 组全绿；契约见 DESIGN-OUTPUT.md 第七节 |

### WIM 直灌实测（2026-10-16，真 ESD 6.86GB）
```bash
tests/wim-apply-test.sh        # 或 WITH_WIM=1 tests/run_all.sh
```
| 路径 | 结果 |
|---|---|
| 完整灌入 | rc=0；日志 `begin(wim/index/part_lba)` → `done duration_ms=`；**抽回 ntoskrnl.exe 13,862,384 字节、PE 头 MZ** ✓ |
| 中途取消（6s） | rc=-13 `IMG_ECANCELLED`，5,995 ms 内停下；日志 `event=cancel level=WARN error_code=CMD-CANCELLED` ✓ |
| 镜像完好性 | 两条路径后 `qemu-img check` 均无错 ✓ |
> ⚠ 耗时波动大（60s / 71s / 461s 三次实测）：沙箱 swap 吃紧时 qcow2 写路径会被拖慢，
> 不是代码回归；真机（KVM/hvf）上会稳定得多。

### WinRE 出厂自动启用（2026-10-16，用户实测发现）
现象：离线拼的盘 `reagentc /info` 显示 Disabled，要手动 `reagentc /enable` 才行（能进恢复环境，只差配置态）。
根因：`reagentc` 看 `C:\Windows\System32\Recovery\ReAgent.xml` 的 InstallState + BCD 的 recoverysequence；
      离线构建没跑过 enable → 两处都是禁用态。
解法（`winre.auto` 命令）：往系统分区写 `\Windows\Setup\Scripts\SetupComplete.cmd`
      （内容：`reagentc /enable` + 把 /info 输出存日志），Windows 首次开机自动跑。
      ★ 不离线写 ReAgent.xml：里面是 `GLOBALROOT\...\partition**N**` 写死的分区号，换机会失效；
        让官方工具在目标机器上自己探测+写才跨机器安全。参考流水线已加 ⑦ 步。

### 库缺失降级（硬缺口 #3，2026-10-16 完成）
之前：缺库只发 warning，照样编译/链接 → **直接构建失败**（那不是降级）。
现在：`IMG_WITH_WIM` / `IMG_WITH_NTFS3G` 开关 + 自动探测；缺库 = 关功能、命令仍在表里
**明确报 `FEATURE-UNAVAILABLE`（rc=-5 + at=缺哪个库 + 建议）**，App 可先看 `--list` 的
`features:{wimlib,ntfs3g,zlib,mkntfs}` 提前灰按钮。
验收（两个独立构建目录实测）：
  - `-DIMG_WITH_WIM=OFF`：features.wimlib=false；wim.*→FEATURE-UNAVAILABLE；fat/ntfs 正常
  - `-DIMG_WITH_NTFS3G=OFF`：ntfs3g=false；ntfs.*/winre.auto/wim.*→FEATURE-UNAVAILABLE；fat 正常
顺带修掉 5 个**先就存在的地雷**（fresh 构建必炸/链接失败）：
  1. 通用管道（ctx_activate/device_ops/probe）被错放在 mkntfs 专属文件 → 新拆 `src/img_ntfs_ops.c`
  2. img_bootcheck.c 错绑 mkntfs 开关 → 默认配置 imgbc 链接失败 → 挪进 NTFS3G 分支
  3. `HIVEX_LIB` 主 CMake 从未 find（老缓存掩盖）→ 补探测 + imggenbcd 门
  4. imgcmd 曾被"整块门"误吞 → 入口命令独立出块（任何配置必须有）
  5. imgntfsfs/imgntfscp 链 mkntfs 对象 → 门从 NTFS3G 改挂 NTFS

### 32 位（armhf）验证 —— 2026-10-16 完成（核心层）
复现配方（本沙箱实测可用）：
```bash
DEBIAN_FRONTEND=noninteractive apt-get install -y gcc-arm-linux-gnueabihf qemu-user-static
cmake -B /tmp/b-arm32 -S . -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
      -DIMG_WITH_WIM=OFF -DIMG_WITH_NTFS3G=OFF -DIMG_BUILD_TESTS=OFF
qemu-arm-static -L /usr/arm-linux-gnueabihf /tmp/b-arm32/tools/imgcmd ...
```
实测（ELF 32-bit ARM，size_t=4 字节）：
| 场景 | 结果 |
|---|---|
| 建 20GB 盘（64 位偏移）| ✅ size=21474836480，分区/GUID 正确 |
| FAT 格式化 + put 50MB（1MiB 分块）| ✅ size 精确，**md5 与源逐字节一致** |
| part.scan / fat.exists | ✅ level=2、free_bytes、size 全对 |
| img.clone（ESP + 19.9GB 分区）| ✅ rc=0、稀疏生效（写 3MB）、2 分区 |
| 静态审计 | ✅ 无 `long` 存 64 位偏移、无 `%ld` 打 64 位（全程 uint64_t/`%llu`）|
**未覆盖**：NTFS 层的 32 位实测 —— noble 源里没有 `libntfs-3g-dev:armhf`。
风险评估：NTFS 路径全程用 libntfs-3g 自己的 s64/u64（显式 64 位），我方代码同为 uint64_t；
真机（arm32 Android，从源码编 ntfsprogs）上做一次 WIM 灌入冒烟即可闭环。

### I/O 超时（无进展看门狗，硬缺口 #2，2026-10-16 完成）
**为什么是"无进展超时"而不是"总时长超时"**：总时长会误杀合法慢操作（实测 WIM 灌入在
swap 吃紧时 461s 但全程 I/O 连续不断）；无进展超时只盯"连续 N 毫秒没有任何一次读写完成"。

**默认 30 秒的依据（自有实测）**：
| 依据 | 数字 |
|---|---|
| 1MiB 块在 5MB/s 慢卡上的合法间隔 | ~170ms |
| WIM 最大单次读在慢卡上的间隔 | ~1.6s |
| 最长正常整段操作（461s 那次）的 I/O 静默 | <1s |
| 心跳判定"死透了"（2s→4s→…翻倍累计）| ~60s |
→ 30s 无 I/O = 不是慢是死（触发它 ≈ 1MiB 块写 30 秒 = 34KB/s）。
**分档建议**：本机镜像 10s / **默认 30s** / SAF·慢介质 60s / `0` 关闭。

实现：`img_io_read/write` 每次完成 → `img_io_touch()`；看门狗线程超时 →
①记 E 级日志（`error_code=IO-TIMEOUT` + gap_ms，任何级别都不丢）②`img_cancel()`
③随后每次 I/O 入口 fail-fast（`IMG_ETIMEDOUT=-16`）。
**诚实边界**：它无法打断"已经卡在内核/JNI 里的那一次调用"；能让后续 I/O 立即失败、
操作尽快停并给出明确错误码 —— 比"静默挂死"强得多。
CLI：`--io-timeout-ms=N`（env `IMGTOOL_IO_TIMEOUT_MS`）；默认 30000。
验证：`tests/timeout_test` 5/5（含 fail-fast）+ 端到端（1ms 触发 IO-TIMEOUT、30s 默认不误杀、0 关闭）。

### MBR/BIOS + RE 盘（进行中，2026-10-16）
**任务**（用户指定）：GPT→MBR 跨格式迁移 —— 128GB MBR 盘：系统保留 + 系统 64GB（从
`Windows 11 UEFI.qcow2` 的系统分区**扇区级克隆**，不做 NTFS 扩容）+ 恢复 1GB（0x27，
官方 winre.wim/boot.sdi）+ **扩展分区**（用户点名要练这块）；一次性脚本用 **RunOnce**
（源盘已进过桌面 → SetupComplete 那次机会已用掉，只能 RunOnce/Startup+自删）。
**已完成**：
- ✅ MBR 引导码参考件 `reference/mbr-code-bios.bin`（从 diskB 提取，440 字节）
- ✅ `part.copy` 跨镜像扇区级拷贝（源只读/目标稀疏/1MiB 分块）——实测 512MB 分区只落盘 3MB、内容逐字节一致
- ✅ `part.mbrboot` 写引导码+随机签名——实测 **sfdisk 独立认账**（Disklabel type: dos、Disk identifier、活动标记）
- ✅ EBR/逻辑分区能力此前已在库里（`img_partition.logical` + `add_logical`）
**待做**：扩展分区布局的 sfdisk 对拍验证 → BIOS 引导文件布局（bootmgr+\Boot\BCD 到活动分区，
VBR 用 img_fixboot）→ 官方 winre.wim/boot.sdi 放 0x27 分区 + BIOS 恢复条目 → RunOnce 离线写入
（libhivex 改 SOFTWARE hive，写 `!WinRE-Enable` 值）→ SeaBIOS 实测进桌面+进恢复环境。

### MBR/BIOS 引导 —— CHS 非法导致 Windows 不认分区（2026-10-16 用户 PE 实测发现）
**症状**：PE 里磁盘管理显示"磁盘 0：未知/128GB/没有初始化"、整盘"未分配"（Windows 不认分区表），
而 sfdisk 却能正常读出全部分区（两套校验标准不同）。
**根因**：MBR 分区表项的【起始 CHS】写的是 `00 00 00` —— CHS 扇区号是 1-based，
**0 是非法值** → Windows 判分区表损坏 → "未初始化"。代码里注释写着"起始 CHS：用真实计算"但**根本没实现**。
**修复**：`lba_to_chs()`（255头/63扇区）算真实 CHS，越界用合法哨兵 `fe ff ff`（段=63 ≥1）；
主表 + EBR 链接项都修。**字节级证据**：修复后项1 = `80 20 21 00 07 7f 39 06 …`
与 diskB（Windows 认的盘）**逐字节相同**（LBA2048→磁头32/扇区33/柱面0；末扇区→柱面6/磁头127/扇区57）。
**未解决的下一环**：bootmgr→winload 仍 0xc000000e（CHS 不是它的原因）。
排除清单：Windows 内容/文件 md5/偏移/签名/BCD绝对偏移/扩展分区/主备启动扇区/克隆完整性 —— 全部一致。
**最终根因（用户手动修复后确认）**：`bootmgr` 无罪（md5 与 Win11 PCAT 副本相同）——
**唯一差异是 BCD**：我借用了 diskB 的 **Windows 10 时代 BCD**，而目标系统是 **Windows 11**。
用户在 Windows 里跑 bcdboot 重建后 → 盘成功启动到 Windows 徽标（我截屏实证）。
**修复资产**：从用户修好的盘提取 `reference/bcd-reference-bios-win11.bin`（32768 字节，
bcdboot 生成的 Win11 BIOS BCD）——替换 diskB 的 Win10 BCD 作为参考件。
**两个学习点**：
  1. 用户重建 MBR 后系统分区表项 = 50GB（= NTFS 卷的真实大小），不是我塞的 64GB
     ——Windows 的"搜索分区"按【卷边界】建表项；克隆 50GB 卷进 64GB 分区会让它困惑。
     → 流水线规则：克隆时要么让卷=分区大小，要么表项=卷大小。
  2. bootrec 会重新生成磁盘签名（用户的 be5e933d）——签名本来就不该沿用。

### GPT→MBR 单变量实验：✅ 最小盘能启动（2026-10-18）
`reference/build-bios-min.sh` —— 128GB MBR 只有【系统保留100M(活动)+系统50G(=源卷)】两分区，
无恢复、无扩展，其余未分配。**SeaBIOS 60s/120s 截图均为 Windows 徽标**（自检 8/8 必需、0 警告）。
→ 证明：**GPT→MBR 跨格式纯克隆迁移这条路是通的**（bootmgr/BCD/几何/CHS/签名 全套配齐）。
→ 之前的蓝屏/彩条 = 恢复分区与 BCD 里残留的 recoverysequence 交互（下一步逐个加回变量定位）。
**第2步已完成（2026-10-18）**：在能启动的最小盘上【只加扩展分区+逻辑分区】（--extended=true，
不动恢复、不动克隆数据）→ sfdisk 认（W95 Ext'd + 逻辑）→ **SeaBIOS 60s/120s 仍为 Windows 徽标** ✅
→ 扩展分区【无罪】。
**第3步已完成**：+恢复分区(0x27，未格式化) → **SeaBIOS 120s 仍为 Windows 徽标** ✅（60s 是启动过渡黑屏）
→ 三个变量逐个加回全部无罪：扩展✗无罪、恢复分区✗无罪、最小盘✗无罪
→ 蓝屏元凶收窄到【恢复分区里的 winre.wim/boot.sdi 内容 + BCD 残留 recoverysequence 的组合】
待办第4步：格式化恢复分区并放入官方件 + RunOnce 脚本 → 若蓝屏复现即锁定元凶 → 拆 recoverysequence 解决

### 定论实验：Win10 引导项(BCD) 在 Win11 系统上不能用（2026-10-18 用户要求的单变量实验）
用修好全部 bug 的工具从零做盘（布局/CHS/EBR/几何/bootmgr=Win11 PCAT/签名 全套与能启动的
BUILD-FULL 一致），【唯一变量】= BCD 换回 Win10 时代参考件(bcd-reference-bios.bin,20480B)，
且盘签名特意对齐其内嵌值 2b93cd4c（BCD 设备对象带签名，6 处）。
结果：SeaBIOS 报错屏 —— Status **0xc000000d（无效参数）**
  （修工具前是 0xc000000e 找不到设备 → 修完 CHS 后错误码变了，说明工具修复真实生效；
   但 Win10 BCD 仍无法引导 Win11）
结论：**"Win10 引导项配 Win11"不兼容是真实的，不是工具 bug 的掩盖**。
工具侧规则（固化）：BCD 必须与目标系统同代 —— Win11 用 bcdboot 真值参考件(你手修盘提取) ✓ 能启动。
**补充实验（同盘外科手术，用户要求）**：在失败盘上只做两步 →
  ① 换 \Boot\BCD 为 Win11 参考件（仍报错）→ ② 盘签名同步为 Win11 BCD 内嵌值 be5e933d
  → **Windows 徽标 ✅ 同盘复活**
  → 定论：**BCD 与磁盘签名是【一个包】**，换 BCD 必须同步签名，不配套照样死；
  "BCD 必须与目标系统同代 + 签名必须与 BCD 内嵌值一致" 是一条完整规则（已固化进流水线）。
实验盘已删（一条命令可再生）。

### BCD 的 MBR 设备对象结构（2026-10-18 实测解析，为 imggenbcd 定制化铺路）
Win11 BCD 参考件实测：磁盘签名 `be5e933d` 出现【5 处】（非 6），每处结构 = `… 01 00 00 00 | 签名(4) | 00 00 00 00 …`
（前面 4 字节 01 00 00 00 = 设备类型标记；签名后是零填充）
**关键发现**：我们盘的系统分区起始 LBA 206848 在 BCD 里【出现 0 次】→ BCD 不存绝对偏移，
  引导项按【分区号】定位（他们的盘和我们的标准布局都是 SR=1/系统=2 → 号相同 → 同一份 BCD 通用 ✓
  这就是 BUILD-FULL 直接能用的原因）
**imggenbcd 定制化方案（下一轮实现）**：只需改写 5 处签名字节 → BCD 即为目标盘定制；
  分区号不同的布局再补改号字段。参考实现：garloff/Fix_BCD 的"读 BCD → 对账签名/分区 → 修正"算法。
Dism++ 源码挖掘（Chuyu-Team/Dism++）：下一轮用 gh 只读扒其引导修复实现。

### ★ 铁律（用户2026-10-18定）：卡住就上网搜，不要空想
遇到解决不了的问题 → 评估后【立刻联网搜索】（API 已配好，不用=浪费）。
证据：本轮"Windows 不认分区"和"BCD 命门"两个关键突破，都是联网搜出来的
（MS 文档的 CHS 规则、rakhesh 的 BCD 设备结构、BCD-SYS 的做法）——自己空想既慢又错。
工具优先级：手头工具（搜索/抓源码/gh）> 闭门推演。

### 任务清单（2026-10-18 定；2026-10-19 更新状态）
1. ✅ **imggenbcd 定制化（2026-10-19 完成）** —— BIOS/UEFI 修复的最后一块：
   - UEFI：按【旧 GUID → 新 GUID】全对象映射（参考件实测系统 3 处 / ESP 2 处 / 磁盘 5 处）；
     4 参模式（只给系统分区的 GUID）也会修 resume 对象，不再只改两个加载器。
   - BIOS：`boot.repair` 改为按 hive 结构遍历全部 type=6 设备对象，
     改【磁盘签名 + 分区字节偏移】，不再在整个文件里裸替换 4 字节签名
     （旧实现会漏掉 memtest/resume，还可能误伤恰好等于签名的数据）。
   - 回归：`mbr_bcd_test`（单元，含"其它字节未动/幂等/落盘重读"）
     + `boot-repair-test.sh`（集成 19 条）+ `genbcd-test.sh`（11 条）。
2~5. ⛔ **冻结（双盘）**：用户 2026-10-19 明确叫停。代码/档案在
   `backup/dual-disk-2026-10-19` 分支 + `DUAL-DISK-ARCHIVE.md`；
   **master 永久保持无双盘内容**，除非用户再次明确指令，否则不做。

### 剩余待办（都需要用户明确发话才动）
| # | 事项 | 状态/门槛 |
|---|---|---|
| 1 | App 侧 JNI 封装 + 文案表（boot.repair/winre.auto/GUID） | ✅ 2026-10-19 完成，见 `../app/`：C 桥 + JNI + 中英文案表 + 主机回归（C 9 条 / JNI 9 条） |
| 2 | WinRE 新方案真机验证（装盘后 `reagentc /info` 应 Enabled） | 需用户实测；沙箱无法验证 Windows 运行时行为 |
| 3 | 32 位（armhf）NTFS 路径实测 | 未做；需从 `third_party/ntfs-3g` 交叉编译（noble 源没有 armhf 的 libntfs-3g-dev） |
| 4 | ESP 写入性能（512B 簇 → 连续簇批量写，预计 30~70MB/s） | 用户定"只记录不优化"，需明确指令 |
| 5 | `recovery_bcd_test` 长期跳过（缺 `../build/w128/iso/bcd`） | 需从官方 ISO 重新提取该参考件，或改为自带参考件 |
| 6 | 双盘/多条目（BIOS+UEFI） | ⛔ 冻结，见上 |

### 2026-10-20 状态更新（本节以下清单已部分落地）
已落地：`imglayout --rec` 默认 0、`imgbc --expect-recovery`、
命令表补 6 条建盘命令 + `iso.ls`/`iso.extract`/`fat.export`（共 28 条）、
真进度（`img_cmd_set_progress`）、EBR/逻辑分区规范修正、命令表入口不建空镜像、
失败卡片日志行改成事实、WIM 解压期间喂看门狗。
仍未做：`winre.find` / `recovery.populate*` / `imgpart attr` 进表 / `img_hb` 打点 /
`winre.auto` 脚本来源标记。详见 `app/docs/工具能力与App集成手册.md` §18。

### 恢复分区/WinRE：默认关（2026-10-19 用户定，待实现）

契约见 `DESIGN-OUTPUT.md §十一`，App 流程见 `../app/docs/RECOVERY-FLOW.md`。

- [ ] `imglayout` 默认 `recovery_bytes=0`（显式 `--rec` 才建）
- [ ] `imgbc` 默认不把“无恢复分区”当警告；加 `--expect-recovery`
- [ ] 新增 `winre.find`（ESD/WIM；ISO 由 App 抽取后传入）
- [ ] 新增 `recovery.populate`（UEFI 独立分区：格式化/属性/写件/复验）
- [ ] 新增 `recovery.populate-system`（BIOS：写 `\Windows\System32\Recovery`）
- [ ] `winre.auto` 脚本加时间戳标记 + 捕获 enable/info 输出（便于确认执行）
- [ ] 错误码进 `E_TEXT` / App `i18n/strings.json` 并对账通过
- [ ] 正反用例：勾选/未勾选、ESD 无 WinRE、换 WIM 成功、取消、写失败
- [ ] 默认关的回归：不带 `--rec` 建盘 → 分区表无恢复分区、`imgbc` 不报红

### 回归入口（改完代码先跑它）
```bash
tests/run_all.sh      # 11 套：cmd · corrupt-chain · timeout · fd · thread · cancel ·
                      #   bcd · mbr_bcd · boot-repair(19) · genbcd(11) · recovery_bcd
                      #   wim-apply 为重型可选档（WITH_WIM=1）
```

### 0c 实测（人话行 + 失败卡片）

成功（`--progress=bar`，stderr 给人看）：
```
▶ 写入 FAT32  /EFI/s2.bin  5 字节
✔ put 完成（0 ms）
```
失败（同一命令、空间不够）：
```
════════ Icdqcow2 错误报告（整段复制即可）════════
工具     imgtool 2026.10.15 · Linux · 64-bit
命令     imgcmd fat.put
镜像     /tmp/fc/t.qcow2
失败位置 /EFI/big.bin
错误码   FAT-ENOSPC
原因     目标分区剩余 196 MB，要写入 286 MB
建议     换更小的文件，或先扩容该分区（imgresize）
日志     /tmp/fc/t.qcow2.imgtool.log
──────── 事件尾部（内存最近 N 条，机器格式）────────
{"t":5,"l":"E","m":"cmd","ev":"fail","rc":-10,...}
════════ 错误报告结束 ════════
```
同一份内容还会落盘成 `<镜像>.error.txt`（用户直接发这个文件即可）。

### 非法参数矩阵结果（回答"App 传参会不会炸"）

```
第一轮 NULL/空串/垃圾：90 条   修前 2 崩  →  修后 0 崩
第二轮 极端值（真实镜像）：40 条  修前 2 崩  →  修后 0 崩 · 0 毁数据
```
修掉的 4 处（都是真 bug，已提交）：
```
1. img_bcd_open_file(NULL)            NULL 直传 libhivex → 段错误
2. img_ev_emit(未 init 的结构)         段错误 → 加 magic 哨兵拒绝
3. img_qcow2_read_full(len 传错)       越界写调用方缓冲区 → 崩（改成先判范围、一字不写）
4. img_qcow2_expand(UINT64_MAX)       ★ 把镜像头部 size 改成 UINT64_MAX，
   img_qcow2_write(offset+len 回绕)      qemu-img 从此打不开（毁用户数据，比崩更糟）
```
复现命令：
```bash
# UBSan 版库（ASAN 在 proot 沙箱起不来：影子内存 mmap 受限）
cmake -B build-ub -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -g" \
      -DIMG_WITH_NTFS=ON -DIMG_BUILD_TOOLS=OFF -DIMG_BUILD_TESTS=OFF && cmake --build build-ub -j4
gcc -fsanitize=undefined -g -Iinclude -DIMG_HAVE_ZLIB=1 -DIMG_WITH_NTFS=1 -o /tmp/argfuzz \
    tests/argfuzz.c src/img_bcd.c build-ub/libimgtool.a -lntfs-3g -lwim -lz -lhivex
/tmp/argfuzz        # → 90 条 / 崩溃 0

# 第二个矩阵需要一张模板盘 + 工作目录（每用例复制一张独立盘再跑）
T=build/tools; W=/tmp/afz; rm -rf $W; mkdir -p $W
$T/imglayout mk $W/tpl.qcow2 $((20*1024*1024*1024)) gpt --esp 200 --msr 16
LBA=$($T/imgpart show $W/tpl.qcow2 | python3 -c "import sys,json;d=json.load(sys.stdin);print([p['first'] for p in d['parts'] if p['typeGuid'].startswith('c12a7328')][0])")
$T/imgmkfs $W/tpl.qcow2 --mkfs $LBA TEST
/tmp/argfuzz_img $W/tpl.qcow2 $LBA $W     # → 40 条 / 崩溃 0 / 用例盘全部通过 qemu-img check
```

### 尚未覆盖（诚实清单，别当成已经安全）
```
⬜ 并发：库用全局状态（img_event 的 G、ntfs ctx）→ 同一时刻只能一个操作；
    App 多线程同时调会互相踩。需要"句柄化 + 锁"或明确契约（未做）
⬜ fd 生命周期：App 提前关 fd / 传错 fd（传了别的文件）→ 未测
⬜ 32 位 Android（arm32，size_t 32 位）→ 未测（当前只在 64 位下验过）
⬜ 超时/卡死：底层读卡住无法中断（P0-2 取消接线后可缓解）
⬜ 内存峰值：img_fat32_put_file 整文件进内存（P1-4）
```

### BIOS Win10/Win11 实测（2026-10-20，含扩展/逻辑分区）

布局：SR 100MB(活动) + 系统 20/30GB + 扩展(0x0F) + 逻辑 NTFS ~4GB
（`part.layout --rec-gb=0 --extended=true`；全程只用工具施工）。

| 镜像 | 结果 | 证据 |
|---|---|---|
| Win10 BIOS（LTSC 19044） | ✅ SeaBIOS 到 Windows 徽标/点状转圈 | `build/bios-test/bd-04.png` |
| Win11 BIOS（ESD 28000） | ✅ SeaBIOS 到 Windows 徽标/圆弧转圈 | `build/bios-test/be-04.png` |
| 扩展分区 | ✅ MBR 0x0F → EBR 0x07 → 逻辑 NTFS 8MiB md5 一致 | 原始 MBR/EBR 解析 + `imgntfsfs export` |

**工具修复（本次，已提交）：**
1. `boot.repair` 的 VBR 必须写【活动/SR】分区，不是 OS 分区。
   原来写 OS → `imgbc` 报 `ntfs.code` 红；SeaBIOS 报 `A disk read error occurred`。
   新增 `--vbr-lba`（缺省=活动/SR）；结果带 `vbr_lba`。
2. `img_fixboot` / `boot.repair` 补 NTFS BPB 几何：
   `SPT=63 / heads=255 / hidden=lba`。mkntfs 留 0 → Windows 引导代码算 CHS 失败
   → SeaBIOS `A disk read error occurred`。结果带 `geometry_patched`。
3. Win10 必须用与 OS 同代的 `bootmgr`：LTSC `install.wim` 的
   `\Windows\Boot\PCAT\bootmgr` 可启动；Xlite 的 bootmgr 会卡在
   `Booting from Hard Disk...`（不是 MBR/VBR 问题）。
4. WinRE 按 BIOS 规则放系统分区 `\Windows\System32\Recovery`（不建独立恢复分区）；
   `SetupComplete.cmd` 开机 `reagentc /enable` 后自删。默认关策略不变。

产物（构建目录，不入库）：
`build/bios-test/Win10-BIOS.qcow2` / `Win11-BIOS.qcow2`。

### P2-5 · 已知性能点（用户决定：**只记录，不优化**）

| 场景 | 实测 | 说明 |
|---|---|---|
| 写入 19GB 数据分区（16KB 簇） | **74.9 MB/s** | 64MB = 4,096 次簇写入 |
| 写入 128MB ESP（512B 簇） | **6.3 MB/s** | 64MB = 131,072 次簇写入（32 倍）|
| 写入 415MB winre.wim（NTFS 路径） | **460 MB/s** | 1MiB 分块 + 大连续区间 |

**读缓冲调大的效果（实测，否决）**：1MB → 4MB → 16MB = 74.9 → 70.3 → 67.3 MB/s —— **变大反而略慢**（多搬内存），已恢复并保持 1MB。
结论：瓶颈不在缓冲，在"512B 簇 + 逐簇写"。

**影响范围**：只有 ESP 这一个 128MB 分区的写入慢（ESP 只装 34MB 启动文件，一次建盘约 10 秒）；
灌系统/写 winre.wim 走 NTFS，460MB/s 不受影响。

**两个候选方案（暂不实施）**：
- A：ESP 改 1KB/簇（128MB 下的合规上限，FAT32 要求 ≥65,525 簇）→ 写入次数减半，约 2×，FAT 表还省 1MB
- B：连续簇批量写（簇号连续就攒成一段一次写）→ 预计 30~70 MB/s，中等改动

**触发条件**（满足任一再做）：
1. App 端出现"往分区拷大文件"的高频场景且用户反馈慢
2. 用户明确说 ESP 建盘/写入速度不能接受
3. 需要重新动 FAT 写入路径时（那时顺手做 B，避免两次改同一条路径）

## 构建组合矩阵（2026-10-22 补记，交叉编译时暴露）
| IMG_WITH_NTFS3G | IMG_WITH_WIM | IMG_WITH_NTFS | 能否构建 | 说明 |
| --- | --- | --- | --- | --- |
| ON | ON | ON | ✅ | 主机默认，`run_all` 17/0/1 |
| ON | ON | OFF | ✅ | **本轮修好**（安卓端用这套：不把 GPL mkntfs 编进 APK） |
| ON | OFF | OFF | ❌ 已知 | `img_recovery.c:132`、`tools/imgwimls.c` 有未包住的 `wimlib_*` 调用 |
| OFF | OFF | OFF | ❌ 已知 | `imgrunonce`/`imgcmd`/`ntfs_dev_test` 引用被摘掉的 `img_ntfs_cb_*` |

> 另修掉两处 CMake 门控错误：`wim_apply_test`、`ntfs_probe` 的
> `target_link_libraries` 写在 `if()` 外层，关掉对应特性时"链接不存在的目标"直接报错。
> 这两个 ❌ 组合安卓端用不到，要补的话按"缺库降级"政策照 NTFS 那次的做法办即可（待用户发话）。

### 裸灌路径（wim.apply）的首次启动问题（2026-10-22，与上面的克隆路径区分开）

**先分清两条路**（这是本节的要害）：
- 上面 10-16~18 的结论都属于 **GPT→MBR 克隆已装好的系统**（源盘进过桌面 → 首次启动无需 specialize
  → 到 Windows 徽标即算成功）。那条路的配置（SR=100MiB 活动 **0x07**）已验证，不要动。
- 本节是 **`wim.apply` 裸灌 install.wim**：系统未 specialize，必须跑 Windows 自己的 setup/OOBE。

**已验证的引导链（三条都成立）**：Win10 x64 / Win11 x64 / Win7 x64+x86 全部进到 Windows 首次启动
（logo 或 specialize），SeaBIOS 无 `A disk read error`，`boot.check` 9/9。

**两个卡点（裸灌特有，与引导码/模板无关）**：
1. 盘符错位：SR 用 0x07 时，Windows 可能把 512MiB 的普通 NTFS 卷认成 C:，系统分区变 D:
   → Win7 报"没有确保计算机之间分配的驱动器号相匹配"。
   **同盘单字节对照**：SR 改 0x27 → 该错误消失，从"应用系统设置"推进到"为首次使用计算机做准备"。
   （0x27 确实是 Windows RE 的类型，但微软建 System Reserved 也用 `diskpart ... id=27`。）
   **结论：默认值仍保持 0x07；0x27 作为裸灌场景的候选，需参数化 + 两条路径分别回归后再改。**
2. setup 状态机：再往后弹"计算机意外地重新启动或遇到错误" → 需改 SYSTEM hive 的
   `SystemSetupInProgress` / `SetupType`（imagex/wimlib 裸灌 Win7 的标准收尾）。

**未做**：`win.finish`（写 setup 状态）、`img.create --sr-type=07|27`、裸灌路径跑进桌面。

### 裸灌路径打通到 OOBE（2026-10-22，Win7 32 位实测）

三关叠加，缺一不可（详见 `../docs/AGENT-MEMORY.md` 同名条目）：
1. **引导链**：MBR 码 + bootmgr/boot.sdi + BCD（按分区号定位）+ 内置 $Boot 引导码模板
2. **系统保留分区 0x27**：官方明文（`use type 0x27 … Do not use … 0x7`）；
   0x07 时 Win7 报"驱动器号不匹配"
3. **`win.finish`**：`SYSTEM\Setup\Status\ChildCompletion\setup.exe` 1→3
   （微软 Learn 官方修复），离线改 hive

实测：`e-win7x86-bios.qcow2`（Win7 旗舰版 32 位，wim.apply 裸灌）
→ QEMU 540 秒进入 **Windows 7 OOBE 界面**（"设置 Windows"，国家/地区/键盘布局）。
`win.finish --dry-run` 输出：`setup.exe=1, SetupType=2, SystemSetupInProgress=1` → 改 3 → 生效。

未做：Win10/Win11 的同样收尾验证（它们的 setup 状态机可能不同，Win10/11 用 Sysprep
的 `SetupType=0` + OOBE；需要各自 dry-run 看实际值）；官方 `bcdboot` 对齐。

### 裸灌三系统实测对照（2026-10-22）
- Win7 32 位：`win.finish` 改 `ChildCompletion\setup.exe` 1→3 → **OOBE** ✅
- Win10 x64：`setup.exe` 本来就是 3、`SystemSetupInProgress=0` → **不需要改**，直接进**桌面** ✅
  （含"网络发现"提示；540s，`-accel tcg,thread=multi`，CPU 975s 满载 = 只是慢）
- Win11 x64：无 ChildCompletion 节点、`SetupType=1` 仍在 specialize → `win.finish` 不适用，待查

### Win11 精简版 BIOS 尝试：失败，原因是平台限制（2026-10-22）
`c-win11-bios.qcow2`（26H1 28000.2525 ESD，idx=1）：
- 引导链 9/9、BCD 正确、0x27 已用，但 **540s 内两次运行 CPU 均 ~606s 且截图停在 logo**；
  `-no-reboot` 时 60s 内退出（重启循环）；`/Windows/Panther` 无 setup 日志 → 内核阶段失败。
- ★ 修正（同日，用户指出）：**Win11 本身 BIOS 是支持的**（BIOS 32/64 都已实测 ✅）。
  这一张失败是**该精简版（不忘初心 26H1 28000）的个例**，不能推广成"Win11 不支持 BIOS"。
  准确表述：同一份 ESD 的这个精简版在 BIOS/MBR 下 logo 后重启循环，原因未定位（可能是精简项/驱动）。
- 完整支持矩阵见 `../docs/AGENT-MEMORY.md`「Windows 引导支持矩阵」（含 32 位 UEFI = OVMF32 的用法）。

### 内置 NTFS 引导码模板的证据链（2026-10-22 固化）

三方互证，不是凭记忆：
1. **当时的脚本原文**：`reference/build-bios-full.sh:26`
   `$T/imgfixboot "$OUT" 2048 --tpl "/workspace/images/Windows 10 Xlite.qcow2" 2048`
   → 两天测试用的模板 = `images/Windows 10 Xlite.qcow2` 分区 2048 的 $Boot
2. **三方 sha256 全等**（引导码段）：`1d79bfa4ef4fbfa72806c4fe14fb19c8`
   = `reference/ntfs-vbr.bin` = `Windows 10 Xlite.qcow2@2048` = `diskB-Windows-BIOS.qcow2@2048`
   = w10b / c-win11 / d-win7 / e-win7x86 四张验证盘
3. **git blob 未变**：`9c8277ceed189fff24a3900469d200480ea87938`（58ab4a2 入库时 = 现在）

风险：源头是两个大文件（Xlite ≈10GB、diskB ≈11GB）。若被清理，可用
`git cat-file -p 9c8277ce…` 取回模板字节，但无法再复核“当时的模板盘”。
校验入口：`python3 tests/vbr-tpl-test.py`（数组==文件==写入盘，10 条断言）。

**存证（2026-10-22）**：`reference/ntfs-vbr-xlite-source.bin` 与 `reference/ntfs-vbr-diskb-source.bin`
是从那两个源盘抽出的 `$Boot` 前 10 扇区快照（各 5120 B）；`reference/ntfs-vbr-README.md`
写清三者关系、证据链与适用范围。`tests/vbr-tpl-test.py` 扩到 15 条断言：
数组 == 文件 == 两个源快照的引导码段 == 写入盘。
