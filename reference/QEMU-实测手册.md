# QEMU 实测手册（给你在本地跑）

> 目的：沙箱里 **无 KVM**（`/dev/kvm` 不存在、CPU 无 vmx/svm），QEMU 只能用 TCG 软件模拟。
> 所有"能启动"的镜像在这个环境里都卡死，**无法分辨是镜像问题还是模拟太慢**。
> 所以把镜像交给你，在你那边（有硬件虚拟化）跑，结果才有判别力。

---

## 一、三个镜像

| 文件 | 是什么 | 预期 |
|---|---|---|
| `TEST-1-原版diskA-预期卡logo.qcow2` | 未改动的 diskA（GPT+UEFI，历史记录里"✅ UEFI 成功"的那个） | 基线 |
| **`TEST-2-带恢复分区-预期0xc0000098.qcow2`** | **diskA + 我们建的恢复分区 + BCD 恢复条目** | 沙箱里出恢复屏报 `0xc0000098` |
| `TEST-3-官方原版Win10-对照.qcow2` | `Windows 10 UEFI.qcow2`，官方 ISO 装的真实系统 | 对照（沙箱里也卡 logo） |

**TEST-2 的完整构成**（全部由我们的 C 工具链生成）：
```
GPT: [0]ESP 100MB  [1]MSR 16MB  [2]OS 32GB  [3]Recovery 634MB(de94bba4)
Recovery 分区: NTFS, GPT属性 0x8000000000000001
  \Recovery\WindowsRE\Winre.wim  435,363,851 字节（读回逐字节 0 差异）
  \Recovery\WindowsRE\boot.sdi     3,170,304 字节（读回逐字节 0 差异）
ESP: \EFI\Microsoft\Boot\BCD 加了 RecoverySequence(14000008) → WinPE loader
bccheck: 必需 8/8 · 警告 4/4 · 共 16 项 →「引导链完整」
```

---

## 二、跑之前先准备 OVMF（UEFI 固件）

**Linux**：`sudo apt install ovmf` → 文件在 `/usr/share/OVMF/OVMF_CODE_4M.fd` 与 `OVMF_VARS_4M.fd`
**macOS**：`brew install qemu` 自带 edk2，路径一般是 `/opt/homebrew/share/qemu/edk2-x86_64-code.fd` **+ 需要一个 VARS 文件**：
```bash
# macOS 造 VARS（64MB 全零即可当空 NVRAM）
dd if=/dev/zero of=OVMF_VARS.fd bs=1m count=64
```
**Windows**：装 QEMU for Windows，用它自带的 `edk2-x86_64-code.fd`；VARS 同上造一个全零文件

---

## 三、跑 TEST-2（关键那个）

### Linux / macOS
```bash
cd <镜像所在目录>
cp /usr/share/OVMF/OVMF_VARS_4M.fd ./vars.fd      # 每次跑前重置 NVRAM（重要！）

qemu-system-x86_64 \
  -machine q35 -m 4096 -smp 4 \
  -accel kvm \                                     # macOS 换成 -accel hvf
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=./vars.fd \
  -drive "file=TEST-2-带恢复分区-预期0xc0000098.qcow2,if=virtio,format=qcow2" \
  -vga std
```
> ⚠️ 路径有空格，`-drive` 的整个参数**要用引号包住**（含空格），否则 QEMU 会报 `Failed to get "write" lock` 之类。

### Windows
```cmd
qemu-system-x86_64.exe ^
  -machine q35 -m 4096 -smp 4 -accel whpx ^
  -drive if=pflash,format=raw,readonly=on,file=edk2-x86_64-code.fd ^
  -drive if=pflash,format=raw,file=vars.fd ^
  -drive "file=TEST-2-带恢复分区-预期0xc0000098.qcow2,if=virtio,format=qcow2" ^
  -vga std
```

### 屏幕看到什么、怎么反馈给我
1. **能进 Windows 恢复界面** → 截图给我
2. **报 `0xc0000098`**（bootmgr 说 BCD 没有有效系统信息）→ 截图
3. **卡在厂商 logo 不动** → 告诉我卡了多久
4. **蓝屏 / 其他错误码** → 截图（错误码数字最重要）

**顺便按 F1 试试能不能进 WinRE**（恢复分区就是给它准备的）。

---

## 四、另外两个（可选，用于对比）

把上面命令里的镜像文件名换成 `TEST-1-...qcow2` / `TEST-3-...qcow2` 即可。
- **TEST-1 能正常进系统** → 说明 diskA 没问题，是我们的改动引入了 `0xc0000098`
- **TEST-1 也卡住/报错** → 说明 diskA 本身就有问题（沙箱里的现象不是模拟慢）

---

## 五、我这边已知的事实

```
沙箱（TCG，无 KVM）：
  TEST-1 原版        → 195 秒仍卡 TianoCore logo
  TEST-1 + 仅hivex重写 → 同样卡（证明 hivex 无副作用）
  TEST-2              → 25 秒到 Windows 恢复屏，报 0xc0000098
  TEST-3 官方原版     → 135 秒卡 logo

隔离实验结论：
  · 我们的 BCD 补丁【零回归】—— 逐对象逐元素 diff 只新增 14000008
  · OS 加载器引用的分区 GUID 与盘上一致（无失效引用）
  · 但 bootmgr 报"BCD 无有效系统信息"，根因未定位
```
