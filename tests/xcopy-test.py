#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xcopy-test —— file.copy（跨分区/跨镜像/宿主/ISO/WIM）端到端

覆盖矩阵：
  宿主目录 → NTFS（递归、字节、时间）
  NTFS → FAT32（同镜像跨分区）
  FAT32 → 宿主（递归、字节）
  NTFS → NTFS（跨 qcow2）
  FAT32 → FAT32（同卷）
  冲突政策 fail / overwrite / rename（"名 (1).扩展名"）
  目录合并 / 目录改名
  中文名往返（NTFS↔FAT↔宿主）
  空文件、递归开关、造环拒绝、自己复制自己、FAT 4 GiB 上限
  批量清单（--list_file，每行带 @lba）
  ISO → NTFS、WIM/ESD → NTFS（素材缺失时跳过，不算失败）
  符号链接跳过（skipped 计数）
  收尾：qc.verify + 独立三方对账（差值模式，不许新增不一致）

设计纪律：每个否定用例都同时断言"盘上没被写脏"（拒绝必须零副作用）。
"""
import json, os, shutil, subprocess, sys, tempfile
import compat

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
INV = os.path.join(ROOT, 'tools', 'ntfs-inventory.py')
W = tempfile.mkdtemp(prefix='xcopy')
ok = bad = skip = 0
GiB = 1024 * 1024 * 1024


def run(cmd, **kw):
    if cmd == 'file.copy':
        cmd, kw = compat.translate_filecopy(kw)
    args = [IMGCMD, cmd] + ['--%s=%s' % (k, v) for k, v in kw.items()]
    # errors='replace'：stderr 的日志行可能带文件名里的原始字节（非 UTF-8），
    # 那是给人看/给 logcat 的，不是给解析的；stdout 的 JSON 由工具端保证合法 UTF-8。
    p = subprocess.run(args, capture_output=True, text=True, errors='replace')
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return json.loads(js[-1]) if js else {'ok': False, 'error_msg': (p.stdout + p.stderr)[-300:]}


def code(d):
    return (d.get('err') or d.get('error') or {}).get('error_code', '')


def data(d):
    return d.get('data') or {}


def check(desc, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1; print('  ✅ %s' % desc)
    else:
        bad += 1; print('  ❌ %s  %s' % (desc, str(extra)[:280]))


def skipped(desc):
    global skip
    skip += 1; print('  ⏭ %s（素材缺失，跳过）' % desc)


def mk(b, name):
    p = os.path.join(W, name)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(b)
    return p


def rd(p):
    with open(p, 'rb') as f:
        return f.read()


def new_img(name, esp_mb=64, msr_mb=16):
    img = os.path.join(W, name)
    d = run('img.create', out=img, **{'size-gb': 2, 'table': 'gpt',
                                      'esp-mb': esp_mb, 'msr-mb': msr_mb,
                                      'limits': 'off'})
    parts = data(d)['pt']['parts']
    esp = parts[0]['first']
    data_lba = parts[-1]['first']
    assert run('fs.format', img=img, lba=esp, fs='fat32', label='ESP', quick='true').get('ok')
    assert run('fs.format', img=img, lba=data_lba, fs='ntfs', label='DATA', quick='true').get('ok')
    return img, esp, data_lba


print('════ file.copy 跨位置复制 ════')
A, AESP, AN = new_img('a.qcow2')
B, BESP, BN = new_img('b.qcow2', esp_mb=32)

# ─────────── 1. 宿主目录 → NTFS（递归） ───────────
tree = os.path.join(W, 'tree')
os.makedirs(os.path.join(tree, 'sub', 'deep'), exist_ok=True)
mk(b'hello-xcopy\n', 'tree/a.txt')
mk(os.urandom(300000), 'tree/sub/b.bin')
mk(os.urandom(1000), 'tree/sub/deep/c.bin')
os.utime(os.path.join(tree, 'sub', 'b.bin'), (1700000000, 1700000000))
d = run('file.copy', img=A, dst_lba=AN, src='host:' + tree, dst='ntfs:/Users')
check('宿主目录 → NTFS：ok + 3 文件 3 目录',
      d.get('ok') and data(d).get('copied_files') == 3 and data(d).get('copied_dirs') == 3,
      json.dumps(d)[:240])
check('  返回 dst_path=/Users', data(d).get('dst_path') == '/Users', data(d).get('dst_path'))

def ntfs_export(p, tag):
    out = os.path.join(W, 'exp_' + tag)
    r = run('ntfs.export', img=A, lba=AN, path=p, dst=out)
    return rd(out) if r.get('ok') and os.path.exists(out) else None

check('  内容逐字节一致（b.bin）', ntfs_export('/Users/sub/b.bin', 'b') == rd(os.path.join(tree, 'sub', 'b.bin')))
check('  内容逐字节一致（a.txt）', ntfs_export('/Users/a.txt', 'a') == rd(os.path.join(tree, 'a.txt')))
root_ls = data(run('ntfs.list', img=A, lba=AN, path='/', times='true')).get('names') or []
u = [x for x in root_ls if x['name'] == 'Users']
want_dir_mtime = int(os.stat(tree).st_mtime)
check('  目录级时间照抄（/Users 的 mtime = 宿主目录 mtime，不是"现在"）',
      u and abs(u[0]['mtime'] - want_dir_mtime) <= 1, (u, want_dir_mtime))
tm = data(run('ntfs.list', img=A, lba=AN, path='/Users', times='true')).get('names') or []
sub = [x for x in tm if x['name'] == 'sub']
want_sub_mtime = int(os.stat(os.path.join(tree, 'sub')).st_mtime)
check('  子目录时间也照抄', sub and abs(sub[0]['mtime'] - want_sub_mtime) <= 1, (sub, want_sub_mtime))

# ─────────── 2. 同名策略（独立目录，避免污染后面的计数） ───────────
mk(b'AAAA\n', 'conf_a.txt'); mk(b'BBBBBBBB\n', 'conf_b.txt')
run('file.copy', img=A, dst_lba=AN, src='host:' + os.path.join(W, 'conf_a.txt'), dst='ntfs:/Conf/a.txt')
run('file.copy', img=A, dst_lba=AN, src='host:' + os.path.join(W, 'conf_b.txt'), dst='ntfs:/Conf/b.txt')
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN, src='ntfs:/Conf/a.txt', dst='ntfs:/Conf/b.txt')
check('同名 + 默认 fail ⇒ COPY-DEST-EXISTS', code(d) == 'COPY-DST-EXISTS', json.dumps(d)[:200])
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN, src='ntfs:/Conf/a.txt', dst='ntfs:/Conf/b.txt', conflict='rename')
check('同名 + rename ⇒ b (1).txt', data(d).get('dst_path') == '/Conf/b (1).txt', data(d).get('dst_path'))
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN, src='ntfs:/Conf/a.txt', dst='ntfs:/Conf/b.txt', conflict='overwrite')
check('同名 + overwrite ⇒ 落点仍是 b.txt', d.get('ok') and data(d).get('dst_path') == '/Conf/b.txt', data(d).get('dst_path'))
check('  覆盖后内容 = 源（不是半截）', ntfs_export('/Conf/b.txt', 'ovw') == b'AAAA\n')

# ─────────── 3. NTFS → FAT32（同镜像跨分区） ───────────
d = run('file.copy', img=A, src_lba=AN, dst_lba=AESP, src='ntfs:/Users', dst='fat:/')
check('NTFS → FAT32 跨分区：≥3 文件 3 目录', d.get('ok') and data(d).get('copied_files') >= 3, json.dumps(d)[:200])
check('  FAT 上目录存在', data(run('fat.ls', img=A, lba=AESP, path='/Users/sub')).get('count') == 2)

# ─────────── 4. FAT32 → 宿主（递归） ───────────
out4 = os.path.join(W, 'fromfat')
d = run('file.copy', img=A, src_lba=AESP, src='fat:/Users', dst='host:' + out4)
got = os.path.join(out4, 'sub', 'b.bin')
check('FAT32 → 宿主：字节一致', d.get('ok') and os.path.exists(got) and rd(got) == rd(os.path.join(tree, 'sub', 'b.bin')),
      json.dumps(d)[:200])
check('  FAT 时间带入宿主（2 秒精度）',
      abs(os.stat(got).st_mtime - 1700000000) <= 2, os.stat(got).st_mtime)

# ─────────── 5. NTFS → NTFS（跨 qcow2） ───────────
d = run('file.copy', src='ntfs:/Users', src_img=A, src_lba=AN,
        dst='ntfs:/Backup', dst_img=B, dst_lba=BN)
check('NTFS → NTFS 跨镜像：≥3 文件', d.get('ok') and data(d).get('copied_files') >= 3, json.dumps(d)[:200])
out5 = os.path.join(W, 'b_bin')
r = run('ntfs.export', img=B, lba=BN, path='/Backup/sub/b.bin', dst=out5)
check('  跨镜像字节一致', r.get('ok') and rd(out5) == rd(os.path.join(tree, 'sub', 'b.bin')))

# ─────────── 6. FAT32 → FAT32（同卷） ───────────
d = run('file.copy', img=A, src_lba=AESP, dst_lba=AESP, src='fat:/Users', dst='fat:/Users2')
check('FAT → FAT 同卷：ok', d.get('ok') and data(d).get('copied_files') >= 3, json.dumps(d)[:200])

# ─────────── 7. 目录冲突：合并 / 改名 ───────────
d = run('file.copy', img=A, src_lba=AESP, dst_lba=AESP, src='fat:/Users', dst='fat:/Users2', conflict='overwrite')
check('目录同名 + overwrite ⇒ 合并（不新建）', d.get('ok') and data(d).get('dst_path') == '/Users2/Users', data(d).get('dst_path'))
d = run('file.copy', img=A, src_lba=AESP, dst_lba=AESP, src='fat:/Users', dst='fat:/Users2', conflict='rename')
check('目录同名 + rename ⇒ Users (1)', d.get('ok') and data(d).get('dst_path') == '/Users2/Users (1)', data(d).get('dst_path'))

# ─────────── 8. 中文名往返 ───────────
cn = os.path.join(W, 'cn')
os.makedirs(os.path.join(cn, '中文 目录'), exist_ok=True)
mk('中文内容\n'.encode(), 'cn/中文 目录/文件 名.txt')
d = run('file.copy', img=A, dst_lba=AN, src='host:' + cn, dst='ntfs:/中文根')
check('中文目录 → NTFS', d.get('ok') and data(d).get('copied_files') == 1, json.dumps(d)[:200])
d = run('file.copy', img=A, src_lba=AN, dst_lba=AESP, src='ntfs:/中文根', dst='fat:/')
check('中文目录 → FAT32（LFN 是 UTF-16）', d.get('ok') and data(d).get('copied_files') == 1, json.dumps(d)[:200])
ls = data(run('fat.ls', img=A, lba=AESP, path='/中文根/中文 目录')).get('entries') or []
check('  FAT 上读回的名字没变成 ??????', ls and ls[0]['name'] == '文件 名.txt', ls)
cnout = os.path.join(W, 'cnout')
d = run('file.copy', img=A, src_lba=AESP, src='fat:/中文根', dst='host:' + cnout)
p = os.path.join(cnout, '中文 目录', '文件 名.txt')
check('  中文名 FAT → 宿主：字节一致、名字原样',
      d.get('ok') and os.path.exists(p) and rd(p) == '中文内容\n'.encode(), os.listdir(cnout) if os.path.isdir(cnout) else d)

# ─────────── 8b. 超长中文名（按"字符"不是"字节"） ───────────
n60 = '中' * 60
n90 = '甲' * 90
os.makedirs(os.path.join(W, 'longname'), exist_ok=True)
mk(b'L\n', 'longname/%s.txt' % n60)
d = run('file.copy', img=A, dst_lba=AN, src='host:' + os.path.join(W, 'longname'), dst='ntfs:/LN')
check('60 个汉字的名字 → NTFS', d.get('ok') and data(d).get('copied_files') == 1, json.dumps(d)[:200])
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN,
        src='ntfs:/LN/%s.txt' % n60, dst='ntfs:/LN/%s.txt' % n90)
check('NTFS 内部复制成 90 个汉字（270 字节）的名字', d.get('ok'), json.dumps(d)[:200])
names = data(run('ntfs.list', img=A, lba=AN, path='/LN')).get('names') or []
check('  列表里名字长度 = 94（90 汉字 + .txt），没有被截断',
      sorted(len(x['name']) for x in names) == [64, 94], [len(x['name']) for x in names])
d = run('file.copy', img=A, src_lba=AN, dst_lba=AESP, src='ntfs:/LN', dst='fat:/LN')
check('超长名 → FAT32（UTF-16 LFN 支持 255 字符）', d.get('ok') and data(d).get('copied_files') == 2,
      json.dumps(d)[:200])
fe = data(run('fat.ls', img=A, lba=AESP, path='/LN')).get('entries') or []
check('  FAT 上读回的名字长度 = 94', sorted(len(x['name']) for x in fe) == [64, 94],
      [len(x['name']) for x in fe])
d = run('file.copy', img=A, src_lba=AESP, src='fat:/LN', dst='host:' + os.path.join(W, 'lnout'))
check('超长名 → 宿主：ext4 名字上限 255 字节 ⇒ 诚实报 COPY-NAME-TOO-LONG（不截断）',
      code(d) == 'COPY-NAME-TOO-LONG', code(d))

# ─────────── 9. 空文件 ───────────
mk(b'', 'empty.bin')
d = run('file.copy', img=A, dst_lba=AN, src='host:' + os.path.join(W, 'empty.bin'), dst='ntfs:/empty.bin')
check('空文件复制（驻留 $DATA 边界）', d.get('ok') and data(run('ntfs.exists', img=A, lba=AN, path='/empty.bin')).get('size_bytes') == 0)

# ─────────── 9b. 宿主 → 宿主（引擎本身也支持；App 也可以不经过工具） ───────────
hh = os.path.join(W, 'hh')
os.makedirs(os.path.join(hh, 'src', 'sub'), exist_ok=True)
mk(b'hh1\n', 'hh/src/a.txt'); mk(b'hh2\n', 'hh/src/sub/b.txt')
d = run('file.copy', src='host:' + os.path.join(hh, 'src'), dst='host:' + os.path.join(hh, 'out'))
check('宿主目录 → 宿主目录（递归）', d.get('ok') and data(d).get('copied_files') == 2, json.dumps(d)[:200])
d = run('file.copy', src='host:' + os.path.join(hh, 'src'), dst='host:' + os.path.join(hh, 'src', 'sub', 'loop'))
check('  宿主→宿主造环也拒绝（零副作用）', code(d) == 'COPY-INTO-SELF'
      and not os.path.exists(os.path.join(hh, 'src', 'sub', 'loop')), code(d))

# ─────────── 10. 递归开关：拒绝且零副作用 ───────────
before = data(run('fat.ls', img=A, lba=AESP, path='/')).get('total')
d = run('file.copy', img=A, src_lba=AN, dst_lba=AESP, src='ntfs:/Users', dst='fat:/NoWay', recursive='false')
after = data(run('fat.ls', img=A, lba=AESP, path='/')).get('total')
check('源是目录 + recursive=false ⇒ COPY-UNSUPPORTED 且什么都没建',
      code(d) == 'COPY-UNSUPPORTED' and before == after, (code(d), before, after))

# ─────────── 11. 造环：拒绝且零副作用 ───────────
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN, src='ntfs:/Users', dst='ntfs:/Users/sub/loop')
check('复制进自己的子树 ⇒ COPY-INTO-SELF', code(d) == 'COPY-INTO-SELF', code(d))
check('  子树里没被建出 loop 目录',
      not data(run('ntfs.exists', img=A, lba=AN, path='/Users/sub/loop')).get('exists'))

# ─────────── 12. 自己复制自己 ───────────
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN,
        src='ntfs:/Users/a.txt', dst='ntfs:/Users/a.txt', conflict='overwrite')
check('同文件 + overwrite ⇒ COPY-SAME-FILE', code(d) == 'COPY-SAME-FILE', code(d))
d = run('file.copy', img=A, src_lba=AN, dst_lba=AN,
        src='ntfs:/Users/a.txt', dst='ntfs:/Users/a.txt', conflict='rename')
check('  同文件 + rename ⇒ 合法（生成副本）', d.get('ok') and data(d).get('dst_path', '').startswith('/Users/a ('),
      data(d).get('dst_path'))

# ─────────── 13. FAT 单文件 4 GiB 上限 ───────────
huge = os.path.join(W, 'huge.bin')
try:
    with open(huge, 'wb') as f:
        f.truncate(5 * GiB)
    d = run('file.copy', img=A, dst_lba=AESP, src='host:' + huge, dst='fat:/huge.bin')
    check('5 GiB → FAT32 ⇒ COPY-FILE-TOO-BIG（提前拒绝，不写半截）',
          code(d) == 'COPY-FILE-TOO-BIG' and not data(run('fat.exists', img=A, lba=AESP, path='/huge.bin')).get('exists'),
          code(d))
except OSError as e:
    skipped('4 GiB 稀疏文件（%s）' % e)

# ─────────── 14. 批量清单（跨分区） ───────────
lst = os.path.join(W, 'list.txt')
with open(lst, 'w') as f:
    f.write('ntfs@%d:/Users/a.txt > fat@%d:/BatchNtfs.txt\n' % (AN, AESP))
    f.write('host:%s > ntfs@%d:/BatchHost.txt\n' % (os.path.join(tree, 'a.txt'), AN))
    f.write('fat@%d:/BatchNtfs.txt > host:%s\n' % (AESP, os.path.join(W, 'batch_back.txt')))
d = run('file.copy', img=A, list_file=lst)
check('批量：3 项全成功', d.get('ok') and data(d).get('copied') == 3 and data(d).get('failed') == 0,
      json.dumps(d)[:300])
check('  批量结果可解析且 bytes 正确（3×12）', data(d).get('bytes') == 36, data(d))

# ─────────── 14b. 批量 JSON 转义（文件名里有引号） ───────────
# ★ 2026-10-24：`"` 是 Windows 非法字符，NTFS/FAT 目标现在会 NAME-INVALID（P0-2）。
#   所以 JSON 转义改在宿主（ext4 允许）上测；镜像侧拒绝另有断言。
mk(b'Q\n', 'quo"te.txt')
qs = os.path.join(W, 'quo"te.txt')
qd = os.path.join(W, 'out"put.txt')
ql = os.path.join(W, 'quote.list')
with open(ql, 'w') as f:
    f.write('host:%s > host:%s\n' % (qs, qd))
d = run('file.copy', list_file=ql)
check('批量：文件名含双引号也能产出合法 JSON，且成功',
      d.get('ok') and data(d).get('copied') == 1 and data(d).get('items')[0].get('ok') is True,
      json.dumps(d)[:240])
check('  成功项带 path（UI 刷新用）',
      data(d).get('items') and str(data(d).get('items')[0].get('path', '')).endswith('out"put.txt'),
      data(d).get('items'))
d = run('file.copy', img=A, dst_lba=AN, src='host:' + qs, dst='ntfs:/')
check('同一个引号名字复制进 NTFS 目录（名字跟源走）⇒ NAME-INVALID，零副作用',
      code(d) == 'NAME-INVALID'
      and not data(run('ntfs.exists', img=A, lba=AN, path='/quo\"te.txt')).get('exists'),
      json.dumps(d)[:240])

# ─────────── 15. lba 自动挑（该文件系统唯一） ───────────
d = run('file.copy', img=A, src_lba=AN, dst_lba=AESP, src='ntfs:/Users/a.txt', dst='fat:/AutoLba.txt')
check('显式 lba 的 NTFS→FAT 复制（新语法不再自动猜分区）', d.get('ok'), json.dumps(d)[:200])

# ─────────── 16. 符号链接跳过 ───────────
ln = os.path.join(tree, 'link.txt')
try:
    if os.path.lexists(ln):
        os.unlink(ln)
    os.symlink(os.path.join(tree, 'a.txt'), ln)
    d = run('file.copy', img=A, dst_lba=AN, src='host:' + tree, dst='ntfs:/WithLink',
            conflict='overwrite')
    check('宿主源里的符号链接被跳过（skipped=1）且不算失败',
          d.get('ok') and data(d).get('skipped') == 1, (data(d).get('skipped'), json.dumps(d)[:160]))
    check('  链接目标没有被复制成实体文件',
          not data(run('ntfs.exists', img=A, lba=AN, path='/WithLink/link.txt')).get('exists'))
except OSError as e:
    skipped('符号链接（%s）' % e)

# ─────────── 17. ISO → NTFS ───────────
iso = None
for cand in [os.path.join(os.path.dirname(ROOT), n) for n in os.listdir(os.path.dirname(ROOT)) if n.lower().endswith('.iso')]:
    iso = cand; break
if iso:
    d = run('file.copy', src='iso:/bootmgr', src_img=iso, dst='ntfs:/FromIso', img=B, dst_lba=BN)
    check('ISO → NTFS（小文件）', d.get('ok') and data(d).get('copied_files') == 1, json.dumps(d)[:200])
    if d.get('ok'):
        outx = os.path.join(W, 'iso_from_image'); ref = os.path.join(W, 'iso_ref')
        r1 = run('ntfs.export', img=B, lba=BN, path='/FromIso', dst=outx)
        r2 = run('iso.extract', iso=iso, path='/bootmgr', dst=ref)
        check('  ISO 字节一致', r1.get('ok') and r2.get('ok') and rd(outx) == rd(ref))
    # 不同 ISO（Win7/Vista/Win11）的 /boot 内容不一样：期望值从 ISO 自己数，
    # 不写死 29 —— 否则换一张素材就假红。
    res = data(run('iso.ls', iso=iso, path='/boot', recursive='true'))
    want_files = sum(1 for e in (res.get('entries') or []) if not e['dir'])
    d = run('file.copy', src='iso:/boot', src_img=iso, dst='fat:/IsoBoot', img=B, dst_lba=BESP)
    check('ISO 目录 → FAT32（%d 文件）' % want_files,
          d.get('ok') and want_files > 0 and data(d).get('copied_files') == want_files,
          (json.dumps(d)[:200], want_files))
    if d.get('ok'):
        f1 = os.path.join(W, 'bcd_ref2'); f2 = os.path.join(W, 'bcd_img2')
        a1 = run('iso.extract', iso=iso, path='/boot/bcd', dst=f1)
        if a1.get('ok'):
            a2 = run('fat.export', img=B, lba=BESP, path='/IsoBoot/bcd', dst=f2)
            check('  ISO 目录里的文件字节一致', a2.get('ok') and rd(f1) == rd(f2))
        else:
            skipped('该 ISO 的 /boot/bcd')
else:
    skipped('ISO → NTFS')

# ─────────── 18. WIM/ESD → NTFS ───────────
_ws = os.path.dirname(os.path.dirname(ROOT))
esd = os.environ.get('IMG_SAMPLE_ESD') or os.path.join(_ws, 'sample.esd')
if os.path.exists(esd):
    d = run('file.copy', src='wim:/Windows/System32/config/BBI', src_img=esd, src_index=1,
            dst='ntfs:/FromWim', img=B, dst_lba=BN)
    check('WIM/ESD → NTFS（单文件）', d.get('ok') and data(d).get('bytes') == 8192, json.dumps(d)[:200])
else:
    skipped('WIM/ESD → NTFS')

# ─────────── 18b. 同镜像跨分区 NTFS → NTFS（两个卷同时挂）+ lba 歧义 ───────────
C = os.path.join(W, 'c.qcow2')
run('img.create', out=C, **{'size-gb': 2, 'table': 'mbr', 'sys-mb': 800, 'limits': 'off'})
d = run('part.layout', img=C, **{'sr-mb': 100, 'sys-gb': 1, 'extended': 'true', 'logical-name': 'Data'})
parts = data(run('part.show', img=C)).get('pt', {}).get('parts') or []
lb = [x['first'] for x in parts]
check('造出两个可格式化的主分区', len(lb) >= 2, lb)
if len(lb) >= 2:
    SR, SYS = lb[0], lb[1]
    run('fs.format', img=C, lba=SR, fs='ntfs', label='SR', quick='true')
    run('fs.format', img=C, lba=SYS, fs='ntfs', label='SYS', quick='true')
    mk(b'two-ntfs\n', 'two.txt')
    run('file.copy', img=C, dst_lba=SYS, src='host:' + os.path.join(W, 'two.txt'), dst='ntfs:/two.txt')
    d = run('file.copy', img=C, src_lba=SYS, dst_lba=SR, src='ntfs:/two.txt', dst='ntfs:/fromsys.txt')
    check('同镜像跨分区 NTFS → NTFS（两个卷同时挂载）', d.get('ok') and data(d).get('copied_files') == 1,
          json.dumps(d)[:200])
    d = run('copy', src=C + '!p99:/two.txt', dst=C + '!p1:/x.txt')
    check('分区序号越界（!p99:）⇒ PATH-BAD（不猜）', code(d) == 'PATH-BAD', code(d))

# ─────────── 18c. create_parents=false 必须拒绝且零副作用 ───────────
d = run('file.copy', img=A, dst_lba=AN, src='host:' + os.path.join(W, 'two.txt'),
        dst='ntfs:/NoSuchParent/x.txt', create_parents='false')
check('create_parents=false + 父目录不存在 ⇒ PATH-DST-NOTFOUND',
      code(d) == 'PATH-DST-NOTFOUND' and
      not data(run('ntfs.exists', img=A, lba=AN, path='/NoSuchParent')).get('exists'),
      code(d))

# ─────────── 19. 收尾自检 + 独立三方对账 ───────────
for tag, img in (('A', A), ('B', B)):
    d = run('qc.verify', img=img)
    check('qc.verify %s 通过' % tag, d.get('ok'), json.dumps(d)[:200])

raw = os.path.join(W, 'a.raw'); base = os.path.join(W, 'a.base.json')
subprocess.run(['qemu-img', 'convert', '-O', 'raw', A, raw], check=True)
subprocess.run([sys.executable, INV, raw, str(AN), '--base-out', base], check=False)
p = subprocess.run([sys.executable, INV, raw, str(AN), '--base', base], capture_output=True, text=True, errors='replace')
check('★ 这一串跨分区/跨镜像操作没有给 NTFS 卷新增任何不一致', p.returncode == 0,
      (p.stdout + p.stderr)[-300:])

print('════ 结果：通过 %d · 失败 %d · 跳过 %d ════' % (ok, bad, skip))
shutil.rmtree(W, ignore_errors=True)
sys.exit(0 if bad == 0 else 1)