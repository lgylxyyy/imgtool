#!/usr/bin/env bash
# provenance-test.sh —— 溯源校验：编译期内置模板 == 微软原件
#
# 这是「第二条防线」，与日常回归分开：
#   日常回归用 tests/fixture.sh 自造夹具，证明【逻辑】对；
#   本脚本证明【字节】对 —— 内置模板与当年从真 Windows 盘抽出的原件逐字节一致。
#
# 需要 reference/*.bin（微软的引导字节快照，版权考虑不随仓库分发）。
# 没有原件时退出码 77（计为跳过），不算失败。
#
# 用法：
#   tests/provenance-test.sh
#   IMGTOOL_REFERENCE_DIR=/path/to/refs tests/provenance-test.sh
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tests/ref.sh"

need="
mbr-code-bios.bin
ntfs-vbr.bin
bcd-tpl-legacy-bios.bin
bcd-tpl-modern-bios.bin
bcd-tpl-modern-uefi.bin
"
missing=""
for f in $need; do
  ref_have "$f" || missing="$missing $f"
done
if [ -n "$missing" ]; then
  echo "  ⏭ 跳过 provenance-test（缺微软原件：$missing ）"
  echo "     这些是版权考虑不随仓库分发的字节快照；有素材时用 IMGTOOL_REFERENCE_DIR 指过来。"
  exit 77
fi

python3 - "$ROOT" "$(ref_path mbr-code-bios.bin)" "$(ref_path ntfs-vbr.bin)" \
  "$(ref_path bcd-tpl-legacy-bios.bin)" "$(ref_path bcd-tpl-modern-bios.bin)" \
  "$(ref_path bcd-tpl-modern-uefi.bin)" <<'PY'
import hashlib, re, sys

root, mbr_ref, vbr_ref, bcd_l, bcd_b, bcd_u = sys.argv[1:7]
ok = bad = 0


def chk(desc, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1
        print('  ✅ %s' % desc)
    else:
        bad += 1
        print('  ❌ %s %s' % (desc, extra))


def array(path, pattern):
    s = open(path, encoding='utf-8').read()
    m = re.search(pattern + r'\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', s, re.S)
    if not m:
        return None
    return bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1)))


def read(p):
    with open(p, 'rb') as f:
        return f.read()


mbr = array(root + '/src/img_mbr_tpl.h', 'IMG_MBR_TPL')
vbr = array(root + '/src/img_vbr_tpl.h', 'IMG_VBR_TPL')
bl = array(root + '/src/img_bcd_tpl.h', 'IMG_BCD_TPL_LEGACY_BIOS')
bb = array(root + '/src/img_bcd_tpl.h', 'IMG_BCD_TPL_MODERN_BIOS')
bu = array(root + '/src/img_bcd_tpl.h', 'IMG_BCD_TPL_MODERN_UEFI')

chk('头文件里能解析出 5 个数组', all(x is not None for x in (mbr, vbr, bl, bb, bu)))

r_mbr, r_vbr = read(mbr_ref), read(vbr_ref)
chk('MBR 引导码 440 字节 == 微软原件', mbr == r_mbr[:440],
    '%d vs %d' % (len(mbr or b''), len(r_mbr)))
chk('NTFS $Boot 5120 字节 == 微软原件', vbr == r_vbr,
    hashlib.sha256(vbr or b'').hexdigest()[:16] + ' vs ' + hashlib.sha256(r_vbr).hexdigest()[:16])

for name, arr, ref in (('legacy_bios', bl, bcd_l), ('modern_bios', bb, bcd_b),
                       ('modern_uefi', bu, bcd_u)):
    r = read(ref)
    chk('BCD 模板 %s == 微软原件' % name, arr == r,
        '%d vs %d 字节' % (len(arr or b''), len(r)))

print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
sys.exit(0 if bad == 0 else 1)
PY