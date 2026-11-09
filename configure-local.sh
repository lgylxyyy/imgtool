#!/usr/bin/env bash
# configure-local.sh —— 探测本机路径并写入 imgtool.conf
#
# 目的：让「换成别人的机器」只需要跑一次这个脚本（或手工填 imgtool.conf），
#       而不是去改构建脚本里的路径。
#
# 用法：
#   ./configure-local.sh              自动探测，写入 imgtool.conf
#   ./configure-local.sh --ask        逐项确认（回车沿用探测值）
#   ./configure-local.sh --show       只显示解析结果，不写文件
#   ./configure-local.sh --presets    同时生成 CMakeUserPresets.json
#   ./configure-local.sh --reset      删除 imgtool.conf
#
# 生成的文件都不进版本库：
#   imgtool.conf           本机路径（本脚本写）
#   CMakeUserPresets.json  CMake 官方约定的机器相关预置（本脚本写）
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="$REPO/imgtool.conf"
EXAMPLE="$REPO/imgtool.conf.example"

ASK=0; SHOW=0; PRESETS=0
for a in "$@"; do
  case "$a" in
    --ask)     ASK=1 ;;
    --show)    SHOW=1 ;;
    --presets) PRESETS=1 ;;
    --reset)   rm -f "$CONF"; echo "已删除 $CONF"; exit 0 ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$a（见 --help）" >&2; exit 2 ;;
  esac
done

# 探测逻辑与 scripts/env.sh 共用一份，避免两处规则不一致。
# 先清掉可能已存在的本机配置，保证 --show 看到的是“探测结果”而不是旧值。
_imgtool_conf_backup=""
if [ -f "$CONF" ] && [ "$SHOW" = "1" ]; then
  _imgtool_conf_backup="$(cat "$CONF")"
  mv "$CONF" "$CONF.show-bak"
fi
# shellcheck disable=SC1091
. "$REPO/scripts/env.sh"
if [ -n "$_imgtool_conf_backup" ]; then
  mv "$CONF.show-bak" "$CONF"
  # shellcheck disable=SC1091
  . "$CONF"
fi

say() { printf '%s\n' "$1"; }
val() { printf '%s' "${1:-}"; }

ask_one() {  # $1=提示 $2=探测值 -> stdout 结果
  local cur="$2" ans=""
  printf '%s\n  当前：%s\n  新值（回车沿用）：' "$1" "${cur:-<未找到>}" >&2
  read -r ans || ans=""
  [ -n "$ans" ] && printf '%s' "$ans" || printf '%s' "$cur"
}

NDK_V="${IMGTOOL_NDK:-}"
SDK_V="${IMGTOOL_ANDROID_SDK:-}"
BUILD_V="${IMGTOOL_BUILD_ROOT:-}"
DEPS_V="${IMGTOOL_DEPS_ROOT:-}"
IMAGES_V="${IMGTOOL_IMAGES:-}"
ISO_V="${IMGTOOL_ISO_WIN10:-}"

if [ "$ASK" = "1" ]; then
  NDK_V="$(ask_one 'NDK 目录（含 source.properties）' "$NDK_V")"
  SDK_V="$(ask_one 'Android SDK 目录（可留空）' "$SDK_V")"
  BUILD_V="$(ask_one '构建产物根目录' "$BUILD_V")"
  DEPS_V="$(ask_one '依赖缓存目录' "$DEPS_V")"
  IMAGES_V="$(ask_one '测试素材目录（ISO/ESD）' "$IMAGES_V")"
  ISO_V="$(ask_one 'boot.sdi/install.wim 比对目录' "$ISO_V")"
fi

# ---- 校验：能找到的一定要真的是它 ---------------------------------------
warn=0
if [ -n "$NDK_V" ]; then
  if [ ! -f "$NDK_V/source.properties" ]; then
    say "警告：$NDK_V 不像 NDK 目录（缺 source.properties），已忽略"
    NDK_V=""; warn=1
  fi
fi
if [ -z "$NDK_V" ]; then
  if [ -n "${IMGTOOL_NDK_RAW:-}" ]; then
    say "警告：配置里填的 NDK 路径无效：$IMGTOOL_NDK_RAW"
  else
    say "警告：未找到 NDK，Android 交叉构建不可用（本地构建与测试不受影响）"
  fi
  warn=1
fi
if [ -n "$SDK_V" ] && [ ! -d "$SDK_V" ]; then
  say "警告：SDK 目录不存在，已忽略：$SDK_V"; SDK_V=""; warn=1
fi

# ---- --show：只报告 -----------------------------------------------------
if [ "$SHOW" = "1" ]; then
  say ""
  say "仓库        $REPO"
  say "构建根      $BUILD_V"
  say "工作区      $(dirname "${BUILD_V:-$REPO/build}")"
  if [ -n "$NDK_V" ]; then
    say "NDK         $NDK_V"
  elif [ -n "${IMGTOOL_NDK_RAW:-}" ]; then
    say "NDK         <无效> $IMGTOOL_NDK_RAW"
  else
    say "NDK         <未找到>"
  fi
  say "Android SDK ${SDK_V:-<未找到>}"
  say "依赖缓存    $DEPS_V"
  say "测试素材    $IMAGES_V$([ -d "$IMAGES_V" ] && echo "" || echo "  （不存在，相关用例会跳过）")"
  say "比对素材    $ISO_V$([ -d "$ISO_V" ] && echo "" || echo "  （不存在，相关用例会跳过）")"
  exit 0
fi

# ---- 写 imgtool.conf ----------------------------------------------------
umask 077
{
  echo "# 由 configure-local.sh 生成于 $(date '+%Y-%m-%d %H:%M:%S')"
  echo "# 本文件只描述【这台机器】的路径，不进版本库。"
  echo "# 字段含义见 imgtool.conf.example。留空 = 自动探测。"
  echo "IMGTOOL_NDK='$NDK_V'"
  echo "IMGTOOL_ANDROID_SDK='$SDK_V'"
  echo "IMGTOOL_BUILD_ROOT='$BUILD_V'"
  echo "IMGTOOL_DEPS_ROOT='$DEPS_V'"
  echo "IMGTOOL_IMAGES='$IMAGES_V'"
  echo "IMGTOOL_ISO_WIN10='$ISO_V'"
} > "$CONF"
chmod 600 "$CONF"

say "已写入 $CONF"
say "  NDK         ${NDK_V:-<未找到>}"
say "  Android SDK ${SDK_V:-<未找到>}"
say "  构建根      $BUILD_V"
say "  依赖缓存    $DEPS_V"
say "  测试素材    $IMAGES_V"

if [ "$PRESETS" = "1" ]; then
  if [ -z "$NDK_V" ]; then
    say "跳过 CMakeUserPresets.json：没有 NDK 就没有可生成的预置"
  else
    cat > "$REPO/CMakeUserPresets.json" <<JSON
{
  "version": 3,
  "configurePresets": [
    {
      "name": "android-arm64",
      "displayName": "Android arm64-v8a（宿主 clang + NDK sysroot）",
      "generator": "Unix Makefiles",
      "binaryDir": "\${sourceDir}/build-android/arm64-v8a",
      "toolchainFile": "\${sourceDir}/cmake/android-arm64host.cmake",
      "cacheVariables": {
        "NDK_ROOT": "$NDK_V",
        "IMG_ANDROID_ABI": "arm64-v8a",
        "IMG_ANDROID_API": "21",
        "IMG_WITH_NTFS": "ON"
      }
    }
  ]
}
JSON
    say "已写入 $REPO/CMakeUserPresets.json（cmake --preset android-arm64）"
  fi
fi

say ""
say "下一步："
say "  本地构建与回归   cmake -S . -B build && cmake --build build -j && tests/run_all.sh"
say "  Android 交叉构建 ./android-cross.sh arm64-v8a"
say "  改了路径就重跑   ./configure-local.sh"
say "  只查看不写入     ./configure-local.sh --show"
exit 0
