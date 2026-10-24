# NTFS 依赖获取说明（源码不入本仓库，保持仓库精简）

## 为什么外置
ntfs-3g + ntfsprogs 约 **99,000 行 C**，全部 vendor 进仓库会淹没我们自己的 diff，
也不便于跟进上游修复。故放在仓库外，由构建脚本按下面步骤获取。

## 获取方式
```bash
# 官方仓库（2026-09 仍活跃）
git clone https://github.com/tuxera/ntfs-3g.git   # 本地工作区: /workspace/build/third_party/ntfs-3g
# 或本沙箱可用的 tarball 方式（git clone 走 HTTPS 会遇到 TLS 限制）
curl -sL https://codeload.github.com/tuxera/ntfs-3g/tar.gz/refs/heads/edge -o ntfs3g.tgz
tar -xzf ntfs3g.tgz
```

## 构建（已实测通过）
```bash
apt-get install -y autoconf automake libtool pkg-config libgcrypt20-dev
cd ntfs-3g && ./autogen.sh && ./configure --disable-nfconv --without-fuse
make -j8          # 只要 mkntfs 与 libntfs-3g，FUSE 驱动可忽略
```
产物：
- `ntfsprogs/.libs/mkntfs`        319,648 字节
- `libntfs-3g/.libs/libntfs-3g.a` 2,728,028 字节

## 宿主用 mkntfs 目标文件（`/workspace/build/mkntfs_obj`）

`IMG_WITH_NTFS=ON` 时 CMake 会 glob `../build/mkntfs_obj/mkntfs-*.o`，并把
`ntfscp.o / ntfsls.o / ntfscat.o` 给 `imgntfscp` / `imgntfsfs` 用。
**这 8 个 .o 是构建输入，不是可随手删的构建垃圾**（删了 → `features.mkntfs=false`，
`fs.format --fs=ntfs` 报 `FEATURE-UNAVAILABLE`）。

一条命令重建（源码树用 `/workspace/third_party/ntfs-3g`，已 configure 过）：

```bash
imgtool/tools/build-mkntfs-obj.sh                    # → /workspace/build/mkntfs_obj
cd imgtool/build && cmake .. -DCMAKE_BUILD_TYPE=Release -DIMG_WITH_NTFS=ON && make -j8
./tools/imgcmd --list | grep -o '"features":{[^}]*}'  # mkntfs 必须是 true
```

脚本做的事：`gcc -DHAVE_CONFIG_H -I<src> -I<src>/include -I<src>/include/ntfs-3g -I<src>/ntfsprogs`
逐个编译 `attrdef/boot/sd/utils/mkntfs`，再 `objcopy --redefine-sym main=mkntfs_main`
（工具三份改成 `ntfscp_main/ntfsls_main/ntfscat_main`），最后断言“没有裸 main”。

## 集成方式（性能最优）
实现 `struct ntfs_device_operations` 的 7 个回调 → qcow2 后端，
使 mkntfs **进程内直接格式化 qcow2 分区**：无临时文件、无 fork、无 loop、无 root。

## 许可证
- `libntfs-3g`  **LGPL-2.1+**（动态链接即可）
- `ntfsprogs`   **GPL-2.0-or-later**（→ 对应编译开关 `-DIMG_WITH_NTFS=ON/OFF`）

## 溯源
- 上游: https://github.com/tuxera/ntfs-3g
- 取得日期: 2026-10-15（edge 分支）
- 改动: **不修改上游源码**，仅在我们的 imgtool 中实现 device 后端并链接
