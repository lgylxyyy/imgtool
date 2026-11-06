# ESP 关键件（不进仓库，按需自取）

**这两个文件已经不在仓库里了**（2026-11-06 从历史中彻底剔除）。

| 文件 | 大小 | sha256 |
|---|---|---|
| `bootmgfw.efi` | 3,008,968 B | `32b233afb2a8b4c517003796d435358dcad78e421a3ed386b8ee275a6957dbd0` |
| `BOOTX64.EFI` | 同上（内容与上面完全一致） | 同上 |

## 为什么删掉

1. **没有任何代码用它**。`grep -rn "reference/esp" src include tools tests CMakeLists.txt` = 0 处。
2. **流程早就不需要外部文件了**。`boot.populate` 现在是**从目标盘自己身上拷**：
   读系统分区 `\Windows\Boot\EFI\bootmgfw.efi` → 写进 ESP 的
   `\EFI\Microsoft\Boot\bootmgfw.efi`（`src/img_cmds.c` 里的 `bp_put` / `bp_add`）。
   修复引导不需要任何 ISO，也不需要这份快照。
3. **它们是微软的二进制**，放进公开仓库等于再分发。

## 历史

2026-10-23 提交 `f8a2797` 把它们固化进了 git，原因是当时的 UEFI 流水线
**运行时要从一张测试大盘里现场抽 `bootmgfw.efi`**，而那张盘被误删过一次，
流程直接断掉。把文件塞进仓库是为了解决"依赖一张可删的大盘取小文件"这个设计缺陷。
后来流程改成"从目标盘自带文件里取"，这份快照就没有存在意义了。

## 需要时怎么拿回来

任意一张装好的 UEFI Windows 盘（qcow2）都有，从它的 ESP 里导出即可：

```bash
imgcmd fat.export --img=<某张 UEFI 盘的 qcow2> --lba=<ESP 起始 LBA> \
                  --path=/EFI/Microsoft/Boot/bootmgfw.efi --dst=./bootmgfw.efi

# 默认启动路径的那份（内容相同）
imgcmd fat.export --img=<同上> --lba=<ESP> \
                  --path=/EFI/BOOT/BOOTX64.EFI --dst=./BOOTX64.EFI
```

取完核对：

```bash
sha256sum bootmgfw.efi BOOTX64.EFI
# 都应该是 32b233afb2a8b4c517003796d435358dcad78e421a3ed386b8ee275a6957dbd0
```

把它们放在本目录下即可，`.gitignore` 已经写了 `reference/esp/*.efi`，不会再被提交。