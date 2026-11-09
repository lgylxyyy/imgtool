#!/usr/bin/env bash
# tests/ref.sh —— 微软参考件（reference/*.bin）的定位与跳过约定
#
# reference/*.bin 是从真 Windows 盘/ISO 抽出的引导字节快照（BCD hive、
# MBR 引导码、NTFS $Boot），版权属微软，**不随仓库分发**（见 reference/README.md）。
#
# 因此测试对它们一律是「有就跑、没有就跳过」，不是必填素材 ——
# 否则别人克隆下来跑回归会直接红，而这跟他的环境无关。
#
# 想跑这几条：把参考件放回 reference/，或指定目录：
#   IMGTOOL_REFERENCE_DIR=/path/to/refs tests/run_all.sh

_ref_dir() {
  printf '%s' "${IMGTOOL_REFERENCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/reference}"
}

# ref_path <文件名> —— 存在则输出绝对路径，否则输出空、返回 1
ref_path() {
  local p
  p="$(_ref_dir)/$1"
  if [ -f "$p" ]; then
    printf '%s' "$p"
    return 0
  fi
  printf ''
  return 1
}

# ref_have <文件名> —— 存在返回 0
ref_have() { [ -f "$(_ref_dir)/$1" ]; }