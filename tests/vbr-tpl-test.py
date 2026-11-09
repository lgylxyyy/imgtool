#!/usr/bin/env python3
"""vbr-tpl-test — 内置 NTFS 引导代码模板的三方一致性

守三件事（任何一边被改动立刻失败，不靠人记）：
  ① src/img_vbr_tpl.h 里的数组 == reference/ntfs-vbr.bin（文件与代码不能漂移）
  ② 头文件里写的 sha256 == 实际算出来的（注释不能撒谎）
  ③ imgfixboot 真写进一张新盘的字节 == 文件（写入逻辑没错位）

背景：mkntfs 因版权原因不写这段微软引导码，BIOS 盘缺它就会
"A disk read error occurred"。模板取自 Windows 安装生成的卷的 $Boot 前 10 扇区。
"""
import hashlib, os, re, subprocess, sys, tempfile

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join(R, 'build', 'tools')
REF_DIR = os.environ.get('IMGTOOL_REFERENCE_DIR', os.path.join(R, 'reference'))
REF = os.path.join(REF_DIR, 'ntfs-vbr.bin')
HDR = os.path.join(R, 'src', 'img_vbr_tpl.h')
ok = bad = 0

def chk(desc, cond, extra=''):
    global ok, bad
    if cond: ok += 1; print('  ✅ %s' % desc)
    else:    bad += 1; print('  ❌ %s %s' % (desc, extra))

def code(b): return b[0x54:0x1FE] + b[512:5120]

# 微软参考件不随仓库分发：缺了就跳过，不算失败（见 reference/README.md）
if not os.path.exists(REF):
    print('════ 跳过 vbr-tpl-test（缺 reference/ntfs-vbr.bin，微软文件未随仓库分发）════')
    print('     该文件是 Windows 卷 $Boot 前 10 扇区快照；有素材时用 IMGTOOL_REFERENCE_DIR 指过来。')
    sys.exit(77)

ref = open(REF, 'rb').read()
chk('reference/ntfs-vbr.bin 恰好 5120 字节', len(ref) == 5120, len(ref))
chk('扇区0 是 NTFS 引导扇区（"NTFS    "）', ref[3:7] == b'NTFS')
chk('扇区0 尾部 55 AA', ref[510] == 0x55 and ref[511] == 0xAA)

# ① 头文件里的数组 vs 文件
h = open(HDR).read()
m = re.search(r'IMG_VBR_TPL\[IMG_VBR_TPL_BYTES\]\s*=\s*\{(.*?)\};', h, re.S)
assert m, '头文件里找不到数组'
vals = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))]
arr = bytes(vals)
chk('数组长度 == 5120', len(arr) == 5120, len(arr))
chk('数组 == reference/ntfs-vbr.bin', arr == ref,
    '数组 sha256=%s 文件 sha256=%s' % (hashlib.sha256(arr).hexdigest()[:16],
                                        hashlib.sha256(ref).hexdigest()[:16]))

# ② 头文件里写的 sha256 vs 实际
full = hashlib.sha256(ref).hexdigest()
cs   = hashlib.sha256(code(ref)).hexdigest()
chk('头文件 IMG_VBR_TPL_SHA256 与实际一致', full in h, '实际=' + full[:16])
chk('头文件 IMG_VBR_TPL_CODE_SHA256 与实际一致', cs in h, '实际=' + cs[:16])

# ③ imgfixboot 写进新盘的字节 vs 文件
W = tempfile.mkdtemp(prefix='vbrtpl')
IMG = os.path.join(W, 'v.qcow2')
def run(*a):
    return subprocess.run(list(a), capture_output=True, text=True)

run(os.path.join(T, 'imgcmd'), 'img.create', '--out=' + IMG, '--size-gb=1', '--table=mbr', '--force=true')
ps = run(os.path.join(T, 'imgcmd'), 'part.show', '--img=' + IMG).stdout
import json
lba = json.loads([l for l in ps.splitlines() if l.startswith('{')][-1])['data']['pt']['parts'][0]['first']
run(os.path.join(T, 'imgcmd'), 'fs.format', '--img=' + IMG, '--lba=%d' % lba, '--fs=ntfs', '--label=T')
r = run(os.path.join(T, 'imgfixboot'), IMG, str(lba))
chk('imgfixboot 无 --tpl 也能写（内置模板）', r.returncode == 0 and '✅' in r.stdout, r.stdout[-80:])
got = subprocess.check_output([os.path.join(T, 'imgdump'), IMG, str(lba * 512), '5120'],
                              stderr=subprocess.DEVNULL)
chk('写进盘的引导码段 == 文件', code(got) == code(ref),
    '盘上=%s 文件=%s' % (hashlib.sha256(code(got)).hexdigest()[:16],
                          hashlib.sha256(code(ref)).hexdigest()[:16]))
r = run(os.path.join(T, 'imgfixboot'), IMG, str(lba), '--verify')
chk('imgfixboot --verify 通过', r.returncode == 0 and '一致' in r.stdout, r.stdout[-80:])


# ④ 源盘存证：两张当时的模板盘的 $Boot 前 10 扇区快照
#    （2026-10-22 从 images/Windows 10 Xlite.qcow2 与 images/diskB-Windows-BIOS.qcow2 抽出）
#    即使那两个 10GB 源盘被清理，也能用这里复核"当时的模板长什么样"。
SRCS = [('ntfs-vbr-xlite-source.bin',  'Xlite 源快照'),
        ('ntfs-vbr-diskb-source.bin', 'diskB 源快照')]
for name_file, name in SRCS:
    f = os.path.join(REF_DIR, name_file)
    if not os.path.exists(f):
        print('  ⏭ 跳过 %s（缺 reference/%s）' % (name, name_file)); continue
    b = open(f, 'rb').read()
    chk('%s 长度 5120' % name, len(b) == 5120, len(b))
    chk('%s 引导码段 == 模板' % name, code(b) == code(ref),
        '该源=%s 模板=%s' % (hashlib.sha256(code(b)).hexdigest()[:16],
                            hashlib.sha256(code(ref)).hexdigest()[:16]))
# diskB 的整 5120 字节应与模板逐字节相同（模板就是从它抽的）
db = os.path.join(REF_DIR, 'ntfs-vbr-diskb-source.bin')
if os.path.exists(db):
    chk('diskB 源快照整文件 == 模板（逐字节副本）', open(db, 'rb').read() == ref)

import shutil; shutil.rmtree(W, ignore_errors=True)
print('════ 结果：通过 %d · 失败 %d ════' % (ok, bad))
sys.exit(0 if bad == 0 else 1)
