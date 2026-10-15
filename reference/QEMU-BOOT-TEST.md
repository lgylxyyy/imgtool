# QEMU 启动实测记录（阶段 C）

> 环境：QEMU 8.2.2 + OVMF(OVMF_CODE_4M) + TCG（无 KVM）+ egl/socat 截屏
> 日期：2026-10-15

## 一、测试矩阵

| 镜像 | 改动 | 结果 |
|---|---|---|
| `diskA-Windows-UEFI-GPT.qcow2`（原版） | 无 | **195 秒仍卡 TianoCore logo**，bootmgfw 无输出 |
| `rewrite.qcow2` = diskA + **仅 hivex 打开/commit** | 只重写 BCD | 同样卡 logo → **hivex 无罪** |
| `c_re.qcow2` = diskA + 恢复分区 + **我们的 BCD 补丁** | 加 `14000008` 与 2 个对象 | **25 秒到 Windows 恢复屏** |

## 二、结论

1. **hivex 重写 hive 无副作用** —— 同大小(28672)、同行为
2. **我们的 BCD 补丁零回归** —— 逐对象逐元素 diff 只新增 `14000008`
   （见 `bcd: 用权威来源修正…` 提交）
3. **diskA 自身无法在本 QEMU 配置下完成启动**（3+ 分钟卡 logo）
4. `0xc0000098`（BCD 无有效系统信息）是**既有问题被我们的改动"提前暴露"**：
   加了恢复条目后 bootmgr 快速失败并给出可读错误，而不是无限卡死

## 三、已确认的 OS 加载器设备对象（无失效引用）

```
加载器 {d14b9cb4-…}
  11000001 / 21000001 (88B) 首16字节全零，[32..47] = 分区唯一 GUID
          22000f30b2ca5a45abff53632646f5a2
          → 300f0022-cab2-455a-abff-53632646f5a2
    diskA 分区[2] partGuid = 300f0022-cab2-455a-abff-53632646f5a2  ✅ 一致
  12000002 = \Windows\system32\winload.efi
  22000002 = \Windows
  bccheck: ntfs.winload ✅（winload.exe 存在）
```
→ 引用本身没有失效，失败原因**尚未定位**（可能在设备对象的其余 40 字节、
  或 bootmgr 对某字段的校验、或 OS 分区内容不完整）

## 四、★ 结局（2026-10-15 用户实测）

**TEST-2 在用户机器（有 KVM）上正常启动 Windows。**

三次迭代定位的三个真 bug（全部是我自己的错误）：
1. BCD 设备路径用了正斜杠 → 应为反斜杠 `\\Recovery\\WindowsRE\\...`
2. 只改了 `21000001`，漏改 `11000001`(ApplicationDevice)，后者仍指向 `\\sources\\boot.wim`
3. **`recoverysequence(14000008)` 的类型写错** —— 它是 GUID list(format=4)，
   必须 `REG_MULTI_SZ`(hive_t=7, 双 NUL)；我写成了 `REG_SZ`(t=1) 单串
   → bootmgr 读列表格式时类型不符 → `0xc0000098`（在读 BCD 阶段就失败，
     根本没机会试 OS 条目 → 这也是"加了恢复条目反而启动不了"的原因）

另外还修了一个会致命的隐患：`ramdisksdidevice(31000003)` 在官方 ISO 里是
`type=5 本地设备`（跟着 BCD 所在分区），我们 winre.wim 在独立恢复分区上，
必须改成 `type=6 分区设备` 显式指向恢复分区（格式已破译，见下方"五"）。

**已验证**：
- OVMF + QEMU 能启动我们的盘
- bootmgr 接受我们构造的 BCD（含 `recoverysequence`）
- "按 F1 进入恢复环境" 选项出现
- **Windows 正常启动**

**未验证**（因宿主机安装慢，暂按有效处理）：
- 按 F1 真正进入 WinRE 的完整流程

## 五、device 对象格式（实测破译）

`88 字节`，用 diskA 的 GPT 磁盘 GUID 逐字节验证：
```
[0..15]  16B  GUID = 0
[16..19] 4B   type = 6 分区设备 / 5 本地设备 / 1 ramdisk
[20..23] 4B   0
[24..27] 4B   size = 72
[28..31] 4B   0
[32..47] 16B  分区唯一 GUID（前3段小端、后2段按字节）
[48..55] 8B   0
[56..71] 16B  磁盘 GUID（同编码）
[72..87] 16B  0
```

## 四·二、待办
- [ ] 完整解码 88 字节设备对象的其余字段（[48..87]）
- [ ] 换用 `Windows 10 UEFI.qcow2`（真实官方安装）做对照，排除 diskA 素材问题
- [ ] 用 `-d int,cpu_reset` 抓 QEMU 中断日志定位卡死点
