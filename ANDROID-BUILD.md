# Android 交叉构建（aarch64 主机）

> 本文件与 `cmake/android-arm64host.cmake`、`android-cross.sh`、`android-deps.sh` 配套。
> 这些文件在 `main` 与 `android` 两个分支上一致；`android` 是给 App 出 `libimgtool.so` 的分支。

## 0. 一句话结论

Google **不提供 linux-arm64 宿主的 NDK**（只有 linux-x86_64 / darwin / windows）。
本机是 aarch64 Ubuntu，所以采用：**宿主原生 clang/lld（apt，ARM64）+ NDK 官方 Bionic
sysroot（头文件与库桩，权威）** 拼成交叉工具链。NDK 自带的 x86_64 二进制一个都不用跑。

| 部件 | 来源 | 位置 |
| --- | --- | --- |
| Bionic 头/库桩/compiler-rt | Android NDK r28c（28.2.13676358） | `$NDK_ROOT`（`android-cross.sh` 可用环境变量 `NDK` 覆盖） |
| 编译器/链接器 | apt `clang` `lld` 18.1.3（aarch64 原生） | `/usr/bin/clang`、`ld.lld` |
| SDK 平台/build-tools | sdkmanager（android-34/35、build-tools 34/35.0.0/35.0.1） | `$ANDROID_SDK` |
| aapt2/aidl/zipalign/split-select | 社区 arm64 构建 `Commit451/android-arm-build-tools` | 已替换进 `build-tools/35.0.1`，原生可执行 |
| Gradle | 8.11.1 | `$GRADLE_HOME` |
| adb/fastboot | apt 34.0.4（aarch64 原生；Google 的是 x86_64，不用） | `/usr/bin/adb` |

## 1. 用法

```bash
# 依赖（一次性；zlib 来自 NDK，libxml2→wimlib→ntfs-3g→hivex）
API=28 ./android-deps.sh arm64-v8a armeabi-v7a x86_64
# 主库（三个 ABI 全量功能）
API=28 ./android-cross.sh
```

`android-cross.sh` 会自动把 NDK 的 `libclang_rt.builtins-*-android.a` 链进
`clang -print-resource-dir`，否则 Ubuntu clang 链接时找不存在的 `libgcc`。

## 2. 五个必须知道的坑

1. **`-rtlib=compiler-rt` 只能放在链接期**。放编译参数里 clang 会刷一屏
   "argument unused during compilation"。
2. **NDK 的库不在标准 `lib/` 布局**（在 `usr/lib/<triple>/<api>/`），`find_package`
   命中不了 → 每个 ABI 建一个标准布局的 stage 前缀
   `build-android/stage/<abi>/{include,lib}`，zlib 与交叉回来的第三方库都装这里。
   静态库要进 `libimgtool.so`，工具链里必须给 `-fPIC`。
3. **minSdk 实际门槛是 API 28**：Bionic 的 `glob()/globfree()` 是 API 28 引入，
   而 wimlib 无条件使用（split reference 路径）。若必须支持 Android 7/8，
   只能自带 glob 实现或砍掉该特性。
4. **gnulib 的 `full_read/full_write` 与 wimlib 同名符号冲突**。主机上各自进 .so
   看不出来，安卓端静态合入一个 .so 就 `duplicate symbol` → hivex 编译期
   `-Dfull_read=hivex_gnulib_full_read` 改名，从根上避免。
5. **ntfs-3g/hivex 只编库**：挂载程序要 libfuse（安卓没有），hivex 的 `images/`
   会在构建期*运行*交叉编译出来的 mklarge（交叉编译必炸）。所以
   `make -C libntfs-3g` / `make -C gnulib/lib lib` 只出库，hivex 还要把
   `libgnu.a` 用 `ar -M` 合并进 `libhivex.a`。

## 3. 产物体检口径

每个 `libimgtool.so` 都要满足：

- `readelf -h`：Class/Machine 与 ABI 相符；
- `readelf -lW | awk '/LOAD/{print $NF}'` 全部 `0x4000` → **16 KiB 页对齐**（Android 15+ 要求）；
- `readelf -d` 的 NEEDED 只有 `libc.so/libdl.so`（+ 动态依赖时 `libz.so`），**不得出现 `libc.so.6` 或 `ld-linux-*`**（那说明混进宿主 glibc）；
- `nm -D | grep GLIBC_` 为 0；
- `wim_apply/ntfs_* /hivex_open` 等符号存在 → NTFS/WIM 功能没被降级掉。

## 4. AGP 注意

AGP 默认从 Maven 取 x86_64 的 aapt2，arm64 主机必须写

```properties
android.aapt2FromMavenOverride=<ANDROID_SDK>/build-tools/35.0.1/aapt2
```

原生 `ndkVersion` 走不通（NDK 宿主二进制是 x86_64）：APK 里的 .so 用本脚本预编译，
以 `jniLibs/<abi>/` 形式打进包里，不要让 Gradle 去调 NDK 的 CMake toolchain。

## 5. 真机（Android 15+ / 4 KiB 页）实测出的两个坑

跑法：`adb push` 交叉出来的 `bin/*` 到 `/data/local/tmp/img/`，`LD_LIBRARY_PATH=.` 直接执行。
同一批测试在主机 17/17 全绿，真机却挂了 6 个 —— 两个原因都是**主机环境才有、安卓没有的东西**：

### 坑 A：库内写死 `/tmp`（Android 没有 /tmp）
`img_bcd.c` 的 `img_bcd_open_mem/img_bcd_save_mem` 与 `img_cmds.c` 的 `bcd.create`
用 `mkstemp("/tmp/...")` 建临时文件 → 安卓上必失败，表现是 `bcd.create` 报打不开。
修法是新增 `img_tmpdir()`，按 `IMGTOOL_TMPDIR → TMPDIR → /tmp` 取第一个真正可写的；
全不可写返回 NULL 并出 `TMPDIR-UNAVAILABLE`。
**App 侧必须把 `IMGTOOL_TMPDIR` 指到自己的私有目录**（`filesDir/tooltmp`），
不要用 `cacheDir`：Android 在存储紧张时会自动清空 `cacheDir`，长任务写到一半会丢文件。

### 坑 B：Bionic 的 iconv 不支持 LATIN1（★ 影响最大）
hivex 解节点名走 `iconv(3)`；BCD hive 里的节点名是 **Latin-1 压缩存储**
（实测根节点名字节 `0c 00 00 00 4e 65 77 ...` = "NewStore…"），于是走
`iconv_open("UTF-8","LATIN1")`。真机实测 Bionic 只认 UTF-16LE/UTF-8/ASCII：

| iconv_open("UTF-8" ← ) | glibc（主机） | Bionic（真机） |
| --- | --- | --- |
| UTF-16LE | ✔ | ✔ |
| LATIN1 | ✔ | **✘ EINVAL** |
| ISO-8859-1 / UTF-16 / UCS-2LE | ✔ | **✘ EINVAL** |

结果：`hivex_open` 成功、子树数量都对，但 **`hivex_node_name()` 一律返回 NULL**
→ 所有按名字的 BCD 查找失败 → `bcd.create` 报「模板里未找到 Windows 加载器对象」，
`bcd_test`/`bcdcreate_test`/`mbr_bcd_test` 全挂。glibc 支持 LATIN1，所以主机永远测不出来。

**修法**：交叉编译 GNU **libiconv 1.18**（静态），在 `build_hivex` 里用 `ar -M` 和
`libgnu.a` 一起合并进 `stage/<abi>/lib/libhivex.a`。合并后 `libimgtool.so` 里
`iconv*` 全部就地定义（未解析 0 个），不再依赖系统 iconv。
代价：`libimgtool.so` 从 1.22 MB 涨到 **2.20 MB**（iconv 编码表）。
若嫌大，可改为给 hivex 打补丁自己做 Latin-1→UTF-8（十几行），能省回约 1 MB —— 待拍板。

### 修完的真机成绩单
`bcdcreate_test bootplan_test identify_test qcscan_test ebr_test timeout_test
fd_test thread_test cancel_test bcd_test mbr_bcd_test` → **11/11 全过**；
CLI `imgcmd bcd.create --img=W/a.qcow2` 在真机上成功往 GPT 盘 ESP 写出 36 864 字节的 BCD。

> 另记：toybox `sh` 的 `$((20*1024*1024*1024))` 会溢出 32 位，建出 0 字节的盘，
> 导致整轮测试级联失败 —— 在安卓 shell 里写脚本要用字面量字节数。

## 6. App 沙箱内的端到端实测（2026-10-22，真机）

探针 APK（`.probe/apk`）在 App 进程里依次执行，全部成功：

| 步 | 命令 | 结果 |
| --- | --- | --- |
| 1 | `System.loadLibrary("imgtool"/"imgapp")` | 加载成功（16 KiB 对齐的包被 `zipalign -c -P 16` 判通过） |
| 2 | `Native.setTmpdir(cacheDir)` | `true` |
| 3 | `Native.version()` / `list()` | `imgtool 2026.10.15` / 命令表 17 750 字符 |
| 4 | `img.create` 2 GiB GPT | ok，3 个分区 |
| 5 | `img.info` | ok |
| 6 | `fs.format` ESP→FAT32 | ok，簇 512B × 403 168 |
| 7 | **`bcd.create`** | **ok**，写出 36 864 B 到 `/EFI/Microsoft/Boot/BCD` |
| 8 | `boot.check` | ok，必需项从 3/8 升到 5/8，中英摘要正常 |

第 7 步是整条链的集中验证点：临时文件（`img_tmpdir`）+ hivex 读写 + Latin-1 iconv +
FAT32 写入，四样在 App 沙箱里同时成立。

装包注意：部分 OEM 系统（如 HyperOS）需要用
`pm install -i com.google.android.packageinstaller` 指定安装来源。

## 7. 打开 NTFS 格式化 + 端到端灌系统（2026-10-22 用户拍板"开源，GPL 可以带"）

`IMG_WITH_NTFS=ON` 需要**交叉出来的 mkntfs 目标文件**（不能用宿主 .o）。做法在
`android-deps.sh: build_mkntfs`：

- 源文件：`ntfsprogs/{attrdef,boot,sd,utils,mkntfs}.c` → `mkntfs-*.o`
  （mkntfs.c 输出成 `mkntfs-main.o`），另需 `ntfscp.o/ntfsls.o/ntfscat.o` 给工具用
- ★ 每个上游 `main` 都要 `llvm-objcopy --redefine-sym main=<名>_main`，否则抢入口
- ★ 编译时要 `-DHAVE_FFS=1`：ntfs-3g 的 `compat.h` 未定义它会 `extern int ffs(int)`，
  而 Bionic 的 `strings.h` 把 ffs 写成 static inline → **32 位安卓**报
  "static declaration follows non-static"（arm64 不报，所以只在 v7a 上暴露）
- ★ `build_ntfs3g` 必须把 `config.h` 装进 `stage/<abi>/include/ntfs-3g/`：
  上游 `types.h` 里有 `#include "config.h"`，缺了 mkntfs 编不过
- ★ wimlib 必须**带 ntfs-3g 支持**（`--with-ntfs-3g` + 覆盖 `LIBNTFS_3G_CFLAGS/LIBS`）。
  之前图省事写了 `--without-ntfs-3g`，交叉出的 libwim 不支持 `WIMLIB_EXTRACT_FLAG_NTFS`，
  真机 `wim.apply` 9 毫秒就报"底层读写失败"

### 真机端到端（Android 15+ / arm64-v8a，全新 20 GiB 盘）

| 步 | 命令 | 结果 |
| --- | --- | --- |
| 1 | `img.create --size-gb=20 --table=gpt` | ✔ 170 ms |
| 2 | `fs.format --lba=2048 --fs=fat32` | ✔ 135 ms |
| 3 | `fs.format --lba=444416 --fs=ntfs` | ✔ **369 ms**（以前 FEATURE-UNAVAILABLE） |
| 4 | `wim.apply --wim=install.wim --index=1` | ✔ **65.8 s**，解出 8.95 GB |
| 5 | `win.version --lba=444416` | ✔ 读出注册表 19044 / 内核 19041.1288 / **PE 头 0x14C=x86** |
| 6 | `bcd.create` | ✘ `BOOT-FORMAT-UNSUPPORTED` —— **政策正确拦截**：32 位系统不能 UEFI/GPT 启动 |
| 7 | `boot.check` | ✔ |

> 顺带纠正一个素材事实：那张 `19044.1288.211006-0501.21h2_x64.iso` **其实是 32 位**
> （WIM XML `<ARCH>=0` 与内核 PE 头 0x14C 两种独立方法一致），文件名里的 x64 是错的。
> 所以 `bcd.create` 在 GPT 上被拦下不是 bug。64 位盘的正向路径此前已在主机 QEMU 里
> 反复验证过；安卓端是同一条代码路径，未额外灌一次 64 位系统（要再推 ~9 GB 素材）。

## 8. 看门狗超时跨命令残留（真机严重 bug，已修）

`g_io_timed_out` 是进程级标志，`img_io_read/write` 一进来就据此 fail-fast，
但它只在 `img_io_watchdog_start/stop` 里复位 → **一次 30 秒无进展之后，同一进程里
所有后续 I/O 永久失败**，上层表现为"所有镜像都不是合法 qcow2"，只有重启进程才好。
CLI 一命令一进程，主机永远测不出来。修法：`img_cmd_exec()` 开头
`img_io_timeout_reset()`；并把这类失败如实报成 `IO-TIMEOUT` 而不是格式错误。
回归用例：`timeout_test` 第 ⑤ 组。
