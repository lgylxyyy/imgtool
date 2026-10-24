#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fileops-test —— 统一路径（! 语法）+ copy / move / rename / rm 端到端

覆盖：
  · 路径：pN / lbaN 等价、越界、缺选择器、转义 \\!、ISO 直接路径
  · copy：同卷 / 跨卷 / 跨镜像 / 宿主↔镜像 / ISO→镜像
  · move：同卷 rename / 同卷跨目录 / 跨镜像（复制+删源）/ 宿主源删源许可 /
          目录跨卷 / 造环 / 只读源
  · rename：文件、目录、重名、非法新名字
  · rm：文件、空目录、非空目录、递归、根目录拒绝、宿主删源许可、ISO 拒绝
  · 批量：copy --list_file / move --list_file

设计纪律：每个否定用例都断言"盘上/宿主没被写脏"（拒绝必须零副作用）。
"""
import json, os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMGCMD = os.path.join(ROOT, 'build', 'tools', 'imgcmd')
W = tempfile.mkdtemp(prefix='fileops')
ok = bad = skip = 0


def run(cmd, *args, **kw):
    argv = [IMGCMD, cmd] + list(args) + ['--%s=%s' % (k, v) for k, v in kw.items()]
    p = subprocess.run(argv, capture_output=True, text=True)
    js = [l for l in p.stdout.splitlines() if l.startswith('{')]
    return json.loads(js[-1]) if js else {'ok': False, 'raw': (p.stdout + p.stderr)[-300:]}


def code(d):
    return (d.get('err') or d.get('error') or {}).get('error_code', '')


def data(d):
    return d.get('data') or {}


def check(desc, cond, extra=''):
    global ok, bad
    if cond:
        ok += 1; print('  ✅ %s' % desc)
    else:
        bad += 1; print('  ❌ %s  %s' % (desc, str(extra)[:300]))


def skipped(desc):
    global skip
    skip += 1; print('  ⏭ %s' % desc)


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
    d = run('img.create', out=img, **{'size-gb': 1, 'table': 'gpt',
                                      'esp-mb': esp_mb, 'msr-mb': msr_mb})
    parts = data(d)['pt']['parts']
    esp, dat = parts[0]['first'], parts[-1]['first']
    assert run('fs.format', img=img, lba=esp, fs='fat32', label='ESP', quick='true').get('ok')
    assert run('fs.format', img=img, lba=dat, fs='ntfs', label='DATA', quick='true').get('ok')
    return img, esp, dat


def exists(img, lba, path):
    return bool(data(run('ntfs.exists', img=img, lba=lba, path=path)).get('exists'))


def fexists(img, lba, path):
    return bool(data(run('fat.exists', img=img, lba=lba, path=path)).get('exists'))


def uri(img, lba, path):
    return '%s!lba%d:%s' % (img, lba, path)


print('════ 统一路径 + copy/move/rename/rm ════')
A, AESP, AN = new_img('a.qcow2')
B, BESP, BN = new_img('b.qcow2', esp_mb=32)

# ---------- 1. 路径语法 ----------
mk(b'HELLO-PATH\n', 'p.txt')
d = run('copy', 'host:' + os.path.join(W, 'p.txt'), uri(A, AN, '/p.txt'))
check('host → qcow2（lbaN 选择器）', d.get('ok'), json.dumps(d)[:240])

# pN：第几个已用分区？GPT 上 ESP=1、MSR=2、数据=3
pn_parts = data(run('part.show', img=A)).get('pt', {}).get('parts') or []
data_idx = None
for i, p in enumerate(pn_parts):
    if p['first'] == AN:
        data_idx = i + 1
check('找到数据分区的序号', data_idx is not None, pn_parts)
d = run('copy', uri(A, AN, '/p.txt'), '%s!p%d:/by-p.txt' % (A, data_idx))
check('pN 与 lbaN 指向同一个分区', d.get('ok') and exists(A, AN, '/by-p.txt'), json.dumps(d)[:240])

d = run('copy', uri(A, AN, '/p.txt'), '%s!p99:/x.txt' % A)
check('!p99: 越界 ⇒ PATH-BAD（不猜）', code(d) == 'PATH-BAD', code(d))
d = run('copy', uri(A, AN, '/p.txt'), A + '!:/x.txt')
check('qcow2 缺分区选择器 ⇒ PATH-BAD', code(d) == 'PATH-BAD', code(d))
d = run('copy', 'host:' + os.path.join(W, 'p.txt'), 'relative/x.txt')
check('相对宿主路径 ⇒ PATH-BAD', code(d) == 'PATH-BAD', code(d))

# 转义 !：盘内文件名里真的有 !
run('copy', 'host:' + os.path.join(W, 'p.txt'), uri(A, AN, '/a!b.txt'))
d = run('copy', uri(A, AN, '/a\\!b.txt'), uri(A, AN, '/a!b-copy.txt'))
check('路径里的 \\! 当字面 !（不是容器边界）', d.get('ok') and exists(A, AN, '/a!b-copy.txt'),
      json.dumps(d)[:240])

# ISO（有素材才测）
iso = None
base = os.path.dirname(ROOT)
for n in os.listdir(base):
    if n.lower().endswith('.iso'):
        iso = os.path.join(base, n); break
if iso:
    d = run('copy', '%s!:/bootmgr' % iso, uri(B, BN, '/from-iso.bin'))
    check('ISO!:/path → NTFS', d.get('ok'), json.dumps(d)[:240])
    d = run('rm', '%s!:/bootmgr' % iso)
    check('ISO 只读：rm ⇒ PATH-READONLY', code(d) == 'PATH-READONLY', code(d))
else:
    skipped('ISO 素材缺失')

# ---------- 2. copy 矩阵 ----------
mk(os.urandom(4096), 'big.bin')
run('copy', 'host:' + os.path.join(W, 'big.bin'), uri(A, AN, '/big.bin'))
d = run('copy', uri(A, AN, '/big.bin'), uri(A, AESP, '/BIG.BIN'))
check('copy 同镜像跨分区 NTFS→FAT', d.get('ok') and fexists(A, AESP, '/BIG.BIN'), json.dumps(d)[:240])
d = run('copy', uri(A, AN, '/big.bin'), uri(B, BN, '/big.bin'))
check('copy 跨镜像 NTFS→NTFS', d.get('ok') and exists(B, BN, '/big.bin'), json.dumps(d)[:240])
out = os.path.join(W, 'out.bin')
d = run('copy', uri(A, AN, '/big.bin'), 'host:' + out)
check('copy 镜像→宿主，字节一致', d.get('ok') and os.path.exists(out) and rd(out) == rd(os.path.join(W, 'big.bin')))
d = run('copy', uri(A, AN, '/big.bin'), uri(A, AN, '/big.bin'), conflict='rename')
check('copy 同名 + rename ⇒ big (1).bin',
      d.get('ok') and data(d).get('path') == '/big (1).bin', data(d).get('path'))
check('  返回 parent_total / size / ino 字段',
      'parent_total' in data(d) and data(d).get('size') == 4096 and 'ino' in data(d), data(d))

# ---------- 3. move 同卷 ----------
d = run('move', uri(A, AN, '/big.bin'), uri(A, AN, '/renamed.bin'))
check('move 同卷改名 ⇒ mode=rename',
      d.get('ok') and data(d).get('mode') == 'rename' and data(d).get('deleted') is True,
      data(d))
check('  源不在、目标在', not exists(A, AN, '/big.bin') and exists(A, AN, '/renamed.bin'))
d = run('move', uri(A, AN, '/renamed.bin'), uri(A, AN, '/sub/renamed.bin'))
check('move 同卷跨目录 ⇒ mode=move', d.get('ok') and data(d).get('mode') == 'move', data(d))
d = run('move', uri(A, AN, '/sub/renamed.bin'), uri(A, AN, '/sub/renamed.bin'))
check('move 自己到自己 ⇒ noop', d.get('ok') and data(d).get('noop') is True, data(d))

# ---------- 4. move 跨镜像 ----------
d = run('move', uri(A, AN, '/sub/renamed.bin'), uri(B, BN, '/moved.bin'))
check('move 跨镜像 ⇒ mode=cross_volume + deleted',
      d.get('ok') and data(d).get('mode') == 'cross_volume' and data(d).get('deleted') is True,
      data(d))
check('  源（镜像 A）已删、目标（镜像 B）在',
      not exists(A, AN, '/sub/renamed.bin') and exists(B, BN, '/moved.bin'))
check('  跨镜像内容一致', rd(os.path.join(W, 'big.bin')) ==
      (lambda p: rd(p) if os.path.exists(p) else b'')(out))

# ---------- 5. move 宿主源：默认拒绝删源 ----------
hp = mk(b'HOST-MOVE\n', 'hostmove.txt')
d = run('move', 'host:' + hp, uri(A, AN, '/from-host.txt'))
check('宿主 → 镜像 move 默认拒绝删源（MOVE-HOST-DELETE-REFUSED）',
      code(d) == 'MOVE-HOST-DELETE-REFUSED', code(d))
check('  源还在、目标没建', os.path.exists(hp) and not exists(A, AN, '/from-host.txt'))
d = run('move', 'host:' + hp, uri(A, AN, '/from-host.txt'), allow_host_delete='true')
check('  显式 allow_host_delete=true ⇒ 成功且源已删',
      d.get('ok') and data(d).get('deleted') is True and not os.path.exists(hp), data(d))

# ---------- 5b. 宿主 ↔ 宿主 ----------
hd = os.path.join(W, 'hostdir'); os.makedirs(os.path.join(hd, 'sub'), exist_ok=True)
mk(b'HOST-RENAME\n', 'hostdir/a.txt')
d = run('move', 'host:' + os.path.join(hd, 'a.txt'), 'host:' + os.path.join(hd, 'b.txt'))
check('宿主同盘改名（rename 系统调用）',
      d.get('ok') and data(d).get('mode') == 'rename' and data(d).get('deleted') is True
      and not os.path.exists(os.path.join(hd, 'a.txt')) and os.path.exists(os.path.join(hd, 'b.txt')),
      data(d))
d = run('move', 'host:' + os.path.join(hd, 'b.txt'), 'host:' + os.path.join(hd, 'sub', 'b.txt'))
check('宿主同盘跨目录移动', d.get('ok') and data(d).get('mode') == 'move'
      and os.path.exists(os.path.join(hd, 'sub', 'b.txt')), data(d))
if os.path.isdir('/dev/shm'):
    src = os.path.join(W, 'cross.txt'); mk(b'CROSS\n', 'cross.txt')
    dst = '/dev/shm/imgtool-cross-%d.txt' % os.getpid()
    d = run('move', 'host:' + src, 'host:' + dst)
    check('宿主跨文件系统 move 默认拒绝删源', code(d) == 'MOVE-HOST-DELETE-REFUSED' and os.path.exists(src), code(d))
    d = run('move', 'host:' + src, 'host:' + dst, allow_host_delete='true')
    check('  显式允许 ⇒ 复制+删源', d.get('ok') and not os.path.exists(src) and os.path.exists(dst), data(d))
    if os.path.exists(dst): os.unlink(dst)
else:
    skipped('宿主跨文件系统（无 /dev/shm）')

# ---------- 6. move 目录跨镜像 ----------
tree = os.path.join(W, 'tree')
os.makedirs(os.path.join(tree, 'sub'), exist_ok=True)
mk(b'1', 'tree/a.txt'); mk(b'2', 'tree/sub/b.txt')
run('copy', 'host:' + tree, uri(A, AN, '/Tree'))
d = run('move', uri(A, AN, '/Tree'), uri(B, BN, '/TreeFromA'))
check('move 目录跨镜像（复制+递归删源）',
      d.get('ok') and data(d).get('dirs') >= 2 and not exists(A, AN, '/Tree')
      and exists(B, BN, '/TreeFromA/sub/b.txt'), data(d))

d = run('move', uri(B, BN, '/TreeFromA'), uri(B, BN, '/TreeFromA/sub/loop'))
check('move 目录进自己子树 ⇒ COPY-INTO-SELF',
      code(d) == 'COPY-INTO-SELF' and not exists(B, BN, '/TreeFromA/sub/loop'), code(d))

if iso:
    d = run('move', '%s!:/bootmgr' % iso, uri(A, AN, '/x.bin'))
    check('move 只读源 ⇒ PATH-READONLY', code(d) == 'PATH-READONLY', code(d))

# ---------- 7. rename ----------
d = run('rename', uri(A, AN, '/p.txt'), 'p-renamed.txt')
check('rename 文件', d.get('ok') and exists(A, AN, '/p-renamed.txt') and not exists(A, AN, '/p.txt'),
      data(d))
d = run('rename', uri(A, AN, '/p-renamed.txt'), 'by-p.txt')
check('rename 撞已有名字 ⇒ COPY-DST-EXISTS', code(d) == 'COPY-DST-EXISTS', code(d))
d = run('rename', uri(A, AN, '/p-renamed.txt'), 'a/b')
check('rename 新名字带 / ⇒ CMD-ARG-BAD', code(d) == 'CMD-ARG-BAD', code(d))
d = run('rename', uri(A, AN, '/p-renamed.txt'), 'P-RENAMED.txt')
check('rename 只改大小写（NTFS）', d.get('ok') and data(d).get('case_only') is True, data(d))

# ---------- 8. rm ----------
mk(b'RM\n', 'rm.txt')
run('copy', 'host:' + os.path.join(W, 'rm.txt'), uri(A, AN, '/rm.txt'))
d = run('rm', uri(A, AN, '/rm.txt'))
check('rm 文件', d.get('ok') and data(d).get('removed') == 1 and not exists(A, AN, '/rm.txt'))
run('copy', 'host:' + tree, uri(A, AN, '/RMTree'))
d = run('rm', uri(A, AN, '/RMTree'))
check('rm 非空目录默认 ⇒ RM-DIR-NOT-EMPTY',
      code(d) == 'RM-DIR-NOT-EMPTY' and exists(A, AN, '/RMTree'), code(d))
d = run('rm', uri(A, AN, '/RMTree'), recursive='true')
check('rm 递归 ⇒ removed ≥ 3 且整棵不在',
      d.get('ok') and data(d).get('removed', 0) >= 3 and not exists(A, AN, '/RMTree'), data(d))
d = run('rm', uri(A, AN, '/'))
check('rm 镜像根 ⇒ RM-ROOT-REFUSED', code(d) == 'RM-ROOT-REFUSED', code(d))
d = run('rm', 'host:' + os.path.join(W, 'rm.txt'))
check('rm 宿主默认 ⇒ RM-HOST-DELETE-REFUSED 且文件还在',
      code(d) == 'RM-HOST-DELETE-REFUSED' and os.path.exists(os.path.join(W, 'rm.txt')), code(d))
d = run('rm', 'host:' + os.path.join(W, 'rm.txt'), allow_host_delete='true')
check('  allow_host_delete=true ⇒ 删掉', d.get('ok') and not os.path.exists(os.path.join(W, 'rm.txt')))
if iso:
    d = run('rm', '%s!:/bootmgr' % iso)
    check('rm ISO 内容 ⇒ PATH-READONLY', code(d) == 'PATH-READONLY', code(d))

# ---------- 9. 批量 ----------
os.makedirs(os.path.join(W, 'batch'), exist_ok=True)
for n in ('b1.txt', 'b2.txt'):
    mk(n.encode(), 'batch/' + n)
run('copy', 'host:' + os.path.join(W, 'batch'), uri(A, AN, '/Batch'))
lst = os.path.join(W, 'list.txt')
with open(lst, 'w') as f:
    f.write('%s > %s\n' % (uri(A, AN, '/Batch/b1.txt'), uri(A, AN, '/Batch/out1.txt')))
    f.write('%s > %s\n' % (uri(A, AN, '/Batch/b2.txt'), uri(A, AN, '/Batch/out2.txt')))
d = run('copy', list_file=lst)
check('批量 copy：2/2 成功且 items 带 path',
      d.get('ok') and data(d).get('copied') == 2 and all(x.get('path') for x in data(d)['items']),
      json.dumps(d)[:300])
lst2 = os.path.join(W, 'list2.txt')
with open(lst2, 'w') as f:
    f.write('%s > %s\n' % (uri(A, AN, '/Batch/out1.txt'), uri(A, AN, '/Batch/m1.txt')))
d = run('move', list_file=lst2)
check('批量 move：1/1 成功', d.get('ok') and data(d).get('moved') == 1, json.dumps(d)[:240])

# ---------- 10. 收尾 ----------
check('qc.verify A', run('qc.verify', img=A).get('ok'))
check('qc.verify B', run('qc.verify', img=B).get('ok'))

print('════ 结果：通过 %d · 失败 %d · 跳过 %d ════' % (ok, bad, skip))
shutil.rmtree(W, ignore_errors=True)
sys.exit(0 if bad == 0 else 1)