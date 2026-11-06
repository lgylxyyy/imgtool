# imgtool

离线读写 **qcow2** 的 C11 工具库 + 命令行（`imgcmd`）。虚拟机必须关机：
我们是直接改镜像字节，不是在线文件系统。

- **完整文档**：`/workspace/docs/工具手册-2026-10-24.md`（能力/契约/错误码/坑/决策/历史）
- App 侧：`/workspace/app/Icdqcow2`
- Android 交叉编译：`/workspace/tools/imgtool-android`（worktree，分支 `android-compat`）

## 构建（★ `-DIMG_WITH_NTFS=ON` 少了就没有 NTFS 格式化）

```bash
cmake -S . -B /workspace/build/host -DCMAKE_BUILD_TYPE=Release -DIMG_WITH_NTFS=ON
cmake --build /workspace/build/host -j8
/workspace/build/host/tools/imgcmd --list | grep -o '"features":{[^}]*}'   # mkntfs 必须 true
```

`build`（本目录下的软链）→ `/workspace/build/host`，只是为了让 tests 里的
`$ROOT/build/tools/...` 不用改。产物物理上都在工作区的 `build/` 下。

缺库会自动降级（命令仍在，报 `FEATURE-UNAVAILABLE`）：
没有 libntfs-3g → NTFS；没有 libwim → `wim.*`；没有 zlib → 压缩簇只读。

## 测试

```bash
tests/run_all.sh            # 25/0/3
DEGRADED=1 tests/run_all.sh # 27/0/1（裁剪构建）
WITH_WIM=1 tests/run_all.sh # 加跑重型 wim-apply-test
```

## 目录

```
src/        实现（img_qcow2* / img_partition / img_fat32* / img_ntfs* / img_xcopy / img_path ...）
include/    公开头（img_*.h）
tools/      imgcmd / imgbc / imglayout / imgmkfs ...（build-mkntfs-obj.sh 也在这里）
tests/      回归（shell + python；compat.py 把旧命令名翻译成新路径语法）
reference/  引导相关参考件与模板（bcd-reference-*.bin、mbr-code-*.bin、boot 模板）
```

## 三条硬规则

1. **库不出现用户可见文案**：只发 `event/action/error_code` + 数值；
   人话在 CLI 的 `E_TEXT/A_TEXT` 与 App 的 `i18n/strings.json`。
2. **拒绝必须零副作用**：所有“政策拒绝/参数非法/名字非法”都在第一个写动作之前判定。
3. **结构类改动**（分区表/文件系统元数据/BCD/qcow2 头）：写前记录偏移与长度，
   写后用**独立实现**复核（`qemu-img check` / `mtools` / `ntfsls` / `sgdisk -v`），
   并补反用例（故意损坏 → 快速报错，不死循环）。详见 `DESIGN-OUTPUT.md` 第十二节。

## 许可

`libntfs-3g` LGPL-2.1+（动态链接）；`mkntfs` 目标文件 GPL-2.0+ ⇒ 对应开关
`-DIMG_WITH_NTFS=ON/OFF`（默认 OFF）。详见 `third_party_ntfs3g.md` 与 `THIRD_PARTY.md`。