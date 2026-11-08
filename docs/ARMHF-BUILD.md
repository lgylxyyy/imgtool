# armhf（32 位）交叉构建与验证

> 本文件属于 **android-compat 分支**（`/workspace/imgtool-android`）。
> `master` 保持可移植基线干净；Android/32 位专属改动只在这里。

## 为什么单独一份

用户 2026-10-19 定：
> "如果要做安卓端兼容的话，就将现在的代码重新放一个位置。什么时候开始做兼容，什么时候放。"

所以：
- `/workspace/imgtool`（master）= 可移植基线，给其他设备移植用；
- `/workspace/imgtool-android`（本目录，分支 `android-compat`）= 兼容改动；
- 基线 tag：`portable-baseline-2026-10-19`。

## 1. 工具链

```bash
apt-get install -y gcc-arm-linux-gnueabihf qemu-user-static
# 只要 hivex 的 armhf 运行时；头文件用宿主 /usr/include（API 相同）
apt-get install -y libhivex0:armhf
```

宿主是 aarch64 也可以交叉出 armhf（armv7）——`gcc-arm-linux-gnueabihf` 自带
`/usr/arm-linux-gnueabihf` 的 glibc，`qemu-arm-static -L` 直接跑。

## 2. 交叉编译 ntfs-3g（armhf，静态）

源码在 `/workspace/third_party/ntfs-3g`。**不要原地 configure**（会污染长期源码区，
且 out-of-tree 会被已有的 config.status 拒绝）。复制到构建区再编：

```bash
rm -rf /workspace/build-armhf/ntfs3g-src
cp -a /workspace/third_party/ntfs-3g /workspace/build-armhf/ntfs3g-src
cd /workspace/build-armhf/ntfs3g-src
make distclean
CC=arm-linux-gnueabihf-gcc CFLAGS="-O2 -pipe" ./configure \
  --host=arm-linux-gnueabihf --disable-nfconv --without-fuse \
  --disable-shared --enable-static
make -j4 || true
# ↑ 最后会在 src/ntfs-3g（FUSE 前端）报缺 fuse.h —— 不影响我们要的两样：
#   libntfs-3g/.libs/libntfs-3g.a
#   ntfsprogs/{mkntfs,ntfscp,ntfsls,ntfscat}
```

### 准备 armhf 的 mkntfs 对象

`IMG_WITH_NTFS=ON` 需要本 ABI 的 mkntfs 对象（不能混用宿主对象）：

```bash
S=/workspace/build-armhf/ntfs3g-src/ntfsprogs
D=/workspace/build-armhf/mkntfs_obj
mkdir -p $D
cp $S/mkntfs-{attrdef,boot,sd,utils}.o $S/{ntfscp,ntfsls,ntfscat}.o $D/
arm-linux-gnueabihf-objcopy --redefine-sym main=mkntfs_main  $S/mkntfs-mkntfs.o $D/mkntfs-main.o
arm-linux-gnueabihf-objcopy --redefine-sym main=ntfscp_main  $D/ntfscp.o
arm-linux-gnueabihf-objcopy --redefine-sym main=ntfsls_main  $D/ntfsls.o
arm-linux-gnueabihf-objcopy --redefine-sym main=ntfscat_main $D/ntfscat.o
```

> 与宿主版本的差异：这个版本的 automake 把 `mkntfs.c` 编成 `mkntfs-mkntfs.o`
> （宿主是 `mkntfs-main.o`），只要符号改名正确即可，文件名不影响 CMake 的 `mkntfs-*.o` glob。

## 3. 构建 imgtool（armhf）

`IMG_MKNTFS_OBJ_DIR` 是本次为交叉编译加的参数（默认仍是宿主 `../build/mkntfs_obj`）：

```bash
cd /workspace/imgtool-android
cmake -S . -B build-armhf \
  -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=armv7l \
  -DCMAKE_BUILD_TYPE=Release \
  -DIMG_WITH_WIM=OFF -DIMG_WITH_ZLIB=OFF -DIMG_BUILD_TESTS=OFF \
  -DIMG_WITH_NTFS3G=ON \
  -DNTFS3G_INC=/workspace/build-armhf/ntfs3g-src/include \
  -DNTFS3G_LIB=/workspace/build-armhf/ntfs3g-src/libntfs-3g/.libs/libntfs-3g.a \
  -DIMG_WITH_NTFS=ON \
  -DNTFSDEV_INC=/workspace/build-armhf/ntfs3g-src/include \
  -DNTFSDEV_LIB=/workspace/build-armhf/ntfs3g-src/libntfs-3g/.libs/libntfs-3g.a \
  -DIMG_MKNTFS_OBJ_DIR=/workspace/build-armhf/mkntfs_obj \
  -DHIVEX_INC=/usr/include \
  -DHIVEX_LIB=/usr/lib/arm-linux-gnueabihf/libhivex.so.0
cmake --build build-armhf -j4
```

说明：
- `IMG_WITH_WIM=OFF`：宿主没有 armhf 的 wimlib；WIM 灌入不在本轮 32 位验证范围。
- `IMG_WITH_ZLIB=OFF`：同理（qcow2 压缩簇关闭；普通镜像读写不受影响）。
- `HIVEX_LIB` 指到 armhf 的 `.so.0`（dev 包的 `.so` 符号链接没装，直接指实体）。

## 4. 验证

```bash
bash tests/armhf-smoke.sh
```

实测（2026-10-19，aarch64 宿主 + qemu-arm-static）：

| 项 | 结果 |
|---|---|
| 6GiB GPT 盘，RE 分区起始 **LBA 10483712（偏移 5,367,660,544 > 4GiB）** | ✅ |
| FAT32（ESP）写 8MiB → 宿主抽回 md5 | ✅ 逐字节一致 |
| NTFS（RE，>4GiB 偏移）arm32 进程内 mkntfs 格式化 | ✅ |
| arm32 NTFS 写 8MiB → 上游 ntfsls 可见 → 宿主抽回 md5 | ✅ 逐字节一致 |
| 宿主 `qemu-img check` | ✅ No errors |

这关闭了 `WORKPLAN-HARDEN` 里"32 位 NTFS 路径未实测"的缺口。

## 5. 诚实边界

- 这是 **glibc armhf + qemu-user**，不是 Android Bionic/NDK 真机。
  它验证的是 32 位 `size_t/long`、64 位偏移、NTFS/mkntfs 的位宽正确性；
  **不**验证 Bionic 差异、SELinux、SAF、NDK 工具链。
- Android NDK 构建仍未做（需要 NDK + 为各 ABI 编 ntfs-3g/hivex/wimlib）。
  下一步在**本分支**里加 `android.toolchain.cmake` 与 `Application.mk` 等价配置。
- `recovery_bcd_test` / WIM 直灌在 armhf 下未跑（缺 armhf wimlib）；与 32 位无关的功能缺口。