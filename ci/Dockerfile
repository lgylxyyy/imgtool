# ci/Dockerfile —— 完整回归用的镜像
#
# 为什么要有它：完整测试需要 ntfs-3g、wimlib、hivex、libarchive、mkntfs 对象
# 以及 qemu-img / mtools / 7z / xorrisofs。GitHub 的临时机器没装这些，
# 现装每次都要几分钟且容易因系统升级而漂移，所以把环境固定在镜像里。
#
# 库版本基本不动，所以这个镜像只在「升级依赖」或「换基础镜像」时才需要重建；
# CI 里用 buildx 的 GHA 缓存，日常不会重新下载。
#
# 构建：docker build -f ci/Dockerfile -t imgtool-ci .
# 用法：docker run --rm -v "$PWD:/src" -w /src imgtool-ci bash ci/run-tests.sh
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# 编译与测试期用到的工具，一次装齐（缺一个就会让某组用例静默跳过）
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config git ca-certificates curl \
        autoconf automake libtool \
        zlib1g-dev libxml2-dev \
        ntfs-3g libntfs-3g-dev libwim-dev libhivex-dev libarchive-dev liblzma-dev \
        p7zip-full zip unzip mtools dosfstools \
        qemu-utils xorriso \
        python3 \
    && rm -rf /var/lib/apt/lists/*

# ntfs-3g 源码：IMG_WITH_NTFS=ON 需要把 mkntfs 编成对象文件链进库，
# 发行包只给二进制，所以必须留一份源码（含 configure 生成的 config.h）。
# 按 commit 固定，避免上游 edge 分支漂移导致 mkntfs 补丁锚点对不上。
ARG NTFS3G_REF=7f0f841fc52cf719106c5c93bafe465004e36816
RUN curl -fsSL "https://codeload.github.com/tuxera/ntfs-3g/tar.gz/${NTFS3G_REF}" -o /tmp/ntfs3g.tgz \
    && mkdir -p /opt/ntfs3g-src \
    && tar -xzf /tmp/ntfs3g.tgz -C /opt/ntfs3g-src --strip-components=1 --no-same-owner \
    && cd /opt/ntfs3g-src \
    && ./autogen.sh \
    && ./configure --disable-nfconv --without-fuse --disable-shared --enable-static \
    && rm -f /tmp/ntfs3g.tgz

ENV NTFS3G_SRC=/opt/ntfs3g-src

WORKDIR /src