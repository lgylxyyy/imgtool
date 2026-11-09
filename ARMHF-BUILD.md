# armhf（32 位）交叉构建与验证

> 用途：验证 32 位位宽（`size_t` / `long` / 64 位偏移）与 NTFS、mkntfs 的位宽正确性。
> Android（Bionic / NDK）的构建见 [`ANDROID-BUILD.md`](ANDROID-BUILD.md)。

## 为什么用 glibc armhf 而不是真机

32 位移植的风险集中在位宽与偏移运算，用 glibc armhf + `qemu-arm-static` 就能覆盖，
不必占用真机。Bionic 差异、SELinux、SAF 不在本文件范围。

历史说明：2026-10-19 曾把 Android 兼容改动放在独立仓库与分支，2026-11-09 已合并回本仓库。
现在只有 `main`（主线）与 `android`（App 发布）两个分支，两者内容一致。

## 1. 工具链

```bash
apt-get install -y gcc-arm-linux-gnueabihf qemu-user-static
# 只要 hivex 的 armhf 运行时；头文件用宿主 /usr/include（API 相同）
apt-get install -y libhivex0:armhf
```

宿主是 aarch64 也可以交叉出 armhf（armv7）——`gcc-arm-linux-gnueabihf` 自带
`/usr/arm-linux-gnueabihf` 的 glibc，`qemu-arm-static -L` 直接跑。

## 2. 交叉编译 ntfs-3g（armhf，静态）

源码在仓库的 `third_party/ntfs-3g`。**不要原地 configure**（会污染长期源码区，
且 out-of-tree 会被已有的 config.status 拒绝）。复制到构建区再编：

```bash
rm -rf build-armhf/ntfs3g-src
cp -a third_party/ntfs-3g build-armhf/ntfs3g-src
cd build-armhf/ntfs3g-src
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
S=build-armhf/ntfs3g-src/ntfsprogs
D=build-armhf/mkntfs_obj
mkdir -p $D
cp $S/mkntfs-{attrdef,boot,sd,utils}.o $S/{ntfscp,ntfsls,ntfscat}.o $D/
arm-linux-gnueabihf-objcopy --redefine-sym main=mkntfs_main  $S/mkntfs-mkntfs.o $D/mkntfs-main.o
arm-linux-gnueabihf-objcopy --redefine-sym main=ntfscp_main  $D/ntfscp.o
arm-linux-gnueabihf-objcopy --redefine-sym main=ntfsls_main  $D/ntfsls.o
arm-linux-gnueabihf-objcopy --redefine-sym main=ntfscat_main $D/ntfscat.o
```

> 与宿主版本的差异：这个版本的 automake 把 `mkntfs.c` 编成 `mkntfs-mkntfs.o`
> （宿主是 `mkntfs-main.o`），只要符号改名正确即可，文件名不影响 CMake 的 `mkntfs-*.o` 通配。

## 3. 构建 imgtool（armhf）

`IMG_MKNTFS_OBJ_DIR` 是交叉编译专用参数（默认仍是宿主的 `build/mkntfs_obj`）。
在仓库根目录执行：

```bash
cmake -S . -B build-armhf \
  -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=armv7l \
  -DCMAKE_BUILD_TYPE=Release \
  -DIMG_WITH_WIM=OFF -DIMG_WITH_ZLIB=OFF -DIMG_BUILD_TESTS=OFF \
  -DIMG_WITH_NTFS3G=ON \
  -DNTFS3G_INC=build-armhf/ntfs3g-src/include \
  -DNTFS3G_LIB=build-armhf/ntfs3g-src/libntfs-3g/.libs/libntfs-3g.a \
  -DIMG_WITH_NTFS=ON \
  -DNTFSDEV_INC=build-armhf/ntfs3g-src/include \
  -DNTFSDEV_LIB=build-armhf/ntfs3g-src/libntfs-3g/.libs/libntfs-3g.a \
  -DIMG_MKNTFS_OBJ_DIR=build-armhf/mkntfs_obj \
  -DHIVEX_INC=/usr/include \
  -DHIVEX_LIB=/usr/lib/arm-linux-gnueabihf/libhivex.so.0
cmake --build build-armhf -j4
```

说明：

- `IMG_WITH_WIM=OFF`：宿主通常没有 armhf 的 wimlib；WIM 写入不在 32 位验证范围。
- `IMG_WITH_ZLIB=OFF`：同理（qcow2 压缩簇关闭；普通镜像读写不受影响）。
- `HIVEX_LIB` 指到 armhf 的 `.so.0`（dev 包的 `.so` 符号链接没装，直接指实体）。
- 路径都写相对仓库根目录的形式；在自己的机器上换成绝对路径也可以。

## 4. 验证

```bash
bash tests/armhf-smoke.sh
```

实测（aarch64 宿主 + `qemu-arm-static`）：

| 项 | 结果 |
|---|---|
| 6 GiB GPT 盘，恢复分区起始 **LBA 10483712（偏移 5,367,660,544 > 4 GiB）** | ✅ |
| FAT32（ESP）写 8 MiB → 宿主抽回 md5 | ✅ 逐字节一致 |
| NTFS（恢复分区，>4 GiB 偏移）arm32 进程内 mkntfs 格式化 | ✅ |
| arm32 NTFS 写 8 MiB → 上游 ntfsls 可见 → 宿主抽回 md5 | ✅ 逐字节一致 |
| 宿主 `qemu-img check` | ✅ No errors |

这关闭了「32 位 NTFS 路径未实测」的缺口。

## 5. 边界

- 这是 **glibc armhf + qemu-user**，不是 Android Bionic/NDK 真机。
  它验证的是 32 位 `size_t`/`long`、64 位偏移、NTFS/mkntfs 的位宽正确性；
  **不**验证 Bionic 差异、SELinux、SAF、NDK 工具链。
- Android NDK 构建与真机验证见 [`ANDROID-BUILD.md`](ANDROID-BUILD.md)。
- `recovery_bcd_test` 与 WIM 写入在 armhf 下未跑（缺 armhf wimlib）；与 32 位位宽无关。
