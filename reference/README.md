# reference/ —— 参考件（二进制不再入库）

本目录里的 `.bin` 是**从真 Windows 盘/ISO 里抽出来的引导字节快照**，
2026-11-09 起**不再随仓库分发**（工作区里保留，`.gitignore` 已忽略）。

## 为什么要移出仓库

| 文件 | 是什么 | 版权归属 |
|---|---|---|
| `bcd-reference-*.bin` | 真机 BCD 注册表 hive 快照 | 微软 |
| `bcd-tpl-*.bin` | 从上面清洗出的 BCD 模板 | 微软 |
| `iso-bcd-win11.bin` | Windows 11 ISO 里的 BCD | 微软 |
| `ntfs-vbr*.bin` | NTFS `$Boot` 前 10 扇区（引导代码） | 微软 |
| `mbr-code-bios.bin` | MBR 引导码（440 字节） | 微软 |

这些文件**只用于研究/比对**，没有任何 C 代码在编译期或运行期读取它们：

```bash
grep -rn "reference/bcd-\|reference/ntfs-vbr\|reference/mbr-code" src include tools tests
# 只有 tools/mkbcdtpl.py（开发期生成脚本）和文档提到
```

## 程序实际用的是这三个头文件

引导模板真正生效的方式是**编译期内置**：

| 头文件 | 内容 | 被谁 include |
|---|---|---|
| `src/img_bcd_tpl.h` | BCD 模板字节数组（458 KB） | `src/img_bcd.c` |
| `src/img_vbr_tpl.h` | NTFS `$Boot` 前 10 扇区（29 KB） | `src/img_fixboot.c` |
| `src/img_mbr_tpl.h` | MBR 引导码前 440 字节（3.5 KB） | `src/img_cmds.c` |

**这三个 `.h` 留在仓库里**，理由见下节。

## 关于这三个内置模板的说明

`mkntfs` 出于版权原因**故意不写** NTFS 引导代码，只写 BPB；
Windows 的 `bootmgr` 要靠 `$Boot` 前 10 扇区那 5120 字节才能被加载。
没有它，BIOS 启动会停在 `A disk read error occurred`。

为了让在 Linux / Android 上做出来的盘能被真实 Windows 引导，
本项目内置了这段引导扇区机器码（16 位实模式代码，不含微软的程序逻辑）。
这与 BootICE、GRUB、Limbo、Vectras VM 等项目的做法一致：
**作为互操作性所需的引导代码使用，不声称原创，也不包含任何微软源代码。**

需要完整还原或核对时，从任意一张同版本 Windows 盘上重新抽即可：

```bash
# NTFS $Boot 前 10 扇区（5120 字节）
imgcmd ntfs.export --img=<qcow2> --lba=<NTFS 分区起始 LBA> \
                   --path=/\$Boot --dst=./boot-sector.bin

# ESP 上的 BCD
imgcmd fat.export  --img=<qcow2> --lba=<ESP 起始 LBA> \
                   --path=/EFI/Microsoft/Boot/BCD --dst=./BCD
```

## 证据链仍然可复核

`ntfs-vbr-README.md` 里记录的 sha256 与三份快照的对应关系依然有效 ——
只要从同版本盘上重新抽，就能验证引导码段是 `1d79bfa4ef4fbfa7…`。

## 需要这些 `.bin` 时

它们不在仓库里了，但**工作区和本地备份里都有**：

- 工作区：`reference/*.bin`（被 `.gitignore` 忽略）
- 备份：`/workspace/backup-prerewrite/`
