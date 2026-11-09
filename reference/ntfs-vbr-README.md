# NTFS 引导码模板 —— 三个文件的关系与证据链

## 为什么需要它

`mkntfs` **故意不写** NTFS 的引导代码（版权原因），只写 BPB。BIOS 启动时 `bootmgr`
要靠 `$Boot` 前 10 个扇区（5120 B）里的机器码加载自己；缺了就是
`A disk read error occurred`。UEFI 不依赖这段（直接执行 ESP 上的 bootmgfw.efi）。

## 三个文件

| 文件 | 是什么 | 整文件 sha256 | 引导码段 sha256 |
|---|---|---|---|
| `ntfs-vbr.bin` | **内置模板**（`src/img_vbr_tpl.h` 的二进制原件） | `5506a698ea03bf13…` | `1d79bfa4ef4fbfa7…` |
| `ntfs-vbr-xlite-source.bin` | **当时（10-16~18）用的模板盘**的 `$Boot` 前 10 扇区快照<br>`images/Windows 10 Xlite.qcow2` @ LBA 2048 | `a054fb74de23d0f8…` | `1d79bfa4ef4fbfa7…` |
| `ntfs-vbr-diskb-source.bin` | **当时被它修过的盘**的快照<br>`images/diskB-Windows-BIOS.qcow2` @ LBA 2048 | `5506a698ea03bf13…` | `1d79bfa4ef4fbfa7…` |

- **整文件不同是正常的**：扇区 0 的 BPB（卷大小/簇数/MFT 位置…）是各卷自己的。
- **引导码段必须相同**：`扇区0[0x54..0x1FD] + 扇区1..9` —— 这才是"引导代码本体"。
- `diskB` 的整 5120 字节与 `ntfs-vbr.bin` **逐字节相同**：模板就是从它抽的。

## 证据链（可复核）

1. **当时的脚本原文**：`reference/build-bios-full.sh:26`
   ```
   $T/imgfixboot "$OUT" 2048 --tpl "<素材目录>/Windows 10 Xlite.qcow2" 2048
   ```
   → 两天测试用的模板 = `Windows 10 Xlite.qcow2` 分区 2048 的 `$Boot`
2. **三方 sha256 全等**（引导码段 `1d79bfa4…`）：模板 / Xlite 快照 / diskB 快照
   + 四张验证盘（w10b / c-win11 / d-win7 / e-win7x86）
3. **git blob 未变**：`9c8277ceed189fff24a3900469d200480ea87938`（58ab4a2 入库 = 现在）
4. **源盘若被清理**：这两个 `-source.bin` 就是存证；需要完整还原时
   `git cat-file -p 9c8277ce…` 取回模板字节。

## 适用范围（别越界）

- **bootsect /nt60 时代**：Vista ~ Windows 11。实测让 Win7(x86/x64)、Win10(x64)、Win11(x64)
  都成功加载到 `bootmgr`/`winload`。
- **不适用**：XP / 2003（NTLDR，`/nt52` 是另一套代码）。
- **架构无关**：32/64 位 Win7 的 `bootmgr` sha256 完全相同（`0769a292114dfe18…`）；
  引导码本身是 16 位实时代码。
- **语言**：引导码里含本地化提示文本（"BOOTMGR is missing"），不同语言字节不同、功能等价。
  本模板取自中文 Windows 10 卷。

## 校验

```bash
python3 tests/vbr-tpl-test.py     # 15 条断言：数组==文件==两个源快照==写入盘
```
