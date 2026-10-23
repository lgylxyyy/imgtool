# ESP 关键件快照（2026-10-23）

**为什么要存进 git**：UEFI 流水线原先**运行时去某张大盘取 `bootmgfw.efi`**。
当天因为误删（见 `docs/清理记录-2026-10-23.md` 的事故更正），那张盘没了 ——
依赖一张可删的大盘取小文件，是设计缺陷，不是运气问题。

| 文件 | 大小 | 出处 | 用法 |
|---|---|---|---|
| `bootmgfw.efi` | 3,008,968 B | `images/Windows 11 UEFI.qcow2` ESP@LBA2048 `/EFI/Microsoft/Boot/bootmgfw.efi`（官方安装真值，`fat.export` 抽出） | 造 UEFI 盘时写进 ESP 的 `\EFI\Microsoft\Boot\` |
| `BOOTX64.EFI` | 同上文件的默认路径副本 | 同一张盘 `/EFI/BOOT/BOOTX64.EFI` | 可移动/默认启动项路径 |

校验：`sha256sum -c` 对照本目录 `SHA256.txt`。
同类真值盘仍在：`images/Windows 11 UEFI.qcow2`、`images/Windows 10 Xlite.qcow2`、
`images/diskB-Windows-BIOS.qcow2`、`/workspace/Windows 7.qcow2`。
