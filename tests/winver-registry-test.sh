#!/bin/bash
# winver-registry-test.sh — win.version 的几个"Windows 自己写"的字段（REQ-20261111-72）
#
# 背景：App 里原来有一张 build → 功能更新代号（22H2 / 24H2）的硬编码表，微软发一个
# 新 build 就过期。这些值就在 HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion 里，
# 多读几个值就能把那张表删掉（REQ-74 又加了 CSDVersion）：
#
#   DisplayVersion     24H2        （1903+）
#   CSDVersion         Service Pack 3（XP/7 的服务包；8 以后为空）
#   ReleaseId          1709        （1511~1809，老系统）
#   UBR                3800        （补丁修订号 → 显示 26100.3800）
#   InstallationType   Client      （同 build 区分 Win11 与 Server 2025）
#
# 契约：**恒定输出**。读不到就是空串 / 0，不省略字段 —— 否则"字段不存在"和
#       "值恰好是空"在 App 那边没法区分。
#
# 素材：造一个只有 /Windows/System32/config/SOFTWARE 的最小盘（不依赖微软文件）。
set -u
T="${IMGTOOL:-$(cd "$(dirname "$0")/.." && pwd)/build/tools/imgcmd}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
fail=0
ok(){ echo "  ✅ $1"; }
ng(){ echo "  ❌ $1"; fail=$((fail+1)); }
jq_(){ python3 -c "import sys,json;d=json.load(sys.stdin);print($1)" 2>/dev/null; }

. "$ROOT/tests/ref.sh"
. "$ROOT/tests/fixture.sh"

IMG="$W/t.qcow2"; LBA=2048
HIVE="$W/hive.bin"
if ! fixture_bcd_hive "$HIVE" mbr; then
  echo "  ⏭ 跳过 winver-registry-test（造 hive 失败）"; exit 77
fi

echo "════ 0. 造盘（只有 SOFTWARE 的最小 Windows 盘）════"
$T img.create --out="$IMG" --size-gb=1 --table=mbr --sys-mb=256 --win-mb=0 --limits=off >/dev/null 2>&1 \
  || { echo "造盘失败"; exit 2; }
$T fs.format --img="$IMG" --lba=$LBA --fs=ntfs >/dev/null 2>&1 || { echo "格式化失败"; exit 2; }
$T ntfs.mkdir --img="$IMG" --lba=$LBA --path=/Windows/System32/config >/dev/null 2>&1
$T ntfs.put --img="$IMG" --lba=$LBA --src="$HIVE" --dst=/Windows/System32/config/SOFTWARE >/dev/null 2>&1 \
  || { echo "放 hive 失败"; exit 2; }

echo "════ 1. 只有基本键时：五个字段都在，值是空串 / 0 ════"
# 先写最小的 Windows 身份（win.version 至少要注册表里有个 build 才肯出手）
KEY='SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\ProductName"  --type=sz --value='Windows 7 Professional' >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\EditionID"    --type=sz --value='Professional'           >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\CurrentBuild" --type=sz --value='7601'                   >/dev/null 2>&1
J="$($T win.version --img="$IMG" --lba=$LBA 2>/dev/null)"
check_empty(){   # check_empty <键> <期望的 python repr>
  local got; got="$(echo "$J" | jq_ "repr(d['data'].get('$1'))" 2>/dev/null)"
  [ "$got" = "$2" ] && ok "$1 = $2" || ng "$1 期望 $2，实际 ${got:-<字段不存在>}"
}
check_empty registry.display_version "''"
check_empty registry.release_id "''"
check_empty registry.installation_type "''"
check_empty registry.ubr "0"
check_empty registry.csd_version "''"

echo "════ 2. 写进去再读（1903+ 形态：DisplayVersion + UBR + Server）════"
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\DisplayVersion"   --type=sz    --value='24H2'  >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\InstallationType" --type=sz    --value='Server' >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\UBR"              --type=dword --value=3800    >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\ProductName"      --type=sz    --value='Windows 11 Pro' >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\EditionID"        --type=sz    --value='Professional'   >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\CurrentBuild"     --type=sz    --value='26100'    >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\CSDVersion"       --type=sz    --value='Service Pack 3' >/dev/null 2>&1

J="$($T win.version --img="$IMG" --lba=$LBA 2>/dev/null)"
get(){ echo "$J" | jq_ "repr(d['data'].get('$1'))" 2>/dev/null; }
[ "$(get registry.display_version)"   = "'24H2'" ]       && ok "DisplayVersion 读回 24H2"   || ng "DisplayVersion 读到 $(get registry.display_version)"
[ "$(get registry.installation_type)" = "'Server'" ]     && ok "InstallationType 读回 Server"|| ng "InstallationType 读到 $(get registry.installation_type)"
[ "$(get registry.ubr)"               = "3800" ]         && ok "UBR 读回 3800"              || ng "UBR 读到 $(get registry.ubr)"
[ "$(get registry.current_build)"     = "26100" ]        && ok "CurrentBuild 读回 26100"    || ng "CurrentBuild 读到 $(get registry.current_build)"
[ "$(get registry.edition_id)"        = "'Professional'" ] && ok "EditionID 读回 Professional" || ng "EditionID 读到 $(get registry.edition_id)"
[ "$(get registry.csd_version)"       = "'Service Pack 3'" ] && ok "CSDVersion 读回 Service Pack 3" || ng "CSDVersion 读到 $(get registry.csd_version)"

echo "════ 3. 老系统形态（只有 ReleaseId，没有 DisplayVersion）════"
$T reg.del --img="$IMG" --lba=$LBA --path="$KEY" --value=DisplayVersion >/dev/null 2>&1
$T reg.set --img="$IMG" --lba=$LBA --path="$KEY\\ReleaseId" --type=sz --value='1709' >/dev/null 2>&1
J="$($T win.version --img="$IMG" --lba=$LBA 2>/dev/null)"
[ "$(get registry.display_version)" = "''" ]     && ok "DisplayVersion 删掉后是空串（不是缺字段）" || ng "DisplayVersion 应为空串，实际 $(get registry.display_version)"
[ "$(get registry.release_id)"      = "'1709'" ] && ok "ReleaseId 读回 1709" || ng "ReleaseId 读到 $(get registry.release_id)"

echo "════ 4. 两次都读同一份盘：结果稳定（幂等）════"
dump(){ $T win.version --img="$IMG" --lba=$LBA 2>/dev/null | python3 -c 'import sys,json;d=json.load(sys.stdin)["data"];print(json.dumps({k:v for k,v in d.items() if k.startswith("registry.")},sort_keys=True))'; }
A="$(dump)"; B="$(dump)"
[ -n "$A" ] && [ "$A" = "$B" ] && ok "两次结果一致" || ng "两次结果不一致"

echo
echo "════ winver-registry-test：通过 $((14-fail)) · 失败 $fail ════"
[ $fail -eq 0 ] || exit 1