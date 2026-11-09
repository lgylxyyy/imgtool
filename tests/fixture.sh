#!/usr/bin/env bash
# tests/fixture.sh —— 自造测试夹具（不依赖微软参考件）
#
# 为什么需要：reference/*.bin 是从真 Windows 盘抽出来的引导字节快照（BCD hive、
# MBR 引导码、NTFS $Boot），版权属微软，不随仓库分发。但有些测试需要
# 「一个结构真实的 BCD hive」当输入。
#
# 解法：用 bcd.create 从【编译期内置模板】现造一份。内置模板本身就是清洗过的
# 真实 BCD，格式与 SYSTEM hive 相同（reg-test 一直拿它当 SYSTEM 的替身）。
# 这样别人克隆下来也能跑，不缺素材；只少了「与微软原件逐字节比对」这一层。
#
# 有原件时怎么办：tests/ref.sh 的 ref_path 会优先返回原件，调用方先问它。

_imgtool_tools() {
  printf '%s' "${IMGTOOL_TOOLS:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/tools}"
}

# fixture_bcd_hive <输出路径> [mbr|gpt]
# 成功返回 0 且文件非空；失败返回非 0（调用方决定是跳过还是报错）。
fixture_bcd_hive() {
  local out="${1:-}" mode="${2:-mbr}" T W ESP SYS SR RC
  [ -n "$out" ] || return 2
  T="$(_imgtool_tools)"
  W="$(mktemp -d /tmp/imgfixtureXXXXXX)" || return 1
  if [ "$mode" = "gpt" ]; then
    "$T/imgcmd" img.create --out="$W/t.qcow2" --size-gb=1 --table=gpt \
        --esp-mb=100 --sys-mb=600 --rec-mb=0 --limits=off >/dev/null 2>&1 || { rm -rf "$W"; return 1; }
    ESP=$("$T/imgcmd" part.show --img="$W/t.qcow2" 2>/dev/null | python3 -c 'import json,sys
d=json.load(sys.stdin)["data"]["pt"]["parts"]
v=[p["first"] for p in d if p["typeGuid"].startswith("c12a7328")]
print(v[0] if v else "")' 2>/dev/null)
    SYS=$("$T/imgcmd" part.show --img="$W/t.qcow2" 2>/dev/null | python3 -c 'import json,sys
d=json.load(sys.stdin)["data"]["pt"]["parts"]
v=[p["first"] for p in d if p["typeGuid"].startswith("ebd0a0a2")]
print(v[0] if v else "")' 2>/dev/null)
    if [ -z "$ESP" ] || [ -z "$SYS" ]; then rm -rf "$W"; return 1; fi
    "$T/imgcmd" fs.format --img="$W/t.qcow2" --lba="$ESP" --fs=fat32 >/dev/null 2>&1 || { rm -rf "$W"; return 1; }
    "$T/imgcmd" fs.format --img="$W/t.qcow2" --lba="$SYS" --fs=ntfs  >/dev/null 2>&1 || { rm -rf "$W"; return 1; }
    "$T/imgcmd" bcd.create --img="$W/t.qcow2" --table=gpt --sys-lba="$SYS" --esp-lba="$ESP" \
        --out="$out" >/dev/null 2>&1
    RC=$?
  else
    "$T/imgcmd" img.create --out="$W/t.qcow2" --size-gb=1 --table=mbr \
        --sys-mb=128 --win-mb=700 --limits=off >/dev/null 2>&1 || { rm -rf "$W"; return 1; }
    SR=$("$T/imgcmd" part.show --img="$W/t.qcow2" 2>/dev/null | python3 -c 'import json,sys
d=json.load(sys.stdin)["data"]["pt"]["parts"]
print(min(p["first"] for p in d))' 2>/dev/null)
    SYS=$("$T/imgcmd" part.show --img="$W/t.qcow2" 2>/dev/null | python3 -c 'import json,sys
d=json.load(sys.stdin)["data"]["pt"]["parts"]
print(max(p["first"] for p in d))' 2>/dev/null)
    if [ -z "$SR" ] || [ -z "$SYS" ]; then rm -rf "$W"; return 1; fi
    "$T/imgcmd" fs.format --img="$W/t.qcow2" --lba="$SR"  --fs=ntfs >/dev/null 2>&1 || { rm -rf "$W"; return 1; }
    "$T/imgcmd" fs.format --img="$W/t.qcow2" --lba="$SYS" --fs=ntfs >/dev/null 2>&1 || { rm -rf "$W"; return 1; }
    "$T/imgcmd" bcd.create --img="$W/t.qcow2" --table=mbr --sys-lba="$SYS" --sr-lba="$SR" \
        --out="$out" >/dev/null 2>&1
    RC=$?
  fi
  rm -rf "$W"
  [ "$RC" = "0" ] && [ -s "$out" ]
}

# fixture_mbr_code <输出路径> —— 从 src/img_mbr_tpl.h 抽出 440 字节 MBR 引导码
# 主程序内置的模板就是它，这里只是把它落成文件供测试当输入。
fixture_mbr_code() {
  local out="${1:-}" hdr
  [ -n "$out" ] || return 2
  hdr="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/src/img_mbr_tpl.h"
  [ -f "$hdr" ] || return 1
  python3 - "$hdr" "$out" <<'PY'
import re, sys
hdr, out = sys.argv[1], sys.argv[2]
s = open(hdr, encoding='utf-8').read()
m = re.search(r'IMG_MBR_TPL\[\w*\]\s*=\s*\{(.*?)\};', s, re.S)
if not m:
    sys.exit(1)
vals = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1)))
open(out, 'wb').write(vals)
sys.exit(0 if len(vals) >= 440 else 1)
PY
}