# imgtool

**不挂载、不开机，直接改 qcow2 镜像里的东西。**

`imgtool` 是一个用 C11 写的库 + 命令行工具（`imgcmd`）。它把一个 `.qcow2` 文件
当成一块裸磁盘来读写：分区表、FAT32 / NTFS 文件、WIM/ESD 灌装、ISO 与压缩包、
Windows 引导（MBR 码 / VBR 码 / BCD / bootmgr / bootmgfw.efi / WinRE）。

典型场景是**手机上给虚拟机装系统**：没有 root、没有 loop 设备、没有 KVM、
没法把镜像挂载出来，只能在用户态按扇区改字节。改完的镜像直接丢给 QEMU / Vectras 启动。

> 相关仓库：[Icdqcow2](https://github.com/lgylxyyy/Icdqcow2)（调用本库的 Android App）。

---

## 它能做什么

| 领域 | 能力 |
|---|---|
| **镜像** | 建 qcow2 / raw / vmdk、扩容、压缩回收（只写非零块）、克隆 |
| **分区表** | MBR / GPT 读写、按角色自动布局（ESP + MSR + 系统 + 恢复）、增删改、属性位、导出/导入整表 |
| **FAT32** | 列目录、读写文件、建目录、改名、删除、卷标、时间戳、8.3 短名处理 |
| **NTFS** | 列目录、读写文件、递归复制/移动、删除、`du`、卷标、格式化（`mkntfs`） |
| **WIM / ESD** | 列映像、估算展开体积、灌到分区、从映像里抽单个文件、**捕获**（qcow2 → 新 WIM） |
| **ISO** | 浏览 ISO9660 / UDF、抽取内部文件（`install.wim` 可直接从 ISO 里取） |
| **压缩包** | zip / 7z / tar 浏览与解压（只读），**RAR 也能读**（内置 unrar 解码器） |
| **引导** | 写 MBR/VBR 引导码、铺 `bootmgr` / `bootmgfw.efi` / `boot.sdi` / 字体、生成 BCD（BIOS 与 UEFI 两套）、引导自检、`win.finish` 收尾 |
| **恢复环境** | 把 `winre.wim` 装进恢复分区或系统分区、写 `ReAgent.xml` / 恢复脚本 |
| **注册表** | 离线读写 5 个标准 hive（SYSTEM/SOFTWARE/SAM/SECURITY/DEFAULT）；事务性：抽出→改→校验→写回 |

### 磁盘格式

| 格式 | 读 | 写 | 建盘 |
|---|---|---|---|
| **qcow2** | ✅ | ✅ | ✅ |
| **raw**（.img/.raw/.dd） | ✅ | ✅ | ✅ |
| **VHD**（vpc，动态盘） | ✅ | ✅ | ⚠️ 暂不支持（用 raw 转） |
| **VMDK**（sparse） | ✅ | ✅ | ✅ |

格式按**魔数**自动识别，不看扩展名；统一路径语法对四种格式一视同仁
（`copy --src="disk.vhd!p2:/x"` 之类）。

**不做的事**：不启动虚拟机、不联网、不做在线挂载。镜像必须处于关机状态 ——
我们是绕过文件系统驱动直接改元数据，两个写入方同时改必然损坏。

---

## 构建

依赖：CMake ≥ 3.16、C11 编译器、`zlib`。以下为可选（缺了对应命令降级，不会编译失败）：

| 可选依赖 | 打开的命令 | 关闭方式 |
|---|---|---|
| `libntfs-3g` | 全部 NTFS 读写 | `-DIMG_WITH_NTFS3G=OFF` |
| `ntfsprogs`（mkntfs） | NTFS 格式化 | 默认就是 OFF；要开 `-DIMG_WITH_NTFS=ON`，且先跑 `tools/build-mkntfs-obj.sh` 生成目标文件（GPL 相关，见「许可」） |
| `wimlib` | `wim.*` | `-DIMG_WITH_WIM=OFF` |
| `libarchive` | `archive.*`（zip/7z/tar） | `-DIMG_WITH_ARCHIVE=OFF` |
| `libhivex` | BCD / 注册表 hive | 自动检测 |
| 内置 unrar | `archive.*` 读 RAR | `-DIMG_WITH_RAR=OFF` |

```bash
# 最省事的构建：不带 GPL 的 mkntfs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

# 需要 NTFS 格式化时：先生成 mkntfs 目标文件，再打开开关
tools/build-mkntfs-obj.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DIMG_WITH_NTFS=ON
cmake --build build -j"$(nproc)"
```

产物：

```
build/tools/imgcmd        主命令行
build/tools/imgbc         单独写 BCD
build/tools/imglayout     单独算分区布局
build/tools/imgdump       扇区/结构 dump（调试用）
build/libimgtool.a        静态库
```

自检（会打印每条命令与四个能力开关）：

```bash
build/tools/imgcmd --list | head -c 200
# {"version":1,"features":{"wimlib":true,"ntfs3g":true,"zlib":true,"mkntfs":true},...}
```

### Android（NDK 交叉编译，产出 `libimgtool.so` 给 App `dlopen`）

同一份源码，换 toolchain 文件即可；构建入口在仓库根：

```bash
export NDK=/path/to/android-ndk-r28c

# 一次性：交叉编译第三方依赖（ntfs-3g / wimlib / zlib / libarchive / unrar 的 .a）
./android-deps.sh

# 每次改完 C 代码：编库
./android-cross.sh                 # 默认 arm64-v8a armeabi-v7a x86_64
./android-cross.sh arm64-v8a       # 只编一个 ABI（日常最快）
TOOLS=1 TESTS=1 ./android-cross.sh # 连 imgcmd 与单元测试一起编（推真机排查用）
NTFS=OFF ./android-cross.sh        # 不带 GPL 的 mkntfs
```

产物：`build/android/<abi>/libimgtool.so`（+ `TOOLS=1` 时的 `imgcmd`）。

App 侧落点与核对方法见 `ANDROID-BUILD.md`；真机跑命令：

```bash
adb push build/android/arm64-v8a/imgcmd /data/local/tmp/imgcmd
adb shell chmod +x /data/local/tmp/imgcmd
adb shell /data/local/tmp/imgcmd img.info --img=/sdcard/Download/test.qcow2
```

Android 上的已知限制（同一份代码里已适配，无需改源码）：

- 长任务只能靠**进度回调 + 取消标志**（没有信号、没有多进程可杀）；
- `content://` 路径由 App 先落缓存再传绝对路径进来；
- 写盘前 App 要查宿主剩余空间（`statvfs`），否则写到一半 `ENOSPC` 会把镜像搞脏。

32 位 ARM（armeabi-v7a / armhf）详见 `ARMHF-BUILD.md`。

---

## 命令行怎么用

```
imgcmd <命令> [--参数=值 ...]
imgcmd --list                 # 列出全部命令（JSON，含参数类型/必填/默认/帮助）
imgcmd --help=<命令>          # 单条命令的用法
```

- **参数一律字符串**，形如 `--img=/sdcard/a.qcow2`、`--recursive=true`。
- **结果走 stdout（一行 JSON）**，日志走 stderr。所以 `imgcmd ... | jq .` 永远干净。
- **成功**：`{"ok":true,"cmd":"img.info","version":1,"duration_ms":12,"data":{...}}`
- **失败**：`{"ok":false,...,"err":{"rc":-4,"error_code":"IMG-OPEN-FAIL","at":"img.info","hint":"<说明>","suggest":"<处置建议>"}}`

`error_code` 是稳定契约（App 按它判分支）；`hint` / `suggest` 是供人阅读的说明文本，**不要解析**。

### 例子 1：看看这个镜像是什么

```bash
imgcmd img.info  --img=/sdcard/win11.qcow2
imgcmd part.show --img=/sdcard/win11.qcow2
```

### 例子 2：造一块 64 GB 的 GPT 盘并格式化

```bash
imgcmd img.create --out=/sdcard/win11.qcow2 --size-gb=64
# 按角色自动布局（GPT：ESP + MSR + 系统 + 恢复）
imgcmd part.create --img=/sdcard/win11.qcow2 --role=esp   --size-mb=200
imgcmd part.create --img=/sdcard/win11.qcow2 --role=msr
imgcmd part.create --img=/sdcard/win11.qcow2 --role=sys   --size-mb=60000
imgcmd part.create --img=/sdcard/win11.qcow2 --role=rec   --size-mb=1024

imgcmd fs.format --img=/sdcard/win11.qcow2 --lba=<ESP> --fs=fat32 --label=SYSTEM
imgcmd fs.format --img=/sdcard/win11.qcow2 --lba=<SYS> --fs=ntfs  --label="Windows OS"
```

### 例子 3：把一个 Windows 装进盘里并修好引导

```bash
# 1) 灌系统（format=off 时先自己把旧根目录搬进 /Windows.old）
imgcmd wim.apply --img=/sdcard/win11.qcow2 --lba=<SYS> \
                 --wim=/sdcard/Win11.iso --inner=/sources/install.wim --index=1 --format=on

# 2) 铺引导（UEFI 一条命令：bootmgfw.efi + bootx64.efi + 字体 + boot.sdi + 内置 BCD）
imgcmd boot.populate --img=/sdcard/win11.qcow2 --sys-lba=<SYS> --esp-lba=<ESP> --fonts=true

# 3) 自检
imgcmd boot.check --img=/sdcard/win11.qcow2 --mode=uefi

# 4) 老系统（Win7/8/8.1）收尾：写 ChildCompletion\setup.exe=3 让首次开机别重装
imgcmd win.finish --img=/sdcard/win11.qcow2 --lba=<SYS>
```

BIOS 盘把第 2 步换成 `--sr-lba=<活动分区>`，不需要 `--esp-lba`。
`boot.check` 的 `--mode` 取 `bios` / `uefi` / `uefi32`（32 位 UEFI 会同时查
`BOOTX64.EFI` 和 `BOOTIA32.EFI`）。

### 例子 4：从镜像里拿文件 / 塞文件

```bash
imgcmd ntfs.list   --img=/sdcard/a.qcow2 --lba=<SYS> --path=/Windows/Boot/EFI
imgcmd ntfs.export --img=/sdcard/a.qcow2 --lba=<SYS> --path=/Windows/Boot/EFI/bootmgfw.efi --dst=/tmp/b.efi
imgcmd fat.put     --img=/sdcard/a.qcow2 --lba=<ESP> --src=/tmp/b.efi --dst=/EFI/Microsoft/Boot/bootmgfw.efi
```

---

## 命令一览（76 条）

> 参数太多，这里只列**必填**参数；完整签名用 `imgcmd --list` 或 `imgcmd --help=<命令>`。

### img.*（4 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `img.info` | 显示镜像基本信息（大小/分区表/分区数） | img |
| `img.clone` | 克隆保留分区到新盘（不列的分区留空 → 换系统主路径；旧镜像自动改名/校验/回滚） | img, keep |
| `img.create` | 建 qcow2 本体 + 分区表（★路径参数叫 out：img 会被当成已存在镜像打开） | out, size-gb |
| `img.compact` | 另存为紧凑镜像（源只读；只写非零块；绝不碰源文件） | src, out |

### part.*（11 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `part.show` | 打印分区表（JSON：GPT/MBR + 每个分区） | img |
| `part.scan` | 扫描分区：文件系统/容量/根目录有没有文件（App 弹不弹提示的依据，三级口径） | img, lba |
| `part.copy` | 跨镜像扇区级拷贝分区（GPT→MBR 迁移的搬运工；源只读、目标稀疏） | img, src, src-lba, dst-lba |
| `part.mbrboot` | 写 MBR 引导码（参考件）+ 随机磁盘签名（没有它 BIOS 会报「未找到操作系统」） | img, code |
| `part.layout` | BIOS/MBR 标准布局生成器（系统保留+系统+恢复0x27+扩展分区含逻辑分区） | img, sys-gb |
| `part.add` | 在【未分配空间】里新建一块分区（只改分区表，不格式化；空间不够报 PART-NOSPACE） | img |
| `part.remove` | 删掉一块分区（只改分区表；扩展分区容器要先删里面的逻辑分区） | img, lba |
| `part.resize` | 调整【最后一块】分区的大小（改分区表；不动别人的起点） | img, lba, size-mb |
| `part.export` | 导出分区表成 .json（只读；可另存原始表区） | img |
| `part.apply` | 按 .json 写整张分区表（LBA/GUID 原样；只改表，不碰数据） | img, in |
| `part.attr` | 设 GPT 分区属性（恢复分区官方要求 0x8000000000000001） | img, lba, attr |

### fat.*（8 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `fat.put` | 把本地文件写入某个 FAT32 分区（带可选空间校验） | img, lba, src, dst |
| `fat.ls` | 列某个 FAT32 分区里的目录 | img, lba |
| `fat.export` | 把 FAT32 分区里的文件导出到本地（UEFI 的 BCD 在 ESP 上） | img, lba, path, dst |
| `fat.exists` | 查 FAT32 路径是否存在（断点续跑探测原语） | img, lba, path |
| `fat.label` | 只改 FAT32 卷标（BPB + 根目录卷标项；不格式化） | img, lba |
| `fat.touch` | 设置 FAT 上路径的时间（同上；精度 2 秒） | img, lba, path |
| `fat.mkdir` | 建目录（FAT32；REQ-30.2） | img, lba, path |
| `fat.stat` | 单条详情（FAT32；REQ-30.3） | img, lba, path |

### wim.*（7 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `wim.size` | 估算某个 WIM/ESD 镜像展开后需要多少空间（工具给数字，App 定政策） | wim |
| `wim.apply` | 灌入 WIM/ESD 到分区（默认先格式化目标分区防残留；--format=off 保留原分区） | img, lba, wim |
| `wim.identify` | 从内容识别 WIM/ESD/ISO 里的 Windows 版本（不信文件名） | src |
| `wim.ls` | 浏览 WIM/ESD 内部目录树（只读；--wim 可直接给 ISO，工具自己找出 install.wim/.esd） | wim |
| `wim.extract` | 从 WIM/ESD 里只读抽出【单个文件】到本地（--wim 可直接给 ISO；取消不留半成品） | wim, path, dst |
| `wim.file-version` | 读映像里某个【文件】的 PE 版本（跨映像取引导文件前的版本闸门；不落临时文件） | path |
| `wim.put` | 把 WIM/ESD/ISO 里的一个文件直接写进分区（0 临时文件；REQ-29） | img, lba, wim, path, dst-path |
| `wim.capture` | 把系统盘捕获成 WIM/ESD（estimate=true 只算大小） | img, lba, out, path, comp, name |
### ntfs.*（9 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `ntfs.list` | 列 NTFS 目录（给 App 名单：显示什么/搬谁由 App 决定） | img, lba |
| `ntfs.exists` | 查 NTFS 路径是否存在（断点续跑探测原语：App 判定步骤做完没） | img, lba, path |
| `ntfs.label` | 只改 NTFS 卷标（不格式化、不动文件数据） | img, lba |
| `ntfs.put` | 把本地文件写进 NTFS 分区（自动建父目录） | img, lba, src, dst |
| `ntfs.export` | 把 NTFS 分区里的文件导出到本地（抽 bootmgr/boot.sdi/winre.wim） | img, lba, path, dst |
| `ntfs.touch` | 设置盘内路径的时间（--now / --time= / --ref= 三选一；0=该项不改） | img, lba, path |
| `ntfs.parent` | 只读检查目录的 ".." 父引用是否自洽（搬过目录之后必查） | img, lba, path |
| `ntfs.mkdir` | 建目录（REQ-30.2；--parents 默认 true） | img, lba, path |
| `ntfs.stat` | 单条详情：大小/占用/四时间/属性/ino/链接数（REQ-30.3） | img, lba, path |

### winre.*（2 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `winre.auto` | 写入/删除【首次开机自动启用 WinRE】脚本（SetupComplete.cmd，官方 reagentc 机制） | img, lba |
| `winre.find` | 在 WIM/ESD 里定位 Winre.wim（只读；未找到报 WINRE-NOTFOUND） | src |

### boot.*（7 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `boot.repair` | BIOS 引导一键修复（诊断→按需重写→复验→JSON） | img, sys-lba |
| `boot.tpl-inspect` | 模板预览（只读）：模板元数据 + 对本盘的适用性判定 | tpl |
| `boot.tpl-export` | 导出 BCD 模板（骨架 + 元数据 JSON；设备绑定清零） | out |
| `boot.tpl-apply` | 应用 BCD 模板（跨族/架构/固件拒绝；同族小版本不同→返回双方版本号） | img, tpl |
| `boot.plan` | 判定『这个系统能不能用 GPT/MBR 启动』（只读，不写盘；App 灰按钮/弹框的依据） |  |
| `boot.populate` | 一条命令铺完引导（UEFI：ESP+bootmgfw+Fonts+boot.sdi+BCD；BIOS：活动标志+bootmgr+MBR/VBR 码+BCD） | img |
| `boot.check` | 引导链自检（= imgbc）：返回逐项结果 + 必需项失败数 | img |

### ctl.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `ctl.cancel` | 请求取消正在执行的操作（另一线程调用；协作式，下一个检查点生效） |  |

### fs.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `fs.format` | 把一个分区格式化成 NTFS / FAT32（不用再调 imgntfsfs/imgmkfs） | img, lba |

### win.*（2 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `win.finish` | 裸灌后首次启动收尾：离线把 SYSTEM\Setup\Status\ChildCompletion\setup.exe 改成 3（微软官方修复） | img, lba |
| `reg.get` | 离线读注册表值（返回类型/可读值/原始字节） | img, lba, path |
| `reg.set` | 离线写注册表值（dword/qword/sz/expand_sz/binary；事务性） | img, lba, path, type, value |
| `reg.list` | 列注册表键的子键与值 | img, lba, path |
| `reg.del` | 离线删注册表值或键（删键默认不递归） | img, lba, path, value, recursive |
| `win.version` | 读已装盘的真实版本：ntoskrnl 的 PE 版本 + 注册表 + SKU 交叉验证 | img, lba |

### copy.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `copy` | 统一复制：同卷/跨分区/跨镜像/宿主/ISO/WIM；路径用 !p3: 语法 |  |

### move.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `move` | 统一移动/改名：同卷纯索引；跨卷复制+校验+删源；ISO/WIM 只读 |  |

### rm.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `rm` | 统一删除：镜像内文件/目录、宿主文件/目录；递归需显式开启 | path |

### rename.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `rename` | 统一改名：同父目录改名（copy/move 的便捷入口） | path, name |

### bcd.*（3 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `bcd.build` | 从模板直接构建 BCD（Win7 的 BCD-Template 没有绑定，必须这条） | template, out |
| `bcd.create` | 用内置模板建一张能启动的 BCD（Vista~11 / BIOS+UEFI / 32+64 位；不要参考盘） | img |
| `bcd.gen-uefi` | UEFI 换盘：按新盘 GUID 重写 BCD（= imggenbcd，同一份库实现） | ref, out, sys-guid, disk-guid |

### qc.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `qc.verify` | qcow2 结构自检（别名/越界/坏 L2）—— 进程被杀后怀疑盘坏了就跑它 | img |

### du.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `du` | 算文件/文件夹的递归大小与项数（统一路径；超限回 truncated 不报错；REQ-40） | src |

### archive.*（3 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `archive.create` | 把选中的文件/目录打包成 zip/7z/tar/tgz/iso（rar 不能创建；REQ-39） | dst |
| `archive.ls` | 列压缩包（只读；返回 index_token 供续页；REQ-31） |  |
| `archive.extract` | 从压缩包抽文件/目录子树（只读；REQ-31） | archive, path, dst |

### recovery.*（3 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `recovery.status` | WinRE 状态（只读）：三处找 Winre.wim + 恢复分区空不空（卡片字段） | img |
| `recovery.populate` | 把 WinRE 填进【独立恢复分区】（UEFI） | img, rec-lba, winre-src |
| `recovery.populate-system` | 把 WinRE 填进系统分区的 Windows\System32\Recovery（BIOS） | img, rec-lba, winre-src |

### iso.*（3 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `iso.probe` | 只读卷描述符（UDF/ISO9660/Joliet）不列目录；崩了能二分是打开还是列举（REQ-45） | iso |
| `iso.ls` | 列 ISO 里的目录（UDF；Windows 安装盘的 install.wim 只在 UDF 里） | iso |
| `iso.extract` | 从 ISO 里抽文件到本地（流式 1 MiB；App 不必自己装 ISO 库） | iso, path, dst |

### cmd.*（1 条）

| 命令 | 作用 | 必填参数 |
|---|---|---|
| `cmd.list` | 导出命令表（名字/参数规格/帮助）——App 可据此自动生成界面 |  |

---

## 在 C 里调（不给命令行）

CLI 只是 `img_cmd_exec()` 的一个前端。Android 的 JNI 薄层、测试、以后可能的
Windows 版，走的都是同一个入口 —— 所以参数校验只写了一遍。

```c
#include "img_cmd.h"

char err[256];
img_ctx *ctx = img_ctx_open("/sdcard/a.qcow2", /*writable=*/false,
                            /*check_size=*/false, err, sizeof err);
if (!ctx) { /* 打不开，err 为说明文本 */ }

img_args *a = img_args_new();
img_args_set(a, "img", "/sdcard/a.qcow2");

img_result r;
img_result_begin(&r, "img.info");
if (img_cmd_exec("img.info", ctx, a, &r) == IMG_OK) {
    char buf[4096];
    img_result_json(&r, buf, sizeof buf);   /* 一行 JSON，和 CLI 输出完全一致 */
}

img_args_free(a);
img_ctx_close(ctx);
```

加一条新命令 = 写一个 handler + 在命令表里加一行 + 加两个用例。
参数规格、`--help` 文本、App 的自动校验全部读同一张表。

## 返回码

`img_result.rc` 与进程退出码同源：

| rc | 含义 | rc | 含义 |
|---|---|---|---|
| 0 | 成功 | -12 | `ESTATE` 状态机不允许 |
| -1 | `EINVAL` 参数非法 | -13 | `ECANCELLED` 用户取消 |
| -2 | `ENOMEM` | -14 | `ETRUNC` 数据截断 |
| -3 | `EIO` 底层读写失败 | -15 | `ERANGE` 偏移越界 |
| -4 | `ENOENT` 不存在 | -16 | `ETIMEDOUT` I/O 看门狗 |
| -5 | `ENOTSUP` 不支持 | -17 | `ECYCLE` 会造环（目录搬进自己） |
| -6 | `EFORMAT` 格式不合法 | -18 | `ESAMEFILE` 源与目标同一文件 |
| -7 | `ECORRUPT` 数据损坏 | -19 | `ENAMETOOLONG` |
| -8 | `EAGAIN` 需重试 | -20 | `EPARTIAL` 复制成功但删源失败 |
| -9 | `ECRC` 校验不符 | -21 | `EREFUSED` 政策拒绝 |
| -10 | `EFULL` 空间不足 | -22 | `EREADONLY` 目标是只读容器 |
| -11 | `EEXISTS` 已存在 | -23 | `ENAMEINVALID` Windows 不接受的名字 |

上层（App / 调用方）只认 `error_code` 字符串，不认 rc 的具体数字。

## 测试

```bash
tests/run_all.sh               # 常规（主机，无需 Android SDK）
DEGRADED=1 tests/run_all.sh    # 裁剪构建（缺库）的期望结果
WITH_WIM=1 tests/run_all.sh    # 额外跑重型 wim.apply 用例
```

回归里的"独立复核"不靠本库自证：分区表用 `sgdisk -v`、FAT 用 `mtools`、
NTFS 用 `ntfsls`、qcow2 用 `qemu-img check` 交叉验证。

## 目录

```
include/    公开头（img_*.h）
src/        实现：qcow2 / 分区表 / FAT32 / NTFS / WIM / ISO / 归档 / BCD / 引导 / 恢复
tools/      imgcmd、imgbc、imglayout、imgdump …（含 build-mkntfs-obj.sh）
tests/      shell + python 回归（compat.py 负责旧命令名 → 新路径语法的翻译）
reference/  引导参考件：bcd-reference-*.bin、mbr-code-*.bin、boot 模板（仅存档）
legacy/     早期实现，留档、不参与构建
third_party/unrar/   内置 RAR 解码器（见下方许可）
LICENSES/   许可证全文
cmake/      Android NDK toolchain 文件（android-arm64host.cmake）
ANDROID-BUILD.md / ARMHF-BUILD.md / WORKPLAN-HARDEN.md
android-cross.sh   Android 交叉编译入口（libimgtool.so / imgcmd / 测试）
android-deps.sh    Android 第三方依赖交叉编译（一次性）
```

## 用到的开源项目

| 组件 | 许可证 | 用途 | 是否可关 |
|---|---|---|---|
| [zlib](https://zlib.net) | Zlib | qcow2 压缩簇（deflate） | `-DIMG_WITH_ZLIB=OFF` |
| [libntfs-3g](https://github.com/tuxera/ntfs-3g) | LGPL-2.1+ | NTFS 读写核心 | `-DIMG_WITH_NTFS3G=OFF` |
| [ntfsprogs / mkntfs](https://github.com/tuxera/ntfs-3g/tree/master/ntfsprogs) | **GPL-2.0+** | NTFS 格式化 | `-DIMG_WITH_NTFS=OFF`（默认关） |
| [wimlib](https://wimlib.net) | LGPL-2.1+ | WIM/ESD 解析与灌装 | `-DIMG_WITH_WIM=OFF` |
| [libarchive](https://libarchive.org) | BSD-2/3-Clause | zip / 7z / tar | `-DIMG_WITH_ARCHIVE=OFF` |
| [hivex](https://github.com/libguestfs/hivex) | LGPL-2.1+ | 注册表 hive（BCD） | 自动检测 |
| [UnRAR](https://www.rarlab.com/rar_add.htm) | UnRAR 自由软件许可 | 只读解 RAR | `-DIMG_WITH_RAR=OFF` |

## 许可

- **默认构建**（`IMG_WITH_NTFS=OFF`）不含 GPL 代码，只含本项目自有代码 +
  LGPL / BSD / Zlib 组件。
- **打开 `-DIMG_WITH_NTFS=ON`**（把 GPL-2.0+ 的 `mkntfs` 静态链进来）后，
  产物整体须按 **GPL-2.0-or-later** 分发，并同时提供完整对应源码。
- 完整许可证文本：`LICENSES/`
  - `GPL-2.0.txt` / `LGPL-2.1.txt` / `Apache-2.0.txt` / `BSD-3-Clause.txt`
  - `MIT.txt` / `Zlib.txt` / `UnRAR.txt`
- 逐项版权与修改说明：`THIRD_PARTY.md`
- 对本仓库内第三方代码的任何修改，都记在对应文件头与提交信息里。
- **LGPL 组件的义务**：`libntfs-3g` / `wimlib` / `hivex` 在本仓库以动态链接为主；
  若静态编进 `.so`，同样须提供"可重新链接"的手段 —— 源码与构建脚本都在本仓库。
