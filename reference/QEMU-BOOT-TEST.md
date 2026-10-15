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

## 四、待办
- [ ] 完整解码 88 字节设备对象的其余字段（[48..87]）
- [ ] 换用 `Windows 10 UEFI.qcow2`（真实官方安装）做对照，排除 diskA 素材问题
- [ ] 用 `-d int,cpu_reset` 抓 QEMU 中断日志定位卡死点
