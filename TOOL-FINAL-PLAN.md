# 工具端收尾计划（App 开工前）— 2026-10-24

> 目的：在开始写 App 之前，把工具端**该做的做完、不该做的写死**。
> 本文是压缩上下文后的**唯一接手入口**：关键路径、提交号、测试数字、决策都在这里。
>
> 读者：接手 App/工具的实现者（人或 AI）。配套：
> `docs/UI设计-2026-10-24.md`、`docs/工具总结-2026-10-23.md`、
> `docs/接手必读-2026-10-23.md`、`Icdqcow2/docs/工具能力与App集成手册.md`。

---

## 0. 结论先说

1. **App 可以开工**：文件页（浏览/复制/剪切/重命名/删除）与全流程装机所需命令都齐了，
   且已在真 Windows 上用 `certutil` 对过 9 个文件的 SHA256（全部一致）。
2. 但文件页开工前，建议先补 **2 个 P0 小口子**：
   - FAT 只改大小写的重命名（现在会被拒）；
   - Windows 非法文件名校验（`\ / : * ? " < > |`、保留名、尾点/尾空格）。
3. 你问的“第四条”（明确不做/以后再做）里，**没有一个是文件操作 MVP 必需的**；
   逐项判定见 §3。
4. P0 完成后即可开始 App；P1 可以边做 App 边补，不阻塞。

---

## 1. 当前状态（压缩上下文必读）

### 1.1 仓库与分支

| 仓库 | 分支 | HEAD | 说明 |
|---|---|---|---|
| `imgtool` | `master` | `f1449c1` | 工具主库；WIP 分支保留作备份 |
| `imgtool` | `wip/unified-paths-20261024` | `1959db2` | 合并前的现场（备份，不要删） |
| `app` | `master` | `1c8d12b` | JNI/文档/i18n；无正式 UI |
| `app` | `wip/unified-paths-20261024` | `30a1784` | i18n/文档备份 |
| `docs` | `master` | `126db51` | 文档仓（本文提交前的 HEAD；本文提交后以 git log 为准） |
| `imgtool-android` | `android-compat` | `786bf09` | Android worktree，已同步 master |

四个仓库当前都**干净**（`git status` 无输出）。

### 1.2 命令与契约

- 命令表 **47 条**；文件操作只有 4 条：**`copy` / `move` / `rename` / `rm`**。
- 路径语法（`src/img_path.c`）：

```text
<宿主路径>[!<选择器>:<容器内路径>]*
/storage/.../Windows.qcow2!p3:/Users/a.txt
/storage/.../Windows.qcow2!lba165888:/a.txt
/storage/.../Win11.iso!:/bootmgr
/storage/.../Win11.iso!:/sources/install.esd!1:/Windows/System32/cmd.exe
/storage/.../boot.wim!2:/Windows/...
/storage/.../a.txt
```

- `!` = 容器边界（宿主里真 `!` 写 `\!`）；`pN` = 第 N 个已用分区；
  `lbaN` = 显式 LBA；ISO 选择器为空；WIM 选择器是映像索引。
- 类型按**魔数**判断；容器内 `/`，`\` 自动归一；禁止 `..`。
- ISO/WIM/ESD 是只读链：可作 `copy` 源；作目标或 `move`/`rm` 源报 `PATH-READONLY`。
- 旧命令 `ntfs.copy` / `fat.copy` / `file.copy` / `ntfs.move` / `fat.move` /
  `ntfs.rm` / `fat.rm` / `ntfs.rename` 已从命令表删除，CLI 报 `CMD-NOTFOUND`。

### 1.3 测试数字（2026-10-24 收盘）

| 项 | 结果 |
|---|---|
| `run_all` | **26 通过 / 0 失败 / 2 跳过** |
| `DEGRADED=1` | **27 通过 / 0 失败 / 1 跳过** |
| `fileops-test`（统一路径 + 四命令 + 名字规则） | **71 / 0 / 1 跳过** |
| `cmd_test` | **87 / 0** |
| `copy-test` | 60 / 0 |
| `xcopy-test` | 58 / 0 |
| `ntfs-move-test` | 80 / 0 |
| `fat-fs-test` | 48 / 0 |
| `ads-test` | 14 / 0 |
| `ntfs-inventory-test` | 26 / 0 |
| i18n 对账 | **47 命令 / 99 参数 / 80 错误码**，退 0 |

### 1.4 真机 / APK

- 三 ABI 交叉编译：零警告，**836** 导出符号，`LOAD` 16 KiB 对齐，
  NEEDED 只有 `libc.so` / `libdl.so`。
- APK `Icdqcow2/testbench`：**9,292,280 B**，sha1
  `487ca3af717f273a91417a5e6f2ca73c46934819`；APK 内 `libimgtool.so`
  sha1 `4b6ad78d7e642f3c6c68accea0e2d282e4613fee`。
- 真机（Redmi / Android 17 / arm64-v8a）跑过 12 项：copy 同卷/跨镜像/ISO、
  move 同卷/跨镜像、rename、rm、宿主删源许可、ADS、中文名、旧命令 `CMD-NOTFOUND`。

### 1.5 真 Windows 证据

- 机器：`Windows X-Lite`，`10.0.19045.5968`，用户 `admin`，卷序列号 `2236-A3D9`
  （`p37757s36220748/Windows.qcow2`，NTFS 在 `lba=104448`）。
- 桌面目标：`C:\Users\Admin\Desktop\Icdqcow2-Test`。
- 用 `certutil -hashfile ... SHA256` 对过 9 个文件，全部与源一致：
  `bootmgr` / `bcd` / `boot.sdi`（ISO）、`cmd.exe` / `cmd.exe.mui`（ESD）、
  `hosts`（Windows 自身）、两个 `.md` + 一张 `.jpg`（手机）。
- 脚本 `Icdqcow2-Check.bat` 生成的完整结果：
  `C:\Users\Admin\Desktop\Icdqcow2-RESULT.txt`（5,841 B）。
- 说明：`rmef9a42ya20748/Windows 11.qcow2` 里**没有 Admin 用户**，只有
  `Default/Public`；Admin 在 `p37757s36220748/Windows.qcow2`。两张盘都写了测试文件
  （Windows 11 盘写在 `C:\Users\Public\Desktop\Icdqcow2-Test`）。

### 1.6 手机上的关键路径

| 用途 | 路径 |
|---|---|
| Windows X-Lite（有 Admin） | `/storage/emulated/0/Documents/VectrasVM/roms/p37757s36220748/Windows.qcow2`（LBA 104448） |
| Windows 11 | `/storage/emulated/0/Documents/VectrasVM/roms/rmef9a42ya20748/Windows 11.qcow2`（LBA 444416） |
| 安装 ISO | `/storage/emulated/0/Windows11_Client_x64_zh-cn_26300_9457.iso` |
| 精简 ESD | `/storage/emulated/0/All PC/镜像/【不忘初心】Windows11_26H1_28000.2525_X64_纯净[深度精简版][1.74G](2026.7.20).esd`（索引 1/2） |
| 设备测试二进制 | `/data/local/tmp/imgtest/{imgcmd,libimgtool.so,ads_probe}` |
| 真机测试产物 | `/storage/emulated/0/Icdqcow2-FromAdminVM/`、`.../Icdqcow2-FromVM/`、`.../Desktop/Icdqcow2-2026-10-24/` |

---

## 2. P0：App 文件页开工前必须做完

### P0-1 FAT 只改大小写的重命名

**现状**：`img_fat32_rename()` 里

```c
if(sdir==ddir && !strcasecmp(sname,dname)) return IMG_EINVAL;   /* 自己改自己 */
```

把 `a.txt → A.txt` 当成“自己改自己”拒绝，文件页重命名大小写会报 `PATH-BAD`。

**要做的**：
- 同目录、`strcasecmp` 相等但 `strcmp` 不等 ⇒ 视为 case-only rename。
- 实现：先改到临时名（同目录、临时名探测不存在），再改到目标名；
  第二步失败回滚回原名。目录也适用。
- `img_xmove_run` 的 `case_only` 判定从“仅 NTFS”扩展到 FAT；
  调用 `img_fat32_rename`，返回 `case_only=true`。
- 验收：`move` / `rename` 在 FAT 上 `a.txt → A.txt` 成功，源不在、目标在，
  内容/时间不变；`run_all` 不回归。

### P0-2 Windows 非法文件名校验

**现状**：NTFS/FAT 写入前不校验；从手机复制 `a:b.txt`、`CON`、`a.`、`a ` 这类名字
会写进盘，Windows 可能打不开/看不见。

**要做的**：
- 新增校验函数（建议放 `src/img_xcopy.c` 或 `img_path.c`）：
  - 禁止字符：`< > : " / \ | ? *` 和 ASCII 0x00–0x1F；
  - 禁止尾随 `.` / ` `（Windows 会去掉，导致名字对不上）；
  - 保留设备名（不区分大小写，含带扩展名）：`CON PRN AUX NUL COM1..COM9 LPT1..LPT9`；
  - 空名字、`.`、`..`。
- 适用对象：`copy` / `move` 的目标文件名、`rename` 的新名字、目录名。
- 只对 **NTFS/FAT 目标**校验；宿主目标按宿主规则（ext4 可用 `:` 等）。
- 失败码：新增稳定码 **`NAME-INVALID`**（或 `COPY-NAME-INVALID`），
  文案中英写入 `Icdqcow2/i18n/strings.json`，并在 `check_i18n.py` 的 EXTRA 里登记。
- **必须在任何写动作之前拒绝**（零副作用），测试要断言盘上没多出东西。
- 验收：新增 `tests/name-test.py`（或并入 `fileops-test`）：
  `a:b.txt` / `CON` / `a.` / `a ` / `a?b` 复制/改名到 NTFS、FAT 都报 `NAME-INVALID`；
  同样名字写宿主（ext4）允许；正常中文/空格/长名字不受影响。

### P0 完成情况（2026-10-24 收盘 ✅）

| 项 | 结果 | 证据 |
|---|---|---|
| P0-1 FAT 大小写改名 | ✅ | `img_fat32_rename` 临时名→目标名（失败回滚）；`case_only` 判定扩到 FAT；目录同样支持 |
| 目录大小写改名误判 | ✅ 修掉 | 以前 `rename /Dir1 DIR1` 被拼成 `/Dir1/DIR1` 报 `COPY-INTO-SELF`；现在 ci 相等不走 `resolve_final` |
| `actual_case_path` 老 bug | ✅ 修掉 | 回调结果被拼装覆盖，实际一直用用户输入的大小写；现在 NTFS/FAT 都换成盘上真实大小写 |
| P0-2 非法文件名校验 | ✅ | 新增 `src/img_name.c`；`copy`/`move`/`rename` 写前校验；`NAME-INVALID` |
| 测试 | ✅ | `fileops-test` **71/0/1**（原 44）、`xcopy-test` **58/0**；`run_all` **26/0/2**；`DEGRADED` **27/0/1** |
| 真机 | ✅ 19 项 | Redmi / Android 17 / arm64-v8a：FAT 文件+目录大小写改名、内容不变、7 个非法名拒绝且条目数不变、host 非法名拒绝、中文名正常 |
| 三 ABI | ✅ 零警告 | 836 导出符号，`LOAD 0x4000`，NEEDED 仍只有 `libc.so`/`libdl.so` |

> APK 尚未重打包：P0 只影响文件页行为，装机用的 testbench APK 仍是旧 `libimgtool.so`
> （真机验收走的是 `/data/local/tmp/imgtest` 里的新二进制）。App 开工时一并重打包。

### P0 完成定义

- `run_all` 全绿（允许已知跳过）；
- `DEGRADED=1` 全绿；
- i18n 对账通过；
- 三 ABI 零警告；
- 真机跑一遍 FAT 大小写改名 + 非法名拒绝；
- 文档同步（UI 规格错误码表、手册、SUMMARY、HANDOFF、AGENT-MEMORY）。

---

## 3. 第四条逐项判定（你要的“有没有我们需要的东西”）

| 项 | 文件操作 MVP 需要吗 | 判定 |
|---|---|---|
| exFAT、ext4 | ❌ 不需要 | 只承诺 NTFS + FAT32；以后要做再开任务 |
| ISO/UDF/WIM 写入 | ❌ 不需要 | 设计就是只读安装源；永远不做 |
| BitLocker/加密卷 | ❌ 不需要 | 不碰加密卷 |
| qcow2 zstd 压缩 | ❌ 不需要 | 默认拒绝；遇到 zstd 镜像是极小概率 |
| qcow2 backing file | ❌ 不需要 | Vectras 镜像不用 backing |
| qcow2 快照 | ❌ 不需要 | 不处理快照 |
| `$EA` 扩展属性 | ❌ 不需要 | 普通文件几乎没有；ADS 已支持 |
| 压缩/稀疏属性 | ❌ 不需要 | 复制成普通文件；可能变大，可接受 |
| ACL/所有者 | ❌ 不需要 | 新对象继承目标父目录 ACL 已够 |
| 符号链接 | ❌ 不需要 | 宿主符号链接跳过并计数 |
| 硬链接 | ❌ 不需要 | 复制成两份数据；同卷 move 保留 inode |
| NTFS 8.3 短名 | ❌ 不需要 | 交给 Windows 自己补 |
| 4Kn 扇区 | ❌ 不需要 | 现有镜像都是 512e |
| 分区扩容 | ❌ 不需要 | 不是文件操作；需要更大盘就 clone + 重建 |
| 按卷标/GUID 选分区 | ❌ 不需要 | `pN` / `lbaN` 已够 |
| VM 运行检测/文件锁 | ❌ 不需要 | App 要求关机 + 每镜像串行队列 |
| undo/审计/事务回滚 | ❌ 不需要 | UI 二次确认；失败保留已写部分 |

**需要额外注意的两个“边角”**（不是第四条里的，但属于文件操作）：

1. **目录 ADS**：现在只复制文件 ADS；目录 ADS 极少见，不做，文档写清。
2. **Windows MAX_PATH 260**：NTFS 允许长路径，但 Explorer 可能打不开；App 可提示，工具不截断。

---

## 4. P1：不阻塞 App，可以边做边补

| 项 | 价值 | 量级 |
|---|---|---|
| `copy --verify=hash` | 复制后内容校验（现在只校验大小/inode） | 中 |
| `fat.ls --after` 游标 | FAT 目录增量刷新（现在全量重拉） | 低 |
| 目录复制整体进度/总数 | UI 显示确定进度（现在逐文件/不确定） | 中 |
| 断点续传 | 大文件中断后不用重来 | 中 |
| `file.stat` / `file.hash` | 通用路径信息/哈希，少调几条命令 | 低 |
| 目录 ADS 复制 | 极少数场景 | 低 |
| `copy --dry-run` / 目录空间预估 | UI 提前提示空间不足 | 中 |

---

## 5. App 开工所需的工具契约（已就绪）

### 5.1 四命令

```bash
copy   "<src>" "<dst>" [--conflict=fail|overwrite|rename] [--list_file=...]
move   "<src>" "<dst>" [--allow_host_delete=true]
rename "<path>" "<新名字>"
rm     "<path>" [--recursive=true] [--allow_host_delete=true]
```

- 粘贴 = 目标是目录（尾斜杠）的 `copy`/`move`。
- 跨卷 `move` 会删源；宿主源必须 `allow_host_delete=true`（App 弹窗后再传）。
- 失败：复制成功但删源失败 ⇒ `MOVE-PARTIAL`，两份都保留，不重试。
- 拒绝：同文件 `COPY-SAME-FILE`；目标在源子树 `COPY-INTO-SELF`；
  只读链 `PATH-READONLY`；根目录 `RM-ROOT-REFUSED`。

### 5.2 返回字段

- `copy`：`copied_files/copied_dirs/bytes/skipped/ads/size/ino/path/replaced/parent_total/dst_path/parent/mode`
- `move`：`mode=rename|move|cross_volume/files/dirs/bytes/ads/deleted/noop/case_only/dst_path`
- `rm`：`removed/deleted`
- 批量：`copied|moved/failed/bytes/truncated/items[]`（每项 `ok/files/dirs/bytes/path|code`）

### 5.3 App 侧必须做的

- **每镜像串行队列**（进度是进程级单例；建议先全局串行）。
- **SAF 只能读**；写盘要真实路径 + `MANAGE_EXTERNAL_STORAGE`。
- 错误文案按 `error_code` 查 `Icdqcow2/i18n/strings.json`（47/99/80）。
- 冲突对话框：跳过 / 保留两者（`conflict=rename`）/ 替换（二次确认）。
- 删除/跨卷移动/宿主删源都要二次确认。
- 大目录复制用**不确定进度**（无总字节），显示已处理文件数 + 当前项。

---

## 6. 实施顺序（P0 做完 → App）

1. ✅ **T7-P0-1** FAT 大小写改名（含 `img_xmove_run` case_only）—— `imgtool` `66fb940`
2. ✅ **T7-P0-2** 非法文件名校验 + `NAME-INVALID` + i18n + 测试 —— 同一个提交
3. ✅ 跑 `run_all`（26/0/2）/ `DEGRADED`（27/0/1）/ 三 ABI（零警告）/ 真机 19 项。
4. ✅ 同步文档（UI 规格错误码、手册、SUMMARY、HANDOFF、AGENT-MEMORY）。
5. ⏭ 合并 `imgtool-android`（已 merge）与 APK 重打包（App 开工时一起做）。
6. ▶ **下一步：开始 App**：Compose 壳 → 文件页 → 全流程装机 → 控制台/设置。

> P1 不与 App 抢跑；App 文件页先按“不确定进度 + 失败提示”做。

---

## 7. 构建 / 测试 / 真机命令（新会话照抄）

### 7.1 桌面

```bash
cd /workspace/imgtool/build && make -j8            # 零警告
cd /workspace/imgtool && tests/run_all.sh          # 26/0/2
cd /workspace/imgtool && DEGRADED=1 tests/run_all.sh  # 27/0/1
```

### 7.2 Android

```bash
cd /workspace/imgtool-android
API=28 TOOLS=1 TESTS=1 ./android-cross.sh arm64-v8a
API=28 TOOLS=1 ./android-cross.sh                  # 三 ABI
```

APK：

```bash
for abi in arm64-v8a armeabi-v7a x86_64; do
  cmake -S /workspace/Icdqcow2/jni -B /workspace/build-android/jni-$abi \
    -DCMAKE_TOOLCHAIN_FILE=/workspace/imgtool-android/cmake/android-arm64host.cmake \
    -DNDK_ROOT=/workspace/android-ndk/android-ndk-r28c -DIMG_ANDROID_ABI=$abi -DIMG_ANDROID_API=28 \
    -DIMG_ANDROID_STAGE=/workspace/build-android/stage/$abi \
    -DIMGTOOL_ROOT=/workspace/imgtool-android -DIMGTOOL_LIB=/workspace/build-android/$abi/libimgtool.so \
    -DIMGAPP_BUILD_TEST=OFF
  cmake --build /workspace/build-android/jni-$abi -j8
  cp /workspace/build-android/$abi/libimgtool.so   /workspace/Icdqcow2/testbench/app/src/main/jniLibs/$abi/
  cp /workspace/build-android/jni-$abi/libimgapp.so /workspace/Icdqcow2/testbench/app/src/main/jniLibs/$abi/
done
cd /workspace/Icdqcow2/testbench && /workspace/gradle/gradle-8.11.1/bin/gradle :app:assembleDebug --no-daemon
```

### 7.3 adb（每次调用都会被回收）

```bash
adb kill-server; adb start-server; sleep 3
adb get-state    # 可能是空串；用 adb shell true 再判
adb shell true
```

设备测试目录：`/data/local/tmp/imgtest`，用 `LD_LIBRARY_PATH=.` 跑 `./imgcmd`。

---

## 8. 文档索引

| 文档 | 内容 |
|---|---|
| `docs/UI设计-2026-10-24.md` | App UI 完整规格（页面/状态机/刷新/错误码/线框） |
| `docs/工具端收尾计划-2026-10-24.md` | 本文（App 开工前工具收尾） |
| `docs/工具总结-2026-10-23.md` | 能力地图/契约/限制/测试数字 |
| `docs/接手必读-2026-10-23.md` | 历史决策、坑、真机证据 |
| `docs/NTFS移动改名-规范与实现对照.md` | MS-FSA 对照 |
| `Icdqcow2/docs/工具能力与App集成手册.md` | 逐命令/错误码/§21.11.13 新契约 |
| `Icdqcow2/i18n/strings.json` | 47 命令/99 参数/80 错误码中英文案 |

---

## 9. 一句话交接

工具端**主干完成**；先做 P0-1（FAT 大小写改名）与 P0-2（非法文件名）两个小补丁，
然后就可以开 App。第四条里的东西对“文件操作 App”都不是必需品，已逐项写死为不做。