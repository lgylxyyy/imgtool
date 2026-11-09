#!/usr/bin/env bash
# scripts/env.sh —— 本机路径的【唯一】解析入口（被其他脚本 source）
#
# 设计依据：
#   · 入库文件只放模板与默认值，机器相关路径放不提交的本地配置
#   · 优先级：环境变量 > imgtool.conf > 自动探测 > 明确报错
#   · 猜不到就报错，不静默指向一个不存在的目录
#
# 用法（在其它脚本里）：
#   . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/scripts/env.sh"
#   imgtool_need "Android NDK" "${IMGTOOL_NDK:-}" "设 NDK 或 ANDROID_NDK_HOME" || exit 1
#
# 本文件只做解析，不创建目录、不打印日志、不退出进程。

_imgtool_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMGTOOL_REPO="$(dirname "$_imgtool_here")"

# ---- 1. 本机配置（不提交）----------------------------------------------
# imgtool.conf 由 ./configure-local.sh 生成，内容是纯 KEY=VALUE 的 shell 片段。
# ★ 环境变量优先：source 会把 conf 里的同名键写进当前 shell，所以先存一份
#   环境变量的值，读完 conf 再盖回去（否则 conf 反压环境变量）。
_img_env_ndk="${IMGTOOL_NDK:-}"
_img_env_sdk="${IMGTOOL_ANDROID_SDK:-}"
_img_env_build="${IMGTOOL_BUILD_ROOT:-}"
_img_env_ws="${IMGTOOL_WS_ROOT:-}"
_img_env_deps="${IMGTOOL_DEPS_ROOT:-}"
_img_env_images="${IMGTOOL_IMAGES:-}"
_img_env_iso="${IMGTOOL_ISO_WIN10:-}"
if [ -f "$IMGTOOL_REPO/imgtool.conf" ]; then
  # shellcheck disable=SC1091
  . "$IMGTOOL_REPO/imgtool.conf"
fi
[ -n "$_img_env_ndk" ]    && IMGTOOL_NDK="$_img_env_ndk"
[ -n "$_img_env_sdk" ]    && IMGTOOL_ANDROID_SDK="$_img_env_sdk"
[ -n "$_img_env_build" ]  && IMGTOOL_BUILD_ROOT="$_img_env_build"
[ -n "$_img_env_ws" ]     && IMGTOOL_WS_ROOT="$_img_env_ws"
[ -n "$_img_env_deps" ]   && IMGTOOL_DEPS_ROOT="$_img_env_deps"
[ -n "$_img_env_images" ] && IMGTOOL_IMAGES="$_img_env_images"
[ -n "$_img_env_iso" ]    && IMGTOOL_ISO_WIN10="$_img_env_iso"

# ---- 2. 构建根 ----------------------------------------------------------
# 仓库内 build 是软链时，取其所指目录的父目录（保持既有产物位置）；
# 否则用仓库内的 build/。两种情况都不写死绝对路径。
if [ -n "${IMGTOOL_BUILD_ROOT:-}" ]; then
  :
elif [ -L "$IMGTOOL_REPO/build" ]; then
  IMGTOOL_BUILD_ROOT="$(dirname "$(readlink -f "$IMGTOOL_REPO/build")")"
else
  IMGTOOL_BUILD_ROOT="$IMGTOOL_REPO/build"
fi
IMGTOOL_WS_ROOT="${IMGTOOL_WS_ROOT:-$(dirname "$IMGTOOL_BUILD_ROOT")}"

# ---- 3. NDK -------------------------------------------------------------
# $SDK/ndk/<version> 是 Android 官方现行布局；ANDROID_NDK_HOME 是旧式变量，CI 仍在用。
# 原始配置值（未校验）留着，出错时能告诉用户“你填的那个路径不对”
IMGTOOL_NDK_RAW="${IMGTOOL_NDK:-}"
_imgtool_pick_ndk() {
  local c
  for c in "${IMGTOOL_NDK_RAW:-}" "${NDK:-}" "${NDK_ROOT:-}" \
           "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
           "${ANDROID_HOME:-}/ndk/"* "${ANDROID_SDK_ROOT:-}/ndk/"* \
           "${IMGTOOL_ANDROID_SDK:-}/ndk/"* \
           "$HOME/Android/Sdk/ndk/"* \
           "$IMGTOOL_WS_ROOT/sdk/android-ndk/"* "$IMGTOOL_WS_ROOT/android-ndk/"*; do
    [ -n "$c" ] || continue
    [ -f "$c/source.properties" ] || continue
    printf '%s\n' "$c"
    return 0
  done
  return 1
}
IMGTOOL_NDK="$(_imgtool_pick_ndk || true)"

# ---- 4. Android SDK / 依赖缓存 / 测试素材 -------------------------------
_imgtool_pick_sdk() {
  local c
  for c in "${IMGTOOL_ANDROID_SDK:-}" "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
           "$HOME/Android/Sdk" "$IMGTOOL_WS_ROOT/android-sdk" "$IMGTOOL_WS_ROOT/sdk"; do
    [ -n "$c" ] || continue
    if [ -d "$c/build-tools" ] || [ -d "$c/platforms" ] || [ -d "$c/ndk" ]; then
      printf '%s\n' "$c"
      return 0
    fi
  done
  return 1
}
IMGTOOL_ANDROID_SDK="${IMGTOOL_ANDROID_SDK:-$(_imgtool_pick_sdk || true)}"
IMGTOOL_DEPS_ROOT="${IMGTOOL_DEPS_ROOT:-$IMGTOOL_WS_ROOT/deps}"
IMGTOOL_IMAGES="${IMGTOOL_IMAGES:-$IMGTOOL_WS_ROOT/images}"
IMGTOOL_ISO_WIN10="${IMGTOOL_ISO_WIN10:-$IMGTOOL_BUILD_ROOT/iso-win10}"

# ---- 5. 工具函数 --------------------------------------------------------
# 需要某个路径时调用，缺了就带说明返回非零（不要静默继续）。
imgtool_need() {
  local what="$1" val="${2:-}" hint="${3:-}"
  if [ -z "$val" ]; then
    printf '错误：未找到 %s\n' "$what" >&2
    [ -n "$hint" ] && printf '  %s\n' "$hint" >&2
    printf '  设置环境变量，或运行 ./configure-local.sh 写入 imgtool.conf\n' >&2
    return 1
  fi
  return 0
}
